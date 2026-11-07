#include "d3d/perfhud.h"

#include "d3d/draw.h"
#include "d3d/host.h"
#include "common.h"

#include <d3dcompiler.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

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
wchar_t g_luid[64];               // "luid_0x%08X_0x%08X" of the card, as in the GPU Engine counters' instance names

// Input-to-present samples from the last 64 input changes, and the last poll/frame to present times. The render
// thread writes them at each Swap; the stats thread reads them once a second.
std::mutex g_latency_lock;
constexpr int kLatencySamples = 64;
uint64_t g_input_samples[kLatencySamples];
int g_input_next, g_input_count;
uint64_t g_frame_last;
uint64_t g_last_present_us;

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

// White text over a translucent dark panel, drawn with GDI into a 32-bit DIB: the text is drawn white on black and
// its brightness becomes both colour and opacity (premultiplied), the panel 60% black.
void render(const std::vector<std::wstring> &lines, Panel &panel)
{
    int px = std::clamp(g_pic_h.load() / 52, 12, 30);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    int w = 0, line_h = px * 5 / 4, pad = px / 2;
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
        if (PdhAddEnglishCounterW(q, L"\\PhysicalDisk(_Total)\\% Idle Time", 0, &disk) != ERROR_SUCCESS) disk = nullptr;
        PdhCollectQueryData(q);
    }
    FILETIME idle0, kern0, user0, c0, e0, pk0, pu0;
    GetSystemTimes(&idle0, &kern0, &user0);
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &pk0, &pu0);
    IO_COUNTERS io0 = {};
    GetProcessIoCounters(GetCurrentProcess(), &io0);
    uint32_t frames0 = g_frames;
    ULONGLONG t0 = GetTickCount64();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::vector<uint8_t> buf;
    logf("[perfhud] on: GPU '%s' (%ls), CPU '%s', counters: GPU %s, disk %s", g_gpu.c_str(), g_luid, g_cpu.c_str(),
         gpu ? "yes" : "no", disk ? "yes" : "no");
    for (;;) {
        Sleep(1000);
        ULONGLONG t1 = GetTickCount64();
        double secs = std::max(0.001, (t1 - t0) / 1000.0);
        uint32_t frames1 = g_frames;
        double fps = (frames1 - frames0) / secs;
        // processor: the whole system from its idle time, and this program from its own times
        FILETIME idle1, kern1, user1, c1, e1, pk1, pu1;
        GetSystemTimes(&idle1, &kern1, &user1);
        GetProcessTimes(GetCurrentProcess(), &c1, &e1, &pk1, &pu1);
        double all = (double)(ft(kern1) - ft(kern0) + ft(user1) - ft(user0)), idle = (double)(ft(idle1) - ft(idle0));
        double cpu = all > 0 ? 100.0 * (all - idle) / all : 0;
        double mine = 100.0 * (ft(pk1) - ft(pk0) + ft(pu1) - ft(pu0)) / (secs * 1e7 * si.dwNumberOfProcessors);
        // graphics card: the 3D engines of this card, all programs (Windows' GPU Engine counters)
        double gpu_use = -1, disk_use = -1;
        if (q && PdhCollectQueryData(q) == ERROR_SUCCESS) {
            if (gpu) {
                DWORD size = 0, count = 0;
                if (PdhGetFormattedCounterArrayW(gpu, PDH_FMT_DOUBLE, &size, &count, nullptr) == PDH_MORE_DATA) {
                    buf.resize(size);
                    auto *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)buf.data();
                    if (PdhGetFormattedCounterArrayW(gpu, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
                        std::map<std::wstring, double> per_card;
                        for (DWORD i = 0; i < count; i++) {
                            const wchar_t *l = wcsstr(items[i].szName, L"luid_");
                            if (l && items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA)
                                per_card[std::wstring(l, std::min<size_t>(wcslen(l), 26))] += items[i].FmtValue.doubleValue;
                        }
                        auto it = per_card.find(g_luid);
                        if (it != per_card.end()) gpu_use = it->second;
                        else
                            for (auto &kv : per_card) gpu_use = std::max(gpu_use, kv.second);
                    }
                }
            }
            PDH_FMT_COUNTERVALUE v;
            if (disk && PdhGetFormattedCounterValue(disk, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS)
                disk_use = 100.0 - v.doubleValue;
        }
        MEMORYSTATUSEX ms = {sizeof ms};
        GlobalMemoryStatusEx(&ms);
        PROCESS_MEMORY_COUNTERS_EX pm = {sizeof pm};
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
        IO_COUNTERS io1 = {};
        GetProcessIoCounters(GetCurrentProcess(), &io1);
        double read_mb = (io1.ReadTransferCount - io0.ReadTransferCount) / secs / (1024.0 * 1024.0);

        const double gb = 1024.0 * 1024.0 * 1024.0;
        wchar_t l[8][256];
        swprintf(l[0], 256, L"FPS   %.0f", fps);
        if (fps > 0.0) swprintf(l[1], 256, L"Frame  %5.1f ms", 1000.0 / fps);
        else swprintf(l[1], 256, L"Frame       -");
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
        if (input_count) swprintf(l[2], 256, L"In %5.1f/%5.1f ms", in_avg, in_max);
        else swprintf(l[2], 256, L"In     -/    - ms");
        if (in_last) swprintf(l[3], 256, L"Poll %5.1f Frm %5.1f", in_last, frame_last);
        else swprintf(l[3], 256, L"Poll     - Frm %5.1f", frame_last);
        if (gpu_use >= 0) swprintf(l[4], 256, L"GPU   %3.0f%%   %hs", std::min(100.0, gpu_use), g_gpu.c_str());
        else swprintf(l[4], 256, L"GPU     -    %hs", g_gpu.c_str());
        swprintf(l[5], 256, L"CPU   %3.0f%%   (game %.0f%%)   %hs", cpu, mine, g_cpu.c_str());
        swprintf(l[6], 256, L"RAM   %3lu%%   %.1f / %.1f GB   (game %.2f GB)", ms.dwMemoryLoad,
                 (ms.ullTotalPhys - ms.ullAvailPhys) / gb, ms.ullTotalPhys / gb, pm.PrivateUsage / gb);
        if (disk_use >= 0) swprintf(l[7], 256, L"Disk  %3.0f%%   (game reads %.1f MB/s)", std::clamp(disk_use, 0.0, 100.0), read_mb);
        else swprintf(l[7], 256, L"Disk    -    (game reads %.1f MB/s)", read_mb);
        render({l[0], l[1], l[2], l[3], l[4], l[5], l[6], l[7]}, g_hud);
        static int updates;
        static ULONGLONG busy;
        busy += GetTickCount64() - t1;
        if (++updates == 20) logf("[perfhud] the overlay thread: %.1f ms per update (read and draw, once a second)", busy / 20.0);

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
    if (g_cfg.perfhud) g_frames++;
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
    if (g_note_until.load(std::memory_order_relaxed)) { // a message (online play): drawn here, on the render thread
        std::wstring text;
        {
            std::lock_guard<std::mutex> lk(g_note_lock);
            if (g_note_new) text = g_note_text;
            g_note_new = false;
        }
        g_pic_h = pic.bottom - pic.top;
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
    g_pic_h = pic.bottom - pic.top;
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
        d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
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
    float m = (float)std::max(4L, (pic.bottom - pic.top) / 60);
    float x0 = pic.right - m - panel.tex.w - 0.5f, y0 = (bottom ? pic.bottom - m - panel.tex.h : pic.top + m) - 0.5f;
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
