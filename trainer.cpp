// trainer.cpp - interactive HOI4 trainer (macOS, x86_64)
//
// build : clang++ -std=c++17 -O2 trainer.cpp -o trainer
// run   : sudo ./trainer
//
// Needs memory.hpp, hoi4_offsets.hpp and hoi4_sdk.hpp in the same directory.
//
// No arguments. Attaches to the running game, shows the current state, and
// offers a menu. Everything is resolved fresh on each action, so the menu can
// stay open while the game runs.

#include <type_traits>

#include "hoi4_sdk.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <unistd.h>

namespace {

// A value of 50 across ~24 states lands around 65,000 of each resource once
// state modifiers are applied. Higher values overflow int32 downstream and the
// Logistics bar wraps negative.
constexpr int32_t kDefaultResourceBase = 50;
constexpr int32_t kMaxSafeResourceBase = 200;
constexpr int32_t kDefaultManpower     = 1000000;
constexpr int16_t kDefaultLevel        = 10;
constexpr int64_t kDefaultExperience   = 500;
constexpr int64_t kDefaultPoliticalPower = 1000;
constexpr int64_t kDefaultCommandPower   = 100;
constexpr int64_t kDefaultCommandPowerCap = 500;
constexpr int64_t kDefaultNukes          = 500;
constexpr int32_t kDefaultBreakthrough   = 100;
constexpr int64_t kDefaultStock          = 10000;

// Division godmode defaults. These are in the units the tooltip shows, so
// kDefaultOrganisation = 200 reads as "200" in game, against a template
// maximum that is usually somewhere around 50.
constexpr int64_t kDefaultOrganisation   = 500;
constexpr int64_t kDefaultHitPoints      = 50000;
constexpr int64_t kDefaultCombatStat     = 50000;
constexpr int     kDefaultFreezeInterval = 100;   // milliseconds

// Building indices in the per-state container. The index is stable across
// states and across countries, but the index -> type mapping is not stored
// anywhere readable, so each one has to be confirmed by writing to it and
// looking at the state screen.
//
// Only infrastructure is confirmed so far: writing index 0 across twelve
// states moved every state's infrastructure counter at once.
//
struct BuildingInfo {
    int         id;
    const char* label;
    const char* codeName;
    int         defaultLevel;
};

const std::vector<BuildingInfo> kBuildingsList = {
    { 2,  "Civilian Factories",    "industrial_complex", 20 },
    { 1,  "Military Factories",    "arms_factory",       20 },
    { 11, "Naval Dockyards",       "dockyard",           20 },
    { 0,  "Infrastructure",        "infrastructure",      5 },
    { 13, "Synthetic Refineries",  "synthetic_refinery",  3 },
    { 3,  "Air Bases",             "air_base",           10 },
    { 12, "Anti-Air",              "anti_air_building",   5 },
    { 15, "Radar Stations",        "radar_station",       6 },
    { 14, "Fuel Silos",            "fuel_silo",           5 },
    { 7,  "Naval Bases",           "naval_base",         10 },
    { 8,  "Land Forts (Bunkers)",  "bunker",             10 },
    { 9,  "Coastal Forts",         "coastal_bunker",     10 },
    { 20, "Nuclear Reactors",      "nuclear_reactor",     1 },
    { 4,  "Supply Nodes",          "supply_node",         1 },
    { 5,  "Railways",              "rail_way",            5 },
};

int32_t         g_originalTag  = 0;
int32_t         g_lastTrollTag = 0;

void line() { std::printf("--------------------------------------------------\n"); }

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

    try {
        return std::stoll(input);
    } catch (...) {
        std::printf("    Not a number, using %lld.\n", fallback);
        return fallback;
    }
}

bool confirm(const char* prompt) {
    std::printf("%s [y/N]: ", prompt);
    std::fflush(stdout);
    std::string answer;
    return readLine(answer) && (answer == "y" || answer == "Y");
}

void waitForEnter() {
    std::printf("\nPress enter to continue...");
    std::fflush(stdout);
    std::string dummy;
    readLine(dummy);
}

// ------------------------------------------------------------------ status

void showStatus(hoi4::Game& game, uint64_t country) {
    auto tag = game.playerTag();
    int32_t cur = tag ? *tag : -1;
    std::string tagStr = (cur > 0) ? game.countryTag(country) : "?";
    std::string nameStr = (cur > 0) ? game.countryName(country) : "";

    std::printf("\nPlayer tag      : %d", cur);
    if (!tagStr.empty()) {
        std::printf(" [%s", tagStr.c_str());
        if (!nameStr.empty() && nameStr != tagStr) {
            std::printf(" - %s", nameStr.c_str());
        }
        std::printf("]");
    }
    if (g_originalTag > 0 && cur != g_originalTag) {
        std::printf("  *** [TROLLING / ALT TAG ACTIVE] ***");
    }
    std::printf("\n");

    if (g_originalTag > 0 && cur != g_originalTag) {
        std::string origTagStr = game.countryTag(g_originalTag);
        std::printf("Original country: Tag %d [%s]  (press 't' then 'o' to switch back)\n",
                    g_originalTag, origTagStr.c_str());
    }

    bool isIron = game.isIronman();
    bool isPatchActive = game.isConsolePatchActive();
    std::printf("Game mode       : %s\n",
                isIron ? "IRONMAN (Achievements eligible)" : "Normal (Non-Ironman)");
    std::printf("Dev console     : %s\n",
                isPatchActive ? "UNLOCKED [~ / § key enabled in Ironman]"
                              : (isIron ? "Default (blocked in Ironman - press 'i' to unlock)" : "Default"));

    std::printf("Country address : 0x%llx\n",
                static_cast<unsigned long long>(country));
    std::printf("Owned states    : %zu\n", game.states(country).size());
    std::printf("Total manpower  : %lld\n",
                static_cast<long long>(game.totalManpower(country)));

    auto base      = game.baseResourceTotals(country);
    auto extracted = game.derivedTotals(country, hoi4::ResourceContainer::Extracted);

    if (!base.empty()) {
        std::printf("\n%-12s %-10s %-10s\n", "resource", "base", "extracted");
        for (size_t i = 0; i < base.size(); ++i) {
            long long ex = 0;
            for (const auto& e : extracted)
                if (e.id == base[i].id) { ex = e.value; break; }
            std::printf("%-12s %-10lld %-10lld\n",
                        base[i].name ? base[i].name : "?",
                        static_cast<long long>(base[i].value), ex);
        }
    }
    std::printf("\n");
}

// ---------------------------------------------------------------- manpower

void doManpower(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Add manpower\n");
    line();

    int64_t before = game.totalManpower(country);
    std::printf("Current total: %lld\n\n", static_cast<long long>(before));

    long long amount = promptNumber("How much to add", kDefaultManpower);
    if (amount == 0) { std::printf("Nothing to do.\n"); return; }

    std::string error;
    if (!game.addManpower(country, static_cast<int32_t>(amount), error)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }

    int64_t after = game.totalManpower(country);
    std::printf("\nManpower: %lld -> %lld (%+lld)\n",
                static_cast<long long>(before), static_cast<long long>(after),
                static_cast<long long>(after - before));
    std::printf("\nThe game caps manpower against recruitable population, so a\n"
                "large amount may be trimmed. Option 3 raises that ceiling.\n");
}

void doPopulation(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Raise manpower ceiling\n");
    line();
    std::printf("Scales recruitable population, which is what caps available\n"
                "manpower. More stable than adding manpower directly.\n\n");

    int64_t before = game.totalPopulation(country);
    std::printf("Current population: %lld\n\n", static_cast<long long>(before));

    long long factor = promptNumber("Multiply by", 10);
    if (factor <= 0) { std::printf("Nothing to do.\n"); return; }

    std::string error;
    if (!game.scalePopulation(country, static_cast<double>(factor), error)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }
    std::printf("\nPopulation: %lld -> %lld (x%lld)\n",
                static_cast<long long>(before),
                static_cast<long long>(game.totalPopulation(country)), factor);
}

// --------------------------------------------------------------- resources

void doResources(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Unlimited resources\n");
    line();
    std::printf("Sets the persistent per-state base amount for every resource.\n"
                "This survives ticks and saves - it is the value the game itself\n"
                "multiplies by state modifiers to produce the Trade figures.\n\n");

    long long value = promptNumber("Base amount per state", kDefaultResourceBase);

    if (value > kMaxSafeResourceBase) {
        std::printf("\n[!] %lld is high. State modifiers multiply this before\n"
                    "    summing, and large inputs overflow int32 downstream -\n"
                    "    the Logistics bar then shows negative numbers.\n"
                    "    %d is the tested safe value (~65,000 of each).\n",
                    value, kDefaultResourceBase);
        if (!confirm("    Continue anyway?")) { std::printf("Cancelled.\n"); return; }
    }

    std::string error;
    int states = 0, slots = 0;
    if (!game.setAllResources(country, static_cast<int32_t>(value), error, &states, &slots)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }
    std::printf("\nWrote %lld to %d slots across %d states.\n", value, slots, states);
}

// --------------------------------------------------------------- buildings

// Prints index down the side, states across the top, so a column can be
// matched against what the state screen shows.
void showBuildingGrid(hoi4::Game& game, uint64_t country) {
    auto states = game.states(country);
    if (states.empty()) { std::printf("No states.\n"); return; }

    std::vector<std::vector<hoi4::StateBuilding>> perState;
    size_t maxIndex = 0;

    for (uint64_t s : states) {
        auto buildings = game.stateBuildings(s);
        for (const auto& b : buildings)
            if (static_cast<size_t>(b.index) + 1 > maxIndex)
                maxIndex = static_cast<size_t>(b.index) + 1;
        perState.push_back(std::move(buildings));
    }

    std::printf("\nBuilding levels. Index down the side, state across the top.\n");
    std::printf("Rows that are zero everywhere are hidden.\n\n");

    std::printf("%-5s", "idx");
    for (size_t i = 0; i < states.size(); ++i) std::printf("%4zu", i);
    std::printf("\n%-5s", "");
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

        std::printf("%-5zu", index);
        for (int v : row) std::printf("%4d", v);

        std::printf("  %s", hoi4::buildingDefinitionName(static_cast<int>(index)));
        std::printf("\n");
    }

    std::printf("\n%zu states.\n\n", states.size());
    std::printf("To name a row: open a state in game, count its civilian and\n"
                "military factories, then find the row whose numbers match that\n"
                "pattern across all states. A row with a single non-zero value\n"
                "is usually the dockyard - only coastal states have one.\n");
}

void setNamedBuilding(hoi4::Game& game, uint64_t country, int defId, const char* name, int defaultLevel) {
    std::printf("\nApply %s (ID %d) to:\n", name, defId);
    std::printf("  1) All owned states\n");
    std::printf("  2) A specific state\n");
    long long scope = promptNumber("Choice", 1);
    long long level = promptNumber("Level to set", defaultLevel);

    if (scope == 2) {
        auto states = game.states(country);
        if (states.empty()) {
            std::printf("[-] Country owns no states.\n");
            return;
        }
        std::printf("Country owns %zu states (indexed 0 to %zu).\n", states.size(), states.size() - 1);
        long long sIdx = promptNumber("State index", 0);
        if (sIdx >= 0 && sIdx < static_cast<long long>(states.size())) {
            std::string err;
            if (!game.setBuildingInState(states[static_cast<size_t>(sIdx)], defId, static_cast<int16_t>(level), err))
                std::printf("[-] Failed: %s\n", err.c_str());
            else
                std::printf("\n[+] %s set to %lld in state #%lld.\n", name, level, sIdx);
        } else {
            std::printf("[-] Invalid state index.\n");
        }
    } else {
        std::string err;
        int touched = 0;
        int failed  = 0;
        if (!game.setBuildingEverywhere(country, defId, static_cast<int16_t>(level), err, &touched, &failed))
            std::printf("[-] Failed: %s\n", err.c_str());
        else {
            std::printf("\n[+] %s set to %lld in %d states", name, level, touched);
            if (failed > 0)
                std::printf(" (%d states skipped - building slot not present)", failed);
            std::printf(".\n");
            if (failed > 0) {
                std::printf("\n    Note: Province-level buildings (railways, supply nodes, bunkers,\n"
                            "    naval bases, coastal forts) may not have entries in all states.\n"
                            "    The game only creates their container entries for provinces that\n"
                            "    actually have them. Use instantconstruction + in-game build queue\n"
                            "    for these, or build the first one manually.\n");
            }
        }
    }
}

void doBuildings(hoi4::Game& game, uint64_t country) {
    while (true) {
        line();
        std::printf("Buildings Management\n");
        line();
        for (size_t i = 0; i < kBuildingsList.size(); ++i) {
            std::printf("  %2zu) %-24s (ID %2d)\n",
                        i + 1, kBuildingsList[i].label, kBuildingsList[i].id);
        }
        std::printf("  16) MAX ALL ESSENTIAL BUILDINGS (Civ, Mil, Dock, Infra, Air, AA, Radar, Ref, Silo, Bunkers, Supply, Rails)\n");
        std::printf("  17) Set a custom building ID directly\n");
        std::printf("  18) View grid of building levels across all states\n");
        std::printf("  19) Repair ALL damaged buildings everywhere (100%% health & 0 damage)\n");
        std::printf("   b) Back\n");
        line();
        std::printf("Choice: ");
        std::fflush(stdout);

        std::string choice;
        if (!readLine(choice)) return;
        if (choice == "b" || choice == "B" || choice.empty()) return;

        if (choice == "19") {
            std::string err;
            int repaired = game.repairAllBuildingsEverywhere(country, err);
            std::printf("\n[+] Repaired %d buildings across all states to 100%% health (0 damage, fully operational)!\n", repaired);
            waitForEnter();
            continue;
        }

        if (choice == "16") {
            long long factLvl   = promptNumber("Factory level (civilians, military, dockyards)", 20);
            long long infraLvl  = promptNumber("Infrastructure level", 5);
            long long airLvl    = promptNumber("Air base level", 10);
            long long aaLvl     = promptNumber("Anti-air level", 5);
            long long radarLvl  = promptNumber("Radar station level", 6);
            long long refLvl    = promptNumber("Synthetic refinery level", 3);
            long long siloLvl   = promptNumber("Fuel silo level", 5);
            long long fortLvl   = promptNumber("Land & coastal bunker level", 10);
            long long supplyLvl = promptNumber("Supply node level", 1);
            long long railLvl   = promptNumber("Railways level", 5);

            std::string err;
            int touched = 0;
            if (!game.setCoreBuildingsEverywhere(country,
                                                static_cast<int16_t>(factLvl),
                                                static_cast<int16_t>(factLvl),
                                                static_cast<int16_t>(factLvl),
                                                static_cast<int16_t>(infraLvl),
                                                static_cast<int16_t>(refLvl),
                                                static_cast<int16_t>(airLvl),
                                                static_cast<int16_t>(aaLvl),
                                                static_cast<int16_t>(radarLvl),
                                                static_cast<int16_t>(siloLvl),
                                                static_cast<int16_t>(fortLvl),
                                                static_cast<int16_t>(supplyLvl),
                                                static_cast<int16_t>(railLvl),
                                                err, &touched)) {
                std::printf("[-] Failed: %s\n", err.c_str());
            } else {
                std::printf("\n[+] SUCCESS! Injected all essential buildings across %d states (0 crashes on conquered territory!):\n"
                            "    - Factories (Civ, Mil, Dock): %lld\n"
                            "    - Infrastructure: %lld | Air Bases: %lld | Anti-Air: %lld\n"
                            "    - Radar: %lld | Refineries: %lld | Fuel Silos: %lld\n"
                            "    - Land/Coastal Bunkers: %lld | Supply Nodes: %lld | Railways: %lld\n"
                            "    - All damaged buildings fully repaired to 100%% health (0 damage)!\n"
                            "    - Instant Construction (ic) automatically ACTIVATED so any new building\n"
                            "      clicked on the map finishes immediately in 1 day!\n",
                            touched, factLvl, infraLvl, airLvl, aaLvl, radarLvl, refLvl, siloLvl, fortLvl, supplyLvl, railLvl);
            }
            waitForEnter();
            continue;
        }

        if (choice == "18") {
            showBuildingGrid(game, country);
            waitForEnter();
            continue;
        }

        if (choice == "17") {
            long long defId = promptNumber("Building definition ID (e.g. 0=infra, 1=mil, 2=civ, 11=dock)", 2);
            setNamedBuilding(game, country, static_cast<int>(defId),
                             hoi4::buildingDefinitionName(static_cast<int>(defId)), kDefaultLevel);
            waitForEnter();
            continue;
        }

        try {
            int num = std::stoi(choice);
            if (num >= 1 && num <= static_cast<int>(kBuildingsList.size())) {
                const auto& b = kBuildingsList[static_cast<size_t>(num - 1)];
                setNamedBuilding(game, country, b.id, b.label, b.defaultLevel);
                waitForEnter();
                continue;
            }
        } catch (...) {}

        std::printf("Unknown choice.\n");
    }
}

void doExperience(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Army / navy / air experience\n");
    line();

    auto before = game.experience(country);
    if (!before) {
        std::printf("[-] Could not resolve the experience object.\n");
        return;
    }

    std::printf("Current   army %lld   navy %lld   air %lld\n\n",
                static_cast<long long>(before->army),
                static_cast<long long>(before->navy),
                static_cast<long long>(before->air));

    long long value = promptNumber("Set all three to", kDefaultExperience);
    if (value < 0) { std::printf("Nothing to do.\n"); return; }

    std::string error;
    if (!game.setAllExperience(country, value, error)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }

    auto after = game.experience(country);
    if (after) {
        std::printf("\nNow       army %lld   navy %lld   air %lld\n",
                    static_cast<long long>(after->army),
                    static_cast<long long>(after->navy),
                    static_cast<long long>(after->air));
    }

    std::printf("\nThese are stored on a 32768 scale, not the 100000 used\n"
                "elsewhere. If the top bar does not move, the army value may\n"
                "live in the accumulator at +0x48 instead - the console path\n"
                "writes both.\n");
}

void doPoliticalPower(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Political power\n");
    line();

    auto before = game.politicalPower(country);
    if (!before) {
        std::printf("[-] Could not resolve the political status object.\n");
        return;
    }

    std::printf("Current: %lld\n\n", static_cast<long long>(*before));

    long long value = promptNumber("Set to", kDefaultPoliticalPower);
    if (value < 0) { std::printf("Nothing to do.\n"); return; }

    std::string error;
    if (!game.setPoliticalPower(country, value, error)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }

    auto after = game.politicalPower(country);
    if (after) {
        std::printf("\nPolitical power: %lld -> %lld\n",
                    static_cast<long long>(*before),
                    static_cast<long long>(*after));
    }

    std::printf("\nThe game clamps this against a global maximum, so a very\n"
                "large value will be pulled back on the next tick.\n");
}

void doCommandPower(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Command power\n");
    line();

    auto before = game.commandPower(country);
    if (!before) {
        std::printf("[-] Could not read command power.\n");
        return;
    }

    std::printf("Current: %lld\n", static_cast<long long>(before->current));
    std::printf("Cap raised by: %lld\n\n", static_cast<long long>(before->bonus));

    std::printf("  1) Set command power\n");
    std::printf("  2) Raise the cap\n");
    std::printf("Choice: ");
    std::fflush(stdout);

    std::string choice;
    if (!readLine(choice)) return;

    std::string error;

    if (choice == "1") {
        long long value = promptNumber("Set command power to", kDefaultCommandPower);
        if (!game.setCommandPower(country, value, error)) {
            std::printf("[-] Failed: %s\n", error.c_str());
            return;
        }
        std::printf("\nThe game clamps this to the cap on the next tick, so raise\n"
                    "the cap first if the value gets trimmed.\n");
    } else if (choice == "2") {
        long long bonus = promptNumber("Raise the cap by", kDefaultCommandPowerCap);
        if (!game.raiseCommandPowerCap(country, bonus, error)) {
            std::printf("[-] Failed: %s\n", error.c_str());
            return;
        }
        std::printf("\nThe tooltip should now show this as \"Allocated\" on top of\n"
                    "the base cap.\n");
    } else {
        std::printf("Nothing to do.\n");
        return;
    }

    auto after = game.commandPower(country);
    if (after) {
        std::printf("\nCommand power %lld, cap raised by %lld\n",
                    static_cast<long long>(after->current),
                    static_cast<long long>(after->bonus));
    }
}

void doNukes(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Nuclear bombs\n");
    line();

    auto before = game.nukes(country);
    if (!before) {
        std::printf("[-] Could not resolve the nuke object.\n");
        return;
    }

    std::printf("Current: %lld\n\n", static_cast<long long>(*before));

    long long count = promptNumber("Set to", kDefaultNukes);
    if (count < 0) { std::printf("Nothing to do.\n"); return; }

    std::string error;
    if (!game.setNukes(country, count, error)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }

    auto after = game.nukes(country);
    if (after) {
        std::printf("\nNukes: %lld -> %lld\n",
                    static_cast<long long>(*before),
                    static_cast<long long>(*after));
    }

    std::printf("\nThe game clamps this at 1000 internally.\n");
}

void doResearchOnClick(hoi4::Game& game) {
    line();
    std::printf("Research on icon click\n");
    line();

    auto state = game.researchOnIconClick();
    if (!state) {
        std::printf("[-] Could not read the flag.\n");
        return;
    }

    std::printf("Currently: %s\n\n", *state ? "on" : "off");
    std::printf("Turn it %s? [y/N]: ", *state ? "off" : "on");
    std::fflush(stdout);

    std::string answer;
    if (!readLine(answer)) return;
    if (answer != "y" && answer != "Y") { std::printf("Left alone.\n"); return; }

    std::string error;
    if (!game.setResearchOnIconClick(!*state, error)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }

    std::printf("\nNow %s. Clicking a technology in the tree researches it\n"
                "instantly. This is a UI flag - the AI never clicks, so it\n"
                "only affects you.\n", *state ? "off" : "on");
}

// The toggles are all one byte in the image, flipped by their console
// commands with a plain XOR, so they share this.
template <typename Read, typename Write>
void toggleFlag(const char* title, const char* note, Read read, Write write) {
    line();
    std::printf("%s\n", title);
    line();

    auto state = read();
    if (!state) {
        std::printf("[-] Could not read the flag.\n");
        return;
    }

    std::printf("Currently: %s\n\n", *state ? "on" : "off");
    std::printf("Turn it %s? [y/N]: ", *state ? "off" : "on");
    std::fflush(stdout);

    std::string answer;
    if (!readLine(answer)) return;
    if (answer != "y" && answer != "Y") { std::printf("Left alone.\n"); return; }

    std::string error;
    if (!write(!*state, error)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }

    std::printf("\nNow %s.\n", *state ? "off" : "on");
    if (note && *note) std::printf("%s\n", note);
}

void doToggles(hoi4::Game& game) {
    line();
    std::printf("Instant toggles\n");
    line();

    auto show = [](const char* name, std::optional<bool> state) {
        std::printf("  %-30s%s\n", name,
                    state ? (*state ? "on" : "off") : "?");
    };

    auto navalDays = game.navalInvasionPrepareDays();
    bool navalOn = navalDays && *navalDays == 0;
    auto paraHours = game.paradropHours();
    bool paraOn = paraHours && *paraHours == 0;

    show("1) Special projects",          game.instantSpecialProjects());
    show("2) Ship refit",                game.instantShipRefit());
    show("3) Construction",              game.instantConstruction());
    show("4) Focus autocomplete",        game.focusAutocomplete());
    show("5) Instant training",          game.instantTraining());
    show("6) Allow trait assign",        game.allowTraitAssign());
    show("7) Instant naval invasion",    navalOn);
    show("8) Instant paradrop (0 hrs)",  paraOn);
    std::printf("  9) ENABLE ALL TOGGLES (Projects, Refit, Const, Focus, Training, Traits, Invasion, Paradrop)\n");
    std::printf("\nChoice: ");
    std::fflush(stdout);

    std::string choice;
    if (!readLine(choice)) return;

    if (choice == "1")
        toggleFlag("Instant special projects",
                   "Started projects finish on the daily tick. The prototype\n"
                   "iterations are skipped, and so are their rewards.",
                   [&] { return game.instantSpecialProjects(); },
                   [&](bool on, std::string& e) {
                       return game.setInstantSpecialProjects(on, e);
                   });
    else if (choice == "2")
        toggleFlag("Instant ship refit",
                   "Refits apply immediately instead of occupying a dockyard.",
                   [&] { return game.instantShipRefit(); },
                   [&](bool on, std::string& e) {
                       return game.setInstantShipRefit(on, e);
                   });
    else if (choice == "3")
        toggleFlag("Instant construction",
                   "Buildings only - ship repair and conversion are excluded\n"
                   "in the game's own code, and this applies to every country,\n"
                   "the AI included.",
                   [&] { return game.instantConstruction(); },
                   [&](bool on, std::string& e) {
                       return game.setInstantConstruction(on, e);
                   });
    else if (choice == "4")
        toggleFlag("Focus in 1 day (freefocuses)",
                   "National focuses complete in 1 day on the daily tick and can\n"
                   "be selected freely without prerequisite restrictions.\n"
                   "Corresponds to the game's `freefocuses` (`ff`) cheat.",
                   [&] { return game.focusAutocomplete(); },
                   [&](bool on, std::string& e) {
                       return game.setFocusAutocomplete(on, e);
                   });
    else if (choice == "5")
        toggleFlag("Instant training",
                   "Divisions finish training instantly. Newly deployed\n"
                   "divisions arrive fully trained. Affects ALL countries.",
                   [&] { return game.instantTraining(); },
                   [&](bool on, std::string& e) {
                       return game.setInstantTraining(on, e);
                   });
    else if (choice == "6")
        toggleFlag("Allow all traits (allowtraits)",
                   "Removes restrictions on learning traits. Any trait can be\n"
                   "assigned to any general, field marshal, or admiral.\n"
                   "Corresponds to the `allowtraits` console command.",
                   [&] { return game.allowTraitAssign(); },
                   [&](bool on, std::string& e) {
                       return game.setAllowTraitAssign(on, e);
                   });
    else if (choice == "7")
        toggleFlag("Instant naval invasion",
                   "Sets naval invasion prepare time to 0 days (instant launch).\n"
                   "Also sets airborne preparation days to 0.",
                   [&]() -> std::optional<bool> {
                       auto d = game.navalInvasionPrepareDays();
                       return d ? std::optional<bool>(*d == 0) : std::nullopt;
                   },
                   [&](bool on, std::string& e) {
                       return game.setInstantNavalInvasion(on, e);
                   });
    else if (choice == "8")
        toggleFlag("Instant paradrop",
                   "Sets paradrop preparation hours to 0 (instant drop without 48h delay).",
                   [&]() -> std::optional<bool> {
                       auto h = game.paradropHours();
                       return h ? std::optional<bool>(*h == 0) : std::nullopt;
                   },
                   [&](bool on, std::string& e) {
                       return game.setInstantParadrop(on, e);
                   });
    else if (choice == "9") {
        std::string e;
        int ok = 0, fail = 0;
        auto trySet = [&](const char* name, bool result) {
            if (result) { ok++; std::printf("  [+] %s: enabled\n", name); }
            else        { fail++; std::printf("  [-] %s: FAILED\n", name); }
        };
        trySet("Special projects",      game.setInstantSpecialProjects(true, e));
        trySet("Ship refit",            game.setInstantShipRefit(true, e));
        trySet("Construction",          game.setInstantConstruction(true, e));
        trySet("Focus autocomplete",    game.setFocusAutocomplete(true, e));
        trySet("Instant training",      game.setInstantTraining(true, e));
        trySet("Allow trait assign",    game.setAllowTraitAssign(true, e));
        trySet("Instant naval invasion", game.setInstantNavalInvasion(true, e));
        trySet("Instant paradrop",      game.setInstantParadrop(true, e));
        std::printf("\n[+] %d toggles enabled", ok);
        if (fail > 0) std::printf(", %d failed (offset may need re-deriving)", fail);
        std::printf(".\n");
    }
    else
        std::printf("Nothing to do.\n");
}

void doBreakthroughs(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Breakthrough points\n");
    line();

    auto entries = game.breakthroughs(country);
    if (entries.empty()) {
        std::printf("[-] No entries in the tree yet.\n\n");
        std::printf("    The game creates one lazily per specialization. Open\n"
                    "    Special Projects and start something in each field you\n"
                    "    care about, then come back.\n");
        return;
    }

    std::printf("%zu specialization(s):\n\n", entries.size());
    std::printf("  id      points\n");
    std::printf("  --------------\n");
    for (const auto& e : entries)
        std::printf("  %-8d%d\n", e.id, e.points);
    std::printf("\n");

    long long value = promptNumber("Set every one to", kDefaultBreakthrough);
    if (value < 0) { std::printf("Nothing to do.\n"); return; }

    std::string error;
    int changed = 0;
    if (!game.setAllBreakthroughs(country, static_cast<int32_t>(value), error, &changed)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }

    std::printf("\nSet %d specialization(s) to %lld.\n",
                changed, static_cast<long long>(value));
}

// Setting a quantity on a whole list, shared by the archetype and variant
// paths so the two menus behave identically.
template <typename Entry, typename SetOne, typename SetAll,
          typename SetCost = std::nullptr_t>
void adjustStockList(const std::vector<Entry>& entries,
                     const char* unitWord,
                     SetOne setOne, SetAll setAll,
                     SetCost setCost = nullptr) {
    std::printf("  1) Set every %s to the same amount\n", unitWord);
    std::printf("  2) Set one %s\n", unitWord);
    std::printf("Choice: ");
    std::fflush(stdout);

    std::string choice;
    if (!readLine(choice)) return;

    std::string error;

    if (choice == "1") {
        long long value = promptNumber("Set every one to", kDefaultStock);
        if (value < 0) { std::printf("Nothing to do.\n"); return; }

        int changed = 0;
        if (!setAll(value, error, &changed)) {
            std::printf("[-] Failed: %s\n", error.c_str());
            return;
        }
        std::printf("\nSet %d to %lld.\n", changed, static_cast<long long>(value));

    } else if (choice == "2") {
        long long which = promptNumber("Which number", 0);
        if (which < 0 || which >= static_cast<long long>(entries.size())) {
            std::printf("No entry with that number.\n");
            return;
        }

        const auto& entry = entries[static_cast<size_t>(which)];
        const char* label = entry.name.empty() ? "That one" : entry.name.c_str();

        std::printf("%s, currently %lld.\n", label,
                    static_cast<long long>(entry.quantity));

        long long value = promptNumber("Set it to", kDefaultStock);
        if (value < 0) { std::printf("Nothing to do.\n"); return; }

        if (!setOne(entry, value, error)) {
            std::printf("[-] Failed: %s\n", error.c_str());
            return;
        }
        std::printf("\n%s: %lld -> %lld\n", label,
                    static_cast<long long>(entry.quantity),
                    static_cast<long long>(value));

    } else if constexpr (!std::is_same_v<SetCost, std::nullptr_t>) {
        if (choice == "c" || choice == "C") {
            long long which = promptNumber("Which number", 0);
            if (which < 0 || which >= static_cast<long long>(entries.size())) {
                std::printf("No entry with that number.\n");
                return;
            }

            long long value = promptNumber("Set its cost to", 1);
            if (value < 1) { std::printf("Nothing to do.\n"); return; }

            std::string costError;
            if (!setCost(entries[static_cast<size_t>(which)], value, costError)) {
                std::printf("[-] Failed: %s\n", costError.c_str());
                return;
            }
            std::printf("\nCost set to %lld. Let a day pass and see whether it\n"
                        "holds - the per-line cost did not.\n",
                        static_cast<long long>(value));
            return;
        }
        std::printf("Nothing to do.\n");
    } else {
        std::printf("Nothing to do.\n");
    }
}

void doEquipmentArchetypes(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Equipment by type\n");
    line();

    auto entries = game.equipmentStock(country);
    if (entries.empty()) {
        std::printf("[-] The country holds no equipment.\n");
        return;
    }

    std::printf("\n%zu type%s - these are the rows in Logistics:\n\n",
                entries.size(), entries.size() == 1 ? "" : "s");
    std::printf("  #    in stock   name\n");
    std::printf("  ---------------------------------------------\n");
    for (const auto& e : entries)
        std::printf("  %-5d%-11lld%s\n", e.index,
                    static_cast<long long>(e.quantity),
                    e.name.empty() ? "?" : e.name.c_str());
    std::printf("\n");

    adjustStockList(entries, "type",
        [&](const hoi4::Game::StockEntry& entry, long long value, std::string& error) {
            return game.setStockQuantity(entry, value, error);
        },
        [&](long long value, std::string& error, int* changed) {
            return game.setAllStock(country, value, error, changed);
        });
}

// Shows a preview of an array so the player can recognise their own country
// by the designs in it.
void printVariantPreview(const hoi4::Game::VariantArray& array, int number) {
    std::printf("  %-4d", number);

    int shown = 0;
    for (const auto& e : array.entries) {
        if (e.name.empty() || e.quantity <= 0) continue;
        if (shown > 0) std::printf(", ");
        std::printf("%s (%lld)", e.name.c_str(), static_cast<long long>(e.quantity));
        if (++shown == 3) break;
    }

    if (shown == 0) std::printf("(nothing stocked)");
    std::printf("\n");
}

void doNaval(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Naval object\n");
    line();

    auto base = game.navalBase(country);
    if (!base) {
        std::printf("[-] Could not read it.\n");
        return;
    }

    std::printf("Naval object: 0x%llx\n\n",
                static_cast<unsigned long long>(*base));
    std::printf("Put a ship on the slipway, then in poke:\n\n");
    std::printf("  d %llx 40\n\n", static_cast<unsigned long long>(*base));
    std::printf("Let a few days pass and read it again. Whichever field\n"
                "climbed is the build progress.\n");
}

void doProduction(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Production lines\n");
    line();

    auto lines = game.productionLines(country);
    if (lines.empty()) {
        std::printf("[-] Could not read the production lines.\n");
        return;
    }

    std::printf("%zu line%s.\n\n", lines.size(), lines.size() == 1 ? "" : "s");
    std::printf("  #    cost\n");
    std::printf("  ------------------\n");
    for (const auto& l : lines)
        std::printf("  %-5d%lld\n", l.index, static_cast<long long>(l.cost));

    std::printf("\nThis is read-only. The cost here is the line's own copy of\n"
                "the variant's, and writing it desyncs the two - the game\n"
                "faults on the next read. Change the cost from the variant\n"
                "menu instead, which is where the line reads from.\n");
}
void doAddLatestEquipment(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Add Equipment to Stockpile (Instant Division Resupply)\n");
    line();

    auto held = game.heldVariants(country);
    auto all  = game.designs(country);

    std::printf("Country has %zu active equipment variants in stockpile and %zu known designs.\n\n",
                held.size(), all.size());
    std::printf("  1) Add stock (default 100,000) to ALL equipment variants & designs\n");
    std::printf("  2) Enter custom quantity for all equipment\n");
    std::printf("  b) Back\n");
    line();
    std::printf("Choice [1]: ");
    std::fflush(stdout);

    std::string choice;
    if (!readLine(choice)) return;
    if (choice == "b" || choice == "B") return;

    long long value = 100000;
    if (choice == "2") {
        value = promptNumber("Quantity of each to add", 100000);
    }
    if (value < 0) { std::printf("Nothing to do.\n"); return; }

    std::string error;
    int changed = 0;
    if (!game.addLatestEquipment(country, value, error, &changed)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }

    std::printf("\n[+] Added %lld units across %d equipment variants and designs!\n",
                static_cast<long long>(value), changed);
    std::printf("    Stockpile and Logistics are now fully supplied. Divisions will immediately\n"
                "    equip and reinforce to 100%% strength.\n");
}

void doEquipmentVariants(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Equipment by variant\n");
    line();

    auto entries = game.heldVariants(country);
    if (entries.empty()) {
        std::printf("[-] The country holds no variants.\n");
        return;
    }

    std::printf("%zu variant%s - these are the rows in Stockpile, and what\n"
                "divisions actually draw on:\n\n",
                entries.size(), entries.size() == 1 ? "" : "s");
    std::printf("  #    in stock   name\n");
    std::printf("  ---------------------------------------------\n");
    for (const auto& e : entries)
        std::printf("  %-5d%-11lld%s\n", e.index,
                    static_cast<long long>(e.quantity),
                    e.name.empty() ? "?" : e.name.c_str());
    std::printf("\n");

    adjustStockList(entries, "variant",
        [&](const hoi4::Game::VariantEntry& entry, long long value, std::string& error) {
            return game.setHeldVariant(entry, value, error);
        },
        [&](long long value, std::string& error, int* changed) {
            return game.setAllHeldVariants(country, value, error, changed);
        });
}

// Production cost lives on the variant, and the build time scales with it.
// Only the country's own designs are listed - captured equipment is in the
// stockpile too, but it is not what the player builds.
void doCosts(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Production costs\n");
    line();

    auto entries = game.designCosts(country);
    if (entries.empty()) {
        std::printf("[-] Could not find your own designs.\n");
        return;
    }

    std::printf("%zu design%s currently on a production line:\n\n",
                entries.size(), entries.size() == 1 ? "" : "s");
    std::printf("  #    cost        variant             cost field\n");
    std::printf("  --------------------------------------------------------------\n");
    for (size_t i = 0; i < entries.size(); ++i)
        std::printf("  %-5zu%-12.2f0x%-16llx  0x%-16llx  %s\n", i,
                    static_cast<double>(entries[i].cost) / 100000.0,
                    static_cast<unsigned long long>(entries[i].variant),
                    static_cast<unsigned long long>(entries[i].variant + 0x358),
                    entries[i].name.empty() ? "?" : entries[i].name.c_str());
    std::printf("\nCosts are shown the way the game does. Enter them the same\n"
                "way - 1 means a cost of 1.0, not 0.00001.\n\n");

    std::printf("  1) Set every cost\n");
    std::printf("  2) Set one cost\n");
    std::printf("Choice: ");
    std::fflush(stdout);

    std::string choice;
    if (!readLine(choice)) return;

    std::string error;

    if (choice == "1") {
        long long value = promptNumber("Set every cost to", 100);
        if (value < 1) { std::printf("Nothing to do.\n"); return; }

        int changed = 0;
        if (!game.setAllDesignCosts(country, value, error, &changed)) {
            std::printf("[-] Failed: %s\n", error.c_str());
            return;
        }
        std::printf("\nSet %d cost%s to %lld.\n", changed,
                    changed == 1 ? "" : "s", static_cast<long long>(value));

    } else if (choice == "2") {
        long long which = promptNumber("Which number", 0);
        if (which < 0 || which >= static_cast<long long>(entries.size())) {
            std::printf("No entry with that number.\n");
            return;
        }

        const auto& entry = entries[static_cast<size_t>(which)];
        const char* label = entry.name.empty() ? "That one" : entry.name.c_str();

        std::printf("%s, cost %.2f.\n", label,
                    static_cast<double>(entry.cost) / 100000.0);

        long long value = promptNumber("Set it to", 100);
        if (value < 1) { std::printf("Nothing to do.\n"); return; }

        if (!game.setDesignCost(entry, value, error)) {
            std::printf("[-] Failed: %s\n", error.c_str());
            return;
        }
        std::printf("\n%s: %.2f -> %lld\n", label,
                    static_cast<double>(entry.cost) / 100000.0,
                    static_cast<long long>(value));

    } else {
        std::printf("Nothing to do.\n");
    }
}

// --------------------------------------------------------------- divisions
//
// None of the division fields hold on their own - the game recomputes every
// one of them, which is why this is a freeze rather than a set of writes.
//
// Divisions are found by searching memory for the pointer each one carries
// back to its country object and keeping the ones that point at the player's
// country. That works in Ironman, where the console is unavailable, in any
// campaign and as any country, and it never touches an enemy division.

// Divisions are found by scanning for the country pointer each one carries,
// so there is nothing for the player to do first - no console, no selecting.
// If the scan still comes back empty, something more basic is wrong.
void explainAnchor() {
    std::printf("\nNo divisions found for your country.\n\n");
    std::printf("Divisions are located by searching memory for the pointer each\n");
    std::printf("one carries back to its country object, so this needs nothing\n");
    std::printf("from you - no console, no selecting a unit. An empty result\n");
    std::printf("usually means one of:\n\n");
    std::printf("  - you are not in a campaign yet, or still on the menu\n");
    std::printf("  - the country has no divisions at all\n");
    std::printf("  - the game updated and the offsets need re-deriving\n");
}

void showDivisions(const std::vector<hoi4::Division>& divisions) {
    std::printf("\n%zu division%s detected automatically:\n\n", divisions.size(),
                divisions.size() == 1 ? "" : "s");
    std::printf("  #     address           org / maxOrg       HP      def     brk    soft    hard   exp (veterancy)\n");
    std::printf("  -------------------------------------------------------------------------------------------------\n");

    for (size_t i = 0; i < divisions.size(); ++i) {
        const auto& d = divisions[i];
        const char* rank = "Green";
        if (d.experience >= 0.90) rank = "Veteran";
        else if (d.experience >= 0.70) rank = "Seasoned";
        else if (d.experience >= 0.25) rank = "Regular";
        else if (d.experience >= 0.08) rank = "Trained";

        double orgPct = d.maxOrganisation > 0 ? (static_cast<double>(d.organisation) * 100.0 / static_cast<double>(d.maxOrganisation)) : 100.0;
        std::printf("  %-5zu 0x%-14llx  %4lld/%-4lld (%3.0f%%)  %7lld %7lld %7lld %7lld %7lld   %5.1f%% (%s)\n",
                    i,
                    static_cast<unsigned long long>(d.address),
                    static_cast<long long>(d.organisation),
                    static_cast<long long>(d.maxOrganisation),
                    orgPct,
                    static_cast<long long>(d.hitPoints),
                    static_cast<long long>(d.defense),
                    static_cast<long long>(d.breakthrough),
                    static_cast<long long>(d.softAttack),
                    static_cast<long long>(d.hardAttack),
                    d.experience * 100.0,
                    rank);
    }
    std::printf("\n");
}

void showFreezeSettings(const hoi4::DivisionGodmode& s) {
    std::printf("  Lock Org to 100%%  %-4s  (Perpetual full organisation - never drops!)\n",
                s.keepOrgFull ? "on" : "off");
    std::printf("  Lock Planning 100%%%-4s  (Perpetual full planning bonus - always at 100%%!)\n",
                s.planning ? "on" : "off");
    std::printf("  Organisation      %-4s  target %lld\n",
                s.organisation ? "on" : "off",
                static_cast<long long>(s.organisationValue));
    std::printf("  Max organisation  %-4s  (CDivisionStats protected from mutations)\n",
                s.maxOrganisation ? "on" : "off");
    std::printf("  Hit points        %-4s  set to %lld (Godmode Invulnerability)\n",
                s.hitPoints ? "on" : "off",
                static_cast<long long>(s.hitPointsValue));
    std::printf("  Combat stats      %-4s  (derived dynamically by engine from template/equipment)\n",
                s.combatStats ? "on" : "off");
    std::printf("  Veterancy (XP)    %-4s  set to %.1f%% (100%% = Veteran)\n",
                s.veterancy ? "on" : "off",
                s.veterancyValue * 100.0);
    std::printf("  Rewrite every     %d ms (safe non-crashing battle interval)\n", s.intervalMs);
}

void doDivisions(hoi4::Game& game, hoi4::DivisionFreeze& freeze) {
    static hoi4::DivisionGodmode settings = [] {
        hoi4::DivisionGodmode s;
        s.keepOrgFull       = true;
        s.planning          = true;  // Perpetually locks planning bonus to 100%
        s.organisation      = true;
        s.maxOrganisation   = false; // Keep false to avoid mutating shared template stats
        s.hitPoints         = true;
        s.combatStats       = false; // Kept false: combat stats are engine-derived; Org+HP provides true godmode
        s.veterancy         = false; // Kept false in loop to avoid battle crash; set on demand via 11
        s.organisationValue = kDefaultOrganisation;
        s.hitPointsValue    = kDefaultHitPoints;
        s.planningValue     = 100;
        s.combatStatValue   = kDefaultCombatStat;
        s.veterancyValue    = 1.0;
        s.intervalMs        = 150; // 150ms ensures organization never drops and zero battle crash
        return s;
    }();

    while (true) {
        line();
        std::printf("Divisions Godmode & Stats\n");
        line();

        auto divisions = game.playerDivisions();

        if (divisions.empty() && !freeze.running()) {
            std::string tagStr = game.playerTagString();
            std::printf("  [!] No divisions currently found for %s.\n",
                        tagStr.empty() ? "your country" : tagStr.c_str());
            std::printf("  (Auto-detected directly from Country + 0x250).\n\n");
        } else if (!divisions.empty()) {
            showDivisions(divisions);
        }

        auto status = freeze.status();
        if (status.running) {
            std::printf("Freeze is RUNNING - %llu pass%s, %d division%s on the last one",
                        static_cast<unsigned long long>(status.passes),
                        status.passes == 1 ? "" : "es",
                        status.lastCount,
                        status.lastCount == 1 ? "" : "s");
            if (status.emptyPasses > 0)
                std::printf(", %llu pass%s found nothing",
                            static_cast<unsigned long long>(status.emptyPasses),
                            status.emptyPasses == 1 ? "" : "es");
            std::printf("\n%llu full scan%s so far, %d division%s known.\n\n",
                        static_cast<unsigned long long>(status.scans),
                        status.scans == 1 ? "" : "s",
                        status.knownCount,
                        status.knownCount == 1 ? "" : "s");
        } else {
            std::printf("Freeze is stopped. (HOI4 recalculates combat stats and org constantly;\n");
            std::printf("the freeze loop continuously enforces full org, HP & 100%% planning every %d ms).\n\n",
                        settings.intervalMs);
        }

        showFreezeSettings(settings);

        std::printf("\n");
        std::printf("  1) %s the Division Freeze / Godmode loop\n", status.running ? "Stop" : "Start");
        std::printf("  2) Refill all divisions Organisation to 100%% once\n");
        std::printf("  3) Apply full Godmode once (instant refill Org, HP, Planning 100%%)\n");
        std::printf("  4) Toggle Lock Org to 100%%     (currently %s - perpetual full org)\n",
                    settings.keepOrgFull ? "ON" : "OFF");
        std::printf("  5) Toggle Lock Planning 100%%   (currently %s - perpetual 100%% planning)\n",
                    settings.planning ? "ON" : "OFF");
        std::printf("  6) Toggle organisation boost   (currently %s)\n",
                    settings.organisation ? "on" : "off");
        std::printf("  7) Toggle hit points           (currently %s)\n",
                    settings.hitPoints ? "on" : "off");
        std::printf("  8) Toggle combat stats         (currently %s)\n",
                    settings.combatStats ? "on" : "off");
        std::printf("  9) Toggle veterancy lock       (currently %s)\n",
                    settings.veterancy ? "on" : "off");
        std::printf(" 10) Change the values & rewrite interval\n");
        std::printf(" 11) Set experience / veterancy for ALL divisions (Green->Veteran)\n");
        std::printf(" 12) Resupply and reinforce all divisions (Stockpile + Manpower)\n");
        std::printf("  r) Refresh division scan\n");
        std::printf("  b) Back\n");
        line();
        std::printf("Choice: ");
        std::fflush(stdout);

        std::string choice;
        if (!readLine(choice)) return;
        if (choice == "b" || choice == "B" || choice.empty()) return;
        if (choice == "r" || choice == "R") continue;

        std::string error;

        if (choice == "1") {
            if (freeze.running()) {
                freeze.stop();
                std::printf("\nStopped. The game will pull everything back to its own\n"
                            "numbers on the next recalculation.\n");
            } else if (!freeze.start(settings, error)) {
                std::printf("\n[-] Could not start: %s\n", error.c_str());
            } else {
                std::printf("\n[+] Division Freeze RUNNING! Rewriting every %d ms across all\n"
                            "player divisions. Org locked to 100%%, Planning locked to 100%%, HP invulnerable!\n",
                            settings.intervalMs);
            }
            waitForEnter();

        } else if (choice == "2") {
            auto c = game.playerCountry();
            if (!c) {
                std::printf("\n[-] Could not resolve player country.\n");
            } else {
                std::string err;
                int total = 0;
                int refilled = game.refillAllDivisionsOrganisation(*c, err, &total);
                std::printf("\n[+] Refilled Organisation to 100%% on %d of %d division%s!\n",
                            refilled, total, total == 1 ? "" : "s");
            }
            waitForEnter();

        } else if (choice == "3") {
            int found = 0;
            const int written = game.applyGodmodeToPlayer(settings, &found);
            std::printf("\nWrote %d of %d division%s (Org, HP, and Planning set to 100%%).\n",
                        written, found, found == 1 ? "" : "s");
            waitForEnter();

        } else if (choice == "4") {
            settings.keepOrgFull = !settings.keepOrgFull;
            freeze.setSettings(settings);

        } else if (choice == "5") {
            settings.planning = !settings.planning;
            freeze.setSettings(settings);

        } else if (choice == "6") {
            settings.organisation = !settings.organisation;
            freeze.setSettings(settings);

        } else if (choice == "7") {
            settings.hitPoints = !settings.hitPoints;
            freeze.setSettings(settings);

        } else if (choice == "8") {
            std::printf("\n[i] In Hearts of Iron IV, combat stats (defense, breakthrough, soft/hard attack)\n"
                        "    are dynamically derived by the engine each tick from division template battalions\n"
                        "    and equipment. Direct memory writes to those fields corrupt internal engine pointers.\n"
                        "    Division Godmode is achieved via 100%% Organisation lock (never lose or retreat),\n"
                        "    100%% Hit Points lock (never take casualties or lose equipment),\n"
                        "    and 100%% Planning bonus lock!\n");
            waitForEnter();

        } else if (choice == "9") {
            settings.veterancy = !settings.veterancy;
            freeze.setSettings(settings);

        } else if (choice == "10") {
            std::printf("\nValues are entered the way the game shows them - 500 means\n"
                        "an organisation of 500, not 0.005.\n\n");

            settings.organisationValue =
                promptNumber("Organisation and its maximum", settings.organisationValue);
            settings.hitPointsValue =
                promptNumber("Hit points (Godmode/Invulnerability)", settings.hitPointsValue);
            settings.planningValue =
                promptNumber("Planning bonus percentage (100 = 100%)", settings.planningValue);
            settings.combatStatValue =
                promptNumber("Defense, breakthrough, soft and hard attack",
                             settings.combatStatValue);

            long long interval = promptNumber("Rewrite interval in ms (default 150)",
                                              settings.intervalMs);
            if (interval < 10)   interval = 10;
            if (interval > 5000) interval = 5000;
            settings.intervalMs = static_cast<int>(interval);

            freeze.setSettings(settings);
            std::printf("\nUpdated. A running freeze picks these up on its next pass.\n");
            waitForEnter();

        } else if (choice == "11") {
            auto c = game.playerCountry();
            if (!c) {
                std::printf("\n[-] Could not resolve player country.\n");
            } else {
                std::printf("\nSelect veterancy rank to apply to all divisions:\n");
                std::printf("  1) Veteran (100%% XP, +75%% combat bonus)\n");
                std::printf("  2) Seasoned (75%% XP, +50%% combat bonus)\n");
                std::printf("  3) Regular (30%% XP, +25%% combat bonus)\n");
                std::printf("  4) Trained (10%% XP, 0%% bonus)\n");
                std::printf("  5) Custom percentage (0 - 100%%)\n");
                std::printf("Choice: ");
                std::fflush(stdout);

                std::string expChoice;
                if (readLine(expChoice)) {
                    double expFrac = 1.0;
                    if (expChoice == "1") expFrac = 1.0;
                    else if (expChoice == "2") expFrac = 0.75;
                    else if (expChoice == "3") expFrac = 0.30;
                    else if (expChoice == "4") expFrac = 0.10;
                    else if (expChoice == "5") {
                        long long pct = promptNumber("Veterancy percentage (0 to 100)", 100);
                        if (pct < 0) pct = 0;
                        if (pct > 100) pct = 100;
                        expFrac = static_cast<double>(pct) / 100.0;
                    }
                    std::string err;
                    int count = 0;
                    int changed = game.setAllDivisionsExperience(*c, expFrac, err, &count);
                    std::printf("\n[+] Set %.1f%% veterancy (XP) on %d of %d divisions!\n",
                                expFrac * 100.0, changed, count);
                }
            }
            waitForEnter();

        } else if (choice == "12") {
            auto c = game.playerCountry();
            if (!c) {
                std::printf("\n[-] Could not resolve player country.\n");
            } else {
                std::string err;
                if (game.resupplyDivisions(*c, err)) {
                    std::printf("\n[+] Resupplied: filled national stockpile with 100k of all equipment variants,\n"
                                "    synchronized logistics, and added +1,000,000 manpower.\n"
                                "    Divisions will reinforce to 100%% strength immediately!\n");
                } else {
                    std::printf("\n[-] Resupply failed: %s\n", err.c_str());
                }
            }
            waitForEnter();
        }
    }
}

// ----------------------------------------------------------- country tag / troll

void doTagSwitch(hoi4::Game& game, hoi4::DivisionFreeze& freeze) {
    bool showAll = false;

    while (true) {
        line();
        std::printf("Switch country tag (Troll / Control another country)\n");
        line();

        auto curTagOpt = game.playerTag();
        int32_t curTag = curTagOpt ? *curTagOpt : -1;
        std::string curTagStr = (curTag > 0) ? game.countryTag(curTag) : "?";
        std::string curNameStr = (curTag > 0) ? game.countryName(curTag) : "";

        std::string origTagStr = (g_originalTag > 0) ? game.countryTag(g_originalTag) : "?";
        std::string origNameStr = (g_originalTag > 0) ? game.countryName(g_originalTag) : "";

        std::printf("Current country  : Tag %d [%s%s%s]%s\n",
                    curTag,
                    curTagStr.c_str(),
                    (curNameStr.empty() || curNameStr == curTagStr) ? "" : " - ",
                    (curNameStr.empty() || curNameStr == curTagStr) ? "" : curNameStr.c_str(),
                    (g_originalTag > 0 && curTag != g_originalTag) ? "  *** [TROLLING / ALT TAG ACTIVE] ***" : "");

        std::printf("Original country : Tag %d [%s%s%s]\n\n",
                    g_originalTag,
                    origTagStr.c_str(),
                    (origNameStr.empty() || origNameStr == origTagStr) ? "" : " - ",
                    (origNameStr.empty() || origNameStr == origTagStr) ? "" : origNameStr.c_str());

        // Fetch all countries
        auto countries = game.allCountries();
        std::vector<hoi4::CountryInfo> active;
        std::vector<hoi4::CountryInfo> inactive;
        for (const auto& c : countries) {
            if (c.stateCount > 0) active.push_back(c);
            else inactive.push_back(c);
        }

        const auto& toDisplay = showAll ? countries : active;

        std::printf("Countries (%zu %s):\n",
                    toDisplay.size(), showAll ? "total in game" : "active on map");
        std::printf("  %-40s | %-40s\n",
                    "Tag   Code   States Name", "Tag   Code   States Name");
        std::printf("  -----------------------------------------+-----------------------------------------\n");

        for (size_t i = 0; i < toDisplay.size(); i += 2) {
            char col1[64] = "";
            char col2[64] = "";

            const auto& c1 = toDisplay[i];
            char marker1 = (c1.tag == curTag) ? '*' : (c1.tag == g_originalTag ? 'o' : ' ');
            std::string name1 = (c1.nameString != c1.tagString) ? c1.nameString : "";
            std::snprintf(col1, sizeof(col1), "#%-3d  %-4s %c (%3d st) %-17.17s",
                          c1.tag, c1.tagString.c_str(), marker1, c1.stateCount, name1.c_str());

            if (i + 1 < toDisplay.size()) {
                const auto& c2 = toDisplay[i + 1];
                char marker2 = (c2.tag == curTag) ? '*' : (c2.tag == g_originalTag ? 'o' : ' ');
                std::string name2 = (c2.nameString != c2.tagString) ? c2.nameString : "";
                std::snprintf(col2, sizeof(col2), "#%-3d  %-4s %c (%3d st) %-17.17s",
                              c2.tag, c2.tagString.c_str(), marker2, c2.stateCount, name2.c_str());
            }

            std::printf("  %-40s | %-40s\n", col1, col2);
        }

        if (!showAll && !inactive.empty()) {
            std::printf("\n  (%zu countries with 0 states hidden. Type 'all' to show all)\n",
                        inactive.size());
        }

        std::printf("\nLegend: '*' = currently controlled, 'o' = original country\n\n");
        std::printf("Commands:\n");
        std::printf("  - Enter Tag ID (e.g. 1) or 3-letter code (e.g. GER, HUN, SOV, ROM)\n");
        if (g_originalTag > 0 && curTag != g_originalTag) {
            std::printf("  - 'o' or 'r' : Restore / switch back to original country (#%d %s)\n",
                        g_originalTag, origTagStr.c_str());
        }
        if (g_lastTrollTag > 0 && g_lastTrollTag != curTag && g_lastTrollTag != g_originalTag) {
            std::string lastTrollStr = game.countryTag(g_lastTrollTag);
            std::printf("  - 't'        : Quick toggle back to troll country (#%d %s)\n",
                        g_lastTrollTag, lastTrollStr.c_str());
        } else if (g_originalTag > 0 && curTag != g_originalTag) {
            std::printf("  - 't'        : Quick toggle back to original country (#%d %s)\n",
                        g_originalTag, origTagStr.c_str());
        }
        if (!showAll) std::printf("  - 'all'      : Show all countries including 0-state ones\n");
        else          std::printf("  - 'active'   : Show only active countries with states > 0\n");
        std::printf("  - 'b'        : Back to main menu\n");
        line();

        std::printf("Choice: ");
        std::fflush(stdout);

        std::string input;
        if (!readLine(input) || input.empty() || input == "b" || input == "B") return;

        if (input == "all" || input == "ALL") {
            showAll = true;
            continue;
        }
        if (input == "active" || input == "ACTIVE") {
            showAll = false;
            continue;
        }

        int32_t targetTag = -1;

        if ((input == "o" || input == "O" || input == "r" || input == "R") && g_originalTag > 0) {
            targetTag = g_originalTag;
        } else if (input == "t" || input == "T") {
            if (curTag != g_originalTag) {
                targetTag = g_originalTag;
            } else if (g_lastTrollTag > 0 && g_lastTrollTag != curTag) {
                targetTag = g_lastTrollTag;
            } else {
                std::printf("No alternate country to toggle with.\n");
                waitForEnter();
                continue;
            }
        } else {
            auto found = game.findTag(input);
            if (!found) {
                std::printf("[-] Unknown country tag or code: '%s'. Try e.g. 'GER', 'SOV', or a number.\n",
                            input.c_str());
                waitForEnter();
                continue;
            }
            targetTag = *found;
        }

        if (targetTag == curTag) {
            std::printf("You are already controlling tag %d.\n", targetTag);
            waitForEnter();
            continue;
        }

        // Check if target country has 0 states
        auto targetAddr = game.country(targetTag);
        if (targetAddr) {
            auto states = game.states(*targetAddr);
            if (states.empty()) {
                std::printf("\n[!] Warning: That country has 0 states (annexed or unreleased).\n"
                            "    Switching to it may trigger Game Over in-game!\n");
                if (!confirm("    Switch anyway?")) {
                    std::printf("Cancelled.\n");
                    waitForEnter();
                    continue;
                }
            }
        }

        std::string err;
        if (!game.setPlayerTag(targetTag, err)) {
            std::printf("\n[-] Failed to switch player tag: %s\n", err.c_str());
            waitForEnter();
            continue;
        }

        // Remember troll tag if switching away from original
        if (curTag == g_originalTag && targetTag != g_originalTag) {
            g_lastTrollTag = targetTag;
        }

        // Notify division freeze thread if running
        if (freeze.running()) {
            freeze.notifyCountryChanged();
        }

        std::string newTagStr = game.countryTag(targetTag);
        std::string newNameStr = game.countryName(targetTag);

        std::printf("\n[+] SUCCESS! Player tag changed: %d (%s) -> %d (%s%s%s)\n",
                    curTag, curTagStr.c_str(),
                    targetTag, newTagStr.c_str(),
                    (newNameStr.empty() || newNameStr == newTagStr) ? "" : " - ",
                    (newNameStr.empty() || newNameStr == newTagStr) ? "" : newNameStr.c_str());

        if (targetTag == g_originalTag) {
            std::printf("    [i] You are back controlling your ORIGINAL country!\n");
        } else {
            std::printf("    [i] You are now in control of this country!\n"
                        "        All trainer options will now apply to it.\n"
                        "        To return to your original country (%d %s), select 'o' in this menu.\n",
                        g_originalTag, origTagStr.c_str());
        }

        waitForEnter();
    }
}

void doIronmanConsole(hoi4::Game& game) {
    line();
    std::printf("Developer console in Ironman & Multiplayer (keeps achievements)\n");
    line();

    bool isIron = game.isIronman();
    bool isActive = game.isConsolePatchActive();
    bool isMpKick = game.isMultiplayerKickUnlockActive();

    std::printf("Ironman save status     : %s\n",
                isIron ? "YES (GameState + 0xA8 bit 0 is active - achievements eligible)"
                       : "NO (Standard game session)");
    std::printf("Console patch status    : %s\n",
                isActive ? "ENABLED (Console keybind, developer & multiplayer commands unlocked)"
                         : "DISABLED (Vanilla behavior - console locked in Ironman & Multiplayer)");
    std::printf("Multiplayer kick status : %s\n\n",
                isMpKick ? "ENABLED (Chat /kick <name> & Kick/Ban GUI buttons unlocked for all players)"
                         : "DISABLED (Host/operator only)");

    std::printf("How this works:\n"
                "  - Vanilla HOI4 blocks the console in Ironman & Multiplayer via\n"
                "    CConsoleCmdManager::IsConsoleAvailable and CConsoleCmdManager::Execute\n"
                "    ('Console not available in multiplayer or ironman mode.').\n"
                "  - This trainer bypasses both checks in memory via runtime code patches.\n"
                "  - CRITICALLY: GameState + 0xA8 (the Ironman mode flag) is NEVER modified!\n"
                "    The save file remains 100%% Ironman, and Steam achievements remain active & earnable!\n"
                "  - MULTIPLAYER NOTE: Informational and debug commands (fow, observe, togglegui,\n"
                "    reload, etc.) work without issue. Commands modifying simulation state (manpower,\n"
                "    annex, add_equipment) will trigger Out-of-Sync (OOS) in peer-to-peer sessions\n"
                "    because HOI4 uses deterministic lockstep networking.\n\n");

    std::printf("  1) %s developer console in Ironman & Multiplayer\n", isActive ? "Disable" : "Enable");
    std::printf("  2) Force Open / Toggle Console Window directly in HOI4\n");
    std::printf("  3) %s Multiplayer Kick Unlock (allows /kick <name> in chat & GUI buttons)\n", isMpKick ? "Disable" : "Enable");
    std::printf("  b) Return to main menu\n");
    line();
    std::printf("Choice: ");
    std::fflush(stdout);

    std::string choice;
    if (!readLine(choice)) return;

    if (choice == "1") {
        std::string err;
        bool target = !isActive;
        if (!game.setConsoleInIronman(target, err)) {
            std::printf("\n[-] Failed to %s console: %s\n",
                        target ? "enable" : "disable", err.c_str());
            waitForEnter();
            return;
        }

        if (target) {
            std::printf("\n[+] SUCCESS! Developer console is now ENABLED in Ironman & Multiplayer!\n\n"
                        "    Controls on macOS:\n"
                        "      - Key below ESC: '`' (US layout) or '§' (Romanian/UK layout) or '^'\n"
                        "      - If your physical key is not recognized by macOS, use Option 2 to force-open it!\n"
                        "      - Tip: You can also add 'U.S.' layout in macOS System Settings -> Keyboard -> Input Sources.\n"
                        "      - Any command can now be run: manpower, pp, instantconstruction, research all, etc.\n"
                        "      - Ironman status & Steam achievements remain 100%% ACTIVE & UNTOUCHED!\n");
        } else {
            std::printf("\n[+] Restored vanilla console checks. Console is once again restricted.\n");
        }
        waitForEnter();
    } else if (choice == "2") {
        std::string err;
        if (game.toggleConsoleWindow(err)) {
            std::printf("\n[+] Sent toggle signal to CConsole! Check your HOI4 window.\n");
        } else {
            std::printf("\n[-] Could not toggle console: %s\n", err.c_str());
        }
        waitForEnter();
    } else if (choice == "3") {
        std::string err;
        bool target = !isMpKick;
        if (!game.setMultiplayerKickUnlock(target, err)) {
            std::printf("\n[-] Failed to update multiplayer kick unlock: %s\n", err.c_str());
            std::printf("\n[+] SUCCESS! Multiplayer Kick Unlock is now %s!\n"
                        "    - You can now type `/kick <player_name>` in chat to kick any player.\n"
                        "    - Kick and Ban UI buttons are enabled for all clients in the multiplayer player list.\n"
                        "    - In the console (` ~ key), you can also type `kick <player_name>` or `kick_id <id>` directly!\n",
                        target ? "ENABLED" : "DISABLED");
        }
        waitForEnter();
    }
}

void showLeaders(const std::vector<hoi4::Leader>& leaders) {
    if (leaders.empty()) {
        std::printf("  [!] No military leaders found in leader manager.\n");
        return;
    }
    std::printf("\nFound %zu Military Leader%s:\n\n", leaders.size(), leaders.size() == 1 ? "" : "s");
    std::printf("  #    Type             Level  Atk  Def  Pln  Log  Sk5    Experience   Status       Name\n");
    std::printf("  ------------------------------------------------------------------------------------------------------\n");
    for (size_t i = 0; i < leaders.size(); ++i) {
        const auto& l = leaders[i];
        const char* typeStr = "General";
        if (l.type == hoi4::LeaderType::FieldMarshal) typeStr = "Field Marshal";
        else if (l.type == hoi4::LeaderType::Admiral) typeStr = "Admiral";

        const char* status = l.isCorrupted ? "[CORRUPTED]" : "OK";

        std::printf("  %-4zu %-16s Lvl %-2d  %-4d %-4d %-4d %-4d %-4d   %10lld   %-12s %s\n",
                    i + 1, typeStr, l.skillLevel,
                    l.attackSkill, l.defenseSkill, l.planningSkill, l.logisticsSkill, l.skill5,
                    static_cast<long long>(l.experience),
                    status, l.name.c_str());
    }
    std::printf("\n");
}

void doLeaders(hoi4::Game& game, uint64_t country) {
    while (true) {
        line();
        std::printf("Military Leaders (Generals, Field Marshals & Admirals XP / Skills)\n");
        line();

        auto leaders = game.leaders(country);
        showLeaders(leaders);

        int corruptedCount = 0;
        for (const auto& l : leaders) {
            if (l.isCorrupted) corruptedCount++;
        }
        if (corruptedCount > 0) {
            std::printf("  [!] WARNING: %d leader(s) have a corrupted role (e.g. role set to 9)!\n", corruptedCount);
            std::printf("      This locks out medals, promotions, and army group assignments.\n");
            std::printf("      Press 5 to REPAIR CORRUPTED LEADERS back to clean state!\n\n");
        }

        std::printf("  1) Select a specific leader by # (XP, Skill, All 5 Sub-skills, Role)\n");
        std::printf("  2) Add XP to ALL leaders\n");
        std::printf("  3) Set All 5 Sub-skills for ALL leaders (Attack, Defense, Planning, Logistics, Skill 5)\n");
        std::printf("  4) MAX ALL LEADERS (Level 9, All 5 Subskills 10, 500k XP - roles preserved!)\n");
        std::printf("  5) REPAIR CORRUPTED LEADERS (only fixes invalid roles outside 0..2)\n");
        std::printf("  r) Refresh\n");
        std::printf("  b) Back\n");
        line();
        std::printf("Choice: ");
        std::fflush(stdout);

        std::string choice;
        if (!readLine(choice)) return;
        if (choice == "b" || choice == "B" || choice.empty()) return;
        if (choice == "r" || choice == "R") continue;

        if (choice == "1") {
            if (leaders.empty()) {
                std::printf("No leaders available to modify.\n");
                waitForEnter();
                continue;
            }
            long long idx = promptNumber("Leader number (#)", 1);
            if (idx < 1 || idx > static_cast<long long>(leaders.size())) {
                std::printf("Invalid leader number.\n");
                waitForEnter();
                continue;
            }
            const auto& selected = leaders[static_cast<size_t>(idx - 1)];
            std::printf("\nSelected: %s (Type: %s, Skill Level: %d, Atk: %d, Def: %d, Pln: %d, Log: %d, Sk5: %d, XP: %lld)\n",
                        selected.name.c_str(),
                        selected.type == hoi4::LeaderType::FieldMarshal ? "Field Marshal" :
                        selected.type == hoi4::LeaderType::Admiral ? "Admiral" : "General",
                        selected.skillLevel,
                        selected.attackSkill, selected.defenseSkill, selected.planningSkill, selected.logisticsSkill, selected.skill5,
                        static_cast<long long>(selected.experience));
            std::printf("  1) Add XP\n");
            std::printf("  2) Set Skill Level directly (1-9)\n");
            std::printf("  3) Set Sub-skills (Attack, Defense, Planning/Maneuvering, Logistics/Coordination, Skill 5)\n");
            std::printf("  4) Max this leader (Level 9, All 5 Subskills 10, 500k XP - role preserved)\n");
            std::printf("  5) Change Role (General / Field Marshal / Admiral)\n");
            std::printf("Choice: ");
            std::fflush(stdout);

            std::string subChoice;
            if (!readLine(subChoice)) continue;

            std::string err;
            if (subChoice == "1") {
                long long add = promptNumber("XP to add", 5000);
                if (game.addLeaderExperience(selected.address, add, err)) {
                    std::printf("\n[+] Added %lld XP to %s!\n", add, selected.name.c_str());
                } else {
                    std::printf("\n[-] Failed: %s\n", err.c_str());
                }
            } else if (subChoice == "2") {
                long long lvl = promptNumber("Skill level (1-9)", 9);
                long long xp = promptNumber("XP to set", 100000);
                if (game.setLeaderSkill(selected.address, static_cast<int32_t>(lvl), xp, err)) {
                    std::printf("\n[+] Set %s to Level %lld (XP %lld)!\n", selected.name.c_str(), lvl, xp);
                } else {
                    std::printf("\n[-] Failed: %s\n", err.c_str());
                }
            } else if (subChoice == "3") {
                long long atk = promptNumber("Attack level (0-10)", selected.attackSkill > 0 ? selected.attackSkill : 10);
                long long def = promptNumber("Defense level (0-10)", selected.defenseSkill > 0 ? selected.defenseSkill : 10);
                long long pln = promptNumber("Planning/Maneuvering level (0-10)", selected.planningSkill > 0 ? selected.planningSkill : 10);
                long long log = promptNumber("Logistics/Coordination level (0-10)", selected.logisticsSkill > 0 ? selected.logisticsSkill : 10);
                long long sk5 = promptNumber("Skill 5 / Extra level (0-10)", selected.skill5 > 0 ? selected.skill5 : 10);
                if (game.setLeaderSubSkills(selected.address, static_cast<int32_t>(atk), static_cast<int32_t>(def), static_cast<int32_t>(pln), static_cast<int32_t>(log), static_cast<int32_t>(sk5), err)) {
                    std::printf("\n[+] Updated sub-skills for %s: Attack %lld, Defense %lld, Planning %lld, Logistics %lld, Skill5 %lld!\n",
                                selected.name.c_str(), atk, def, pln, log, sk5);
                } else {
                    std::printf("\n[-] Failed: %s\n", err.c_str());
                }
            } else if (subChoice == "4") {
                game.setLeaderSkill(selected.address, 9, 500000, err);
                game.setLeaderSubSkills(selected.address, 10, 10, 10, 10, 10, err);
                std::printf("\n[+] %s is now MAX LEVEL 9 (Attack 10, Def 10, Pln 10, Log 10, Skill5 10, 500k XP)!\n", selected.name.c_str());
            } else if (subChoice == "5") {
                std::printf("\nSelect role for %s:\n", selected.name.c_str());
                std::printf("  1) General (Corps Commander - role 0)\n");
                std::printf("  2) Field Marshal (Army Group Commander - role 1)\n");
                std::printf("  3) Navy Admiral (role 2)\n");
                std::printf("Choice: ");
                std::fflush(stdout);
                std::string roleChoice;
                if (readLine(roleChoice)) {
                    int32_t newRole = 0;
                    if (roleChoice == "1") newRole = 0;
                    else if (roleChoice == "2") newRole = 1;
                    else if (roleChoice == "3") newRole = 2;
                    else { std::printf("Invalid role.\n"); waitForEnter(); continue; }

                    if (game.setLeaderRole(selected.address, newRole, err)) {
                        std::printf("\n[+] Changed role of %s to %s!\n",
                                    selected.name.c_str(),
                                    newRole == 1 ? "Field Marshal" : (newRole == 2 ? "Admiral" : "General"));
                    } else {
                        std::printf("\n[-] Failed: %s\n", err.c_str());
                    }
                }
            }
            waitForEnter();

        } else if (choice == "2") {
            long long add = promptNumber("XP to add to all leaders", 10000);
            int touched = 0;
            for (const auto& l : leaders) {
                std::string err;
                if (game.addLeaderExperience(l.address, add, err)) touched++;
            }
            std::printf("\n[+] Added %lld XP to %d leaders!\n", add, touched);
            waitForEnter();

        } else if (choice == "3") {
            long long atk = promptNumber("Attack level for ALL leaders (0-10)", 10);
            long long def = promptNumber("Defense level for ALL leaders (0-10)", 10);
            long long pln = promptNumber("Planning/Maneuvering level for ALL leaders (0-10)", 10);
            long long log = promptNumber("Logistics/Coordination level for ALL leaders (0-10)", 10);
            long long sk5 = promptNumber("Skill 5 / Extra level for ALL leaders (0-10)", 10);
            std::string err;
            int changed = game.setAllLeadersSubSkills(country, static_cast<int32_t>(atk), static_cast<int32_t>(def), static_cast<int32_t>(pln), static_cast<int32_t>(log), static_cast<int32_t>(sk5), err);
            std::printf("\n[+] Updated all 5 sub-skills on %d leaders (Attack %lld, Defense %lld, Planning %lld, Logistics %lld, Skill5 %lld)!\n",
                        changed, atk, def, pln, log, sk5);
            waitForEnter();

        } else if (choice == "4") {
            int total = 0;
            std::string err;
            int touched = game.maxAllLeaders(country, err, &total);
            std::printf("\n[+] Maxed %d of %d leaders to Skill Level 9, Subskills 10 (all 5 skills) & 500k XP!\n"
                        "    All leader roles were PRESERVED untouched (Generals remain Generals, Admirals remain Admirals).\n",
                        touched, total);
            waitForEnter();

        } else if (choice == "5") {
            std::string err;
            int repaired = game.repairLeaders(country, err);
            std::printf("\n[+] REPAIR COMPLETE: Fixed %d corrupted leader(s)!\n", repaired);
            std::printf("    Medals, assignments, and Field Marshal promotions are now UNLOCKED!\n");
            waitForEnter();
        }
    }
}

void doAgency(hoi4::Game& game) {
    while (true) {
        line();
        std::printf("Intelligence Agency & Operations (La Résistance Godmode)\n");
        line();

        auto instOp     = game.instantOperation();
        auto instNet    = game.instantIntelNetwork();
        auto instSlot   = game.instantAgencySlotUnlock();
        auto instUpg    = game.instantAgencyUpgrade();
        auto allowOp    = game.allowOperations();
        auto preventDet = game.preventOperativeDetection();

        auto fmt = [](std::optional<bool> v) {
            return !v ? "unknown" : (*v ? "ON" : "off");
        };

        std::printf("Current Intelligence Agency Status:\n");
        std::printf("  - Instant Operations Completion   : %s\n", fmt(instOp));
        std::printf("  - Instant 100%% Intel Network       : %s\n", fmt(instNet));
        std::printf("  - Instant Operative Slot Unlock   : %s\n", fmt(instSlot));
        std::printf("  - Instant Agency Upgrades & Build : %s\n", fmt(instUpg));
        std::printf("  - Allow All Operations (bypass)   : %s\n", fmt(allowOp));
        std::printf("  - Operative Immunity (no capture) : %s\n\n", fmt(preventDet));

        std::printf("  How Agency Godmode works:\n");
        std::printf("  - Creating an Agency: Takes 0 days with upgrades enabled!\n");
        std::printf("  - Departments: Build in 0 days with 0 civilian factories required.\n");
        std::printf("  - Operations: Complete instantly (0 days) upon launch.\n");
        std::printf("  - Spies: 100%% network strength in seconds, immune to capture/death.\n\n");

        std::printf("  1) TOGGLE ALL AGENCY GODMODE (Master Switch - activates everything)\n");
        std::printf("  2) Toggle Instant Operations Completion (operations complete instantly)\n");
        std::printf("  3) Toggle Instant 100%% Intel Network (network strength maxes instantly)\n");
        std::printf("  4) Toggle Instant Operative Slot Unlock (unlock all operative slots)\n");
        std::printf("  5) Toggle Instant Agency Upgrades & Dept Construction (0 days / 0 civ factories)\n");
        std::printf("  6) Toggle Allow All Operations (start any operation without prerequisites)\n");
        std::printf("  7) Toggle Operative Immunity (operatives cannot be detected, captured, or killed)\n");
        std::printf("  r) Refresh\n");
        std::printf("  b) Back\n");
        line();
        std::printf("Choice: ");
        std::fflush(stdout);

        std::string choice;
        if (!readLine(choice)) return;
        if (choice == "b" || choice == "B" || choice.empty()) return;
        if (choice == "r" || choice == "R") continue;

        std::string err;
        if (choice == "1") {
            bool target = !(instOp && *instOp && instUpg && *instUpg);
            if (game.setAllAgencyGodmode(target, err)) {
                std::printf("\n[+] Set All Agency Godmode to %s!\n", target ? "ON" : "OFF");
            } else {
                std::printf("\n[-] Failed: %s\n", err.c_str());
            }
            waitForEnter();
        } else if (choice == "2") {
            bool target = !(instOp && *instOp);
            game.setInstantOperation(target, err);
        } else if (choice == "3") {
            bool target = !(instNet && *instNet);
            game.setInstantIntelNetwork(target, err);
        } else if (choice == "4") {
            bool target = !(instSlot && *instSlot);
            game.setInstantAgencySlotUnlock(target, err);
        } else if (choice == "5") {
            bool target = !(instUpg && *instUpg);
            game.setInstantAgencyUpgrade(target, err);
        } else if (choice == "6") {
            bool target = !(allowOp && *allowOp);
            game.setAllowOperations(target, err);
        } else if (choice == "7") {
            bool target = !(preventDet && *preventDet);
            game.setPreventOperativeDetection(target, err);
        }
    }
}

void doDoctrines(hoi4::Game& game, uint64_t country) {
    while (true) {
        line();
        std::printf("Doctrines & Subdoctrines Instant Research / Maxing\n");
        line();

        auto roic     = game.researchOnIconClick();
        auto fastTech = game.researchFast();

        auto fmt = [](std::optional<bool> v) {
            return !v ? "unknown" : (*v ? "ON" : "off");
        };

        std::printf("Current Status:\n");
        std::printf("  - Instant Research on Icon Click (roic): %s\n", fmt(roic));
        std::printf("  - Fast Research (1 RP base tech cost)  : %s\n", fmt(fastTech));

        auto subTracks = game.activeSubdoctrines();
        std::printf("  - Active Subdoctrine Tracks in Memory  : %zu found\n", subTracks.size());
        for (size_t i = 0; i < subTracks.size(); ++i) {
            std::printf("      Track #%zu: Milestones Tier %d | Current Mastery: %lld | Banked: %lld\n",
                        i + 1, subTracks[i].milestones,
                        static_cast<long long>(subTracks[i].currentMastery),
                        static_cast<long long>(subTracks[i].bankedMastery));
        }
        std::printf("\n");

        std::printf("  How Doctrines & Subdoctrines work:\n");
        std::printf("  - Land doctrines (Grand Battleplan, Mobile Warfare, Superior Firepower, Mass Assault)\n");
        std::printf("    require Army XP, branching subdoctrine tracks, and Mastery points.\n");
        std::printf("  - With 'Research on Icon Click' (roic) turned ON, clicking ANY doctrine or subdoctrine\n");
        std::printf("    icon in the research screen INSTANTLY completes it with 0 days wait!\n");
        std::printf("  - Subdoctrine Mastery can also be instantly maxed via direct memory or native console!\n\n");

        std::printf("  1) MAX ALL MILITARY XP (Set 500 Army, 500 Navy, 500 Air XP)\n");
        std::printf("  2) Toggle Instant Research on Icon Click (roic - 1-click instant unlock for any doctrine)\n");
        std::printf("  3) Toggle Fast Research (1 RP cost)\n");
        std::printf("  4) ACTIVATE INSTANT DOCTRINE UNLOCK GODMODE (500 XP all + roic enabled)\n");
        std::printf("  5) MAX SUBDOCTRINE MASTERY (Grant 50,000 Points via console `mastery 50000`)\n");
        std::printf("  6) ADD +5,000 SUBDOCTRINE MASTERY POINTS (Grant 5,000 Points via console `mastery 5000`)\n");
        std::printf("  r) Refresh\n");
        std::printf("  b) Back\n");
        line();
        std::printf("Choice: ");
        std::fflush(stdout);

        std::string choice;
        if (!readLine(choice)) return;
        if (choice == "b" || choice == "B" || choice.empty()) return;
        if (choice == "r" || choice == "R") continue;

        std::string err;
        if (choice == "1") {
            if (game.setAllExperience(country, 500, err)) {
                std::printf("\n[+] Refilled Army, Navy, and Air XP to MAX (500 XP each)!\n");
            } else {
                std::printf("\n[-] Failed: %s\n", err.c_str());
            }
            waitForEnter();
        } else if (choice == "2") {
            bool target = !(roic && *roic);
            if (game.setResearchOnIconClick(target, err)) {
                std::printf("\n[+] Research on Icon Click is now %s!\n", target ? "ENABLED (Click any doctrine to unlock!)" : "disabled");
            } else {
                std::printf("\n[-] Failed: %s\n", err.c_str());
            }
            waitForEnter();
        } else if (choice == "3") {
            bool target = !(fastTech && *fastTech);
            if (game.setResearchFast(target, err)) {
                std::printf("\n[+] Fast Research is now %s!\n", target ? "ENABLED" : "disabled");
            } else {
                std::printf("\n[-] Failed: %s\n", err.c_str());
            }
            waitForEnter();
        } else if (choice == "4") {
            game.setAllExperience(country, 500, err);
            game.setResearchOnIconClick(true, err);
            game.setResearchFast(true, err);
            std::printf("\n[+] DOCTRINE GODMODE ACTIVATED!\n"
                        "    - Army, Navy, and Air XP set to 500 (max cap).\n"
                        "    - 'Research on Icon Click' (roic) is ENABLED.\n"
                        "    - Open your Officer Corp / Research tree in HOI4 and click your chosen\n"
                        "      doctrine & subdoctrine nodes (e.g. Grand Battleplan path) -> they unlock INSTANTLY!\n");
            waitForEnter();
        } else if (choice == "5") {
            if (game.setSubdoctrineMastery(50000, 5, err)) {
                std::printf("\n[+] CONSOLE UNLOCKED & SUBDOCTRINE MASTERY READY!\n"
                            "    - Console is fully unlocked in Ironman & Multiplayer.\n"
                            "    - Open console in HOI4 (` ~ key) and type:\n"
                            "         mastery 50000\n"
                            "      (This grants 50,000 mastery points natively via HOI4 engine with ZERO crash!)\n");
            } else {
                std::printf("\n[-] Note: %s\n", err.c_str());
            }
            waitForEnter();
        } else if (choice == "6") {
            if (game.setSubdoctrineMastery(5000, 5, err)) {
                std::printf("\n[+] CONSOLE UNLOCKED & SUBDOCTRINE MASTERY READY!\n"
                            "    - Console is fully unlocked in Ironman & Multiplayer.\n"
                            "    - Open console in HOI4 (` ~ key) and type:\n"
                            "         mastery 5000\n"
                            "      (This grants 5,000 mastery points natively via HOI4 engine with ZERO crash!)\n");
            } else {
                std::printf("\n[-] Note: %s\n", err.c_str());
            }
            waitForEnter();
        }
    }
}

void doToggleScanner(hoi4::Game& game) {
    line();
    std::printf("Toggle offset scanner / probe\n");
    line();
    std::printf("Scans bytes around the known toggle offsets to help find\n");
    std::printf("focus.autocomplete, instanttraining, and allow_trait_assign.\n\n");
    std::printf("These console commands all follow the same pattern:\n");
    std::printf("  data_XXXXXXX ^= 1  (a single byte at a static address)\n\n");
    std::printf("The known toggles are at:\n");
    std::printf("  0x34EDFBE  research_on_icon_click\n");
    std::printf("  0x34EDFD0  instantconstruction\n");
    std::printf("  0x34EDFD1  instantshiprefit\n");
    std::printf("  0x34EE029  sp_instant (special projects)\n\n");

    std::printf("Scanning nearby addresses for bytes that are 0 or 1...\n\n");

    // Scan a window around the known toggles
    constexpr int64_t kToggleBase = 0x34EDFA0;  // start before first known toggle
    constexpr int64_t kToggleEnd  = 0x34EE080;  // end after last known toggle

    struct ToggleProbe {
        int64_t offset;
        uint8_t value;
        const char* known;  // non-null if this is a known toggle
    };

    std::vector<ToggleProbe> probes;
    for (int64_t off = kToggleBase; off < kToggleEnd; ++off) {
        auto val = game.process().read<uint8_t>(game.process().imageBase() + off);
        if (!val) continue;
        if (*val > 1) continue;  // toggles are always 0 or 1

        const char* name = nullptr;
        if (off == 0x34EDFBE) name = "research_on_icon_click (CONFIRMED)";
        if (off == 0x34EDFD0) name = "instantconstruction (CONFIRMED)";
        if (off == 0x34EDFD1) name = "instantshiprefit (CONFIRMED)";
        if (off == 0x34EDFD2) name = "focus.autocomplete (CANDIDATE)";
        if (off == 0x34EDFD3) name = "instanttraining (CANDIDATE)";
        if (off == 0x34EDFD4) name = "allow_trait_assign (CANDIDATE)";
        if (off == 0x34EE029) name = "sp_instant (CONFIRMED)";

        probes.push_back({ off, *val, name });
    }

    std::printf("  %-12s  %-5s  %s\n", "Offset", "Value", "Known as");
    std::printf("  %-12s  %-5s  %s\n", "------", "-----", "--------");
    for (const auto& p : probes) {
        std::printf("  0x%-10llx  %-5d  %s\n",
                    static_cast<unsigned long long>(p.offset),
                    p.value,
                    p.known ? p.known : "");
    }

    std::printf("\n%zu byte(s) in range [0x%llx, 0x%llx) hold 0 or 1.\n\n",
                probes.size(),
                static_cast<unsigned long long>(kToggleBase),
                static_cast<unsigned long long>(kToggleEnd));

    std::printf("To test a candidate:\n");
    std::printf("  1) Note which toggles are currently OFF in-game\n");
    std::printf("  2) Enter an offset to flip (e.g. 34EDFD2)\n");
    std::printf("  3) Check in-game which toggle changed\n\n");

    std::printf("Enter offset to flip (hex, or 'b' to go back): ");
    std::fflush(stdout);

    std::string input;
    if (!readLine(input) || input == "b" || input == "B" || input.empty()) return;

    uint64_t off = std::strtoull(input.c_str(), nullptr, 16);
    if (off < kToggleBase || off >= kToggleEnd) {
        std::printf("[-] Offset out of range.\n");
        return;
    }

    auto current = game.process().read<uint8_t>(game.process().imageBase() + off);
    if (!current) {
        std::printf("[-] Could not read that offset.\n");
        return;
    }

    uint8_t newVal = (*current == 0) ? 1 : 0;
    std::string err;
    if (!game.process().write<uint8_t>(game.process().imageBase() + off, newVal, err)) {
        std::printf("[-] Write failed: %s\n", err.c_str());
        return;
    }

    std::printf("\n[+] Flipped 0x%llx: %d -> %d\n",
                static_cast<unsigned long long>(off), *current, newVal);
    std::printf("    Now check in-game which toggle changed!\n");
}

} // namespace

int main() {
    std::printf("HOI4 trainer - macOS x86_64\n");
    line();

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

    hoi4::Game game(process);

    // Show console status (user enables it manually via menu 'i')
    std::printf("[i] Developer Console: %s  (use 'i' in the menu to toggle)\n",
                game.isConsolePatchActive() ? "ACTIVE" : "not active");

    // Division stats are all recomputed by the game, so the only way to hold
    // them is to rewrite them on a timer. This owns the thread that does it;
    // its destructor stops it, so quitting the menu always shuts it down.
    hoi4::DivisionFreeze freeze(game);

    if (!game.playerCountry()) {
        std::fprintf(stderr,
            "\n[-] Could not resolve the player country.\n"
            "    Load a campaign and get onto the map, then run this again.\n");
        return 2;
    }

    auto initialTag = game.playerTag();
    if (initialTag) {
        g_originalTag = *initialTag;
    }

    while (true) {
        // Re-resolve every loop: objects move when the game reallocates.
        auto country = game.playerCountry();
        if (!country) {
            std::printf("\n[-] Lost the player country. Still in a campaign?\n");
            std::printf("    Press enter to retry, or type q to quit: ");
            std::fflush(stdout);
            std::string answer;
            if (!readLine(answer) || answer == "q") break;
            continue;
        }

        showStatus(game, *country);

        line();
        std::printf("  1) Add manpower\n");
        std::printf("  2) Unlimited resources\n");
        std::printf("  3) Raise manpower ceiling (scale population)\n");
        std::printf("  4) Buildings (factories, infrastructure, dockyards)\n");
        std::printf("  5) Army / navy / air experience\n");
        std::printf("  6) Political power\n");
        std::printf("  7) Command power\n");
        std::printf("  8) Nuclear bombs\n");
        std::printf("  9) Research on icon click (instant tech)\n");
        std::printf("  b) Breakthrough points (special projects)\n");
        std::printf("  s) Instant toggles (focus, training, construction, traits, refit, projects)\n");
        std::printf("  e) Equipment by type (Logistics rows)\n");
        std::printf("  v) Equipment by variant (Stockpile rows)\n");
        std::printf("  c) Production costs (your own designs)\n");
        std::printf("  l) add_latest_equipment\n");
        std::printf("  p) Production lines (output via cost)\n");
        std::printf("  n) Naval object address\n");
        std::printf("  d) Divisions (organisation, HP, combat stats)%s\n",
                    freeze.running() ? "  [FREEZE RUNNING]" : "");
        std::printf("  t) Switch country tag (control another country / troll)%s\n",
                    (g_originalTag > 0 && game.playerTag() && *game.playerTag() != g_originalTag)
                    ? "  [ALT TAG ACTIVE]" : "");
        std::printf("  i) Developer console in Ironman & Multiplayer (%s, keeps achievements)\n",
                    game.isConsolePatchActive() ? "ENABLED [~ key unlocked]" : "disabled");
        std::printf("  g) Military leaders (generals, marshals, admirals XP / skills)\n");
        std::printf("  a) Intelligence agency & operations (La Résistance godmode, 100%% network, instant upgrades)\n");
        std::printf("  m) Doctrines & subdoctrines (instant unlocks, subdoctrine maxing, 500 XP)\n");
        std::printf("  x) Toggle offset scanner (find focus/training/traits offsets)\n");
        std::printf("  r) Refresh status\n");
        std::printf("  q) Quit\n");
        line();
        std::printf("Choice: ");
        std::fflush(stdout);

        std::string choice;
        if (!readLine(choice)) break;

        if (choice == "q" || choice == "Q") break;
        if (choice == "r" || choice == "R" || choice.empty()) continue;

        if      (choice == "1") { doManpower(game, *country);   waitForEnter(); }
        else if (choice == "2") { doResources(game, *country);  waitForEnter(); }
        else if (choice == "3") { doPopulation(game, *country); waitForEnter(); }
        else if (choice == "4") { doBuildings(game, *country);            }
        else if (choice == "5") { doExperience(game, *country);     waitForEnter(); }
        else if (choice == "6") { doPoliticalPower(game, *country); waitForEnter(); }
        else if (choice == "7") { doCommandPower(game, *country);   waitForEnter(); }
        else if (choice == "8") { doNukes(game, *country);          waitForEnter(); }
        else if (choice == "9") { doResearchOnClick(game);          waitForEnter(); }
        else if (choice == "s" || choice == "S")
                                { doToggles(game);                 waitForEnter(); }
        else if (choice == "b" || choice == "B")
                                { doBreakthroughs(game, *country);  waitForEnter(); }
        else if (choice == "e" || choice == "E")
                                { doEquipmentArchetypes(game, *country); waitForEnter(); }
        else if (choice == "v" || choice == "V")
                                { doEquipmentVariants(game, *country); waitForEnter(); }
        else if (choice == "c" || choice == "C")
                                { doCosts(game, *country);         waitForEnter(); }
        else if (choice == "l" || choice == "L")
                                { doAddLatestEquipment(game, *country); waitForEnter(); }
        else if (choice == "p" || choice == "P")
                                { doProduction(game, *country);    waitForEnter(); }
        else if (choice == "n" || choice == "N")
                                { doNaval(game, *country);         waitForEnter(); }
        else if (choice == "d" || choice == "D")
                                { doDivisions(game, freeze);                       }
        else if (choice == "t" || choice == "T")
                                { doTagSwitch(game, freeze);                       }
        else if (choice == "i" || choice == "I")
                                { doIronmanConsole(game);                          }
        else if (choice == "g" || choice == "G")
                                { doLeaders(game, *country);                      }
        else if (choice == "a" || choice == "A")
                                { doAgency(game);                                  }
        else if (choice == "m" || choice == "M")
                                { doDoctrines(game, *country);                     }
        else if (choice == "x" || choice == "X")
                                { doToggleScanner(game);            waitForEnter(); }
        else    std::printf("Unknown choice.\n");
    }

    // Leaving the values frozen after the menu closes would keep a thread
    // writing into a process this program no longer watches.
    if (freeze.running()) {
        freeze.stop();
        std::printf("\nFreeze stopped.\n");
    }

    std::printf("\nDone.\n");
    return 0;
}
