// Xbox textures -> host textures. Swizzled (Morton order) and linear layouts are decoded on the CPU into
// A8R8G8B8, DXT data is uploaded as is. Host copies are cached per resource header and re-uploaded when
// the header changes, the game locks the texture, or a sampled hash of the texel data changes (checked at the
// first use in a frame, every 16th frame for textures that have not changed for a while).
// Each is a D3DPOOL_SYSTEMMEM texture the Xbox data is decoded into and a D3DPOOL_DEFAULT copy the draws sample,
// brought up to date by UpdateTexture. A Direct3D 9Ex device has no D3DPOOL_MANAGED; the system copy also makes the
// DEFAULT one again after a Reset drops it (texture_drop_device_copies). Uploads cost the same as from MANAGED
// textures (ex9test: Iris Xe and RTX 3060, 200 textures of five formats and sizes, new and rewritten, 2026-10-24).
#include "d3d/device.h"
#include "d3d/dispatch.h"
#include "d3d/host.h"
#include "d3d/state.h"
#include "perf.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace xd3d {
namespace {

struct Entry {
    IDirect3DBaseTexture9 *tex = nullptr; // D3DPOOL_DEFAULT: what the draws sample
    IDirect3DBaseTexture9 *sys = nullptr; // D3DPOOL_SYSTEMMEM: the decoded Xbox data, copied into tex
    uint32_t data = 0, format = 0, size = 0;
    const void *palette = nullptr;
    uint32_t hash = 0;
    bool dirty = false;
    uint32_t checked_frame = ~0u; // the frame the contents were last checked (hashing on every bind cost ~8 ms a frame)
    uint32_t stable = 0;          // consecutive checks without a change
    uint32_t hashed_frame = 0;    // the frame the contents were last hashed (write-watch comparisons)
};

// A texture whose contents did not change for this many checks is no longer hashed every frame (busy
// scenes bind hundreds of textures a frame; hashing each once a frame still cost ~3 ms). Textures that
// change (video frames, render-to-memory) stay at one check a frame; a Lock marks a texture dirty at once.
constexpr uint32_t kStableChecks = 30;

std::unordered_map<const Resource *, Entry> g_cache;

// Pages of the 64 MB contiguous block written since the last frame (GetWriteWatch over MEM_WRITE_WATCH memory,
// kernel/k_memory.cpp). A stable texture skips its content hash for up to 16 frames; Ninja Gaiden Black rewrites its
// save thumbnails in place, so the old picture stayed for a while ("ghosting" on the load page, r072/r073). Now a
// texture whose pages were written since its last hash is checked at once; the 16-frame check stays as a fallback.
constexpr uint32_t kContigBase = 0x80000000u, kContigSize = 64u << 20, kPage = 4096;
uint32_t g_page_written[kContigSize / kPage]; // frame + 1 when the page was last reported written
uint32_t g_ww_frame = ~0u;
bool g_ww_ok = true;
void *g_ww_addrs[kContigSize / kPage];

void write_watch_update(uint32_t frame)
{
    if (!g_ww_ok || frame == g_ww_frame) return;
    g_ww_frame = frame;
    ULONG_PTR count = kContigSize / kPage;
    DWORD gran = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)kContigBase, kContigSize, g_ww_addrs, &count, &gran) != 0) {
        g_ww_ok = false;
        logf("[tex] write watch not available (error %lu): textures fall back to periodic checks", GetLastError());
        return;
    }
    for (ULONG_PTR i = 0; i < count; i++) g_page_written[((uintptr_t)g_ww_addrs[i] - kContigBase) / kPage] = frame + 1;
}

} // namespace

// Writes the write watch does not see: file reads land in guest memory from the host kernel (NtReadFile), possibly
// after the call returns (overlapped). The pages count as written for the next 30 frames.
void texture_note_write(const void *p, uint32_t len)
{
    uintptr_t a = (uintptr_t)p;
    if (a < kContigBase || a >= kContigBase + kContigSize || !len) return;
    uint32_t until = g_frame_count.load(std::memory_order_relaxed) + 31;
    uintptr_t end = a + len - 1 < kContigBase + kContigSize ? a + len - 1 : kContigBase + kContigSize - 1;
    for (uintptr_t q = (a - kContigBase) / kPage; q <= (end - kContigBase) / kPage; q++) g_page_written[q] = until;
}

namespace {

bool written_since(uint32_t data_phys, uint32_t bytes, uint32_t since_frame)
{
    if (!g_ww_ok) return false;
    uint32_t lo = (data_phys & 0x03FFFFFF) / kPage, hi = ((data_phys & 0x03FFFFFF) + (bytes ? bytes - 1 : 0)) / kPage;
    for (uint32_t p = lo; p <= hi && p < kContigSize / kPage; p++)
        if (g_page_written[p] > since_frame) return true;
    return false;
}
CRITICAL_SECTION g_cs;
struct Init {
    Init() { InitializeCriticalSection(&g_cs); }
} g_init;

uint32_t fmt_of(const PixelContainer *pc) { return (pc->Format >> FMT_FORMAT_SHIFT) & 0xFF; }

uint32_t sample_hash(const uint8_t *p, uint32_t bytes)
{
    uint32_t h = 2166136261u;
    uint32_t n = bytes / 4;
    const uint32_t *d = (const uint32_t *)p;
    uint32_t step = n > 8192 ? n / 4096 : 1;
    for (uint32_t i = 0; i < n; i += step) h = (h ^ d[i]) * 16777619u;
    return h;
}

// Morton-order offset tables for one mip level.
struct Swizzle {
    uint32_t mx = 0, my = 0;
    Swizzle(uint32_t w, uint32_t h)
    {
        uint32_t bit = 1;
        for (uint32_t i = 1; i < w || i < h; i <<= 1) {
            if (i < w) { mx |= bit; bit <<= 1; }
            if (i < h) { my |= bit; bit <<= 1; }
        }
    }
    static uint32_t deposit(uint32_t v, uint32_t mask)
    {
        uint32_t r = 0;
        for (uint32_t b = 1; mask; b <<= 1) {
            uint32_t low = mask & (~mask + 1);
            if (v & b) r |= low;
            mask &= mask - 1;
        }
        return r;
    }
};

uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }
uint32_t expand4(uint32_t v) { return v * 17; }

// Decode one texel of an uncompressed Xbox format into A8R8G8B8.
uint32_t decode(uint32_t fmt, const uint8_t *p, const uint32_t *pal)
{
    uint32_t v16 = p[0] | (p[1] << 8);
    switch (fmt) {
    case XFMT_A8R8G8B8: case XFMT_LIN_A8R8G8B8: return *(const uint32_t *)p;
    case XFMT_X8R8G8B8: case XFMT_LIN_X8R8G8B8: return *(const uint32_t *)p | 0xFF000000;
    case XFMT_R5G6B5: case XFMT_LIN_R5G6B5:
        return 0xFF000000 | expand5(v16 >> 11) << 16 | expand6((v16 >> 5) & 63) << 8 | expand5(v16 & 31);
    case XFMT_A1R5G5B5: case XFMT_LIN_A1R5G5B5:
        return ((v16 & 0x8000) ? 0xFF000000 : 0) | expand5((v16 >> 10) & 31) << 16 | expand5((v16 >> 5) & 31) << 8 | expand5(v16 & 31);
    case XFMT_X1R5G5B5: case XFMT_LIN_X1R5G5B5:
        return 0xFF000000 | expand5((v16 >> 10) & 31) << 16 | expand5((v16 >> 5) & 31) << 8 | expand5(v16 & 31);
    case XFMT_A4R4G4B4: case XFMT_LIN_A4R4G4B4:
        return expand4(v16 >> 12) << 24 | expand4((v16 >> 8) & 15) << 16 | expand4((v16 >> 4) & 15) << 8 | expand4(v16 & 15);
    case XFMT_R6G5B5: case XFMT_LIN_R6G5B5:
        return 0xFF000000 | expand6(v16 >> 10) << 16 | expand5((v16 >> 5) & 31) << 8 | expand5(v16 & 31);
    case XFMT_L8: case XFMT_LIN_L8: return 0xFF000000 | p[0] * 0x010101u;
    case XFMT_AL8: case XFMT_LIN_AL8: return p[0] * 0x01010101u;
    case XFMT_A8: case XFMT_LIN_A8: return (uint32_t)p[0] << 24 | 0xFFFFFF;
    case XFMT_A8L8: case XFMT_LIN_A8L8: return (uint32_t)p[1] << 24 | p[0] * 0x010101u;
    case XFMT_L16: case XFMT_LIN_L16: return 0xFF000000 | p[1] * 0x010101u;
    case XFMT_P8: return pal ? pal[p[0]] : 0xFF000000 | p[0] * 0x010101u;
    case XFMT_R8B8: case XFMT_LIN_R8B8: return 0xFF000000 | (uint32_t)p[1] << 16 | p[0];
    case XFMT_G8B8: case XFMT_LIN_G8B8: return 0xFF000000 | (uint32_t)p[1] << 8 | p[0];
    case XFMT_V8U8: return 0xFF000000 | ((p[0] ^ 0x80u) << 16) | ((p[1] ^ 0x80u) << 8);
    case XFMT_A8B8G8R8: case XFMT_LIN_A8B8G8R8: { uint32_t v = *(const uint32_t *)p; return (v & 0xFF00FF00) | (v & 0xFF) << 16 | (v >> 16 & 0xFF); }
    case XFMT_B8G8R8A8: case XFMT_LIN_B8G8R8A8: { uint32_t v = *(const uint32_t *)p; return (v << 24) | (v >> 8); }
    case XFMT_R8G8B8A8: case XFMT_LIN_R8G8B8A8: { uint32_t v = *(const uint32_t *)p; return (v << 24) | (v >> 24 & 0xFF) << 16 | (v >> 16 & 0xFF) << 8 | (v >> 8 & 0xFF); }
    case XFMT_D24S8: case XFMT_LIN_D24S8: case XFMT_F24S8: case XFMT_LIN_F24S8: { uint32_t d = *(const uint32_t *)p >> 24; return 0xFF000000 | d * 0x010101u; }
    case XFMT_D16: case XFMT_LIN_D16: case XFMT_F16: case XFMT_LIN_F16: return 0xFF000000 | p[1] * 0x010101u;
    default: return 0xFFFF00FF; // magenta: format not handled
    }
}

uint32_t yuv_to_rgb(int y, int u, int v)
{
    int c = y - 16, d = u - 128, e = v - 128;
    auto clamp = [](int x) { return x < 0 ? 0 : x > 255 ? 255 : x; };
    int r = clamp((298 * c + 409 * e + 128) >> 8), g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8),
        b = clamp((298 * c + 516 * d + 128) >> 8);
    return 0xFF000000 | r << 16 | g << 8 | b;
}

// Write level `level` of `pc` (from Xbox memory) into a locked host rect.
void upload_level(const PixelContainer *pc, uint32_t face, uint32_t level, uint8_t *dst, uint32_t dst_pitch, const uint32_t *pal)
{
    uint32_t fmt = fmt_of(pc);
    const FormatInfo &fi = format_info(fmt);
    uint32_t pitch, w, h;
    uint32_t off = texture_level_offset(pc, face, level, &pitch, &w, &h);
    const uint8_t *src = (const uint8_t *)phys_to_virt(pc->Data) + off;
    if (fi.compressed) {
        uint32_t rows = (h + 3) / 4;
        for (uint32_t y = 0; y < rows; y++) memcpy(dst + y * dst_pitch, src + y * pitch, pitch < dst_pitch ? pitch : dst_pitch);
        return;
    }
    uint32_t bpp = fi.bpp / 8;
    if (fmt == XFMT_YUY2 || fmt == XFMT_UYVY) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *s = src + y * pitch;
            uint32_t *d = (uint32_t *)(dst + y * dst_pitch);
            for (uint32_t x = 0; x + 1 < w; x += 2, s += 4) {
                int y0, y1, u, v;
                if (fmt == XFMT_YUY2) { y0 = s[0]; u = s[1]; y1 = s[2]; v = s[3]; }
                else { u = s[0]; y0 = s[1]; v = s[2]; y1 = s[3]; }
                d[x] = yuv_to_rgb(y0, u, v);
                d[x + 1] = yuv_to_rgb(y1, u, v);
            }
        }
        return;
    }
    if (fi.linear) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *s = src + y * pitch;
            uint32_t *d = (uint32_t *)(dst + y * dst_pitch);
            for (uint32_t x = 0; x < w; x++) d[x] = decode(fmt, s + x * bpp, pal);
        }
        return;
    }
    Swizzle sw(w, h);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t oy = Swizzle::deposit(y, sw.my);
        uint32_t *d = (uint32_t *)(dst + y * dst_pitch);
        uint32_t ox = 0;
        for (uint32_t x = 0; x < w; x++) {
            d[x] = decode(fmt, src + (ox | oy) * bpp, pal);
            ox = (ox - sw.mx) & sw.mx; // next x in Morton order
        }
    }
}

D3DFORMAT host_format(uint32_t fmt)
{
    switch (fmt) {
    case XFMT_DXT1: return D3DFMT_DXT1;
    case XFMT_DXT3: return D3DFMT_DXT3;
    case XFMT_DXT5: return D3DFMT_DXT5;
    default: return D3DFMT_A8R8G8B8;
    }
}

uint32_t total_bytes(const PixelContainer *pc)
{
    uint32_t w, h, d, levels;
    pixel_container_dims(pc, &w, &h, &d, &levels);
    uint32_t pitch, lw, lh;
    bool cube = (pc->Format & FMT_CUBEMAP) != 0;
    uint32_t last = texture_level_offset(pc, cube ? 5 : 0, levels - 1, &pitch, &lw, &lh);
    const FormatInfo &fi = format_info(fmt_of(pc));
    return last + pitch * (fi.compressed ? (lh + 3) / 4 : lh);
}

// Decode the Xbox data of `r` into the system copy `base` (every face and level). Locking whole levels marks them
// dirty, so the next UpdateTexture copies them.
void fill(IDirect3DBaseTexture9 *base, Resource *r, Palette *pal)
{
    auto *pc = (PixelContainer *)r;
    const uint32_t *palette = pal ? (const uint32_t *)phys_to_virt(pal->Data) : nullptr;
    if (base->GetType() == D3DRTYPE_CUBETEXTURE) {
        auto *t = (IDirect3DCubeTexture9 *)base;
        for (uint32_t f = 0; f < 6; f++)
            for (uint32_t l = 0; l < t->GetLevelCount(); l++) {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(t->LockRect((D3DCUBEMAP_FACES)f, l, &lr, nullptr, 0))) {
                    upload_level(pc, f, l, (uint8_t *)lr.pBits, lr.Pitch, palette);
                    t->UnlockRect((D3DCUBEMAP_FACES)f, l);
                }
            }
        return;
    }
    auto *t = (IDirect3DTexture9 *)base;
    for (uint32_t l = 0; l < t->GetLevelCount(); l++) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(t->LockRect(l, &lr, nullptr, 0))) {
            upload_level(pc, 0, l, (uint8_t *)lr.pBits, lr.Pitch, palette);
            t->UnlockRect(l);
        }
    }
}

// An empty system copy of the shape of `r` (cube or 2D; volume textures are not made)
IDirect3DBaseTexture9 *new_system_copy(Resource *r)
{
    auto *pc = (PixelContainer *)r;
    uint32_t fmt = fmt_of(pc);
    uint32_t w, h, d, levels;
    pixel_container_dims(pc, &w, &h, &d, &levels);
    if (levels == 0) levels = 1;
    uint32_t dim = (pc->Format >> FMT_DIMENSION_SHIFT) & 0xF;
    if (dim == 3) {
        KNOTE_ONCE("volume textures are not uploaded yet (%ux%ux%u fmt %s)", w, h, d, format_info(fmt).name);
        return nullptr;
    }
    D3DFORMAT hf = host_format(fmt);
    IDirect3DDevice9 *dev = host3d::dev();
    if (pc->Format & FMT_CUBEMAP) {
        IDirect3DCubeTexture9 *t = nullptr;
        if (FAILED(dev->CreateCubeTexture(w, levels, 0, hf, D3DPOOL_SYSTEMMEM, &t, nullptr))) return nullptr;
        return t;
    }
    IDirect3DTexture9 *t = nullptr;
    if (FAILED(dev->CreateTexture(w, h, levels, 0, hf, D3DPOOL_SYSTEMMEM, &t, nullptr))) {
        logf("[tex] CreateTexture %ux%u x%u %s failed", w, h, levels, format_info(fmt).name);
        return nullptr;
    }
    return t;
}

// ---- the texture pool ----
// A new texture made in the middle of a frame waited for the driver: on the RTX 3060 an area's first burst of new
// textures (109 in two frames of the attract demo, nearly all DXT) spent 34-40 ms in CreateTexture, a few calls taking
// 1-10 ms whatever their size (an 8x8 as long as a 512x512), while the same calls in a quiet loop take 12-50 us: the
// driver syncs with its own thread and the work it has queued. Growing the driver's heaps ahead did nothing (--tex-warm
// at 32 and 128 MB, 2026-11-14). The game never gives a texture back (none in 420 s: its headers live in the area data it
// loads), so nothing can be reused either. So the pairs (system + DEFAULT copy) are made ahead, by shape, when the
// driver is quiet: at device creation, and later in the frame pacing wait (texture_pool_refill). A pool filled at the
// start took all 109 (texture time 34-40 -> 1.5-3 ms, that frame 74 -> 53 ms).
// What each shape keeps: the most of it one burst made -- textures less than kQuietFrames apart, the gap the refill
// waits for, so a burst is what the pool must hold without being filled in between (counted over 3 frames instead,
// 26% of the textures in a 150 s run found none) -- from the seed shipped beside the program (texture_pool.txt,
// tools/make_texture_seed.py) and what this player's runs needed beyond it (cache\texture_pool.txt).
struct Shape {
    uint32_t w, h, levels;
    D3DFORMAT f;
};
uint64_t shape_key(const Shape &s) { return (uint64_t)s.w | (uint64_t)s.h << 14 | (uint64_t)s.levels << 28 | (uint64_t)(uint32_t)s.f << 32; }
Shape shape_of(uint64_t k) { return {(uint32_t)(k & 0x3FFF), (uint32_t)(k >> 14 & 0x3FFF), (uint32_t)(k >> 28 & 0xF), (D3DFORMAT)(uint32_t)(k >> 32)}; }
uint32_t shape_bytes(const Shape &s) // one copy
{
    uint32_t b = 0;
    for (uint32_t i = 0; i < s.levels; i++) {
        uint32_t w = std::max(1u, s.w >> i), h = std::max(1u, s.h >> i);
        b += s.f == D3DFMT_A8R8G8B8 ? w * h * 4 : ((w + 3) / 4) * ((h + 3) / 4) * (s.f == D3DFMT_DXT1 ? 8 : 16);
    }
    return b;
}
// Not kept: a texture over 512 KB (the screen-sized copies and movie frames, rare and big), shapes past 24 MB in all
constexpr uint32_t kPoolMaxTexture = 512u << 10, kPoolMaxBytes = 24u << 20;
constexpr uint32_t kQuietFrames = 30; // no texture made for this long: a burst is over, the driver not busy (refill)
struct PoolSlot {
    uint32_t target = 0;
    std::vector<std::pair<IDirect3DTexture9 *, IDirect3DTexture9 *>> items;
};
std::unordered_map<uint64_t, PoolSlot> g_pool;
uint64_t g_pool_bytes, g_pool_target_bytes; // one copy: what the pool holds, what its targets add up to
bool g_pool_on;                             // started (texture_pool_start) on the current device
uint32_t g_last_made = 0;                   // the frame a texture was last made (pooled or not)
std::unordered_map<uint64_t, uint32_t> g_burst; // the burst being counted: each shape's count
bool g_learned_dirty;
ULONGLONG g_learned_saved;
uint32_t g_pool_hits, g_pool_misses, g_pool_refilled, g_pool_slow, g_pool_raised; // since the last report

// A 2D texture's shape when the pool keeps that kind, else false
bool pool_shape(const PixelContainer *pc, Shape *s)
{
    uint32_t w, h, d, levels;
    pixel_container_dims(pc, &w, &h, &d, &levels);
    if ((pc->Format & FMT_CUBEMAP) || ((pc->Format >> FMT_DIMENSION_SHIFT) & 0xF) == 3 || w > 0x3FFF || h > 0x3FFF) return false;
    *s = {w, h, levels ? std::min(levels, 15u) : 1, host_format(fmt_of(pc))};
    return shape_bytes(*s) <= kPoolMaxTexture;
}

// Raise a shape's target (kept within the budget); true when it changed
bool pool_raise(uint64_t key, uint32_t want)
{
    PoolSlot &slot = g_pool[key];
    if (want <= slot.target) return false;
    uint32_t b = shape_bytes(shape_of(key));
    uint32_t room = (uint32_t)((kPoolMaxBytes - std::min<uint64_t>(g_pool_target_bytes, kPoolMaxBytes)) / b);
    uint32_t add = std::min(want - slot.target, room);
    if (!add) return false;
    slot.target += add;
    g_pool_target_bytes += (uint64_t)add * b;
    return true;
}

// A new texture: counted in the current burst (the target learns from it), then a pooled pair of its shape if any
bool pool_take(Resource *r, uint32_t frame, IDirect3DBaseTexture9 **sys, IDirect3DBaseTexture9 **tex)
{
    if (frame - g_last_made >= kQuietFrames) g_burst.clear(); // a new burst
    g_last_made = frame;
    Shape s;
    if (!g_pool_on || !pool_shape((const PixelContainer *)r, &s)) return false;
    uint64_t key = shape_key(s);
    if (pool_raise(key, ++g_burst[key])) {
        g_learned_dirty = true;
        g_pool_raised++;
    }
    PoolSlot &slot = g_pool[key];
    if (slot.items.empty()) {
        g_pool_misses++;
        return false;
    }
    *sys = slot.items.back().first;
    *tex = slot.items.back().second;
    slot.items.pop_back();
    g_pool_bytes -= shape_bytes(s);
    g_pool_hits++;
    return true;
}

// One pair of a shape into the pool
bool pool_make(IDirect3DDevice9 *dev, uint64_t key, PoolSlot &slot)
{
    Shape s = shape_of(key);
    IDirect3DTexture9 *sys = nullptr, *tex = nullptr;
    if (FAILED(dev->CreateTexture(s.w, s.h, s.levels, 0, s.f, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return false;
    if (FAILED(dev->CreateTexture(s.w, s.h, s.levels, 0, s.f, D3DPOOL_DEFAULT, &tex, nullptr))) {
        sys->Release();
        return false;
    }
    slot.items.push_back({sys, tex});
    g_pool_bytes += shape_bytes(s);
    return true;
}

// The system copy `sys` into its DEFAULT copy `tex`, made when null (a new texture, or after a Reset). UpdateTexture
// copies only the regions locked since the last one, so a new DEFAULT copy has the whole system copy marked first.
int64_t g_push_create_ticks; // --hitch-detail: the last push's DEFAULT CreateTexture time
std::mutex g_push_frame_lock;
std::unordered_map<IDirect3DBaseTexture9 *, uint32_t> g_push_frame; // --hitch-detail: each DEFAULT texture's last upload frame
IDirect3DBaseTexture9 *push(IDirect3DBaseTexture9 *sys, IDirect3DBaseTexture9 *tex)
{
    IDirect3DDevice9 *dev = host3d::dev();
    int64_t c0 = perf::g_detail && !tex ? perf::det_now() : 0;
    g_push_create_ticks = 0;
    if (!tex) {
        if (sys->GetType() == D3DRTYPE_CUBETEXTURE) {
            auto *s = (IDirect3DCubeTexture9 *)sys;
            D3DSURFACE_DESC desc;
            IDirect3DCubeTexture9 *t = nullptr;
            if (FAILED(s->GetLevelDesc(0, &desc)) ||
                FAILED(dev->CreateCubeTexture(desc.Width, s->GetLevelCount(), 0, desc.Format, D3DPOOL_DEFAULT, &t, nullptr)))
                return nullptr;
            for (uint32_t f = 0; f < 6; f++) s->AddDirtyRect((D3DCUBEMAP_FACES)f, nullptr);
            tex = t;
        } else {
            auto *s = (IDirect3DTexture9 *)sys;
            D3DSURFACE_DESC desc;
            IDirect3DTexture9 *t = nullptr;
            if (FAILED(s->GetLevelDesc(0, &desc)) ||
                FAILED(dev->CreateTexture(desc.Width, desc.Height, s->GetLevelCount(), 0, desc.Format, D3DPOOL_DEFAULT, &t, nullptr))) {
                logf("[tex] DEFAULT copy %ux%u x%lu failed", desc.Width, desc.Height, s->GetLevelCount());
                return nullptr;
            }
            s->AddDirtyRect(nullptr);
            tex = t;
        }
    }
    if (c0) g_push_create_ticks = perf::det_now() - c0;
    dev->UpdateTexture(sys, tex);
    if (perf::g_detail) {
        std::lock_guard<std::mutex> g(g_push_frame_lock);
        g_push_frame[tex] = g_frame_count.load(std::memory_order_relaxed);
    }
    return tex;
}

} // namespace

// --hitch-detail: the frame a host texture was last uploaded at (push), or ~0u when not known
uint32_t texture_push_frame(IDirect3DBaseTexture9 *t)
{
    std::lock_guard<std::mutex> g(g_push_frame_lock);
    auto it = g_push_frame.find(t);
    return it == g_push_frame.end() ? ~0u : it->second;
}

void pixel_container_dims(const PixelContainer *pc, uint32_t *w, uint32_t *h, uint32_t *d, uint32_t *levels)
{
    if (pc->Size) {
        *w = (pc->Size & SIZE_WIDTH_MASK) + 1;
        *h = ((pc->Size >> SIZE_HEIGHT_SHIFT) & 0xFFF) + 1;
        *d = 1;
        *levels = 1;
    } else {
        *w = 1u << ((pc->Format >> FMT_USIZE_SHIFT) & 0xF);
        *h = 1u << ((pc->Format >> FMT_VSIZE_SHIFT) & 0xF);
        *d = 1u << ((pc->Format >> FMT_PSIZE_SHIFT) & 0xF);
        *levels = (pc->Format >> FMT_MIPMAP_SHIFT) & 0xF;
        if (!*levels) *levels = 1;
    }
}

uint32_t texture_level_offset(const PixelContainer *pc, uint32_t face, uint32_t level, uint32_t *pitch, uint32_t *w, uint32_t *h)
{
    uint32_t tw, th, td, levels;
    pixel_container_dims(pc, &tw, &th, &td, &levels);
    const FormatInfo &fi = format_info(fmt_of(pc));
    if (pc->Size) { // linear: one level
        *pitch = (((pc->Size >> SIZE_PITCH_SHIFT) & 0xFF) + 1) * 64;
        *w = tw;
        *h = th;
        return 0;
    }
    auto level_bytes = [&](uint32_t l, uint32_t *p, uint32_t *lw, uint32_t *lh) {
        uint32_t x = tw >> l, y = th >> l;
        if (!x) x = 1;
        if (!y) y = 1;
        *lw = x;
        *lh = y;
        if (fi.compressed) {
            uint32_t bw = (x + 3) / 4, bh = (y + 3) / 4;
            *p = bw * fi.bpp * 2;
            return *p * bh;
        }
        *p = x * fi.bpp / 8;
        return *p * y;
    };
    uint32_t off = 0, p, lw, lh;
    if (face) {
        uint32_t face_bytes = 0;
        for (uint32_t l = 0; l < levels; l++) face_bytes += level_bytes(l, &p, &lw, &lh);
        face_bytes = (face_bytes + 127) & ~127u; // cube faces are 128-byte aligned
        off = face * face_bytes;
    }
    for (uint32_t l = 0; l < level; l++) off += level_bytes(l, &p, &lw, &lh);
    level_bytes(level, pitch, w, h);
    return off;
}

IDirect3DBaseTexture9 *host_texture(Resource *r, Palette *pal)
{
    if (!r || !r->Data) return nullptr;
    perf::Scope perf_scope(perf::kTexture);
    auto *pc = (PixelContainer *)r;
    EnterCriticalSection(&g_cs);
    Entry &e = g_cache[r];
    if (e.sys && !e.tex) e.tex = push(e.sys, nullptr); // dropped by a Reset (texture_drop_device_copies)
    uint32_t frame = g_frame_count.load(std::memory_order_relaxed);
    write_watch_update(frame);
    // Texture data is always read through the write-watched contiguous block (phys_to_virt), so with the write watch
    // working a stable texture is re-hashed only when its pages were written; the 16-frame periodic check was ~11% of
    // the frame time in chapter 1 (r093 perf). cfg:texperiodic:1 brings the periodic check back.
    bool periodic_due = (!g_ww_ok || g_cfg.tex_periodic) && ((frame + ((uintptr_t)r >> 6)) & 15) == 0;
    static uint32_t n_skip, n_new, n_written, n_periodic, n_other, stat_frame;
    if (frame != stat_frame && frame % 600 == 0) {
        logf("[tex] last 600 frames: %u skipped, hashed %u young, %u written, %u periodic, %u other", n_skip, n_new, n_written,
             n_periodic, n_other);
        if (g_pool_hits || g_pool_misses || g_pool_refilled) texture_pool_report();
        n_skip = n_new = n_written = n_periodic = n_other = 0;
    }
    stat_frame = frame;
    bool skip = e.checked_frame == frame ||
                (!g_cfg.tex_every_frame && e.stable >= kStableChecks && !periodic_due &&
                 !(e.tex && written_since(pc->Data, total_bytes(pc), e.hashed_frame)) &&
                 // a P8 texture also changes when its palette is rewritten in place: the NV2A reads the palette from
                 // memory at every draw (docs/07 #15). The hash below covers the palette's 1024 bytes.
                 !(e.tex && pal && fmt_of(pc) == XFMT_P8 && written_since(pal->Data, 1024, e.hashed_frame)));
    // The palette matters for P8 textures only (as in the staleness test below, which is also the only place that
    // records it): comparing it for every format sent each non-P8 texture created under another palette through the
    // full hash on every bind, ~350 hashes a frame in chapter 1 (r120).
    if (skip && e.tex && !e.dirty && e.data == pc->Data && e.format == pc->Format && e.size == pc->Size &&
        (fmt_of(pc) != XFMT_P8 || e.palette == (pal ? phys_to_virt(pal->Data) : nullptr))) {
        IDirect3DBaseTexture9 *t = e.tex;
        n_skip++;
        LeaveCriticalSection(&g_cs);
        return t;
    }
    if (skip) n_other++; // skipped by the rules but the shape / palette / dirty flag changed
    else if (e.stable < kStableChecks) n_new++;
    else if (periodic_due) n_periodic++;
    else if (e.checked_frame != frame) n_written++;
    e.checked_frame = frame;
    e.hashed_frame = frame;
    uint32_t bytes = total_bytes(pc);
    uint32_t hash = sample_hash((const uint8_t *)phys_to_virt(pc->Data), bytes);
    const void *palptr = pal ? phys_to_virt(pal->Data) : nullptr;
    if (fmt_of(pc) == XFMT_P8 && pal) hash ^= sample_hash((const uint8_t *)palptr, 1024);
    bool same_shape = e.tex && e.data == pc->Data && e.format == pc->Format && e.size == pc->Size;
    bool stale = !same_shape || e.dirty || e.hash != hash || (fmt_of(pc) == XFMT_P8 && e.palette != palptr);
    e.stable = stale ? 0 : std::min<uint32_t>(e.stable + 1, kStableChecks);
    if (stale && same_shape) { // contents changed (video frames, render-to-memory): refill in place
        {
            perf::DetScope ds(perf::kDetTexFill);
            fill(e.sys, r, pal);
        }
        {
            perf::DetScope ds(perf::kDetTexPush);
            push(e.sys, e.tex);
        }
        perf::add_texture_upload(bytes);
        e.palette = palptr;
        e.hash = hash;
        e.dirty = false;
        stale = false;
    }
    if (stale) {
        const bool fresh = !e.sys && !e.tex; // a header seen for the first time (else reshaped, or after a Reset)
        if (e.tex) e.tex->Release();
        if (e.sys) e.sys->Release();
        e.tex = nullptr;
        int64_t t0 = perf::g_detail ? perf::det_now() : 0, t1 = 0, t2 = 0;
        IDirect3DBaseTexture9 *pooled = nullptr;
        {
            perf::DetScope ds(perf::kDetTexCreate);
            if (!pool_take(r, frame, &e.sys, &pooled)) e.sys = new_system_copy(r);
        }
        if (e.sys) {
            if (t0) t1 = perf::det_now();
            {
                perf::DetScope ds(perf::kDetTexFill);
                fill(e.sys, r, pal);
            }
            if (t0) t2 = perf::det_now();
            perf::DetScope ds(perf::kDetTexPush);
            e.tex = push(e.sys, pooled);
        }
        if (t0 && e.sys) { // --hitch-detail: each texture made, with its shape and where its time went
            int64_t t3 = perf::det_now();
            uint32_t w, h, d, l;
            pixel_container_dims(pc, &w, &h, &d, &l);
            D3DFORMAT hf = host_format(fmt_of(pc));
            uint32_t host_bytes = 0, faces = (pc->Format & FMT_CUBEMAP) ? 6 : 1;
            for (uint32_t i = 0; i < l; i++) {
                uint32_t lw = std::max(1u, w >> i), lh = std::max(1u, h >> i);
                host_bytes += hf == D3DFMT_A8R8G8B8 ? lw * lh * 4 : ((lw + 3) / 4) * ((lh + 3) / 4) * (hf == D3DFMT_DXT1 ? 8 : 16);
            }
            host_bytes *= faces;
            double us = 1000.0 * (double)perf::det_ms(1);
            logf("[tex] made %p frame %u: %ux%u x%u %s%s -> %s, Xbox %u KB, host %u KB, %s: system copy %.0f us, decode %.0f us, "
                 "DEFAULT copy %.0f us, UpdateTexture %.0f us", r, frame, w, h, l, format_info(fmt_of(pc)).name,
                 faces == 6 ? " cube" : "", hf == D3DFMT_A8R8G8B8 ? "A8R8G8B8" : hf == D3DFMT_DXT1 ? "DXT1" : hf == D3DFMT_DXT3 ? "DXT3" : "DXT5",
                 bytes >> 10, host_bytes >> 10, pooled ? "pooled" : fresh ? "new header" : "reshaped", (t1 - t0) * us, (t2 - t1) * us,
                 g_push_create_ticks * us, (t3 - t2 - g_push_create_ticks) * us);
        }
        perf::add_texture_upload(bytes);
        e.data = pc->Data;
        e.format = pc->Format;
        e.size = pc->Size;
        e.palette = palptr;
        e.hash = hash;
        e.dirty = false;
        static std::atomic<int> uploads{0};
        if (++uploads <= 40 || g_trace_d3d) {
            uint32_t w, h, d, l;
            pixel_container_dims(pc, &w, &h, &d, &l);
            logf("[tex] upload %p: %ux%u x%u %s%s, data 0x%08x", r, w, h, l, format_info(fmt_of(pc)).name,
                 (pc->Format & FMT_CUBEMAP) ? " cube" : "", pc->Data);
        }
    }
    IDirect3DBaseTexture9 *t = e.tex;
    LeaveCriticalSection(&g_cs);
    return t;
}

void texture_mark_dirty(const Resource *r)
{
    EnterCriticalSection(&g_cs);
    auto it = g_cache.find(r);
    if (it != g_cache.end()) it->second.dirty = true;
    LeaveCriticalSection(&g_cs);
}

void texture_forget(const Resource *r)
{
    EnterCriticalSection(&g_cs);
    auto it = g_cache.find(r);
    if (it != g_cache.end()) {
        if (it->second.tex) it->second.tex->Release();
        if (it->second.sys) it->second.sys->Release();
        g_cache.erase(it);
    }
    LeaveCriticalSection(&g_cs);
}

namespace {
// The pooled pairs released (a Reset or a new device); the targets stay, so the refill makes them again
void pool_release()
{
    for (auto &kv : g_pool) {
        for (auto &p : kv.second.items) {
            p.first->Release();
            p.second->Release();
        }
        kv.second.items.clear();
    }
    g_pool_bytes = 0;
}

// Targets from a file of lines "width height levels d3dformat count" (# comments)
unsigned pool_read_targets(const std::wstring &path)
{
    FILE *f = _wfopen(path.c_str(), L"r");
    if (!f) return 0;
    char line[160];
    unsigned shapes = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned w, h, l, fmt, n;
        if (line[0] == '#' || sscanf(line, "%u %u %u %u %u", &w, &h, &l, &fmt, &n) != 5) continue;
        Shape s = {w, h, l, (D3DFORMAT)fmt};
        if (!w || !h || !l || l > 15 || w > 0x3FFF || h > 0x3FFF || shape_bytes(s) > kPoolMaxTexture) continue;
        if (fmt != D3DFMT_A8R8G8B8 && fmt != D3DFMT_DXT1 && fmt != D3DFMT_DXT3 && fmt != D3DFMT_DXT5) continue;
        pool_raise(shape_key(s), std::min(n, 256u));
        shapes++;
    }
    fclose(f);
    return shapes;
}

std::wstring learned_path() { return g_cfg.port_dir + L"\\cache\\texture_pool.txt"; }

// The targets this player's runs raised past the seed, written off the render thread
void pool_save_learned()
{
    std::string text = "# texture pool targets learned in play (d3d/texture.cpp): width height levels d3dformat count\n";
    char line[96];
    for (auto &kv : g_pool) {
        if (!kv.second.target) continue;
        Shape s = shape_of(kv.first);
        snprintf(line, sizeof line, "%u %u %u %u %u\n", s.w, s.h, s.levels, (unsigned)s.f, kv.second.target);
        text += line;
    }
    std::wstring path = learned_path();
    std::thread([text, path] {
        CreateDirectoryW((g_cfg.port_dir + L"\\cache").c_str(), nullptr);
        std::wstring tmp = path + L".tmp";
        if (FILE *f = _wfopen(tmp.c_str(), L"wb")) {
            bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
            ok = fclose(f) == 0 && ok;
            if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
        }
    }).detach();
}
} // namespace

// At device creation: the targets (the seed beside the program, then what this player's runs learned) and the pool
// filled to them. The first start of a device reads the files; a new device keeps the targets.
void texture_pool_start(IDirect3DDevice9 *dev)
{
    if (!g_cfg.tex_pool) { // --no-tex-pool: g_pool_on stays false, so nothing is taken, learned or made again
        static bool noted;
        if (!noted) logf("[tex] pool: off (--no-tex-pool)");
        noted = true;
        return;
    }
    EnterCriticalSection(&g_cs);
    static bool read;
    unsigned seed = 0, learned = 0;
    if (!read) {
        read = true;
        seed = pool_read_targets(g_cfg.port_dir + L"\\texture_pool.txt");
        learned = pool_read_targets(learned_path());
    }
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    unsigned made = 0, failed = 0;
    for (auto &kv : g_pool)
        while (kv.second.items.size() < kv.second.target)
            if (pool_make(dev, kv.first, kv.second)) made++;
            else {
                failed++;
                break;
            }
    g_pool_on = true;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    logf("[tex] pool: %u shapes from the seed, %u learned; %u pairs made (%llu KB a copy) in %.0f ms%s", seed, learned, made,
         (unsigned long long)(g_pool_bytes >> 10), (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart,
         failed ? " (some could not be made)" : "");
    LeaveCriticalSection(&g_cs);
}

// In the frame pacing wait (pace_swap), with `deadline` the QueryPerformanceCounter of the vblank it waits for: pairs
// made again for what the bursts took, while the driver is quiet (no texture made for kQuietFrames) and at least 6 ms
// are left; at most 4 a frame, none once under 3 ms are left. A pair taking over 2 ms is counted (the report says).
void texture_pool_refill(IDirect3DDevice9 *dev, int64_t deadline)
{
    if (!g_pool_on) return;
    LARGE_INTEGER now, fq;
    QueryPerformanceFrequency(&fq);
    auto left_ms = [&] {
        QueryPerformanceCounter(&now);
        return (double)(deadline - now.QuadPart) * 1000.0 / (double)fq.QuadPart;
    };
    uint32_t frame = g_frame_count.load(std::memory_order_relaxed);
    if (frame - g_last_made < kQuietFrames || left_ms() < 6.0) return;
    EnterCriticalSection(&g_cs);
    int made = 0;
    for (auto &kv : g_pool) {
        while (kv.second.items.size() < kv.second.target && made < 4 && left_ms() >= 3.0) {
            int64_t t0 = now.QuadPart;
            if (!pool_make(dev, kv.first, kv.second)) break;
            QueryPerformanceCounter(&now);
            if ((double)(now.QuadPart - t0) * 1000.0 / (double)fq.QuadPart > 2.0) g_pool_slow++;
            made++;
            g_pool_refilled++;
        }
        if (made >= 4) break;
    }
    if (g_learned_dirty && GetTickCount64() - g_learned_saved > 10000) {
        g_learned_dirty = false;
        g_learned_saved = GetTickCount64();
        pool_save_learned();
    }
    LeaveCriticalSection(&g_cs);
}

void texture_pool_report()
{
    size_t held = 0, wanted = 0;
    for (auto &kv : g_pool) {
        held += kv.second.items.size();
        wanted += kv.second.target;
    }
    logf("[tex] pool: %u taken, %u not in it, %u made again (%u over 2 ms), %u targets raised; holds %zu of %zu (%llu KB a copy)",
         g_pool_hits, g_pool_misses, g_pool_refilled, g_pool_slow, g_pool_raised, held, wanted,
         (unsigned long long)(g_pool_bytes >> 10));
    g_pool_hits = g_pool_misses = g_pool_refilled = g_pool_slow = g_pool_raised = 0;
}

void texture_drop_device_copies()
{
    EnterCriticalSection(&g_cs);
    pool_release();
    for (auto &kv : g_cache)
        if (kv.second.tex) {
            kv.second.tex->Release();
            kv.second.tex = nullptr;
        }
    LeaveCriticalSection(&g_cs);
}

void texture_drop_all()
{
    EnterCriticalSection(&g_cs);
    pool_release();
    for (auto &kv : g_cache) {
        if (kv.second.tex) kv.second.tex->Release();
        if (kv.second.sys) kv.second.sys->Release();
        kv.second.tex = kv.second.sys = nullptr; // made again from Xbox memory at the next use (a null tex is stale)
    }
    LeaveCriticalSection(&g_cs);
}

IDirect3DBaseTexture9 *texture_system_copy(IDirect3DBaseTexture9 *tex)
{
    IDirect3DBaseTexture9 *sys = nullptr;
    EnterCriticalSection(&g_cs);
    for (auto &kv : g_cache)
        if (kv.second.tex == tex) sys = kv.second.sys;
    LeaveCriticalSection(&g_cs);
    return sys;
}

} // namespace xd3d
