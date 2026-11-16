// Ninja Gaiden Black: the saved sound volumes applied at a cold start, as ngbrecomp 0.9.54 does (#182, sound_boot.c).
//
// The game's start-up applies the settings of system.dat with FUN_001DB1C0 (x2.1), the same function as the one it uses
// when relaunched (FUN_001DAFF0) but for its first three loads: the music, sound effect and voice volumes come from the
// defaults (movzx eax, byte [0x42D3AB / AE / B1]) instead of the saved settings (movzx eax, byte [esi + 0x87 / 88 / 89]).
// So after a cold start the volumes were the defaults, and once any had been changed the title asked "Apply changes to
// the sound settings?" every time; its Yes runs FUN_001DAFF0. The three loads are made the relaunch's (the same length,
// 7 bytes each): the answer given at start. The console behaves as before this; --no-saved-volumes keeps it so.
//
// Found by the instructions both functions share after those loads (movzx of [esi + 0x8C / 8E / 8D]): of the three
// places every build of Ninja Gaiden Black has them (the relaunch, the Yes, the cold start), the one whose loads at
// -0x43, -0x32, -0x19 read fixed addresses is the cold start's. Any other shape leaves the code alone.
#include "common.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {
const uint8_t kShared[] = {0x0F, 0xB6, 0x86, 0x8C, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0x8E, 0x8E,
                           0x00, 0x00, 0x00, 0x0F, 0xB6, 0x96, 0x8D, 0x00, 0x00, 0x00};
const struct {
    uint32_t back;
    uint8_t disp;
} kLoads[3] = {{0x43, 0x87}, {0x32, 0x88}, {0x19, 0x89}}; // music, effects, voices
} // namespace

void install_saved_volumes(uint32_t text_va, uint32_t text_size)
{
    if (!g_cfg.saved_volumes || !text_va) return;
    const uint8_t *b = (const uint8_t *)(uintptr_t)text_va;
    uint32_t cold = 0, n = 0;
    for (uint32_t i = 0x43; i + sizeof kShared <= text_size; i++) {
        if (b[i] != kShared[0] || memcmp(b + i, kShared, sizeof kShared)) continue;
        bool fixed = true;
        for (auto &l : kLoads) fixed = fixed && b[i - l.back] == 0x0F && b[i - l.back + 1] == 0xB6 && b[i - l.back + 2] == 0x05;
        if (fixed) cold = text_va + i, n++;
    }
    if (n != 1) {
        if (n) logf("[sound] the cold start's volume loads found %u times: left as the game has them", n);
        return;
    }
    for (auto &l : kLoads) {
        auto *p = (uint8_t *)(uintptr_t)(cold - l.back);
        const uint8_t with[7] = {0x0F, 0xB6, 0x86, l.disp, 0x00, 0x00, 0x00}; // movzx eax, byte ptr [esi + disp]
        DWORD old;
        VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old);
        memcpy(p, with, 7);
        VirtualProtect(p, 7, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 7);
    }
    logf("[sound] a cold start applies the saved sound volumes (0x%08x; --no-saved-volumes: the defaults, as the console)",
         cold - kLoads[0].back);
}
