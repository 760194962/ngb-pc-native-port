// Ninja Gaiden (2004): an EXIT GAME row on the title menu that asks "Exit the game?" Ⓐ Yes Ⓑ No in the game's own green
// box (the one the Master Ninja Tournament uses) and leaves the program on Ⓐ.
//
// The title menu (title update 1.06.04 addresses; work/ng04, 2026-10-31):
// - FUN_0016FE30 builds the main list from the 6-entry table 0x40E668 {id, p1, p2, condition}: ids into 0xE7AC88, the
//   sprite numbers p2 into 0xE7C5C0, the count into [0xE7C221] (0x16FF80). The id array has room for six only (its 7th
//   byte, 0xE7AC8E, is another variable), so its three users read a copy here instead.
// - FUN_00171B90 (the list's handler, through the menu table at 0x40E5C0) selects: 0x171C44 takes the id of the cursor's
//   row into [esi+0x1C], then 0x171D9C calls FUN_00171E00, which draws the rows through FUN_001DBEB0: row i is sprite
//   0xC4 + p2, its glow (the selected row) 0xCE + p2.
// - The sprite bank of the title (ids 0xBD-0xD8) still has a seventh word, p2 = 6: "RETURN TO DEMO DISC" (sprite 0xCA,
//   256x32 P8) with its glow (0xD4, 256x32 A8), left over from the kiosk demo. The new row uses it, with "EXIT GAME" put
//   into those two textures: letters cut from the game's own words (E, I, T from RETURN TO DEMO DISC, X from XBOX LIVE,
//   GAME from LOAD GAME), so it is the menu's own type. The sources are checked by hash first; anything else: no row.
// - Pad input of the title menus goes through FUN_00170F80 (ecx = button, edx = held / pressed / repeat): it answers
//   nothing while the box is up. The pad record is 0xDFB5C0 + port * 0x78 ([0xE8ECAC] the port), +8 pressed this frame
//   (0x510 Ⓐ/START, 0x220 Ⓑ/BACK: Ninja Gaiden Black's own record).
// - The box: FUN_0021C020 (cdecl, a struct whose first u16 is a message number; it adds 0x40000) drawn every frame; the
//   text comes from FUN_001D5AC0 (eax = message id), answered here for our message. Sounds: FUN_001D31F0(id, 0x40).
#include "build_profile.h"
#include "common.h"

#include <windows.h>

#include <cstdint>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace xd3d { void texture_note_write(const void *p, uint32_t len); } // d3d/texture.cpp

namespace {

// One build's addresses (the title update and disc 1.02 differ only in where things are)
struct Addrs {
    uint32_t append;         // mov [count], cl after the build loop (6 bytes)
    uint32_t ids_write;      // mov [ecx+ids], al (disp at +2)
    uint32_t select;         // movzx edx, byte ptr [ecx+ids] (7 bytes); +7 the next instruction
    uint32_t ids_read2;      // movzx ecx, byte ptr [eax+ids] (disp at +3)
    uint32_t select_skip;    // call FUN_00171E00 (the rows' drawing), where a select ends
    uint32_t menu_draw;      // FUN_00171E00
    uint32_t pad_test;       // FUN_00170F80 (8 bytes: xor eax,eax; cmp ecx,0x2000)
    uint32_t text;           // FUN_001D5AC0 (6 bytes: push ebx; mov bl,[esp+8]; push edi)
    uint32_t box;            // FUN_0021C020
    uint32_t sound;          // FUN_001D31F0
    uint32_t sprite;         // FUN_0020A8C0 (eax = sprite id -> its record)
    uint32_t tex_table;      // 0x988458: {texture, palette} per texture number
    uint32_t ids;            // 0xE7AC88
    uint32_t p2;             // 0xE7C5C0
    uint32_t count;          // 0xE7C221
    uint32_t pad;            // 0xDFB5C0
    uint32_t port;           // 0xE8ECAC
    uint32_t text_on;        // 0x9A4B1C: the box writes its text
    uint32_t tex_count;      // the texture table's size
};

const Addrs kTu = {0x0016FF80, 0x0016FF5C, 0x00171C44, 0x00171D0F, 0x00171D9C, 0x00171E00, 0x00170F80, 0x001D5AC0,
                   0x0021C020, 0x001D31F0, 0x0020A8C0, 0x00988458, 0x00E7AC88, 0x00E7C5C0, 0x00E7C221, 0x00DFB5C0,
                   0x00E8ECAC, 0x009A4B1C, 0x4C5};
// Disc 1.02 (without the title update): the same code, found by its instruction shapes (2026-10-31)
const Addrs kDisc = {0x00161240, 0x0016121C, 0x00162F01, 0x00162FCC, 0x00163059, 0x001630C0, 0x00162240, 0x001C4850,
                     0x0020A380, 0x001C1F60, 0x001F8740, 0x0096B668, 0x00E5CB88, 0x00E5E4C0, 0x00E5E121, 0x00DDD4C0,
                     0x00E70BA6, 0x009875DC, 0x3DB};

const Addrs *g_a;

constexpr uint8_t kExitId = 9;        // the row's id (the game's own ids are 2-8)
constexpr uint8_t kExitSprite = 6;    // p2: sprites 0xCA (text) / 0xD4 (glow)
constexpr uint32_t kBoxMessage = 0x0004F0E1;
const char kBoxText[] = " Exit the game?\n\n                              @BA Yes    @BB No";

} // namespace

extern "C" {
uint8_t g_ng04x_ids[8];
volatile uint32_t g_ng04x_dialog; // 1 while the box is up
uint32_t g_ng04x_rows = 1;        // 0: the textures did not check out, no row
uint32_t g_ng04x_p2, g_ng04x_count;
uint32_t g_ng04x_append_resume, g_ng04x_select_resume, g_ng04x_select_skip, g_ng04x_menu_draw, g_ng04x_pad_resume,
    g_ng04x_text_resume, g_ng04x_sprite;
}

namespace {

uint16_t g_box[8] = {(uint16_t)(kBoxMessage & 0xFFFF)};
uint32_t g_box_frames;

// ---- the row's two textures ------------------------------------------------------------------------------------------

struct XTex { // the game's texture (a D3D pixel container)
    uint32_t common, data, lock, format, size;
};

uint8_t *va(uint32_t phys) { return (uint8_t *)(uintptr_t)(phys | 0x80000000u); }

uint32_t fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// Morton order of a swizzled texture: x and y bits interleaved from bit 0 while both have bits left
struct Swz {
    uint32_t mx = 0, my = 0;
    Swz(uint32_t w, uint32_t h)
    {
        for (uint32_t bit = 1; w > 1 || h > 1;) {
            if (w > 1) mx |= bit, bit <<= 1, w >>= 1;
            if (h > 1) my |= bit, bit <<= 1, h >>= 1;
        }
    }
    static uint32_t dep(uint32_t v, uint32_t m)
    {
        uint32_t r = 0;
        for (uint32_t i = 0, bit = 1; m >> i; i++)
            if ((m >> i) & 1) {
                if (v & bit) r |= 1u << i;
                bit <<= 1;
            }
        return r;
    }
    uint32_t at(uint32_t x, uint32_t y) const { return dep(x, mx) | dep(y, my); }
};

struct Src { // a texture (and palette) of a sprite
    XTex *tex = nullptr;
    const uint32_t *pal = nullptr;
};

Src sprite_src(uint32_t id)
{
    uint32_t rec = 0, fn = g_ng04x_sprite;
    __asm {
        mov eax, id
        call dword ptr [fn]
        mov rec, eax
    }
    Src s;
    if (!rec) return s;
    uint32_t n = *(const uint32_t *)(uintptr_t)(rec + 0x10);
    if (n >= g_a->tex_count) return s;
    const uint32_t *e = (const uint32_t *)(uintptr_t)(g_a->tex_table + n * 8);
    s.tex = (XTex *)(uintptr_t)e[0];
    if (e[1]) s.pal = (const uint32_t *)va(((const XTex *)(uintptr_t)e[1])->data);
    return s;
}

uint8_t g_text[256 * 32], g_glow[256 * 32]; // what the row's textures hold once made (swizzled)
bool g_made;

// A pixel of the game's menu words: its white (f) and the dark drop shadow under it (d); the rest is the red halo
struct Px {
    float f, d;
};
Px px_of(uint32_t c)
{
    float a = (c >> 24) / 255.0f, r = ((c >> 16) & 255) * a, g = ((c >> 8) & 255) * a;
    float h = (r - g) / 253.0f;
    if (h < 0) h = 0;
    float f = g / 255.0f, d = a - f - h;
    return {f, d > 0 ? d : 0};
}

// A separable Gaussian blur of a 256x32 map (edges clamped)
void blur(const float (*src)[256], float (*dst)[256], float sigma)
{
    int r = (int)(3 * sigma) + 1;
    float k[32], sum = 0;
    for (int i = -r; i <= r; i++) sum += k[i + r] = expf(-(float)(i * i) / (2 * sigma * sigma));
    for (int i = 0; i <= 2 * r; i++) k[i] /= sum;
    static float tmp[32][256];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 256; x++) {
            float v = 0;
            for (int i = -r; i <= r; i++) v += src[y][x + i < 0 ? 0 : x + i > 255 ? 255 : x + i] * k[i + r];
            tmp[y][x] = v;
        }
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 256; x++) {
            float v = 0;
            for (int i = -r; i <= r; i++) v += tmp[y + i < 0 ? 0 : y + i > 31 ? 31 : y + i][x] * k[i + r];
            dst[y][x] = v;
        }
}

// The row's textures from the game's letters. Returns 1 ready, 0 not loaded yet, -1 not the expected art.
// The letters' strokes (their white and dark drop shadow) are lifted from LOAD GAME / XBOX LIVE (E, X, GAME) and
// MASTER NINJA TOURNAMENT (I, T) and set side by side; the soft red halo around them and the glow are made again for
// the whole word (Gaussian blurs fitted to the game's own words: halo sigma 3 x 1.8, glow sigma 1.6 x 4), so no letter
// keeps a halo cut off where its old neighbour was.
int make_textures()
{
    Src t512 = sprite_src(0xC4), tm = sprite_src(0xC6), t6 = sprite_src(0xCA), g6 = sprite_src(0xD4);
    if (!t512.tex || !tm.tex || !t6.tex || !g6.tex || !t512.pal || !tm.pal || !t6.pal) return 0;
    if (t512.tex->format != 0x05910B2A || tm.tex->format != 0x05910B2A || t6.tex->format != 0x05810B2A ||
        g6.tex->format != 0x0581192A)
        return -1;
    uint8_t *dt = va(t6.tex->data), *dg = va(g6.tex->data);
    if (g_made && !memcmp(dt, g_text, sizeof g_text) && !memcmp(dg, g_glow, sizeof g_glow)) return 1; // still ours
    const uint8_t *s512 = va(t512.tex->data), *sm = va(tm.tex->data);
    if (fnv(s512, 16384) != 0xF4DBDF41 || fnv((const uint8_t *)t512.pal, 1024) != 0x72D483D2 ||
        fnv(sm, 16384) != 0x926120E0 || fnv((const uint8_t *)tm.pal, 1024) != 0xFC447605 ||
        fnv((const uint8_t *)t6.pal, 1024) != 0x0091A797)
        return g_made ? 1 : -1; // (made once and since reloaded as something else: leave it)
    Swz z512(512, 32), z256(256, 32);
    // pieces: the source (0 LOAD GAME / XBOX LIVE, 1 MASTER NINJA TOURNAMENT), the letter's fill columns [x0, x1), and
    // its neighbours' fill edges (the letter is taken no further)
    struct Piece {
        int src, x0, x1, prev, next;
    };
    const Piece pieces[] = {
        {0, 285, 299, 284, 317}, // E (GAME)
        {0, 369, 386, 368, 392}, // X (XBOX)
        {1, 199, 207, 197, 208}, // I (NINJA)
        {1, 258, 274, 253, 275}, // T (TOURNAMENT)
        {-1, 0, 7, 0, 0},        // space
        {0, 231, 299, 222, 317}, // GAME (LOAD GAME)
    };
    const int kPieces = sizeof pieces / sizeof pieces[0], kGap = 1, kMaxW = 128;
    static Px cut[kPieces][32][kMaxW];
    static uint8_t keep[32][512];
    static int stack[32 * 512];
    int width[kPieces], total = kGap * (kPieces - 1);
    for (int n = 0; n < kPieces; n++) {
        const Piece &p = pieces[n];
        if (p.src < 0) {
            width[n] = p.x1 - p.x0, total += width[n];
            continue;
        }
        const uint8_t *st = p.src ? sm : s512;
        const uint32_t *pal = p.src ? tm.pal : t512.pal;
        auto at = [&](int x, int y) { return px_of(pal[st[z512.at(x, y)]]); };
        // the letter: what joins its fill inside [x0, x1) (a flood fill), within its neighbours' edges
        int lo = p.x0 - 3 > p.prev ? p.x0 - 3 : p.prev, hi = (p.x1 + 3 < p.next ? p.x1 + 3 : p.next) + 2;
        memset(keep, 0, sizeof keep);
        int sp = 0;
        for (int y = 0; y < 32; y++)
            for (int x = p.x0; x < p.x1; x++)
                if (at(x, y).f > 0.15f) keep[y][x] = 1, stack[sp++] = y * 512 + x;
        while (sp) {
            int v = stack[--sp], y = v / 512, x = v % 512;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if ((dy || dx) && yy >= 0 && yy < 32 && xx >= lo && xx < hi && !keep[yy][xx]) {
                        Px q = at(xx, yy);
                        bool beyond = xx < p.prev + 1 || xx >= p.next; // a neighbour's columns: its shadow at most
                        if (beyond ? q.f < 0.1f && q.d > 0.25f : q.f > 0.02f || q.d > 0.25f)
                            keep[yy][xx] = 1, stack[sp++] = yy * 512 + xx;
                    }
                }
        }
        int a = hi, b = lo;
        for (int y = 0; y < 32; y++)
            for (int x = lo; x < hi; x++)
                if (keep[y][x]) a = x < a ? x : a, b = x + 1 > b ? x + 1 : b;
        if (b <= a || b - a > kMaxW) return -1;
        width[n] = b - a, total += width[n];
        for (int y = 0; y < 32; y++)
            for (int x = a; x < b; x++) cut[n][y][x - a] = keep[y][x] ? at(x, y) : Px{0, 0};
    }
    if (total > 250) return -1;
    static float F[32][256], D[32][256], H[32][256], G[32][256];
    memset(F, 0, sizeof F);
    memset(D, 0, sizeof D);
    int dx = (256 - total) / 2;
    for (int n = 0; n < kPieces; n++) {
        if (pieces[n].src >= 0)
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < width[n]; x++) {
                    const Px &q = cut[n][y][x];
                    float &f = F[y][dx + x], &d = D[y][dx + x];
                    if (q.f > f) f = q.f;
                    if (q.d > d) d = q.d;
                }
        dx += width[n] + kGap;
    }
    blur(F, H, 3.0f);
    blur(F, G, 1.6f);
    // Into the row's own palette (nearest colour) and texture layout
    std::unordered_map<uint32_t, uint8_t> nearest;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 256; x++) {
            float f = F[y][x], d = D[y][x] < 1 - f ? D[y][x] : 1 - f, h = 1.8f * H[y][x];
            if (h > 1) h = 1;
            float a = f + d + h * (1 - f - d), r = 255 * f + 253 * h * (1 - f - d), g = 255 * f;
            uint32_t c = 0;
            if (a > 0) {
                uint32_t ca = (uint32_t)(a * 255 + 0.5f), cr = (uint32_t)(r / a + 0.5f), cg = (uint32_t)(g / a + 0.5f);
                c = ca << 24 | (cr > 255 ? 255 : cr) << 16 | (cg > 255 ? 255 : cg) * 0x101u;
            }
            auto it = nearest.find(c);
            if (it == nearest.end()) {
                uint32_t best = ~0u;
                uint8_t bi = 0;
                for (int i = 0; i < 256; i++) {
                    uint32_t q = t6.pal[i], e2 = 0;
                    for (int s = 0; s < 32; s += 8) {
                        int e = (int)((c >> s) & 255) - (int)((q >> s) & 255);
                        e2 += (uint32_t)(e * e);
                    }
                    if (e2 < best) best = e2, bi = (uint8_t)i;
                }
                it = nearest.emplace(c, bi).first;
            }
            float gl = 4 * G[y][x] * 255;
            g_text[z256.at(x, y)] = it->second;
            g_glow[z256.at(x, y)] = (uint8_t)(gl > 255 ? 255 : gl + 0.5f);
        }
    memcpy(dt, g_text, sizeof g_text);
    memcpy(dg, g_glow, sizeof g_glow);
    xd3d::texture_note_write(dt, sizeof g_text); // the texture cache uploads them again (also where the write watch is missing)
    xd3d::texture_note_write(dg, sizeof g_glow);
    g_made = true;
    logf("ng04 exit menu: EXIT GAME written into the title's spare row textures (%zu colours)", nearest.size());
    return 1;
}

using SoundFn = void(__cdecl *)(int, int);
using BoxFn = void(__cdecl *)(void *);

} // namespace

// ---- called from the thunks -----------------------------------------------------------------------------------------

// The list was built: is the seventh row to be added? (the textures checked; not loaded yet is fine, made at drawing)
extern "C" uint32_t __cdecl ng04x_want_row()
{
    if (!g_ng04x_rows) return 0;
    int r = make_textures();
    if (r < 0) {
        g_ng04x_rows = 0;
        logf("ng04 exit menu: the title's menu textures are not the expected ones; no EXIT GAME row");
        return 0;
    }
    return 1;
}

extern "C" void __cdecl ng04x_before_draw()
{
    if (g_ng04x_rows && make_textures() < 0) g_ng04x_rows = 0;
}

extern "C" void __cdecl ng04x_open()
{
    g_box_frames = 0;
    g_ng04x_dialog = 1;
    logf("ng04 exit menu: EXIT GAME selected, asking");
}

// A frame of the menu while the box is up: Ⓐ / Ⓑ, then the box
extern "C" void __cdecl ng04x_frame()
{
    uint32_t port = *(const uint8_t *)(uintptr_t)g_a->port;
    uint32_t pressed = *(const uint32_t *)(uintptr_t)(g_a->pad + port * 0x78 + 8);
    SoundFn sound = (SoundFn)(uintptr_t)g_a->sound;
    if (g_box_frames++ > 0) { // not the press that opened it
        if (pressed & 0x510) {
            if (!exit_requested()) {
                sound(5, 0x40);
                logf("ng04 exit menu: yes, leaving");
                request_exit(0, "EXIT GAME on the title menu");
            }
        } else if (pressed & 0x220) {
            sound(3, 0x40);
            g_ng04x_dialog = 0;
            logf("ng04 exit menu: no, back to the menu");
            return;
        }
    }
    // The box writes its text only while [text_on] is set (the Master Ninja Tournament's screens set it): for this call
    auto *text_on = (uint32_t *)(uintptr_t)g_a->text_on;
    uint32_t old_on = *text_on;
    *text_on = 1;
    ((BoxFn)(uintptr_t)g_a->box)(g_box);
    *text_on = old_on;
}

namespace {

// After the build loop (mov [count], cl): the seventh row, then the count
__declspec(naked) void append_thunk()
{
    __asm {
        pushad
        call ng04x_want_row
        test eax, eax
        popad
        jz store
        push eax
        push edx
        movzx eax, cl
        mov byte ptr [g_ng04x_ids + eax], 9 // kExitId
        mov edx, dword ptr [g_ng04x_p2]
        mov byte ptr [edx + eax], 6 // kExitSprite
        pop edx
        pop eax
        inc cl
    store:
        push eax
        mov eax, dword ptr [g_ng04x_count]
        mov byte ptr [eax], cl
        pop eax
        jmp dword ptr [g_ng04x_append_resume]
    }
}

// A row selected (ecx = the cursor; FUN_00171A80 put the chosen row into [esi+0xC]): EXIT GAME opens the box instead
__declspec(naked) void select_thunk()
{
    __asm {
        movzx edx, byte ptr [g_ng04x_ids + ecx]
        cmp edx, 9 // kExitId
        je ours
        jmp dword ptr [g_ng04x_select_resume]
    ours:
        mov byte ptr [esi + 0xC], 0xFF // no row chosen: the rows would play their "chosen" fade-out (mode 3) and stop
        pushad
        call ng04x_open
        popad
        jmp dword ptr [g_ng04x_select_skip]
    }
}

// The menu's drawing (call FUN_00171E00), then the box over it while it is up
__declspec(naked) void draw_thunk()
{
    __asm {
        pushad
        call ng04x_before_draw
        popad
        call dword ptr [g_ng04x_menu_draw]
        cmp dword ptr [g_ng04x_dialog], 0
        je done
        pushad
        call ng04x_frame
        popad
    done:
        ret
    }
}

// FUN_00170F80 (a title-menu button test): nothing pressed while the box is up
__declspec(naked) void pad_thunk()
{
    __asm {
        xor eax, eax
        cmp dword ptr [g_ng04x_dialog], 0
        jne blocked
        cmp ecx, 0x2000
        jmp dword ptr [g_ng04x_pad_resume]
    blocked:
        ret
    }
}

// FUN_001D5AC0 (a message's text): the box's question
__declspec(naked) void text_thunk()
{
    __asm {
        cmp eax, 0x0004F0E1 // kBoxMessage
        je ours
        push ebx
        mov bl, byte ptr [esp + 8]
        push edi
        jmp dword ptr [g_ng04x_text_resume]
    ours:
        mov eax, offset kBoxText
        ret
    }
}

void put_branch(uint32_t at, uint8_t op, const void *to, uint32_t len)
{
    auto *p = (uint8_t *)(uintptr_t)at;
    DWORD old;
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    p[0] = op; // E8 call / E9 jmp
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
    for (uint32_t i = 5; i < len; i++) p[i] = 0x90;
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
}

void put_u32(uint32_t at, uint32_t v)
{
    DWORD old;
    VirtualProtect((void *)(uintptr_t)at, 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)(uintptr_t)at, &v, 4);
    VirtualProtect((void *)(uintptr_t)at, 4, old, &old);
}

bool expect(uint32_t at, std::initializer_list<uint8_t> bytes)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)at;
    for (uint8_t b : bytes)
        if (*p++ != b) {
            logf("ng04 exit menu: code at 0x%08x is not as expected; no EXIT GAME row", at);
            return false;
        }
    return true;
}

uint8_t b(uint32_t v, int i) { return (uint8_t)(v >> (i * 8)); }

} // namespace

void install_ng04_exitmenu()
{
    const BuildProfile *pr = g_cfg.profile;
    if (pr == &kProfileNg04Tu) g_a = &kTu;
    if (pr == &kProfileNg04Disc) g_a = &kDisc;
    if (!g_a) return;
    const Addrs &a = *g_a;
    int32_t rel = (int32_t)(a.menu_draw - (a.select_skip + 5));
    uint32_t ids = a.ids, cnt = a.count;
    if (!expect(a.append, {0x88, 0x0D, b(cnt, 0), b(cnt, 1), b(cnt, 2), b(cnt, 3)}) ||
        !expect(a.ids_write, {0x88, 0x81, b(ids, 0), b(ids, 1), b(ids, 2), b(ids, 3)}) ||
        !expect(a.select, {0x0F, 0xB6, 0x91, b(ids, 0), b(ids, 1), b(ids, 2), b(ids, 3)}) ||
        !expect(a.ids_read2, {0x0F, 0xB6, 0x88, b(ids, 0), b(ids, 1), b(ids, 2), b(ids, 3)}) ||
        !expect(a.select_skip, {0xE8, b(rel, 0), b(rel, 1), b(rel, 2), b(rel, 3)}) ||
        !expect(a.pad_test, {0x33, 0xC0, 0x81, 0xF9, 0x00, 0x20, 0x00, 0x00}) ||
        !expect(a.text, {0x53, 0x8A, 0x5C, 0x24, 0x08, 0x57}))
        return;
    g_ng04x_p2 = a.p2;
    g_ng04x_count = a.count;
    g_ng04x_append_resume = a.append + 6;
    g_ng04x_select_resume = a.select + 7;
    g_ng04x_select_skip = a.select_skip;
    g_ng04x_menu_draw = a.menu_draw;
    g_ng04x_pad_resume = a.pad_test + 8;
    g_ng04x_text_resume = a.text + 6;
    g_ng04x_sprite = a.sprite;
    put_u32(a.ids_write + 2, (uint32_t)(uintptr_t)g_ng04x_ids);
    put_u32(a.ids_read2 + 3, (uint32_t)(uintptr_t)g_ng04x_ids);
    put_branch(a.append, 0xE9, append_thunk, 6);
    put_branch(a.select, 0xE9, select_thunk, 7);
    put_branch(a.select_skip, 0xE8, draw_thunk, 5);
    put_branch(a.pad_test, 0xE9, pad_thunk, 8);
    put_branch(a.text, 0xE9, text_thunk, 6);
    logf("ng04 exit menu: EXIT GAME row installed (seven title rows, the green box)");
}
