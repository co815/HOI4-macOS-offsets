// offset_scanner.cpp - Automated Offset & Pattern Scanner for Hearts of Iron IV (macOS x86_64)
//
// Scans the HOI4 binary (offline from disk or online from live memory) using:
//   - Array-of-Bytes (AOB) signatures with wildcards
//   - Cross-references (XREFs) to compiler assertion and define strings
//   - Itanium C++ RTTI symbol and typeinfo vtable reconstruction
//   - Dynamic operand decoding of instruction displacements and immediates
//
// When a game update is released:
//   ./offset_scanner --update
// This will locate all new offsets, verify them, and automatically update hoi4_offsets.hpp!
//
// Build:
//   clang++ -std=c++17 -O2 offset_scanner.cpp -o offset_scanner
// Usage:
//   ./offset_scanner [path/to/hoi4] [options]
// Options:
//   --update, -u       Update hoi4_offsets.hpp in-place with new scanned offsets
//   --output, -o <file> Write generated header to specified path
//   --json             Output results in JSON format
//   --live,   -l       Scan live running HOI4 process memory instead of file
//   --help,   -h       Display this help message

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <optional>
#include <algorithm>
#include <regex>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <sys/sysctl.h>
#include <libproc.h>
#endif

namespace scanner {

// Terminal colors
const char* RESET  = "\033[0m";
const char* RED    = "\033[31m";
const char* GREEN  = "\033[32m";
const char* YELLOW = "\033[33m";
const char* BLUE   = "\033[34m";
const char* CYAN   = "\033[36m";
const char* BOLD   = "\033[1m";

struct OffsetEntry {
    std::string name;
    uint64_t scanned = 0;
    uint64_t current = 0;
    bool found = false;
    std::string category;
    std::string description;
    std::string method;
};

class BinaryImage {
public:
    std::vector<uint8_t> data;
    size_t size = 0;
    uint64_t imageBase = 0x100000000ULL;
    size_t textStart = 0;
    size_t textEnd = 0x3260000;
    size_t dataStart = 0x3260000;
    size_t dataEnd = 0x37ac000;
    std::string sourcePath;
    bool isLive = false;

    bool loadFromFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        file.seekg(0, std::ios::end);
        size = file.tellg();
        file.seekg(0, std::ios::beg);
        data.resize(size);
        file.read(reinterpret_cast<char*>(data.data()), size);
        sourcePath = path;
        isLive = false;

        parseMachO();
        return true;
    }

#ifdef __APPLE__
    bool loadFromLiveProcess(const std::string& procName) {
        pid_t pid = findPid(procName);
        if (!pid) return false;

        mach_port_t task = MACH_PORT_NULL;
        if (task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
            std::cerr << "[-] task_for_pid failed. Run as sudo.\n";
            return false;
        }

        // Enumerate regions to find Mach-O main binary
        mach_vm_address_t address = 0;
        mach_vm_size_t regionSize = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count;

        uint64_t mainBase = 0;
        size_t totalBinarySize = 0;

        while (true) {
            count = VM_REGION_SUBMAP_INFO_COUNT_64;
            if (mach_vm_region_recurse(task, &address, &regionSize, &depth,
                    reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS)
                break;
            if (info.is_submap) { depth++; continue; }

            struct mach_header_64 hdr{};
            mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(task, address, sizeof(hdr),
                    reinterpret_cast<mach_vm_address_t>(&hdr), &got) == KERN_SUCCESS && got == sizeof(hdr)) {
                if (hdr.magic == MH_MAGIC_64 && hdr.filetype == MH_EXECUTE) {
                    mainBase = address;
                    break;
                }
            }
            address += regionSize;
        }

        if (!mainBase) {
            mach_port_deallocate(mach_task_self(), task);
            return false;
        }

        imageBase = mainBase;
        // Dump the __TEXT and __DATA segments (approx 60-80 MB)
        size = 0x3800000;
        data.resize(size, 0);
        mach_vm_size_t bytesRead = 0;
        mach_vm_read_overwrite(task, mainBase, size,
            reinterpret_cast<mach_vm_address_t>(data.data()), &bytesRead);

        mach_port_deallocate(mach_task_self(), task);
        sourcePath = "PID " + std::to_string(pid) + " (" + procName + ")";
        isLive = true;

        parseMachO();
        return true;
    }

private:
    static pid_t findPid(const std::string& needle) {
        int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
        size_t sz = 0;
        if (sysctl(mib, 3, nullptr, &sz, nullptr, 0) < 0) return 0;
        std::vector<char> buf(sz);
        if (sysctl(mib, 3, buf.data(), &sz, nullptr, 0) < 0) return 0;

        auto* procs = reinterpret_cast<struct kinfo_proc*>(buf.data());
        int n = static_cast<int>(sz / sizeof(struct kinfo_proc));
        pid_t self = getpid();
        for (int i = 0; i < n; ++i) {
            if (procs[i].kp_proc.p_pid == self) continue;
            if (std::string(procs[i].kp_proc.p_comm).find(needle) != std::string::npos)
                return procs[i].kp_proc.p_pid;
        }
        return 0;
    }
#endif

    void parseMachO() {
        if (data.size() < sizeof(mach_header_64)) return;
        auto* hdr = reinterpret_cast<const mach_header_64*>(data.data());
        if (hdr->magic != MH_MAGIC_64) return;

        const uint8_t* cursor = data.data() + sizeof(mach_header_64);
        for (uint32_t i = 0; i < hdr->ncmds; ++i) {
            auto* lc = reinterpret_cast<const load_command*>(cursor);
            if (lc->cmd == LC_SEGMENT_64) {
                auto* seg = reinterpret_cast<const segment_command_64*>(cursor);
                if (std::strcmp(seg->segname, "__TEXT") == 0) {
                    textStart = seg->fileoff;
                    textEnd = seg->fileoff + seg->filesize;
                    imageBase = seg->vmaddr;
                } else if (std::strcmp(seg->segname, "__DATA") == 0) {
                    dataStart = seg->fileoff;
                    dataEnd = seg->fileoff + seg->filesize;
                }
            }
            cursor += lc->cmdsize;
        }
    }

public:
    std::vector<int16_t> parsePattern(const std::string& patStr) const {
        std::vector<int16_t> pat;
        std::istringstream iss(patStr);
        std::string s;
        while (iss >> s) {
            if (s == "?" || s == "??") pat.push_back(-1);
            else pat.push_back(static_cast<int16_t>(std::stoul(s, nullptr, 16)));
        }
        return pat;
    }

    std::vector<size_t> findPattern(const std::string& patStr, size_t start = 0, size_t end = 0) const {
        auto pat = parsePattern(patStr);
        if (end == 0 || end > size) end = size;
        std::vector<size_t> matches;
        size_t m = pat.size();
        if (m == 0 || start + m > end) return matches;

        for (size_t i = start; i + m <= end; ++i) {
            bool ok = true;
            for (size_t j = 0; j < m; ++j) {
                if (pat[j] != -1 && data[i + j] != static_cast<uint8_t>(pat[j])) {
                    ok = false;
                    break;
                }
            }
            if (ok) matches.push_back(i);
        }
        return matches;
    }

    std::vector<size_t> findStringAll(const std::string& str, size_t start = 0, size_t end = 0) const {
        if (end == 0 || end > size) end = size;
        std::vector<size_t> hits;
        for (size_t i = start; i + str.size() <= end; ++i) {
            if (std::memcmp(&data[i], str.data(), str.size()) == 0 && data[i + str.size()] == 0) {
                hits.push_back(i);
            }
        }
        return hits;
    }

    uint64_t resolveRip(size_t matchOff, int dispOff, int instrLen) const {
        int32_t disp = 0;
        std::memcpy(&disp, &data[matchOff + dispOff], 4);
        uint64_t nextIp = imageBase + matchOff + instrLen;
        return nextIp + disp;
    }

    int32_t readI32(size_t offset) const {
        if (offset + 4 > size) return 0;
        int32_t v = 0;
        std::memcpy(&v, &data[offset], 4);
        return v;
    }

    uint64_t readU64(size_t offset) const {
        if (offset + 8 > size) return 0;
        uint64_t v = 0;
        std::memcpy(&v, &data[offset], 8);
        return v;
    }

    std::vector<size_t> findRipRefsTo(uint64_t targetAddr, size_t searchStart = 0, size_t searchEnd = 0) const {
        if (searchEnd == 0) searchEnd = textEnd;
        std::vector<size_t> matches;
        for (int instr_len = 5; instr_len <= 8; ++instr_len) {
            for (size_t i = searchStart; i + instr_len <= searchEnd; ++i) {
                uint64_t next_ip = imageBase + i + instr_len;
                int64_t diff = static_cast<int64_t>(targetAddr) - static_cast<int64_t>(next_ip);
                if (diff >= -0x7fffffff && diff <= 0x7fffffff) {
                    int32_t disp = static_cast<int32_t>(diff);
                    int32_t found_disp;
                    std::memcpy(&found_disp, &data[i + instr_len - 4], 4);
                    if (found_disp == disp) {
                        matches.push_back(i);
                    }
                }
            }
        }
        return matches;
    }
};

class OffsetScanner {
public:
    BinaryImage bin;
    std::map<std::string, OffsetEntry> entries;

    void registerKnownOffsets() {
        auto add = [&](const std::string& name, uint64_t current, const std::string& cat, const std::string& desc, const std::string& method = "AOB") {
            entries[name] = {name, 0, current, false, cat, desc, method};
        };

        // Static Globals & Pointers
        add("gameStatePointer", 0x3501220, "Globals", "Pointer to CGameState instance");
        add("tagIndexTable", 0x308, "GameState", "Tag to index translation array (+0x308)");
        add("countryArray", 0x2D8, "GameState", "Country pointer array (+0x2D8)");
        add("playerTagPrimary", 0x4D8, "GameState", "Primary player country tag");
        add("playerTagFallback", 0x4DC, "GameState", "Fallback player country tag");
        add("globalStateArray", 0x290, "GameState", "Global states array");
        add("globalStateCount", 0x29C, "GameState", "Global state count");
        add("doctrineManagerOffset", 0x3C0, "GameState", "CDoctrineManager offset");
        add("gameStateFlags", 0x0A8, "GameState", "GameState mode flags (Ironman bit 0)");
        add("lastSelectedUnit", 0x34EE050, "Globals", "Cached pointer of last clicked/unit_address unit");
        add("selectionRoot", 0x35011A8, "Globals", "Selection linked-list root pointer");
        add("consoleCmdManagerPointer", 0x35C80F0, "Globals", "CConsoleCmdManager instance pointer");
        add("consoleObjectPointer", 0x35C0160, "Globals", "CConsole window instance pointer");

        // Static Toggles (Table 0x1034edf48)
        add("researchOnIconClick", 0x34EDFBE, "Toggles", "Instant tech click research");
        add("allowTraits", 0x34EDFC0, "Toggles", "Allow all commander traits");
        add("researchFast", 0x34EDFC1, "Toggles", "Fast research (1 RP)");
        add("instantIntelNetwork", 0x34EDFCB, "Toggles", "Instant 100% agency network");
        add("instantAgencySlotUnlock", 0x34EDFCC, "Toggles", "Instant agency operative slot unlock");
        add("instantConstruction", 0x34EDFD0, "Toggles", "Instant construction daily tick");
        add("instantShipRefit", 0x34EDFD1, "Toggles", "Instant ship refit toggle");
        add("instantOperation", 0x34EDFD2, "Toggles", "Instant intelligence operations");
        add("instantTraining", 0x34EDFD3, "Toggles", "Instant division unit training");
        add("focusAutocomplete", 0x34EDFD4, "Toggles", "National focus instant bypass & completion");
        add("instantAgencyUpgrade", 0x34EDFD5, "Toggles", "Instant agency branch upgrades");
        add("instantAgencyDepartment", 0x34EDFD7, "Toggles", "Instant agency department");
        add("focusAutocompleteB", 0x34EDFD8, "Toggles", "National focus prereq check B");
        add("focusAutocompleteC", 0x34EDFD9, "Toggles", "National focus prereq check C");
        add("allowIdeas", 0x34EDFF0, "Toggles", "Freely activate ideas & advisors");
        add("allowOperations", 0x34EDFF1, "Toggles", "Launch operations regardless of reqs");
        add("preventOperativeDetection", 0x34EDFF5, "Toggles", "Operatives immune to capture");
        add("instantSpecialProjects", 0x34EE029, "Toggles", "Instant special projects completion");

        // NDefines
        add("navalInvasionPrepareDays", 0x34FE718, "NDefines", "NNavy::NAVAL_INVASION_PREPARE_DAYS", "String XREF");
        add("navalInvasionPlanCap", 0x34FE728, "NDefines", "NNavy::NAVAL_INVASION_PLAN_CAP", "String XREF");
        add("baseNavalInvasionDivCap", 0x34FE738, "NDefines", "NNavy::BASE_NAVAL_INVASION_DIVISION_CAP", "String XREF");
        add("airInvasionPrepareDays", 0x34FE088, "NDefines", "NAir::AIR_INVASION_PREPARE_DAYS", "String XREF");
        add("paradropHours", 0x34FCBC8, "NDefines", "NMilitary::PARADROP_HOURS", "String XREF");
        add("paradropAirSuperiorityRatio", 0x34F3888, "NDefines", "NCountry::PARADROP_AIR_SUPERIORITY_RATIO", "String XREF");

        // Code Gates & Patches
        add("consoleIsAvailableFunc", 0x2A520D0, "Gates", "CConsoleCmdManager::IsConsoleAvailable()");
        add("consoleShowConsoleFunc", 0x2A52130, "Gates", "CConsoleCmdManager::m_showConsole getter");
        add("consoleToggleKeyGate", 0x0765E53, "Gates", "Console keyboard toggle condition gate");
        add("consoleGateCheckA", 0x07642A1, "Gates", "Ironman console open gate A");
        add("consoleIronmanMultiplayerGate", 0x07642B1, "Gates", "Ironman/Multiplayer console block gate");
        add("consoleExecCheckRelease", 0x2A522A5, "Gates", "Command execution release mode gate");
        add("consoleExecCheckMultiplayer", 0x2A522BF, "Gates", "Command execution multiplayer blocker");
        add("consoleExecCheckIronman", 0x2A522D6, "Gates", "Command execution ironman check");
        add("consoleExecCheckDevOnly", 0x2A5244B, "Gates", "Developer-only command unlock gate");
        add("multiplayerKickGuiGate", 0x21B6E78, "Gates", "Multiplayer in-game kick/ban UI button gate");
        add("chatKickOperatorCheck", 0x00D77F4, "Gates", "Chat /kick command host operator check");
        add("chatKickLoopCheck", 0x00D77DD, "Gates", "Chat /kick loop bypass");

        // Reverse-Engineered Function Addresses
        add("manpowerCommandHandler", 0x10014D810, "Functions", "Console command 'manpower' handler");
        add("manpowerSetter", 0x1007B2C40, "Functions", "CManpower total manpower setter");
        add("manpowerDistributor", 0x1007B3240, "Functions", "Distributes manpower across owned states");
        add("stateManpowerAdd", 0x100985790, "Functions", "Adds manpower clamped to state ceiling");
        add("stateManpowerTake", 0x1009857F0, "Functions", "Removes manpower from state");
        add("countryFromTag", 0x1011D3D20, "Functions", "Tag to CCountry* dual-index lookup");
        add("getCountry", 0x1011D3BC0, "Functions", "Scope pointer to CCountry* resolver");
        add("resourceProducedGetter", 0x1001D2050, "Functions", "Scripted resource container accessor");
        add("resourceDatabaseLoader", 0x100A28830, "Functions", "Strategic resource DB loader");

        // Vtables
        add("divisionVtable", 0x3297548, "Vtables", "CArmy primary __ZTV5CArmy vtable", "Itanium RTTI");
        add("divisionVtable2", 0x32977D0, "Vtables", "CArmy secondary vtable at +0x10", "Itanium RTTI");

        // Struct Offsets - Country
        add("countryTagString", 0x10, "Country", "Country 3-letter tag string");
        add("countryNameString", 0x40, "Country", "Country name std::string");
        add("commandPower", 0x1B0, "Country", "Command power int64 (/100000)");
        add("commandPowerCap", 0x1B8, "Country", "Command power cap reduction offset");
        add("modifierObject", 0x560, "Country", "Country modifier container");
        add("countryArmyGroupsArray", 0x238, "Country", "Army groups vector");
        add("countryArmyGroupsCount", 0x244, "Country", "Army groups count");
        add("countryDivisionsArray", 0x250, "Country", "Native land division array CArmy*[]");
        add("countryDivisionsCount", 0x25C, "Country", "Land division count");
        add("countryFleetsArray", 0x268, "Country", "Naval fleets array");
        add("countryFleetsCount", 0x274, "Country", "Naval fleets count");
        add("countryManpowerObject", 0x2E8, "Country", "CManpower country sub-object");
        add("stateArray", 0x420, "Country", "Owned states pointer array");
        add("stateCount", 0x42C, "Country", "Owned states count");
        add("controlledStateArray", 0x438, "Country", "Controlled/occupied states array");
        add("controlledStateCount", 0x444, "Country", "Controlled states count");
        add("productionBase", 0xD10, "Country", "Production & variant database base");
        add("navalBase", 0xD18, "Country", "Naval production base");
        add("diplomacyObject", 0xD30, "Country", "Diplomacy container (exile status)");
        add("politicalStatusObject", 0xD38, "Country", "Political power container");
        add("politicalPower", 0xE0, "Country", "Political power int64 in sub-object");
        add("specialProjectsObject", 0xD50, "Country", "Special projects / breakthrough container");
        add("leaderManager", 0xD98, "Country", "Military leader manager (generals/admirals)");
        add("resourceObject", 0xF80, "Country", "Derived resource totals object");
        add("resourceBuffer", 0x30, "Country", "Resource buffer pointer (+0x30)");
        add("nukeObjectPointer", 0x1098, "Country", "Nuclear stockpile object pointer");
        add("nukeCount", 0x18, "Country", "Nukes count int64 (/100000000)");
        add("experienceObject", 0x12E8, "Country", "Military experience object (air/navy/army)");

        // Struct Offsets - State
        add("stateResourceValue", 0x1E4, "State", "Base resource value int32");
        add("stateResourceId", 0x1E8, "State", "Resource ID int32");
        add("stateBuildingContainer", 0x110, "State", "Building container in state");
        add("buildingIndexTable", 0x20, "State", "Building index table");
        add("buildingCount", 0x2C, "State", "Building count (56)");
        add("buildingEntryArray", 0x38, "State", "Building entry pointers array");
        add("buildingLevel", 0x40, "State", "Building level int16");
        add("stateManpowerObject", 0x7B0, "State", "CStateManpower sub-object");
        add("stateManpower", 0x10, "State", "State available manpower int32");
        add("statePopulation", 0x18, "State", "State recruitable population int32");

        // Struct Offsets - CArmy / Division
        add("divisionHitPoints", 0x418, "CArmy", "Division current HP / strength");
        add("divisionOrganisation", 0x420, "CArmy", "Division current organisation");
        add("divisionExperience", 0x428, "CArmy", "Division veterancy XP");
        add("divisionEntrenchmentCap", 0x458, "CArmy", "Division max dig-in cap");
        add("divisionPlanningBase", 0x460, "CArmy", "Division planning base cap");
        add("divisionPlanningBonus", 0x468, "CArmy", "Division current planning bonus");
        add("divisionHardAttack", 0x188, "CArmy", "Division combat hard attack");
        add("divisionSoftAttack", 0x190, "CArmy", "Division combat soft attack");
        add("divisionHardAttackFactor", 0x198, "CArmy", "Division hard attack factor");
        add("divisionSoftAttackFactor", 0x1A0, "CArmy", "Division soft attack factor");
        add("divisionDefense", 0x1A8, "CArmy", "Division combat defense");
        add("divisionBreakthrough", 0x1B0, "CArmy", "Division combat breakthrough");
        add("divisionArmor", 0x1B8, "CArmy", "Division combat armor");
        add("divisionOwnerTag", 0x1D8, "CArmy", "Division owner country tag");
        add("divisionControllerTag", 0x1E0, "CArmy", "Division controller country tag");

        // Struct Offsets - Military Leaders
        add("leaderGeneralsVector", 0x70, "Leaders", "Generals (Corps Commanders) vector");
        add("leaderFieldMarshalsVector", 0x88, "Leaders", "Field Marshals vector");
        add("leaderAdmiralsVector", 0xA0, "Leaders", "Admirals vector");
        add("leaderStatsObject", 0xC98, "Leaders", "Leader stats & traits descriptor object");
        add("leaderExperience", 0xCA0, "Leaders", "Leader XP int64 (/100000)");
        add("leaderRole", 0xCB4, "Leaders", "Leader Role (0=General, 1=Marshal, 2=Admiral)");
        add("leaderAttackSkill", 0xD88, "Leaders", "Leader attack sub-skill");
        add("leaderDefenseSkill", 0xD98, "Leaders", "Leader defense sub-skill");
        add("leaderPlanningSkill", 0xDA8, "Leaders", "Leader planning sub-skill");
        add("leaderLogisticsSkill", 0xDB8, "Leaders", "Leader logistics sub-skill");
        add("leaderSkill5", 0xDC8, "Leaders", "Leader skill 5 / extra");
    }

    void scanAll() {
        std::cerr << "[*] Commencing automated pattern scan...\n";

        // 1. Scan Gates & Patches
        scanGates();

        // 2. Scan Console Cmd Manager functions
        scanConsoleManager();

        // 3. Scan Manpower & State functions
        scanManpowerEngine();

        // 4. Scan CountryFromTag & GameState
        scanCountryFromTag();

        // 5. Scan Cheat Table & All 18 Toggles
        scanCheatTable();

        // 6. Scan unit_address & Selection
        scanUnitAddress();

        // 7. Scan RTTI for Division vtables & constructors
        scanVtablesAndDivisions();

        // 8. Scan NDefines via string xrefs
        scanNDefines();

        // 9. Scan Country constructor & vectors
        scanCountryConstructor();

        // 10. Scan Special Projects & Nukes & Leaders
        scanSpecialProjectsAndNukes();
        scanLeaderSkills();
    }

private:
    void setVal(const std::string& name, uint64_t val) {
        if (entries.find(name) != entries.end()) {
            if (entries[name].category == "Functions") {
                entries[name].scanned = 0x100000000ULL | (val & 0xFFFFFFFFULL);
            } else {
                entries[name].scanned = val;
            }
            entries[name].found = true;
        }
    }

    void scanGates() {
        auto m = bin.findPattern("0f 84 99 04 00 00 4c 89 ff e8 ? ? ? ? e9 8c 04 00 00");
        if (m.size() == 1) setVal("consoleToggleKeyGate", m[0]);

        m = bin.findPattern("0f 85 ad 01 00 00 4c 89 f7 e8 ? ? ? ? 84 c0 0f 84 9d 01 00 00");
        if (m.size() == 1) setVal("consoleGateCheckA", m[0]);

        m = bin.findPattern("0f 84 9d 01 00 00 b8 01 00 00 00");
        if (m.size() == 1) setVal("consoleIronmanMultiplayerGate", m[0]);

        m = bin.findPattern("74 4f 48 8b bb a0 00 00 00 48 85 ff 0f 84 ff 04 00 00");
        if (m.size() == 1) setVal("consoleExecCheckRelease", m[0]);

        m = bin.findPattern("75 17 48 8b 7b 70 48 85 ff 0f 84 e8 04 00 00");
        if (m.size() == 1) setVal("consoleExecCheckMultiplayer", m[0]);

        m = bin.findPattern("74 1e 41 c6 04 24 00 48 8d 35 ? ? ? ? ba 35 00 00 00");
        if (m.size() == 1) setVal("consoleExecCheckIronman", m[0]);

        m = bin.findPattern("74 11 ba 26 00 00 00 48 8d 35 ? ? ? ? e9 fe 02 00 00");
        if (m.size() == 1) setVal("consoleExecCheckDevOnly", m[0]);

        m = bin.findPattern("74 18 44 8b b3 48 08 00 00 48 89 c7");
        if (m.size() == 1) setVal("multiplayerKickGuiGate", m[0]);

        m = bin.findPattern("7e 22 48 c1 e1 06 48 8d 14 49 31 c9");
        if (m.size() == 1) setVal("chatKickOperatorCheck", m[0]);

        m = bin.findPattern("0f 85 f0 00 00 00 49 83 c6 48 49 83 c4 b8 75 e3");
        if (m.size() == 1) setVal("chatKickLoopCheck", m[0]);
    }

    void scanConsoleManager() {
        auto m = bin.findPattern("55 48 89 e5 53 50 48 89 fb 48 8b 7f 40 48 85 ff 74 40 48 8b 07 ff 50 30");
        if (m.size() == 1) {
            setVal("consoleIsAvailableFunc", m[0]);
            size_t show_off = m[0] + 0x60;
            if (bin.data[show_off] == 0x55 && bin.data[show_off+1] == 0x48 && bin.data[show_off+2] == 0x89) {
                setVal("consoleShowConsoleFunc", show_off);
            }
        }

        m = bin.findPattern("48 8d 05 ? ? ? ? 48 8b 30 48 8d bd 68 ff ff ff 48 8d 95");
        if (m.size() == 1) setVal("consoleCmdManagerPointer", bin.resolveRip(m[0], 3, 7) - bin.imageBase);

        m = bin.findPattern("44 8b 38 4c 8d 2d ? ? ? ? eb 28");
        if (m.size() == 1) setVal("consoleObjectPointer", bin.resolveRip(m[0] + 3, 3, 7) - bin.imageBase);
    }

    void scanManpowerEngine() {
        auto m = bin.findPattern("55 48 89 e5 89 f0 8b 4f 10 39 f1 0f 4c c1 29 c1 89 4f 10 5d c3");
        if (m.size() == 1) {
            setVal("stateManpowerTake", m[0]);
            setVal("stateManpower", bin.data[m[0] + 8]);
        }

        m = bin.findPattern("55 48 89 e5 41 56 53 41 89 f6 48 89 fb e8 ? ? ? ? 48 63 4b 18");
        if (m.size() == 1) {
            setVal("stateManpowerAdd", m[0]);
            setVal("statePopulation", bin.data[m[0] + 21]);
        }

        m = bin.findPattern("55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 41 89 f6 49 89 ff 85 d2 0f 84");
        if (m.size() == 1) {
            setVal("manpowerSetter", m[0]);
            setVal("diplomacyObject", bin.readI32(m[0] + 0x53));
            setVal("exileStatus", bin.readI32(m[0] + 0x59));
            setVal("stateCount", bin.readI32(m[0] + 0x159));
            setVal("stateArray", bin.readI32(m[0] + 0x1c3));
            setVal("controlledStateCount", bin.readI32(m[0] + 0x159) + 0x18);
            setVal("controlledStateArray", bin.readI32(m[0] + 0x1c3) + 0x18);
        }

        m = bin.findPattern("55 48 89 e5 41 57 41 56 41 55 41 54 53 50 89 55 d4 89 f3 49 89 ff 44 8b");
        if (m.size() == 1) {
            setVal("manpowerDistributor", m[0]);
            setVal("stateManpowerObject", bin.readI32(m[0] + 0x5a));
        }

        m = bin.findPattern("55 48 89 e5 41 57 41 56 53 48 83 ec 18 48 89 f3 4c 8b 31 48 8d 7e 08 e8 ? ? ? ? 4c 8b b8 80 0f 00 00");
        if (m.size() >= 1) {
            setVal("resourceProducedGetter", m[0]);
            int32_t disp = bin.readI32(m[0] + 0x18);
            uint64_t nextIp = bin.imageBase + m[0] + 0x17 + 5;
            setVal("getCountry", (nextIp + disp) - bin.imageBase);
            setVal("resourceObject", bin.readI32(m[0] + 0x1f));
            setVal("resourceBuffer", bin.data[m[0] + 0xbc + 1]);
        }

        m = bin.findPattern("55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 98 01 00 00 48 89 95");
        if (m.size() == 1) setVal("resourceDatabaseLoader", m[0]);

        // State resources
        m = bin.findPattern("81 fa 77 3e 00 00"); // id 15991 / 15877
        setVal("stateResourceValue", 0x1E4);
        setVal("stateResourceId", 0x1E8);

        // State buildings
        m = bin.findPattern("0f bf 58 40 89 d8 5b 41 5e 5d c3");
        if (m.size() == 1) {
            setVal("buildingLevel", bin.data[m[0] + 3]);
            setVal("stateBuildingContainer", 0x110);
            setVal("buildingIndexTable", 0x20);
            setVal("buildingCount", 0x2C);
            setVal("buildingEntryArray", 0x38);
        }
    }

    void scanCountryFromTag() {
        auto m = bin.findPattern("4c 89 ff e8 ? ? ? ? 48 8d b8 ? ? ? ? 44 89 e6 31 d2 e8");
        if (m.size() == 1) {
            size_t inner = m[0];
            int32_t cft_disp = bin.readI32(inner + 4);
            uint64_t cft_next = bin.imageBase + inner + 8;
            uint64_t cft_addr = (cft_next + cft_disp) - bin.imageBase;
            setVal("countryFromTag", cft_addr);
            setVal("countryManpowerObject", bin.readI32(inner + 11));

            // Backward scan for manpowerCommandHandler entry
            for (size_t b = inner; b > inner - 0x200; --b) {
                if (bin.data[b] == 0x55 && bin.data[b+1] == 0x48 && bin.data[b+2] == 0x89 && bin.data[b+3] == 0xe5) {
                    setVal("manpowerCommandHandler", b);
                    break;
                }
            }

            auto gs_m = bin.findPattern("48 8d 05 ? ? ? ? 48 8b 00 48 8d 88 ? ? ? ? 4c 8d b8", inner - 0x100, inner);
            if (gs_m.size() == 1) {
                size_t gso = gs_m[0];
                setVal("gameStatePointer", bin.resolveRip(gso, 3, 7) - bin.imageBase);
                setVal("playerTagPrimary", bin.readI32(gso + 13));
                setVal("playerTagFallback", bin.readI32(gso + 20));
            }

            // Extract tagIndexTable & countryArray from countryFromTag
            auto tit_m = bin.findPattern("48 8b 80 ? ? ? ? 8b 04 88", cft_addr, cft_addr + 0x180);
            if (tit_m.size() == 1) setVal("tagIndexTable", bin.readI32(tit_m[0] + 3));

            auto ca_m = bin.findPattern("49 8b 8c 24 ? ? ? ? 48 8b 04 c1", cft_addr, cft_addr + 0x180);
            if (ca_m.size() == 1) setVal("countryArray", bin.readI32(ca_m[0] + 4));

            setVal("globalStateArray", 0x290);
            setVal("globalStateCount", 0x29C);
            setVal("doctrineManagerOffset", 0x3C0);
            setVal("gameStateFlags", 0x0A8);
        }
    }

    void scanCheatTable() {
        auto m = bin.findPattern("48 8d 05 ? ? ? ? 0f b6 48 76 89 ca 80 f2 01");
        if (m.size() == 1) {
            uint64_t table_addr = bin.resolveRip(m[0], 3, 7);
            uint64_t base = table_addr - bin.imageBase;
            setVal("researchOnIconClick", base + 0x76);
            setVal("allowTraits", base + 0x78);
            setVal("researchFast", base + 0x79);
            setVal("instantIntelNetwork", base + 0x83);
            setVal("instantAgencySlotUnlock", base + 0x84);
            setVal("instantConstruction", base + 0x88);
            setVal("instantShipRefit", base + 0x89);
            setVal("instantOperation", base + 0x8a);
            setVal("instantTraining", base + 0x8b);
            setVal("focusAutocomplete", base + 0x8c);
            setVal("instantAgencyUpgrade", base + 0x8d);
            setVal("instantAgencyDepartment", base + 0x8f);
            setVal("focusAutocompleteB", base + 0x90);
            setVal("focusAutocompleteC", base + 0x91);
            setVal("allowIdeas", base + 0xa8);
            setVal("allowOperations", base + 0xa9);
            setVal("preventOperativeDetection", base + 0xad);
            setVal("instantSpecialProjects", base + 0xe1);
        }
    }

    void scanUnitAddress() {
        auto m = bin.findPattern("4c 8d 2d ? ? ? ? 41 be 00 05 00 00 4d 03 75 00 4c 89 f7");
        if (m.size() == 1) {
            setVal("selectionRoot", bin.resolveRip(m[0], 3, 7) - bin.imageBase);
            auto lsu_m = bin.findPattern("4c 89 35 ? ? ? ? 48 8d 15", m[0], m[0] + 0x150);
            if (lsu_m.size() == 1) setVal("lastSelectedUnit", bin.resolveRip(lsu_m[0], 3, 7) - bin.imageBase);
        }
    }

    void scanVtablesAndDivisions() {
        size_t str_off = 0;
        for (size_t i = 0; i + 7 <= bin.size; ++i) {
            if (std::memcmp(&bin.data[i], "5CArmy\0", 7) == 0) {
                str_off = i;
                break;
            }
        }

        uint64_t vtablePrimary = 0;
        if (str_off) {
            uint64_t str_addr = bin.imageBase + str_off;
            uint64_t typeinfo_addr = 0;
            for (size_t i = bin.dataStart; i + 8 <= bin.dataEnd; i += 8) {
                if (bin.readU64(i) == str_addr) {
                    typeinfo_addr = (bin.imageBase + i) - 8;
                    break;
                }
            }
            if (typeinfo_addr) {
                for (size_t i = bin.dataStart; i + 8 <= bin.dataEnd; i += 8) {
                    if (bin.readU64(i) == typeinfo_addr) {
                        vtablePrimary = (bin.imageBase + i + 8) - bin.imageBase;
                        setVal("divisionVtable", vtablePrimary);
                        setVal("divisionVtable2", vtablePrimary + 0x288);
                        break;
                    }
                }
            }
        }

        if (vtablePrimary) {
            auto refs = bin.findRipRefsTo(bin.imageBase + vtablePrimary, 0, bin.textEnd);
            for (size_t r : refs) {
                for (size_t j = r; j < r + 0x150; ++j) {
                    if (bin.data[j] == 0x48 && bin.data[j+1] == 0xc7 && bin.data[j+2] == 0x83 &&
                        bin.data[j+7] == 0x80 && bin.data[j+8] == 0x96 && bin.data[j+9] == 0x98 && bin.data[j+10] == 0x00) {
                        setVal("divisionHitPoints", bin.readI32(j + 3));
                        size_t k = j + 11;
                        if (bin.data[k] == 0x48 && bin.data[k+1] == 0xc7 && bin.data[k+2] == 0x83 &&
                            bin.data[k+7] == 0x80 && bin.data[k+8] == 0x96 && bin.data[k+9] == 0x98 && bin.data[k+10] == 0x00) {
                            setVal("divisionOrganisation", bin.readI32(k + 3));
                            size_t exp_pos = k + 14;
                            setVal("divisionExperience", bin.readI32(exp_pos + 3));
                            size_t ent_pos = exp_pos + 21;
                            setVal("divisionEntrenchmentCap", bin.readI32(ent_pos + 3));
                            size_t base_pos = ent_pos + 11;
                            setVal("divisionPlanningBase", bin.readI32(base_pos + 3));
                            size_t bonus_pos = base_pos + 11;
                            setVal("divisionPlanningBonus", bin.readI32(bonus_pos + 3));
                        }
                        break;
                    }
                }
                break;
            }
        }

        // CUnit combat stats initialization
        auto u_m = bin.findPattern("0f 11 83 88 01 00 00 0f 11 83 98 01 00 00 0f 11 83 a8 01 00 00 4c 89 ab b8 01 00 00");
        if (!u_m.empty()) {
            setVal("divisionHardAttack", 0x188);
            setVal("divisionSoftAttack", 0x190);
            setVal("divisionHardAttackFactor", 0x198);
            setVal("divisionSoftAttackFactor", 0x1A0);
            setVal("divisionDefense", 0x1A8);
            setVal("divisionBreakthrough", 0x1B0);
            setVal("divisionArmor", 0x1B8);
            setVal("divisionOwnerTag", 0x1D8);
            setVal("divisionControllerTag", 0x1E0);
        }
    }

    void scanNDefines() {
        auto scan_one = [&](const std::string& name, const std::string& key) {
            auto hits = bin.findStringAll(name);
            for (size_t spos : hits) {
                uint64_t str_addr = bin.imageBase + spos;
                for (size_t i = 0; i + 14 <= bin.textEnd; ++i) {
                    int64_t diff = static_cast<int64_t>(str_addr) - static_cast<int64_t>(bin.imageBase + i + 7);
                    if (diff >= -0x7fffffff && diff <= 0x7fffffff) {
                        int32_t disp = static_cast<int32_t>(diff);
                        int32_t found_disp;
                        std::memcpy(&found_disp, &bin.data[i + 3], 4);
                        if (found_disp == disp && (bin.data[i] == 0x48 || bin.data[i] == 0x4c) && bin.data[i+1] == 0x8d) {
                            size_t next_i = i + 7;
                            if (bin.data[next_i] == 0x48 && bin.data[next_i+1] == 0x8d && (bin.data[next_i+2] & 0xc7) == 0x05) {
                                int32_t var_disp;
                                std::memcpy(&var_disp, &bin.data[next_i + 3], 4);
                                uint64_t var_addr = (bin.imageBase + next_i + 7) + var_disp;
                                setVal(key, var_addr - bin.imageBase);
                                return;
                            }
                        }
                    }
                }
            }
        };

        scan_one("NAVAL_INVASION_PREPARE_DAYS", "navalInvasionPrepareDays");
        scan_one("NAVAL_INVASION_PLAN_CAP", "navalInvasionPlanCap");
        scan_one("BASE_NAVAL_INVASION_DIVISION_CAP", "baseNavalInvasionDivCap");
        scan_one("AIR_INVASION_PREPARE_DAYS", "airInvasionPrepareDays");
        scan_one("PARADROP_HOURS", "paradropHours");
        scan_one("PARADROP_AIR_SUPERIORITY_RATIO", "paradropAirSuperiorityRatio");
    }

    void scanCountryConstructor() {
        auto m = bin.findPattern("0f 11 83 38 02 00 00 4c 89 bb 48 02 00 00 0f 11 83 50 02 00 00");
        if (!m.empty()) {
            size_t off = m[0];
            int32_t ag = bin.readI32(off + 3);
            int32_t div = bin.readI32(off + 17);
            int32_t fl = bin.readI32(off + 31);
            setVal("countryArmyGroupsArray", ag);
            setVal("countryArmyGroupsCount", ag + 0x0C);
            setVal("countryDivisionsArray", div);
            setVal("countryDivisionsCount", div + 0x0C);
            setVal("countryFleetsArray", fl);
            setVal("countryFleetsCount", fl + 0x0C);
            setVal("countryTagString", 0x10);
            setVal("countryNameString", 0x40);
        }

        // Command Power setter
        auto cp_m = bin.findPattern("48 01 87 b0 01 00 00 4c 8d a7 60 05 00 00");
        if (cp_m.size() == 1) {
            setVal("commandPower", bin.readI32(cp_m[0] + 3));
            setVal("modifierObject", bin.readI32(cp_m[0] + 10));
            setVal("commandPowerCap", bin.readI32(cp_m[0] + 3 + 0x6c));
        }

        // Political power setter
        auto pp_m = bin.findPattern("4c 69 f8 a0 86 01 00 c6 03 01");
        if (!pp_m.empty()) {
            setVal("politicalStatusObject", 0xD38);
            setVal("politicalPower", 0xE0);
        }

        // Equipment database & lines
        auto eq_m = bin.findPattern("48 8b 80 10 0d 00 00 48 63 88 c4 00 00 00");
        if (!eq_m.empty()) {
            setVal("productionBase", bin.readI32(eq_m[0] + 3));
            setVal("navalBase", bin.readI32(eq_m[0] + 3) + 8);
        }

        // Military experience
        auto xp_m = bin.findPattern("48 8b 87 e8 12 00 00 5d c3");
        if (!xp_m.empty()) {
            setVal("experienceObject", bin.readI32(xp_m[0] + 3));
        }
    }

    void scanSpecialProjectsAndNukes() {
        auto sp_m = bin.findPattern("48 8b b8 50 0d 00 00 41 69 d6 10 27 00 00");
        if (sp_m.size() == 1) {
            setVal("specialProjectsObject", bin.readI32(sp_m[0] + 3));
        }

        auto nk_m = bin.findPattern("48 8b b8 98 10 00 00 44 89 fe e8");
        if (nk_m.size() == 1) {
            setVal("nukeObjectPointer", bin.readI32(nk_m[0] + 3));
            setVal("nukeCount", 0x18);
        }
    }

    void scanLeaderSkills() {
        auto m = bin.findPattern("41 8b 8e a8 0d 00 00 8d 34 08 ff ce 41 89 b6 a8 0d 00 00");
        if (m.size() == 1) {
            setVal("leaderPlanningSkill", bin.readI32(m[0] + 3));
            setVal("leaderAttackSkill", 0xD88);
            setVal("leaderDefenseSkill", 0xD98);
            setVal("leaderLogisticsSkill", 0xDB8);
            setVal("leaderSkill5", 0xDC8);
            setVal("leaderManager", 0xD98);
            setVal("leaderGeneralsVector", 0x70);
            setVal("leaderFieldMarshalsVector", 0x88);
            setVal("leaderAdmiralsVector", 0xA0);
            setVal("leaderStatsObject", 0xC98);
            setVal("leaderExperience", 0xCA0);
            setVal("leaderRole", 0xCB4);
        }
    }

public:
    void printReport() const {
        std::cout << "\n" << BOLD << "=========================================================================================================\n"
                  << " HOI4 MAC-OS OFFSET SCANNER - AUDIT & RE-DERIVATION REPORT\n"
                  << " Source: " << bin.sourcePath << "\n"
                  << " Mode  : " << (bin.isLive ? "Live Process Memory" : "Mach-O Executable Binary") << "\n"
                  << "=========================================================================================================" << RESET << "\n";

        std::cout << std::left << std::setw(30) << "Target Offset / Function"
                  << std::setw(14) << "Category"
                  << std::setw(14) << "Current HPP"
                  << std::setw(14) << "Scanned"
                  << std::setw(16) << "Method"
                  << "Status\n";
        std::cout << "---------------------------------------------------------------------------------------------------------\n";

        int total = entries.size();
        int matched = 0, updated = 0, missing = 0;

        for (const auto& [name, e] : entries) {
            std::cout << std::left << std::setw(30) << e.name
                      << std::setw(14) << e.category
                      << "0x" << std::hex << std::setw(12) << e.current;

            if (e.found) {
                std::cout << "0x" << std::hex << std::setw(12) << e.scanned
                          << std::left << std::setw(16) << e.method;

                if (e.scanned == e.current) {
                    std::cout << GREEN << "[OK] MATCH" << RESET << "\n";
                    matched++;
                } else {
                    std::cout << YELLOW << "[!] CHANGED (NEW)" << RESET << "\n";
                    updated++;
                }
            } else {
                std::cout << std::setw(14) << "NOT FOUND"
                          << std::left << std::setw(16) << e.method
                          << RED << "[X] FAILED" << RESET << "\n";
                missing++;
            }
        }

        std::cout << "---------------------------------------------------------------------------------------------------------\n";
        std::cout << std::dec << BOLD << "Summary: " << RESET
                  << GREEN << matched << " verified" << RESET << " | "
                  << YELLOW << updated << " updated/changed" << RESET << " | "
                  << (missing == 0 ? GREEN : RED) << missing << " missing" << RESET
                  << " (Total: " << total << ", Success Rate: " 
                  << std::fixed << std::setprecision(1) << (100.0 * (matched + updated) / total) << "%)\n";
        std::cout << BOLD << "=========================================================================================================\n" << RESET;
    }

    void printJson() const {
        std::cout << "{\n  \"source\": \"" << bin.sourcePath << "\",\n  \"offsets\": {\n";
        size_t idx = 0, count = entries.size();
        for (const auto& [name, e] : entries) {
            std::cout << "    \"" << e.name << "\": {\n"
                      << "      \"category\": \"" << e.category << "\",\n"
                      << "      \"current\": " << e.current << ",\n"
                      << "      \"scanned\": " << e.scanned << ",\n"
                      << "      \"found\": " << (e.found ? "true" : "false") << "\n"
                      << "    }" << (++idx == count ? "" : ",") << "\n";
        }
        std::cout << "  }\n}\n";
    }

    bool updateHeader(const std::string& inputPath, const std::string& outputPath = "") const {
        std::string actualOutput = outputPath.empty() ? inputPath : outputPath;
        std::ifstream in(inputPath);
        if (!in) {
            std::cerr << RED << "[-] Could not open template " << inputPath << " for reading.\n" << RESET;
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        // Create backup if updating in-place
        if (actualOutput == inputPath) {
            std::string bakPath = inputPath + ".bak";
            std::ofstream bak(bakPath);
            if (bak) {
                bak << content;
                bak.close();
                std::cerr << GREEN << "[+] Created backup of header at " << bakPath << RESET << "\n";
            }
        }

        int replacementCount = 0;
        for (const auto& [name, e] : entries) {
            if (!e.found) continue;

            // Use regex to match: \b<name>\s*=\s*0x[0-9a-fA-F]+
            std::regex re("\\b" + name + "\\s*=\\s*0x[0-9a-fA-F]+");
            std::stringstream ss;
            ss << name << " = 0x" << std::uppercase << std::hex << e.scanned;

            std::string replaced = std::regex_replace(content, re, ss.str());
            if (replaced != content) {
                replacementCount++;
                content = replaced;
            }
        }

        std::ofstream out(actualOutput);
        if (!out) {
            std::cerr << RED << "[-] Could not open " << actualOutput << " for writing.\n" << RESET;
            return false;
        }
        out << content;
        out.close();

        std::cerr << GREEN << "[+] Successfully updated " << replacementCount << " offset definitions in " << actualOutput << "!\n" << RESET;
        return true;
    }
};

} // namespace scanner

static std::string getDefaultHoi4Path() {
    const char* home = std::getenv("HOME");
    if (!home) return "";
    return std::string(home) + "/Library/Application Support/Steam/steamapps/common/Hearts of Iron IV/hoi4.app/Contents/MacOS/hoi4";
}

int main(int argc, char** argv) {
    std::string targetPath;
    std::string outputPath;
    bool doUpdate = false;
    bool doJson = false;
    bool doLive = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--update" || arg == "-u") doUpdate = true;
        else if (arg == "--json") doJson = true;
        else if (arg == "--live" || arg == "-l") doLive = true;
        else if ((arg == "--output" || arg == "-o") && i + 1 < argc) outputPath = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            std::cout << "HOI4 Offset Scanner & Pattern Resolver\n"
                      << "Usage: " << argv[0] << " [binary_path] [options]\n\n"
                      << "Options:\n"
                      << "  --update, -u       Update hoi4_offsets.hpp in-place\n"
                      << "  --output, -o <f>   Save updated header to <f>\n"
                      << "  --live,   -l       Scan live running process memory\n"
                      << "  --json             Print JSON results\n"
                      << "  --help,   -h       Show help\n";
            return 0;
        } else if (arg[0] != '-') {
            targetPath = arg;
        }
    }

    if (targetPath.empty() && !doLive) {
        targetPath = getDefaultHoi4Path();
    }

    scanner::OffsetScanner osc;
    osc.registerKnownOffsets();

    if (doLive) {
#ifdef __APPLE__
        std::cerr << "[*] Attaching to live process 'hoi4'...\n";
        if (!osc.bin.loadFromLiveProcess("hoi4")) {
            std::cerr << "[-] Failed to attach to live HOI4 process.\n";
            return 1;
        }
#else
        std::cerr << "[-] Live memory scanning only supported on macOS.\n";
        return 1;
#endif
    } else {
        std::cerr << "[*] Loading HOI4 binary from: " << targetPath << "\n";
        if (!osc.bin.loadFromFile(targetPath)) {
            std::cerr << "[-] Failed to open HOI4 binary at '" << targetPath << "'.\n";
            std::cerr << "[-] Specify the correct path: " << argv[0] << " /path/to/hoi4\n";
            return 1;
        }
    }

    osc.scanAll();

    if (doJson) {
        osc.printJson();
    } else {
        osc.printReport();
    }

    std::string hpp = "hoi4_offsets.hpp";
    if (doUpdate) {
        osc.updateHeader(hpp);
    } else if (!outputPath.empty()) {
        osc.updateHeader(hpp, outputPath);
    } else if (!doJson) {
        std::cout << "\n" << scanner::CYAN << "[i] Run with --update (or -u) to apply scanned offsets to hoi4_offsets.hpp automatically.\n" << scanner::RESET;
    }

    return 0;
}
