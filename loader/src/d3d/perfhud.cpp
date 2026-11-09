#include "d3d/perfhud.h"

#include "d3d/draw.h"
#include "d3d/host.h"
#include "common.h"

#include <d3dcompiler.h>
#include <dxgi.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace perfhud {

namespace {

std::atomic<uint32_t> g_frames;   // Swaps since start
std::atomic<uint64_t> g_thread_us; // the thread's time for its last 20 updates (its cost, logged once)
std::atomic<uint64_t> g_input_change_us; // a controller/key change and no present since
std::atomic<int> g_pic_h{720};    // the picture's height: the text is sized from it
std::string g_gpu, g_cpu;         // names
std::string g_ram_type = "-";     // DDR4 / DDR5 / ...
std::string g_disk_type = "-";    // SSD / SSD (NVMe) / HDD
int g_disk_number = -1;            // physical disk backing the port
wchar_t g_luid[64];               // "luid_0x%08X_0x%08X" of the card, as in the GPU Engine counters' instance names

// Input-to-present samples from the last 64 input changes, and the last poll/frame to present times. The render
// thread writes them at each Swap; the stats thread reads them once a second.
std::mutex g_latency_lock;
constexpr int kLatencySamples = 64;
uint64_t g_input_samples[kLatencySamples];
int g_input_next, g_input_count;
uint64_t g_frame_last;
uint64_t g_last_present_us;

// Frame-to-frame intervals, timestamped like the recomp's flip counter. The render thread writes them at Swap; the
// stats thread reads the last window once a second. 256 entries cover several seconds at 60/120 fps.
struct FrameSample {
    uint64_t at_us;
    uint64_t interval_us;
};
std::mutex g_frame_lock;
constexpr int kFrameSamples = 256;
FrameSample g_frame_samples[kFrameSamples];
int g_frame_next, g_frame_count;
uint64_t g_frame_last_us;

uint64_t now_us()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A text panel: the bitmap drawn for it, waiting for the render thread (premultiplied BGRA, top-down), and its texture
struct Panel {
    std::vector<uint32_t> pixels;
    int bw = 0, bh = 0;
    bool fresh = false;
    host3d::CpuTexture tex; // tex.w x tex.h: the size it was made at
};
std::mutex g_lock;
Panel g_hud;  // the figures, top-right
Panel g_note; // a short message (notice()), bottom-right

// notice(): the text waiting to be drawn and until when it shows (GetTickCount64)
std::mutex g_note_lock;
std::wstring g_note_text;
bool g_note_new;
std::atomic<uint64_t> g_note_until;

IDirect3DPixelShader9 *g_ps;
IDirect3DStateBlock9 *g_saved; // just the states the overlay sets (made once; a full state block a frame cost ~0.3 ms, r324)

std::string cpu_name()
{
    char v[128] = "";
    DWORD size = sizeof v - 1;
    HKEY k;
    if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_QUERY_VALUE, &k)) {
        RegQueryValueExA(k, "ProcessorNameString", nullptr, nullptr, (BYTE *)v, &size);
        RegCloseKey(k);
    }
    std::string s = v;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s.empty() ? "CPU" : s;
}

uint64_t ft(const FILETIME &f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

std::string short_gpu(std::string s)
{
    for (const char *p : {"NVIDIA ", "AMD ", "Intel(R) ", "Intel "}) {
        size_t n = strlen(p);
        if (s.compare(0, n, p) == 0) {
            s.erase(0, n);
            break;
        }
    }
    for (const char *p : {" Graphics", " graphics"}) {
        size_t n = strlen(p);
        if (s.size() >= n && s.compare(s.size() - n, n, p) == 0) s.erase(s.size() - n);
    }
    if (s.compare(0, 8, "GeForce ") == 0) s.erase(0, 8);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s.empty() ? "-" : s;
}

std::string short_cpu(std::string s)
{
    size_t p = s.find("Core(TM) ");
    if (p != std::string::npos) s = s.substr(p + 9);
    else if ((p = s.find("Ryzen ")) != std::string::npos) s = s.substr(p);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s.empty() ? "-" : s;
}

// SMBIOS type 17: the installed memory's DDR generation (the same source Windows' System Information shows).
std::string smbios_memory_type()
{
    DWORD size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (size < 8) return "-";
    std::vector<uint8_t> b(size);
    if (GetSystemFirmwareTable('RSMB', 0, b.data(), size) != size) return "-";
    uint32_t len = 0;
    memcpy(&len, b.data() + 4, 4);
    uint8_t *p = b.data() + 8, *end = b.data() + std::min<size_t>(size, (size_t)8 + len);
    while (p + 4 <= end) {
        uint8_t type = p[0], n = p[1];
        if (!n || type == 127) break;
        if (type == 17 && n > 0x13) {
            uint16_t capacity = 0;
            memcpy(&capacity, p + 0x0C, 2);
            uint8_t mt = p[0x12];
            if (capacity && mt) {
                switch (mt) {
                case 0x18: return "DDR3";
                case 0x1A: return "DDR4";
                case 0x1D: return "LPDDR3";
                case 0x1E: return "LPDDR4";
                case 0x22: return "DDR5";
                case 0x23: return "LPDDR5";
                default: return "-";
                }
            }
        }
        p += n;
        while (p + 1 < end && !(p[0] == 0 && p[1] == 0)) p++;
        p += 2;
    }
    return "-";
}

// The port's drive: partition -> physical disk -> seek penalty (SSD/HDD) and bus (NVMe/SATA/USB).
std::string drive_type()
{
    wchar_t volume[MAX_PATH] = L"";
    if (!GetVolumePathNameW(g_cfg.port_dir.c_str(), volume, MAX_PATH)) return "-";
    std::wstring vol = volume;
    if (vol.size() < 2) return "-";
    std::wstring device = L"\\\\.\\" + vol.substr(0, 2);
    HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "-";
    std::vector<uint8_t> ext(sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 8);
    DWORD ret = 0;
    DWORD disk = 0;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, ext.data(), (DWORD)ext.size(), &ret, nullptr) &&
        ret >= sizeof(VOLUME_DISK_EXTENTS)) {
        disk = ((VOLUME_DISK_EXTENTS *)ext.data())->Extents[0].DiskNumber;
        g_disk_number = (int)disk;
    } else {
        CloseHandle(h);
        return "-";
    }
    CloseHandle(h);
    std::wstring phys = L"\\\\.\\PhysicalDrive" + std::to_wstring(disk);
    HANDLE d = CreateFileW(phys.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (d == INVALID_HANDLE_VALUE) return "-";
    STORAGE_PROPERTY_QUERY q = {StorageDeviceSeekPenaltyProperty, PropertyStandardQuery};
    DEVICE_SEEK_PENALTY_DESCRIPTOR sp = {};
    bool ssd = false, have_seek = false;
    if (DeviceIoControl(d, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &sp, sizeof sp, &ret, nullptr)) {
        ssd = !sp.IncursSeekPenalty;
        have_seek = true;
    }
    STORAGE_PROPERTY_QUERY q2 = {StorageDeviceProperty, PropertyStandardQuery};
    std::vector<uint8_t> db(1024);
    uint32_t bus = 0;
    if (DeviceIoControl(d, IOCTL_STORAGE_QUERY_PROPERTY, &q2, sizeof q2, db.data(), (DWORD)db.size(), &ret, nullptr) &&
        ret >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
        bus = ((STORAGE_DEVICE_DESCRIPTOR *)db.data())->BusType;
    CloseHandle(d);
    if (!have_seek) return "-";
    const char *kind = ssd ? "SSD" : "HDD";
    if (bus == BusTypeNvme) return std::string(kind) + " (NVMe)";
    if (bus == BusTypeSata) return std::string(kind) + " (SATA)";
    if (bus == BusTypeUsb) return std::string(kind) + " (USB)";
    return kind;
}

struct CorePerf {
    uint64_t idle, kernel, user;
};

bool query_core_times(std::vector<CorePerf> &out)
{
    struct Raw {
        LARGE_INTEGER idle, kernel, user, dpc, interrupt;
        ULONG count;
    };
    using NtQuerySystemInformationFn = LONG(NTAPI *)(ULONG, PVOID, ULONG, PULONG);
    static NtQuerySystemInformationFn query = [] {
        return (NtQuerySystemInformationFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    }();
    if (!query) return false;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::vector<Raw> raw(si.dwNumberOfProcessors);
    ULONG got = 0;
    if (query(8 /* SystemProcessorPerformanceInformation */, raw.data(), (ULONG)(raw.size() * sizeof(Raw)), &got) < 0) return false;
    out.resize(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        out[i].idle = (uint64_t)raw[i].idle.QuadPart;
        out[i].kernel = (uint64_t)raw[i].kernel.QuadPart;
        out[i].user = (uint64_t)raw[i].user.QuadPart;
    }
    return true;
}

// The busiest thread of this process, as a share of one logical core. A single game thread can be at 100% while the
// whole process looks low when its time is divided by all processors.
double game_max_thread_cpu(double secs, int core_count, std::vector<double> &game_core, std::vector<uint8_t> &game_core_seen,
                           bool &core_known)
{
    static std::map<DWORD, uint64_t> previous;
    std::map<DWORD, uint64_t> current;
    double best = 0;
    core_known = false;
    game_core.assign(core_count, 0.0);
    game_core_seen.assign(core_count, 0);
    auto account = [&](HANDLE th) {
        FILETIME c, e, k, u;
        if (!GetThreadTimes(th, &c, &e, &k, &u)) return;
        DWORD tid = GetThreadId(th);
        uint64_t t = ft(k) + ft(u);
        int core = -1;
        GROUP_AFFINITY ga = {};
        if (GetThreadGroupAffinity(th, &ga) && ga.Mask && !(ga.Mask & (ga.Mask - 1))) {
            int bit = 0;
            while (bit < 64 && !(ga.Mask & ((KAFFINITY)1 << bit))) bit++;
            core = (int)ga.Group * 64 + bit;
            if (core < core_count) core_known = true;
            else core = -1;
        }
        auto it = previous.find(tid);
        if (it != previous.end() && t >= it->second) {
            double pct = 100.0 * (t - it->second) / (secs * 1e7);
            best = std::max(best, pct);
            if (core >= 0) {
                game_core[core] += pct;
                game_core_seen[core] = 1;
            }
        }
        current[tid] = t;
    };
    // NtGetNextThread walks only this process's threads; CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD) snapshots every
    // thread on the system and took 70 ms on this machine.
    using NtGetNextThreadFn = LONG(NTAPI *)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
    static NtGetNextThreadFn next_thread = [] {
        return (NtGetNextThreadFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread");
    }();
    if (next_thread) {
        HANDLE th = nullptr;
        for (;;) {
            HANDLE next = nullptr;
            LONG status = next_thread(GetCurrentProcess(), th, THREAD_QUERY_INFORMATION, 0, 0, &next);
            if (th) CloseHandle(th);
            th = next;
            if (status < 0 || !th) break;
            account(th);
        }
        if (th) CloseHandle(th);
    } else {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        THREADENTRY32 te = {sizeof te};
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!th) continue;
                account(th);
                CloseHandle(th);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
    previous.swap(current);
    return best;
}

// White text over a translucent dark panel, drawn with GDI into a 32-bit DIB: the text is drawn white on black and
// its brightness becomes both colour and opacity (premultiplied), the panel 60% black.
void render(const std::vector<std::wstring> &lines, Panel &panel, bool mono = false)
{
    int px = std::clamp(g_pic_h.load() / 60, 9, 30);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                             mono ? L"Consolas" : L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    int w = 0, line_h = px * 9 / 8, pad = px / 2;
    for (const std::wstring &l : lines) {
        SIZE s;
        GetTextExtentPoint32W(dc, l.c_str(), (int)l.size(), &s);
        w = std::max(w, (int)s.cx);
    }
    w += pad * 2;
    int h = line_h * (int)lines.size() + pad * 2;
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old_bmp = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)w * h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        for (size_t i = 0; i < lines.size(); i++)
            TextOutW(dc, pad, pad + (int)i * line_h, lines[i].c_str(), (int)lines[i].size());
        GdiFlush();
        std::vector<uint32_t> out((size_t)w * h);
        const uint32_t *p = (const uint32_t *)bits;
        for (size_t i = 0; i < out.size(); i++) {
            uint32_t c = p[i], v = std::max({c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF});
            uint32_t a = std::max(v, 150u);
            out[i] = a << 24 | v << 16 | v << 8 | v;
        }
        SelectObject(dc, old_bmp);
        std::lock_guard<std::mutex> lk(g_lock);
        panel.pixels.swap(out);
        panel.bw = w;
        panel.bh = h;
        panel.fresh = true;
    }
    if (bmp) DeleteObject(bmp);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
}

DWORD WINAPI stats_thread(void *)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER gpu = nullptr, disk = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &gpu) != ERROR_SUCCESS) gpu = nullptr;
        if (PdhAddEnglishCounterW(q, L"\\PhysicalDisk(*)\\% Idle Time", 0, &disk) != ERROR_SUCCESS) disk = nullptr;
        PdhCollectQueryData(q);
    }
    FILETIME idle0, kern0, user0, c0, e0, pk0, pu0;
    GetSystemTimes(&idle0, &kern0, &user0);
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &pk0, &pu0);
    IO_COUNTERS io0 = {};
    GetProcessIoCounters(GetCurrentProcess(), &io0);
    uint32_t frames0 = g_frames;
    uint64_t t0 = now_us();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::vector<uint8_t> buf;
    logf("[perfhud] on: GPU '%s' (%ls), CPU '%s', counters: GPU %s, disk %s", g_gpu.c_str(), g_luid, g_cpu.c_str(),
         gpu ? "yes" : "no", disk ? "yes" : "no");
    for (;;) {
        Sleep(1000);
        uint64_t t1 = now_us();
        double secs = std::max(0.001, (t1 - t0) / 1e6);
        uint32_t frames1 = g_frames;
        double fps = (frames1 - frames0) / secs;
        // processor: the whole system from its idle time, and this program from its own times
        FILETIME idle1, kern1, user1, c1, e1, pk1, pu1;
        GetSystemTimes(&idle1, &kern1, &user1);
        GetProcessTimes(GetCurrentProcess(), &c1, &e1, &pk1, &pu1);
        double all = (double)(ft(kern1) - ft(kern0) + ft(user1) - ft(user0)), idle = (double)(ft(idle1) - ft(idle0));
        double cpu = all > 0 ? 100.0 * (all - idle) / all : 0;
        double mine = 100.0 * (ft(pk1) - ft(pk0) + ft(pu1) - ft(pu0)) / (secs * 1e7 * si.dwNumberOfProcessors);
        std::vector<double> game_core;
        std::vector<uint8_t> game_core_seen;
        bool core_known = false;
        double max_thread_cpu = game_max_thread_cpu(secs, si.dwNumberOfProcessors, game_core, game_core_seen, core_known);
        static std::vector<CorePerf> previous_cores;
        std::vector<CorePerf> current_cores;
        int max_core = -1;
        double max_core_total = 0, max_core_game = -1;
        if (query_core_times(current_cores)) {
            if (previous_cores.size() == current_cores.size()) {
                for (size_t i = 0; i < current_cores.size(); i++) {
                    uint64_t all = (current_cores[i].kernel + current_cores[i].user) -
                                   (previous_cores[i].kernel + previous_cores[i].user);
                    uint64_t idle = current_cores[i].idle - previous_cores[i].idle;
                    if (!all) continue;
                    double use = 100.0 * (all - std::min(all, idle)) / all;
                    if (use > max_core_total) {
                        max_core_total = use;
                        max_core = (int)i;
                    }
                }
                if (max_core >= 0 && core_known && (size_t)max_core < game_core.size() && game_core_seen[max_core])
                    max_core_game = std::min(100.0, game_core[max_core]);
            }
            previous_cores = current_cores;
        }
        // graphics card: the 3D engines of this card. The GPU Engine instances carry the process id, so the
        // game's own share can be summed; if the driver does not expose per-process instances, show the card total.
        double gpu_use = -1, gpu_game_use = -1, disk_use = -1;
        const std::wstring game_pid = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
        if (q && PdhCollectQueryData(q) == ERROR_SUCCESS) {
            if (gpu) {
                DWORD size = 0, count = 0;
                if (PdhGetFormattedCounterArrayW(gpu, PDH_FMT_DOUBLE, &size, &count, nullptr) == PDH_MORE_DATA) {
                    buf.resize(size);
                    auto *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)buf.data();
                    if (PdhGetFormattedCounterArrayW(gpu, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
                        std::map<std::wstring, double> per_engine, all_engine;
                        std::map<std::wstring, double> game_engine;
                        const std::wstring card_luid = g_luid;
                        bool card_seen = false;
                        for (DWORD i = 0; i < count; i++) {
                            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
                            std::wstring name = items[i].szName;
                            const wchar_t *l = wcsstr(name.c_str(), L"luid_");
                            std::wstring engine = l ? l : name;
                            bool on_card = card_luid.empty() || name.find(card_luid) != std::wstring::npos;
                            if (on_card) {
                                per_engine[engine] += items[i].FmtValue.doubleValue;
                                card_seen = true;
                            }
                            all_engine[engine] += items[i].FmtValue.doubleValue;
                            if (on_card && name.compare(0, game_pid.size(), game_pid) == 0)
                                game_engine[engine] = std::max(game_engine[engine], items[i].FmtValue.doubleValue);
                        }
                        auto &source = card_seen ? per_engine : all_engine;
                        for (auto &kv : source) gpu_use = std::max(gpu_use, kv.second);
                        for (auto &kv : game_engine) gpu_game_use = std::max(gpu_game_use, kv.second);
                    }
                }
            }
            if (disk) {
                DWORD size = 0, count = 0;
                if (PdhGetFormattedCounterArrayW(disk, PDH_FMT_DOUBLE, &size, &count, nullptr) == PDH_MORE_DATA) {
                    buf.resize(size);
                    auto *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)buf.data();
                    if (PdhGetFormattedCounterArrayW(disk, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
                        const std::wstring prefix = std::to_wstring(g_disk_number) + L" ";
                        double busy = -1, all = -1;
                        for (DWORD i = 0; i < count; i++) {
                            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
                            std::wstring name = items[i].szName;
                            double v = 100.0 - items[i].FmtValue.doubleValue;
                            if (name == L"_Total") all = v;
                            if (g_disk_number >= 0 && (name == std::to_wstring(g_disk_number) || name.rfind(prefix, 0) == 0)) busy = v;
                        }
                        disk_use = busy >= 0 ? busy : all; // the port's physical disk; _Total only as a fallback
                    }
                }
            }
        }
        MEMORYSTATUSEX ms = {sizeof ms};
        GlobalMemoryStatusEx(&ms);
        PROCESS_MEMORY_COUNTERS_EX pm = {sizeof pm};
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
        IO_COUNTERS io1 = {};
        GetProcessIoCounters(GetCurrentProcess(), &io1);
        double read_mb = (io1.ReadTransferCount - io0.ReadTransferCount) / secs / (1024.0 * 1024.0);
        double write_mb = (io1.WriteTransferCount - io0.WriteTransferCount) / secs / (1024.0 * 1024.0);

        const double gb = 1024.0 * 1024.0 * 1024.0;
        double in_avg = 0, in_max = 0, in_last = 0, frame_last = 0;
        int input_count = 0;
        {
            std::lock_guard<std::mutex> lk(g_latency_lock);
            input_count = g_input_count;
            for (int i = 0; i < input_count; i++) {
                double v = g_input_samples[i] / 1000.0;
                in_avg += v;
                if (v > in_max) in_max = v;
            }
            if (input_count) in_avg /= input_count;
            if (input_count) {
                int last = (g_input_next + kLatencySamples - 1) % kLatencySamples;
                in_last = g_input_samples[last] / 1000.0;
            }
            frame_last = g_frame_last / 1000.0;
        }
        double frame_max_ms = 0;
        {
            std::lock_guard<std::mutex> lk(g_frame_lock);
            uint64_t cutoff = t0; // every frame since the last update (once a second)
            for (int i = 0; i < g_frame_count; i++) {
                const FrameSample &s = g_frame_samples[i];
                if (s.at_us >= cutoff) frame_max_ms = std::max(frame_max_ms, s.interval_us / 1000.0);
            }
        }
        auto row = [](std::wstring a, const std::wstring &b) {
            const size_t col = 27;
            if (a.size() < col) a.append(col - a.size(), L' ');
            return a + L"   " + b; // 3 spaces between the value column and the next label
        };
        wchar_t a[128], b[128];
        std::vector<std::wstring> lines;
        if (fps > 0.0) {
            swprintf(a, 128, L"FPS %.0f", fps);
            if (frame_max_ms > 0) swprintf(b, 128, L"Frame %.1f ms  max %.1f ms", 1000.0 / fps, frame_max_ms);
            else swprintf(b, 128, L"Frame %.1f ms", 1000.0 / fps);
        } else {
            swprintf(a, 128, L"FPS 0");
            swprintf(b, 128, L"Frame -");
        }
        lines.push_back(row(a, b));
        if (input_count) {
            swprintf(a, 128, L"Input avg %.1f ms", in_avg);
            swprintf(b, 128, L"max %.1f ms", in_max);
        } else {
            swprintf(a, 128, L"Input avg -");
            swprintf(b, 128, L"max -");
        }
        lines.push_back(row(a, b));
        if (in_last) swprintf(a, 128, L"Poll -> Present %.1f ms", in_last);
        else swprintf(a, 128, L"Poll -> Present -");
        swprintf(b, 128, L"Present -> Present %.1f ms", frame_last);
        lines.push_back(row(a, b));
        lines.push_back(row(L"GPU " + widen(short_gpu(g_gpu)), [&] {
            wchar_t s[128];
            double total = gpu_use >= 0 ? std::min(100.0, gpu_use) : -1;
            double game = gpu_game_use >= 0 ? std::min(total < 0 ? 100.0 : total, gpu_game_use) : -1;
            if (game >= 0 && total >= 0) swprintf(s, 128, L"%.1f%% total (game %.1f%%)", total, game);
            else if (total >= 0) swprintf(s, 128, L"%.1f%% total", total);
            else swprintf(s, 128, L"-");
            return std::wstring(s);
        }()));
        lines.push_back(row(L"CPU " + widen(short_cpu(g_cpu)), [&] {
            wchar_t s[128];
            if (max_thread_cpu > 0) swprintf(s, 128, L"%.0f%% total (game %.0f%%, max thread %.0f%%)", cpu, mine, std::min(100.0, max_thread_cpu));
            else swprintf(s, 128, L"%.0f%% total (game %.0f%%)", cpu, mine);
            return std::wstring(s);
        }()));
        if (max_core >= 0) {
            wchar_t label[64];
            swprintf(label, 64, L"CPU max core #%d", max_core);
            lines.push_back(row(label, [&] {
                wchar_t s[128];
                if (max_core_game >= 0)
                    swprintf(s, 128, L"%.0f%% total (game %.0f%%)", std::min(100.0, max_core_total), max_core_game);
                else swprintf(s, 128, L"%.0f%% total (game n/a)", std::min(100.0, max_core_total));
                return std::wstring(s);
            }()));
        }
        std::wstring ram_type = widen(g_ram_type), disk_type = widen(g_disk_type);
        wchar_t ram_label[128];
        swprintf(ram_label, 128, L"RAM %ls %.1f/%.1f GB", ram_type.c_str(),
                 (ms.ullTotalPhys - ms.ullAvailPhys) / gb, ms.ullTotalPhys / gb);
        lines.push_back(row(ram_label, [&] {
            wchar_t s[128];
            swprintf(s, 128, L"%lu%% (game %.1f GB)", ms.dwMemoryLoad, pm.WorkingSetSize / gb);
            return std::wstring(s);
        }()));
        lines.push_back(row(L"Disk " + disk_type, [&] {
            wchar_t s[128];
            if (disk_use >= 0) swprintf(s, 128, L"%.0f%% (game R %.1f W %.1f MB/s)", std::clamp(disk_use, 0.0, 100.0), read_mb, write_mb);
            else swprintf(s, 128, L"- (game R %.1f W %.1f MB/s)", read_mb, write_mb);
            return std::wstring(s);
        }()));
        render(lines, g_hud, true);
        static int updates;
        static ULONGLONG busy;
        busy += (now_us() - t1) / 1000;
        if (++updates == 20) logf("[perfhud] the overlay thread: %.1f ms per update; CPU %.1f%% total, game %.1f%%, max thread %.1f%%, max core #%d %.1f%% (game %.1f%%)", busy / 20.0, cpu, mine, max_thread_cpu, max_core, max_core_total, max_core_game);

        idle0 = idle1, kern0 = kern1, user0 = user1, pk0 = pk1, pu0 = pu1, io0 = io1;
        frames0 = frames1;
        t0 = t1;
    }
}

} // namespace

unsigned g_vendor, g_device;

void start(const char *gpu_name, unsigned vendor_id, unsigned device_id)
{
    g_gpu = gpu_name;
    g_vendor = vendor_id;
    g_device = device_id;
}

void start_thread()
{
    static bool started;
    if (started) return;
    started = true;
    unsigned vendor_id = g_vendor, device_id = g_device;
    g_cpu = cpu_name();
    g_ram_type = smbios_memory_type();
    g_disk_type = drive_type();
    logf("[perfhud] hardware: GPU '%s', CPU '%s', RAM %s, disk %s", g_gpu.c_str(), g_cpu.c_str(), g_ram_type.c_str(),
         g_disk_type.c_str());
    IDXGIFactory1 *f = nullptr; // the card's LUID, for its engines in the GPU counters
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) {
        IDXGIAdapter1 *a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(a->GetDesc1(&d)) && d.VendorId == vendor_id && d.DeviceId == device_id && !g_luid[0])
                swprintf(g_luid, 64, L"luid_0x%08X_0x%08X", (unsigned)d.AdapterLuid.HighPart, (unsigned)d.AdapterLuid.LowPart);
            a->Release();
        }
        f->Release();
    }
    CloseHandle(CreateThread(nullptr, 0, stats_thread, nullptr, 0, nullptr));
}

void frame()
{
    if (!g_cfg.perfhud) return;
    uint64_t now = now_us();
    {
        std::lock_guard<std::mutex> lk(g_frame_lock);
        if (g_frame_last_us && now >= g_frame_last_us) {
            g_frame_samples[g_frame_next] = {now, now - g_frame_last_us};
            g_frame_next = (g_frame_next + 1) % kFrameSamples;
            if (g_frame_count < kFrameSamples) g_frame_count++;
        }
        g_frame_last_us = now;
    }
    g_frames++;
}

void input_changed()
{
    if (g_cfg.perfhud) g_input_change_us.store(now_us(), std::memory_order_relaxed);
}

void input_presented()
{
    if (!g_cfg.perfhud) return;
    uint64_t now = now_us();
    std::lock_guard<std::mutex> lk(g_latency_lock);
    if (g_last_present_us && now >= g_last_present_us) g_frame_last = now - g_last_present_us;
    g_last_present_us = now;
    uint64_t t = g_input_change_us.exchange(0, std::memory_order_relaxed);
    if (!t || now < t || now - t >= 500000) return; // no input, or a stale change from a paused frame
    g_input_samples[g_input_next] = now - t;
    g_input_next = (g_input_next + 1) % kLatencySamples;
    if (g_input_count < kLatencySamples) g_input_count++;
}

bool active() { return g_cfg.perfhud || g_note_until.load(std::memory_order_relaxed); }

void notice(const std::wstring &text, unsigned ms)
{
    std::lock_guard<std::mutex> lk(g_note_lock);
    g_note_text = text;
    g_note_new = true;
    g_note_until = GetTickCount64() + ms;
}

void draw_quad(IDirect3DDevice9 *d, const RECT &pic, Panel &panel, bool bottom);

void draw(IDirect3DDevice9 *d, const RECT &pic)
{
    static int design_h;
    if (!design_h) design_h = pic.bottom - pic.top;
    g_pic_h = design_h; // one texture size: the quad alone follows a drag, so the text does not jump after it
    if (g_note_until.load(std::memory_order_relaxed)) { // a message (online play): drawn here, on the render thread
        std::wstring text;
        {
            std::lock_guard<std::mutex> lk(g_note_lock);
            if (g_note_new) text = g_note_text;
            g_note_new = false;
        }
        if (!text.empty()) render({text}, g_note);
        if (GetTickCount64() < g_note_until) draw_quad(d, pic, g_note, true);
        else g_note_until = 0;
    }
    if (!g_cfg.perfhud) return;
    start_thread();
    static LARGE_INTEGER f, sum;
    static uint32_t n;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    draw_quad(d, pic, g_hud, false);
    QueryPerformanceCounter(&b);
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    sum.QuadPart += b.QuadPart - a.QuadPart;
    if (++n == 1200) // ~20 s: the render thread's cost of the overlay
        logf("[perfhud] drawing the overlay: %.3f ms a frame on the render thread (average of %u frames)",
             sum.QuadPart * 1000.0 / f.QuadPart / n, n);
}

void draw_quad(IDirect3DDevice9 *d, const RECT &pic, Panel &panel, bool bottom)
{
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (panel.fresh) { // a new bitmap (the figures once a second, a message once): into the texture
            panel.fresh = false;
            int pitch;
            void *px = panel.tex.make(panel.bw, panel.bh) ? panel.tex.lock(&pitch) : nullptr;
            if (px) {
                for (int y = 0; y < panel.tex.h; y++)
                    memcpy((uint8_t *)px + y * pitch, &panel.pixels[(size_t)y * panel.tex.w], panel.tex.w * 4);
                panel.tex.unlock();
            }
        }
    }
    IDirect3DTexture9 *tex = panel.tex.tex;
    if (!tex) return;
    if (!g_ps) {
        const char src[] = "sampler2D t : register(s0);\n"
                           "float4 main(float2 uv : TEXCOORD0) : COLOR0 { return tex2D(t, uv); }\n";
        ID3DBlob *code = nullptr;
        if (SUCCEEDED(D3DCompile(src, sizeof src - 1, "perfhud", nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, nullptr))) {
            d->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &g_ps);
            code->Release();
        }
        if (!g_ps) return;
    }
    // One premultiplied quad at the picture's top-right (the game's health bar is at the top-left) or bottom-right, 1:1,
    // over the back buffer; the states it sets are captured first and put back after
    auto set_states = [&] {
        d->SetVertexShader(nullptr);
        d->SetPixelShader(g_ps);
        d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        d->SetTexture(0, tex);
        d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        for (D3DRENDERSTATETYPE rs : {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
                                      D3DRS_FOGENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE})
            d->SetRenderState(rs, FALSE);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    };
    if (!g_saved) { // a state block of exactly these states (recording does not change the device)
        d->BeginStateBlock();
        set_states();
        d->EndStateBlock(&g_saved);
    }
    D3DVIEWPORT9 old_vp;
    d->GetViewport(&old_vp);
    if (g_saved) g_saved->Capture();
    IDirect3DSurface9 *old_rt = nullptr, *old_z = nullptr, *bb = nullptr;
    d->GetRenderTarget(0, &old_rt);
    d->GetDepthStencilSurface(&old_z);
    d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    d->SetRenderTarget(0, bb);
    d->SetDepthStencilSurface(nullptr);
    set_states();
    // 2.2.7 interaction: one panel at a fixed pixel size at the picture's top-right. Nothing here asks the window
    // for its size, so dragging/resizing it cannot call user32 from the render thread.
    float m = (float)std::max(4L, (pic.bottom - pic.top) / 60);
    float x0 = pic.right - m - panel.tex.w - 0.5f;
    float y0 = (bottom ? pic.bottom - m - panel.tex.h : pic.top + m) - 0.5f;
    float x1 = x0 + panel.tex.w, y1 = y0 + panel.tex.h;
    struct V {
        float x, y, z, rhw, u, v;
    } q[4] = {{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    host3d::begin_scene();
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    d->SetRenderTarget(0, old_rt);
    d->SetDepthStencilSurface(old_z);
    d->SetViewport(&old_vp); // SetRenderTarget reset it
    if (g_saved) g_saved->Apply();
    xd3d::forget_sent_shaders();
    if (bb) bb->Release();
    if (old_rt) old_rt->Release();
    if (old_z) old_z->Release();
}

void release()
{
    if (g_saved) g_saved->Release();
    g_saved = nullptr;
}

void release_all()
{
    release();
    if (g_ps) g_ps->Release();
    g_ps = nullptr;
    std::lock_guard<std::mutex> lk(g_lock);
    for (Panel *p : {&g_hud, &g_note}) {
        p->tex.release();
        p->fresh = !p->pixels.empty(); // the bitmap goes into the new device's texture
    }
}

} // namespace perfhud
