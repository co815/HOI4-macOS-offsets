#include "memory.hpp"
#include "hoi4_sdk.hpp"
#include <cstdio>
#include <vector>

int main() {
    mem::Process proc;
    std::string error;
    if (!proc.attach("hoi4", error)) {
        std::printf("Could not attach: %s\n", error.c_str());
        return 1;
    }
    std::printf("Attached to hoi4 (PID %d), imageBase: 0x%llx\n", proc.pid(), proc.imageBase());

    hoi4::Game game(proc);
    auto countryOpt = game.playerCountry();
    if (!countryOpt) {
        std::printf("Could not resolve player country\n");
        return 1;
    }
    uint64_t country = *countryOpt;
    std::printf("Player country: 0x%llx\n", country);

    // Check last selected unit global
    auto sel = game.selectedUnit();
    std::printf("lastSelectedUnit (0x34EE050): 0x%llx\n", sel ? *sel : 0ULL);
    if (sel && *sel != 0) {
        std::printf("Dumping fields at selected unit (0x%llx):\n", *sel);
        for (int64_t off = 0; off < 0x500; off += 8) {
            auto val = proc.read<uint64_t>(*sel + off);
            if (val && *val != 0) {
                if (*val == country) {
                    std::printf("  +0x%03llx: 0x%llx (MATCHES COUNTRY!)\n", off, *val);
                } else if (*val < 20000000) {
                    auto val32 = proc.read<int32_t>(*sel + off);
                    if (val32 && *val32 > 1000) {
                        std::printf("  +0x%03llx: %d (/100k: %.2f)\n", off, *val32, *val32 / 100000.0);
                    }
                }
            }
        }
    }

    // Scan country object for arrays of units/armies/theatres
    // CPdxArray: ptr at +0x00, cap at +0x08, count at +0x0C
    std::printf("\nScanning Country (0x%llx) for CPdxArray:\n", country);
    for (int64_t off = 0; off < 0x1500; off += 8) {
        auto ptr = proc.read<uint64_t>(country + off);
        auto cap = proc.read<int32_t>(country + off + 0x08);
        auto cnt = proc.read<int32_t>(country + off + 0x0C);
        if (ptr && cnt && cap && mem::Process::plausiblePointer(*ptr) && *cnt > 0 && *cnt <= *cap && *cap < 50000) {
            std::printf("  Array at Country + 0x%03llx: ptr=0x%llx, cap=%d, count=%d\n", off, *ptr, *cap, *cnt);
            for (int32_t j = 0; j < std::min(*cnt, 3); ++j) {
                auto elem = proc.read<uint64_t>(*ptr + j * 8);
                if (elem && mem::Process::plausiblePointer(*elem)) {
                    auto tag = proc.read<int32_t>(*elem + 8);
                    auto owner = proc.read<uint64_t>(*elem + 0x820);
                    std::printf("    [%d] = 0x%llx (tag=%d, owner=0x%llx)\n", j, *elem, tag ? *tag : -1, owner ? *owner : 0ULL);
                }
            }
        }
    }

    return 0;
}
