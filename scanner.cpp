// scanner.cpp - interactive memory value scanner (Cheat Engine style, read-only)
// build : clang++ -std=c++17 -O2 scanner.cpp -o scanner
// run   : sudo ./scanner [process_name]
//
// Commands:
//   scan <number>  - initial scan, searches for the value across all RW memory
//   next <number>  - filters existing candidates by the new value
//   list [n]       - displays the first n candidates (default 20)
//   types          - counts candidates grouped by encoding type
//   reset          - clears candidates, start over
//   quit
//
// DO NOT name the binary with the same substring as the target process.

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_error.h>
#include <sys/sysctl.h>
#include <libproc.h>

enum VType : uint8_t { T_I32 = 0, T_I64, T_F32, T_F64, T_I32K, T_I64K, T_COUNT };
static const char* TNAME[T_COUNT] = { "i32", "i64", "f32", "f64", "i32/1000", "i64/1000" };

struct Cand {
    uint64_t addr;
    uint8_t  type;
    bool operator<(const Cand& o) const {
        return addr != o.addr ? addr < o.addr : type < o.type;
    }
    bool operator==(const Cand& o) const { return addr == o.addr && type == o.type; }
};

// ---------------------------------------------------------------- process

static pid_t find_pid(const char* needle, std::string& path_out) {
    int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
    size_t size = 0;
    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) < 0) return 0;
    std::vector<char> buf(size);
    if (sysctl(mib, 3, buf.data(), &size, nullptr, 0) < 0) return 0;

    auto* proc = reinterpret_cast<struct kinfo_proc*>(buf.data());
    int n = static_cast<int>(size / sizeof(struct kinfo_proc));
    pid_t self = getpid(), found = 0;
    int cands = 0;

    for (int i = 0; i < n; ++i) {
        if (proc[i].kp_proc.p_pid == self) continue;
        std::string comm = proc[i].kp_proc.p_comm;
        if (comm.find(needle) == std::string::npos) continue;

        pid_t pid = proc[i].kp_proc.p_pid;
        char p[PROC_PIDPATHINFO_MAXSIZE] = {0};
        std::string full = (proc_pidpath(pid, p, sizeof(p)) > 0) ? p : "(inaccessible)";
        printf("[i] candidate #%d: pid=%d comm=\"%s\"\n    %s\n", ++cands, pid, comm.c_str(), full.c_str());
        if (found == 0) { found = pid; path_out = full; }
    }
    if (cands > 1) printf("[!] %d candidates found - using the first one.\n", cands);
    return found;
}

// ---------------------------------------------------------------- regions

struct Region { mach_vm_address_t base; mach_vm_size_t size; };

static std::vector<Region> rw_regions(mach_port_t task) {
    std::vector<Region> out;
    mach_vm_address_t addr = 0;
    mach_vm_size_t    sz   = 0;
    natural_t         depth = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt;

    while (true) {
        cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(task, &addr, &sz, &depth,
                reinterpret_cast<vm_region_recurse_info_t>(&info), &cnt) != KERN_SUCCESS)
            break;
        if (info.is_submap) { depth++; continue; }
        if ((info.protection & VM_PROT_READ) && (info.protection & VM_PROT_WRITE))
            out.push_back({ addr, sz });
        addr += sz;
    }
    return out;
}

// ---------------------------------------------------------------- matching

// In-game displayed value is truncated to integer, so match within [target, target + 1).
static inline bool match_at(const char* p, uint8_t type, long long target) {
    switch (type) {
        case T_I32: { int32_t v; memcpy(&v, p, 4); return v == (int32_t)target; }
        case T_I64: { int64_t v; memcpy(&v, p, 8); return v == (int64_t)target; }
        case T_F32: { float   v; memcpy(&v, p, 4);
                      if (!std::isfinite(v)) return false;
                      return v >= (float)target && v < (float)(target + 1); }
        case T_F64: { double  v; memcpy(&v, p, 8);
                      if (!std::isfinite(v)) return false;
                      return v >= (double)target && v < (double)(target + 1); }
        case T_I32K:{ int32_t v; memcpy(&v, p, 4);
                      if (v < 0) return false;
                      return (long long)v / 1000 == target; }
        case T_I64K:{ int64_t v; memcpy(&v, p, 8);
                      if (v < 0) return false;
                      return v / 1000 == target; }
    }
    return false;
}

static inline size_t tsize(uint8_t t) {
    return (t == T_I32 || t == T_F32 || t == T_I32K) ? 4 : 8;
}

// Print raw value for context
static void print_val(const char* p, uint8_t type) {
    switch (type) {
        case T_I32:  { int32_t v; memcpy(&v, p, 4); printf("%d", v); break; }
        case T_I64:  { int64_t v; memcpy(&v, p, 8); printf("%lld", (long long)v); break; }
        case T_F32:  { float   v; memcpy(&v, p, 4); printf("%.4f", v); break; }
        case T_F64:  { double  v; memcpy(&v, p, 8); printf("%.4f", v); break; }
        case T_I32K: { int32_t v; memcpy(&v, p, 4); printf("%d (=%d)", v, v / 1000); break; }
        case T_I64K: { int64_t v; memcpy(&v, p, 8);
                       printf("%lld (=%lld)", (long long)v, (long long)(v / 1000)); break; }
    }
}

// ---------------------------------------------------------------- scans

static const size_t MAX_CANDS = 4000000;

static std::vector<Cand> first_scan(mach_port_t task, long long target) {
    std::vector<Cand> out;
    auto regs = rw_regions(task);
    const size_t CHUNK = 1 << 20, OVER = 8;
    std::vector<char> buf(CHUNK + OVER);
    unsigned long long scanned = 0;
    bool capped = false;

    for (auto& r : regs) {
        for (mach_vm_size_t off = 0; off < r.size; off += CHUNK) {
            mach_vm_size_t want = std::min<mach_vm_size_t>(CHUNK + OVER, r.size - off);
            mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(task, r.base + off, want,
                    reinterpret_cast<mach_vm_address_t>(buf.data()), &got) != KERN_SUCCESS)
                continue;
            scanned += got;
            if (got < 8) continue;

            for (size_t i = 0; i + 8 <= got; i += 4) {
                const char* p = buf.data() + i;
                uint64_t a = r.base + off + i;
                if (match_at(p, T_I32,  target)) out.push_back({ a, T_I32  });
                if (match_at(p, T_F32,  target)) out.push_back({ a, T_F32  });
                if (match_at(p, T_I32K, target)) out.push_back({ a, T_I32K });
                if ((a & 7) == 0) {
                    if (match_at(p, T_I64,  target)) out.push_back({ a, T_I64  });
                    if (match_at(p, T_F64,  target)) out.push_back({ a, T_F64  });
                    if (match_at(p, T_I64K, target)) out.push_back({ a, T_I64K });
                }
                if (out.size() > MAX_CANDS) { capped = true; break; }
            }
            if (capped) break;
        }
        if (capped) break;
    }

    printf("[i] scanned %.1f MB across %zu regions\n", scanned / 1048576.0, regs.size());
    if (capped) printf("[!] candidate limit of %zu reached - choose a less common value.\n", MAX_CANDS);

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Re-reading grouped by pages to avoid one syscall per candidate.
static std::vector<Cand> next_scan(mach_port_t task, const std::vector<Cand>& in, long long target) {
    std::vector<Cand> out;
    const uint64_t PAGE = 4096;
    uint64_t cur_page = ~0ULL;
    std::vector<char> page(PAGE + 8);
    bool page_ok = false;

    for (auto& c : in) {
        uint64_t pb = c.addr & ~(PAGE - 1);
        if (pb != cur_page) {
            mach_vm_size_t got = 0;
            page_ok = (mach_vm_read_overwrite(task, pb, PAGE + 8,
                          reinterpret_cast<mach_vm_address_t>(page.data()), &got) == KERN_SUCCESS
                       && got >= PAGE);
            cur_page = pb;
        }
        if (page_ok) {
            if (match_at(page.data() + (c.addr - pb), c.type, target)) out.push_back(c);
        } else {
            char tmp[8];
            mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(task, c.addr, 8,
                    reinterpret_cast<mach_vm_address_t>(tmp), &got) == KERN_SUCCESS && got == 8)
                if (match_at(tmp, c.type, target)) out.push_back(c);
        }
    }
    return out;
}

static void list_cands(mach_port_t task, const std::vector<Cand>& v, size_t n) {
    size_t k = std::min(n, v.size());
    for (size_t i = 0; i < k; ++i) {
        char tmp[8] = {0};
        mach_vm_size_t got = 0;
        printf("  0x%012llx  %-9s  ", (unsigned long long)v[i].addr, TNAME[v[i].type]);
        if (mach_vm_read_overwrite(task, v[i].addr, 8,
                reinterpret_cast<mach_vm_address_t>(tmp), &got) == KERN_SUCCESS && got == 8)
            print_val(tmp, v[i].type);
        else
            printf("(unreadable)");
        printf("\n");
    }
    if (v.size() > k) printf("  ... and %zu more\n", v.size() - k);
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
    std::string procname = (argc > 1) ? argv[1] : "hoi4";

    char selfpath[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (proc_pidpath(getpid(), selfpath, sizeof(selfpath)) > 0
        && std::string(selfpath).find(procname) != std::string::npos)
        printf("[!] WARNING: your binary name contains \"%s\". Rename it.\n", procname.c_str());

    std::string path;
    pid_t pid = find_pid(procname.c_str(), path);
    if (!pid) { fprintf(stderr, "[-] Process not found: \"%s\"\n", procname.c_str()); return 1; }

    mach_port_t task = MACH_PORT_NULL;
    kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[-] task_for_pid kr=%d (%s)\n", kr, mach_error_string(kr));
        return 1;
    }
    printf("[+] Attached to pid %d\n", pid);
    printf("[+] Commands: scan <n> | next <n> | list [n] | types | reset | quit\n\n");

    std::vector<Cand> cands;
    std::string line;

    while (true) {
        printf("> ");
        fflush(stdout);
        if (!std::getline(std::cin, line)) break;

        std::istringstream is(line);
        std::string cmd;
        if (!(is >> cmd)) continue;

        if (cmd == "quit" || cmd == "q") break;

        if (cmd == "reset") { cands.clear(); printf("[i] candidates cleared\n"); continue; }

        if (cmd == "scan") {
            long long t;
            if (!(is >> t)) { printf("[-] usage: scan <number>\n"); continue; }
            cands = first_scan(task, t);
            printf("[+] %zu candidates\n", cands.size());
            continue;
        }

        if (cmd == "next") {
            long long t;
            if (!(is >> t)) { printf("[-] usage: next <number>\n"); continue; }
            if (cands.empty()) { printf("[-] run an initial scan first\n"); continue; }
            size_t before = cands.size();
            cands = next_scan(task, cands, t);
            printf("[+] %zu -> %zu candidates\n", before, cands.size());
            if (cands.size() <= 20) list_cands(task, cands, 20);
            continue;
        }

        if (cmd == "list") {
            size_t n = 20;
            is >> n;
            if (cands.empty()) printf("[-] no candidates\n");
            else list_cands(task, cands, n);
            continue;
        }

        if (cmd == "types") {
            size_t c[T_COUNT] = {0};
            for (auto& x : cands) c[x.type]++;
            for (int t = 0; t < T_COUNT; ++t)
                if (c[t]) printf("  %-9s : %zu\n", TNAME[t], c[t]);
            continue;
        }

        printf("[-] unknown command\n");
    }

    return 0;
}
