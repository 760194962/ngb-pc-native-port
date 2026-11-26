// Runs the vblank clock's policy (loader/src/d3d/frame_clock_model.h) against simulated displays and reports what the
// screen would show. Build: cl /O2 /EHsc /std:c++17 tools/clock_sim.cpp /I loader/src/d3d
//
// Each scenario runs 120 s of ticks. The clock thread wakes at each tick (late by 0-0.15 ms, now and then by 2 ms) and
// reads the scanline twice a tick (right after the tick, and at a rotating point in its middle). The game presents a
// frame 0.5-3 ms after each tick (now and then 8 ms); the GPU finishes it 0.3-2 ms later; a fixed-refresh display shows
// it from the first vblank start after that (a flip, or the compositor's next pass). Reported: the tick rate, the
// clock's state, and how many frames stayed on screen for a number of refreshes other than the steady one (judder).
#include "frame_clock_model.h"

#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>
#include <functional>

using namespace frame_clock;

struct Display {
    double period;
    uint32_t vtotal, vactive;
    bool vrr = false;          // starts a frame when one is presented (at most every `period`)
    double vrr_max_wait = 0.05; // a VRR display refreshes by itself after this
    bool readable = true;      // the scanline can be read
    bool linear = true;        // false: the counter runs over 3/4 of the lines in the active time and jumps the rest (seen
                               // on the user's laptop after an exclusive mode change)
    bool compositor = true;    // the compositor's vblank times (each 0-300 us late, ~3% 0.3-1 ms late)
};

struct Result {
    double rate, follow_share, lock_time, err_p99;
    int uneven, frames;
    State end_state;
};

static double g_bias = 0; // added to every reading's time: a source that is off by this much

static Result run(Display d, bool follow, double seconds, uint32_t seed, double latch, Display *after = nullptr, double switch_at = 1e9)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    Clock c;
    c.allow_follow(follow);
    auto timing = [](const Display &x) { return Timing{x.vrr ? 0 : x.period, x.vtotal, x.vactive}; };
    // a VRR display reports its maximum rate as its mode
    auto timing_of = [&](const Display &x) { return Timing{x.period, x.vtotal, x.vactive}; };
    (void)timing;
    c.set_timing(timing_of(d)); // QueryDisplayConfig: known whatever can be read
    const double t0 = 0.0013; // the display's first frame start
    std::vector<double> vrr_starts{t0};
    auto line_at = [&](double t, bool *vb) -> uint32_t {
        double start;
        if (!d.vrr) {
            double x = std::fmod(t - t0, d.period);
            start = t - x;
        } else {
            auto it = std::upper_bound(vrr_starts.begin(), vrr_starts.end(), t);
            start = it == vrr_starts.begin() ? t0 : *(it - 1);
        }
        double l = (t - start) / d.period * d.vtotal;
        *vb = l >= d.vactive;
        if (!d.linear && !*vb) l *= 0.75; // the source's lines, then a jump at the end of the active time
        return (uint32_t)std::min<double>(l, d.vtotal - 1);
    };
    double due = 0.01, last_change = 0;
    int ticks = 0, following = 0;
    double lock_time = -1;
    std::vector<double> shown; // when each frame first shows
    std::vector<double> errs;
    double rot = 0;
    while (due < seconds) {
        if (after && due >= switch_at) {
            d = *after;
            after = nullptr;
            c.set_timing(timing_of(d)); // QueryDisplayConfig: known whatever can be read
            last_change = due;
            lock_time = -1;
        }
        double wake = due + u(rng) * 0.00015 + (u(rng) < 0.002 ? 0.002 : 0);
        ticks++;
        // the frame of this tick
        // the frame was drawn before the tick (Swap waits for it); what is left is the final copy and the Present
        double present = wake + 0.0002 + u(rng) * 0.0008 + (u(rng) < 0.01 ? 0.008 : 0);
        double ready = present + 0.0002 + u(rng) * 0.0013 + (u(rng) < 0.01 ? 0.003 : 0);
        if (!d.vrr) {
            // the frame is taken at the first latch after it is ready: a flip at the vblank start (exclusive), the
            // compositor's pass ~0.75 ms after it (window)
            double v0 = t0 + d.period * d.vactive / d.vtotal + latch; // a latch
            double n = std::ceil((ready - v0) / d.period);
            shown.push_back(v0 + n * d.period);
        } else {
            double start = std::max(ready, vrr_starts.back() + d.period);
            // refreshes the display made by itself while waiting
            while (start - vrr_starts.back() > d.vrr_max_wait) vrr_starts.push_back(vrr_starts.back() + d.vrr_max_wait);
            vrr_starts.push_back(start);
            shown.push_back(start);
        }
        // readings
        if (d.compositor && !d.vrr) { // the compositor's last vblank start, read right after the tick
            double v0 = t0 + d.period * d.vactive / d.vtotal;
            double last = v0 + std::floor((wake - v0) / d.period) * d.period;
            double late = u(rng) < 0.03 ? 0.0003 + u(rng) * 0.0007 : u(rng) * 0.0003;
            if (last + late < wake) c.vblank_at(last + late + g_bias);
        }
        if (d.readable) {
            bool vb;
            double ts = wake + 0.00003;
            uint32_t l = line_at(ts, &vb);
            c.sample(ts + (u(rng) - 0.5) * 80e-6 + g_bias, vb, l); // a reading takes ~62 us: its time is the middle of the call
            rot = std::fmod(rot + 0.2, 1.0);
            double tm = due + (0.15 + 0.7 * rot) * kConsolePeriod;
            l = line_at(tm, &vb);
            c.sample(tm + (u(rng) - 0.5) * 80e-6 + g_bias, vb, l);
        }
        if (c.state() == State::Following) {
            following++;
            if (lock_time < 0 && std::fabs(c.error()) < 0.0003) lock_time = due - last_change;
            if (due > 10) errs.push_back(std::fabs(c.error()));
        }
        due = c.next(due);
    }
    // judder: frames after the first 10 s whose time on screen differs from the most common one
    std::vector<int> stay;
    for (size_t i = 1; i < shown.size(); i++) {
        if (shown[i] < 10) continue;
        double gap = shown[i] - shown[i - 1];
        stay.push_back((int)std::lround(gap / (d.vrr ? 0.001 : d.period)));
    }
    int mode = 0, best = 0;
    for (int v : stay) {
        int n = (int)std::count(stay.begin(), stay.end(), v);
        if (n > best) best = n, mode = v;
    }
    int uneven = 0;
    for (int v : stay) uneven += v != mode && !(d.vrr && std::abs(v - mode) <= 1);
    std::sort(errs.begin(), errs.end());
    return {ticks / seconds, (double)following / ticks, lock_time, errs.empty() ? 0 : errs[errs.size() * 99 / 100] * 1000, uneven,
            (int)stay.size(), c.state()};
}

static const char *state_name(State s) { return s == State::Free ? "free" : s == State::Following ? "following" : "follows-us"; }

int main()
{
    struct Case { const char *name; Display d; };
    // vtotal / vactive: typical CVT-RB timings; the 60 Hz mode of a 165 Hz panel keeps the pixel clock (long blanking)
    std::vector<Case> cases = {
        {"60.008 Hz (this laptop)", {1 / 60.008, 1732, 1600}},
        {"60.000 Hz", {1 / 60.0, 1125, 1080}},
        {"59.940 Hz (TV)", {1001.0 / 60000, 1125, 1080}},
        {"59.951 Hz", {1 / 59.951, 1111, 1080}},
        {"60.100 Hz", {1 / 60.1, 1111, 1080}},
        {"60 Hz, 165 Hz panel timing", {1 / 60.0, 4400, 1600}},
        {"119.880 Hz", {1001.0 / 120000, 1125, 1080}},
        {"120.000 Hz", {1 / 120.0, 1144, 1080}},
        {"144.000 Hz", {1 / 144.0, 1157, 1080}},
        {"165.019 Hz", {1 / 165.019, 1666, 1600}},
        {"75.000 Hz", {1 / 75.0, 1111, 1080}},
        {"100.000 Hz", {1 / 100.0, 1111, 1080}},
        {"50.000 Hz (PAL TV)", {1 / 50.0, 1125, 1080}},
        {"180.000 Hz", {1 / 180.0, 1111, 1080}},
        {"240.000 Hz", {1 / 240.0, 1111, 1080}},
        {"360.000 Hz", {1 / 360.0, 1111, 1080}},
        {"VRR 48-165 Hz", {1 / 165.0, 1111, 1080, true}},
        {"VRR 48-144 Hz", {1 / 144.0, 1111, 1080, true}},
        {"VRR 40-120 Hz", {1 / 120.0, 1111, 1080, true}},
        {"VRR up to 60 Hz", {1 / 60.0, 1111, 1080, true}},
        {"60 Hz, no scanline", {1 / 60.0, 1125, 1080, false, 0.05, false}},
        {"60.008 Hz, scanline jumps", {1 / 60.008, 1732, 1600, false, 0.05, true, false}},
        {"120 Hz, scanline jumps", {1 / 120.0, 1144, 1080, false, 0.05, true, false}},
        {"60 Hz, nothing readable", {1 / 60.0, 1125, 1080, false, 0.05, false, true, false}},
    };
    printf("%-30s %-6s %-10s %8s %9s %8s %8s %12s %12s\n", "display", "path", "clock", "tick Hz", "followed", "lock s", "err p99", "judder new",
           "judder old");
    for (auto &k : cases) {
        for (double latch : {0.0, 0.00075}) { // exclusive (flip at the vblank start), window (the compositor's pass)
            Result r = run(k.d, true, 120, 7, latch);
            Result o = run(k.d, false, 120, 7, latch); // the 59.94 Hz timer alone
            printf("%-30s %-6s %-10s %8.3f %8.0f%% %8.2f %6.3fms %5d / %-5d %5d / %-5d\n", k.name, latch > 0 ? "window" : "excl",
                   state_name(r.end_state), r.rate, r.follow_share * 100, r.lock_time, r.err_p99, r.uneven, r.frames, o.uneven, o.frames);
        }
    }
    // the margin: every reading off by a fixed time
    printf("\nreadings off by a fixed time (60.008 Hz / 120 Hz, judder new):\n");
    for (double bias : {-0.004, -0.003, -0.002, -0.001, 0.001, 0.002, 0.003, 0.004}) {
        g_bias = bias;
        Result a = run({1 / 60.008, 1732, 1600}, true, 60, 5, 0.00075), b = run({1 / 120.0, 1144, 1080}, true, 60, 5, 0.00075);
        printf("  %+.0f ms: %d / %d, %d / %d\n", bias * 1000, a.uneven, a.frames, b.uneven, b.frames);
    }
    g_bias = 0;
    // a mode change while running: 165 Hz desktop -> 60 Hz exclusive -> back
    Display a{1 / 165.019, 1666, 1600}, b{1 / 60.008, 1732, 1600};
    Result r = run(a, true, 60, 3, 0, &b, 20);
    printf("165 -> 60 Hz at 20 s: end %s, followed %.0f%%, locked %.2f s after the change, judder %d / %d\n", state_name(r.end_state),
           r.follow_share * 100, r.lock_time, r.uneven, r.frames);
    return 0;
}
