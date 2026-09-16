// memory.hpp - generic macOS external process memory access
//
// Game-agnostic layer: process attach, image base resolution, typed read/write,
// pointer chain walking. No HOI4-specific knowledge lives here.
//
// Requires root (task_for_pid). If the target uses hardened runtime without
// get-task-allow, SIP must be disabled.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <optional>
#include <initializer_list>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_error.h>
#include <sys/sysctl.h>
#include <libproc.h>
#include <mach-o/loader.h>

namespace mem {

struct Region {
    uint64_t  base;
    uint64_t  size;
    vm_prot_t protection;
    bool readable()  const { return protection & VM_PROT_READ; }
    bool writable()  const { return protection & VM_PROT_WRITE; }
};

class Process {
public:
    Process() = default;
    ~Process() {
        if (task_ != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), task_);
    }

    Process(const Process&)            = delete;
    Process& operator=(const Process&) = delete;

    // Attach by substring match on the process name.
    // Never matches our own process - naming your binary after the target is a
    // classic self-scan trap.
    bool attach(const std::string& processName, std::string& error) {
        pid_ = findPid(processName);
        if (!pid_) {
            error = "process not found: " + processName;
            return false;
        }

        kern_return_t kr = task_for_pid(mach_task_self(), pid_, &task_);
        if (kr != KERN_SUCCESS) {
            error = std::string("task_for_pid failed: ") + mach_error_string(kr)
                  + " (run as root; check SIP / hardened runtime)";
            return false;
        }

        if (!resolveImageBase()) {
            error = "could not locate main executable Mach-O header";
            return false;
        }
        return true;
    }

    bool attached() const { return task_ != MACH_PORT_NULL; }
    pid_t pid()     const { return pid_; }

    // Runtime load address of the main image.
    uint64_t imageBase() const { return imageBase_; }
    // ASLR slide: runtime address minus the address the image was linked at.
    uint64_t slide()     const { return slide_; }

    // Convert a static address (as seen in a disassembler) to a runtime address.
    uint64_t fromStatic(uint64_t staticAddress) const { return staticAddress + slide_; }

    // ---------------------------------------------------------------- read

    template <typename T>
    std::optional<T> read(uint64_t address) const {
        static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
        T out{};
        mach_vm_size_t got = 0;
        kern_return_t kr = mach_vm_read_overwrite(
            task_, address, sizeof(T),
            reinterpret_cast<mach_vm_address_t>(&out), &got);
        if (kr != KERN_SUCCESS || got != sizeof(T)) return std::nullopt;
        return out;
    }

    bool readBytes(uint64_t address, void* dst, size_t length) const {
        mach_vm_size_t got = 0;
        kern_return_t kr = mach_vm_read_overwrite(
            task_, address, length,
            reinterpret_cast<mach_vm_address_t>(dst), &got);
        return kr == KERN_SUCCESS && got == length;
    }

    // Reads a NUL-terminated string, capped at maxLength.
    std::string readString(uint64_t address, size_t maxLength = 64) const {
        std::vector<char> buf(maxLength + 1, 0);
        if (!readBytes(address, buf.data(), maxLength)) return {};
        return std::string(buf.data());
    }

    // --------------------------------------------------------------- write

    template <typename T>
    bool write(uint64_t address, const T& value, std::string& error) {
        static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
        return writeBytes(address, &value, sizeof(T), error);
    }

    // Temporarily lifts page protection when the target page is not writable,
    // then restores the original protection.
    bool writeBytes(uint64_t address, const void* src, size_t length, std::string& error) {
        mach_vm_address_t regionAddr = address;
        mach_vm_size_t    regionSize = 0;
        natural_t         depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count;

        vm_prot_t original = VM_PROT_READ | VM_PROT_EXECUTE;
        bool protectionChanged = false;
        mach_vm_size_t pageSize = static_cast<mach_vm_size_t>(getpagesize());
        if (pageSize < 4096) pageSize = 4096;
        mach_vm_address_t pageStart = address & ~(pageSize - 1);
        mach_vm_size_t pageLen = ((address + length + pageSize - 1) & ~(pageSize - 1)) - pageStart;

        while (true) {
            count = VM_REGION_SUBMAP_INFO_COUNT_64;
            kern_return_t kr = mach_vm_region_recurse(
                task_, &regionAddr, &regionSize, &depth,
                reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
            if (kr != KERN_SUCCESS) break;
            if (info.is_submap) { depth++; continue; }
            original = info.protection;
            break;
        }

        if (!(original & VM_PROT_WRITE)) {
            kern_return_t kp = mach_vm_protect(task_, pageStart, pageLen, FALSE,
                                               original | VM_PROT_WRITE | VM_PROT_COPY);
            if (kp != KERN_SUCCESS) {
                kp = mach_vm_protect(task_, pageStart, pageLen, FALSE,
                                    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
            }
            if (kp != KERN_SUCCESS) {
                kp = mach_vm_protect(task_, pageStart, pageLen, FALSE,
                                    VM_PROT_READ | VM_PROT_WRITE);
            }
            if (kp == KERN_SUCCESS) {
                protectionChanged = true;
            }
        }

        kern_return_t kw = mach_vm_write(
            task_, address,
            reinterpret_cast<vm_offset_t>(const_cast<void*>(src)),
            static_cast<mach_msg_type_number_t>(length));

        if (protectionChanged)
            mach_vm_protect(task_, pageStart, pageLen, FALSE, original);

        if (kw != KERN_SUCCESS) {
            error = std::string("mach_vm_write failed: ") + mach_error_string(kw);
            return false;
        }
        return true;
    }

    // -------------------------------------------------------------- layout

    // Enumerates readable/writable regions. Useful for scanning.
    std::vector<Region> regions(bool writableOnly = true) const {
        std::vector<Region> out;
        mach_vm_address_t address = 0;
        mach_vm_size_t    size    = 0;
        natural_t         depth   = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count;

        while (true) {
            count = VM_REGION_SUBMAP_INFO_COUNT_64;
            if (mach_vm_region_recurse(task_, &address, &size, &depth,
                    reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS)
                break;
            if (info.is_submap) { depth++; continue; }

            bool r = info.protection & VM_PROT_READ;
            bool w = info.protection & VM_PROT_WRITE;
            if (r && (!writableOnly || w))
                out.push_back({ address, size, info.protection });

            address += size;
        }
        return out;
    }

    // Heuristic: does this look like a valid 8-byte-aligned userspace pointer?
    // Cheap guard that stops a broken chain from being followed into garbage.
    static bool plausiblePointer(uint64_t p) {
        return p >= 0x100000000ULL && p < 0x800000000000ULL && (p & 7) == 0;
    }

    // ---------------------------------------------------------------- chain

    // Walks a pointer chain starting at a static address.
    //
    //   chain(0x3501220, { 0x2D8, 0x40 })
    //     tmp  = [imageBase + 0x3501220]
    //     tmp  = [tmp + 0x2D8]
    //     return tmp + 0x40          <- final offset is NOT dereferenced
    //
    // Returns nullopt as soon as any intermediate pointer fails validation,
    // rather than propagating garbage.
    std::optional<uint64_t> chain(uint64_t staticOffset,
                                  std::initializer_list<int64_t> offsets) const {
        auto root = read<uint64_t>(imageBase_ + staticOffset);
        if (!root || !plausiblePointer(*root)) return std::nullopt;

        uint64_t address = *root;
        size_t index = 0, total = offsets.size();

        for (int64_t off : offsets) {
            address += off;
            if (++index == total) break;             // last hop stays an address
            auto next = read<uint64_t>(address);
            if (!next || !plausiblePointer(*next)) return std::nullopt;
            address = *next;
        }
        return address;
    }

private:
    pid_t       pid_       = 0;
    mach_port_t task_      = MACH_PORT_NULL;
    uint64_t    imageBase_ = 0;
    uint64_t    slide_     = 0;

    static pid_t findPid(const std::string& needle) {
        int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
        size_t size = 0;
        if (sysctl(mib, 3, nullptr, &size, nullptr, 0) < 0) return 0;

        std::vector<char> buffer(size);
        if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) < 0) return 0;

        auto* procs = reinterpret_cast<struct kinfo_proc*>(buffer.data());
        int n = static_cast<int>(size / sizeof(struct kinfo_proc));
        pid_t self = getpid();

        for (int i = 0; i < n; ++i) {
            if (procs[i].kp_proc.p_pid == self) continue;   // never match ourselves
            if (std::string(procs[i].kp_proc.p_comm).find(needle) != std::string::npos)
                return procs[i].kp_proc.p_pid;
        }
        return 0;
    }

    // Finds the main executable's Mach-O header in the target and derives the
    // ASLR slide from the __TEXT segment's linked vmaddr.
    bool resolveImageBase() {
        for (const Region& r : regions(/*writableOnly=*/false)) {
            struct mach_header_64 header{};
            if (!readBytes(r.base, &header, sizeof(header))) continue;
            if (header.magic != MH_MAGIC_64 || header.filetype != MH_EXECUTE) continue;

            std::vector<char> commands(header.sizeofcmds);
            if (readBytes(r.base + sizeof(header), commands.data(), commands.size())) {
                const char* cursor = commands.data();
                for (uint32_t i = 0; i < header.ncmds; ++i) {
                    auto* lc = reinterpret_cast<const load_command*>(cursor);
                    if (lc->cmd == LC_SEGMENT_64) {
                        auto* seg = reinterpret_cast<const segment_command_64*>(cursor);
                        if (std::strcmp(seg->segname, "__TEXT") == 0) {
                            imageBase_ = r.base;
                            slide_     = r.base - seg->vmaddr;
                            return true;
                        }
                    }
                    cursor += lc->cmdsize;
                }
            }

            imageBase_ = r.base;
            slide_     = 0;
            return true;
        }
        return false;
    }
};

} // namespace mem
