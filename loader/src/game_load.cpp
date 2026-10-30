// The game's own load meter (Ninja Gaiden Black), handled as ngbrecomp 0.9.31-0.9.33 does it (#50, #96).
//
// The engine times its frame from its start (the cycle counter it keeps at 0xD21680, read at an rdtsc site: hooks.cpp)
// to the point it hands the frame over (FUN_0030E140 -> 0xD21698, a percentage), and takes how far into the frame the
// GPU got to a callback it pushes at the end (0x30E060 -> 0xD21690). FUN_001D5E30 takes the larger: over 94 it lowers
// the quality level (0x8CDA5C: Ryu's shadow map gives way to the round shadow under him), and over about 95 the engine
// leaves its HUD and 2D pass out for frames, then draws them all at once (docs/04 §3.17). On the Xbox a Direct3D call
// only queues work for the GPU; here each one translates and draws on the game thread, inside the frame the engine
// times, so on a slower PC the meter crossed those limits while the frame still made 60 fps.
// - The time the presenting thread spends in the D3D library's calls (their thunks, dispatch.cpp) moves the frame's
//   start on by as much: the engine measures its own work, as on the Xbox. Timed with the host's cycle counter (a
//   clock call per D3D call was a seventh of the game thread in ngbrecomp, #96), turned into the engine's counts by a
//   ratio measured once a frame.
// - What is left is kept at 80 at most as the frame is presented: under every limit the engine compares it with.
// - The GPU's share is reported as nothing: the push-buffer callback runs at once here (pushbuf.cpp), so its timing
//   says nothing about a GPU.
// --original-load keeps the engine's own measurement (comparing). Either way the engine's figure is logged every 30 s.
#include "game_load.h"

#include "common.h"
#include "hooks.h"

#include <intrin.h>

namespace game_load {
namespace {

constexpr uint32_t kFrameStart = 0x00D21680; // uint64: the engine's cycle count as its frame began
constexpr uint32_t kGpuCycles = 0x00D21690;  // uint64: cycles into the frame when the GPU reached the callback
constexpr uint32_t kCpuLoad = 0x00D21698;    // float: the frame's CPU share in percent (FUN_0030E140)
constexpr uint32_t kGpuCallback = 0x0030E060;

bool g_ngb, g_active;
double g_per_cycle; // engine counts per host cycle; 0 until measured (nothing is taken out before)
double g_at_zero;   // the engine's count at host cycle 0 by that ratio: its count now, without a clock call

thread_local int t_depth;
thread_local uint64_t t_t0;
thread_local uint32_t t_start_lo; // the frame's start as the call began
thread_local bool t_frame_thread; // the thread that presents (set at its first Swap)

uint64_t &frame_start() { return *(uint64_t *)(uintptr_t)kFrameStart; }

// The engine's figure over the last 1800 frames (before the cap), for the log
float g_max;
double g_sum;
uint32_t g_frames, g_over80, g_over94;

} // namespace

void install(bool ngb)
{
    g_ngb = ngb;
    g_active = ngb && !g_cfg.original_load;
    if (ngb)
        logf("game load: %s", g_active ? "Direct3D time left out of the engine's frame time, its CPU figure kept at 80% at most, "
                                         "its GPU figure 0 (--original-load keeps the engine's own)"
                                       : "the engine's own measurement (--original-load)");
}

bool active() { return g_active; }

void on_swap()
{
    if (!g_ngb) return;
    float &load = *(float *)(uintptr_t)kCpuLoad;
    g_max = load > g_max ? load : g_max;
    g_sum += load;
    g_over80 += load > 80.0f;
    g_over94 += load > 94.0f;
    if (++g_frames == 1800) {
        logf("game load: the engine's CPU figure over the last 1800 frames: mean %.1f, max %.1f, over 80 in %u, over 94 in %u%s",
             g_sum / g_frames, g_max, g_over80, g_over94, g_active ? " (before the cap)" : "");
        g_max = 0;
        g_sum = 0;
        g_frames = g_over80 = g_over94 = 0;
    }
    if (!g_active) return;
    t_frame_thread = true;
    static uint64_t c0, e0;
    uint64_t c = __rdtsc(), e = engine_tsc();
    if (!c0) {
        c0 = c;
        e0 = e;
    } else if (c - c0 > 100000000ull) { // a tenth of a second or so: the ratio is good, and better as it grows
        g_per_cycle = (double)(e - e0) / (double)(c - c0);
        g_at_zero = (double)e - (double)c * g_per_cycle;
    }
    if (load > 80.0f) load = 80.0f;
}

bool gpu_callback(uint32_t fn)
{
    if (!g_active || fn != kGpuCallback) return false;
    *(uint64_t *)(uintptr_t)kGpuCycles = 0;
    return true;
}

} // namespace game_load

using namespace game_load;

extern "C" void __cdecl game_load_span_enter()
{
    if (t_depth++ == 0 && t_frame_thread) {
        t_t0 = __rdtsc();
        t_start_lo = (uint32_t)frame_start();
    }
}

extern "C" void __cdecl game_load_span_leave()
{
    if (t_depth <= 0 || --t_depth) return;
    uint64_t t0 = t_t0;
    t_t0 = 0;
    // A call in which the engine began its next frame (its start changed meanwhile): its time belongs to the frame
    // before, and moving the new start by it could put the start past the end (ngbrecomp #96).
    if (!t0 || g_per_cycle <= 0 || (uint32_t)frame_start() != t_start_lo) return;
    uint64_t c = __rdtsc();
    uint64_t start = frame_start() + (uint64_t)((double)(c - t0) * g_per_cycle);
    uint64_t now = (uint64_t)(g_at_zero + (double)c * g_per_cycle); // never past now (a ratio a little off)
    frame_start() = start < now ? start : now;
}
