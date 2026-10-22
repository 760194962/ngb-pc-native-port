// Ninja Gaiden Black x2.1 in 16:10 or 21:9 (black.ini aspect).
//
// The game takes its picture shape from one constant: five places (0xE7F07, 0xFDE66, 0x2E075F, 0x31151E, 0x312CFD)
// load the float at 0x414AF8 (16/9) when the console reports a widescreen TV ([0x9776A8] & 0x10, our XC_VIDEO) and
// the one at 0x414AF4 (4/3) otherwise, and store it, times [0x4148D0], as the projection's aspect at 0x977EF8.
// Writing the wanted shape into 0x414AF8 makes the projection that wide; the host widens the render targets and the
// window to match (d3d/device.cpp setup_render_scale, open_window).
#include "common.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

void install_ngb_aspect()
{
    if (!g_cfg.widescreen || (g_cfg.aspect_w == 16 && g_cfg.aspect_h == 9)) return;
    float *k = (float *)(uintptr_t)0x414AF8;
    float want = (float)g_cfg.aspect_w / (float)g_cfg.aspect_h;
    if (*k < 1.777f || *k > 1.778f) { // not the 16/9 expected there: another build
        logf("aspect: 0x414AF8 holds %g, not 16/9: %d:%d not applied", *k, g_cfg.aspect_w, g_cfg.aspect_h);
        g_cfg.aspect_w = 16, g_cfg.aspect_h = 9;
        return;
    }
    DWORD old;
    VirtualProtect(k, 4, PAGE_READWRITE, &old);
    *k = want;
    VirtualProtect(k, 4, old, &old);
    logf("aspect: widescreen %d:%d (the game's 16/9 at 0x414AF8 is now %g)", g_cfg.aspect_w, g_cfg.aspect_h, want);
}
