#include "d3d/frame_clock.h"

#include "common.h"
#include "d3d/frame_clock_model.h"
#include "d3d/host.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include <dwmapi.h>
#include <mmsystem.h>

namespace frame_clock {

namespace {

std::atomic<double> g_tick_period{kConsolePeriod};

// How long after its tick each Present came (before any hold): 0.25 ms bins to 20 ms, the last one beyond. Two copies:
// one for the minute log, one for the hold's estimate every 2 s.
struct PresentTimes {
    static constexpr int kBins = 81;
    std::atomic<uint32_t> bins[kBins];
    void add(double ms) { bins[ms < 0 ? 0 : ms >= 20 ? kBins - 1 : (int)(ms * 4)].fetch_add(1, std::memory_order_relaxed); }
    // Takes the counts (and empties them): how many, and the time p of them came within
    uint32_t take(uint32_t (&out)[kBins])
    {
        uint32_t n = 0;
        for (int i = 0; i < kBins; i++) n += out[i] = bins[i].exchange(0);
        return n;
    }
    static double percentile(const uint32_t (&b)[kBins], uint32_t n, double p)
    {
        uint32_t want = (uint32_t)(p * n), seen = 0;
        for (int i = 0; i < kBins; i++)
            if ((seen += b[i]) > want) return (i + 1) * 0.25;
        return kBins * 0.25;
    }
};
PresentTimes g_present_minute, g_present_recent;

// The variable-refresh hold (before_present): how long after its tick a Present waits, QueryPerformanceCounter units
// (0: no hold)
std::atomic<int64_t> g_hold{0};

// D3DKMT (gdi32): the display controller's scanline for a GDI display, read without waiting
typedef UINT KmtHandle;
struct KmtOpenFromName { WCHAR DeviceName[32]; KmtHandle hAdapter; LUID AdapterLuid; UINT VidPnSourceId; };
struct KmtClose { KmtHandle hAdapter; };
struct KmtScanLine { KmtHandle hAdapter; UINT VidPnSourceId; BOOLEAN InVerticalBlank; UINT ScanLine; };
using KmtOpenFn = LONG(APIENTRY *)(KmtOpenFromName *);
using KmtCloseFn = LONG(APIENTRY *)(KmtClose *);
using KmtScanFn = LONG(APIENTRY *)(KmtScanLine *);

struct Qpc {
    LARGE_INTEGER f, t0;
    Qpc() { QueryPerformanceFrequency(&f), QueryPerformanceCounter(&t0); }
    double now() const
    {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return (double)(c.QuadPart - t0.QuadPart) / f.QuadPart;
    }
    int64_t ticks(double s) const { return t0.QuadPart + (int64_t)(s * f.QuadPart); }
    double seconds(int64_t ticks) const { return (double)(ticks - t0.QuadPart) / f.QuadPart; }
};

// The desktop compositor's last vblank (DwmGetCompositionTimingInfo: no wait). It is updated in exclusive fullscreen
// too (2026-11-26, Iris Xe). Only when its refresh is the display's: with several monitors it is the compositor's own
// clock.
class CompositorTiming {
public:
    CompositorTiming()
    {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        get_ = m ? (Get)GetProcAddress(m, "DwmGetCompositionTimingInfo") : nullptr;
    }
    bool available() const { return get_ != nullptr; }
    bool last_vblank(const Qpc &q, double period, double *t)
    {
        DWM_TIMING_INFO ti = {};
        ti.cbSize = sizeof ti;
        if (!get_ || FAILED(get_(nullptr, &ti)) || !ti.qpcRefreshPeriod || !ti.qpcVBlank) return false;
        double p = (double)ti.qpcRefreshPeriod / q.f.QuadPart;
        if (std::fabs(p - period) > period * 0.005) return false;
        if (ti.qpcVBlank == last_) return false; // the same vblank again: it says nothing new, and a stalled one must not
        last_ = ti.qpcVBlank;                    // keep the grid alive
        *t = q.seconds((int64_t)ti.qpcVBlank);
        return true;
    }

private:
    using Get = HRESULT(WINAPI *)(HWND, DWM_TIMING_INFO *);
    Get get_ = nullptr;
    UINT64 last_ = 0;
};

// The display the game's window is on: its nominal timing and its scanline
class Display {
public:
    Display()
    {
        HMODULE g = GetModuleHandleW(L"gdi32.dll");
        open_ = g ? (KmtOpenFn)GetProcAddress(g, "D3DKMTOpenAdapterFromGdiDisplayName") : nullptr;
        close_ = g ? (KmtCloseFn)GetProcAddress(g, "D3DKMTCloseAdapter") : nullptr;
        scan_ = g ? (KmtScanFn)GetProcAddress(g, "D3DKMTGetScanLine") : nullptr;
    }
    ~Display() { close(); }

    // The monitor's GDI name and timing (QueryDisplayConfig, ~0.1-0.3 ms); the scanline handle follows the name
    void update(HMONITOR mon)
    {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof mi;
        if (!mon || !GetMonitorInfoW(mon, &mi)) {
            timing_ = Timing{};
            return;
        }
        timing_ = query_timing(mi.szDevice);
        if (wcscmp(mi.szDevice, name_)) {
            close();
            wcscpy_s(name_, mi.szDevice);
            if (open_ && scan_ && !g_cfg.no_scanline) {
                KmtOpenFromName o = {};
                wcscpy_s(o.DeviceName, mi.szDevice);
                LONG st = open_(&o);
                if (st == 0) {
                    adapter_ = o.hAdapter;
                    source_ = o.VidPnSourceId;
                } else {
                    logf("[clock] %ls: no scanline handle (D3DKMTOpenAdapterFromGdiDisplayName 0x%08lx)", name_, st);
                }
            }
        }
    }
    // The scanline now; t is the middle of the call (it takes ~60 us)
    bool read(const Qpc &q, double *t, bool *in_vblank, uint32_t *line)
    {
        if (!adapter_) return false;
        KmtScanLine s = {adapter_, source_};
        double a = q.now();
        LONG st = scan_(&s);
        double b = q.now();
        calls.push_back((float)((b - a) * 1e6));
        if (st != 0) {
            failed++;
            if (failures_++ < 3) logf("[clock] %ls: D3DKMTGetScanLine 0x%08lx", name_, st);
            return false;
        }
        if (b - a > 0.0002) { // preempted in the call (it takes ~60 us): its time says little
            slow++;
            return false;
        }
        *t = (a + b) / 2;
        *in_vblank = s.InVerticalBlank != 0;
        *line = s.ScanLine;
        blank += *in_vblank;
        lowest = std::min(lowest, *line);
        highest = std::max(highest, *line);
        return true;
    }
    // the minute's readings, for the log
    std::vector<float> calls; // us a call
    uint32_t failed = 0, slow = 0, blank = 0, lowest = ~0u, highest = 0;
    void clear_counts()
    {
        calls.clear();
        failed = slow = blank = highest = 0;
        lowest = ~0u;
    }
    const Timing &timing() const { return timing_; }
    const wchar_t *name() const { return name_; }
    uint32_t signal_w() const { return signal_w_; }
    bool readable() const { return adapter_ != 0; }

private:
    Timing query_timing(const wchar_t *gdi_name)
    {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return {};
        paths_.resize(np);
        modes_.resize(nm);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths_.data(), &nm, modes_.data(), nullptr) != ERROR_SUCCESS) return {};
        for (UINT32 i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn = {};
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof sn;
            sn.header.adapterId = paths_[i].sourceInfo.adapterId;
            sn.header.id = paths_[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, gdi_name)) continue;
            UINT32 mi = paths_[i].targetInfo.modeInfoIdx;
            if (mi == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || mi >= nm || modes_[mi].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
                return {};
            const auto &v = modes_[mi].targetMode.targetVideoSignalInfo;
            // interlaced and unknown timings: a scanline means something else there
            if (!v.vSyncFreq.Numerator || !v.vSyncFreq.Denominator || !v.totalSize.cy || v.activeSize.cy >= v.totalSize.cy ||
                v.scanLineOrdering != DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE)
                return {};
            signal_w_ = v.activeSize.cx;
            return Timing{(double)v.vSyncFreq.Denominator / v.vSyncFreq.Numerator, v.totalSize.cy, v.activeSize.cy};
        }
        return {};
    }
    void close()
    {
        if (adapter_ && close_) {
            KmtClose c = {adapter_};
            close_(&c);
        }
        adapter_ = 0;
        name_[0] = 0;
    }

    KmtOpenFn open_;
    KmtCloseFn close_;
    KmtScanFn scan_;
    KmtHandle adapter_ = 0;
    UINT source_ = 0;
    WCHAR name_[32] = {};
    Timing timing_;
    uint32_t signal_w_ = 0, failures_ = 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths_;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes_;
};

// Sleeps until `until` (seconds on q) without keeping a core busy: this thread runs at time-critical priority, and
// Sleep(0) yields only to threads of its own priority or higher, so the 2.2 loop's last ~2 ms of Sleep(0) a tick (twice a
// tick here) took that core from the game's movie decoder (movie frames shown for 3 refreshes, 2026-11-26). A
// high-resolution waitable timer (Windows 10 1803+) wakes within ~0.5 ms; `precise` then spins the last 0.3 ms with
// pause instructions. Without that timer: Sleep(1) at timeBeginPeriod(1) to 2 ms before, then the same spin.
class Sleeper {
public:
    Sleeper() { timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); }
    ~Sleeper()
    {
        if (timer_) CloseHandle(timer_);
    }
    bool high_resolution() const { return timer_ != nullptr; }
    void until(const Qpc &q, double t, bool precise)
    {
        const double spin = precise ? 0.0003 : 0;
        for (;;) {
            double left = t - q.now() - spin;
            if (left <= 0) break;
            if (timer_) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)(left * 1e7); // relative, 100 ns units
                if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer_, 1000);
                    continue;
                }
            }
            if (left > 0.002) Sleep(1);
            else break;
        }
        while (q.now() < t) YieldProcessor();
    }

private:
    HANDLE timer_ = nullptr;
};

// Keeps the desktop compositor composing at every refresh while the game shows in a window or borderless: when frames
// come every other refresh (the 30 fps movies), DWM idles in between and, woken by the next Present, now and then took
// it a refresh late -- 31 movie frames in 20 s shown for 3 refreshes; with a DwmFlush loop running beside the game, 0,
// as with the 2.2 clock, whose tick was DwmFlush (2026-11-26, Iris Xe, 60 Hz). Its own thread: only it waits.
class CompositorKeeper {
public:
    CompositorKeeper()
    {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        flush_ = m ? (Flush)GetProcAddress(m, "DwmFlush") : nullptr;
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (flush_ && wake_) CloseHandle(CreateThread(nullptr, 0, &CompositorKeeper::run, this, 0, nullptr));
    }
    void keep(bool on)
    {
        if (on == on_.load()) return;
        on_ = on;
        if (on) SetEvent(wake_);
    }

private:
    using Flush = HRESULT(WINAPI *)();
    static DWORD WINAPI run(void *p)
    {
        auto *k = (CompositorKeeper *)p;
        for (;;) {
            if (!k->on_.load()) {
                WaitForSingleObject(k->wake_, INFINITE);
                continue;
            }
            if (FAILED(k->flush_())) Sleep(16); // no composition (minimised, a session without DWM)
        }
    }
    Flush flush_ = nullptr;
    HANDLE wake_ = nullptr;
    std::atomic<bool> on_{false};
};

} // namespace

struct Ticker::Impl {
    Qpc q;
    Sleeper sleeper;
    CompositorKeeper keeper;
    Clock clock;
    Display display;
    CompositorTiming compositor;
    double due = 0, mid = 0, rot = 0;
    uint32_t ticks = 0;
    HMONITOR monitor = nullptr;
    bool exclusive = false;
    // what the log last said
    State said_state = State::Free;
    std::string said_reason;
    int said = 0;
    // the minute's figures
    double minute_from = 0;
    uint32_t minute_ticks = 0, minute_following = 0, minute_late = 0;
    std::vector<float> errors, wakes; // phase errors while following; how late each tick woke, us
    bool holding = false; // the variable-refresh hold, as the log last said

    // Readings of where the display is: its scanline and the compositor's last vblank, each to its own grid
    void sample()
    {
        double t;
        bool vb;
        uint32_t line;
        if (display.read(q, &t, &vb, &line)) clock.sample(t, vb, line);
        if (compositor.last_vblank(q, display.timing().period, &t)) clock.vblank_at(t);
    }
    // Every 2 s: whether Presents are held, and to what 97% of them took after their tick plus 0.5 ms (1.5-8 ms). Held
    // where the screen may refresh when a frame comes, which is where it cannot be followed: a display seen following our
    // ticks (variable refresh), or a refresh above 61 Hz that is no multiple of ~60 (144, 165 Hz). There a variable
    // refresh rate shows each frame as it is presented (G-SYNC at 165 Hz: 30% of frames 16.2-17.2 ms apart, the rest
    // 13-20 ms, 2026-11-26); a fixed one shows them unevenly whatever is done, and the hold costs ~4 ms of latency. By
    // default exclusive and borderless fullscreen switch such screens to 60 Hz and are followed (display_refresh.cpp):
    // the hold is for windows and for players who keep the high refresh. (A check of the scanline's share in the
    // blanking was tried first: it flapped around its threshold and never saw G-SYNC in a window.)
    void decide_hold()
    {
        uint32_t b[PresentTimes::kBins];
        const uint32_t presents = g_present_recent.take(b);
        const Timing &tm = display.timing();
        const bool unfollowable = tm.period > 0 && 1 / tm.period > 61 && !clock.refreshes();
        const bool wait = !g_cfg.present_early &&
                          (clock.state() == State::FollowsUs || (clock.state() == State::Free && unfollowable));
        double ms = 0;
        if (wait && presents >= 30) ms = std::min(8.0, std::max(1.5, PresentTimes::percentile(b, presents, 0.97) + 0.5));
        else if (wait) ms = holding ? (double)g_hold.load() / q.f.QuadPart * 1000 : 0; // too few frames to judge: as it was
        g_hold.store((int64_t)(ms / 1000 * q.f.QuadPart));
        const bool hold = ms > 0;
        if (hold != holding && said++ < 200) {
            if (hold) logf("[clock] Presents held %.2f ms after their tick (a refresh that may follow the frames)", ms);
            else logf("[clock] Presents no longer held");
        }
        holding = hold;
    }
    const char *phase_source() const { return clock.source() == Source::Scanline ? "scanline" : "compositor"; }
    void update_display(bool force)
    {
        HWND w = host3d::window();
        HMONITOR m = w ? MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        bool ex = host3d::exclusive();
        if (!force && m == monitor && ex == exclusive) return;
        monitor = m;
        exclusive = ex;
        display.update(m);
        clock.set_timing(display.timing());
    }
    std::string free_reason() const
    {
        char b[160];
        const Timing &tm = display.timing();
        if (g_cfg.timer_vblank) return "the fixed clock is asked (black.ini timer_vblank)";
        if (g_cfg.background && !g_cfg.display_vblank) return "--background";
        if (clock.state() == State::FollowsUs) return "the display's refreshes follow the presents (variable refresh)";
        if (!display.readable() && !compositor.available()) return "neither the scanline nor the compositor's timing can be read";
        if (tm.period <= 0) return "the display's timing is unknown";
        if (!clock.refreshes()) {
            snprintf(b, sizeof b, "%.3f Hz is no whole multiple of a rate near 59.94 Hz", 1 / tm.period);
            return b;
        }
        return "no steady reading of the display's vblanks (scanline or compositor)";
    }
    void note_state()
    {
        State s = clock.state();
        std::string reason = s == State::Following ? "" : free_reason();
        if (s == said_state && reason == said_reason) return;
        said_state = s;
        said_reason = reason;
        if (said++ >= 200) return;
        if (s == State::Following) {
            const double p = clock.display_period();
            logf("[clock] following %ls by its %s: %.4f Hz (%u lines, %u shown), a tick every %d refresh%s = %.4f Hz, %.2f ms after its "
                 "vblank%s",
                 display.name(), phase_source(), 1 / p, display.timing().vtotal, display.timing().vactive, clock.refreshes(),
                 clock.refreshes() > 1 ? "es" : "", 1 / (clock.refreshes() * p),
                 1000 * (clock.phase > 0 ? clock.phase : Clock::phase_for(p)), exclusive ? " (exclusive)" : "");
        } else {
            logf("[clock] free 59.94 Hz timer: %s%s", reason.c_str(), exclusive ? " (exclusive)" : "");
        }
    }
    void minute(double now)
    {
        if (now - minute_from < 60) return;
        auto pct = [](std::vector<float> &v, double p) {
            if (v.empty()) return 0.0f;
            std::nth_element(v.begin(), v.begin() + (size_t)(p * (v.size() - 1)), v.end());
            return v[(size_t)(p * (v.size() - 1))];
        };
        logf("[clock] last %.0f s: %u ticks = %.4f Hz, %u following (phase error median %.0f us, 99%% %.0f, max %.0f); woke late "
             "median %.0f us, 99%% %.0f, max %.0f; %u more than a tick behind",
             now - minute_from, minute_ticks, minute_ticks / (now - minute_from), minute_following, pct(errors, 0.5),
             pct(errors, 0.99), pct(errors, 1.0), pct(wakes, 0.5), pct(wakes, 0.99), pct(wakes, 1.0), minute_late);
        uint32_t bins[PresentTimes::kBins];
        if (uint32_t presents = g_present_minute.take(bins))
            logf("[clock] Presents: %u, after their tick (before any hold) median %.2f ms, 90%% %.2f, 99%% %.2f, %u over 20 ms%s",
                 presents, PresentTimes::percentile(bins, presents, 0.5), PresentTimes::percentile(bins, presents, 0.9),
                 PresentTimes::percentile(bins, presents, 0.99), bins[PresentTimes::kBins - 1], holding ? "; held" : "");
        const Grid &gs = clock.grid(Source::Scanline), &gc = clock.grid(Source::Compositor);
        logf("[clock] scanline: %zu calls (median %.0f us, 99%% %.0f), %u failed, %u too slow, %u in the blanking, lines %u-%u, "
             "%u on its grid, %u off, %u restarts; compositor: %u on its grid, %u off, %u restarts; following the %s",
             display.calls.size(), pct(display.calls, 0.5), pct(display.calls, 0.99), display.failed, display.slow, display.blank,
             display.lowest == ~0u ? 0 : display.lowest, display.highest, gs.hits, gs.offs, gs.restarts, gc.hits, gc.offs,
             gc.restarts, clock.state() == State::Following ? phase_source() : "timer");
        display.clear_counts();
        clock.clear_grid_counts();
        minute_from = now;
        minute_ticks = minute_following = minute_late = 0;
        errors.clear();
        wakes.clear();
    }
};

Ticker::Ticker() : d(new Impl)
{
    timeBeginPeriod(1);
    d->clock.phase = g_cfg.tick_phase_ms / 1000; // tests: --tick-phase MS (0: by the refresh)
    d->clock.allow_follow(!g_cfg.timer_vblank && !(g_cfg.background && !g_cfg.display_vblank));
    double a = d->q.now();
    d->update_display(true);
    logf("[clock] display %ls: %.4f Hz, %u of %u lines shown, scanline %s (read in %.2f ms); %s timer", d->display.name(),
         d->display.timing().period > 0 ? 1 / d->display.timing().period : 0.0, d->display.timing().vactive,
         d->display.timing().vtotal, d->display.readable() ? "readable" : "not readable", (d->q.now() - a) * 1000,
         d->sleeper.high_resolution() ? "high-resolution" : "1 ms");
    d->due = d->mid = d->minute_from = d->q.now();
    d->note_state();
}

int64_t Ticker::wait()
{
    if (d->mid > d->q.now()) { // a reading in the middle of the tick: right after it may always fall in the blanking
        d->sleeper.until(d->q, d->mid, false);
        d->sample();
    }
    d->sleeper.until(d->q, d->due, true);
    double now = d->q.now();
    d->wakes.push_back((float)((now - d->due) * 1e6));
    if (now - d->due > d->clock.tick_period()) { // more than a tick behind (the PC stalled): start again from now
        d->due = now;
        d->minute_late++;
    }
    return d->q.ticks(d->due);
}

void Ticker::after_tick()
{
    Impl &m = *d;
    m.sample();
    if (++m.ticks % 120 == 0) {
        m.update_display(true); // a mode change on the same monitor: every 2 s
        m.decide_hold();
    } else if (m.ticks % 15 == 0) {
        m.update_display(false); // the window moved to another monitor, or went exclusive
    }
    const double prev = m.due;
    m.due = m.clock.next(prev);
    m.rot = m.rot + 0.2 < 1 ? m.rot + 0.2 : m.rot - 0.8;
    m.mid = prev + (0.15 + 0.7 * m.rot) * (m.due - prev);
    g_tick_period.store(m.due - prev);
    m.minute_ticks++;
    if (m.clock.state() == State::Following) {
        m.minute_following++;
        m.errors.push_back((float)(std::fabs(m.clock.error()) * 1e6));
    }
    m.keeper.keep(m.clock.state() == State::Following && !m.exclusive);
    m.note_state();
    m.minute(prev);
}

double tick_period() { return g_tick_period.load(); }

void before_present(int64_t tick, bool may_hold)
{
    static const Qpc q;
    const double ms = (q.ticks(q.now()) - tick) * 1000.0 / q.f.QuadPart;
    g_present_minute.add(ms);
    g_present_recent.add(ms);
    const int64_t hold = may_hold ? g_hold.load() : 0;
    if (!hold) return;
    thread_local Sleeper sleeper; // the game's render thread: its own timer
    sleeper.until(q, q.seconds(tick + hold), true);
}

} // namespace frame_clock
