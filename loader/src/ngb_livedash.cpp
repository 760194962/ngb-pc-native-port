// Ninja Gaiden Black x2.1: Ⓐ in Ranking's Xbox Live boxes that would open the dashboard ("修复玩家代号", network
// settings, a new account) cancels the box instead, as Ⓑ does. There is no dashboard here: XLaunchNewImage with no
// title (k_misc.cpp HalReturnToFirmware) starts the game afresh, so Ⓐ there threw the player back to the title.
//
// The boxes' input (project/docs/09; r446 probed FUN_00302560 while "此玩家代号无法使用" was up): three handlers read the
// port's buttons pressed this frame (0xD219A8 + port * 0x78; 0x110 Ⓐ, 0x220 Ⓑ) and, for Ⓐ, play sound 5, call
// FUN_0031F930 and launch the dashboard with a reason (6 network settings, 7 new account, 8 account recovery). Each
// such Ⓐ block starts with push 0x40 / push 5 / call (the sound): that becomes a jmp to the same box's Ⓑ code (sound 3,
// close), or, for the box with Ⓐ only (FUN_00302560 case 4), to the close the other cases share. Stack and registers
// at each jump are those the target expects (no push between them; 0x302409 pops esi first and its je sees ZF = 0
// from the Ⓐ test, so it falls into the Ⓑ code).
#include "build_profile.h"
#include "common.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

struct Jump {
    uint32_t from, to;
};
const Jump kJumps[] = {
    {0x002FFAE5, 0x002FFC04}, // FUN_002FFAB0: network settings (reason 6) -> its Ⓑ
    {0x002FFB39, 0x002FFB6B}, // FUN_002FFAB0: account recovery (reason 8) -> its Ⓑ
    {0x003023CC, 0x00302409}, // FUN_003023B0: new account (reason 7) -> pop esi, its Ⓑ
    {0x00302668, 0x00302782}, // FUN_00302560 cases 4 / 5: account recovery -> close (case 5's Ⓑ)
    {0x00302698, 0x003026E4}, // FUN_00302560 case 6: account recovery -> its Ⓑ
    {0x00302751, 0x00302782}, // FUN_00302560 case 7: network settings -> its Ⓑ
};
// Ninja Gaiden (2004), whose Master Ninja Tournament has the same boxes (the same code, found by instruction shapes,
// 2026-10-31): the title update 1.06.04 and disc 1.02
const Jump kJumpsNg04Tu[] = {
    {0x002EE385, 0x002EE4A4}, {0x002EE3D9, 0x002EE40B}, {0x002F3F8C, 0x002F3FC9},
    {0x002F4228, 0x002F4342}, {0x002F4258, 0x002F42A4}, {0x002F4311, 0x002F4342},
};
const Jump kJumpsNg04Disc[] = {
    {0x002DA1F5, 0x002DA314}, {0x002DA249, 0x002DA27B}, {0x002DFE1C, 0x002DFE59},
    {0x002E00B8, 0x002E01D2}, {0x002E00E8, 0x002E0134}, {0x002E01A1, 0x002E01D2},
};
const uint8_t kExpect[5] = {0x6A, 0x40, 0x6A, 0x05, 0xE8}; // push 0x40; push 5; call (the sound)

void install_jumps(const Jump *jumps, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const Jump &j = jumps[i];
        if (memcmp((const void *)(uintptr_t)j.from, kExpect, sizeof kExpect) != 0) {
            logf("live boxes: code at 0x%08x is not as expected; Ⓐ keeps opening the dashboard", j.from);
            return;
        }
    }
    for (size_t i = 0; i < n; i++) {
        const Jump &j = jumps[i];
        auto *p = (uint8_t *)(uintptr_t)j.from;
        DWORD old;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        p[0] = 0xE9;
        int32_t rel = (int32_t)(j.to - (j.from + 5));
        memcpy(p + 1, &rel, 4);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }
    logf("live boxes: Ⓐ on the dashboard choices of the Xbox Live boxes cancels them (%zu sites)", n);
}

// Ninja Gaiden 2004's downloader.xbe (the disc's and the title update's: the same code here, r761): its three ways
// to the dashboard (FUN_00014ED7, XLaunchNewImage with a reason) go back instead, as Ⓑ does elsewhere in it.
//   0x1743A: an error box's Ⓐ: "je 0x17448" (the three errors that go back to the previous page) becomes a jmp, so
//            every error goes back there instead of to the dashboard's network settings or account pages;
//   0x17481: its Ⓑ for errors 0x0D / 0x12 / 0x2F (push [esi+4]; call FUN_00014ED7 after two pushed arguments):
//            add esp,8 and the other errors' way back (0x174A1);
//   0x189A0: the sign-in list's "New Account" (push ebp, ebp, 7; call FUN_00014ED7): nothing happens (jmp 0x18A9A).
struct Patch {
    uint32_t at;
    uint8_t expect[9], with[9];
    uint32_t len;
};
const Patch kDownloader[] = {
    {0x0001743A, {0x74, 0x0C}, {0xEB, 0x0C}, 2},
    {0x00017481, {0xFF, 0x76, 0x04, 0xE8, 0x4E, 0xDA, 0xFF, 0xFF}, {0x83, 0xC4, 0x08, 0xE9, 0x18, 0x00, 0x00, 0x00}, 8},
    {0x000189A0, {0x55, 0x55, 0x6A, 0x07, 0xE8, 0x2E, 0xC5, 0xFF, 0xFF}, {0xE9, 0xF5, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90}, 9},
};

void install_downloader_patches()
{
    for (const Patch &p : kDownloader)
        if (memcmp((const void *)(uintptr_t)p.at, p.expect, p.len) != 0) {
            logf("live boxes: the downloader's code at 0x%08x is not as expected; its dashboard choices stay", p.at);
            return;
        }
    for (const Patch &p : kDownloader) {
        auto *q = (uint8_t *)(uintptr_t)p.at;
        DWORD old;
        VirtualProtect(q, p.len, PAGE_EXECUTE_READWRITE, &old);
        memcpy(q, p.with, p.len);
        VirtualProtect(q, p.len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), q, p.len);
    }
    logf("live boxes: the downloader's dashboard choices go back instead (%zu sites)", sizeof kDownloader / sizeof kDownloader[0]);
}

} // namespace

void install_ngb_livedash() { install_jumps(kJumps, sizeof kJumps / sizeof kJumps[0]); }

void install_ng04_livedash()
{
    if (g_cfg.profile == &kProfileNg04Tu) install_jumps(kJumpsNg04Tu, sizeof kJumpsNg04Tu / sizeof kJumpsNg04Tu[0]);
    if (g_cfg.profile == &kProfileNg04Disc) install_jumps(kJumpsNg04Disc, sizeof kJumpsNg04Disc / sizeof kJumpsNg04Disc[0]);
    if (g_cfg.profile == &kProfileNg04Downloader || g_cfg.profile == &kProfileNg04DownloaderTu) install_downloader_patches();
}
