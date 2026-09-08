// hoi4_sdk.hpp - Hearts of Iron IV game layer (macOS, x86_64)
//
// Built on memory.hpp. Offsets live in hoi4_offsets.hpp.
//
// Design note: the pointer chain is resolved from the static offset on every
// call. Nothing is cached between calls. This costs a few extra reads but
// avoids the failure mode that dominates this kind of tooling - the allocator
// frees an object and reuses the block, leaving plausible-looking garbage at
// an address you saved earlier. The country-level resource container in
// particular is reallocated on every recalculation.

#pragma once

#include "memory.hpp"
#include "hoi4_offsets.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <vector>
#include <optional>
#include <string>
#include <climits>

// Needed by DivisionFreeze at the bottom of this file. Division stats are all
// recomputed by the game, so holding a value means rewriting it on a timer
// from a background thread rather than writing it once.
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

namespace hoi4 {

// One state's manpower data, with the addresses it was read from so the
// caller can write back without re-resolving.
struct StateManpower {
    uint64_t stateAddress   = 0;   // State*
    uint64_t manpowerObject = 0;   // State* + stateManpowerObject
    int32_t  manpower       = 0;
    int32_t  population     = 0;
};

// One resource slot inside one state. `value` is the persistent BASE amount -
// the number shown as "Base:" in the state tooltip.
struct StateResource {
    uint64_t stateAddress = 0;
    uint64_t valueAddress = 0;   // State* + 0x1E4 + slot*16
    int      slot         = 0;
    int32_t  id           = 0;
    int32_t  value        = 0;
    const char* name      = nullptr;   // inferred, may be null
};

// One building slot inside one state. `level` is the persistent value the
// game itself reads when evaluating building_level.
struct StateBuilding {
    uint64_t stateAddress = 0;
    uint64_t entryAddress = 0;   // the entry object
    uint64_t levelAddress = 0;   // entry + buildingLevel
    int      index        = 0;   // position in the entry array
    int16_t  level        = 0;
};

// A country-level derived total, read from the container at Country+0xF80.
// These are recalculated every tick - useful to read, pointless to write.
struct ResourceTotal {
    int      slot  = 0;
    int32_t  id    = 0;
    int64_t  value = 0;          // already divided by the fixed-point scale
    const char* name = nullptr;
};

enum class ResourceContainer {
    Extracted = 0,
    Imported  = 1,
    Exported  = 2,
    Projects  = 3,
    Production = 4,
};

struct CountryInfo {
    int32_t  tag           = 0;
    int32_t  internalIndex = 0;
    uint64_t address       = 0;
    int32_t  stateCount    = 0;
};

// One division, read out of the pool. The addresses are kept so a caller can
// write back without walking the pool again - but note they are only good
// until the game frees the object, which is why the freeze loop re-enumerates
// instead of holding on to a list.
struct Division {
    uint64_t address         = 0;   // the division object
    uint64_t owner           = 0;   // Country*, straight off the division
    int      slot            = 0;   // position in the pool, relative to the anchor
    int64_t  hitPoints       = 0;   // already divided by the scale
    int64_t  organisation    = 0;
    int64_t  maxOrganisation = 0;
    int64_t  defense         = 0;
    int64_t  breakthrough    = 0;
    int64_t  softAttack      = 0;
};

// What DivisionFreeze holds, and at what value. Everything is off by default -
// the caller turns on what it wants.
//
// The values are in the units the game shows, not raw: organisation 200 means
// the tooltip reads 200, not 0.002.
struct DivisionGodmode {
    bool    organisation    = true;
    bool    maxOrganisation = true;
    bool    hitPoints       = false;
    bool    combatStats     = false;

    int64_t organisationValue = 200;
    int64_t hitPointsValue    = 5000;
    int64_t combatStatValue   = 1000;

    // How often to rewrite. The game restores organisation within a tick while
    // exercising and max organisation at a day boundary, so anything in this
    // range wins the race with room to spare. Lower is not better - each pass
    // costs a few reads and writes per division.
    int intervalMs = 200;

    bool anythingOn() const {
        return organisation || maxOrganisation || hitPoints || combatStats;
    }
};

// Which of the three experience pools to act on.
enum class Branch { Army, Navy, Air };

class Game {
public:
    explicit Game(mem::Process& process, Offsets offsets = {})
        : process_(process), offsets_(offsets) {}

    const Offsets&   offsets()   const { return offsets_; }
    Offsets&         offsets()         { return offsets_; }
    const Functions& functions() const { return functions_; }

    // ------------------------------------------------------------ core chain

    std::optional<uint64_t> gameState() const {
        auto gs = process_.read<uint64_t>(process_.imageBase() + offsets_.gameStatePointer);
        if (!gs || !mem::Process::plausiblePointer(*gs)) return std::nullopt;
        return gs;
    }

    // Mirrors the game's own logic: primary field wins when positive.
    std::optional<int32_t> playerTag() const {
        auto gs = gameState();
        if (!gs) return std::nullopt;

        auto primary = process_.read<int32_t>(*gs + offsets_.playerTagPrimary);
        if (primary && *primary > 0) return primary;
        return process_.read<int32_t>(*gs + offsets_.playerTagFallback);
    }

    std::optional<int32_t> tagToIndex(int32_t tag) const {
        if (tag < 0) return std::nullopt;
        auto gs = gameState();
        if (!gs) return std::nullopt;

        auto table = process_.read<uint64_t>(*gs + offsets_.tagIndexTable);
        if (!table || !mem::Process::plausiblePointer(*table)) return std::nullopt;

        return process_.read<int32_t>(*table + static_cast<int64_t>(tag) * 4);
    }

    // Country* for an arbitrary tag. Reproduces countryFromTag without calling it.
    std::optional<uint64_t> country(int32_t tag) const {
        auto gs    = gameState();
        auto index = tagToIndex(tag);
        if (!gs || !index || *index < 0) return std::nullopt;

        auto array = process_.read<uint64_t>(*gs + offsets_.countryArray);
        if (!array || !mem::Process::plausiblePointer(*array)) return std::nullopt;

        auto ptr = process_.read<uint64_t>(*array + static_cast<int64_t>(*index) * 8);
        if (!ptr || !mem::Process::plausiblePointer(*ptr)) return std::nullopt;
        return ptr;
    }

    std::optional<uint64_t> playerCountry() const {
        auto tag = playerTag();
        if (!tag) return std::nullopt;
        return country(*tag);
    }

    // ---------------------------------------------------------------- states

    std::vector<uint64_t> states(uint64_t countryAddress) const {
        std::vector<uint64_t> result;

        auto array = process_.read<uint64_t>(countryAddress + offsets_.stateArray);
        auto count = process_.read<int32_t>(countryAddress + offsets_.stateCount);
        if (!array || !count) return result;
        if (!mem::Process::plausiblePointer(*array)) return result;
        if (*count <= 0 || *count > kMaxStates) return result;

        result.reserve(static_cast<size_t>(*count));
        for (int32_t i = 0; i < *count; ++i) {
            auto state = process_.read<uint64_t>(*array + static_cast<int64_t>(i) * 8);
            if (state && mem::Process::plausiblePointer(*state))
                result.push_back(*state);
        }
        return result;
    }

    // ------------------------------------------------------------- resources

    // Looks up the inferred display name for a resource id.
    static const char* resourceName(int32_t id) {
        for (const auto& r : kResources)
            if (r.id == id) return r.name;
        return nullptr;
    }

    // Per-state BASE resource amounts. This is the persistent, writable data.
    // Note the state container starts at slot 0 - unlike the country-level
    // container, slot 0 here is a real resource.
    std::vector<StateResource> stateResources(uint64_t stateAddress) const {
        std::vector<StateResource> result;

        for (int slot = offsets_.firstStateResourceSlot;
             slot < offsets_.resourceSlotCount; ++slot) {

            const int64_t stride = offsets_.stateResourceStride * slot;

            auto id    = process_.read<int32_t>(stateAddress + offsets_.stateResourceId + stride);
            auto value = process_.read<int32_t>(stateAddress + offsets_.stateResourceValue + stride);
            if (!id || !value) continue;

            StateResource entry;
            entry.stateAddress = stateAddress;
            entry.valueAddress = stateAddress + offsets_.stateResourceValue + stride;
            entry.slot  = slot;
            entry.id    = *id;
            entry.value = *value;
            entry.name  = resourceName(*id);
            result.push_back(entry);
        }
        return result;
    }

    // Sums the BASE amounts across a country's states, keyed by resource id.
    // This is the pre-modifier total, so it reads lower than the Trade tab.
    //
    // Aggregating by id rather than slot matters: slot ordering is not
    // guaranteed to be identical for every country, and an id that appears in
    // one state may be absent from another.
    std::vector<ResourceTotal> baseResourceTotals(uint64_t countryAddress) const {
        std::vector<ResourceTotal> totals;

        for (uint64_t state : states(countryAddress)) {
            for (const auto& r : stateResources(state)) {
                if (r.id == 0) continue;

                auto it = totals.end();
                for (auto candidate = totals.begin(); candidate != totals.end(); ++candidate)
                    if (candidate->id == r.id) { it = candidate; break; }

                if (it == totals.end()) {
                    ResourceTotal t;
                    t.slot  = r.slot;
                    t.id    = r.id;
                    t.name  = r.name;
                    t.value = r.value;
                    totals.push_back(t);
                } else {
                    it->value += r.value;
                }
            }
        }
        return totals;
    }

    // Country-level derived totals, as shown in the Trade tab. Read-only in
    // practice: the container is rebuilt on every recalculation.
    std::vector<ResourceTotal> derivedTotals(
            uint64_t countryAddress,
            ResourceContainer which = ResourceContainer::Extracted) const {

        std::vector<ResourceTotal> result;

        auto resObj = process_.read<uint64_t>(countryAddress + offsets_.resourceObject);
        if (!resObj || !mem::Process::plausiblePointer(*resObj)) return result;

        auto buffer = process_.read<uint64_t>(*resObj + offsets_.resourceBuffer);
        if (!buffer || !mem::Process::plausiblePointer(*buffer)) return result;

        const uint64_t base = *buffer
            + static_cast<int64_t>(which) * offsets_.resourceContainerStride;

        for (int slot = offsets_.firstDerivedResourceSlot;
             slot < offsets_.resourceSlotCount; ++slot) {

            auto value = process_.read<int64_t>(base + slot * 16);
            auto id    = process_.read<int64_t>(base + slot * 16 + 8);
            if (!value || !id) continue;

            ResourceTotal t;
            t.slot  = slot;
            t.id    = static_cast<int32_t>(*id);
            t.value = *value / offsets_.resourceFixedPointScale;
            t.name  = resourceName(t.id);
            result.push_back(t);
        }
        return result;
    }

    // --- resource writing ---

    // Sets one slot in one state.
    bool setStateResource(const StateResource& entry, int32_t value, std::string& error) {
        return process_.write<int32_t>(entry.valueAddress, clamp32(value), error);
    }

    // Sets every resource slot in every state the country owns.
    //
    // Keep the value modest. Each state's BASE is multiplied by state
    // modifiers, infrastructure and supply routes before being summed, so
    // large inputs overflow int32 somewhere downstream and the Logistics bar
    // wraps negative. A value of 50 across 24 states produced roughly 65,000
    // of each resource; 999 overflowed.
    bool setAllResources(uint64_t countryAddress, int32_t value, std::string& error,
                         int* statesTouched = nullptr, int* slotsWritten = nullptr) {
        auto owned = states(countryAddress);
        if (owned.empty()) { error = "country owns no states"; return false; }

        int slots = 0;
        for (uint64_t state : owned) {
            for (int slot = offsets_.firstStateResourceSlot;
                 slot < offsets_.resourceSlotCount; ++slot) {

                const uint64_t address = state + offsets_.stateResourceValue
                                       + offsets_.stateResourceStride * slot;
                if (!process_.write<int32_t>(address, clamp32(value), error))
                    return false;
                slots++;
            }
        }
        if (statesTouched) *statesTouched = static_cast<int>(owned.size());
        if (slotsWritten)  *slotsWritten  = slots;
        return true;
    }

    // Sets a single resource, identified by slot, across every owned state.
    bool setResourceBySlot(uint64_t countryAddress, int slot, int32_t value,
                           std::string& error, int* statesTouched = nullptr) {
        if (slot < offsets_.firstStateResourceSlot || slot >= offsets_.resourceSlotCount) {
            error = "slot out of range";
            return false;
        }
        auto owned = states(countryAddress);
        if (owned.empty()) { error = "country owns no states"; return false; }

        for (uint64_t state : owned) {
            const uint64_t address = state + offsets_.stateResourceValue
                                   + offsets_.stateResourceStride * slot;
            if (!process_.write<int32_t>(address, clamp32(value), error))
                return false;
        }
        if (statesTouched) *statesTouched = static_cast<int>(owned.size());
        return true;
    }

    // ------------------------------------------------------------- buildings

    // Every populated building slot in one state.
    //
    // Iterating the entry array blindly does NOT work: the lookup the game
    // performs checks the index table first,
    //
    //     idx = *(*(container + 0x20) + def->id * 4)
    //     if (idx != -1) return *(*(container + 0x38) + idx * 8)
    //
    // so only the indices the table points at hold real entries. The rest of
    // the array is uninitialised, and reading +0x40 from those slots returns
    // garbage - large negatives and six-digit values.
    //
    // Since the definition ids that feed the table are not enumerable from
    // here, entries are validated instead: the pointer must be plausible and
    // 8-byte aligned, and the level must fall in a sane range. Anything else
    // is treated as an empty slot.
    std::vector<StateBuilding> stateBuildings(uint64_t stateAddress) const {
        std::vector<StateBuilding> result;

        const uint64_t container = stateAddress + offsets_.stateBuildingContainer;

        auto count = process_.read<int32_t>(container + offsets_.buildingCount);
        auto array = process_.read<uint64_t>(container + offsets_.buildingEntryArray);
        if (!count || !array) return result;
        if (*count <= 0 || *count > kMaxBuildings) return result;
        if (!mem::Process::plausiblePointer(*array)) return result;

        result.reserve(static_cast<size_t>(*count));
        for (int i = 0; i < *count; ++i) {
            auto entry = process_.read<uint64_t>(*array + static_cast<int64_t>(i) * 8);
            if (!entry || !isPlausibleEntry(*entry)) continue;

            auto level = process_.read<int16_t>(*entry + offsets_.buildingLevel);
            if (!level || !isPlausibleLevel(*level)) continue;

            StateBuilding b;
            b.stateAddress = stateAddress;
            b.entryAddress = *entry;
            b.levelAddress = *entry + offsets_.buildingLevel;
            b.index        = i;
            b.level        = *level;
            result.push_back(b);
        }
        return result;
    }

    // A building entry lives on the heap and is pointer-aligned.
    static bool isPlausibleEntry(uint64_t entry) {
        return mem::Process::plausiblePointer(entry);
    }

    // Levels are small. Anything outside this range is an uninitialised slot,
    // not a building - the game itself caps them well below this.
    static bool isPlausibleLevel(int16_t level) {
        return level >= 0 && level <= kMaxBuildingLevel;
    }

    bool setBuildingLevel(const StateBuilding& building, int16_t level, std::string& error) {
        return process_.write<int16_t>(building.levelAddress, level, error);
    }

    // Sets one building index across every state the country owns.
    bool setBuildingEverywhere(uint64_t countryAddress, int index, int16_t level,
                               std::string& error, int* statesTouched = nullptr) {
        auto owned = states(countryAddress);
        if (owned.empty()) { error = "country owns no states"; return false; }

        int touched = 0;
        for (uint64_t state : owned) {
            for (const auto& b : stateBuildings(state)) {
                if (b.index != index) continue;
                if (!process_.write<int16_t>(b.levelAddress, level, error))
                    return false;
                touched++;
                break;
            }
        }
        if (statesTouched) *statesTouched = touched;
        return true;
    }

    // ------------------------------------------------------------- manpower

    std::vector<StateManpower> stateManpower(uint64_t countryAddress) const {
        std::vector<StateManpower> result;

        for (uint64_t state : states(countryAddress)) {
            StateManpower entry;
            entry.stateAddress   = state;
            entry.manpowerObject = state + offsets_.stateManpowerObject;

            auto manpower   = process_.read<int32_t>(entry.manpowerObject + offsets_.stateManpower);
            auto population = process_.read<int32_t>(entry.manpowerObject + offsets_.statePopulation);
            if (!manpower || !population) continue;

            entry.manpower   = *manpower;
            entry.population = *population;
            result.push_back(entry);
        }
        return result;
    }

    // Sum across all states - what the top bar displays. The game does not
    // store this total anywhere.
    int64_t totalManpower(uint64_t countryAddress) const {
        int64_t sum = 0;
        for (const auto& s : stateManpower(countryAddress)) sum += s.manpower;
        return sum;
    }

    int64_t totalPopulation(uint64_t countryAddress) const {
        int64_t sum = 0;
        for (const auto& s : stateManpower(countryAddress)) sum += s.population;
        return sum;
    }

    // Spreads an amount across states the way the game's distributor does.
    // Note the game caps this against recruitable population on the next
    // recalculation - scalePopulation is the stable alternative.
    bool addManpower(uint64_t countryAddress, int32_t amount, std::string& error) {
        auto owned = stateManpower(countryAddress);
        if (owned.empty()) { error = "country owns no states"; return false; }

        const int32_t share     = amount / static_cast<int32_t>(owned.size());
        const int32_t remainder = amount - share * static_cast<int32_t>(owned.size());

        for (size_t i = 0; i < owned.size(); ++i) {
            int64_t value = static_cast<int64_t>(owned[i].manpower)
                          + share + (i == 0 ? remainder : 0);
            if (!process_.write<int32_t>(owned[i].manpowerObject + offsets_.stateManpower,
                                         clamp32(value), error))
                return false;
        }
        return true;
    }

    bool scalePopulation(uint64_t countryAddress, double factor, std::string& error) {
        auto owned = stateManpower(countryAddress);
        if (owned.empty()) { error = "country owns no states"; return false; }

        for (const auto& state : owned) {
            int64_t value = clamp32(static_cast<int64_t>(state.population * factor));
            if (!process_.write<int32_t>(state.manpowerObject + offsets_.statePopulation,
                                         static_cast<int32_t>(value), error))
                return false;
        }
        return true;
    }

    // ---------------------------------------------------------------- experience

    struct Experience {
        uint64_t object = 0;   // the XP object itself
        int64_t  army   = 0;   // already divided by the fixed-point scale
        int64_t  navy   = 0;
        int64_t  air    = 0;
    };

    // The XP object hangs off the country at 0x12E8. All three values live in
    // it side by side.
    std::optional<uint64_t> experienceObject(uint64_t countryAddress) const {
        auto obj = process_.read<uint64_t>(countryAddress + offsets_.experienceObject);
        if (!obj || !mem::Process::plausiblePointer(*obj)) return std::nullopt;
        return *obj;
    }

    std::optional<Experience> experience(uint64_t countryAddress) const {
        auto obj = experienceObject(countryAddress);
        if (!obj) return std::nullopt;

        auto army = process_.read<int64_t>(*obj + offsets_.armyExperience);
        auto navy = process_.read<int64_t>(*obj + offsets_.navyExperience);
        auto air  = process_.read<int64_t>(*obj + offsets_.airExperience);
        if (!army || !navy || !air) return std::nullopt;

        const int64_t scale = offsets_.experienceFixedPointScale;

        Experience xp;
        xp.object = *obj;
        xp.army   = *army / scale;
        xp.navy   = *navy / scale;
        xp.air    = *air  / scale;
        return xp;
    }

    // Writes the raw field. The console path also touches an accumulator at
    // +0x48 for army, which this does not - if the top bar does not move,
    // that is the field to try instead.
    bool setExperience(uint64_t countryAddress, Branch branch, int64_t value,
                       std::string& error) {
        auto obj = experienceObject(countryAddress);
        if (!obj) { error = "could not resolve the experience object"; return false; }

        int64_t field = 0;
        switch (branch) {
            case Branch::Army: field = offsets_.armyExperience; break;
            case Branch::Navy: field = offsets_.navyExperience; break;
            case Branch::Air:  field = offsets_.airExperience;  break;
        }

        const int64_t stored = value * offsets_.experienceFixedPointScale;
        return process_.write<int64_t>(*obj + field, stored, error);
    }

    bool setAllExperience(uint64_t countryAddress, int64_t value, std::string& error) {
        return setExperience(countryAddress, Branch::Army, value, error)
            && setExperience(countryAddress, Branch::Navy, value, error)
            && setExperience(countryAddress, Branch::Air,  value, error);
    }

    // -------------------------------------------------------- equipment stock

    struct StockEntry {
        uint64_t    entry     = 0;   // address of the entry, for writing
        uint64_t    archetype = 0;   // the archetype it points at
        std::string name;            // e.g. "infantry_equipment"
        int64_t     quantity  = 0;   // already divided by the scale
        int         index     = 0;
    };

    // Archetype names are script tokens: lowercase letters, digits and
    // underscores, nothing else. Checking this is what separates the real
    // array from memory that happens to share its shape.
    static bool looksLikeArchetypeName(const std::string& name) {
        if (name.size() < 4) return false;

        bool hasLetter = false;
        for (char c : name) {
            const bool lower  = (c >= 'a' && c <= 'z');
            const bool digit  = (c >= '0' && c <= '9');
            const bool symbol = (c == '_');
            if (!lower && !digit && !symbol) return false;
            if (lower) hasLetter = true;
        }
        return hasLetter;
    }

    // The archetype's name, read out of the libc++ std::string it carries.
    std::string archetypeName(uint64_t archetype) const {
        auto header = process_.read<uint8_t>(archetype + offsets_.archetypeName);
        if (!header) return {};

        // Bit 0 clear means the text is stored inline right after the byte.
        if ((*header & 1) == 0) {
            const int length = *header >> 1;
            if (length <= 0 || length > offsets_.archetypeNameMax) return {};

            return process_.readString(archetype + offsets_.archetypeNameInline,
                                       static_cast<size_t>(length));
        }

        // Otherwise the text is on the heap.
        auto size    = process_.read<uint64_t>(archetype + offsets_.archetypeNameSize);
        auto pointer = process_.read<uint64_t>(archetype + offsets_.archetypeNamePointer);
        if (!size || !pointer) return {};
        if (*size == 0 || *size > static_cast<uint64_t>(offsets_.archetypeNameMax)) return {};
        if (!mem::Process::plausiblePointer(*pointer)) return {};

        return process_.readString(*pointer, static_cast<size_t>(*size));
    }

    // Straight off the country - no scanning. Each entry is one of the rows in
    // the Logistics screen: an archetype and the amount held.
    std::vector<StockEntry> equipmentStock(uint64_t countryAddress) const {
        std::vector<StockEntry> result;

        // Country + 0xD10 holds a pointer to the variant database; the
        // container sits at +0x200 inside it. Adding both offsets without
        // following the pointer lands in the middle of the country object.
        auto base = process_.read<uint64_t>(countryAddress
                                            + offsets_.equipmentStockObject);
        if (!base || !mem::Process::plausiblePointer(*base)) return result;

        const uint64_t container = *base + offsets_.equipmentStockInner;

        auto array = process_.read<uint64_t>(container + offsets_.stockGroupArray);
        auto count = process_.read<int32_t>(container + offsets_.stockGroupCount);
        if (!array || !count) return result;
        if (*count <= 0 || *count > kMaxStockEntries) return result;
        if (!mem::Process::plausiblePointer(*array)) return result;

        result.reserve(static_cast<size_t>(*count));
        for (int i = 0; i < *count; ++i) {
            const uint64_t entry = *array
                                 + static_cast<uint64_t>(i) * offsets_.stockGroupStride;

            auto archetype = process_.read<uint64_t>(entry + offsets_.stockGroupArchetype);
            auto quantity  = process_.read<int64_t>(entry + offsets_.stockGroupTotal);
            if (!archetype || !quantity) continue;
            if (!mem::Process::plausiblePointer(*archetype)) continue;

            StockEntry e;
            e.entry     = entry;
            e.archetype = *archetype;
            e.name      = archetypeName(*archetype);
            e.quantity  = *quantity / offsets_.equipmentFixedPointScale;
            e.index     = i;
            result.push_back(e);
        }
        return result;
    }

    bool setStockQuantity(const StockEntry& entry, int64_t quantity,
                          std::string& error) {
        const int64_t stored = quantity * offsets_.equipmentFixedPointScale;
        return process_.write<int64_t>(entry.entry + offsets_.equipmentEntryQuantity,
                                       stored, error);
    }

    bool setAllStock(uint64_t countryAddress, int64_t quantity,
                     std::string& error, int* changed = nullptr) {
        auto entries = equipmentStock(countryAddress);
        if (entries.empty()) {
            error = "could not locate the equipment array";
            return false;
        }

        int written = 0;
        for (const auto& e : entries) {
            if (!setStockQuantity(e, quantity, error)) return false;
            ++written;
        }
        if (changed) *changed = written;
        return true;
    }

    // --------------------------------------------------------- naval object

    // Just the address, for now - enough to dump the object in poke and see
    // which field moves while a ship is building.
    std::optional<uint64_t> navalBase(uint64_t countryAddress) const {
        auto base = process_.read<uint64_t>(countryAddress + offsets_.navalBase);
        if (!base || !mem::Process::plausiblePointer(*base)) return std::nullopt;
        return *base;
    }

    // ----------------------------------------------------- production lines

    struct ProductionLine {
        uint64_t line    = 0;
        uint64_t variant = 0;   // the design on this line
        int64_t  cost    = 0;
        int      index   = 0;
    };

    std::vector<ProductionLine> productionLines(uint64_t countryAddress) const {
        std::vector<ProductionLine> result;

        auto base = process_.read<uint64_t>(countryAddress + offsets_.productionBase);
        if (!base || !mem::Process::plausiblePointer(*base)) return result;

        const uint64_t container = *base + offsets_.productionLines;

        auto array = process_.read<uint64_t>(container + offsets_.productionLineArray);
        auto count = process_.read<int32_t>(container + offsets_.productionLineCount);
        if (!array || !count) return result;
        if (*count <= 0 || *count > offsets_.productionLineMax) return result;
        if (!mem::Process::plausiblePointer(*array)) return result;

        result.reserve(static_cast<size_t>(*count));
        for (int i = 0; i < *count; ++i) {
            auto line = process_.read<uint64_t>(*array + static_cast<uint64_t>(i) * 8);
            if (!line || !mem::Process::plausiblePointer(*line)) continue;

            auto cost    = process_.read<int64_t>(*line + offsets_.productionLineCost);
            auto variant = process_.read<uint64_t>(*line + offsets_.productionLineVariant);

            ProductionLine l;
            l.line    = *line;
            l.variant = (variant && mem::Process::plausiblePointer(*variant))
                      ? *variant : 0;
            l.cost    = cost ? *cost : 0;
            l.index   = i;
            result.push_back(l);
        }
        return result;
    }

    // Output is inversely proportional to this, so a smaller cost means a
    // bigger weekly figure. Zero would divide by zero inside the game, so the
    // caller is kept away from it.
    bool setLineCost(const ProductionLine& line, int64_t cost, std::string& error) {
        if (cost < 1) {
            error = "cost has to be at least 1";
            return false;
        }
        return process_.write<int64_t>(line.line + offsets_.productionLineCost,
                                       cost, error);
    }

    // ----------------------------------------------------- equipment variants

    struct VariantEntry {
        uint64_t    entry    = 0;
        uint64_t    variant  = 0;
        std::string name;          // e.g. "Panzer I Ausf. A"
        int64_t     quantity = 0;
        int64_t     cost     = 0;  // production cost, /100000
        int         index    = 0;
    };

    // Variant display names are readable text, not script tokens, so they
    // allow spaces, capitals and punctuation - "Panzer I Ausf. A".
    static bool looksLikeVariantName(const std::string& name) {
        if (name.size() < 3) return false;

        bool hasLetter = false;
        for (char c : name) {
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (static_cast<unsigned char>(c) > 0x7E) return false;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) hasLetter = true;
        }
        return hasLetter;
    }

    // Falls back to the equipment token when the variant has no name of its
    // own, so default designs show up as "infantry_equipment_1" rather than
    // as a blank.
    std::string variantDisplayName(uint64_t variant) const {
        std::string name = variantName(variant);
        if (!name.empty()) return name;

        auto equipment = process_.read<uint64_t>(variant + offsets_.variantEquipment);
        if (!equipment || !mem::Process::plausiblePointer(*equipment)) return {};

        return readStdString(*equipment + offsets_.variantEquipmentName);
    }

    // Reads either libc++ string layout - see the note in hoi4_offsets.hpp.
    std::string readStdString(uint64_t address) const {
        auto header = process_.read<uint8_t>(address);
        if (!header) return {};

        if ((*header & 1) == 0) {
            const int length = *header >> 1;
            if (length <= 0 || length > offsets_.stringMaxLength) return {};
            return process_.readString(address + 1, static_cast<size_t>(length));
        }

        auto size = process_.read<uint64_t>(address + offsets_.stringLongSize);
        auto text = process_.read<uint64_t>(address + offsets_.stringLongPointer);
        if (!size || !text) return {};
        if (*size == 0 || *size > static_cast<uint64_t>(offsets_.stringMaxLength))
            return {};
        if (!mem::Process::plausiblePointer(*text)) return {};

        return process_.readString(*text, static_cast<size_t>(*size));
    }

    std::string variantName(uint64_t variant) const {
        return readStdString(variant + offsets_.variantName);
    }

    // One array per country, and nothing found so far ties an array to a
    // particular country - a scan alone will happily return Italy's. So this
    // returns every array it finds and the caller picks, which the player can
    // do at a glance from the variant names.
    struct VariantArray {
        uint64_t                  address = 0;
        int                       count   = 0;
        std::vector<VariantEntry> entries;
    };

    std::vector<VariantArray> allVariantArrays() const {
        std::vector<VariantArray> result;

        for (const auto& found : findVariantArrays()) {
            VariantArray array;
            array.address = found.first;
            array.count   = found.second;
            array.entries = readVariantArray(found.first, found.second);
            if (!array.entries.empty()) result.push_back(std::move(array));
        }
        return result;
    }

    // Where the variant array was found. Only meaningful after a scan; used
    // to work out what in the country object points at it.
    uint64_t variantArrayAddress() const { return cachedVariantArray_; }

    // Once the player has identified their array, this remembers it so the
    // menu does not ask again.
    void useVariantArray(uint64_t address, int count) const {
        cachedVariantArray_ = address;
        cachedVariantCount_ = count;
    }

    std::vector<VariantEntry> equipmentVariants() const {
        if (cachedVariantArray_
            && variantArrayStillValid(cachedVariantArray_, cachedVariantCount_))
            return readVariantArray(cachedVariantArray_, cachedVariantCount_);

        return {};
    }

    bool setVariantQuantity(const VariantEntry& entry, int64_t quantity,
                            std::string& error) {
        const int64_t stored = quantity * offsets_.equipmentFixedPointScale;
        return process_.write<int64_t>(entry.entry + offsets_.variantEntryQuantity,
                                       stored, error);
    }

    // A design belongs to the country when it carries a name. The rest of the
    // database is unnamed filler - see the note in hoi4_offsets.hpp for why
    // the flag word at +0x3F8 is not the test it looks like.
    bool variantIsOwnDesign(const std::string& name) const {
        return looksLikeVariantName(name);
    }

    // ------------------------------------------- variants held, off the chain

    // Same container as equipmentStock, second array. No scanning, so this
    // resolves in any campaign and after any restart.
    std::vector<VariantEntry> heldVariants(uint64_t countryAddress) const {
        std::vector<VariantEntry> result;

        auto base = process_.read<uint64_t>(countryAddress
                                            + offsets_.equipmentStockObject);
        if (!base || !mem::Process::plausiblePointer(*base)) return result;

        const uint64_t container = *base + offsets_.equipmentStockInner;

        auto array = process_.read<uint64_t>(container + offsets_.stockEntryArray);
        auto count = process_.read<int32_t>(container + offsets_.stockEntryCount);
        if (!array || !count) return result;
        if (*count <= 0 || *count > kMaxStockEntries) return result;
        if (!mem::Process::plausiblePointer(*array)) return result;

        result.reserve(static_cast<size_t>(*count));
        for (int i = 0; i < *count; ++i) {
            const uint64_t entry = *array
                                 + static_cast<uint64_t>(i) * offsets_.stockEntryStride;

            auto variant  = process_.read<uint64_t>(entry + offsets_.stockEntryVariant);
            auto quantity = process_.read<int64_t>(entry + offsets_.stockEntryQuantity);
            if (!variant || !quantity) continue;
            if (!mem::Process::plausiblePointer(*variant)) continue;

            auto cost = process_.read<int64_t>(*variant + offsets_.variantCost);

            VariantEntry e;
            e.entry    = entry;
            e.variant  = *variant;
            e.name     = variantDisplayName(*variant);
            e.quantity = *quantity / offsets_.equipmentFixedPointScale;
            e.cost     = cost ? *cost : 0;
            e.index    = i;
            result.push_back(e);
        }
        return result;
    }

    // Build time scales with this, so a smaller cost means a faster line.
    // Values in the low hundreds have held up; 1 has too, though the game has
    // no reason to expect it, so treat very low numbers as experimental.
    bool setVariantCost(const VariantEntry& entry, int64_t cost,
                        std::string& error) {
        if (cost < 1) {
            error = "cost has to be at least 1";
            return false;
        }
        return process_.write<int64_t>(entry.variant + offsets_.variantCost,
                                       cost, error);
    }

    bool setAllVariantCosts(uint64_t countryAddress, int64_t cost,
                            std::string& error, int* changed = nullptr) {
        auto entries = heldVariants(countryAddress);
        if (entries.empty()) {
            error = "the country holds no variants";
            return false;
        }

        int written = 0;
        for (const auto& e : entries) {
            if (e.cost <= 0) continue;          // nothing sensible to replace
            if (!setVariantCost(e, cost, error)) return false;
            ++written;
        }

        if (written == 0) {
            error = "none of the variants carry a cost";
            return false;
        }

        if (changed) *changed = written;
        return true;
    }

    // Costs come off the design database rather than the stockpile. Going
    // through the stockpile misses whole categories - ships, land cruisers,
    // railway guns keep their own containers, so Panzerschiff and Ratte never
    // showed up - and it also drags in captured equipment, which is held but
    // not built. The database has exactly what the country designs.
    struct CostEntry {
        uint64_t    variant = 0;
        std::string name;
        int64_t     cost    = 0;
        int         index   = 0;
    };

    // Only what is actually on a production line. The design database holds
    // everything the country could build - 79 entries against 21 lines - so
    // it is the lines that say which of them matter. Each one points at its
    // design at + 0x88; found by reading a naval line and matching the
    // pointer against the Panzerschiff variant.
    std::vector<CostEntry> designCosts(uint64_t countryAddress) const {
        std::vector<CostEntry> result;

        int index = 0;
        for (const auto& l : productionLines(countryAddress)) {
            if (!l.variant) continue;

            auto cost = process_.read<int64_t>(l.variant + offsets_.variantCost);
            if (!cost || *cost <= 0) continue;

            CostEntry e;
            e.variant = l.variant;
            e.name    = variantDisplayName(l.variant);
            e.cost    = *cost;
            e.index   = index++;
            result.push_back(e);
        }
        return result;
    }

    // Takes the cost the way the game shows it - 8663.75 for a Panzerschiff -
    // and scales it. Writing a raw 1 means a cost of 0.00001, which crashes
    // ships; a cost of 1.0 does not, and that is what `u <addr> 1` was
    // writing back when this looked like it worked by hand.
    bool setDesignCost(const CostEntry& entry, int64_t units, std::string& error) {
        if (units < 1) {
            error = "cost has to be at least 1";
            return false;
        }
        const int64_t stored = units * offsets_.equipmentFixedPointScale;
        return process_.write<int64_t>(entry.variant + offsets_.variantCost,
                                       stored, error);
    }

    bool setAllDesignCosts(uint64_t countryAddress, int64_t units,
                           std::string& error, int* changed = nullptr) {
        auto entries = designCosts(countryAddress);
        if (entries.empty()) {
            error = "no designs with a cost";
            return false;
        }

        int written = 0;
        for (const auto& e : entries) {
            if (!setDesignCost(e, units, error)) return false;
            ++written;
        }
        if (changed) *changed = written;
        return true;
    }

    bool setHeldVariant(const VariantEntry& entry, int64_t quantity,
                        std::string& error) {
        const int64_t stored = quantity * offsets_.equipmentFixedPointScale;
        return process_.write<int64_t>(entry.entry + offsets_.stockEntryQuantity,
                                       stored, error);
    }

    bool setAllHeldVariants(uint64_t countryAddress, int64_t quantity,
                            std::string& error, int* changed = nullptr) {
        auto entries = heldVariants(countryAddress);
        if (entries.empty()) {
            error = "the country holds no variants";
            return false;
        }

        int written = 0;
        for (const auto& e : entries) {
            if (!setHeldVariant(e, quantity, error)) return false;
            ++written;
        }
        if (changed) *changed = written;
        return true;
    }

    // ------------------------------------------------- add_latest_equipment

    struct DesignEntry {
        uint64_t    variant = 0;
        std::string name;
        uint64_t    flags   = 0;   // the word at +0x3F8, still being read
        bool        latest  = false;
        int         index   = 0;
    };

    // Every design the country has, straight off the country object. This is
    // the chain the console command uses, so it resolves in any campaign and
    // after any restart - unlike the stock arrays, which have to be found by
    // scanning.
    std::vector<DesignEntry> designs(uint64_t countryAddress) const {
        std::vector<DesignEntry> result;

        auto database = process_.read<uint64_t>(countryAddress
                                                + offsets_.variantDatabasePointer);
        if (!database || !mem::Process::plausiblePointer(*database)) return result;

        auto array = process_.read<uint64_t>(*database + offsets_.variantDatabaseArray);
        auto count = process_.read<int32_t>(*database + offsets_.variantDatabaseCount);
        if (!array || !count) return result;
        if (*count <= 0 || *count > offsets_.variantDatabaseMax) return result;
        if (!mem::Process::plausiblePointer(*array)) return result;

        result.reserve(static_cast<size_t>(*count));
        for (int i = 0; i < *count; ++i) {
            auto variant = process_.read<uint64_t>(*array + static_cast<int64_t>(i) * 8);
            if (!variant || !mem::Process::plausiblePointer(*variant)) continue;

            auto flags = process_.read<uint64_t>(*variant + offsets_.variantActiveFlags);

            DesignEntry d;
            d.variant = *variant;
            d.name    = variantName(*variant);
            d.flags   = flags ? *flags : 0;
            d.latest  = variantIsOwnDesign(d.name);
            d.index   = i;
            result.push_back(d);
        }
        return result;
    }

    // Writes stock for one design. The stock lives in the variant array, so
    // this looks the design up there; a design the country has never held has
    // no entry yet and is reported rather than silently skipped.
    bool giveDesign(uint64_t variant, int64_t quantity, std::string& error) {
        for (const auto& e : equipmentVariants()) {
            if (e.variant != variant) continue;
            return setVariantQuantity(e, quantity, error);
        }
        error = "that design has no stock entry yet";
        return false;
    }

    // add_latest_equipment <amount>, through the same chain the command walks.
    // Every design the country actually owns gets the amount; the unnamed
    // filler entries in the database are skipped.
    bool addLatestEquipment(uint64_t countryAddress, int64_t quantity,
                            std::string& error, int* changed = nullptr) {
        auto all = designs(countryAddress);
        if (all.empty()) {
            error = "could not read the variant database";
            return false;
        }

        int written = 0;
        int skipped = 0;
        std::string lastError;

        for (const auto& d : all) {
            if (!d.latest) continue;

            std::string writeError;
            if (giveDesign(d.variant, quantity, writeError)) ++written;
            else { ++skipped; lastError = writeError; }
        }

        if (written == 0) {
            error = skipped > 0
                  ? "the designs have no stock entries yet - the stock array "
                    "has to be found first, from the variant menu"
                  : "no named designs in the database";
            return false;
        }

        if (changed) *changed = written;
        return true;
    }

    bool setAllVariants(int64_t quantity, std::string& error, int* changed = nullptr) {
        auto entries = equipmentVariants();
        if (entries.empty()) {
            error = "could not locate the variant array";
            return false;
        }

        int written = 0;
        for (const auto& e : entries) {
            if (!setVariantQuantity(e, quantity, error)) return false;
            ++written;
        }
        if (changed) *changed = written;
        return true;
    }

    // ------------------------------------------- special projects / breakthroughs

    struct Breakthrough {
        uint64_t node   = 0;   // address of the map node
        int32_t  id     = 0;   // specialization id
        int32_t  points = 0;   // already divided by the scale
    };

    std::optional<uint64_t> specialProjectsObject(uint64_t countryAddress) const {
        auto obj = process_.read<uint64_t>(countryAddress + offsets_.specialProjectsObject);
        if (!obj || !mem::Process::plausiblePointer(*obj)) return std::nullopt;
        return *obj;
    }

    // The container is a std::map, so this is a depth-first walk rather than
    // an array scan. Uses an explicit stack and a visited set - a corrupted
    // or concurrently-modified tree would otherwise loop forever.
    std::vector<Breakthrough> breakthroughs(uint64_t countryAddress) const {
        std::vector<Breakthrough> result;

        auto obj = specialProjectsObject(countryAddress);
        if (!obj) return result;

        auto root = process_.read<uint64_t>(*obj + offsets_.breakthroughTreeRoot);
        if (!root || !mem::Process::plausiblePointer(*root)) return result;

        auto count = process_.read<int64_t>(*obj + offsets_.breakthroughTreeCount);
        const size_t limit = (count && *count > 0 && *count < kMaxTreeNodes)
                           ? static_cast<size_t>(*count) + 8
                           : kMaxTreeNodes;

        std::vector<uint64_t> stack{*root};
        std::set<uint64_t>    seen;

        while (!stack.empty() && result.size() < limit) {
            uint64_t node = stack.back();
            stack.pop_back();

            if (!mem::Process::plausiblePointer(node)) continue;
            if (!seen.insert(node).second) continue;

            auto key   = process_.read<int32_t>(node + offsets_.mapNodeKey);
            auto value = process_.read<int32_t>(node + offsets_.mapNodeValue);
            if (key && value) {
                Breakthrough b;
                b.node   = node;
                b.id     = *key;
                b.points = *value / static_cast<int32_t>(offsets_.breakthroughFixedPointScale);
                result.push_back(b);
            }

            auto left  = process_.read<uint64_t>(node + offsets_.mapNodeLeft);
            auto right = process_.read<uint64_t>(node + offsets_.mapNodeRight);
            if (left  && mem::Process::plausiblePointer(*left))  stack.push_back(*left);
            if (right && mem::Process::plausiblePointer(*right)) stack.push_back(*right);
        }

        return result;
    }

    // Sets every specialization that already has an entry. The game creates
    // entries lazily, so a specialization the country has never touched will
    // not be in the tree - research something in it once and it appears.
    bool setAllBreakthroughs(uint64_t countryAddress, int32_t points,
                             std::string& error, int* changed = nullptr) {
        auto entries = breakthroughs(countryAddress);
        if (entries.empty()) {
            error = "no breakthrough entries yet - the tree is empty";
            return false;
        }

        const int32_t stored = points
                             * static_cast<int32_t>(offsets_.breakthroughFixedPointScale);

        int written = 0;
        for (const auto& e : entries) {
            if (!process_.write<int32_t>(e.node + offsets_.mapNodeValue, stored, error))
                return false;
            ++written;
        }

        if (changed) *changed = written;
        return true;
    }

    // ------------------------------------------------------------ nuclear bombs

    // 0x1098 holds a pointer, so this needs one dereference.
    std::optional<uint64_t> nukeObject(uint64_t countryAddress) const {
        auto obj = process_.read<uint64_t>(countryAddress + offsets_.nukeObjectPointer);
        if (!obj || !mem::Process::plausiblePointer(*obj)) return std::nullopt;
        return *obj;
    }

    std::optional<int64_t> nukes(uint64_t countryAddress) const {
        auto obj = nukeObject(countryAddress);
        if (!obj) return std::nullopt;

        auto raw = process_.read<int64_t>(*obj + offsets_.nukeCount);
        if (!raw) return std::nullopt;
        return *raw / offsets_.nukeFixedPointScale;
    }

    // The game clamps this at 1000 internally, so anything above that will
    // be trimmed on the next write the game itself makes.
    bool setNukes(uint64_t countryAddress, int64_t count, std::string& error) {
        auto obj = nukeObject(countryAddress);
        if (!obj) { error = "could not resolve the nuke object"; return false; }

        if (count > offsets_.nukeMaximum) count = offsets_.nukeMaximum;
        if (count < 0) count = 0;

        const int64_t stored = count * offsets_.nukeFixedPointScale;
        return process_.write<int64_t>(*obj + offsets_.nukeCount, stored, error);
    }

    // ---------------------------------------------------------- static toggles

    // A single byte at an image-relative address - no country involved.
    std::optional<bool> researchOnIconClick() const {
        auto value = process_.read<uint8_t>(
            process_.imageBase() + offsets_.researchOnIconClick);
        if (!value) return std::nullopt;
        return *value != 0;
    }

    bool setResearchOnIconClick(bool enabled, std::string& error) {
        return process_.write<uint8_t>(
            process_.imageBase() + offsets_.researchOnIconClick,
            enabled ? 1 : 0, error);
    }

    std::optional<bool> instantSpecialProjects() const {
        auto value = process_.read<uint8_t>(
            process_.imageBase() + offsets_.instantSpecialProjects);
        if (!value) return std::nullopt;
        return *value != 0;
    }

    bool setInstantSpecialProjects(bool enabled, std::string& error) {
        return process_.write<uint8_t>(
            process_.imageBase() + offsets_.instantSpecialProjects,
            enabled ? 1 : 0, error);
    }

    std::optional<bool> instantShipRefit() const {
        auto value = process_.read<uint8_t>(
            process_.imageBase() + offsets_.instantShipRefit);
        if (!value) return std::nullopt;
        return *value != 0;
    }

    bool setInstantShipRefit(bool enabled, std::string& error) {
        return process_.write<uint8_t>(
            process_.imageBase() + offsets_.instantShipRefit,
            enabled ? 1 : 0, error);
    }

    std::optional<bool> instantConstruction() const {
        auto value = process_.read<uint8_t>(
            process_.imageBase() + offsets_.instantConstruction);
        if (!value) return std::nullopt;
        return *value != 0;
    }

    bool setInstantConstruction(bool enabled, std::string& error) {
        return process_.write<uint8_t>(
            process_.imageBase() + offsets_.instantConstruction,
            enabled ? 1 : 0, error);
    }

    // ------------------------------------------------------------ command power

    struct CommandPower {
        int64_t current = 0;   // already divided by the scale
        int64_t bonus   = 0;   // how much the cap has been raised by
    };

    // Command power lives directly in the country object - no sub-object.
    std::optional<CommandPower> commandPower(uint64_t countryAddress) const {
        auto current = process_.read<int64_t>(countryAddress + offsets_.commandPower);
        auto cap     = process_.read<int64_t>(countryAddress + offsets_.commandPowerCap);
        if (!current || !cap) return std::nullopt;

        const int64_t scale = offsets_.commandPowerFixedPointScale;

        CommandPower cp;
        cp.current = *current / scale;
        cp.bonus   = -(*cap) / scale;   // stored negated, see below
        return cp;
    }

    bool setCommandPower(uint64_t countryAddress, int64_t value, std::string& error) {
        const int64_t stored = value * offsets_.commandPowerFixedPointScale;
        return process_.write<int64_t>(countryAddress + offsets_.commandPower, stored, error);
    }

    // The game computes the cap as (modifiers) - *(country + 0x1B8), so a
    // negative value in that field raises the ceiling. Pass the bonus you
    // want - 100 here shows up as "Allocated: +100" in the tooltip and takes
    // a base cap of 80 to 180.
    bool raiseCommandPowerCap(uint64_t countryAddress, int64_t bonus, std::string& error) {
        const int64_t stored = -bonus * offsets_.commandPowerFixedPointScale;
        return process_.write<int64_t>(countryAddress + offsets_.commandPowerCap, stored, error);
    }

    // ----------------------------------------------------------- political power

    // Political power sits in the political status object at Country + 0xD38,
    // at +0xE0, on the same 100000 scale as resources.
    std::optional<uint64_t> politicalStatusObject(uint64_t countryAddress) const {
        auto obj = process_.read<uint64_t>(countryAddress + offsets_.politicalStatusObject);
        if (!obj || !mem::Process::plausiblePointer(*obj)) return std::nullopt;
        return *obj;
    }

    std::optional<int64_t> politicalPower(uint64_t countryAddress) const {
        auto obj = politicalStatusObject(countryAddress);
        if (!obj) return std::nullopt;

        auto raw = process_.read<int64_t>(*obj + offsets_.politicalPower);
        if (!raw) return std::nullopt;
        return *raw / offsets_.politicalPowerFixedPointScale;
    }

    // The setter in the game clamps against a global maximum, so a very large
    // value here will be pulled back the next time the field is touched.
    bool setPoliticalPower(uint64_t countryAddress, int64_t value, std::string& error) {
        auto obj = politicalStatusObject(countryAddress);
        if (!obj) { error = "could not resolve the political status object"; return false; }

        const int64_t stored = value * offsets_.politicalPowerFixedPointScale;
        return process_.write<int64_t>(*obj + offsets_.politicalPower, stored, error);
    }

    // ------------------------------------------------------------ enumeration

    std::vector<CountryInfo> allCountries(int32_t maxTag = 400) const {
        std::vector<CountryInfo> result;

        for (int32_t tag = 1; tag <= maxTag; ++tag) {
            auto address = country(tag);
            if (!address) continue;

            auto count = process_.read<int32_t>(*address + offsets_.stateCount);
            if (!count || *count < 0 || *count > kMaxStates) continue;

            auto index = tagToIndex(tag);
            result.push_back({ tag, index ? *index : -1, *address, *count });
        }
        return result;
    }

    // ------------------------------------------------------------- divisions

    // The pointer unit_address cached the last time it ran. This is the anchor
    // the pool walk starts from - select a division in game, run the command
    // once, and everything below resolves without another manual step.
    std::optional<uint64_t> selectedUnit() const {
        auto unit = process_.read<uint64_t>(process_.imageBase()
                                            + offsets_.lastSelectedUnit);
        if (!unit || !mem::Process::plausiblePointer(*unit)) return std::nullopt;
        return *unit;
    }

    // A live slot reads 1 at +0x0C; a free one reads 0 there and at +0x08.
    // Maximum organisation and hit points are both positive on anything real,
    // which rules out a slot that carries the flag but nothing else.
    bool looksLikeDivision(uint64_t address) const {
        auto used = process_.read<int32_t>(address + offsets_.divisionSlotUsed);
        if (!used || *used != 1) return false;

        auto maxOrg = process_.read<int32_t>(address + offsets_.divisionMaxOrganisation);
        if (!maxOrg || *maxOrg <= 0) return false;

        auto hp = process_.read<int32_t>(address + offsets_.divisionHitPoints);
        if (!hp || *hp <= 0) return false;

        return true;
    }

    std::optional<Division> readDivision(uint64_t address, int slot) const {
        if (!looksLikeDivision(address)) return std::nullopt;

        const int64_t scale = offsets_.divisionFixedPointScale;

        auto hp     = process_.read<int32_t>(address + offsets_.divisionHitPoints);
        auto org    = process_.read<int32_t>(address + offsets_.divisionOrganisation);
        auto maxOrg = process_.read<int32_t>(address + offsets_.divisionMaxOrganisation);
        auto def    = process_.read<int32_t>(address + offsets_.divisionDefense);
        auto brk    = process_.read<int32_t>(address + offsets_.divisionBreakthrough);
        auto soft   = process_.read<int32_t>(address + offsets_.divisionSoftAttack);
        if (!hp || !org || !maxOrg) return std::nullopt;

        auto owner = process_.read<uint64_t>(address + offsets_.divisionOwnerCountry);

        Division d;
        d.address         = address;
        d.owner           = (owner && mem::Process::plausiblePointer(*owner)) ? *owner : 0;
        d.slot            = slot;
        d.hitPoints       = *hp     / scale;
        d.organisation    = *org    / scale;
        d.maxOrganisation = *maxOrg / scale;
        d.defense         = def  ? *def  / scale : 0;
        d.breakthrough    = brk  ? *brk  / scale : 0;
        d.softAttack      = soft ? *soft / scale : 0;
        return d;
    }

    // Walks the pool outwards from the anchor in both directions.
    //
    // The pool is sparse - a division that was disbanded leaves its slot
    // zeroed - so a single empty slot is not the end of the array. The walk
    // keeps going for divisionMaxGap empty slots before giving up in that
    // direction, which covers every gap seen in the mapped range.
    //
    // Nothing is cached. The allocator reuses freed blocks, and a division
    // that merges or dies between two calls would otherwise leave an address
    // that still reads plausibly.
    std::vector<Division> divisions() const {
        std::vector<Division> result;

        auto anchor = selectedUnit();
        if (!anchor) return result;
        if (!looksLikeDivision(*anchor)) return result;

        const int64_t stride = offsets_.divisionStride;

        // Downwards, then reverse, so the list comes back in address order.
        std::vector<Division> below;
        for (int gap = 0, step = 1;
             gap <= offsets_.divisionMaxGap
             && static_cast<int>(below.size()) < offsets_.divisionMaxCount;
             ++step) {

            const uint64_t at = *anchor - static_cast<uint64_t>(step) * stride;
            if (at > *anchor) break;              // wrapped past zero

            auto d = readDivision(at, -step);
            if (d) { below.push_back(*d); gap = 0; }
            else   { ++gap; }
        }
        std::reverse(below.begin(), below.end());
        result = std::move(below);

        auto self = readDivision(*anchor, 0);
        if (self) result.push_back(*self);

        for (int gap = 0, step = 1;
             gap <= offsets_.divisionMaxGap
             && static_cast<int>(result.size()) < offsets_.divisionMaxCount;
             ++step) {

            const uint64_t at = *anchor + static_cast<uint64_t>(step) * stride;

            auto d = readDivision(at, step);
            if (d) { result.push_back(*d); gap = 0; }
            else   { ++gap; }
        }

        return result;
    }

    // Every division the player owns, found without any help from the player.
    //
    // WHY A SCAN RATHER THAN A POOL WALK
    //
    // The walk above has to start from a division the player selected, which
    // means running unit_address in the console - and the console does not
    // exist in Ironman. It also assumes the pool holds one country's
    // divisions, which was never proven; on a big country with the front line
    // close by, walking blindly could reach the enemy's divisions just as
    // easily as your own.
    //
    // The division carries a pointer to its country at +0x820, so both
    // problems go away at once: search writable memory for the player's
    // country address, treat every hit as a candidate division at
    // hit - 0x820, and validate it. What comes back is exactly the player's
    // divisions, in any campaign, as any country, with the console closed.
    //
    // The cost is a pass over writable memory, which takes a moment - so the
    // freeze loop does this occasionally and re-validates cheaply in between.
    std::vector<Division> scanDivisionsOwnedBy(uint64_t countryAddress) const {
        std::vector<Division> result;
        std::set<uint64_t>    seen;

        if (!mem::Process::plausiblePointer(countryAddress)) return result;

        const int64_t ownerOffset = offsets_.divisionOwnerCountry;

        for (const mem::Region& region : process_.regions(/*writableOnly=*/true)) {
            if (region.size > kMaxRegionSize) continue;
            if (region.size < 8) continue;

            std::vector<uint8_t> buffer(static_cast<size_t>(region.size));

            // Chunked, because one partially failed read of a large region
            // would silently skip everything in it.
            constexpr size_t kChunk = 1024 * 1024;
            size_t done = 0;
            bool   ok   = true;
            while (done < buffer.size()) {
                const size_t take = std::min(kChunk, buffer.size() - done);
                if (!process_.readBytes(region.base + done,
                                        buffer.data() + done, take)) {
                    ok = false;
                    break;
                }
                done += take;
            }
            if (!ok) continue;

            for (size_t offset = 0; offset + 8 <= buffer.size(); offset += 8) {
                uint64_t word = 0;
                std::memcpy(&word, buffer.data() + offset, sizeof(word));
                if (word != countryAddress) continue;

                if (offset < static_cast<size_t>(ownerOffset)) continue;
                const uint64_t base = region.base + offset
                                    - static_cast<uint64_t>(ownerOffset);

                if (!seen.insert(base).second) continue;
                if (static_cast<int>(result.size()) >= offsets_.divisionMaxCount) break;

                // Cheap rejection out of the buffer before spending syscalls:
                // the country pointer turns up in plenty of places that are
                // not divisions.
                const size_t baseOffset = offset - static_cast<size_t>(ownerOffset);
                if (baseOffset + static_cast<size_t>(offsets_.divisionHitPointsCopy) + 4
                        <= buffer.size()) {

                    int32_t used = 0, hp = 0, maxOrg = 0;
                    std::memcpy(&used,
                                buffer.data() + baseOffset + offsets_.divisionSlotUsed, 4);
                    std::memcpy(&hp,
                                buffer.data() + baseOffset + offsets_.divisionHitPoints, 4);
                    std::memcpy(&maxOrg,
                                buffer.data() + baseOffset + offsets_.divisionMaxOrganisation, 4);

                    if (used != 1 || hp <= 0 || maxOrg <= 0) continue;
                }

                auto d = readDivision(base, static_cast<int>(result.size()));
                if (!d) continue;
                if (d->owner != countryAddress) continue;

                result.push_back(*d);
            }
        }

        std::sort(result.begin(), result.end(),
                  [](const Division& a, const Division& b) {
                      return a.address < b.address;
                  });

        for (size_t i = 0; i < result.size(); ++i)
            result[i].slot = static_cast<int>(i);

        return result;
    }

    // The player's divisions, resolved end to end - country from the static
    // chain, divisions from the country pointer they carry.
    std::vector<Division> playerDivisions() const {
        auto country = playerCountry();
        if (!country) return {};
        return scanDivisionsOwnedBy(*country);
    }

    // Cheap enough to run before every write: confirms the slot is still live
    // and still belongs to the same country. A division that was disbanded
    // between the scan and now fails here rather than being written into.
    bool divisionStillOwnedBy(uint64_t division, uint64_t countryAddress) const {
        if (!looksLikeDivision(division)) return false;

        auto owner = process_.read<uint64_t>(division + offsets_.divisionOwnerCountry);
        return owner && *owner == countryAddress;
    }

    // --- division writing ---
    //
    // Every one of these takes the value the way the UI shows it and applies
    // the scale, so 200 means an organisation of 200 rather than 0.002.
    //
    // None of them stick on their own. The game recomputes all of these - see
    // the note in hoi4_offsets.hpp - so a lone write moves the number in game
    // and is gone again within a tick or a day depending on the field. Use
    // DivisionFreeze to hold them.

    bool setDivisionField(uint64_t division, int64_t field, int64_t value,
                          std::string& error) {
        const int64_t stored = value * offsets_.divisionFixedPointScale;
        if (stored > INT32_MAX || stored < 0) {
            error = "value out of range for a 32-bit fixed point field";
            return false;
        }
        return process_.write<int32_t>(division + field,
                                       static_cast<int32_t>(stored), error);
    }

    bool setOrganisation(uint64_t division, int64_t value, std::string& error) {
        return setDivisionField(division, offsets_.divisionOrganisation, value, error);
    }

    bool setMaxOrganisation(uint64_t division, int64_t value, std::string& error) {
        return setDivisionField(division, offsets_.divisionMaxOrganisation, value, error);
    }

    // Both copies, since the UI reads one and parts of the game the other.
    bool setHitPoints(uint64_t division, int64_t value, std::string& error) {
        return setDivisionField(division, offsets_.divisionHitPoints, value, error)
            && setDivisionField(division, offsets_.divisionHitPointsCopy, value, error);
    }

    bool setCombatStats(uint64_t division, int64_t value, std::string& error) {
        return setDivisionField(division, offsets_.divisionDefense,      value, error)
            && setDivisionField(division, offsets_.divisionBreakthrough, value, error)
            && setDivisionField(division, offsets_.divisionSoftAttack,   value, error);
    }

    // Applies a whole godmode setting to one division. Re-checks the slot flag
    // first: between enumerating and writing, a division can be disbanded and
    // its block handed to something else.
    bool applyGodmode(uint64_t division, const DivisionGodmode& settings,
                      std::string& error) {
        if (!looksLikeDivision(division)) {
            error = "the division is no longer there";
            return false;
        }

        if (settings.maxOrganisation
            && !setMaxOrganisation(division, settings.organisationValue, error))
            return false;

        // After the maximum, so the current value is not clamped to the old one.
        if (settings.organisation
            && !setOrganisation(division, settings.organisationValue, error))
            return false;

        if (settings.hitPoints
            && !setHitPoints(division, settings.hitPointsValue, error))
            return false;

        if (settings.combatStats
            && !setCombatStats(division, settings.combatStatValue, error))
            return false;

        return true;
    }

    // One pass over every division. Returns how many were written; a division
    // that vanished mid-pass is skipped rather than treated as a failure.
    int applyGodmodeToAll(const DivisionGodmode& settings) {
        int written = 0;
        for (const Division& d : divisions()) {
            std::string ignored;
            if (applyGodmode(d.address, settings, ignored)) ++written;
        }
        return written;
    }

    // The same, over a list that was scanned earlier, re-checking ownership
    // rather than the slot flag alone. This is what the freeze loop calls: a
    // full scan every pass would be wasteful, but writing into a stale address
    // would be worse, so each one is confirmed to still be a live division
    // belonging to the same country immediately before it is written.
    int applyGodmodeToList(const std::vector<Division>& divisions,
                           uint64_t countryAddress,
                           const DivisionGodmode& settings) {
        int written = 0;
        for (const Division& d : divisions) {
            if (!divisionStillOwnedBy(d.address, countryAddress)) continue;

            std::string ignored;
            if (applyGodmode(d.address, settings, ignored)) ++written;
        }
        return written;
    }

    // Every division the player owns, scanned fresh and written in one go.
    int applyGodmodeToPlayer(const DivisionGodmode& settings, int* found = nullptr) {
        auto country = playerCountry();
        if (!country) { if (found) *found = 0; return 0; }

        auto divisions = scanDivisionsOwnedBy(*country);
        if (found) *found = static_cast<int>(divisions.size());

        return applyGodmodeToList(divisions, *country, settings);
    }

private:
    static constexpr int32_t kMaxStates    = 4096;
    // ------------------------------------------- equipment array location

    // An entry is a plausible archetype pointer, a small pair of counts, and
    // a quantity that is a whole number of units at the 100000 scale.
    bool looksLikeStockEntry(uint64_t address) const {
        auto archetype = process_.read<uint64_t>(address + offsets_.equipmentEntryArchetype);
        if (!archetype) return false;
        if (*archetype < offsets_.archetypeBandLow) return false;
        if (*archetype > offsets_.archetypeBandHigh) return false;
        if ((*archetype & 7) != 0) return false;

        auto quantity = process_.read<int64_t>(address + offsets_.equipmentEntryQuantity);
        if (!quantity) return false;
        if (*quantity < 0) return false;
        if (*quantity > kMaxStockValue) return false;
        if (*quantity % offsets_.equipmentFixedPointScale != 0) return false;

        return true;
    }

    // How many consecutive entries start here.
    int countStockEntries(uint64_t address) const {
        int found = 0;
        while (found < offsets_.equipmentMaxEntries) {
            const uint64_t at = address
                              + static_cast<uint64_t>(found) * offsets_.equipmentEntryStride;
            if (!looksLikeStockEntry(at)) break;
            ++found;
        }
        return found;
    }

    bool stockArrayStillValid(uint64_t address, int count) const {
        if (count < offsets_.equipmentMinEntries) return false;
        if (countStockEntries(address) < count) return false;

        auto archetype = process_.read<uint64_t>(address + offsets_.equipmentEntryArchetype);
        if (!archetype) return false;
        return looksLikeArchetypeName(archetypeName(*archetype));
    }

    std::vector<StockEntry> readStockArray(uint64_t address, int count) const {
        std::vector<StockEntry> result;
        result.reserve(static_cast<size_t>(count));

        for (int i = 0; i < count; ++i) {
            const uint64_t at = address
                              + static_cast<uint64_t>(i) * offsets_.equipmentEntryStride;

            auto archetype = process_.read<uint64_t>(at + offsets_.equipmentEntryArchetype);
            auto quantity  = process_.read<int64_t>(at + offsets_.equipmentEntryQuantity);
            if (!archetype || !quantity) continue;

            StockEntry e;
            e.entry     = at;
            e.archetype = *archetype;
            e.name      = archetypeName(*archetype);
            e.quantity  = *quantity / offsets_.equipmentFixedPointScale;
            e.index     = i;
            result.push_back(e);
        }
        return result;
    }

    // Walks writable memory looking for the longest run of entries.
    //
    // The whole run check happens inside the buffer that was already read, so
    // a region costs one read rather than thousands. Reading per candidate
    // instead made this take tens of seconds.
    static bool entryLooksRightInBuffer(const uint8_t* buffer, size_t size,
                                        size_t offset, const Offsets& offsets) {
        if (offset + 0x18 > size) return false;

        uint64_t archetype = 0;
        std::memcpy(&archetype, buffer + offset, sizeof(archetype));
        if (archetype < offsets.archetypeBandLow)  return false;
        if (archetype > offsets.archetypeBandHigh) return false;
        if ((archetype & 7) != 0) return false;

        int64_t quantity = 0;
        std::memcpy(&quantity, buffer + offset + 0x10, sizeof(quantity));
        if (quantity < 0) return false;
        if (quantity > kMaxStockValue) return false;
        if (quantity % offsets.equipmentFixedPointScale != 0) return false;

        return true;
    }

    static int countEntriesInBuffer(const uint8_t* buffer, size_t size,
                                    size_t offset, const Offsets& offsets) {
        int found = 0;
        while (found < offsets.equipmentMaxEntries) {
            const size_t at = offset
                            + static_cast<size_t>(found) * offsets.equipmentEntryStride;
            if (!entryLooksRightInBuffer(buffer, size, at, offsets)) break;
            ++found;
        }
        return found;
    }

    bool findStockArray(uint64_t& outAddress, int& outCount) const {
        uint64_t bestAddress = 0;
        int      bestCount   = 0;
        int      bestStocked = 0;

        for (const mem::Region& region : process_.regions(/*writableOnly=*/true)) {
            if (region.base < offsets_.archetypeBandLow)  continue;
            if (region.base > offsets_.archetypeBandHigh) continue;
            if (region.size > kMaxRegionSize) continue;

            std::vector<uint8_t> buffer(static_cast<size_t>(region.size));
            if (!process_.readBytes(region.base, buffer.data(), buffer.size()))
                continue;

            for (size_t offset = 0; offset + kMinRunBytes <= buffer.size(); offset += 8) {
                const int count = countEntriesInBuffer(buffer.data(), buffer.size(),
                                                       offset, offsets_);
                if (count < offsets_.equipmentMinEntries) continue;

                // Score by how many entries carry an actual amount. Zeroed
                // memory satisfies the shape check trivially and would
                // otherwise win on length alone.
                int stocked = 0;
                for (int i = 0; i < count; ++i) {
                    int64_t quantity = 0;
                    std::memcpy(&quantity,
                                buffer.data() + offset
                                    + static_cast<size_t>(i) * offsets_.equipmentEntryStride
                                    + 0x10,
                                sizeof(quantity));
                    if (quantity > 0) ++stocked;
                }
                if (stocked <= bestStocked) continue;

                // Only the real array points at archetypes carrying readable
                // script names. This is the one check worth a read into the
                // process, and by now it runs for a handful of candidates.
                uint64_t archetype = 0;
                std::memcpy(&archetype, buffer.data() + offset, sizeof(archetype));
                if (!looksLikeArchetypeName(archetypeName(archetype))) continue;

                bestStocked = stocked;
                bestCount   = count;
                bestAddress = region.base + offset;
            }
        }

        if (bestStocked < offsets_.equipmentMinEntries) return false;

        outAddress = bestAddress;
        outCount   = bestCount;
        return true;
    }

    // --------------------------------------------- variant array location

    static bool variantEntryLooksRight(const uint8_t* buffer, size_t size,
                                       size_t offset, const Offsets& offsets) {
        if (offset + 0x10 > size) return false;

        uint64_t variant = 0;
        std::memcpy(&variant, buffer + offset, sizeof(variant));
        if (variant < offsets.archetypeBandLow)  return false;
        if (variant > offsets.archetypeBandHigh) return false;
        if ((variant & 7) != 0) return false;

        int64_t quantity = 0;
        std::memcpy(&quantity, buffer + offset + 8, sizeof(quantity));
        if (quantity < 0) return false;
        if (quantity > kMaxStockValue) return false;
        if (quantity % offsets.equipmentFixedPointScale != 0) return false;

        return true;
    }

    static int countVariantEntries(const uint8_t* buffer, size_t size,
                                   size_t offset, const Offsets& offsets) {
        int found = 0;
        while (found < offsets.equipmentMaxEntries) {
            const size_t at = offset
                            + static_cast<size_t>(found) * offsets.variantEntryStride;
            if (!variantEntryLooksRight(buffer, size, at, offsets)) break;
            ++found;
        }
        return found;
    }

    int countVariantEntriesLive(uint64_t address) const {
        int found = 0;
        while (found < offsets_.equipmentMaxEntries) {
            const uint64_t at = address
                              + static_cast<uint64_t>(found) * offsets_.variantEntryStride;

            auto variant  = process_.read<uint64_t>(at + offsets_.variantEntryVariant);
            auto quantity = process_.read<int64_t>(at + offsets_.variantEntryQuantity);
            if (!variant || !quantity) break;
            if (*variant < offsets_.archetypeBandLow)  break;
            if (*variant > offsets_.archetypeBandHigh) break;
            if (*quantity < 0 || *quantity > kMaxStockValue) break;
            if (*quantity % offsets_.equipmentFixedPointScale != 0) break;
            ++found;
        }
        return found;
    }

    bool variantArrayStillValid(uint64_t address, int count) const {
        if (count < 1) return false;
        if (countVariantEntriesLive(address) < count) return false;

        auto variant = process_.read<uint64_t>(address + offsets_.variantEntryVariant);
        if (!variant) return false;
        return looksLikeVariantName(variantName(*variant));
    }

    std::vector<VariantEntry> readVariantArray(uint64_t address, int count) const {
        std::vector<VariantEntry> result;
        result.reserve(static_cast<size_t>(count));

        for (int i = 0; i < count; ++i) {
            const uint64_t at = address
                              + static_cast<uint64_t>(i) * offsets_.variantEntryStride;

            auto variant  = process_.read<uint64_t>(at + offsets_.variantEntryVariant);
            auto quantity = process_.read<int64_t>(at + offsets_.variantEntryQuantity);
            if (!variant || !quantity) continue;

            VariantEntry e;
            e.entry    = at;
            e.variant  = *variant;
            e.name     = variantName(*variant);
            e.quantity = *quantity / offsets_.equipmentFixedPointScale;
            e.index    = i;
            result.push_back(e);
        }
        return result;
    }

    // Every distinct run in writable memory that looks like a variant array
    // with at least one stocked entry and a readable first name. Overlapping
    // runs are collapsed to the longest one so a single array does not show
    // up dozens of times at successive offsets.
    std::vector<std::pair<uint64_t, int>> findVariantArrays() const {
        std::vector<std::pair<uint64_t, int>> found;

        for (const mem::Region& region : process_.regions(/*writableOnly=*/true)) {
            if (region.size > kMaxRegionSize) continue;

            std::vector<uint8_t> buffer(static_cast<size_t>(region.size));
            if (!process_.readBytes(region.base, buffer.data(), buffer.size()))
                continue;

            size_t offset = 0;
            while (offset + 0x40 <= buffer.size()) {
                const int count = countVariantEntries(buffer.data(), buffer.size(),
                                                      offset, offsets_);
                if (count < kMinVariantEntries) { offset += 8; continue; }

                int stocked = 0;
                for (int i = 0; i < count; ++i) {
                    int64_t quantity = 0;
                    std::memcpy(&quantity,
                                buffer.data() + offset
                                    + static_cast<size_t>(i) * offsets_.variantEntryStride
                                    + 8,
                                sizeof(quantity));
                    if (quantity > 0) ++stocked;
                }

                uint64_t variant = 0;
                std::memcpy(&variant, buffer.data() + offset, sizeof(variant));

                if (stocked >= 1 && looksLikeVariantName(variantName(variant))) {
                    found.emplace_back(region.base + offset, count);

                    // Skip past this run so its tail does not register as a
                    // shorter array of its own.
                    offset += static_cast<size_t>(count) * offsets_.variantEntryStride;
                    continue;
                }

                offset += 8;
            }

        }

        return found;
    }

    static constexpr int    kMinVariantEntries = 3;

    mutable uint64_t cachedVariantArray_ = 0;
    mutable int      cachedVariantCount_ = 0;

    mutable uint64_t cachedStockArray_ = 0;
    mutable int      cachedStockCount_ = 0;

    static constexpr int64_t kMaxStockValue  = 100000LL * 100000000LL;
    static constexpr size_t  kMinRunBytes    = 0x18 * 4;
    static constexpr uint64_t kMaxRegionSize = 512ULL * 1024 * 1024;

    static constexpr int32_t kMaxBuildings     = 256;
    static constexpr size_t  kMaxTreeNodes     = 4096;
    static constexpr int32_t kMaxStockEntries  = 2048;
    static constexpr int16_t kMaxBuildingLevel = 1000;

    static int32_t clamp32(int64_t v) {
        if (v < 0)         return 0;
        if (v > INT32_MAX) return INT32_MAX;
        return static_cast<int32_t>(v);
    }

    mem::Process& process_;
    Offsets       offsets_;
    Functions     functions_;
};

// ---------------------------------------------------------------- freeze

// Holds division stats by rewriting them on a timer.
//
// WHY THIS EXISTS
//
// Nothing in the division object is authoritative. Organisation is forced back
// within a tick while a division is exercising, maximum organisation reverts
// at a day boundary, and the combat stats follow the equipment level. A single
// write shows up in game and then goes away.
//
// The saving grace is that the game is slow about it. Rewriting every couple
// of hundred milliseconds wins the race comfortably - unlike the country
// resource container, where the game managed 13 overwrites a second and a 2 ms
// loop still lost.
//
// WHY IT RE-ENUMERATES EVERY PASS
//
// Divisions are disbanded, merged and destroyed far more often than the
// objects the rest of this SDK touches, and the allocator hands the block to
// something else. Holding a list of addresses across passes would mean writing
// into whatever moved in. So each pass walks the pool again from the anchor,
// and applyGodmode re-checks the slot flag immediately before writing.
//
// The anchor is whatever unit_address last selected. If the player never ran
// it, or the anchored division dies, the pass finds nothing and the loop keeps
// going - it will pick up again as soon as there is a valid anchor.
class DivisionFreeze {
public:
    struct Status {
        bool     running        = false;
        uint64_t passes         = 0;
        int      lastCount      = 0;   // divisions written on the last pass
        uint64_t emptyPasses    = 0;   // passes that found no divisions at all
        int      knownCount     = 0;   // divisions the last scan turned up
        uint64_t scans          = 0;   // full memory scans done so far
    };

    explicit DivisionFreeze(Game& game) : game_(game) {}

    ~DivisionFreeze() { stop(); }

    DivisionFreeze(const DivisionFreeze&)            = delete;
    DivisionFreeze& operator=(const DivisionFreeze&) = delete;

    bool running() const { return running_.load(); }

    DivisionGodmode settings() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return settings_;
    }

    // Safe to call while running - the next pass picks up the new values.
    void setSettings(const DivisionGodmode& settings) {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_ = settings;
    }

    Status status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Status s = status_;
        s.running = running_.load();
        return s;
    }

    bool start(const DivisionGodmode& settings, std::string& error) {
        if (running_.load()) { error = "already running"; return false; }

        if (!settings.anythingOn()) {
            error = "nothing is turned on";
            return false;
        }

        // Fail early rather than spinning against nothing. This is also the
        // first full scan, so a successful start means the divisions were
        // actually found rather than merely looked for.
        auto country = game_.playerCountry();
        if (!country) {
            error = "could not resolve the player country - is a campaign loaded?";
            return false;
        }

        auto divisions = game_.scanDivisionsOwnedBy(*country);
        if (divisions.empty()) {
            error = "no divisions found for the player country";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            settings_          = settings;
            status_            = Status{};
            status_.knownCount = static_cast<int>(divisions.size());
            status_.scans      = 1;
            country_           = *country;
            divisions_         = std::move(divisions);
        }

        running_.store(true);
        worker_ = std::thread(&DivisionFreeze::loop, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
    }

private:
    void loop() {
        uint64_t sinceScan = 0;

        while (running_.load()) {
            DivisionGodmode       settings;
            std::vector<Division> divisions;
            uint64_t              country = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                settings   = settings_;
                divisions  = divisions_;
                country    = country_;
            }

            // A full scan reads all of writable memory, so it happens on a
            // slow cadence - often enough to pick up newly trained divisions
            // and to recover if the country object moved, rarely enough not
            // to cost anything noticeable. Between scans, every address is
            // still re-validated before it is written.
            const uint64_t scanEvery =
                static_cast<uint64_t>(kRescanSeconds * 1000
                                      / std::max(1, settings.intervalMs));

            if (sinceScan >= scanEvery || divisions.empty() || country == 0) {
                auto fresh = game_.playerCountry();
                if (fresh) {
                    auto found = game_.scanDivisionsOwnedBy(*fresh);
                    std::lock_guard<std::mutex> lock(mutex_);
                    country_           = *fresh;
                    divisions_         = found;
                    status_.knownCount = static_cast<int>(found.size());
                    ++status_.scans;
                    divisions = std::move(found);
                    country   = *fresh;
                }
                sinceScan = 0;
            }
            ++sinceScan;

            const int written = (country && !divisions.empty())
                              ? game_.applyGodmodeToList(divisions, country, settings)
                              : 0;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.passes;
                status_.lastCount = written;
                if (written == 0) ++status_.emptyPasses;
            }

            // Sleep in small slices so stop() does not have to wait out a
            // whole interval.
            const int slice = 20;
            for (int slept = 0;
                 slept < settings.intervalMs && running_.load();
                 slept += slice) {
                std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            }
        }
    }

    // How long between full memory scans. Everything in between is a cheap
    // re-validation of addresses already known.
    static constexpr int kRescanSeconds = 10;

    Game&                 game_;
    std::thread           worker_;
    std::atomic<bool>     running_{false};
    mutable std::mutex    mutex_;
    DivisionGodmode       settings_;
    Status                status_;
    uint64_t              country_ = 0;
    std::vector<Division> divisions_;
};

} // namespace hoi4
