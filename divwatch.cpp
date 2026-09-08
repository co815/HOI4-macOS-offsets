// divwatch.cpp - snapshot / diff / search an object in a live process
//
// build : clang++ -std=c++17 -O2 divwatch.cpp -o divwatch
// run   : sudo ./divwatch <hex-address|sel> [length] [--expect A B]
//         sudo ./divwatch find <hex-address|sel> <length> <value> [value...]
//
// Needs memory.hpp in the same directory.
//
// Two modes.
//
// Diff mode takes a snapshot of `length` bytes at the address, waits for
// enter, takes a second one, and prints only the 8-byte slots that changed -
// each with every interpretation worth considering, so the encoding does not
// have to be guessed up front. --expect A B highlights slots whose value went
// from roughly A to roughly B under some interpretation; feed it the two
// numbers the UI showed at each snapshot.
//
// Find mode takes a single snapshot and prints every slot holding one of the
// values given. This is the check that turns a candidate offset into a
// confirmed one: read a number off the tooltip, search for it, and see
// whether the offset the diff suggested is where it lands.
//
// `sel` in place of an address reads the pointer unit_address caches in its
// global, so the address does not have to be copied by hand.

#include "memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <iostream>

namespace {

constexpr size_t kSlot = 8;

// Written by the unit_address handler (sub_1001628C0) with the pointer it
// just printed. Survives until the command is run again.
constexpr uint64_t kLastSelectedUnit = 0x34EE050;

// The handler accepts a unit whose type tag at +0x08 is 0, 1 or 13.
constexpr int64_t  kUnitTypeTag  = 0x08;
constexpr uint32_t kUnitTypeMask = 0x2003;

struct Reading {
    const char* label;
    double      value;
    bool        valid;
};

// Every encoding this codebase has actually run into, plus the plain ones.
// Fixed-point scales come straight from the scale table in the notes.
std::vector<Reading> interpret(const uint8_t* p) {
    int32_t  i32a = 0, i32b = 0;
    int64_t  i64  = 0;
    float    f32a = 0.0f, f32b = 0.0f;
    double   f64  = 0.0;

    std::memcpy(&i32a, p,     sizeof(i32a));
    std::memcpy(&i32b, p + 4, sizeof(i32b));
    std::memcpy(&i64,  p,     sizeof(i64));
    std::memcpy(&f32a, p,     sizeof(f32a));
    std::memcpy(&f32b, p + 4, sizeof(f32b));
    std::memcpy(&f64,  p,     sizeof(f64));

    const bool f32aOk = std::isfinite(f32a) && std::fabs(f32a) < 1e12;
    const bool f32bOk = std::isfinite(f32b) && std::fabs(f32b) < 1e12;
    const bool f64Ok  = std::isfinite(f64)  && std::fabs(f64)  < 1e12;

    return {
        { "i32",        static_cast<double>(i32a),            true   },
        { "i32@4",      static_cast<double>(i32b),            true   },
        { "i64",        static_cast<double>(i64),             true   },
        { "f32",        static_cast<double>(f32a),            f32aOk },
        { "f32@4",      static_cast<double>(f32b),            f32bOk },
        { "f64",        f64,                                  f64Ok  },
        { "i32/1000",   static_cast<double>(i32a) / 1000.0,   true   },
        { "i32/100000", static_cast<double>(i32a) / 100000.0, true   },
        { "i64/1000",   static_cast<double>(i64)  / 1000.0,   true   },
        { "i64/100000", static_cast<double>(i64)  / 100000.0, true   },
        { "i64/32768",  static_cast<double>(i64)  / 32768.0,  true   },
    };
}

bool close(double a, double b) {
    const double scale = std::fmax(1.0, std::fmax(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) / scale < 0.01;
}

bool snapshot(const mem::Process& process, uint64_t address,
              std::vector<uint8_t>& out) {
    // 1 MB at a time - a single large read that fails partially would
    // silently skip the whole object.
    constexpr size_t kChunk = 1024 * 1024;

    size_t done = 0;
    while (done < out.size()) {
        const size_t take = std::min(kChunk, out.size() - done);
        if (!process.readBytes(address + done, out.data() + done, take)) {
            std::fprintf(stderr, "[-] read failed at +0x%zx\n", done);
            return false;
        }
        done += take;
    }
    return true;
}

// A live unit object carries a small type tag. Freed memory almost never
// does, which is what makes this worth checking before reading offsets out
// of it for an hour.
bool looksLikeUnit(const mem::Process& process, uint64_t address) {
    auto tag = process.read<int32_t>(address + kUnitTypeTag);
    if (!tag) return false;
    if (*tag < 0 || *tag > 0xD) return false;
    return (kUnitTypeMask >> *tag) & 1;
}

// Resolves "sel" to whatever unit_address last cached.
bool resolveAddress(const mem::Process& process, const char* argument,
                    uint64_t& out) {
    if (std::strcmp(argument, "sel") != 0) {
        out = std::strtoull(argument, nullptr, 16);
        if (out == 0) {
            std::fprintf(stderr, "[-] Could not read an address from \"%s\".\n",
                         argument);
            return false;
        }
        return true;
    }

    auto cached = process.read<uint64_t>(process.imageBase() + kLastSelectedUnit);
    if (!cached || !mem::Process::plausiblePointer(*cached)) {
        std::fprintf(stderr,
            "[-] Nothing cached - run unit_address in game first.\n");
        return false;
    }

    out = *cached;
    std::printf("Using the last unit_address selection: 0x%llx\n",
                static_cast<unsigned long long>(out));
    return true;
}

void printSlot(size_t offset, const uint8_t* before, const uint8_t* after,
               bool haveExpect, double expectA, double expectB) {
    auto a = interpret(before);
    auto b = interpret(after);

    // Does any interpretation match the two UI numbers?
    std::string hit;
    if (haveExpect) {
        for (size_t i = 0; i < a.size(); ++i) {
            if (!a[i].valid || !b[i].valid) continue;
            if (close(a[i].value, expectA) && close(b[i].value, expectB)) {
                hit = a[i].label;
                break;
            }
        }
    }

    std::printf("\n+0x%04zx%s\n", offset, hit.empty() ? "" : "   <<< MATCH");

    uint64_t rawA = 0, rawB = 0;
    std::memcpy(&rawA, before, sizeof(rawA));
    std::memcpy(&rawB, after,  sizeof(rawB));
    std::printf("  raw   %016llx -> %016llx\n",
                static_cast<unsigned long long>(rawA),
                static_cast<unsigned long long>(rawB));

    for (size_t i = 0; i < a.size(); ++i) {
        if (!a[i].valid || !b[i].valid) continue;
        if (close(a[i].value, b[i].value)) continue;   // did not move

        const bool marked = (!hit.empty() && hit == a[i].label);
        std::printf("  %-11s%14.4f -> %14.4f  %s%s\n",
                    a[i].label, a[i].value, b[i].value,
                    b[i].value > a[i].value ? "up" : "down",
                    marked ? "   <<<" : "");
    }
}

// One snapshot, every slot matching any of the values given.
int findMode(const mem::Process& process, uint64_t address, size_t length,
             const std::vector<double>& targets) {
    std::vector<uint8_t> buffer(length);
    if (!snapshot(process, address, buffer)) return 2;

    int hits = 0;
    for (size_t offset = 0; offset + kSlot <= length; offset += kSlot) {
        auto values = interpret(buffer.data() + offset);

        for (const auto& v : values) {
            if (!v.valid) continue;

            for (double target : targets) {
                if (!close(v.value, target)) continue;
                std::printf("+0x%04zx  %-11s%14.4f   (looking for %.4f)\n",
                            offset, v.label, v.value, target);
                ++hits;
                break;
            }
        }
    }

    std::printf("\n%d match%s.\n", hits, hits == 1 ? "" : "es");
    if (hits == 0)
        std::printf("Nothing holds those values. Check the tooltip is for the\n"
                    "same division this address came from.\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: sudo %s <hex-address|sel> [length] [--expect A B]\n"
            "       sudo %s find <hex-address|sel> <length> <value> [value...]\n\n"
            "  <hex-address>  from the unit_address console command\n"
            "  sel            whatever unit_address selected last\n"
            "  [length]       bytes to watch, default 0x600\n"
            "  --expect A B   the UI value at snapshot 1 and at snapshot 2\n"
            "  find           one snapshot, print slots holding those values\n",
            argv[0], argv[0]);
        return 1;
    }

    const bool wantFind = (std::strcmp(argv[1], "find") == 0);

    if (wantFind && argc < 5) {
        std::fprintf(stderr,
            "[-] find needs an address, a length and at least one value.\n");
        return 1;
    }

    size_t length = 0x600;

    bool   haveExpect = false;
    double expectA = 0.0, expectB = 0.0;
    std::vector<double> targets;

    // Argument parsing only - the address is resolved after attaching, since
    // "sel" has to be read out of the process.
    if (wantFind) {
        length = std::strtoull(argv[3], nullptr, 0);
        for (int i = 4; i < argc; ++i)
            targets.push_back(std::strtod(argv[i], nullptr));
    } else {
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--expect") == 0 && i + 2 < argc) {
                expectA    = std::strtod(argv[i + 1], nullptr);
                expectB    = std::strtod(argv[i + 2], nullptr);
                haveExpect = true;
                i += 2;
            } else {
                length = std::strtoull(argv[i], nullptr, 0);
            }
        }
    }

    length = (length + kSlot - 1) / kSlot * kSlot;

    mem::Process process;
    std::string error;

    if (!process.attach("hoi4", error)) {
        std::fprintf(stderr, "[-] %s\n", error.c_str());
        std::fprintf(stderr, "    Is the game running? Did you start this with sudo?\n");
        return 1;
    }

    std::printf("Attached to pid %d (base 0x%llx, slide 0x%llx)\n",
                process.pid(),
                static_cast<unsigned long long>(process.imageBase()),
                static_cast<unsigned long long>(process.slide()));

    uint64_t address = 0;
    if (!resolveAddress(process, wantFind ? argv[2] : argv[1], address)) return 2;

    if (!looksLikeUnit(process, address)) {
        std::fprintf(stderr,
            "\n[!] 0x%llx does not carry a unit type tag at +0x08.\n"
            "    It may have been freed and the block reused - run\n"
            "    unit_address again and use the new address.\n\n",
            static_cast<unsigned long long>(address));
    }

    if (wantFind) {
        std::printf("Searching 0x%llx, 0x%zx bytes\n\n",
                    static_cast<unsigned long long>(address), length);
        return findMode(process, address, length, targets);
    }

    std::printf("Watching 0x%llx, 0x%zx bytes\n",
                static_cast<unsigned long long>(address), length);
    if (haveExpect)
        std::printf("Expecting %.4f -> %.4f\n", expectA, expectB);

    std::vector<uint8_t> before(length), after(length);

    if (!snapshot(process, address, before)) return 2;
    std::printf("\nSnapshot 1 taken.\n\n"
                "Now let the value move in game - unpause a few days, or march\n"
                "the division - then pause again and press enter.\n");

    std::string dummy;
    std::getline(std::cin, dummy);

    if (!snapshot(process, address, after)) return 2;

    int changed = 0;
    for (size_t offset = 0; offset + kSlot <= length; offset += kSlot) {
        if (std::memcmp(before.data() + offset, after.data() + offset, kSlot) == 0)
            continue;
        printSlot(offset, before.data() + offset, after.data() + offset,
                  haveExpect, expectA, expectB);
        ++changed;
    }

    std::printf("\n%d of %zu slots changed.\n", changed, length / kSlot);
    if (changed == 0)
        std::printf("Nothing moved - wrong address, or the game was still paused.\n");
    else if (changed > 60)
        std::printf("That is a lot of movement. Take both snapshots with the game\n"
                    "paused, and leave the division stationary - marching moves\n"
                    "position, supply and progress fields all at once.\n");

    return 0;
}
