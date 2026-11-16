// Ninja Gaiden Black and Ninja Gaiden (2004): the saved sound volumes applied at a cold start, as ngbrecomp 0.9.54 does
// for Black (#182, sound_boot.c).
//
// The game's start-up applies the settings of system.dat with FUN_001DB1C0 (x2.1), the same function as the one it uses
// when relaunched (FUN_001DAFF0) but for its first three loads: the music, sound effect and voice volumes come from the
// defaults (movzx eax, byte [0x42D3AB / AE / B1]) instead of the saved settings (movzx eax, byte [esi + 0x87 / 88 / 89]).
// So after a cold start the volumes were the defaults, and once any had been changed the title asked "Apply changes to
// the sound settings?" every time; its Yes runs FUN_001DAFF0. The three loads are made the relaunch's (the same length,
// 7 bytes each): the answer given at start. The console behaves as before this; --no-saved-volumes keeps it so.
// Ninja Gaiden (2004) has the same pair with its settings 0x50 bytes earlier in the save: disc 1.02.04 FUN_001BA930 /
// FUN_001BA740 (defaults [0x3FB9AB / AE / B1], saved [esi + 0x37 / 38 / 39]), title update 1.06.04 FUN_001CB750.
//
// Found by the instructions both functions share after those loads, movzx eax / ecx / edx of [esi + A / A+2 / A+1]
// (A = 0x8C in Black, with 32-bit displacements; 0x3C in 2004, 8-bit): of the places every build has them (the
// relaunch, the Yes, the cold start), the one whose loads at -0x43, -0x32, -0x19 read fixed addresses is the cold
// start's, and the volumes are saved at A-5, A-4, A-3. Exactly one in each of the six Black builds and both 2004
// programs here; any other count or shape leaves the code alone.
#include "common.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {
const uint32_t kLoadsBack[3] = {0x43, 0x32, 0x19}; // music, effects, voices

uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// The shared instructions at b: A (the displacement of the first), or 0 when they are not there
uint32_t shared_disp(const uint8_t *b)
{
    if (b[0] == 0x0F && b[1] == 0xB6 && b[2] == 0x86 && b[7] == 0x0F && b[8] == 0xB6 && b[9] == 0x8E && b[14] == 0x0F &&
        b[15] == 0xB6 && b[16] == 0x96) { // 32-bit displacements (Black)
        uint32_t a = rd32(b + 3);
        return rd32(b + 10) == a + 2 && rd32(b + 17) == a + 1 ? a : 0;
    }
    if (b[0] == 0x0F && b[1] == 0xB6 && b[2] == 0x46 && b[4] == 0x0F && b[5] == 0xB6 && b[6] == 0x4E && b[8] == 0x0F &&
        b[9] == 0xB6 && b[10] == 0x56) { // 8-bit displacements (2004)
        uint32_t a = b[3];
        return b[7] == a + 2 && b[11] == a + 1 ? a : 0;
    }
    return 0;
}
} // namespace

void install_saved_volumes(uint32_t text_va, uint32_t text_size)
{
    if (!g_cfg.saved_volumes || !text_va) return;
    const uint8_t *b = (const uint8_t *)(uintptr_t)text_va;
    uint32_t cold = 0, disp = 0, n = 0;
    for (uint32_t i = kLoadsBack[0]; i + 21 <= text_size; i++) {
        if (b[i] != 0x0F) continue;
        uint32_t a = shared_disp(b + i);
        if (a < 5 || a > 0x100) continue; // 0x8C / 0x3C in the builds here
        bool fixed = true;
        for (uint32_t back : kLoadsBack) fixed = fixed && b[i - back] == 0x0F && b[i - back + 1] == 0xB6 && b[i - back + 2] == 0x05;
        if (fixed) cold = text_va + i, disp = a, n++;
    }
    if (n != 1) {
        if (n) logf("[sound] the cold start's volume loads found %u times: left as the game has them", n);
        return;
    }
    for (int k = 0; k < 3; k++) {
        auto *p = (uint8_t *)(uintptr_t)(cold - kLoadsBack[k]);
        uint8_t with[7] = {0x0F, 0xB6, 0x86, 0, 0, 0, 0}; // movzx eax, byte ptr [esi + disp32]
        uint32_t d = disp - 5 + k;
        memcpy(with + 3, &d, 4);
        DWORD old;
        VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old);
        memcpy(p, with, 7);
        VirtualProtect(p, 7, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 7);
    }
    logf("[sound] a cold start applies the saved sound volumes (0x%08x, saved at +0x%x; --no-saved-volumes: the defaults, "
         "as the console)", cold - kLoadsBack[0], disp - 5);
}
