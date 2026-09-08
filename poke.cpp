// poke.cpp - direct read and write at an address, for testing purposes
//
// build : clang++ -std=c++17 -O2 poke.cpp -o poke
// run   : sudo ./poke
//
// Exists because lldb sometimes enters a state where writes fail
// silently. This uses the same memory layer as the trainer, which works.
//
// Commands:
//   r <address>           read 8 bytes (hex + decimal + /100000)
//   d <address> [n]       dump n qwords (default 8)
//   w <address> <value>   write 8 bytes
//   u <address> <units>   write units * 100000 (equipment scale)
//   watch <address>       read every second, show when it changes
//   q
//
// Addresses are specified in hex, with or without 0x.

#include "memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <unistd.h>

namespace {

constexpr int64_t kEquipmentScale = 100000;

bool readLine(std::string& out) {
    if (!std::getline(std::cin, out)) return false;
    size_t start = out.find_first_not_of(" \t\r\n");
    size_t end   = out.find_last_not_of(" \t\r\n");
    out = (start == std::string::npos) ? "" : out.substr(start, end - start + 1);
    return true;
}

// Accepts 0x600006d19b28 or 600006d19b28.
bool parseAddress(const std::string& text, uint64_t& out) {
    if (text.empty()) return false;
    const char* s = text.c_str();
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        s += 2;

    char* end = nullptr;
    unsigned long long value = std::strtoull(s, &end, 16);
    if (end == s || *end != '\0') return false;

    out = static_cast<uint64_t>(value);
    return true;
}

bool parseSigned(const std::string& text, long long& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

void showValue(uint64_t address, int64_t raw) {
    std::printf("  0x%llx  =  %lld\n",
                static_cast<unsigned long long>(address),
                static_cast<long long>(raw));
    if (raw % kEquipmentScale == 0) {
        std::printf("               %lld at the /100000 scale\n",
                    static_cast<long long>(raw / kEquipmentScale));
    } else {
        std::printf("               %.2f at the /100000 scale\n",
                    static_cast<double>(raw) / kEquipmentScale);
    }
}

void doRead(mem::Process& process, uint64_t address) {
    auto value = process.read<int64_t>(address);
    if (!value) {
        std::printf("[-] Could not read that address.\n");
        return;
    }
    showValue(address, *value);
}

void doDump(mem::Process& process, uint64_t address, int count) {
    if (count <= 0 || count > 64) count = 8;

    for (int i = 0; i < count; ++i) {
        const uint64_t at = address + static_cast<uint64_t>(i) * 8;
        auto value = process.read<uint64_t>(at);
        if (!value) {
            std::printf("  +0x%-4x  <unreadable>\n", i * 8);
            continue;
        }

        std::printf("  +0x%-4x  0x%016llx  %lld\n",
                    i * 8,
                    static_cast<unsigned long long>(*value),
                    static_cast<long long>(static_cast<int64_t>(*value)));
    }
}

void doWrite(mem::Process& process, uint64_t address, int64_t value) {
    auto before = process.read<int64_t>(address);
    if (!before) {
        std::printf("[-] Could not read that address - not writing.\n");
        return;
    }

    std::string error;
    if (!process.write<int64_t>(address, value, error)) {
        std::printf("[-] Write failed: %s\n", error.c_str());
        return;
    }

    auto after = process.read<int64_t>(address);
    if (!after) {
        std::printf("[-] Wrote, but could not read back.\n");
        return;
    }

    std::printf("  before: %lld\n", static_cast<long long>(*before));
    std::printf("  after : %lld\n", static_cast<long long>(*after));

    if (*after != value)
        std::printf("\n  The value did not stick - the game overwrote it\n"
                    "  immediately, so this address is derived, not a source.\n");
}

// Reads once a second and reports changes. This is how you tell a real
// source from a value the game recomputes: leave it running after a write
// and see whether the number survives.
void doWatch(mem::Process& process, uint64_t address) {
    auto initial = process.read<int64_t>(address);
    if (!initial) {
        std::printf("[-] Could not read that address.\n");
        return;
    }

    std::printf("Watching 0x%llx, starting at %lld.\n",
                static_cast<unsigned long long>(address),
                static_cast<long long>(*initial));
    std::printf("Press Ctrl+C to stop.\n\n");

    int64_t previous = *initial;
    int     tick     = 0;

    while (true) {
        sleep(1);
        ++tick;

        auto now = process.read<int64_t>(address);
        if (!now) {
            std::printf("  [%3ds] unreadable - the object moved\n", tick);
            continue;
        }

        if (*now != previous) {
            std::printf("  [%3ds] %lld -> %lld  (%+lld)\n", tick,
                        static_cast<long long>(previous),
                        static_cast<long long>(*now),
                        static_cast<long long>(*now - previous));
            previous = *now;
        }
    }
}

void help() {
    std::printf("\n");
    std::printf("  r <addr>            read 8 bytes\n");
    std::printf("  d <addr> [n]        dump n qwords (default 8)\n");
    std::printf("  w <addr> <value>    write 8 bytes\n");
    std::printf("  u <addr> <units>    write units * 100000\n");
    std::printf("  watch <addr>        poll once a second, report changes\n");
    std::printf("  q                   quit\n");
    std::printf("\n");
    std::printf("Addresses are hex, with or without 0x. Values are decimal.\n");
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    std::printf("poke - direct memory read/write\n");
    std::printf("--------------------------------------------------\n");

    mem::Process process;
    std::string  error;

    const char* target = (argc > 1) ? argv[1] : "hoi4";
    if (!process.attach(target, error)) {
        std::fprintf(stderr, "[-] %s\n", error.c_str());
        std::fprintf(stderr, "    Is the game running? Did you start this with sudo?\n");
        return 1;
    }

    std::printf("Attached to pid %d (base 0x%llx, slide 0x%llx)\n",
                process.pid(),
                static_cast<unsigned long long>(process.imageBase()),
                static_cast<unsigned long long>(process.slide()));

    help();

    while (true) {
        std::printf("> ");
        std::fflush(stdout);

        std::string line;
        if (!readLine(line)) break;
        if (line.empty()) continue;

        std::istringstream stream(line);
        std::string        command;
        stream >> command;

        if (command == "q" || command == "quit") break;
        if (command == "h" || command == "help") { help(); continue; }

        std::string addressText;
        stream >> addressText;

        uint64_t address = 0;
        if (!parseAddress(addressText, address)) {
            std::printf("[-] Bad address. Give it in hex, e.g. 600006d19b28\n");
            continue;
        }

        if (command == "r") {
            doRead(process, address);

        } else if (command == "d") {
            std::string countText;
            stream >> countText;
            long long count = 8;
            if (!countText.empty()) parseSigned(countText, count);
            doDump(process, address, static_cast<int>(count));

        } else if (command == "w" || command == "u") {
            std::string valueText;
            stream >> valueText;

            long long value = 0;
            if (!parseSigned(valueText, value)) {
                std::printf("[-] Bad value. Give it in decimal.\n");
                continue;
            }

            if (command == "u") value *= kEquipmentScale;
            doWrite(process, address, value);

        } else if (command == "watch") {
            doWatch(process, address);

        } else {
            std::printf("[-] Unknown command. Type help.\n");
        }
    }

    std::printf("\nDone.\n");
    return 0;
}
