// buildings.cpp - identify and set HOI4 building levels
//
// build : clang++ -std=c++17 -O2 buildings.cpp -o buildings
// run   : sudo ./buildings
//
// Needs memory.hpp, hoi4_offsets.hpp and hoi4_sdk.hpp in the same directory.
//
// The building container uses a stable index per building type across every
// state, but the index -> type mapping is not stored anywhere readable. This
// tool identifies them by comparison: it prints a grid of index against state,
// and you match a column against what the state screen shows.

#include "hoi4_sdk.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <iostream>

namespace {

// Filled in as they are confirmed. Index 0 was verified on twelve states at
// once - every state's infrastructure went to the written value.
const std::map<int, const char*> kKnownIndices = {
    { 0, "infrastructure" },
};

bool readLine(std::string& out) {
    if (!std::getline(std::cin, out)) return false;
    size_t start = out.find_first_not_of(" \t\r\n");
    size_t end   = out.find_last_not_of(" \t\r\n");
    out = (start == std::string::npos) ? "" : out.substr(start, end - start + 1);
    return true;
}

long long promptNumber(const char* prompt, long long fallback) {
    std::printf("%s [%lld]: ", prompt, fallback);
    std::fflush(stdout);
    std::string input;
    if (!readLine(input) || input.empty()) return fallback;
    try { return std::stoll(input); } catch (...) { return fallback; }
}

const char* nameFor(int index) {
    auto it = kKnownIndices.find(index);
    return (it != kKnownIndices.end()) ? it->second : "";
}

// Prints index down the side, states across the top. Only rows where at least
// one state has a non-zero level are shown - the rest are noise.
void printGrid(hoi4::Game& game, uint64_t country) {
    auto states = game.states(country);
    if (states.empty()) { std::printf("No states.\n"); return; }

    std::vector<std::vector<hoi4::StateBuilding>> perState;
    perState.reserve(states.size());
    size_t maxIndex = 0;

    for (uint64_t s : states) {
        auto buildings = game.stateBuildings(s);
        for (const auto& b : buildings)
            if (static_cast<size_t>(b.index) + 1 > maxIndex)
                maxIndex = static_cast<size_t>(b.index) + 1;
        perState.push_back(std::move(buildings));
    }

    std::printf("\nBuilding levels, index down the side, state across the top.\n");
    std::printf("Only rows with a non-zero value somewhere are listed.\n\n");

    std::printf("%-6s", "idx");
    for (size_t i = 0; i < states.size(); ++i) std::printf("%4zu", i);
    std::printf("   name\n");

    std::printf("%-6s", "");
    for (size_t i = 0; i < states.size(); ++i) std::printf("----");
    std::printf("\n");

    for (size_t index = 0; index < maxIndex; ++index) {
        std::vector<int> row(states.size(), 0);
        bool any = false;

        for (size_t s = 0; s < perState.size(); ++s)
            for (const auto& b : perState[s])
                if (static_cast<size_t>(b.index) == index) {
                    row[s] = b.level;
                    if (b.level != 0) any = true;
                }

        if (!any) continue;

        std::printf("%-6zu", index);
        for (int v : row) std::printf("%4d", v);
        std::printf("   %s\n", nameFor(static_cast<int>(index)));
    }

    std::printf("\n%zu states. Match a column against the state screen to name an index.\n",
                states.size());
}

void listOneState(hoi4::Game& game, uint64_t country, size_t which) {
    auto states = game.states(country);
    if (which >= states.size()) { std::printf("No such state.\n"); return; }

    auto buildings = game.stateBuildings(states[which]);
    std::printf("\nState %zu at 0x%llx - %zu entries\n\n", which,
                static_cast<unsigned long long>(states[which]), buildings.size());
    std::printf("%-6s %-8s %s\n", "idx", "level", "name");

    for (const auto& b : buildings) {
        if (b.level == 0) continue;
        std::printf("%-6d %-8d %s\n", b.index, b.level, nameFor(b.index));
    }
    std::printf("\n(zero-level entries hidden)\n");
}

} // namespace

int main() {
    std::printf("HOI4 building tool - macOS x86_64\n");
    std::printf("--------------------------------------------------\n");

    mem::Process process;
    std::string error;
    if (!process.attach("hoi4", error)) {
        std::fprintf(stderr, "[-] %s\n    Is the game running? Started with sudo?\n",
                     error.c_str());
        return 1;
    }
    std::printf("Attached to pid %d\n", process.pid());

    hoi4::Game game(process);
    if (!game.playerCountry()) {
        std::fprintf(stderr, "[-] No player country. Load a campaign first.\n");
        return 2;
    }

    while (true) {
        auto country = game.playerCountry();
        if (!country) { std::printf("Lost the country. Still in a campaign?\n"); break; }

        std::printf("\n--------------------------------------------------\n");
        std::printf("  1) Grid of all indices across all states\n");
        std::printf("  2) List one state's buildings\n");
        std::printf("  3) Set one index in every state\n");
        std::printf("  4) Probe an index (write 7, then check in game)\n");
        std::printf("  q) Quit\n");
        std::printf("--------------------------------------------------\n");
        std::printf("Choice: ");
        std::fflush(stdout);

        std::string choice;
        if (!readLine(choice)) break;
        if (choice == "q" || choice == "Q") break;

        if (choice == "1") {
            printGrid(game, *country);
        }
        else if (choice == "2") {
            long long which = promptNumber("Which state (0-based)", 0);
            listOneState(game, *country, static_cast<size_t>(which));
        }
        else if (choice == "3") {
            long long index = promptNumber("Building index", 0);
            long long level = promptNumber("Level to set", 10);

            int touched = 0;
            if (!game.setBuildingEverywhere(*country, static_cast<int>(index),
                                            static_cast<int16_t>(level), error, &touched)) {
                std::printf("[-] %s\n", error.c_str());
            } else {
                std::printf("Index %lld set to %lld in %d states.\n", index, level, touched);
                std::printf("Factories are also capped by unlocked building slots,\n"
                            "so a high level may not all be usable.\n");
            }
        }
        else if (choice == "4") {
            long long index = promptNumber("Index to probe", 1);
            int touched = 0;
            if (!game.setBuildingEverywhere(*country, static_cast<int>(index), 7,
                                            error, &touched)) {
                std::printf("[-] %s\n", error.c_str());
            } else {
                std::printf("\nWrote 7 to index %lld in %d states.\n", index, touched);
                std::printf("Open a state in game - whichever counter reads 7 is this index.\n");
            }
        }
        else {
            continue;
        }

        std::printf("\nPress enter to continue...");
        std::fflush(stdout);
        std::string dummy;
        readLine(dummy);
    }

    std::printf("\nDone.\n");
    return 0;
}
