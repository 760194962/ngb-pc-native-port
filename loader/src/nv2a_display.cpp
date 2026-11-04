// The NV2A's display side for programs that drive it themselves (nxdk: the Insignia Setup Assistant, SDL2 drawing into a
// framebuffer in contiguous memory). The games' Direct3D is HLE'd and never touches these registers; this is installed
// only for a program no build profile matches (main.cpp).
//
// The registers at 0xFD000000 (16 MB) are reserved inaccessible; accesses trap to mmio_decode.cpp and a small model
// (offsets from nxdk lib/hal/video.c): PCRTC_START (the scan-out address), the vertical-blank interrupt (PCRTC_INTR /
// PCRTC_INTR_EN, PMC_INTR_EN_0), the DAC palette (the gamma ramp: ignored, identity). AvSetDisplayMode hands over the
// framebuffer, its format and pitch. A 60 Hz thread copies the framebuffer to the window and raises the vertical blank:
// the program's service routine (KeConnectInterrupt, bus level 3) runs at its IRQL, as the NIC's does (net/nvnet.cpp).
#include "nv2a_display.h"

#include "common.h"
#include "d3d/draw.h"
#include "d3d/host.h"
#include "kernel/xboxkrnl.h"
#include "mmio_decode.h"
#include "xthread.h"

#include <d3d9.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t REG_BASE = 0xFD000000, REG_SIZE = 0x1000000;
constexpr uint32_t PMC_INTR_EN_0 = 0x000140, PCRTC_INTR = 0x600100, PCRTC_INTR_EN = 0x600140, PCRTC_START = 0x600800;
constexpr uint32_t DAC_WRITE_INDEX = 0x6813C8, DAC_DATA = 0x6813C9;

std::mutex g_m;
std::unordered_map<uint32_t, uint32_t> g_regs; // dword-aligned register values the program wrote
uint32_t g_intr, g_intr_en, g_pmc_en;
uint32_t g_fb, g_pitch, g_width, g_height, g_format; // physical address, bytes per line, pixels, AvSetDisplayMode format
XKINTERRUPT *g_irq;
bool g_installed, g_started;
std::atomic<uint64_t> g_unknown_reads;
IDirect3DSurface9 *g_surface; // the framebuffer's copy (offscreen plain, X8R8G8B8)
uint32_t g_surface_w, g_surface_h;

uint32_t rd(uint32_t off, int size)
{
    std::lock_guard<std::mutex> g(g_m);
    uint32_t a = off & ~3u, v;
    switch (a) {
    case PCRTC_INTR: v = g_intr; break;
    case PCRTC_INTR_EN: v = g_intr_en; break;
    case PMC_INTR_EN_0: v = g_pmc_en; break;
    case PCRTC_START: v = g_fb; break;
    default: {
        auto it = g_regs.find(a);
        v = it != g_regs.end() ? it->second : 0;
        if (it == g_regs.end() && g_unknown_reads++ < 32) logf("[nv2a] read of 0x%06x (never written): 0", off);
    }
    }
    v >>= (off & 3) * 8;
    return size == 1 ? v & 0xFF : size == 2 ? v & 0xFFFF : v;
}

void wr(uint32_t off, uint32_t v, int size)
{
    std::lock_guard<std::mutex> g(g_m);
    uint32_t a = off & ~3u;
    switch (a) {
    case PCRTC_INTR: g_intr &= ~v; return; // write 1 to clear
    case PCRTC_INTR_EN: g_intr_en = v; return;
    case PMC_INTR_EN_0: g_pmc_en = v; return;
    case PCRTC_START:
        if (v != g_fb) logf("[nv2a] scan-out address 0x%08x", v);
        g_fb = v;
        return;
    case DAC_WRITE_INDEX & ~3u: return; // the gamma ramp (palette index / data): identity, ignored
    default: {
        uint32_t cur = g_regs[a], shift = (off & 3) * 8, mask = (size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu) << shift;
        g_regs[a] = (cur & ~mask) | ((v << shift) & mask);
    }
    }
}

LONG CALLBACK trap(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t addr = (uint32_t)er->ExceptionInformation[1];
    if (addr - REG_BASE >= REG_SIZE) return EXCEPTION_CONTINUE_SEARCH;
    static const MmioOps ops = {rd, wr};
    if (mmio_decode(ep->ContextRecord, addr - REG_BASE, ops)) return EXCEPTION_CONTINUE_EXECUTION;
    const uint8_t *ip = (const uint8_t *)(uintptr_t)ep->ContextRecord->Eip;
    logf("[nv2a] register access at %08lx (address %08x) not decoded: %02x %02x %02x %02x %02x %02x", ep->ContextRecord->Eip, addr,
         ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
    return EXCEPTION_CONTINUE_SEARCH;
}

// The framebuffer to the window: copied into a surface, stretched into the picture's place, presented
void present()
{
    uint32_t fb, pitch, w, h;
    {
        std::lock_guard<std::mutex> g(g_m);
        fb = g_fb, pitch = g_pitch, w = g_width, h = g_height;
    }
    IDirect3DDevice9 *d = host3d::dev();
    if (!d || !fb || !w || !h || pitch < w * 4) return;
    if (!g_surface || g_surface_w != w || g_surface_h != h) {
        if (g_surface) g_surface->Release(), g_surface = nullptr;
        if (FAILED(d->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_surface, nullptr))) return;
        g_surface_w = w, g_surface_h = h;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(g_surface->LockRect(&lr, nullptr, 0))) return;
    const uint8_t *src = (const uint8_t *)(uintptr_t)(0x80000000u | (fb & 0x7FFFFFFFu)); // contiguous RAM (kernel/k_memory.cpp)
    for (uint32_t y = 0; y < h; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, src + y * pitch, w * 4);
    g_surface->UnlockRect();
    static uint32_t frames;
    if (!g_cfg.screenshot_dir.empty() && g_cfg.shot_every && ++frames % g_cfg.shot_every == 0) { // --screenshot
        std::vector<uint8_t> px((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                memcpy(&px[((size_t)y * w + x) * 4], src + y * pitch + x * 4, 3);
                px[((size_t)y * w + x) * 4 + 3] = 0xFF;
            }
        wchar_t name[64];
        swprintf(name, 64, L"\\fb_%05u.png", frames);
        xd3d::write_png((g_cfg.screenshot_dir + name).c_str(), w, h, px.data());
    }
    IDirect3DSurface9 *bb = nullptr;
    if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        RECT frame, pic;
        host3d::frame_layout(&frame, &pic);
        d->ColorFill(bb, &frame, 0);
        d->StretchRect(g_surface, nullptr, bb, &pic, D3DTEXF_LINEAR);
        bb->Release();
    }
    host3d::present();
}

void __stdcall vblank_thread(void *)
{
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -166667; // 16.7 ms
    SetWaitableTimer(timer, &due, 16, nullptr, nullptr, FALSE);
    for (;;) {
        WaitForSingleObject(timer, INFINITE);
        if (host3d::window_closed()) {
            request_exit(0, "window closed");
            return;
        }
        present();
        bool fire;
        {
            std::lock_guard<std::mutex> g(g_m);
            if (g_intr_en & 1) g_intr |= 1;
            fire = g_irq && (g_intr & g_intr_en & 1) && g_pmc_en;
        }
        if (fire) {
            uint8_t old = xthread_raise_irql((uint8_t)g_irq->Irql);
            ((BOOLEAN(__stdcall *)(void *, void *))g_irq->ServiceRoutine)(g_irq, g_irq->ServiceContext);
            xthread_lower_irql(old);
        }
    }
}

} // namespace

void nv2a_display_install()
{
    if (g_cfg.profile) return;
    void *p = VirtualAlloc((void *)(uintptr_t)REG_BASE, REG_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (p != (void *)(uintptr_t)REG_BASE) {
        logc("[nv2a] cannot reserve the display registers at 0x%08x (error %lu)", REG_BASE, GetLastError());
        return;
    }
    AddVectoredExceptionHandler(1, trap);
    g_installed = true;
    logf("[nv2a] display registers at 0x%08x trap to the display model (a program without a build profile)", REG_BASE);
}

void nv2a_display_mode(uint32_t mode, uint32_t format, uint32_t pitch, uint32_t framebuffer)
{
    if (!g_installed) return;
    uint32_t w = pitch / 4, h = w == 640 || w == 720 ? 480 : w == 1280 ? 720 : w == 1920 ? 1080 : w * 3 / 4;
    {
        std::lock_guard<std::mutex> g(g_m);
        g_fb = framebuffer, g_pitch = pitch, g_width = w, g_height = h, g_format = format;
    }
    logf("[nv2a] display mode 0x%08x, format 0x%x, pitch %u: %ux%u from 0x%08x", mode, format, pitch, w, h, framebuffer);
    if (g_started) return;
    g_started = true;
    if (!host3d::open_window(g_cfg.window_w, g_cfg.window_h, 4, 3) || !host3d::create_device()) {
        logc("[nv2a] no window / Direct3D device: the program's picture is not shown");
        return;
    }
    kthread_create(nullptr, nullptr, 0, 0, (uint32_t)(uintptr_t)&vblank_thread, 0, 0, false, "nv2a-vblank");
}

bool nv2a_connect_interrupt(void *kinterrupt)
{
    XKINTERRUPT *i = (XKINTERRUPT *)kinterrupt;
    if (!g_installed || i->BusInterruptLevel != 3) return false;
    std::lock_guard<std::mutex> g(g_m);
    g_irq = i;
    logf("[nv2a] vertical-blank interrupt connected (service routine 0x%08x)", (uint32_t)(uintptr_t)i->ServiceRoutine);
    return true;
}
