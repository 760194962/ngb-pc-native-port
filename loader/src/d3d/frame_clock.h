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

// Swap, just before a Present (tick: the last tick's QueryPerformanceCounter time). Notes how long after it the Present
// comes. With a variable refresh rate the screen shows each frame as it is presented, and the game presents 0.3-4 ms
// after the tick (it often waits for the vblank, then draws): G-SYNC at 165 Hz showed 60 frames a second spread over
// 14-19 ms (30% within 16.2-17.2 ms, 2026-11-26). There, when `may_hold`, the Present waits until the same time after
// every tick (what most frames take, plus a margin), so the screen refreshes as evenly as the ticks come.
void before_present(int64_t tick, bool may_hold);

// Low-latency present (black.ini low_latency_present), Swap: whether the frame, ready at `now` (QueryPerformanceCounter
// time), is presented before its tick. Presented at the tick a frame reaches the screen a refresh after it; presented as
// soon as it is drawn, at the refresh before -- if it is there before that refresh's latch. A frame that misses it shows
// a refresh late, and the next one, on time, lands in the same refresh: one picture twice, one never (2.2.34's windowed
// complaint). On a followed display (one tick a refresh) the vblanks are known: in exclusive fullscreen a frame goes early
// only when it is ready 4 ms before the next one (the GPU's last work); after a miss every frame waits for its tick until
// 120 in a row would have made it. Through the compositor (window, borderless) frames wait for their tick. Where the
// display is not followed the frame goes at once, as before.
bool present_before_tick(int64_t now);

// A refresh rate the clock can follow, as Windows and Direct3D 9 list modes in whole Hz: a whole multiple of ~60
// (59/60/61, 119/120/121, 239/240/241...; 59.94 Hz modes are listed as 59 or 60). Which mode exclusive and borderless
// fullscreen keep (host.cpp exclusive_mode, display_refresh.cpp); the clock itself checks the exact rate.
inline bool followable_hz(unsigned hz)
{
    int k = (int)((hz + 30) / 60);
    return k >= 1 && k <= 8 && (int)hz - 60 * k <= k && 60 * k - (int)hz <= k;
}

} // namespace frame_clock
