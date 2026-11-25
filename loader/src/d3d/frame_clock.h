// The game's vblank clock: when the emulated vblank comes (device.cpp vblank_thread signals the game at each tick).
//
// A timer that never waits on the display. When the window's display refreshes at a whole multiple of a rate near the
// console's 59.94 Hz, the timer follows that display's vblanks (frame_clock_model.h): its period becomes 1, 2, 3...
// refreshes and every tick falls the same short time after a vblank, so each frame is shown for the same number of
// refreshes in a window, borderless or exclusive fullscreen alike. The display's phase comes from its scanline
// (D3DKMTGetScanLine: no wait) and its timing from QueryDisplayConfig. Anywhere else the tick is the console's 59.94 Hz.
//
// This replaces the 2.2.x clock: DwmFlush in a 60 Hz window (the tick waited on the compositor, returning 0.9-5.2 ms
// after the vblank) and a free 59.94 Hz timer everywhere else, which at 60.008 Hz showed a frame twice every ~14 s in
// exclusive fullscreen.
#pragma once

#include <cstdint>

namespace frame_clock {

// Runs on the vblank thread
class Ticker {
public:
    Ticker();
    int64_t wait();   // sleeps until the next tick; its time (QueryPerformanceCounter)
    void after_tick(); // readings, the display's timing, the next tick's time, the log
private:
    struct Impl;
    Impl *d;
};

// The tick period now, s (59.94 Hz, or the followed display's refreshes): what pace_swap expects before a target vblank
double tick_period();

} // namespace frame_clock
