#include "d3d/display_refresh.h"

#include "common.h"
#include "d3d/frame_clock.h"

#include <cstdlib>

namespace display_refresh {

namespace {

WCHAR g_switched[32]; // the monitor enter switched (its GDI name), or empty

} // namespace

void enter(HWND window)
{
    if (g_switched[0] || !g_cfg.fullscreen_60hz || !window) return;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    DEVMODEW now = {};
    now.dmSize = sizeof now;
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi) ||
        !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &now))
        return;
    if (frame_clock::followable_hz(now.dmDisplayFrequency)) return; // 60, 120, 240 Hz...: followed as it is
    DEVMODEW best = {}, m = {};
    m.dmSize = sizeof m;
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &m); i++) {
        if (m.dmPelsWidth != now.dmPelsWidth || m.dmPelsHeight != now.dmPelsHeight || m.dmBitsPerPel != now.dmBitsPerPel ||
            (m.dmDisplayFlags & DM_INTERLACED) || m.dmDisplayFrequency < 59 || m.dmDisplayFrequency > 61)
            continue;
        if (!best.dmSize || abs((int)m.dmDisplayFrequency - 60) < abs((int)best.dmDisplayFrequency - 60)) best = m;
    }
    if (!best.dmSize) {
        logf("host: borderless at %lu Hz: %ls has no 60 Hz mode at %lux%lu; the game's clock runs free", now.dmDisplayFrequency,
             mi.szDevice, now.dmPelsWidth, now.dmPelsHeight);
        return;
    }
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    LONG r = ChangeDisplaySettingsExW(mi.szDevice, &best, nullptr, CDS_FULLSCREEN, nullptr);
    logf("host: borderless: %ls %lux%lu from %lu to %lu Hz (60 frames a second shown evenly): %s (%ld)", mi.szDevice, best.dmPelsWidth,
         best.dmPelsHeight, now.dmDisplayFrequency, best.dmDisplayFrequency, r == DISP_CHANGE_SUCCESSFUL ? "done" : "refused", r);
    if (r == DISP_CHANGE_SUCCESSFUL) wcscpy_s(g_switched, mi.szDevice);
}

void leave()
{
    if (!g_switched[0]) return;
    LONG r = ChangeDisplaySettingsExW(g_switched, nullptr, nullptr, 0, nullptr); // the desktop's mode (the registry's)
    logf("host: %ls back to the desktop's mode: %s (%ld)", g_switched, r == DISP_CHANGE_SUCCESSFUL ? "done" : "refused", r);
    g_switched[0] = 0;
}

} // namespace display_refresh
