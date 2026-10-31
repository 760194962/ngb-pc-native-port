// Per-build facts the loader needs before it can run an XBE: which build it is (SHA-256 of default.xbe), how many
// fs:[KPCR] accesses the fs patcher should find, where the CRT initializer tables are (their entries are library
// code that must run natively), which XBE sections hold the libraries replaced by HLE, and whether the BLACK-only
// game patches apply. Everything else (section layout, kernel imports, entry point) is read from the XBE itself.
//
// Builds: BLACK NTSC-U (the original black-pc target) and NINJA GAIDEN BLACK "Hurricane" vb-zh-x2.1 (this fork).
#pragma once

#include <cstdint>
#include <cstddef>

struct InitTableRange {
    uint32_t begin, end; // [begin, end) of function pointers in .data; 0 / -1 entries are skipped by the CRT
};

struct BuildProfile {
    const char *name;          // shown in the log and in the "wrong build" message
    const char *sha256;        // of default.xbe, lower-case hex
    uint32_t title_id;
    int fs_patch_count;        // expected fs:[0x04/0x20/0x24/0x28/0x58] rewrites (xthread.cpp)
    bool black;                // the BLACK-specific game patches, HLE tables and front-end work apply
    InitTableRange init_tables[3]; // _rtinit, _cinit C and C++ tables (XAPI 5849 layout); unused = {0, 0}
    const char *hooked_sections[8]; // library sections whose every function is HLE or stop-on-first-call; nullptr-terminated
    uint32_t xdevice_type_gamepad; // XAPI's g_DeviceType_Gamepad descriptor (XbSymbolDatabase), the XDEVICE_TYPE_GAMEPAD value
    uint32_t switch_to_fiber;      // XAPI SwitchToFiber, replaced so Windows knows the fiber stacks (fiber.cpp); 0 = no fibers
    uint32_t fiber_tls_index;      // the dword holding XAPI's fiber TLS index (read by SwitchToFiber's first instruction)
    const uint32_t *rdtsc_sites;   // rdtsc instructions in game / XAPI code, trapped to a 733.33 MHz counter (hooks.cpp)
    int rdtsc_site_count;
    uint32_t rdtsc_qpc_site;       // the one inside XAPI's QueryPerformanceCounter: full rate; the engine's timer sites get --tsc-divisor
    const char *aux;               // a bundled program of Ninja Gaiden Black (loader/src/aux, tools/gen_aux_tables.py): its tables'
                                   // name; nullptr for BLACK and Ninja Gaiden Black itself
};

// BLACK (Criterion, NTSC-U): numbers from black-pc NOTES.md.
inline const BuildProfile kProfileBlack = {
    "BLACK NTSC-U",
    "df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6",
    0x45410083,
    34,
    true,
    {{0x2964F0, 0x296B6C}, {0x296B70, 0x296B84}, {0, 0}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x0027CF4C,
    0,
    0,
    nullptr,
    0,
    0,
};

// NINJA GAIDEN BLACK vb-zh-x2.1 (title 5443000D, XDK 5849): numbers from docs/02_静态分析.md of the port project.
//  - 41 fs sites (capstone census, tools/xbe_fs_scan.py);
//  - initializer tables read from XAPI's _rtinit (0x32CC3E) and _cinit (0x32CBE6): _rtinit 0x4162C0-0x4162CC,
//    C++ 0x4162D0-0x41634C, C 0x416350-0x416368;
//  - D3D, DSOUND and XPP are the library sections replaced; D3DX / XGRPH (CPU-only helpers), XONLINE / XNET
//    (networking; their kernel calls are stubbed) and the rest run natively.
// rdtsc sites (capstone scan of the executable sections for 0F 31): the engine's frame timer (FUN_0030E140 and
// neighbours, scaled by 0.113 from Xbox TSC ticks) and XAPI's QueryPerformanceCounter (0x376E40). The engine
// turns the character shadow and effect detail down when the measured frame time is high, and a host TSC runs
// 4x faster than the Xbox's 733 MHz (docs/04 §3.9). The D3D (0x3CF970) and DSOUND (0x39374F) copies sit in
// HLE-replaced sections and are left alone.
inline const uint32_t kNgbRdtscSites[] = {0x0030CA3F, 0x0030CAA1, 0x0030E060, 0x0030E284, 0x0030E58E, 0x00376E44};

inline const BuildProfile kProfileNgbHurricaneX21 = {
    "NINJA GAIDEN BLACK Hurricane vb-zh-x2.1",
    "aa3c5bec2923f83ccb37fe6a63ea5516b26d477b53c679a1620d19e4a8e4e8f6",
    0x5443000D,
    41,
    false,
    {{0x4162C0, 0x4162CC}, {0x4162D0, 0x41634C}, {0x416350, 0x416368}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x003D9694,
    0x0032A293,
    0x0045E84C,
    kNgbRdtscSites,
    6,
    0x00376E44,
};

// NINJA GAIDEN BLACK NTSC-U, the original retail release (title 5443000D, version 5, XBE 2005-08-31). x2.1 is this
// XBE patched: the same 16 sections at the same addresses, the library sections (D3D, DSOUND, XPP, XGRPH, D3DX,
// XONLINE, XNET, DOLBY) byte for byte identical, .text differing in 360 bytes at 65 places (the Chinese text hooks into
// x2.1's added HANZI section) and none of them at an address the port uses (scratchpad xbe_hits.py, 2026-10-31). So
// every x2.1 table and patch address holds here too.
inline const BuildProfile kProfileNgbUs = {
    "NINJA GAIDEN BLACK NTSC-U",
    "df1d2af0154d771f42b77ec4df03a862417ac457ff0d39fec846c40cb55ffbda",
    0x5443000D,
    41,
    false,
    {{0x4162C0, 0x4162CC}, {0x4162D0, 0x41634C}, {0x416350, 0x416368}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x003D9694,
    0x0032A293,
    0x0045E84C,
    kNgbRdtscSites,
    6,
    0x00376E44,
};

// The programs Ninja Gaiden Black x2.1 launches (docs/06 of the port project): the NES trilogy of the original release
// (NinjaTomoe.xbe Japanese, NinjaTrilogy.xbe US; XDK 5849) and the SNES trilogy added by x2.1 (NinjaFCJ.xbe Japanese,
// NinjaFCU.xbe US; XDK 5659). Plain (non-LTCG) D3D8 / DSOUND: the HLE tables come from XbSymbolDatabase names
// (tools/gen_aux_tables.py). Initializer tables read from each XAPI's _rtinit / _cinit; rdtsc only in QueryPerformanceCounter.
inline const uint32_t kTomoeQpc[] = {0x0006CD14};
inline const BuildProfile kProfileNinjaTomoe = {
    "NINJA GAIDEN BLACK x2.1: NinjaTomoe.xbe (NES trilogy, Japanese)",
    "2aff93ad098b3b5762d294d9c17a44c36d04bfa31843e413cf5e2963f14282ff",
    0x5443000D, 31, false,
    {{0x90440, 0x9044C}, {0x90450, 0x90468}, {0x9046C, 0x9047C}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x0008798C, 0, 0, kTomoeQpc, 1, 0x0006CD14, "NinjaTomoe",
};
inline const BuildProfile kProfileNinjaTrilogy = {
    "NINJA GAIDEN BLACK x2.1: NinjaTrilogy.xbe (NES trilogy, US)",
    "31f8efd073abba2ac339ee44f4b13011844afc470b166f826dc12f6b79ff5621",
    0x5443000D, 31, false,
    {{0x90440, 0x9044C}, {0x90450, 0x90468}, {0x9046C, 0x9047C}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x0008798C, 0, 0, kTomoeQpc, 1, 0x0006CD14, "NinjaTrilogy",
};
inline const uint32_t kFcjQpc[] = {0x0033B5D4};
inline const BuildProfile kProfileNinjaFCJ = {
    "NINJA GAIDEN BLACK x2.1: NinjaFCJ.xbe (SNES trilogy, Japanese)",
    "b5b8754e6008f3b2e773c1eb9afe50ee9decebf0eadb945a9953369b1b9f4f31",
    0x5443000D, -1, false,
    {{0x35C3C0, 0x35C3CC}, {0x35C3D0, 0x35C3E8}, {0x35C3EC, 0x35C3FC}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x003547AC, 0, 0, kFcjQpc, 1, 0x0033B5D4, "NinjaFCJ",
};
inline const uint32_t kFcuQpc[] = {0x001D5E94};
inline const BuildProfile kProfileNinjaFCU = {
    "NINJA GAIDEN BLACK x2.1: NinjaFCU.xbe (SNES trilogy, US)",
    "9fac4bb5162b8a99c65c0fbb6289ee81db4c42bdbd253807e74261a68e3399b1",
    0x5443000D, -1, false,
    {{0x1F6C00, 0x1F6C0C}, {0x1F6C10, 0x1F6C28}, {0x1F6C2C, 0x1F6C3C}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x001EF06C, 0, 0, kFcuQpc, 1, 0x001D5E94, "NinjaFCU",
};

inline const BuildProfile *const kProfiles[] = {&kProfileBlack, &kProfileNgbHurricaneX21, &kProfileNgbUs, &kProfileNinjaTomoe, &kProfileNinjaTrilogy,
                                                &kProfileNinjaFCJ, &kProfileNinjaFCU};

// The profile whose SHA-256 matches, or nullptr for an unknown build.
inline const BuildProfile *build_profile_for(const char *sha256_hex)
{
    for (const BuildProfile *p : kProfiles) {
        const char *a = p->sha256, *b = sha256_hex;
        while (*a && *b && *a == *b) a++, b++;
        if (!*a && !*b) return p;
    }
    return nullptr;
}
