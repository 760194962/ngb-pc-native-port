// The game's vblank clock as a pure model: frame_clock.cpp gives it the display's timing and scanline readings and asks
// when the next tick is due. Nothing here waits on the display or touches Windows, so the policy can be run against
// simulated displays (tools/clock_sim.cpp).
//
// The tick is a timer. On a fixed-refresh display whose refresh is a whole multiple of a rate near the console's
// (60, 59.94, 120, 180, 240, 60.1 Hz...), the timer is steered onto that display's vblanks: its period becomes k
// refreshes (at most a few per mille from 59.94 Hz) and each tick falls a fixed time after a vblank, so every frame is
// shown for the same number of refreshes. Otherwise (75, 144, 165 Hz, a display that follows our presents (VRR), no
// scanline) the tick runs free at the console's 59.94 Hz. The display never holds a tick back: a wrong reading can only
// move the period within the clamp below.
#pragma once

#include <cmath>
#include <cstdint>

namespace frame_clock {

constexpr double kConsolePeriod = 1001.0 / 60000.0; // 59.94 Hz
// Tick rates the game may run at while it follows a display: 59.94 Hz -0.4% / +0.6%. 60.008 Hz (the user's panel) is
// +0.11%; the windowed path ran at that rate from 2.2.31 to 2.3 with no complaint (sound runs on its own clock).
constexpr double kMinRate = 59.70, kMaxRate = 60.30;

// The display's nominal timing (QueryDisplayConfig: the target's vsync as a fraction and its vertical lines).
struct Timing {
    double period = 0; // s; 0: unknown
    uint32_t vtotal = 0, vactive = 0;
    bool operator==(const Timing &o) const { return period == o.period && vtotal == o.vtotal && vactive == o.vactive; }
    bool operator!=(const Timing &o) const { return !(*this == o); }
};

// How many refreshes one tick can span on a display of this period: k when 1 / (k period) is a rate the game may run
// at, else 0
inline int refreshes_per_tick(double period)
{
    if (period <= 0) return 0;
    int k = (int)std::lround(kConsolePeriod / period);
    if (k < 1 || k > 8) return 0;
    double rate = 1 / (k * period);
    return rate >= kMinRate && rate <= kMaxRate ? k : 0;
}

// Where the display's frames start, fitted from scanline readings (or the compositor's vblank times): a frame starts at
// g + n p. A reading (t, line) says its frame started at t - line * p / vtotal. Readings that miss the grid by more than kTolerance are counted, not
// used; many in a row mean the timing changed (a mode change, or a display that starts frames when we present).
class Grid {
public:
    static constexpr double kTolerance = 250e-6;

    void reset(const Timing &tm)
    {
        tm_ = tm;
        p_ = tm.period;
        have_ = false;
        history_ = 0;
        count_ = 0;
        misses_ = 0;
    }
    void sample(double t, bool in_vblank, uint32_t line)
    {
        if (tm_.period <= 0 || !tm_.vtotal || in_vblank || line >= tm_.vactive) return; // blanking lines say nothing exact
        frame_start(t, t - line * p_ / tm_.vtotal);
    }
    // A vblank start's time from elsewhere (the desktop compositor's timing, when the scanline cannot be read)
    void vblank_at(double t)
    {
        if (tm_.period <= 0 || !tm_.vtotal) return;
        frame_start(t, t - p_ * tm_.vactive / tm_.vtotal);
    }
    // At least 64 readings, 62 of the last 64 on the grid, the last good one under a second before `now` (readings that
    // stop, or only fall in the blanking, leave the grid unknown)
    bool steady(double now) const
    {
        if (count_ < 64 || now - last_hit_ > 1.0) return false;
        uint64_t h = history_;
        int hits = 0;
        for (int i = 0; i < 64; i++, h >>= 1) hits += (int)(h & 1);
        return hits >= 62;
    }
    // Still on the grid (for a clock already following): not started again, a good reading under a second before `now`.
    // Following starts on steady() and ends on this, so a few odd readings do not drop it.
    bool alive(double now) const { return count_ >= 64 && now - last_hit_ <= 1.0; }
    double period() const { return p_; }
    // Time from the last vblank start (the end of a frame's active lines) to t, in [0, period)
    double since_vblank(double t) const
    {
        double v = g_ + p_ * tm_.vactive / tm_.vtotal;
        double x = std::fmod(t - v, p_);
        return x < 0 ? x + p_ : x;
    }
    const Timing &timing() const { return tm_; }

private:
    // A frame started at `start` (read at t)
    void frame_start(double t, double start)
    {
        if (!have_) {
            g_ = start;
            have_ = true;
            return;
        }
        double n = std::floor((start - g_) / p_ + 0.5);
        double r = start - (g_ + n * p_);
        bool hit = std::fabs(r) < kTolerance;
        history_ = history_ << 1 | (hit ? 1 : 0);
        count_++;
        if (!hit) {
            if (++misses_ >= 8) { // the grid is wrong now: start again from this reading
                Timing tm = tm_;
                reset(tm);
                g_ = start;
                have_ = true;
            }
            return;
        }
        misses_ = 0;
        last_hit_ = t;
        // Small gains: a reading's time is only known to ~+-30 us (the call takes ~60 us), and the nominal period is
        // already right to ~1e-5 (60.008 Hz nominal, 60.0077 fitted on the user's panel), so the period moves slowly
        g_ += n * p_ + r / 8;
        if (n >= 1) p_ += r / (32 * n);
        if (std::fabs(p_ - tm_.period) > tm_.period * 0.005) { // not this display's refresh: start again
            Timing tm = tm_;
            reset(tm);
        }
    }

    Timing tm_;
    double g_ = 0, p_ = 0, last_hit_ = -1e9;
    bool have_ = false;
    uint64_t history_ = 0;
    uint32_t count_ = 0, misses_ = 0;
};

enum class State : uint8_t {
    Free,      // 59.94 Hz timer: no display to follow (refresh not a multiple, no readings, fixed clock asked)
    Following, // on the display's vblanks (pulling in while the phase error is large)
    FollowsUs, // the display's frames follow our ticks (variable refresh): free for a while
};

// Decides each tick's time. Times are seconds on one clock (QueryPerformanceCounter).
class Clock {
public:
    // Where a tick falls after a vblank start (0: by the refresh, phase_for). The frame is drawn before its tick (Swap
    // waits for it); what follows the tick (the last copy, the Present, the GPU's end of it) takes 0.5-2.5 ms. The tick
    // must come after the latch of the refresh that just began (a flip latches at the vblank start, the desktop
    // compositor ~0.5-1.4 ms after it: DwmFlush returned 0.86-5.2 ms, median 1.42, on the user's panel) and leave that
    // work before the next latch, so every frame is taken at the same one.
    double phase = 0;
    // 2 ms at 60 Hz, 1 at 120, 0.5 at 240: at high refresh the work after the tick is most of a refresh, and a tick just
    // before the compositor's latch still has its frame ready only after it (the simulation, tools/clock_sim.cpp)
    static double phase_for(double period) { return period * 0.12 < 0.002 ? period * 0.12 : 0.002; }

    // A new display timing (mode change, another monitor, exclusive fullscreen); period 0 = unknown
    void set_timing(const Timing &tm)
    {
        if (tm == grid_.timing()) return;
        grid_.reset(tm);
        k_ = refreshes_per_tick(tm.period);
        free_until_ = 0;
        leave_following();
    }
    void sample(double t, bool in_vblank, uint32_t line) { grid_.sample(t, in_vblank, line); }
    void vblank_at(double t) { grid_.vblank_at(t); }
    void allow_follow(bool on) { allow_ = on; }

    // The tick after the one due at `due`
    double next(double due)
    {
        if (!can_follow(due)) {
            leave_following();
            return due + kConsolePeriod;
        }
        const double p = grid_.period(), span = k_ * p;
        if (state_ != State::Following) {
            state_ = State::Following;
            base_ = span;
            far_since_ = unpinned_since_ = due;
        }
        double e = grid_.since_vblank(due) - (phase > 0 ? phase : phase_for(p)); // the error, to the nearest vblank: [-p/2, p/2)
        e -= p * std::floor(e / p + 0.5);
        error_ = e;
        // PI loop, critically damped (both poles 0.95: an error settles in ~20 ticks); the period clamps keep the
        // rate inside kMinRate..kMaxRate and the change per tick small, so pulling in from half a refresh takes ~2 s
        const double lo = span * (1 - 0.002), hi = span * (1 + 0.002);
        base_ = clamp(base_ - kKi * e, lo, hi);
        double t = clamp(base_ - kKp * e, span * (1 - 0.004), span * (1 + 0.004));
        t = clamp(t, 1 / kMaxRate, 1 / kMinRate);
        // A display that starts its frames when we present (VRR) keeps the error where it is however the tick moves: the
        // error stays large, or the period runs to its clamp and stays there (a pull-in takes ~2 s)
        if (std::fabs(e) < 0.001) far_since_ = due;
        if (base_ > lo && base_ < hi) unpinned_since_ = due;
        if (due - far_since_ > 4.0 || due - unpinned_since_ > 4.0) {
            state_ = State::FollowsUs;
            free_until_ = due + 60.0;
            return due + kConsolePeriod;
        }
        return due + t;
    }

    State state() const { return state_; }
    int refreshes() const { return k_; }
    double display_period() const { return grid_.period(); }
    double error() const { return error_; }      // the last phase error while following, s
    double tick_period() const { return state_ == State::Following ? base_ : kConsolePeriod; }
    const Grid &grid() const { return grid_; }

private:
    static constexpr double kKp = 0.1, kKi = 0.0025;
    static double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
    bool can_follow(double due)
    {
        if (!allow_ || !k_) return false;
        if (state_ == State::Following ? !grid_.alive(due) : !grid_.steady(due)) return false;
        if (due < free_until_) return false;
        return true;
    }
    void leave_following()
    {
        if (state_ == State::Following) state_ = State::Free;
        if (state_ == State::FollowsUs && free_until_ == 0) state_ = State::Free;
        error_ = 0;
    }

    Grid grid_;
    int k_ = 0;
    bool allow_ = true;
    State state_ = State::Free;
    double base_ = kConsolePeriod, error_ = 0, far_since_ = 0, unpinned_since_ = 0, free_until_ = 0;
};

} // namespace frame_clock
