# HOI4-macOS-offsets
# HOI4 — Current State and Continuation Handover

Handover document for a new session. Covers everything reverse-engineered so far on Hearts of Iron IV, macOS x86_64 running under Rosetta 2, plus what failed and why.

Working files: `memory.hpp`, `hoi4_offsets.hpp`, `hoi4_sdk.hpp`, `trainer.cpp`, `poke.cpp`, `valfind` (scanner). Offsets have their derivation notes directly inside `hoi4_offsets.hpp`.

The ASLR slide was 0 in all sessions, but it is recalculated on every launch.

---

## What Works in the Trainer

| Key | Action / Function |
|---|---|
| 1-3 | manpower, resources, population ceiling / cap |
| 4 | buildings (infrastructure + civilian factories confirmed) |
| 5-7 | XP, political power, command power with cap override |
| 8 | nukes |
| 9 | research on click (works in Ironman, it is purely a UI flag) |
| b | breakthrough points |
| s | instant toggles: special projects, ship refit, construction |
| e | equipment by archetype (Logistics table rows) |
| v | equipment by variant (Stockpile table rows) |
| c | production costs, active line designs only |
| l | add_latest_equipment |
| p | production lines (read-only) |
| n | naval object address (diagnostic) |

---

## Confirmed Pointer Chains

### Root

```
imageBase + 0x3501220     → GameState
  + 0x4D8   int32  player tag (primary)
  + 0x4DC   int32  fallback tag
  + 0x2D8   → Country*[]
  + 0x290   → State*[]
  + 0x29C   int32 state count
```

Player resolution pattern, identical across all handlers:

```c
rdi = GameState + 0x4DC;
if (*(GameState + 0x4D8) > 0) rdi = GameState + 0x4D8;
country = sub_1011D3D20(rdi);
```

### Country

```
Country + 0x1B0    int64  command power, /100000
Country + 0x1B8    int64  ceiling deduction (negative = raises ceiling)
Country + 0x420    → State*[] owned
Country + 0x42C    int32  owned states count
Country + 0x560    modifier object
Country + 0xD10    → variant database base       ← key for equipment
Country + 0xD18    → naval object
Country + 0xD38    → political status (+0xE0 = PP, /100000)
Country + 0xD50    → special projects
Country + 0xF80    → resources (derived, non-writable)
Country + 0xF88    → naval container (convoys) — NOT general equipment
Country + 0x1098   → nukes (+0x18, scale 100000000)
Country + 0x12E8   → XP (+0x10 air, +0x28 navy, +0x40 army, scale 32768)
```

### Equipment — Complete Chain

Derived by tracing `add_equipment` (`sub_10015D8F0`), which branches between two containers:

```c
if (sub_10111D910(*(variant + 0x3E0)) == 0)
    sub_10140D3D0(*(country + 0xD10), variant, quantity);   // general
else if (*(variant + 0x3F8) & 1)
    sub_1001ABDC0(country + 0xF88, variant, quantity);      // naval
```

and `sub_10140D3D0` is merely `sub_10113D190(base + 0x200, variant, quantity * 0x186A0)`.

```
Country + 0xD10 (dereferenced!) + 0x200   equipment container
  + 0x08   → archetype array, stride 0x18
      + 0x00  → archetype
      + 0x08  int32 index into variant array
      + 0x0C  int32 variant count
      + 0x10  int64 total, /100000
  + 0x14   int32 archetype count
  + 0x20   → variant array, stride 0x10
      + 0x00  → variant
      + 0x08  int64 quantity, /100000
  + 0x2C   int32 variant count
```

Divisions consume from the **variant array**, not the archetype array. Writing to archetype reflects in Logistics but does not fix "missing equipment".

### Design Database

```
Country + 0xD10 (dereferenced) + 0xB8   → pointer array to variants
                               + 0xC4   int32 count
```

Contains everything the country can construct — including ships, land cruisers, railway guns, which do **not** appear in the equipment container at `+0x200`. In late-game Germany: 79 designs versus 21 active production lines.

### Variant

```
variant + 0x28    proper name (std::string), empty for default designs
variant + 0x358   int64 production cost, /100000   ← writable
variant + 0x3E0   → equipment object
    + 0x10        token (std::string), e.g. "infantry_equipment_1"
variant + 0x3F8   flags: equipment category, NOT "current design"
```

Categories at `+0x3F8`, read on Germany:

```
0x04 armor     0x20 infantry   0x40 capital ship   0x80 submarine
0x100 escort   0x400 fighter   0x1000 bomber       0x4000 CAS
0x8000 seaplane   0x80000 railway gun   0x80000000 train
```

### Production Lines

```
Country + 0xD10 (dereferenced) + 0x58   line container
  + 0x00   → array
  + 0x0C   int32 count
    line + 0x28   int64 cached cost copy   ← DO NOT WRITE HERE
    line + 0x88   → design being produced
```

---

## libc++ Strings

Both representations occur and must be handled:

```
Short form (≤22 chars): first byte = (length << 1), text follows immediately
Long form: bit 0 set, length at +0x08, pointer to text at +0x10
```

Handling only the short form produced `?` for all long names like `ballistic_missile_equipment_2` (29 characters). `readStdString` in the SDK handles both.

---

## Global Flags (One Byte, XOR in Handler)

```
imageBase + 0x34EDFBE   research_on_icon_click
imageBase + 0x34EDFD0   instantconstruction   — buildings only, all countries
imageBase + 0x34EDFD1   instantshiprefit
imageBase + 0x34EE028   sp_fast
imageBase + 0x34EE029   sp_instant
```

`instantconstruction` explicitly excludes naval repairs and conversions — condition in `AddConstruction` is `!bRepair && !bConversion`. There is no flag for instant repair.

---

## Scales

| Subsystem | Scale Factor |
|---|---|
| Resources, PP, CP, equipment, production cost | 100000 |
| Experience | 32768 |
| Breakthrough points | 10000 |
| Nukes | 100000000 |
| Manpower, building levels | 1 |

---

## Confirmed Traps in This Session

**Line copy vs variant source.** The production line stores a copy of the cost at `+0x28`. Writing there desynchronizes the line from its source, causing the game to crash with `EXC_BAD_ACCESS` on a virtual call — the object's vtable gets corrupted indirectly. The canonical source is `variant + 0x358`.

**Cost scale factor.** Writing raw `1` means cost 0.00001 and stalls the game on ships. `100000` (meaning 1.0) works. Confusion arose because `u <address> 1` in poke multiplies by 100000, whereas hardcoded logic had written the raw value.

**Wrong stride on variant array.** Archetype array stride is `0x18`, variant array stride is `0x10`. Using `0x18` on variants corrupts pointers and crashes the game.

**Forgotten dereference.** `Country + 0xD10` stores a pointer to the base, not the base struct itself. Adding `0xD10 + 0x200` directly lands in the middle of the Country object.

**Naval container confused with general equipment.** `Country + 0xF88` stores only convoys. Reading it early on another country led to the false conclusion that it was the general equipment container, wasting hours on useless scans. The assert string references `convoys.cpp` because the code path is shared.

**Pattern scan vs pointer chain.** Pattern scanning locates matching arrays, but every country has identical struct layouts — for variants, 428 candidate lists were found. Without a deterministic chain from the country object, one cannot distinguish the player's data.

---

## Method That Worked Consistently

1. Locate the equivalent console command in the table initialized at `sub_100191E40`.
2. Decompile the handler in Binary Ninja.
3. Trace down to the actual setter — typically 2-3 calls deep.
4. Set a breakpoint on the call in lldb, execute the command in-game.
5. Inspect registers: difference between the passed object and Country base yields the offset.
6. Verify read value against UI before attempting any write.

Read watchpoints are equally useful: placed on a field, the backtrace reveals who reads it and with what base object in registers. That is how we confirmed production lines read cost from variants, not vice versa.

---

## Open Investigations

**Division stats** — organization, planning bonus, fuel, attrition, supply. Unmapped. Attrition and supply are almost certainly calculated on the fly. Starting point: strings in division tooltips, or an equivalent console command.

**Instant naval construction & repair.** The `instantconstruction` flag does not cover them. Need to locate naval line progress inside the object at `Country + 0xD18`. Observed sub-structures: `+0x20`, `+0xF8`, `+0x38`/`+0x40` (count 52 at `+0x48`), `+0x100`, `+0x118`.

**Building indices** for military factories and dockyards. Clean path: enumerate via `TGameItemDatabase<CBuildingDatabase>`.

**Max construction slots per state** (e.g. 6/25). Likely located in modifier structs under `Country + 0x560`.

**Cost floor threshold for ships.** `1.0` worked for Panzerschiff in tests; exact minimum threshold across hull types remains undetermined.

---

## 10. Manpower — COMPLETE, Validated

**Manpower is not a country-level field.** The top-bar total is an aggregate sum across all owned states and is stored nowhere. Value scanning for it returns zero results — this was session lesson #1.

```
State + 0x7B0         CStateManpower object
  + 0x10              int32  available manpower     ← writable, authoritative
  + 0x18              int32  recruitable population ← raises cap
```

Relevant functions:

| Address | Description |
|---|---|
| `0x10014D810` | `manpower` command handler |
| `0x1007B2C40` | CManpower setter (from `manpower.cpp`) |
| `0x1007B3240` | per-state distributor |
| `0x100985790` | `stateManpowerAdd` — capped by population |
| `0x1009857F0` | `stateManpowerTake` |

Offset `+0x10` was extracted directly from `stateManpowerTake`:

```asm
mov 0x10(%rdi), %ecx
sub %eax, %ecx
mov %ecx, 0x10(%rdi)
```

Writing to `+0x10` persists. To exceed the cap, also write `+0x18`, because distribution routes through `stateManpowerAdd` which clamps against population.

---

---

## 11. Resources — COMPLETE, Validated

Resources exist on two distinct tiers, and only the lower tier is writable.

### Per-State (Authoritative)

```
State + 0x1E4 + j*16   int32  BASE quantity, slot j   ← writable, persistent
State + 0x1E8 + j*16   int32  resource ID, slot j
```

The state container begins at **slot 0** with an actual resource.

### Per-Country (Effectively Read-Only)

```
Country + 0xF80        → resource object
  + 0x30               → buffer (single dereference)
    + i*16             int64  quantity, fixed-point ÷100000
    + i*16 + 8         int64  resource ID
```

Five consecutive containers, stride `0xC0`, matching Trade tab order: **Extracted, Imported, Exported, Projects, Production**. Surplus = Extracted + Imported − Exported − Production.

The country container has a **sentinel at slot 0** (ID 357, value 0). Iteration starts at index 1.

This indexing difference caused a critical bug during the session: reading the state container starting from slot 1 skipped a resource and shifted all resource names by one slot. **Always match by resource ID, never by array index.**

### Resource IDs

| ID | Resource Name |
|---|---|
| 20023 | oil |
| 19999 | aluminium |
| 15877 | rubber |
| 15878 | tungsten |
| 15875 | steel |
| 15879 | chromium |
| 20005 | coal |

IDs are confirmed stable across campaigns and countries. Names are **inferred** from icon order in the Logistics bar — not yet cross-verified against the internal resource database.

### Why Country-Level Writing Fails

The object at `+0xF80` is **reallocated on every recalculation tick**. A rewrite loop running every 2 ms was outpaced by ~13 engine overwrites per second. The written value persists for under 100 ms. Per-state BASE writing is the only method that sticks.

### Safe Value Range

**50 per slot** yields ~65,000 of each resource after state modifiers. Values exceeding ~200 cause int32 overflow downstream, turning the Logistics bar negative.

---

---

## 12. Buildings — PARTIAL

Data structure is confirmed; index-to-building-type mapping is pending.

```
State + 0x110          building container (polymorphic object, vtable at +0)
  + 0x20               int32[]   ID → index translation table
  + 0x2C               int32     count (56)
  + 0x38               entry*[]  pointer array to building entries
    entry + 0x40       int16     level  ← writable
```

Derived from the `building_level` scripted variable getter (`sub_1011F91E0`) and its three helper accessors:

```c
sub_100D280D0(out, buildingDef, state):
    if (lookup(state + 0x110, def)) out = entryFor(state + 0x110, def)

sub_100D2CAC0(container, def):
    idx = *(*(container + 0x20) + def->id * 4)
    if (idx != -1) return *(*(container + 0x38) + idx * 8)

sub_100D27B80(accessor):
    return *(int16*)(entry + 0x40)
```

The level is **int16**, not int32. The scripted getter multiplies by `0x186A0` for fixed-point script representation, but the stored value is raw integer (3, 5, 10).

### Confirmed Indices

- **index 0 = infrastructure** — written across all 12 Romanian states; all rendered 10/5.
- **index 6 = civilian factory** — verified via isolation: after writing, `Civilian Factories owned` jumped from 59 to 71, while Military and Naval Dockyards remained unchanged.

### What Does Not Work

**You cannot blindly iterate the array at `+0x38`.** The `idx != -1` check in the engine code is critical: the translation table at `+0x20` maps only buildings that are actually instantiated; remaining slots are uninitialized. Blind iteration reads garbage — numbers like `-11664`, `-20520`, `76818544`.

A range filter (level between 0 and 1000, plausible pointer) eliminated absurd numbers but **did not solve the root issue**: identical slots returned differing values across runs without any writes, proving they were unallocated memory.

### The Clean Solution (Unimplemented)

Enumerate via `TGameItemDatabase<CBuildingDatabase>` — visible in getter decompilation. This provides the canonical name and ID for each building, while the translation table at `container + 0x20` resolves the ID to the storage index. Zero heuristics required; resolves all 56 buildings at once.

Same architecture as resources, where `sub_100A28830` provided the definition database with array at `+0x28` and count at `+0x34`.

### Empirical Testing Pitfall

Factories exceeding unlocked state slots show as `Damaged` and **decay over time**. If tested while unpaused, tooltip figures shift dynamically, making it impossible to separate your write from tick decay. All empirical tests must be performed **on pause**, from a clean save.

### Separate Constraint

Factories are constrained by **unlocked slots** per state (`6/25` in Muntenia). You can show 50 military factories "owned" but only 4 "in use". The slot count variable has not yet been located.

---

---

## 13. Experience (Army / Navy / Air) — COMPLETE, Derived

XP does not reside directly in the Country object. Handler for `xp` (`sub_1001591A0`) resolves it as follows:

```c
obj = sub_1011D4960(tag)
    = sub_10118C210(sub_1011D3D20(tag))
    = *(Country + 0x12E8)
```

Then calls three individual setters on the resolved object:

```c
sub_1001AF670(obj, &xp, ...)   *(obj + 0x40) += xp   // army
sub_1001AF470(obj, &xp, ...)   *(obj + 0x28) += xp   // navy
sub_1001AF270(obj, &xp, ...)   *(obj + 0x10) += xp   // air
```

Object structure:

```
obj + 0x00   vtable
obj + 0x08   → CountryBase
obj + 0x10   int64  air XP
obj + 0x28   int64  navy XP
obj + 0x40   int64  army XP
obj + 0x48   int64  army accumulator (also written by setter)
```

**Scale factor is 32768** (`2^15`), not 100000. Evident in the handler:

```asm
movl $0x1f38000, %r12d     ; default
movslq %eax, %r12          ; argument
shlq $0xf, %r12            ; << 15
```

Live verification: breakpoint on army setter, execute `xp 1000`, step out → `+0x48 = 32768000` and `+0x40 = 16384000`. Both get written on positive delta; **which field is sampled by the top HUD remains unverified**. Write `+0x40` first and check UI.

All three setters also include a branch propagating XP to faction allies (loop over `*(country + 0xD30) + 0x2C`, checking `*(entry + 8) == 0x38D9`), triggered only when `arg3 != 0`.

---

---

## 14. Political Power — COMPLETE, Derived

From the `pp` handler (`sub_10015EFF0`):

```c
sub_1013704A0(sub_1011A6A70(sub_1011D3D20(tag)), amount)
```

`sub_1011A6A70` returns `*(Country + 0xD38)` — the political status object. Setter logic:

```c
sub_1013704A0(obj, amount):
    *(obj + 0xE0) += amount
    clamp between data_1034F37B8 (min) and data_1034F37C8 (max)
    return sub_10100E3B0(*(obj + 0xF8))    // UI refresh
```

```
politicalStatus + 0xE0    int64  political power, ÷100000
```

**Scale factor is 100000**, matching resources — not 32768 like XP.

The clamp reads global engine variables rather than country fields. Setting a value above maximum gets snapped back on the next engine touch. Overriding the ceiling requires modifying globals, which affects all nations globally.

Assert string confirms subsystem: `CPoliticalStatus::AddPoliticalPower %lld to %s` in `politics/politics.cpp:0x657`.

---

---

## 15. Console Command Table

`sub_100191E40` populates the complete console command registry. It is a massive function consisting of uniform sequential blocks containing:

- Command string (`__builtin_strncpy` into stack buffer)
- Optional alias
- Description text (allocated via `sub_102A6A1C0`)
- **Handler function pointer** — the critical entry point
- Argument count and argument descriptors

This is the single most efficient reverse engineering vector in the binary. Any feature exposed to console has a handler here, and handlers are compact functions linking directly to fields.

Identified Handlers:

| Command | Alias | Handler | Purpose |
|---|---|---|---|
| `manpower` | — | `sub_10014D810` | add manpower |
| `pp` | `political_power` | `sub_10015EFF0` | political power |
| `xp` | — | `sub_1001591A0` | army + navy + air simultaneously |
| `cp` | — | `sub_100159660` | command power |
| `nukes` | `nuke` | `sub_100140570` | add nuclear warheads |
| `launch_nuke` | — | `sub_100140790` | launch without prerequisites |
| `armageddon` | — | `sub_100140E70` | global thermonuclear strike on all states |
| `fuel` | `army_juice` | `sub_10015E150` | fuel capacity/amount |
| `fuel_gain` | — | `sub_10015E590` | daily fuel production |
| `add_stability` | `stability` | `sub_10015F4C0` | stability |
| `add_war_support` | `war_support` | `sub_10015F900` | war support |
| `threat` | `tension` | `sub_10015CE70` | world tension level |
| `add_latest_equipment` | — | `sub_10015D3B0` | grant newest equipment variants |
| `add_equipment` | — | `sub_10015D8F0` | equipment by token name |
| `research` | — | `sub_10014B220` | complete slot or all research |
| `research_fast` | — | `sub_10014BD10` | 1 RP tech cost |
| `instantconstruction` | — | `sub_100142420` | instant building construction |
| `building_health` | `bhealth` | `sub_100141350` | building durability/health |
| `deironman` | — | `sub_10013A100` | strip Ironman mode flag |
| `resistance` | — | `sub_10015EA50` | state resistance |
| `compliance` | — | `sub_10015ED20` | state compliance |
| `add_legitimacy` | — | `sub_100172F60` | government legitimacy |
| `set_var` / `get_var` | — | `sub_1001698D0` / `sub_10016A3F0` | scripted variables |
| `tag` | — | `sub_10014DF50` | switch controlled country |
| `gamespeed` | — | `sub_100162660` | engine game speed, 0 = pause |
| `unit_address` | — | `sub_1001628C0` | selected unit pointer address (great for RE) |

Note on `deironman`: console access is blocked in Ironman mode, but reaching the handler directly bypasses the restriction and clears the flag.

---

---

## 16. General Reverse Engineering Methodology

Universal guidelines for game RE, applicable beyond HOI4.

### 16.1 Stability Categories

Most memory editing mistakes stem from confusing three fundamentally different concepts:

| Type | Example | Lifetime / Scope | Persists Across Runs? |
|---|---|---|---|
| Absolute Virtual Address | `0x104af8010` | Changes on every run, occasionally during execution | No |
| Static Offset | `base + 0x3261220` | Fixed for a specific binary build | Yes, plus runtime ASLR slide |
| Struct Member Offset | `this + 0x308` | Fixed for a specific binary build | Yes, absolute relative to `this` |
| Container Index | `array[42]` | Depends on runtime simulation state | Unreliable |

**ASLR** shifts image load base on every launch. Real virtual address = `static_offset + slide`. Obtain the slide via `image list -o -f` in lldb or parse the Mach-O header at runtime.

**Heap Allocations:** Modern memory managers constantly recycle and reallocate heap chunks. Freed objects leave behind seemingly valid memory footprints that no longer represent live data. A pointer recorded 10 minutes ago will frequently point to garbage or another entity.

The end goal is never a raw address; it is always a **deterministic pointer chain**:

```
base + static_offset  →  GameState*
GameState + 0x??      →  CountryArray*
CountryArray + idx*8  →  CountryBase*
CountryBase + 0x??    →  field_value
```

A proper chain functions across all reboots. A raw address fails on run #2.

---

### 16.2 Static Analysis

Conducted offline on the binary disk image using Binary Ninja, Ghidra, or IDA Pro.

### 16.2.1 Setup & Architecture

Always verify binary architecture first. Universal binaries pack multiple slices; disassembling the wrong slice renders all discovered offsets invalid:

```bash
lipo -info /path/to/game
file /path/to/game
```

On Apple Silicon, an x86_64 binary executes translated via Rosetta 2. Verify the live process architecture:

```bash
ps -o pid,arch -p $(pgrep -x game)
```

Dictates assembly dialect to look for: `mov [rax+0xNNN], rcx` on x86_64 vs `str x8, [x9, #0xNNN]` on ARM64.

### 16.2.2 Triage Vector: Strings

The lowest-barrier starting point in an unknown binary. However, string quality varies drastically:

| Category | Example | Diagnostic Value |
|---|---|---|
| Parser Keys | `manpower`, `conscription` | Poor — leads to `.txt` script parser, not runtime fields |
| Localization Keys | `MANPOWER_DESC`, `current_manpower` | Good — leads to UI formatting code that **reads** fields |
| Console Commands | `"Adds manpower to player"` | High — handler directly **writes** into target fields |
| Assertions & Logging | `"country.cpp"`, error formats | Exceptional — references struct field and file context directly |
| Source File Paths | `.../country_manpower.cpp` | Exceptional — reveals compilation unit and domain |

Workflow: `Strings` view → filter → inspect xrefs. **Discard strings with dozens of references** — these point to generic string serialization or formatting utilities.

Console dispatch tables are gold mines: they aggregate command name, description, parameter format, and **handler function pointer** in one contiguous record. Handlers are compact routines ending in the exact store instruction you need.

### 16.2.3 Triage Vector: RTTI and Vtables

Binaries compiled with RTTI (standard across C++ with exceptions enabled) expose cleartext type names:

```bash
strings -a game | grep -E "^_ZTS|class .*Country"
```

A `type_info` record reveals class hierarchy, and its xrefs locate the vtable. Vtables locate constructors, and **constructors are the authoritative source for struct layout**: they initialize every member field sequentially using hardcoded literal offsets.

```
mov dword [rdi+0x2F0], 0     ; int32 field at +0x2F0
mov qword [rdi+0x2F8], 0     ; int64 field at +0x2F8
```

Constructors reconstruct the entire class memory map in one view.

### 16.2.4 Triage Vector: Engine Constants & Signatures

Simulations employ characteristic mathematical constants — decay factors, tick multipliers, unit conversion ratios. If game mechanics rely on constants like `0.001` or `1.5`, inspect `__const` data sections and analyze referencing code.

### 16.2.5 Assembly Patterns to Recognize

- **Inline Getters**: 2-3 instructions, `mov rax, [rdi+0xNNN]; ret` → struct member at `0xNNN`.
- **Indexed Container Access**: `mov rax, [rdi+0x10]; mov rax, [rax+rsi*8]` → pointer array, entry indexed by `rsi`.
- **`std::vector` (libc++)**: Triad of consecutive pointers: `begin`, `end`, `capacity`. Length = `(end - begin) / sizeof(T)`.
- **`std::string` (libc++)**: Short String Optimization (SSO) — if lowest bit of first byte is 0, string is inline in 23 bytes; otherwise stores `{size, capacity, ptr}`.
- **Engine Singletons**: `mov rax, [rel 0x1XXXXXXX]; test rax, rax; jz init` → static root base pointer.

### 16.2.6 Limitations of Static Analysis

Modern game engines span hundreds of thousands of subroutines. Without an anchor, static disassembly is an unguided maze. Static analysis is most powerful when seeded by dynamic observation: dynamic scan for address/instruction → static disassembly for context & struct layout → dynamic confirmation.

---

### 16.3 Dynamic Analysis

Conducted on the live target using lldb and custom Mach VM memory scanners.

### 16.3.1 Process Attachment on macOS

On macOS, `task_for_pid` requires root privileges, and Hardened Runtime can reject process attachment entirely:

```bash
csrutil status
codesign -dv --entitlements - /path/to/game 2>&1 | grep -i runtime
sudo lldb -p $(pgrep -x game)
```

If attachment fails due to code signing or system policy, System Integrity Protection (SIP) must be disabled via Recovery. Confirm attachment viability before writing injection code.

**Self-Matching Pitfall**: When targeting a process by name pattern, explicitly exclude `getpid()`. A scanner named `game_reader` matches `game` and scans its own memory space, generating meaningless false positives.

### 16.3.2 Value Scanning Principles

Universal starting point, independent of symbols:

1. Identify candidate value in game UI; pause simulation.
2. Scan readable/writable (`PROT_READ | PROT_WRITE`) memory regions.
3. Advance simulation to perturb state; re-pause.
4. Rescan filtered candidate list; retain set intersection.
5. Repeat until candidates converge.

Optimization Rules:

**Value Selection:** Obscure, non-round values (e.g. `3847291`) produce orders of magnitude fewer initial hits than round figures (`100`, `0`).

**Encoding Variants:** Do not assume integer types. Test `int32`, `int64`, `float`, `double`, and fixed-point representations (`value * 1000`) concurrently.

**Tolerant Bounds:** UIs frequently display rounded values. Scan floating-point values via half-open interval `[v, v + 1.0)`.

**Natural Alignment:** 64-bit pointers and integers are aligned on 8-byte boundaries. Enforcing alignment eliminates spurious misaligned hits and accelerates scans.

**Page-Buffered I/O:** Reading memory candidate-by-candidate results in millions of syscall traps. Sort candidates by virtual address and read candidate clusters via whole 4 KB page reads.

### 16.3.3 Watchpoints

Watchpoints bridge the gap between "I have an ephemeral memory address" and "I have the static code instruction":

```
(lldb) watchpoint set expression -w write -s 8 -- 0x104af8010
(lldb) c
```

Upon breakpoint hit:

```
(lldb) bt
(lldb) register read
(lldb) disassemble --frame --count 16
```

The immediate offset in `mov [rax+0x308], rcx` gives the precise member offset; register `rax` holds the base pointer of the containing struct.

Hardware debug registers are scarce (typically 4 on x86/ARM). Conserve them for high-value write targets.

### 16.3.4 Handler Breakpoints

Superior to watchpoints when static analysis has already revealed candidate subroutines. Triggers deterministically on user action rather than waiting for background simulation cycles:

```
(lldb) image list -o -f | head -3        # ASLR slide
(lldb) breakpoint set -a <static_offset + slide>
(lldb) c
```

Trigger the mechanic in-game (console command, button click). The process freezes at execution point, enabling clean call stack navigation (`finish`, `disassemble --frame`) down to the field assignment.

### 16.3.5 Reverse Pointer Scanning

Used to convert a live dynamic object pointer into a perpetual pointer chain. Example: given verified `CountryBase` at `0x1a2b3c000`, locate its static origin:

1. Scan process memory for 8-byte values matching `0x1a2b3c000`.
2. Each match represents a reference. If a hit occurs at `container + 42*8`, you have identified the containing array and player index.
3. Take the container base address and repeat pointer scanning upstream.
4. Ascend the hierarchy until reaching a static image section (`__DATA` / `__data`).
5. `static_address − image_base` gives the anchor static offset.

Distinguish heap regions from image binaries using `memory region <addr>` or by checking against the Mach-O segment limits.

**Validation:** Terminate process, relaunch, resolve the newly constructed pointer chain from base. A pointer chain that fails across launches is invalid.

---

### 16.4 The Complete Engineering Cycle

```
┌─ Dynamic: Value scanning ────────────► Candidate address
│                                              │
│                                              ▼
│                              Dynamic: Write watchpoint ──► Code instruction address
│                                              │
│                                              ▼
│                        Static: Decompile routine in BN ──► Struct member offset
│                                              │             + call-site context
│                                              ▼
│                        Static: Constructor / Vtable ─────► Full class layout
│                                              │
│                                              ▼
│                   Dynamic: Reverse pointer scan ────────► Static anchor + chain
│                                              │
│                                              ▼
└──────────── Validation: Relaunch process & reapply chain ◄───┘
```

Static analysis gives structural blueprint without live placement. Dynamic analysis gives runtime memory snapshots without context. Systematically combining both is mandatory.

---

### 16.5 Verification & Falsification Discipline

Principle: **Every hypothesis must be falsifiable by an explicit experiment, executed before any dependent logic is authored.**

Hallmarks of false offsets:

- Extracted from forum posts or disparate operating systems. C++ ABI and struct padding differ substantially between MSVC and clang, across game versions, and between architecture slices.
- Unnaturally uniform padding. Real complex engine structs feature variable alignments, inheritance offsets, and bitfields. Clean 8-byte aligned tables are often fantasy.
- Never tested directly on the target binary.

Low-cost sanity checks:

- **Logical Coherence:** If `+0x2F8` represents `total` and `+0x308` represents `available`, then `total >= available`. If available reads 3.8 million while total reads 0, the hypothesis is dead.
- **Memory Mapping Boundaries:** An instantiated object occupies a continuous virtual memory region. If `base + off1` and `base + off2` land in distinct VM mappings, `base` is not an object base.
- **Reference Count:** Real entities are tracked by the engine. Zero pointer references across memory indicates stale heap garbage or a temporary stack copy.
- **Entropy & Density:** High-level entities are not zero-filled pages with isolated scattered bytes.
- **Sentinel Loops:** An address containing its own pointer points to an empty container sentinel (e.g. `libc++` empty `std::list` head `__end_`). Writing to it corrupts list integrity and causes instant crashes on iteration.

### Authoritative vs Derived Fields

A valid struct offset does not guarantee a write will stick. Engines recompute derived properties on each simulation tick from primary sources. Modifying a UI presentation cache will be discarded within milliseconds.

Persistence Test: Write a distinct value once and inspect behavior over time:

| UI Updates? | Value Persists? | Diagnosis |
|---|---|---|
| Yes | Yes | Authoritative primary field |
| Yes | No | Live simulation field, but recomputed continuously — requires active freezing |
| No | Yes | Abandoned duplicate / dead copy |
| No | No | Ephemeral UI cache overwritten by primary engine source |

Authoritative fields are identified inside the native setter routines: they represent the true source data updated when actions are legitimately processed.

---

### 16.6 Common Engineering Failures

**Scanner Self-Detection:** Scanning tool process name matches target search filter. Scanner dumps matches within its own heap. Solution: explicitly filter out `getpid()` and disambiguate naming.

**Truncated Scan Boundaries:** Heap allocations under 64-bit architectures easily exceed `0x400000000`. Enumerate valid VM regions via OS APIs rather than hardcoding memory search limits.

**Coarse Region Dumps:** Requesting monolithic 512 MB chunk reads will abort entirely if any sub-page lacks read permissions. Read memory in chunks of 1 MB with a `sizeof(T)` overlap to avoid boundary misses.

**Conflating Read Errors with Zero:** Boolean-returning read helpers that initialize outputs to zero mask memory access errors as valid zeroes. Always log OS error codes (`kern_return_t`).

**Cross-Platform Offset Pollution:** Relying on Windows PE offsets when targeting macOS Mach-O.

**Heap Recycling Deception:** Deallocated structs frequently preserve coherent member values until overwritten by new allocations. Revalidate pointer chains dynamically.

---

### 16.7 Tooling Reference

| Task | Tool |
|---|---|
| Disassembly / Decompilation | Binary Ninja, Ghidra, IDA Pro |
| Live Kernel/Userland Debugging | lldb (macOS), gdb, x64dbg (Windows) |
| Live Value Scanning | Cheat Engine (Windows), Custom `mach_vm_*` scanner (macOS) |
| Memory Region Auditing | `vmmap <pid>`, `memory region` in lldb |
| String Extraction | `strings -a`, Binary Ninja strings table |
| Architecture Verification | `lipo -info`, `file`, `ps -o arch` |
| Entitlements / Hardened Runtime | `codesign -dv --entitlements -` |

Relevant macOS Mach Virtual Memory APIs: `task_for_pid`, `mach_vm_region_recurse`, `mach_vm_read_overwrite`, `mach_vm_write`, `mach_vm_protect`.

---
---

## 17. Post-Mortem of Previous Session Failures

**Self-Scanning Trap:** A tool matching substrings of the target process attached to itself. Resolved by enforcing `getpid()` checks.

**Searching for Non-Existent Aggregates:** Top HUD manpower and resource surpluses are calculated dynamically during frame rendering. Value scanning for them yielded zero hits not because the scanner failed, but because no such static variables exist. Lesson: If a visible statistic cannot be found via value scan, investigate whether it is an unbuffered derived metric before redesigning the scanner.

**Writing to Aggregates Instead of Roots:** Country-level resource structs are reallocated and overwritten every game tick. Even a 2 ms background rewrite thread was beaten by ~13 engine ticks per second. Writes must target source inputs (per-state BASE), not cached aggregates.

**Inconsistent Array Offsets:** State resource arrays begin at index 0 with real data; country containers utilize index 0 as an empty sentinel. Assuming identical conventions resulted in shifted indices and misnamed resources across the nation.

**Iterating Unindexed Containers:** Engine access patterns for building entries rely on index translation tables accompanied by `!= -1` validity checks. Blind iteration across allocated pointer capacity yields uninitialized heap data that survives simple sanity filters. The definitive indicator: identical indices returned fluctuating garbage across identical game saves.

**Heuristic Band-Aids:** Imposing arbitrary value clamps (e.g. 0 to 1000) disguised the symptom by hiding absurd numbers, but left garbage data intact. If heuristics are required to make memory data look plausible, the structural model is wrong.

**Experimentation on Polluted State:** Sequential memory writes corrupted building grids, making it impossible to distinguish genuine game state from experimental debris. Diagnostic experiments must be executed against clean game saves, isolating one variable per run.

**Interference from Running Simulation:** Damaged buildings gradually decay during active play. Multiple tests conducted while unpaused produced false conclusions because values fluctuated due to simulation decay rather than our writes. Pausing the game engine is mandatory during diagnostic verification.
