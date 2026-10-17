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
};

// NINJA GAIDEN BLACK vb-zh-x2.1 (title 5443000D, XDK 5849): numbers from docs/02_静态分析.md of the port project.
//  - 41 fs sites (capstone census, tools/xbe_fs_scan.py);
//  - initializer tables read from XAPI's _rtinit (0x32CC3E) and _cinit (0x32CBE6): _rtinit 0x4162C0-0x4162CC,
//    C++ 0x4162D0-0x41634C, C 0x416350-0x416368;
//  - D3D, DSOUND and XPP are the library sections replaced; D3DX / XGRPH (CPU-only helpers), XONLINE / XNET
//    (networking; their kernel calls are stubbed) and the rest run natively.
inline const BuildProfile kProfileNgbHurricaneX21 = {
    "NINJA GAIDEN BLACK Hurricane vb-zh-x2.1",
    "aa3c5bec2923f83ccb37fe6a63ea5516b26d477b53c679a1620d19e4a8e4e8f6",
    0x5443000D,
    41,
    false,
    {{0x4162C0, 0x4162CC}, {0x4162D0, 0x41634C}, {0x416350, 0x416368}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x003D9694,
};

inline const BuildProfile *const kProfiles[] = {&kProfileBlack, &kProfileNgbHurricaneX21};

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
