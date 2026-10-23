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
const uint8_t kExpect[5] = {0x6A, 0x40, 0x6A, 0x05, 0xE8}; // push 0x40; push 5; call FUN_001E3400

} // namespace

void install_ngb_livedash()
{
    for (const Jump &j : kJumps)
        if (memcmp((const void *)(uintptr_t)j.from, kExpect, sizeof kExpect) != 0) {
            logf("live boxes: code at 0x%08x does not match x2.1; Ⓐ keeps opening the dashboard", j.from);
            return;
        }
    for (const Jump &j : kJumps) {
        auto *p = (uint8_t *)(uintptr_t)j.from;
        DWORD old;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        p[0] = 0xE9;
        int32_t rel = (int32_t)(j.to - (j.from + 5));
        memcpy(p + 1, &rel, 4);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }
    logf("live boxes: Ⓐ on the dashboard choices of Ranking's Xbox Live boxes cancels them (%zu sites)", sizeof kJumps / sizeof kJumps[0]);
}
