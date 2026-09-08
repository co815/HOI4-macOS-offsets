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
constexpr int64_t kDefaultOrganisation   = 200;
constexpr int64_t kDefaultHitPoints      = 5000;
constexpr int64_t kDefaultCombatStat     = 1000;
constexpr int     kDefaultFreezeInterval = 200;   // milliseconds

// Building indices in the per-state container. The index is stable across
// states and across countries, but the index -> type mapping is not stored
// anywhere readable, so each one has to be confirmed by writing to it and
// looking at the state screen.
//
// Only infrastructure is confirmed so far: writing index 0 across twelve
// states moved every state's infrastructure counter at once.
//
// Use "Identify building indices" in the menu to fill in the rest, then edit
// these constants and rebuild - or just use "Set a building index directly".
struct BuildingIndices {
    int infrastructure   = 0;    // confirmed
    int civilianFactory  = -1;   // not yet identified
    int militaryFactory  = -1;   // not yet identified
    int dockyard         = -1;   // not yet identified
};

BuildingIndices g_buildings;

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

    std::printf("\nPlayer tag      : %d\n", tag ? *tag : -1);
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

        if (static_cast<int>(index) == g_buildings.infrastructure)  std::printf("  infrastructure");
        if (static_cast<int>(index) == g_buildings.civilianFactory) std::printf("  civilian factory");
        if (static_cast<int>(index) == g_buildings.militaryFactory) std::printf("  military factory");
        if (static_cast<int>(index) == g_buildings.dockyard)        std::printf("  dockyard");
        std::printf("\n");
    }

    std::printf("\n%zu states.\n\n", states.size());
    std::printf("To name a row: open a state in game, count its civilian and\n"
                "military factories, then find the row whose numbers match that\n"
                "pattern across all states. A row with a single non-zero value\n"
                "is usually the dockyard - only coastal states have one.\n");
}

void doIdentifyBuildings(hoi4::Game& game, uint64_t country) {
    line();
    std::printf("Identify building indices\n");
    line();

    showBuildingGrid(game, country);

    std::printf("\nWrite a marker value into one index to confirm it?\n");
    if (!confirm("Probe an index")) return;

    long long index = promptNumber("Index to probe", 1);
    long long value = promptNumber("Marker value", 7);

    std::string error;
    int touched = 0;
    if (!game.setBuildingEverywhere(country, static_cast<int>(index),
                                    static_cast<int16_t>(value), error, &touched)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }
    std::printf("\nWrote %lld to index %lld in %d states.\n", value, index, touched);
    std::printf("Open any state in game - whichever counter reads %lld is this index.\n", value);

    if (!confirm("\nRecord this index now")) return;

    std::printf("  1) infrastructure   2) civilian factory\n");
    std::printf("  3) military factory 4) dockyard\n");
    long long which = promptNumber("Which building is it", 0);

    switch (which) {
        case 1: g_buildings.infrastructure  = static_cast<int>(index); break;
        case 2: g_buildings.civilianFactory = static_cast<int>(index); break;
        case 3: g_buildings.militaryFactory = static_cast<int>(index); break;
        case 4: g_buildings.dockyard        = static_cast<int>(index); break;
        default: std::printf("Not recorded.\n"); return;
    }
    std::printf("Recorded for this session. Edit BuildingIndices in the source\n"
                "and rebuild to make it permanent.\n");
}

void setNamedBuilding(hoi4::Game& game, uint64_t country, int index, const char* name) {
    if (index < 0) {
        std::printf("\n[-] The index for %s has not been identified yet.\n"
                    "    Use \"Identify building indices\" first.\n", name);
        return;
    }

    long long level = promptNumber("Level to set", kDefaultLevel);

    std::string error;
    int touched = 0;
    if (!game.setBuildingEverywhere(country, index, static_cast<int16_t>(level),
                                    error, &touched)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }
    std::printf("\n%s set to %lld in %d states.\n", name, level, touched);
}

void doBuildings(hoi4::Game& game, uint64_t country) {
    while (true) {
        line();
        std::printf("Buildings\n");
        line();
        std::printf("  1) Identify building indices (grid + probe)\n");
        std::printf("  2) Infrastructure      %s\n",
                    g_buildings.infrastructure  >= 0 ? "" : "(index unknown)");
        std::printf("  3) Civilian factories  %s\n",
                    g_buildings.civilianFactory >= 0 ? "" : "(index unknown)");
        std::printf("  4) Military factories  %s\n",
                    g_buildings.militaryFactory >= 0 ? "" : "(index unknown)");
        std::printf("  5) Dockyards           %s\n",
                    g_buildings.dockyard        >= 0 ? "" : "(index unknown)");
        std::printf("  6) Set a building index directly\n");
        std::printf("  b) Back\n");
        line();
        std::printf("Choice: ");
        std::fflush(stdout);

        std::string choice;
        if (!readLine(choice)) return;
        if (choice == "b" || choice == "B" || choice.empty()) return;

        if      (choice == "1") doIdentifyBuildings(game, country);
        else if (choice == "2") setNamedBuilding(game, country, g_buildings.infrastructure,  "Infrastructure");
        else if (choice == "3") setNamedBuilding(game, country, g_buildings.civilianFactory, "Civilian factories");
        else if (choice == "4") setNamedBuilding(game, country, g_buildings.militaryFactory, "Military factories");
        else if (choice == "5") setNamedBuilding(game, country, g_buildings.dockyard,        "Dockyards");
        else if (choice == "6") {
            long long index = promptNumber("Building index", 0);
            long long level = promptNumber("Level to set", kDefaultLevel);

            std::string error;
            int touched = 0;
            if (!game.setBuildingEverywhere(country, static_cast<int>(index),
                                            static_cast<int16_t>(level), error, &touched))
                std::printf("[-] Failed: %s\n", error.c_str());
            else
                std::printf("\nIndex %lld set to %lld in %d states.\n", index, level, touched);
        }
        else continue;

        std::printf("\nFactories are still limited by unlocked building slots,\n"
                    "so a level above the slot count may not all be usable.\n");
        waitForEnter();
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
        std::printf("  %-24s%s\n", name,
                    state ? (*state ? "on" : "off") : "?");
    };

    show("1) Special projects", game.instantSpecialProjects());
    show("2) Ship refit", game.instantShipRefit());
    show("3) Construction", game.instantConstruction());
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
    std::printf("add_latest_equipment\n");
    line();

    auto all = game.designs(country);
    if (all.empty()) {
        std::printf("[-] Could not read the variant database.\n");
        return;
    }

    int named = 0;
    for (const auto& d : all) if (d.latest) ++named;

    std::printf("%d design%s:\n\n", named, named == 1 ? "" : "s");
    for (const auto& d : all) {
        if (!d.latest) continue;
        std::printf("  %s\n", d.name.c_str());
    }
    std::printf("\n");

    long long value = promptNumber("How many of each", kDefaultStock);
    if (value < 0) { std::printf("Nothing to do.\n"); return; }

    std::string error;
    int changed = 0;
    if (!game.addLatestEquipment(country, value, error, &changed)) {
        std::printf("[-] Failed: %s\n", error.c_str());
        return;
    }

    std::printf("\nGave %lld of each to %d design%s.\n",
                static_cast<long long>(value), changed,
                changed == 1 ? "" : "s");
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
    std::printf("\n%zu division%s:\n\n", divisions.size(),
                divisions.size() == 1 ? "" : "s");
    std::printf("  #     address           org        HP      def     brk    soft\n");
    std::printf("  ---------------------------------------------------------------\n");

    for (size_t i = 0; i < divisions.size(); ++i) {
        const auto& d = divisions[i];
        std::printf("  %-5zu 0x%-14llx  %4lld/%-4lld %7lld %7lld %7lld %7lld\n",
                    i,
                    static_cast<unsigned long long>(d.address),
                    static_cast<long long>(d.organisation),
                    static_cast<long long>(d.maxOrganisation),
                    static_cast<long long>(d.hitPoints),
                    static_cast<long long>(d.defense),
                    static_cast<long long>(d.breakthrough),
                    static_cast<long long>(d.softAttack));
    }
    std::printf("\n");
}

void showFreezeSettings(const hoi4::DivisionGodmode& s) {
    std::printf("  Organisation      %-4s  set to %lld\n",
                s.organisation ? "on" : "off",
                static_cast<long long>(s.organisationValue));
    std::printf("  Max organisation  %-4s  set to %lld\n",
                s.maxOrganisation ? "on" : "off",
                static_cast<long long>(s.organisationValue));
    std::printf("  Hit points        %-4s  set to %lld\n",
                s.hitPoints ? "on" : "off",
                static_cast<long long>(s.hitPointsValue));
    std::printf("  Combat stats      %-4s  set to %lld  (defense, breakthrough, soft attack)\n",
                s.combatStats ? "on" : "off",
                static_cast<long long>(s.combatStatValue));
    std::printf("  Rewrite every     %d ms\n", s.intervalMs);
}

void doDivisions(hoi4::Game& game, hoi4::DivisionFreeze& freeze) {
    static hoi4::DivisionGodmode settings = [] {
        hoi4::DivisionGodmode s;
        s.organisation      = true;
        s.maxOrganisation   = true;
        s.hitPoints         = false;
        s.combatStats       = false;
        s.organisationValue = kDefaultOrganisation;
        s.hitPointsValue    = kDefaultHitPoints;
        s.combatStatValue   = kDefaultCombatStat;
        s.intervalMs        = kDefaultFreezeInterval;
        return s;
    }();

    while (true) {
        line();
        std::printf("Divisions\n");
        line();

        auto divisions = game.playerDivisions();

        if (divisions.empty() && !freeze.running()) {
            explainAnchor();
            waitForEnter();
            return;
        }

        if (!divisions.empty()) showDivisions(divisions);

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
            std::printf("Freeze is stopped. None of these fields hold on their own -\n");
            std::printf("the game recomputes all of them, so a single write shows up\n");
            std::printf("in game and is gone again within a tick or a day.\n\n");
        }

        showFreezeSettings(settings);

        std::printf("\n");
        std::printf("  1) %s the freeze\n", status.running ? "Stop" : "Start");
        std::printf("  2) Apply once, without the freeze\n");
        std::printf("  3) Toggle organisation        (currently %s)\n",
                    settings.organisation ? "on" : "off");
        std::printf("  4) Toggle max organisation    (currently %s)\n",
                    settings.maxOrganisation ? "on" : "off");
        std::printf("  5) Toggle hit points          (currently %s)\n",
                    settings.hitPoints ? "on" : "off");
        std::printf("  6) Toggle combat stats        (currently %s)\n",
                    settings.combatStats ? "on" : "off");
        std::printf("  7) Change the values\n");
        std::printf("  b) Back\n");
        line();
        std::printf("Choice: ");
        std::fflush(stdout);

        std::string choice;
        if (!readLine(choice)) return;
        if (choice == "b" || choice == "B" || choice.empty()) return;

        std::string error;

        if (choice == "1") {
            if (freeze.running()) {
                freeze.stop();
                std::printf("\nStopped. The game will pull everything back to its own\n"
                            "numbers on the next recalculation.\n");
            } else if (!freeze.start(settings, error)) {
                std::printf("\n[-] Could not start: %s\n", error.c_str());
            } else {
                std::printf("\nRunning. Values are rewritten every %d ms across every\n"
                            "division your country owns. The list is rebuilt by a full\n"
                            "scan every 10 seconds so newly trained divisions are\n"
                            "picked up, and every address is re-checked for a live\n"
                            "slot and the right owner immediately before it is\n"
                            "written - a disbanded division is dropped rather than\n"
                            "written into.\n", settings.intervalMs);
            }
            waitForEnter();

        } else if (choice == "2") {
            int found = 0;
            const int written = game.applyGodmodeToPlayer(settings, &found);
            std::printf("\nWrote %d of %d division%s.\n",
                        written, found, found == 1 ? "" : "s");
            std::printf("\nWatch the tooltip: organisation goes back within a tick if\n"
                        "the division is on Army Exercises, and max organisation\n"
                        "reverts at the next day boundary. That is what the freeze\n"
                        "is for.\n");
            waitForEnter();

        } else if (choice == "3") {
            settings.organisation = !settings.organisation;
            freeze.setSettings(settings);

        } else if (choice == "4") {
            settings.maxOrganisation = !settings.maxOrganisation;
            freeze.setSettings(settings);

        } else if (choice == "5") {
            settings.hitPoints = !settings.hitPoints;
            freeze.setSettings(settings);

        } else if (choice == "6") {
            settings.combatStats = !settings.combatStats;
            freeze.setSettings(settings);

        } else if (choice == "7") {
            std::printf("\nValues are entered the way the game shows them - 200 means\n"
                        "an organisation of 200, not 0.002.\n\n");

            settings.organisationValue =
                promptNumber("Organisation and its maximum", settings.organisationValue);
            settings.hitPointsValue =
                promptNumber("Hit points", settings.hitPointsValue);
            settings.combatStatValue =
                promptNumber("Defense, breakthrough and soft attack",
                             settings.combatStatValue);

            long long interval = promptNumber("Rewrite interval in ms",
                                              settings.intervalMs);
            if (interval < 20)   interval = 20;
            if (interval > 5000) interval = 5000;
            settings.intervalMs = static_cast<int>(interval);

            freeze.setSettings(settings);
            std::printf("\nUpdated. A running freeze picks these up on its next pass.\n");
            waitForEnter();
        }
    }
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
        std::printf("  s) Instant toggles (projects, refit, construction)\n");
        std::printf("  e) Equipment by type (Logistics rows)\n");
        std::printf("  v) Equipment by variant (Stockpile rows)\n");
        std::printf("  c) Production costs (your own designs)\n");
        std::printf("  l) add_latest_equipment\n");
        std::printf("  p) Production lines (output via cost)\n");
        std::printf("  n) Naval object address\n");
        std::printf("  d) Divisions (organisation, HP, combat stats)%s\n",
                    freeze.running() ? "  [FREEZE RUNNING]" : "");
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
