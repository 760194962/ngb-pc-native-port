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

// The engine's load meter (game_load.cpp): where it keeps its frame's start, the GPU's share and the CPU percentage,
// and the GPU timer callback it pushes.
struct LoadMeter {
    uint32_t frame_start, gpu_cycles, cpu_load, gpu_callback;
};
// Ninja Gaiden Black (every build of x2.1's code): docs/04 §3.17.
inline constexpr LoadMeter kLoadMeterNgb = {0x00D21680, 0x00D21690, 0x00D21698, 0x0030E060};
// Ninja Gaiden 2004 1.06.04: the same engine; the four are where code paired with x2.1's refers to them (tools/x21_map.py,
// unanimous: 4/4, 2/2, 3/3 votes; the callback is the function paired with 0x30E060, an rdtsc site of the profile).
inline constexpr LoadMeter kLoadMeterNg04Tu = {0x00DFB2A0, 0x00DFB2B0, 0x00DFB2B8, 0x002FFAC0};

// Wider than 16:9 (d3d/draw.cpp squeeze_169): the draws the game makes from these places keep the 16:9 shape. The
// return addresses of the movie player's full-screen blit (a call of DrawVerticesUP) and of the underwater ripples'
// draw, and the menus' 2D routine.
struct Wide2D {
    uint32_t movie_ret, ripples_ret, menu_lo, menu_hi;
};
// Ninja Gaiden Black: FUN_0031DA50 + 0x13C, FUN_0013A050's 0x13A8E6, FUN_00222730 (r346, r503, r375).
inline constexpr Wide2D kWide2DNgb = {0x0031DB8C, 0x0013A8E6, 0x00222730, 0x0022281A};
// Ninja Gaiden 2004 1.06.04: the same routines (x21_map: 0x31DA50 -> 0x30EFB0, 0x13A050 -> 0x137290, 0x222730 ->
// 0x212410), each return address after the same call of DrawVerticesUP (0x3B0540), checked in the disassembly.
inline constexpr Wide2D kWide2DNg04Tu = {0x0030F0EC, 0x00137B26, 0x00212410, 0x002124FA};

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
    const wchar_t *window_title = nullptr; // Ninja Gaiden Black builds: the game window's title (nullptr: BLACK)
    bool chinese = false;                  // its menus have Chinese text (x2.1's language switch, [0xDB9890] = 0)
    const LoadMeter *load_meter = nullptr; // the engine's load meter (game_load.cpp); nullptr: none
    uint32_t aspect_const = 0;             // the game's 16/9 float (ngb_aspect.cpp: other widescreen shapes); 0: 16:9 only
    const Wide2D *wide2d = nullptr;        // its 2D that keeps the 16:9 shape when wider (draw.cpp); nullptr: none known
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

// Ninja Gaiden Black and the builds patched from it: everything but the name, the hash, the fs site count (a patch's
// added HANZI section is code too), the window title and whether it has Chinese is the US release's.
constexpr BuildProfile ngb_family(const char *name, const char *sha256, int fs_patch_count, const wchar_t *title, bool chinese)
{
    return {name, sha256, 0x5443000D, fs_patch_count, false,
            {{0x4162C0, 0x4162CC}, {0x4162D0, 0x41634C}, {0x416350, 0x416368}},
            {"D3D", "DSOUND", "XPP", nullptr},
            0x003D9694, 0x0032A293, 0x0045E84C, kNgbRdtscSites, 6, 0x00376E44, nullptr, title, chinese, &kLoadMeterNgb,
            0x00414AF8, &kWide2DNgb};
}

// The user's Hurricane edition, Chinese / English: the port's own target. "Ninja Gaiden Black·Hurricane[中英文版]" (the
// user's name, 2026-10-22: no "X21" in the names).
inline const BuildProfile kProfileNgbHurricaneX21 = ngb_family(
    "NINJA GAIDEN BLACK Hurricane vb-zh-x2.1", "aa3c5bec2923f83ccb37fe6a63ea5516b26d477b53c679a1620d19e4a8e4e8f6", 41,
    L"Ninja Gaiden Black\u00B7Hurricane[\u4E2D\u82F1\u6587\u7248]", true);

// NINJA GAIDEN BLACK NTSC-U, the original retail release (title 5443000D, version 5, XBE 2005-08-31), which every
// build here is patched from: the same 16 sections at the same addresses, the library sections (D3D, DSOUND, XPP,
// XGRPH, D3DX, XONLINE, XNET, DOLBY) byte for byte identical. x2.1's .text differs from it in 360 bytes at 65 places,
// the Japanese x2.1's in 341 at 61, the basic x2's in 153 at 31; none of them at an address the port uses (scratchpad
// xbe_hits.py, 2026-10-31). So x2.1's tables and patches hold for all four. Only English on its menus.
inline const BuildProfile kProfileNgbUs = ngb_family(
    "NINJA GAIDEN BLACK NTSC-U", "df1d2af0154d771f42b77ec4df03a862417ac457ff0d39fec846c40cb55ffbda", 41,
    L"Ninja Gaiden Black", false);

// The same US release as Redump / extract-xiso dumps have it: byte for byte the above but for the certificate (title
// name "NINJA GAIDEN Black", region NA, media 0x202 instead of all regions and all media; 20 bytes at 0x192-0x224).
// The user's E:\games\pc\美版XISO, 2026-10-31.
inline const BuildProfile kProfileNgbUsRetail = ngb_family(
    "NINJA GAIDEN BLACK NTSC-U (retail certificate)", "5fa3997b0d518a9db3867ca6ae2942c99e5349fd138f536ff56772e1a0202200", 41,
    L"Ninja Gaiden Black", false);

// The user's Hurricane edition, Japanese / English (vb-jp-x2.1): x2.1's gameplay changes without the translation.
inline const BuildProfile kProfileNgbHurricaneJp = ngb_family(
    "NINJA GAIDEN BLACK Hurricane vb-jp-x2.1", "535778c8a4623195c8f320b85d2d139f20ff13c020d40150adaed54cfdeffc38", 41,
    L"Ninja Gaiden Black\u00B7Hurricane[\u65E5\u82F1\u7248]", false);

// The user's basic edition, Chinese / English (va-zh-x2): the original game translated, with the camera fix and the
// bow's crosshair; no SFC trilogy.
inline const BuildProfile kProfileNgbBasicX2 = ngb_family(
    "NINJA GAIDEN BLACK va-zh-x2 (basic)", "00cef52546618eb0149d7895f9042403e95f361b5483aac38933e799c67a50e1", 41,
    L"Ninja Gaiden Black[\u4E2D\u82F1\u6587\u7248]", true);

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

// The US release's two NES programs with the retail certificate (4 bytes at 0x220-0x224, as default.xbe above).
inline const BuildProfile kProfileNinjaTomoeRetail = {
    "NINJA GAIDEN BLACK NTSC-U: NinjaTomoe.xbe (NES trilogy, Japanese; retail certificate)",
    "8d761be1789bb9d000d561debab5bb6c8d7be376d7dec452f6020043e0b653f8",
    0x5443000D, 31, false,
    {{0x90440, 0x9044C}, {0x90450, 0x90468}, {0x9046C, 0x9047C}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x0008798C, 0, 0, kTomoeQpc, 1, 0x0006CD14, "NinjaTomoe",
};
inline const BuildProfile kProfileNinjaTrilogyRetail = {
    "NINJA GAIDEN BLACK NTSC-U: NinjaTrilogy.xbe (NES trilogy, US; retail certificate)",
    "c86ed2baa91f204b176b7329bc087ed499b1ecd4146a62bfc493a161a1b44330",
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

// NINJA GAIDEN (2004, title 54430003): the title update 1.06.04 (Xbox Live, 2004-09-17; TDATA/54430003/$u/default.xbe),
// the main program the Hurricane Packs need. XDK 5659 (Black is 5849): every address differs, so it runs on tables
// generated like the bundled programs' (tools/gen_aux_tables.py, "NG04TU"; work/functions_NG04TU.csv from xboxrecomp
// + XbSymbolDatabase, 2026-10-31). Initializer tables from _rtinit 0x31E16D / _cinit 0x31E115; SwitchToFiber 0x31AF77
// reads the TLS index at 0x43EF3C; rdtsc in the engine's timer functions (FUN_002FE4D0, 2FFAC0, 2FFBA0, 2FFFC0) and in
// QueryPerformanceCounter 0x361470 (work/ng04/profile_probe.py). None of Black's game patches apply (aux).
inline const uint32_t kNg04TuRdtscSites[] = {0x002FE53F, 0x002FE5A1, 0x002FFAC0, 0x002FFCDA, 0x002FFFCE, 0x00361474};
inline const BuildProfile kProfileNg04Tu = {
    "NINJA GAIDEN (2004) title update 1.06.04",
    "86475ba502c0e7f147334b167041c3fd310f8fb34f9e77b0ee6e6866e0764ee3",
    0x54430003, 42, false,
    {{0x3F6C00, 0x3F6C0C}, {0x3F6C10, 0x3F6C5C}, {0x3F6C60, 0x3F6C78}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x003C0234, 0x0031AF77, 0x0043EF3C, kNg04TuRdtscSites, 6, 0x00361474, "NG04TU", L"Ninja Gaiden", false, &kLoadMeterNg04Tu,
    0x003F5284, // 16/9, 4/3 at 0x3F5280 (paired with x2.1's 0x414AF8 / 0x414AF4, 3/3 votes each; values checked)
    &kWide2DNg04Tu,
};

// Ninja Gaiden 2004's disc program 1.02.04 (2004-02-20; XDK 5659): what runs when the title update is not installed
// (main.cpp; the user, 2026-10-31). The same way as the update: tables from x2.1's by code matching ("NG04DISC";
// work/functions_NG04DISC.csv), initializer tables from _rtinit 0x30A201 / _cinit 0x30A1A9, SwitchToFiber 0x30700B
// (TLS index 0x422A7C), rdtsc in FUN_002EA520 / 2EBB60 / 2EBC40 / 2EC060 and QueryPerformanceCounter 0x34D550.
// Load meter 0xDDD1A0 / B0 / B8 (4/4, 2/2, 3/3 votes), GPU callback 0x2EBB60 (paired with 0x30E060); 16/9 at 0x3DE74C
// (3/3, value checked); the movie blit returns to 0x2FB15C (the only push 6 / call DrawVerticesUP 0x39C980 / push 0;
// xor eax, eax), the ripples' draw to 0x129A26 (FUN_00129190 + 0x896 as in x2.1), the menus' 2D FUN_002007A0.
inline const uint32_t kNg04DiscRdtscSites[] = {0x002EA58F, 0x002EA5F1, 0x002EBB60, 0x002EBD7A, 0x002EC06E, 0x0034D554};
inline constexpr LoadMeter kLoadMeterNg04Disc = {0x00DDD1A0, 0x00DDD1B0, 0x00DDD1B8, 0x002EBB60};
inline constexpr Wide2D kWide2DNg04Disc = {0x002FB15C, 0x00129A26, 0x002007A0, 0x0020088A};
inline const BuildProfile kProfileNg04Disc = {
    "NINJA GAIDEN (2004) disc 1.02.04",
    "bea2a85eea7e6e0ba6c3e201966a46b24b27e25b3307a29bc3de5b4254791308",
    0x54430003, 42, false,
    {{0x3E00E0, 0x3E00EC}, {0x3E00F0, 0x3E013C}, {0x3E0140, 0x3E0158}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x003AC674, 0x0030700B, 0x00422A7C, kNg04DiscRdtscSites, 6, 0x0034D554, "NG04DISC", L"Ninja Gaiden", false,
    &kLoadMeterNg04Disc, 0x003DE74C, &kWide2DNg04Disc,
};

// Ninja Gaiden 2004's own SNES trilogy programs: x2.1's NinjaFCJ / NinjaFCU are these with the title id changed and one
// byte patched (0x3305C8 / 0x1CAE88: jl -> jmp, not in any hooked function), so they run on those programs' tables.
inline const BuildProfile kProfileNg04Tomoe = {
    "NINJA GAIDEN (2004): NinjaTomoe.xbe (SNES trilogy, Japanese)",
    "d2aeab806769813bdf8a57449beb5725c02269b83e0a50f72768794f62986b1e",
    0x54430003, -1, false,
    {{0x35C3C0, 0x35C3CC}, {0x35C3D0, 0x35C3E8}, {0x35C3EC, 0x35C3FC}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x003547AC, 0, 0, kFcjQpc, 1, 0x0033B5D4, "NinjaFCJ", L"Ninja Gaiden",
};
inline const BuildProfile kProfileNg04Trilogy = {
    "NINJA GAIDEN (2004): NinjaTrilogy.xbe (SNES trilogy, US)",
    "18eaadc569f31dd239545444ba27fc7b9956177f344e9b421d440a1bc6786ca4",
    0x54430003, -1, false,
    {{0x1F6C00, 0x1F6C0C}, {0x1F6C10, 0x1F6C28}, {0x1F6C2C, 0x1F6C3C}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x001EF06C, 0, 0, kFcuQpc, 1, 0x001D5E94, "NinjaFCU", L"Ninja Gaiden",
};

// NINJA GAIDEN (2004)'s downloader.xbe (disc 1.02.04): the XDK's Xbox Live content downloader (library DOWNLDR, XDK
// 5659, plain D3D8 / DSOUND), which the Xbox Live "NEW NOTICE!" starts (r750) and which goes back to the game when done.
// _rtinit 0x1DA0C / _cinit 0x1D9B4 (XbSymbolDatabase), no fibers; every rdtsc of .text is trapped (XONLINE's timing),
// QueryPerformanceCounter's at full rate (work/ng04/profile_probe.py, 2026-10-31).
inline const uint32_t kNg04DlRdtsc[] = {0x14FC0, 0x15004, 0x15033, 0x15067, 0x150AC, 0x150DC, 0x15700, 0x1577A,
                                        0x16DB7, 0x16DCB, 0x1711A, 0x17159, 0x172BF, 0x27A36, 0x27D81, 0x27E2B,
                                        0x27FC9, 0x2800A, 0x28163, 0x281EC, 0x3FBF1};
inline const BuildProfile kProfileNg04Downloader = {
    "NINJA GAIDEN (2004): downloader.xbe (Xbox Live content downloader)",
    "fcce4a17781b5a84483b821e0e8e39f4b4ea97e6b386cbb206f2b6df887f04ce",
    0x54430003, -1, false,
    {{0xB6440, 0xB644C}, {0xB6450, 0xB6468}, {0xB646C, 0xB647C}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x000AEB14, 0, 0, kNg04DlRdtsc, sizeof kNg04DlRdtsc / sizeof kNg04DlRdtsc[0], 0x0003FBF1, "NG04DL", L"Ninja Gaiden",
};
// The title update's downloader.xbe (1.06.04, TDATA\54430003\$u): the disc's with one byte of .text changed (0x1D895
// jmp -> jge) and a new certificate; everything above holds. It is the one that runs once the update is installed.
inline const BuildProfile kProfileNg04DownloaderTu = {
    "NINJA GAIDEN (2004) title update 1.06.04: downloader.xbe (Xbox Live content downloader)",
    "a99574a8ac0d17702b20e4e5821372ab964cfdbecdaee6d4863f296d924b2e92",
    0x54430003, -1, false,
    {{0xB6440, 0xB644C}, {0xB6450, 0xB6468}, {0xB646C, 0xB647C}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x000AEB14, 0, 0, kNg04DlRdtsc, sizeof kNg04DlRdtsc / sizeof kNg04DlRdtsc[0], 0x0003FBF1, "NG04DL", L"Ninja Gaiden",
};

// NINJA GAIDEN (2004)'s update.xbe (disc 1.02.04): the XDK's online updater ("Online Updater Application", library
// UPDATE, XDK 5659, plain D3D8 / DSOUND), which the game starts when Xbox Live asks for the title update (the Master
// Ninja Tournament without the Hurricane Packs' update installed). _rtinit 0x1AC49 / _cinit 0x1ABF1, no fibers; the one
// rdtsc of .text is QueryPerformanceCounter's (work/ng04/profile_probe.py, 2026-11-01).
inline const uint32_t kNg04UpQpc[] = {0x17F5A};
inline const BuildProfile kProfileNg04Updater = {
    "NINJA GAIDEN (2004): update.xbe (Xbox Live title updater)",
    "ca827a33c4b82e41f6eb3c10f78277ca4c01a329aa8c5ab49981bad4eb9097c3",
    0x54430003, -1, false,
    {{0x93CE0, 0x93CEC}, {0x93CF0, 0x93D30}, {0x93D34, 0x93D4C}},
    {"D3D", "DSOUND", "XPP", nullptr},
    0x0008C5B4, 0, 0, kNg04UpQpc, 1, 0x00017F5A, "NG04UP", L"Ninja Gaiden",
};

inline const BuildProfile *const kProfiles[] = {&kProfileBlack, &kProfileNgbHurricaneX21, &kProfileNgbUs,
                                                &kProfileNgbHurricaneJp, &kProfileNgbBasicX2, &kProfileNinjaTomoe, &kProfileNinjaTrilogy,
                                                &kProfileNinjaFCJ, &kProfileNinjaFCU, &kProfileNg04Tu,
                                                &kProfileNg04Tomoe, &kProfileNg04Trilogy, &kProfileNg04Disc,
                                                &kProfileNgbUsRetail, &kProfileNinjaTomoeRetail, &kProfileNinjaTrilogyRetail,
                                                &kProfileNg04Downloader, &kProfileNg04DownloaderTu,
                                                &kProfileNg04Updater};

// Ninja Gaiden 2004's disc as Redump has it ("Ninja Gaiden (USA, Asia) (En,Ja)", the user's, 2026-11-01): the same
// build 1.02.04 as the files above (same time stamp and sections) but each program without the one byte those carry in
// XapiInitProcess (file 0x2F9D10 / 0xD895 / 0x320EE0 / 0x1BB7A0 / 0x947A: 0x7D jge as signed, theirs 0xEB jmp, which
// skips the start-up check's failure: a common patch for running from a hard disk). Not a byte the port touches, so
// these run on the same profiles (and the title update's downloader already has the 0x7D: kProfileNg04DownloaderTu).
inline const struct {
    const char *sha256;
    const BuildProfile *same;
} kProfileAliases[] = {
    {"86c23d787ecf9f6400b9378544a49b62cfdd478dc04d6ef01ceed2a6429e84c0", &kProfileNg04Disc},
    {"3bc7c979bc0f4b641f25a376afbc98e6ec614b2e5080b64feae022d9cce9e573", &kProfileNg04Downloader},
    {"a8f6a2e484024a9ac6b3a156d3b1b7b5b67ddd3d8407648c9e1f81e5f2d30391", &kProfileNg04Tomoe},
    {"e4fcb8ace1f73172633991c8ddacb924700abe7639a3efd6f3195e2d320343ee", &kProfileNg04Trilogy},
    {"1023bafc5362928eee2a6429c86dcb5bb91c3aa03e58392e81f3def6ad4d56c8", &kProfileNg04Updater},
};

// The profile whose SHA-256 matches (or an alias's), or nullptr for an unknown build.
inline const BuildProfile *build_profile_for(const char *sha256_hex)
{
    auto same = [](const char *a, const char *b) {
        while (*a && *b && *a == *b) a++, b++;
        return !*a && !*b;
    };
    for (const BuildProfile *p : kProfiles)
        if (same(p->sha256, sha256_hex)) return p;
    for (const auto &a : kProfileAliases)
        if (same(a.sha256, sha256_hex)) return a.same;
    return nullptr;
}
