// hoi4_offsets.hpp - Hearts of Iron IV offsets (macOS, x86_64)
//
// ============================================================================
//  SCOPE
// ============================================================================
//  Every offset below was derived by reading disassembly or confirmed by
//  in-game verification - none were ported from a Windows table. Each entry
//  lists where it came from so it can be re-derived after a game update.
//
//  Platform : macOS, x86_64 binary (runs under Rosetta 2 on Apple Silicon)
//  Observed ASLR slide: 0. Do not rely on it - compute it.
//
//  Static addresses assume the image is linked at 0x100000000.
//
// ============================================================================
//  STRUCTURE SUMMARY
// ============================================================================
//
//  imageBase + 0x3501220        -> GameState*
//
//  GameState + 0x4D8            int32   player country tag (used if > 0)
//  GameState + 0x4DC            int32   fallback tag
//  GameState + 0x308            -> int32[]    tag -> internal index table
//  GameState + 0x2D8            -> Country*[] country array
//
//  Country   + 0x2E8            CManpower    country-level manpower object
//  Country   + 0x420            -> State*[]  owned states
//  Country   + 0x42C            int32   number of owned states
//  Country   + 0xD30            -> Diplomacy*
//  Country   + 0xF80            -> resource object (derived totals)
//
//  State     + 0x1E4 + j*16     int32   BASE resource amount, slot j  <-- writable
//  State     + 0x1E8 + j*16     int32   resource id, slot j
//  State     + 0x7B0            CStateManpower
//
//  CStateManpower + 0x10        int32   available manpower
//  CStateManpower + 0x18        int32   recruitable population
//
// ============================================================================
//  WHAT IS AND IS NOT WRITABLE
// ============================================================================
//  Almost nothing the UI displays is stored. The game derives most values
//  every tick. Two consequences:
//
//  1. There is NO country-level manpower total. The top bar sums
//     CStateManpower+0x10 across owned states. Value-scanning for it never
//     produces a hit.
//
//  2. The resource figures in the Trade tab (Extracted / Imported / Exported /
//     Projects / Production / Surplus) are recalculated each tick from the
//     per-state BASE amounts. Writing them holds for a fraction of a second
//     and is then overwritten - measured at roughly 13 overwrites per second
//     even with a 2 ms rewrite loop. Readable, not writable.
//
//     The per-state BASE value at State+0x1E4+j*16 IS persistent. It changes
//     only when buildings are built or territory changes hands, so a single
//     write survives indefinitely and propagates through every derived
//     figure. This is the field to write.
//
//  Multiplayer uses deterministic lockstep. Memory writes desync the session
//  rather than granting an advantage. Single-player only.
//
// ============================================================================

#pragma once

#include <cstdint>

namespace hoi4 {

// ---------------------------------------------------------------- functions

// Static addresses of functions identified during reversing. Useful as
// breakpoint targets when re-deriving offsets after a patch.
struct Functions {
    // Console command handler for "manpower". Found via the command
    // registration table, which stores the command name, its description
    // ("Adds manpower to player") and the handler pointer together.
    uint64_t manpowerCommandHandler = 0x100150650;

    // CManpower setter. Source file confirmed by an embedded assert path:
    //   .../hoi4/source/manpower.cpp
    // Signature: (CManpower* self, int32 amount, int32 exileFlag)
    uint64_t manpowerSetter = 0x1007B6040;

    // Distributes an amount across the country's states.
    // Signature: (Country+0x420, int32 amount, int32 exileFlag)
    uint64_t manpowerDistributor = 0x1007B6640;

    // Adds to a single state's manpower, clamped by a cap derived from
    // recruitable population. Returns how much was actually added.
    uint64_t stateManpowerAdd = 0x100989F20;

    // Removes from a single state's manpower. Returns how much was taken.
    uint64_t stateManpowerTake = 0x100989F80;

    // Tag -> Country* lookup. Double indirection:
    //   index = [[GameState + 0x308] + tag * 4]
    //   return  [[GameState + 0x2D8] + index * 8]
    uint64_t countryFromTag = 0x1011D7110;

    // GetCountry() applied to a tag pointer.
    uint64_t getCountry = 0x1011D6FB0;

    // Scripted variable getter for resource_produced. Reads
    //   [[GetCountry(scope) + 0xF80] + 0x30] [resourceIndex * 2]
    // which is how the resource container layout was found.
    uint64_t resourceProducedGetter = 0x1001D4EA0;

    // Strategic resource database loader (strategic_resource_database.cpp).
    // Each definition is 0xF0 bytes and carries its index at +0xB8.
    uint64_t resourceDatabaseLoader = 0x100A2CFC0;
};

// ------------------------------------------------------------------ offsets

struct Offsets {
    // --- static ---

    // Global holding the GameState pointer.
    // Derived from: lea 0x232d435(%rip), %r13 ; mov (%r13), %r12
    uint64_t gameStatePointer = 0x3519740;

    // --- GameState ---

    // Player country tag. The handler prefers +0x4D8 when positive:
    //   r15 = gs + 0x4DC
    //   if (*(gs + 0x4D8) > 0) r15 = gs + 0x4D8
    int64_t playerTagPrimary = 0x4D8;
    int64_t playerTagFallback = 0x4DC;

    // Tag -> internal index table, 4 bytes per entry.
    // Derived from: mov 0x308(%rax), %rax ; mov (%rax,%rcx,4), %eax
    int64_t tagIndexTable = 0x308;

    // Country pointer array, 8 bytes per entry, indexed by internal index.
    // Derived from: mov 0x2d8(%r12), %rcx ; mov (%rcx,%rax,8), %rax
    int64_t countryArray = 0x2D8;

    // Doctrine Manager / Country Doctrine storage
    // GameState + 0x3C0 -> CDoctrineManager pointer (container of country doctrine trees & mastery)
    int64_t doctrineManagerOffset = 0x3C0;

    // --- Country ---

    // Country tag string (3 letters e.g. "ROM", "GER", "ENG", "SOV")
    // libc++ std::string at Country + 0x10, returned by sub_1011772d0
    int64_t countryTagString = 0x10;

    // Country name / localized token string (e.g. "Romania", "Germany")
    // libc++ std::string at Country + 0x40, returned by sub_1011772e0
    int64_t countryNameString = 0x40;

    // Country-level CManpower sub-object, passed as `this` to manpowerSetter.
    int64_t countryManpowerObject = 0x2E8;

    // Owned state array and its element count.
    // Derived from: mov (%r15), %rax ; mov (%rax,%r14,8), %rdi
    //               mov 0xc(%r15), %r13d      where r15 = Country + 0x420
    int64_t stateArray = 0x420;
    int64_t stateCount = 0x42C;

    // Controlled / occupied state array (Country + 0x438)
    int64_t controlledStateArray = 0x438;
    int64_t controlledStateCount = 0x444;

    // Diplomacy object, used by the exile check.
    int64_t diplomacyObject = 0xD30;

    // Derived resource totals. Read-only in practice - see the note above.
    // Layout: [Country + 0xF80] + 0x30 -> buffer
    //           buffer + i*16     int64  amount, fixed point, divide by 100000
    //           buffer + i*16 + 8 int64  resource id
    // Five consecutive containers, stride 0xC0, in Trade tab order:
    //   Extracted, Imported, Exported, Projects, Production
    // Surplus = Extracted + Imported - Exported - Production.
    int64_t resourceObject = 0xF80;
    int64_t resourceBuffer = 0x30;
    int64_t resourceContainerStride = 0xC0;
    int64_t resourceFixedPointScale = 100000;

    // --- State ---

    // BASE resource amounts, per state, one entry per resource slot. This is
    // the value shown as "Base:" in a state's resource tooltip, before state
    // modifiers, infrastructure and supply-route multipliers.
    //
    // Confirmed structurally: resource id 19999 sits at State+0x1F8 in all 24
    // states of a test country, and id 20023 at State+0x1E8 - a stride of 16
    // with the value 4 bytes ahead of the id.
    //
    // Confirmed behaviourally: writing every slot in every state changes all
    // seven figures in the Logistics bar simultaneously, and the change
    // survives multiple in-game days.
    int64_t stateResourceValue = 0x1E4;   // int32, writable, persistent
    int64_t stateResourceId = 0x1E8;   // int32
    int64_t stateResourceStride = 16;

    // The two containers do NOT share slot numbering:
    //
    //  - State container (0x1E4/0x1E8): slot 0 is a real resource. Verified
    //    on two different countries - slot 0 carries id 20023 and slot 1
    //    carries id 19999 in both. Iterate from 0.
    //
    //  - Country container (0xF80 -> 0x30): slot 0 is a sentinel carrying
    //    id 357 with a value of 0. Iterate from 1.
    //
    // Reading the state container from slot 1 silently drops one resource and
    // shifts every name by one position.
    int resourceSlotCount        = 8;
    int firstStateResourceSlot   = 0;
    int firstDerivedResourceSlot = 1;

    // --- State buildings ---

    // Container of buildings inside each state object. Reached through a
    // lookup, not a flat array - the layout was taken from the scripted
    // variable getter for building_level (sub_1011F91E0) and the two accessors
    // it calls:
    //
    //   sub_100D280D0(out, buildingDefinition, state):
    //       if (lookup(state + 0x110, def)) out = entryFor(state + 0x110, def)
    //
    //   sub_100D2CAC0(container, def):
    //       idx = *(*(container + 0x20) + def->id * 4)
    //       if (idx != -1) return *(*(container + 0x38) + idx * 8)
    //
    //   sub_100D27B80(accessor): return *(int16*)(entry + 0x40)
    //
    // The getter then multiplies by 0x186A0 (100000) for the fixed-point
    // scripted value, so the stored level is the raw number - 3, 5, 10.
    int64_t stateBuildingContainer = 0x110;   // State + this
    int64_t buildingIndexTable = 0x20;    // container + this -> int32[]
    int64_t buildingCount = 0x2C;    // container + this -> int32 (56)
    int64_t buildingEntryArray = 0x38;    // container + this -> entry*[]
    int64_t buildingLevel = 0x40;    // entry + this -> int16, writable
    int64_t buildingHealthyLevel   = 0x42;    // entry + this -> int16, operational/healthy level (matches level for 100% health)
    int64_t buildingDamage         = 0x48;    // entry + this -> int64, damage counter (0 = full repair)
    int64_t buildingHealthRatio    = 0x50;    // entry + this -> int64, health factor (100000 = 100% full health)
    int64_t buildingActive         = 0x8C;    // entry + this -> uint8, active flag (1 = active)

    // --- GameState global state array ---

    // Every state in the game, indexed directly by state id. Found in the
    // building_level getter:
    //   if (id > 0 && *(GameState + 0x29C) > id)
    //       state = *(*(GameState + 0x290) + id * 8)
    // Count reads 1082 on a vanilla map, which matches the state count.
    int64_t globalStateArray = 0x290;
    int64_t globalStateCount = 0x29C;

    // --- CStateManpower ---

    // Sub-object inside each state. Every state loop uses the literal 0x7B0.
    int64_t stateManpowerObject = 0x7B0;

    // Available manpower. Derived from stateManpowerTake:
    //   mov 0x10(%rdi), %ecx ; sub %eax, %ecx ; mov %ecx, 0x10(%rdi)
    int64_t stateManpower = 0x10;

    // Recruitable population. The add path computes a cap from it, so raising
    // this raises the manpower ceiling.
    int64_t statePopulation = 0x18;

    // --- Experience (army / navy / air) ---
    //
    // XP does not live in the country object. The `xp` console handler
    // (sub_1001591A0) resolves it as
    //
    //   obj = sub_1011D4960(tag)
    //       = sub_10118C210(sub_1011D3D20(tag))
    //       = *(Country + 0x12E8)
    //
    // and then calls three separate setters on that one object:
    //
    //   sub_1001AF670(obj, &xp, ...)   *(obj + 0x40) += xp   army
    //   sub_1001AF470(obj, &xp, ...)   *(obj + 0x28) += xp   navy
    //   sub_1001AF270(obj, &xp, ...)   *(obj + 0x10) += xp   air
    //
    // The console argument is shifted left by 15 before it reaches them
    // (`shlq $0xf, %r12`), so the stored numbers are fixed point with a scale
    // of 32768 - NOT the 100000 used by resources and political power.
    //
    // Confirmed live: breaking on the army setter with `xp 1000` and stepping
    // out left 32768000 at obj+0x48 and 16384000 at obj+0x40. Both fields are
    // written on the positive path, so which one the top bar reads has not
    // been pinned down - write 0x40 first and check the bar before trusting
    // it.
    int64_t experienceObject = 0x12E8;   // Country + this -> XP object
    int64_t airExperience    = 0x10;     // int64, /32768
    int64_t navyExperience   = 0x28;     // int64, /32768
    int64_t armyExperience   = 0x40;     // int64, /32768
    int64_t armyExperienceAccumulator = 0x48;  // int64, also written
    int64_t experienceFixedPointScale = 32768;

    // --- Equipment stockpile (found by pattern, not by chain) ---
    //
    // Four separate static routes were followed for this and each landed in a
    // different part of the equipment system - the UI cache, a temporary
    // container, the production lines, and the convoy stockpile. The array
    // that actually backs the Logistics screen was found by value scanning
    // and confirmed by writing to it.
    //
    // Its layout, read off a live process and matched against the Logistics
    // screen entry by entry:
    //
    //   entry + 0x00   -> archetype pointer
    //   entry + 0x08   int32 variant count, int32 index
    //   entry + 0x10   int64 quantity, /100000
    //   stride 0x18
    //
    // Confirmed on a German save: 9,300,000 -> 93 Artillery,
    // 652,000,000 -> 6520 Infantry Eq., 24,400,000 -> 244 Truck, all matching
    // the screen exactly. Writing the quantity field moves the number in game
    // and it sticks.
    //
    // What is NOT known is what owns the array. It does not hang off the
    // country at any offset that was tried, and scanning for a pointer TO it
    // returns nothing - so it is likely referenced through a packed or
    // tagged pointer. Until that is resolved the SDK locates the array by
    // scanning for the pattern above, which costs a fraction of a second at
    // startup and survives restarts.
    // The archetype carries its name as a libc++ std::string at +0x10. Short
    // names live inline: the first byte holds (length << 1) and the text
    // starts right after it. Long names set bit 0 of that byte and keep the
    // text on the heap, with size at +0x18 and the pointer at +0x20.
    //
    // Read off a live archetype: byte 0x24 at +0x10 (= length 18) followed by
    // "infantry_equipment".
    int64_t archetypeName        = 0x10;
    int64_t archetypeNameInline  = 0x11;   // text, when short
    int64_t archetypeNameSize    = 0x18;   // size_t, when long
    int64_t archetypeNamePointer = 0x20;   // char*, when long
    int     archetypeNameMax     = 64;

    // The variant array. Separate from the archetype array above: writing an
    // archetype total does NOT distribute down to variants, and the Stockpile
    // panel lists variants, so this is what to touch for a specific design.
    //
    //   entry + 0x00   -> variant pointer
    //   entry + 0x08   int64 quantity, /100000
    //   stride 0x10
    //
    // Confirmed against the Stockpile panel of a German save: 57,600,000 ->
    // 576 Panzer I Ausf. A, 11,400,000 -> 114 Panzer II, 7,800,000 -> 78 for
    // the Panzer II actually in production.
    //
    // Variants carry their display name the same way archetypes carry their
    // token - a libc++ short string - but at +0x28 rather than +0x10, and the
    // text is the readable name ("Panzer I Ausf. A"), not a script id.
    int64_t variantEntryVariant  = 0x00;
    int64_t variantEntryQuantity = 0x08;
    int64_t variantEntryStride   = 0x10;

    // The `add_latest_equipment` handler (sub_10015D3B0) walks the variant
    // database and only touches variants whose flag word at +0x3F8 has bit 0
    // set - that is what "latest" means: the designs currently available to
    // build, as opposed to superseded ones still sitting in stock.
    //
    //   for each variant v in the database:
    //       if (*(v + 0x3F8) & 1)
    //           addToStock(country, v, amount * 100000)
    int64_t variantActiveFlags   = 0x3F8;

    // Only variants defined explicitly in the files - Panzer I Ausf. A, Bf 109
    // D - carry a name of their own at variantName. The default designs leave
    // it empty, and the game shows them under their equipment's token
    // instead. This is the way there: variant + 0x3E0 points at the equipment
    // object, which holds its token at +0x10 in the usual short-string form.
    //
    // Verified live: an unnamed variant's +0x3E0 led to an object reading
    // "infantry_equipment_1".
    // Production cost, and the one place it can safely be written.
    //
    // A production line keeps its own copy at line + 0x28, which is what the
    // Logistics widget divides by. Writing that copy desyncs the line from
    // its source and the game faults on the next read - confirmed the hard
    // way, with a corrupted vtable and EXC_BAD_ACCESS on a virtual call.
    //
    // The source is here, on the variant. A read watchpoint on it stopped
    // with rdi holding the production line, so the line reads through to this
    // field rather than the other way round. Writing it works and the build
    // time follows.
    int64_t variantCost          = 0x358;

    int64_t variantEquipment     = 0x3E0;
    int64_t variantEquipmentName = 0x10;

    // libc++ stores a string one of two ways, and which one is in the low bit
    // of the first byte. Short: that byte holds (length << 1) and the text
    // follows it. Long: the bit is set, and the real length and a pointer to
    // the text sit at +0x08 and +0x10. Anything over 22 characters takes the
    // long form, which is why "ballistic_missile_equipment_2" and its like
    // came back blank while shorter tokens read fine.
    int64_t stringLongSize       = 0x08;
    int64_t stringLongPointer    = 0x10;
    int     stringMaxLength      = 256;

    int64_t variantName          = 0x28;
    int64_t variantNameInline    = 0x29;
    int64_t variantNameSize      = 0x30;
    int64_t variantNamePointer   = 0x38;

    int64_t equipmentEntryArchetype = 0x00;
    int64_t equipmentEntryCounts    = 0x08;
    int64_t equipmentEntryQuantity  = 0x10;
    int64_t equipmentEntryStride    = 0x18;

    // Archetype pointers all sit in the same high band. Used to recognise a
    // real entry while scanning.
    uint64_t archetypeBandLow  = 0x00007FF400000000ULL;
    uint64_t archetypeBandHigh = 0x00007FFF00000000ULL;

    // A country has at least this many equipment types once it is producing
    // anything, and never anywhere near the upper bound.
    int equipmentMinEntries = 4;
    int equipmentMaxEntries = 64;

    // --- Convoy stockpile ---
    //
    // From the `add_latest_equipment` handler (sub_10015D3B0), which walks
    // the variant database and calls a stock setter per active variant:
    //
    //   db  = *(Country + 0xD10)
    //   arr = *(db + 0xB8), cnt = *(db + 0xC4)
    //   for each variant v with (*(v + 0x3F8) & 1):
    //       sub_1001ABDE0(Country + 0xF88, v, amount * 0x186A0)
    //
    // sub_1001ABDE0 forwards to sub_10113D190(container + 0x10, ...), which
    // is where the actual storage lives. That function is a sorted container
    // with insertion, 1.5x growth and SIMD index fixup - reproducing the
    // INSERT path is not worth the risk, but overwriting an existing entry
    // is a single write.
    //
    // Layout of the container (Country + 0xF88 + 0x10):
    //
    //   + 0x08   -> archetype group array, stride 0x18
    //       group + 0x00   -> archetype pointer
    //       group + 0x08   int32  first variant index
    //       group + 0x0C   int32  variant count in this group
    //       group + 0x10   int64  total for the archetype
    //   + 0x14   int32  number of groups
    //   + 0x20   -> variant entry array, stride 0x10
    //       entry + 0x00   -> variant pointer
    //       entry + 0x08   int64  quantity        <- writable
    //   + 0x2C   int32  number of variant entries
    //
    // Values are clamped to +/- 0x53E2D620FB40, so there is no practical
    // ceiling. Negative values are accepted by this code but the rest of the
    // game assumes positive stock.
    //
    // A variant only gets an entry once the country has actually held one of
    // it. Produce a single unit of a custom variant and it appears here.
    // Confirmed by breaking on the call the handler makes:
    //
    //   0x10015D5EA  callq sub_1001ABDE0(%r14, %r12, %rax)
    //     r13 = 0x7FB5F6A1AC00   the country
    //     r14 = 0x7FB5F6A1BB88   the container   -> difference is exactly 0xF88
    //
    // An early read of this container looked like it only held convoys, which
    // sent this down a long detour through value scanning. It did not - that
    // read was of a different country, and the container is the general one.
    // The setter's assert string mentions convoys.cpp because the function is
    // shared, not because it is convoy-only.
    // --- Production lines ---
    //
    // The Logistics widget is handed its line container directly:
    //
    //   sub_10212A790(widget, *(Country + 0xD10) + 0x58)
    //
    // and inside, the per-line weekly output is computed as
    //
    //   output = (*(*line + 0x78))(line) * 0xAAE60 / line[5]
    //
    // 0xAAE60 is 700000 - seven days at the usual 100000 scale - and line[5]
    // is line + 0x28, the divisor. That divisor is the production cost, so
    // lowering it raises output in direct proportion.
    //
    // The displayed efficiency is not a field: sub_1013BE450 averages a ring
    // buffer at line + 0xC0 over line + 0x14 samples, and the widget then
    // averages that across lines into its own + 0x430. Writing the buffer
    // works but the game overwrites it as new samples arrive, which is why
    // the cost is the better handle.
    // The naval side of production, next door to the variant database. Seen
    // in the instantshiprefit path (sub_100A65000), which reaches for
    // *(country + 0xD18) + 0x20 and passes *(country + 0xD18) to
    // sub_1013D2F40, an accessor returning that object + 0xF8.
    //
    // The line list inside it is not mapped yet - it is being dumped live to
    // find which field is the build progress.
    int64_t navalBase = 0xD18;   // Country + this

    int64_t productionBase = 0xD10;   // Country + this
    int64_t productionLines      = 0x58;    // + this -> line container
    int64_t productionLineArray  = 0x00;
    int64_t productionLineCount  = 0x0C;    // int32
    int64_t productionLineCost   = 0x28;    // int64, writable, do not touch
    int64_t productionLineVariant = 0x88;   // -> the design being built
    int     productionLineMax    = 256;

    // Traced through add_equipment, which works in game and so shows the real
    // path. Its handler picks between two containers:
    //
    //   if (sub_10111D910(*(variant + 0x3E0)) == 0)
    //       sub_10140D3D0(*(country + 0xD10), variant, amount)   general
    //   else if (*(variant + 0x3F8) & 1)
    //       sub_1001ABDC0(country + 0xF88, variant, amount)      naval
    //
    // and sub_10140D3D0 is just
    //
    //   sub_10113D190(base + 0x200, variant, amount * 0x186A0)
    //
    // So +0xF88 is the naval container - which is why reading it only ever
    // showed convoys - and the general one hangs off the variant database at
    // +0xD10 instead. Confirmed live: rdi = 0x7F899336AA00 at the call, rdx =
    // 0x1388 for "add_equipment 5000 ...", and +0x200 held a group array
    // whose amounts matched the Logistics rows.
    int64_t equipmentStockObject = 0xD10;   // Country + this
    int64_t equipmentStockInner  = 0x200;   // + this -> the container proper
    // Each group, read live out of the general container:
    //
    //   group + 0x00   -> archetype
    //   group + 0x08   int32 count, int32 index
    //   group + 0x10   int64 amount held, /100000
    //   stride 0x18
    int64_t stockGroupArray      = 0x08;
    int64_t stockGroupCount      = 0x14;    // int32
    int64_t stockGroupStride     = 0x18;
    int64_t stockGroupArchetype  = 0x00;    // pointer
    int64_t stockGroupFirst      = 0x08;    // int32
    int64_t stockGroupSize       = 0x0C;    // int32
    int64_t stockGroupTotal      = 0x10;    // int64
    // The same container holds a second array, for variants rather than
    // archetypes. Writing an archetype total shows up in Logistics but not in
    // Stockpile, and divisions still report missing equipment, because what
    // they draw on is the variant entry - so this is the one to write for
    // equipment that can actually be used.
    //
    // Live read of the container: +0x20 -> array, +0x2C -> 4 entries, which
    // matched the four designs the country had.
    int64_t stockEntryArray      = 0x20;
    int64_t stockEntryCount      = 0x2C;    // int32
    // The two arrays have different shapes, which is what made an earlier
    // attempt write over variant pointers and crash the game. From
    // sub_10113D190, the setter add_equipment ends up in:
    //
    //   archetype array (+0x08), stride 0x18
    //     +0x00  -> archetype
    //     +0x08  int32 index into the variant array
    //     +0x0C  int32 how many variants
    //     +0x10  int64 total
    //
    //   variant array (+0x20), stride 0x10
    //     +0x00  -> variant
    //     +0x08  int64 amount
    //
    // and it updates both: the archetype total at (i*3 << 3) + 0x10, and the
    // variant amount at (j << 4) + 8.
    int64_t stockEntryStride     = 0x10;
    int64_t stockEntryVariant    = 0x00;    // pointer
    int64_t stockEntryQuantity   = 0x08;    // int64, writable
    int64_t equipmentFixedPointScale = 100000;

    // --- Variant database: the chain add_latest_equipment actually uses ---
    //
    // This is the part of sub_10015D3B0 that matters:
    //
    //   db  = *(Country + 0xD10)
    //   arr = *(db + 0xB8), cnt = *(db + 0xC4)
    //   for each variant v in arr:
    //       if (*(v + 0x3F8) & 1)  ->  give the player `amount` of v
    //
    // That last test was read wrong at first. The word at +0x3F8 is not a
    // "this is the current design" flag - it is the equipment category, read
    // off a live German database:
    //
    //   0x04 armour        0x20 infantry      0x40 capital ship
    //   0x80 submarine     0x100 screen       0x400 fighter
    //   0x1000 bomber      0x4000 CAS         0x8000 float plane
    //   0x80000 railway artillery             0x80000000 train
    //
    // with the high bits (0x08000000, 0x10000000, 0x20000000) set on only a
    // few entries. What actually separates a country's real designs from the
    // rest of the database is simpler: real designs carry a name, and the
    // filler entries do not.
    //
    // Unlike the stock arrays, this hangs off the country at a fixed offset,
    // so it resolves the same way every session and in every campaign - no
    // scanning, no picking a list by hand.
    //
    // Verified live: Country + 0xD10 -> 0x7FB5F395A400, whose +0xB8 pointed
    // at an array of 73 variant pointers (count at +0xC4, capacity at +0xC0).
    int64_t variantDatabasePointer = 0xD10;   // Country + this
    int64_t variantDatabaseArray   = 0xB8;
    int64_t variantDatabaseCount   = 0xC4;    // int32
    int     variantDatabaseMax     = 4096;

    // Variant database, used to resolve which variants exist at all.
    int64_t variantDatabase      = 0xD10;   // Country + this -> pointer
    int64_t variantArray         = 0xB8;
    int64_t variantCount         = 0xC4;    // int32
    int64_t variantActiveFlag    = 0x3F8;   // bit 0 set = active

    // --- Special projects / breakthrough points ---
    //
    // From the console command that prints "Added breakthrough point"
    // (sub_101572120), which resolves
    //
    //   obj = *(Country + 0xD50)
    //   sub_10159E5A0(obj, specializationId, amount * 0x2710)
    //
    // Note the scale here is 0x2710 = 10000, different from everything else
    // in this file.
    //
    // Points are stored PER SPECIALIZATION, and the container is a red-black
    // tree (std::map), not an array - sub_10159E5A0 walks it comparing the
    // key at +0x1C and inserts a node when the key is missing:
    //
    //   node + 0x00   left
    //   node + 0x08   right
    //   node + 0x10   parent
    //   node + 0x1C   int32  specialization id (the key)
    //   node + 0x20   int32  breakthrough points (the value)
    //
    // The command with one argument iterates every entry of
    // CSpecializationDatabase and calls the setter for each, which is why it
    // tops up all specializations at once.
    int64_t specialProjectsObject = 0xD50;   // Country + this -> pointer
    int64_t breakthroughTreeRoot  = 0x60;    // in that object -> map root
    int64_t breakthroughTreeCount = 0x68;    // int64, number of nodes
    int64_t mapNodeLeft   = 0x00;
    int64_t mapNodeRight  = 0x08;
    int64_t mapNodeKey    = 0x1C;   // int32
    int64_t mapNodeValue  = 0x20;   // int32
    int64_t breakthroughFixedPointScale = 10000;

    // --- Nuclear bombs ---
    //
    // From the `nukes` handler (sub_100140570):
    //
    //   sub_100861D20(*(Country + 0x1098), count)
    //
    // Note the dereference - 0x1098 holds a POINTER to the nuke object, not
    // the object itself. The setter is:
    //
    //   sub_100861D20(obj, count):
    //       *(obj + 0x18) += count * 1000 * 100000
    //       clamp to 0x1742810700 (= 100,000,000,000)
    //
    // So the stored value is scaled by 100,000,000 (1000 * 100000) and the
    // internal ceiling works out to 1000 nukes. The console handler clamps
    // its argument to 999 separately, but that is input validation - writing
    // the field directly is only bound by the internal clamp.
    int64_t nukeObjectPointer = 0x1098;   // Country + this -> pointer
    int64_t nukeCount = 0x18;     // int64 in that object
    int64_t nukeFixedPointScale = 100000000;   // 1000 * 100000
    int64_t nukeMaximum         = 1000;

    // --- Command power ---
    //
    // From the `cp` handler (sub_100159660), which ends in
    //
    //   sub_101195BA0(sub_1011D3D20(tag), &amount)
    //
    // Note this setter takes the country directly - no sub-object, unlike
    // political power and experience. The setter is:
    //
    //   sub_101195BA0(country, &amount):
    //       *(country + 0x1B0) += amount
    //       modA = GetValue(country + 0x560)
    //       modB = GetValue(country + 0x560)
    //       if (cp < 0) cp = 0
    //       else {
    //           max = (data_1034F3B98 + modA) * (modB + 100000) / 100000
    //               - *(country + 0x1B8)
    //           if (cp > max) cp = max
    //       }
    //
    // So the cap is computed from modifiers minus the field at +0x1B8. The
    // game shows that field in the tooltip as "Allocated", and because the
    // formula SUBTRACTS it, writing a negative value RAISES the cap.
    //
    // Confirmed live: field read 0 with a cap of 80; writing -10000000
    // (-100 at scale) produced "Current Max: 180, Base value: 80,
    // Allocated: +100" in the tooltip.
    //
    // Scale is 100000, same as political power and resources.
    int64_t commandPower = 0x1B0;   // int64, /100000
    int64_t commandPowerCap = 0x1B8;   // int64, /100000, subtracted from max
    int64_t commandPowerFixedPointScale = 100000;

    // Modifier object, seen twice: in the command power cap formula above and
    // in the three experience setters, both as GetValue(country + 0x560).
    // This is where the game's derived caps come from. Not explored yet - it
    // is the likely route to the building slot limit that blocks factories
    // from being usable.
    int64_t modifierObject = 0x560;

    // --- Political power ---
    //
    // From the `pp` handler (sub_10015EFF0), which ends in
    //
    //   sub_1013704A0(sub_1011A6A70(sub_1011D3D20(tag)), amount)
    //
    // with sub_1011A6A70 returning *(Country + 0xD38) - the political status
    // object - and the setter itself being
    //
    //   sub_1013704A0(obj, amount):
    //       *(obj + 0xE0) += amount, then clamped to a global min/max
    //
    // The console argument is multiplied by 0x186A0 here, so political power
    // uses the same 100000 scale as resources.
    //
    // The clamp reads two globals (data_1034F37B8 / data_1034F37C8). Writing
    // far past the maximum will be pulled back the next time the game touches
    // the field.
    int64_t politicalStatusObject = 0xD38;   // Country + this
    int64_t politicalPower = 0xE0;    // int64, /100000
    int64_t politicalPowerFixedPointScale = 100000;

    // --- Diplomacy ---
    int64_t exileStatus = 0x1A8;

    // --- Static toggles ---
    //
    // Not country fields - single bytes at fixed image-relative addresses.
    //
    // researchOnIconClick is the `research_on_icon_click` / `roic` console
    // command (sub_10014BC60), which is just:
    //
    //   data_1034EDFBE ^= 1
    //
    // It is a UI-side flag: clicking a technology in the tree researches it
    // instantly. The AI never clicks, so unlike instantconstruction this
    // affects only the player.
    int64_t researchOnIconClick = 0x35064CE;   // imageBase + this, one byte

    // The `sp_instant` console command (sub_10156E550), same shape:
    //
    //   data_1034EE029 ^= 1
    //
    // With it on, every started Special Project completes on the daily tick,
    // skipping the prototype iterations and the rewards that come with them.
    int64_t instantSpecialProjects = 0x3506539;   // imageBase + this, one byte

    // `instantshiprefit` (sub_1001424F0). With it set, sub_100A65000 skips
    // the refit process entirely and applies the upgrade to each selected
    // ship straight away.
    int64_t instantConstruction = 0x35064E0;   // imageBase + this, one byte (0x1034edf48 + 0x88)
    int64_t instantShipRefit = 0x35064E1;   // imageBase + this, one byte (0x1034edf48 + 0x89)
    int64_t instantTraining = 0x35064E3;   // imageBase + this, one byte (0x1034edf48 + 0x8b)

    // `allowtraits` console command (sub_10013f890): "Allows to learn all traits."
    // Reverses restriction on assigning traits to military leaders.
    int64_t allowTraits = 0x35064D0;   // imageBase + this, one byte (0x1034edf48 + 0x78)

    // `freefocuses` / `ff` console command (sub_1001863d0): "Enable freely activating any focuses"
    // Sets three static flags that bypass prerequisites, allow any focus to start,
    // and complete active focus in 1 day on the daily tick.
    int64_t focusAutocomplete = 0x35064E4;   // imageBase + this, byte (0x1034edf48 + 0x8c)
    int64_t focusAutocompleteB = 0x35064E8;   // imageBase + this, byte (0x1034edf48 + 0x90)
    int64_t focusAutocompleteC = 0x35064E9;   // imageBase + this, byte (0x1034edf48 + 0x91)

    // Additional confirmed toggles in the same table:
    int64_t allowIdeas = 0x3506500;   // imageBase + this, byte (0x1034edf48 + 0xa8, allowideas)
    int64_t allowOperations = 0x3506501;   // imageBase + this, byte (0x1034edf48 + 0xa9, allowoperations)
    int64_t researchFast = 0x35064D1;   // imageBase + this, byte (0x1034edf48 + 0x79, research_fast - tech cost 1 RP)

    // Intelligence Agency & Operations (La Résistance toggles in 0x1034edf48 table):
    int64_t instantOperation = 0x35064E2;   // imageBase + this, byte (0x1034edf48 + 0x8a, Operation.Instant)
    int64_t instantIntelNetwork = 0x35064DB;   // imageBase + this, byte (0x1034edf48 + 0x83, IntelNetwork.Instant)
    int64_t instantAgencySlotUnlock = 0x35064DC;   // imageBase + this, byte (0x1034edf48 + 0x84, Agency.InstantSlotUnlock)
    int64_t instantAgencyUpgrade = 0x35064E5;   // imageBase + this, byte (0x1034edf48 + 0x8d, Agency.Autocomplete - upgrades build in 0 days)
    int64_t instantAgencyDepartment = 0x35064E7;   // imageBase + this, byte (0x1034edf48 + 0x8f, Department instant)
    int64_t preventOperativeDetection = 0x3506505;   // imageBase + this, byte (0x1034edf48 + 0xad, prevent_operative_detection - operatives never detected/killed)

    // Naval Invasions & Paradrop (NDefines in DATA segment):
    int64_t navalInvasionPrepareDays = 0x3516C38; // NNavy::NAVAL_INVASION_PREPARE_DAYS (int32_t, vanilla=3)
    int64_t navalInvasionPlanCap = 0x3516C48; // NNavy::NAVAL_INVASION_PLAN_CAP (int32_t)
    int64_t baseNavalInvasionDivCap = 0x3516C58; // NNavy::BASE_NAVAL_INVASION_DIVISION_CAP (int32_t, vanilla=10)
    int64_t airInvasionPrepareDays = 0x35165A8; // NAir::AIR_INVASION_PREPARE_DAYS (int32_t, vanilla=7)
    int64_t paradropHours = 0x35150D8; // NMilitary::PARADROP_HOURS (int32_t, vanilla=48)
    int64_t paradropAirSuperiorityRatio = 0x350BD98; // NCountry::PARADROP_AIR_SUPERIORITY_RATIO (int64_t, vanilla=70000)

    // --- Divisions ---
    //
    // Every offset here was derived in a live session against the division
    // tooltip and the Unit Details panel, on Divizie 12 Infanterie and
    // confirmed on a second division of the same template.
    //
    // HOW A DIVISION IS FOUND
    //
    // The `unit_address` console handler (sub_1001628C0) walks the selection
    // list, takes the first entry whose type tag at +0x08 is 0, 1 or 13
    // (mask 0x2003), caches that pointer in a global and prints it:
    //
    //     sel  = *(imageBase + 0x35011A8) + 0x500     the selection list
    //     node + 0x00   -> the unit
    //     node + 0x08   -> prev
    //     node + 0x10   -> next
    //     data_1034EE050 = the unit it settled on
    //
    // Reading that global is how the SDK gets an anchor without the player
    // copying an address by hand: select a division, run unit_address once,
    // and every later call resolves from the global.
    //
    // HOW THE REST ARE FOUND
    //
    // Divisions live in a contiguous pool with a 0x1000 stride. Confirmed by
    // searching a 0x60000 window for the template's maximum organisation -
    // the hits landed at +0x420 in slot after slot, 22 of them, matching the
    // country's division count. The pool is sparse: a free slot reads zero at
    // both +0x08 and +0x0C, while a live one reads 0 and 1.
    //
    // The pool held only this country's divisions in every check made, but
    // nothing was found that proves it - so the enumeration anchors on a
    // division the player selected and walks outwards, rather than scanning
    // memory for the pattern.
    //
    // WHAT IS AND IS NOT AUTHORITATIVE
    //
    // Nothing here is. Every field is recomputed by the game, and the only
    // difference between them is how often:
    //
    //   +0x420 (organisation)  is forced back within the same tick while a
    //                          division is on Army Exercises, which pin it to
    //                          20% of maximum
    //   +0xA68 (max org)       survived several minutes of play and looked
    //                          authoritative, then reverted at a day boundary
    //   the combat stats       track the equipment level and move whenever it
    //                          does
    //
    // So a single write shows up in the UI and then goes away. Holding a value
    // means rewriting it faster than the game recomputes it, which is what
    // DivisionFreeze in the SDK does - at a few hundred milliseconds it wins
    // comfortably, unlike the country resource container where the game
    // managed 13 overwrites a second.
    //
    // A NOTE ON WHAT IS NOT HERE
    //
    // Planning, attrition and the equipment counts behind Strength are NOT in
    // this object. A search for the division's 430/910 infantry equipment came
    // back empty over 0x2000 bytes, so the per-battalion equipment hangs off a
    // pointer somewhere else. Strength therefore cannot be set from here -
    // filling the country stockpile and letting the divisions reinforce is the
    // way to move it today.
    // --- Divisions (CArmy : public CUnit) ---
    //
    // Binary architecture confirmed by Mach-O disassembly:
    // Every land division in memory is an instance of CArmy (size 0x678 bytes).
    // The virtual table pointer at +0x00 is imageBase + 0x3297548 (__ZTV5CArmy).
    //
    // How the player's divisions are found:
    // CCountry maintains a direct dynamic array CPdxArray<CArmy*, int> at
    // Country + 0x250 (pointer array) with count at Country + 0x25C.
    // This allows instantaneous, 100% complete discovery of ALL divisions owned
    // by the player without requiring unit selection or console commands.
    uint64_t divisionVtable = 0x32AF5A8; // imageBase + this -> __ZTV5CArmy primary vtable
    uint64_t divisionVtable2 = 0x32AF830; // imageBase + this -> secondary vtable at +0x10
    int64_t countryDivisionsArray = 0x250;     // Country + this -> CArmy*[] (native division vector)
    int64_t countryDivisionsCapacity = 0x258;     // Country + this -> int32 capacity
    int64_t countryDivisionsCount = 0x25C;     // Country + this -> int32 count
    int64_t countryArmyGroupsArray = 0x238;     // Country + this -> CArmyGroup*[]
    int64_t countryArmyGroupsCount = 0x244;     // Country + this -> int32 count
    int64_t countryFleetsArray = 0x268;     // Country + this -> CFleet*[]
    int64_t countryFleetsCount = 0x274;     // Country + this -> int32 count

    int64_t divisionTypeTag          = 0x08;      // int32, 0 for land army unit
    int64_t divisionSlotUsed         = 0x0C;      // int32, legacy pool slot flag
    int64_t divisionStatsObject      = 0x138;     // CArmy + this -> CDivisionStats*
    int64_t divisionStatsMaxOrg      = 0x268;     // Stats + this -> int64 (/100000, max organisation)
    int64_t divisionStatsMaxHP       = 0x270;     // Stats + this -> int64 (/100000, max HP / strength)

    // In CUnit (all combat stats are 64-bit int64_t scaled by 100,000):
    int64_t divisionHardAttack = 0x188;     // int64, /100000
    int64_t divisionSoftAttack = 0x190;     // int64, /100000
    int64_t divisionHardAttackFactor = 0x198;     // int64, /100000
    int64_t divisionSoftAttackFactor = 0x1A0;     // int64, /100000
    int64_t divisionDefense = 0x1A8;     // int64, /100000 (defense)
    int64_t divisionBreakthrough = 0x1B0;     // int64, /100000 (breakthrough)
    int64_t divisionArmor = 0x1B8;     // int64, /100000 (armor)
    int64_t divisionOwnerTag = 0x1D8;     // int32, country tag
    int64_t divisionControllerTag = 0x1E0;     // int32, country tag

    // Dynamic runtime values (int64_t scaled by 100,000):
    int64_t divisionHitPoints = 0x418;     // int64, /100000 (current HP / strength)
    int64_t divisionOrganisation = 0x420;     // int64, /100000 (current organisation)
    int64_t divisionExperience = 0x428;     // int64, /100000 (veterancy 0 to 100,000)
    int64_t divisionPlanningBonus = 0x468;     // int64, /100000 (planning bonus, 100000 = 100%)
    int64_t divisionPlanningBase = 0x460;     // int64, /100000 (base planning cap)
    int64_t divisionEntrenchment     = 0x450;     // int64, /100000 (current dig_in)
    int64_t divisionEntrenchmentCap = 0x458;     // int64, /100000 (max dig_in_cap)
    int64_t divisionMaxOrganisation  = 0x268;     // in stats object (Stats + 0x268)
    int64_t divisionHitPointsCopy    = 0x270;     // in stats object
    int64_t divisionOwnerCountry     = 0x820;     // -> Country* pointer
    int64_t divisionFixedPointScale  = 100000;

    int64_t divisionStride           = 0x1000;    // pool stride, legacy
    int      divisionMaxGap          = 128;
    int      divisionMaxCount        = 2048;

    // --- Military Leaders / Commanders (Generals, Field Marshals, Admirals) ---
    // Country + 0xD98 -> Character / Military Leader Manager
    // Vector format: ptr at +0x00, capacity at +0x08, count at +0x0C
    int64_t leaderManager = 0xD98;    // Country + this -> Manager*
    int64_t leaderGeneralsVector = 0x70;     // Manager + this -> BVector<Leader*> (Corps Commanders)
    int64_t leaderFieldMarshalsVector = 0x88;     // Manager + this -> BVector<Leader*> (Field Marshals)
    int64_t leaderAdmiralsVector = 0xA0;     // Manager + this -> BVector<Leader*> (Navy Admirals)
    int64_t leaderExperience = 0xCA0;    // Leader + this -> int64 (XP * 100000)
    int64_t leaderStatsObject = 0xC98;    // Leader + this -> Stats* (stats/traits descriptor object)
    int64_t leaderStatsSkillLevel    = 0x180;    // Stats + this -> int32 (Skill level 1-9)

    // Leader Role / Type at +0xCB4:
    //   0 = Corps Commander (General)
    //   1 = Field Marshal
    //   2 = Navy Admiral
    //
    // CRITICAL: The game engine checks `cmpl $1, 0xCB4(%rax)` and `cmpl $0, 0xCB4(%rax)`
    // in hundreds of places for medal eligibility, field marshal promotion, and assignment.
    // Overwriting 0xCB4 with 9 (as a mistaken skill level) corrupts the leader's role,
    // locking out medals, field marshal promotions, and army group assignments!
    // Generals MUST have 0, Field Marshals MUST have 1, Admirals MUST have 2.
    int64_t leaderRole = 0xCB4;    // Leader + this -> int32 (0=General, 1=FieldMarshal, 2=Admiral)
    int64_t leaderSkillLevelLegacy   = 0xCB4;    // Kept for compatibility, but DO NOT WRITE 9 HERE!

    // Leader sub-skill offsets (Attack, Defense, Planning, Logistics, Skill 5)
    // In HOI4 (0x1017f3c40 and 0x1017f6751), leader sub-skills are stored at stride 0x10 (16 bytes):
    int64_t leaderAttackSkill = 0xD88;    // Leader + this -> int32 (Attack)
    int64_t leaderDefenseSkill = 0xD98;    // Leader + this -> int32 (Defense)
    int64_t leaderPlanningSkill = 0xDA8;    // Leader + this -> int32 (Planning for Army / Maneuvering for Navy)
    int64_t leaderLogisticsSkill = 0xDB8;    // Leader + this -> int32 (Logistics for Army / Coordination for Navy)
    int64_t leaderSkill5 = 0xDC8;    // Leader + this -> int32 (Skill 5 / Extra)

    // Written by unit_address with the pointer it just printed.
    int64_t lastSelectedUnit = 0x3506560;   // imageBase + this -> unit pointer

    // The selection list itself, for enumerating what the player has selected
    // without going through the console. Not used yet - the cached global
    // above is enough for an anchor.
    int64_t selectionRoot = 0x35196C8;   // imageBase + this -> pointer
    int64_t selectionOffset  = 0x500;       // + this -> the list
    int64_t selectionNext    = 0x10;        // node + this -> next node

    // Convenience helpers, relative to the state object itself.
    int64_t stateManpowerAbsolute()   const { return stateManpowerObject + stateManpower; }
    int64_t statePopulationAbsolute() const { return stateManpowerObject + statePopulation; }

    // ---------------------------------------------------- ironman, multiplayer & console
    // GameState + 0x0A8: uint32 flags (bit 0 = 1 if Ironman mode is active).
    // Achievements require Ironman to be on. We NEVER clear this bit!
    int64_t gameStateFlags = 0xA8;

    // CConsoleCmdManager::IsConsoleAvailable() at imageBase + 0x2A520D0.
    int64_t consoleIsAvailableFunc = 0x2A67AE0;

    // CConsoleCmdManager::m_showConsole check function at imageBase + 0x2A52130.
    // Original: 55 48 89 e5 0f b6 87 b0 00 00 00 5d c3
    // Patch   : b8 01 00 00 00 c3 (mov $1, %eax; retq) -> always returns true
    int64_t consoleShowConsoleFunc = 0x2A67B40;

    // Keyboard event console toggle gate at imageBase + 0x0765E53:
    // 0x0765E53: je 0x1007662f2 (0f 84 99 04 00 00 -> 90 90 90 90 90 90)
    // NOPing this out forces the keyboard handler to ALWAYS toggle CConsole window.
    int64_t consoleToggleKeyGate = 0x769253;

    // Keyboard event Ironman / Multiplayer gates:
    // 0x07642A1: jne 0x100764454 (0f 85 ad 01 00 00 -> 90 90 90 90 90 90)
    // 0x07642B1: je  0x100764454 (0f 84 9d 01 00 00 -> 90 90 90 90 90 90)
    int64_t consoleGateCheckA = 0x7676A1;
    int64_t consoleIronmanMultiplayerGate = 0x7676B1;

    // CConsoleCmdManager::Execute(char const*) checks:
    // 0x2A522A5: je 0x102a522f6 (74 4f -> eb 4f) skips multiplayer/release checks
    // 0x2A522BF: jne 0x102a522d8 (75 17 -> 90 90) disables multiplayer blocker jump
    // 0x2A522D6: je 0x102a522f6 (74 1e -> eb 1e) skips ironman check
    // 0x2A5244B: je 0x102a5245e (74 11 -> eb 11) unlocks developer-only commands
    int64_t consoleExecCheckRelease = 0x2A67CB5;
    int64_t consoleExecCheckMultiplayer = 0x2A67CCF;
    int64_t consoleExecCheckIronman = 0x2A67CE6;
    int64_t consoleExecCheckDevOnly = 0x2A67E5B;

    // Global CConsoleCmdManager instance pointer at imageBase + 0x35C80F0
    int64_t consoleCmdManagerPointer = 0x35E0610;

    // Global CConsole instance at imageBase + 0x35C0160
    int64_t consoleObjectPointer = 0x35D8680;
    int64_t consoleGuiObject            = 0xB0;
    int64_t consoleIsOpen               = 0xC4;

    // Multiplayer Kick Unlocks
    // 1. In-game Kick/Ban UI button gate (imageBase + 0x21B6E78)
    //    Original: 74 18 (je 0x1021b6e92)
    //    Patch   : eb 18 (jmp 0x1021b6e92) -> unlocks Kick/Ban buttons for all players in MP
    int64_t multiplayerKickGuiGate = 0x21C9F58;

    // 2. Chat /kick command operator check (imageBase + 0x00D77F4)
    //    Original: 7e 22 (jle 0x1000d7818 -> CHAT_ERROR_KICK_NOTOPERATOR)
    //    Patch   : eb dd (jmp 0x1000d78d3) -> bypasses operator check, kicks targeted player
    int64_t chatKickOperatorCheck = 0xDA294;

    // 3. Loop operator bypass (imageBase + 0x00D77DD)
    //    Original: 0f 85 f0 00 00 00 (jne 0x1000d78d3)
    //    Patch   : e9 f1 00 00 00 90 (jmp 0x1000d78d3; nop)
    int64_t chatKickLoopCheck = 0xDA27D;
};

// ---------------------------------------------------------------- resources

// Resource ids read from the per-slot id field. Stable across campaigns and
// across countries - the same ids appear in a freshly loaded save.
//
// Always look up by ID, never by slot position. Slot numbering differs
// between the state and country containers, and a country that owns no
// deposit of a given resource may still carry the slot with a zero value.
//
// The name mapping is INFERRED from icon order in the Logistics bar of a
// country that produces all seven. It has not been confirmed against the
// resource database entries, so treat the names as a convenience and the ids
// as the reliable part.
struct ResourceInfo {
    int32_t     id;
    const char* name;      // inferred, see note above
};

static constexpr ResourceInfo kResources[] = {
    { 20023, "oil"       },
    { 19999, "aluminium" },
    { 15877, "rubber"    },
    { 15878, "tungsten"  },
    { 15875, "steel"     },
    { 15879, "chromium"  },
    { 20005, "coal"      },
};

static constexpr int kResourceCount = 7;

} // namespace hoi4
