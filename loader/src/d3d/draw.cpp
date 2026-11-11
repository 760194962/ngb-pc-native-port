// Draw calls and state application. State lives in the Xbox arrays (render_state(), texture_state())
// because game code writes them directly; it is translated to D3D9 at each draw (with a cache of what
// was last sent).
#include "d3d/draw.h"

#include "d3d/device.h"
#include "d3d/dispatch.h"
#include "d3d/ffp_ps.h"
#include "d3d/host.h"
#include "build_profile.h"
#include "d3d/nv2a_vsh.h"
#include "d3d/pshader.h"
#include "d3d/pushbuf.h"
#include "d3d/state.h"
#include "d3d/vshader.h"
#include "fov.h"
#include "image.h"
#include "perf.h"
#include "xbe.h"

#include <wincodec.h>
#include <intrin.h> // _AddressOfReturnAddress: MSVC 14.16 (VS2017) does not declare it via <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>

namespace xd3d {

LastDraw g_last_draws[4];
unsigned g_last_draw_next;

IDirect3DTexture9 *rt_texture_for(const PixelContainer *tex, uint32_t draw_serial, bool linear); // device.cpp
bool samples_shadow_map(); // device.cpp
uint32_t g_draw_serial; // increments per draw (render-target snapshots are shared within a draw)
uint32_t backbuffer_data();
uint32_t depthbuffer_data();
// --debug-frame N[-M]: true while frame N (or any frame up to M) is being built
bool debug_frame_active()
{
    uint32_t f = g_frame_count + 1;
    if (!g_cfg.debug_frame) return false;
    return g_cfg.debug_frame_end ? (f >= g_cfg.debug_frame && f <= g_cfg.debug_frame_end) : f == g_cfg.debug_frame;
}

// ---- enum conversions ----

D3DCMPFUNC conv_cmp(uint32_t v) { return (v >= 0x200 && v <= 0x207) ? (D3DCMPFUNC)(v - 0x200 + 1) : D3DCMP_ALWAYS; }

D3DBLEND conv_blend(uint32_t v)
{
    switch (v) {
    case 0: return D3DBLEND_ZERO;
    case 1: return D3DBLEND_ONE;
    case 0x300: return D3DBLEND_SRCCOLOR;
    case 0x301: return D3DBLEND_INVSRCCOLOR;
    case 0x302: return D3DBLEND_SRCALPHA;
    case 0x303: return D3DBLEND_INVSRCALPHA;
    case 0x304: return D3DBLEND_DESTALPHA;
    case 0x305: return D3DBLEND_INVDESTALPHA;
    case 0x306: return D3DBLEND_DESTCOLOR;
    case 0x307: return D3DBLEND_INVDESTCOLOR;
    case 0x308: return D3DBLEND_SRCALPHASAT;
    // The Xbox values are the NV2A/GL ones: 0x8001 CONSTANTCOLOR, 0x8002 INVCONSTANTCOLOR, 0x8003 CONSTANTALPHA,
    // 0x8004 INVCONSTANTALPHA. D3D9 only has BLENDFACTOR; for the alpha modes the factor is the blend colour's
    // alpha replicated (see blend_factor in apply_render_states).
    case 0x8001: case 0x8003: return D3DBLEND_BLENDFACTOR;
    case 0x8002: case 0x8004: return D3DBLEND_INVBLENDFACTOR;
    default: return D3DBLEND_ONE;
    }
}

D3DBLENDOP conv_blendop(uint32_t v)
{
    switch (v) {
    case 0x800A: return D3DBLENDOP_SUBTRACT;
    case 0x800B: return D3DBLENDOP_REVSUBTRACT;
    case 0x8007: return D3DBLENDOP_MIN;
    case 0x8008: return D3DBLENDOP_MAX;
    default: return D3DBLENDOP_ADD;
    }
}

D3DSTENCILOP conv_stencilop(uint32_t v)
{
    switch (v) {
    case 0: return D3DSTENCILOP_ZERO;
    case 0x1E01: return D3DSTENCILOP_REPLACE;
    case 0x1E02: return D3DSTENCILOP_INCRSAT;
    case 0x1E03: return D3DSTENCILOP_DECRSAT;
    case 0x150A: return D3DSTENCILOP_INVERT;
    case 0x8507: return D3DSTENCILOP_INCR;
    case 0x8508: return D3DSTENCILOP_DECR;
    default: return D3DSTENCILOP_KEEP;
    }
}

D3DTEXTUREOP conv_texop(uint32_t v)
{
    static const D3DTEXTUREOP map[] = {
        D3DTOP_DISABLE, D3DTOP_DISABLE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE, D3DTOP_MODULATE2X,
        D3DTOP_MODULATE4X, D3DTOP_ADD, D3DTOP_ADDSIGNED, D3DTOP_ADDSIGNED2X, D3DTOP_SUBTRACT, D3DTOP_ADDSMOOTH,
        D3DTOP_BLENDDIFFUSEALPHA, D3DTOP_BLENDCURRENTALPHA, D3DTOP_BLENDTEXTUREALPHA, D3DTOP_BLENDFACTORALPHA,
        D3DTOP_BLENDTEXTUREALPHAPM, D3DTOP_PREMODULATE, D3DTOP_MODULATEALPHA_ADDCOLOR, D3DTOP_MODULATECOLOR_ADDALPHA,
        D3DTOP_MODULATEINVALPHA_ADDCOLOR, D3DTOP_MODULATEINVCOLOR_ADDALPHA, D3DTOP_DOTPRODUCT3, D3DTOP_MULTIPLYADD,
        D3DTOP_LERP, D3DTOP_BUMPENVMAP, D3DTOP_BUMPENVMAPLUMINANCE};
    return v < sizeof map / sizeof map[0] ? map[v] : D3DTOP_MODULATE;
}

D3DTEXTUREADDRESS conv_address(uint32_t v)
{
    switch (v) {
    case 2: return D3DTADDRESS_MIRROR;
    case 3: case 5: return D3DTADDRESS_CLAMP;
    case 4: return D3DTADDRESS_BORDER;
    default: return D3DTADDRESS_WRAP;
    }
}

DWORD conv_colorwrite(uint32_t v)
{
    DWORD r = 0;
    if (v & 0x00010000) r |= D3DCOLORWRITEENABLE_RED;
    if (v & 0x00000100) r |= D3DCOLORWRITEENABLE_GREEN;
    if (v & 0x00000001) r |= D3DCOLORWRITEENABLE_BLUE;
    if (v & 0x01000000) r |= D3DCOLORWRITEENABLE_ALPHA;
    return r;
}

static D3DCULL conv_cull(uint32_t cull)
{
    if (cull == 0) return D3DCULL_NONE;
    return cull == 0x900 ? D3DCULL_CW : D3DCULL_CCW;
}

// ---- defaults (what the original CreateDevice sets up) ----

static float as_float(uint32_t v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static uint32_t f2u(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

void init_default_states()
{
    uint32_t *rs = render_state();
    rs[XRS_ZFUNC] = 0x203;
    rs[XRS_ALPHAFUNC] = 0x207;
    rs[XRS_SRCBLEND] = 1;
    rs[XRS_DESTBLEND] = 0;
    rs[XRS_ZWRITEENABLE] = 1;
    rs[XRS_SHADEMODE] = 0x1D01;
    rs[XRS_COLORWRITEENABLE] = 0x01010101;
    rs[XRS_STENCILZFAIL] = rs[XRS_STENCILPASS] = rs[XRS_STENCILFAIL] = 0x1E00;
    rs[XRS_STENCILFUNC] = 0x207;
    rs[XRS_STENCILMASK] = rs[XRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    rs[XRS_BLENDOP] = 0x8006;
    rs[XRS_FOGEND] = f2u(1.0f);
    rs[XRS_FOGDENSITY] = f2u(1.0f);
    rs[XRS_LIGHTING] = 1;
    rs[XRS_LOCALVIEWER] = 1;
    rs[XRS_COLORVERTEX] = 1;
    rs[XRS_SPECULARMATERIALSOURCE] = 2;
    rs[XRS_DIFFUSEMATERIALSOURCE] = 1;
    rs[XRS_POINTSIZE] = f2u(1.0f);
    rs[XRS_POINTSIZE_MAX] = f2u(64.0f); // D3D8 defaults
    rs[XRS_POINTSCALE_A] = f2u(1.0f);
    rs[XRS_FILLMODE] = rs[XRS_BACKFILLMODE] = 0x1B02;
    rs[XRS_ZENABLE] = 1;
    rs[XRS_FRONTFACE] = 0x900;
    rs[XRS_CULLMODE] = 0x901;
    rs[XRS_TEXTUREFACTOR] = 0xFFFFFFFF;
    rs[XRS_MULTISAMPLEMASK] = 0xFFFFFFFF;
    for (int s = 0; s < 4; s++) {
        uint32_t *ts = texture_state(s);
        ts[0] = ts[1] = ts[2] = 1;   // ADDRESSU/V/W = WRAP
        ts[3] = ts[4] = 2;           // MAG/MIN = LINEAR
        ts[5] = 0;                   // MIPFILTER = NONE
        ts[8] = 1;                   // MAXANISOTROPY
        ts[12] = s == 0 ? 4 : 1;     // COLOROP: MODULATE / DISABLE
        ts[14] = 2;                  // COLORARG1 = TEXTURE
        ts[15] = 1;                  // COLORARG2 = CURRENT
        ts[16] = s == 0 ? 2 : 1;     // ALPHAOP: SELECTARG1 / DISABLE
        ts[18] = 2;                  // ALPHAARG1 = TEXTURE
        ts[19] = 1;                  // ALPHAARG2 = CURRENT
        ts[20] = 1;                  // RESULTARG = CURRENT
        ts[28] = (uint32_t)s;        // TEXCOORDINDEX
    }
}

// ---- state application ----

namespace {

uint32_t g_rs_sent[XRS_MAX];
bool g_rs_valid[XRS_MAX];
uint32_t g_ts_sent[4][32];
bool g_ts_valid[4][32];
IDirect3DBaseTexture9 *g_tex_sent[4];

// What the host holds for states the port sets at every draw (point sprites, the stage texture scale): sent again
// only when the value changes. Every draw sent 8 point states, 4 texture transforms and 4 transform flags before,
// ~7,000 of the ~11,000 Direct3D calls of a chapter-1 frame (r192, --api-count), each one a D3D9On12 translation
// on integrated GPUs. Cleared by forget_sent_state (device reset, fullscreen passes) and when another path sets a
// texture transform (host_texture_transform_changed).
DWORD g_point_sent[8];
bool g_point_valid;
D3DMATRIX g_texm_sent[4];
bool g_texm_valid[4];
uint32_t g_texm_stale; // draws that found another matrix on the host where the game's own applies
DWORD g_ttff_sent[4];
bool g_ttff_valid[4];

void set_tex_transform(IDirect3DDevice9 *d, int s, const D3DMATRIX &m)
{
    if (g_texm_valid[s] && !memcmp(&g_texm_sent[s], &m, sizeof m)) return;
    g_texm_sent[s] = m;
    g_texm_valid[s] = true;
    d->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &m);
}

void set_ttff(IDirect3DDevice9 *d, int s, DWORD v)
{
    if (g_ttff_valid[s] && g_ttff_sent[s] == v) return;
    g_ttff_sent[s] = v;
    g_ttff_valid[s] = true;
    d->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, v);
}

// Whether the bound pixel shader's combiners read the FOG register (register 3) in any used input.
bool psh_reads_fog()
{
    const uint32_t *rs = render_state();
    auto has = [](uint32_t inputs) {
        for (int b = 0; b < 4; b++)
            if (((inputs >> (8 * b)) & 0x0F) == 3) return true;
        return false;
    };
    uint32_t stages = rs[53] & 0xFF; // PSCombinerCount
    if (stages > 8) stages = 8;
    for (uint32_t i = 0; i < stages; i++)
        if (has(rs[34 + i]) || has(rs[i])) return true; // PSRGBInputs / PSAlphaInputs
    return has(rs[8]) || has(rs[9] & 0xFFFFFF00u);         // final combiner ABCD, EFG (low byte = flags)
}

template <typename F> void on_change(uint32_t state, F apply)
{
    uint32_t v = render_state()[state];
    if (g_rs_valid[state] && g_rs_sent[state] == v) return;
    g_rs_valid[state] = true;
    g_rs_sent[state] = v;
    apply(v);
}

void apply_render_states(IDirect3DDevice9 *d)
{
    auto set = [&](D3DRENDERSTATETYPE s, DWORD v) { d->SetRenderState(s, v); };
    on_change(XRS_ZENABLE, [&](uint32_t v) { set(D3DRS_ZENABLE, v); });
    // Depth bias: the NV2A polygon offset (D3DRS_POLYGONOFFSETZOFFSET in depth units, POLYGONOFFSETZSLOPESCALE,
    // enabled for solid fill by SOLIDOFFSETENABLE) -> the host's DEPTHBIAS / SLOPESCALEDEPTHBIAS. D3DRS_ZBIAS reaches
    // these states through the library's conversion (ngb/d3d_ngb.cpp SetRenderState_ZBias), the game also writes
    // them inline (FUN_001D45C0, FUN_001D5420). A unit is 1/2^24 of the 24-bit depth range. The slope term is
    // per pixel of the target (the largest depth change between neighbouring pixels): at k times the Xbox
    // resolution the slope is k times smaller, so the factor is scaled by the target scale to keep the same offset.
    // Ninja Gaiden Black draws lightmap / decal layers coplanar over the rocks with ZBIAS (docs/04 §3.8, §3.17).
    {
        const uint32_t *rs = render_state();
        float bias = 0.0f, slope = 0.0f;
        // The vertex-program draws with an offset (the shadow passes): the NV2A takes the slope per Xbox pixel on each axis
        // (xemu psh.c), the host one factor for both. Taken on the smaller render-scale axis the offset is the console's
        // where the depth changes along that axis (y at 4 x 3: the ground seen from behind Ryu) and less elsewhere, never
        // more: more let a lower ground layer's shadow through (the band, run/r835-r842). Exact per pixel (zoffset 1)
        // cost about 2 ms on the iGPU at 4K (r880-r885: the pixel shader writing depth loses early depth rejection).
        const bool prog = prog_depth_offset(), in_shader = prog && g_cfg.zoffset_mode == 1;
        if (rs[XRS_SOLIDOFFSETENABLE] && !in_shader) {
            float off;
            memcpy(&off, &rs[XRS_POLYGONOFFSETZOFFSET], 4);
            memcpy(&slope, &rs[XRS_POLYGONOFFSETZSLOPESCALE], 4);
            bias = off / 16777216.0f;
            slope *= prog && g_cfg.zoffset_mode == 2 ? std::min(target_scale_x(), target_scale_y())
                                                     : std::max(target_scale_x(), target_scale_y());
        }
        // Ryu's shadow pass -- the surfaces drawn again over themselves (multiplied into the picture), sampling the
        // shadow map, depth tested and not written -- pulled toward the camera, as ngbrecomp #106 does: drawn by another
        // vertex path (a vertex program over the fixed-function first pass), its depths tie with what is already there,
        // and as Ryu moved they went one way or the other: the shadow flickered between two shades (the user, outside the
        // chapter-1 gate, 2026-10-31; not on xemu; two shades as if one of two layers there lost them, not yet confirmed).
        // The game's own offset there is -3 / -0.75 (ps 0x3ad6bf0). Each step adds 2 depth units and half a console
        // pixel's worth of slope: 1 (ngbrecomp's) stopped it where the ground is flat, uneven ground needed 4 (8 too);
        // shadow quality, render scale and point filtering made no difference. cfg:shadowpull:N, 0 off.
        // 2 since 2.0.3: most of that flicker was the vertex-program path's 1/8-pixel snap (nv2a_vsh.cpp, now off);
        // 4 also passed surfaces that lie behind the floor, a second, displaced shadow in Murai's dojo (Black and
        // 2004; run/r790, r791; the user chose 2 with the snap off, 2026-11-01). 1 since 2.1.0 (the user, 2026-11-01).
        // Since 2.1.2 the vertex-program shadow passes take the NV2A offset per pixel (exact_depth_offset) and no pull:
        // the pull's slope part let a second, lower ground layer's shadow through, a darker band that moved with the
        // camera (run/r835-r842: depth paths agree within 2-4 units; the gap was the host's one-factor slope bias).
        if (g_cfg.shadow_pull && !(prog && g_cfg.zoffset_mode) && rs[XRS_ZENABLE] && !rs[XRS_ZWRITEENABLE] && samples_shadow_map()) {
            int pb = g_cfg.shadow_pull_bias >= 0 ? g_cfg.shadow_pull_bias : g_cfg.shadow_pull;
            int ps = g_cfg.shadow_pull_slope >= 0 ? g_cfg.shadow_pull_slope : g_cfg.shadow_pull;
            bias -= 2.0f * pb / 16777216.0f;
            slope -= 0.5f * ps * std::max(target_scale_x(), target_scale_y());
        }
        static float sent_bias, sent_slope;
        if (!g_rs_valid[XRS_ZBIAS] || bias != sent_bias || slope != sent_slope) { // ZBIAS's slot marks "sent"
            DWORD b, s;
            memcpy(&b, &bias, 4);
            memcpy(&s, &slope, 4);
            set(D3DRS_DEPTHBIAS, b);
            set(D3DRS_SLOPESCALEDEPTHBIAS, s);
            sent_bias = bias;
            sent_slope = slope;
            g_rs_valid[XRS_ZBIAS] = true;
        }
    }
    on_change(XRS_ZFUNC, [&](uint32_t v) { set(D3DRS_ZFUNC, conv_cmp(v)); });
    on_change(XRS_ZWRITEENABLE, [&](uint32_t v) { set(D3DRS_ZWRITEENABLE, v != 0); });
    on_change(XRS_ALPHATESTENABLE, [&](uint32_t v) { set(D3DRS_ALPHATESTENABLE, v != 0); });
    on_change(XRS_ALPHAFUNC, [&](uint32_t v) { set(D3DRS_ALPHAFUNC, conv_cmp(v)); });
    on_change(XRS_ALPHAREF, [&](uint32_t v) { set(D3DRS_ALPHAREF, v & 0xFF); });
    on_change(XRS_ALPHABLENDENABLE, [&](uint32_t v) { set(D3DRS_ALPHABLENDENABLE, v != 0); });
    // D3DBLEND_CONSTANTALPHA / INVCONSTANTALPHA (0x8003 / 0x8004) take the blend colour's alpha, replicated into
    // the host's BLENDFACTOR. Ninja Gaiden Black copies the back buffer with them (BLENDCOLOR 0xffffffff: a full
    // copy) and draws a fade overlay over the pickup dialog with 0x00ffffff (alpha 0: nothing); read as the colour
    // modes, that overlay replaced the dialog with the snapshot (docs/04 §3.8).
    auto blend_factor = [&]() {
        const uint32_t *rs = render_state();
        uint32_t sb = rs[XRS_SRCBLEND], db = rs[XRS_DESTBLEND], bc = rs[XRS_BLENDCOLOR];
        bool alpha = sb == 0x8003 || sb == 0x8004 || db == 0x8003 || db == 0x8004;
        return alpha ? (bc >> 24) * 0x01010101u : bc;
    };
    on_change(XRS_SRCBLEND, [&](uint32_t v) {
        set(D3DRS_SRCBLEND, conv_blend(v));
        g_rs_valid[XRS_BLENDCOLOR] = false; // the factor depends on the mode
    });
    on_change(XRS_DESTBLEND, [&](uint32_t v) {
        set(D3DRS_DESTBLEND, conv_blend(v));
        g_rs_valid[XRS_BLENDCOLOR] = false;
    });
    on_change(XRS_BLENDOP, [&](uint32_t v) { set(D3DRS_BLENDOP, conv_blendop(v)); });
    on_change(XRS_BLENDCOLOR, [&](uint32_t) { set(D3DRS_BLENDFACTOR, blend_factor()); });
    on_change(XRS_DITHERENABLE, [&](uint32_t v) { set(D3DRS_DITHERENABLE, v != 0); });
    on_change(XRS_SHADEMODE, [&](uint32_t v) { set(D3DRS_SHADEMODE, v == 0x1D00 ? D3DSHADE_FLAT : D3DSHADE_GOURAUD); });
    on_change(XRS_COLORWRITEENABLE, [&](uint32_t v) { set(D3DRS_COLORWRITEENABLE, conv_colorwrite(v)); });
    on_change(XRS_STENCILENABLE, [&](uint32_t v) { set(D3DRS_STENCILENABLE, v != 0); });
    on_change(XRS_STENCILFUNC, [&](uint32_t v) { set(D3DRS_STENCILFUNC, conv_cmp(v)); });
    on_change(XRS_STENCILREF, [&](uint32_t v) { set(D3DRS_STENCILREF, v); });
    on_change(XRS_STENCILMASK, [&](uint32_t v) { set(D3DRS_STENCILMASK, v); });
    on_change(XRS_STENCILWRITEMASK, [&](uint32_t v) { set(D3DRS_STENCILWRITEMASK, v); });
    on_change(XRS_STENCILFAIL, [&](uint32_t v) { set(D3DRS_STENCILFAIL, conv_stencilop(v)); });
    on_change(XRS_STENCILZFAIL, [&](uint32_t v) { set(D3DRS_STENCILZFAIL, conv_stencilop(v)); });
    on_change(XRS_STENCILPASS, [&](uint32_t v) { set(D3DRS_STENCILPASS, conv_stencilop(v)); });
    on_change(XRS_CULLMODE, [&](uint32_t v) { set(D3DRS_CULLMODE, conv_cull(v)); });
    on_change(XRS_FILLMODE, [&](uint32_t v) {
        set(D3DRS_FILLMODE, v == 0x1B00 ? D3DFILL_POINT : v == 0x1B01 ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    });
    // Host fog is for the fixed-function path only. The programmable path gets the NV2A fog factor interpolated
    // through VSOut.t4 and blends it into its own FOGR register (nv2a_vsh.cpp / nv2a_psh.cpp), so adding D3D9's
    // post-shader fog there would apply it twice. Reversed linear ranges still mean no visible fog (see below).
    {
        static DWORD fog_sent;
        DWORD fog = render_state()[XRS_FOGENABLE] != 0 && !g_cfg.no_fog && !g_st.pixel_shader;
        if (fog && render_state()[XRS_FOGTABLEMODE] == D3DFOG_LINEAR) {
            // The Stage 3 skylight cutscene sets fog 5 -> -1.2 with the 0xff6e3c1e colour. D3D9's fixed-function
            // linear fog on Intel saturates every vertex to the fog colour and the whole scene turns flat brown. The
            // NV2A keeps the formula, (end - d) / (end - start): fog only nearer than 5, rising towards the camera,
            // which D3D9's linear fog cannot express (its factor always falls with distance); it stays off here.
            // start = end is the library's slope of 8192 (FUN_003D0EC0), sent as a start 1/8192 before the end.
            float fs = 0, fe = 0;
            memcpy(&fs, &render_state()[XRS_FOGSTART], 4);
            memcpy(&fe, &render_state()[XRS_FOGEND], 4);
            if (fe < fs) fog = 0;
        }
        if (!g_rs_valid[XRS_FOGENABLE] || fog != fog_sent) {
            set(D3DRS_FOGENABLE, fog);
            fog_sent = fog;
            g_rs_valid[XRS_FOGENABLE] = true;
        }
    }
    auto fog_start = [&]() { // the start sent: 1/8192 before the end when they are equal (see above)
        float fs, fe;
        memcpy(&fs, &render_state()[XRS_FOGSTART], 4);
        memcpy(&fe, &render_state()[XRS_FOGEND], 4);
        return fs == fe ? f2u(fe - 1.0f / 8192.0f) : render_state()[XRS_FOGSTART];
    };
    on_change(XRS_FOGSTART, [&](uint32_t) { set(D3DRS_FOGSTART, fog_start()); });
    on_change(XRS_FOGEND, [&](uint32_t v) {
        set(D3DRS_FOGEND, v);
        set(D3DRS_FOGSTART, fog_start());
    });
    on_change(XRS_FOGDENSITY, [&](uint32_t v) { set(D3DRS_FOGDENSITY, v); });
    on_change(XRS_FOGCOLOR, [&](uint32_t v) { set(D3DRS_FOGCOLOR, v); });
    on_change(XRS_RANGEFOGENABLE, [&](uint32_t v) { set(D3DRS_RANGEFOGENABLE, v != 0); });
    on_change(XRS_LIGHTING, [&](uint32_t v) { set(D3DRS_LIGHTING, v != 0); });
    // Xbox D3DRS_VERTEXBLEND values are NV2A skin modes (SetRenderState_VertexBlend 0x3C5BE0 writes the value to
    // NV097_SET_SKIN_MODE as is): 0 off, 1 = 2 matrices / 1 weight, 2 = 2 weights 2 matrices, 3 = 3 matrices / 2
    // weights, 4 = 3 weights 3 matrices, 5 = 4 matrices / 3 weights, 6 = 4 weights 4 matrices. Direct3D 9 only has
    // the "last weight computed" forms; the explicit-last-weight ones use the same matrix count. Ninja Gaiden
    // Black's character joints are XYZB1 meshes (FVF 0x116) with mode 1; without this they were drawn with
    // WORLD0 only and ended up off the body.
    on_change(XRS_VERTEXBLEND, [&](uint32_t v) {
        DWORD h = v == 0 ? D3DVBF_DISABLE : v <= 2 ? D3DVBF_1WEIGHTS : v <= 4 ? D3DVBF_2WEIGHTS : D3DVBF_3WEIGHTS;
        set(D3DRS_VERTEXBLEND, h);
    });
    on_change(XRS_SPECULARENABLE, [&](uint32_t v) { set(D3DRS_SPECULARENABLE, v != 0 && !g_cfg.no_specular); });
    // LOCALVIEWER: the Xbox library turns it into the LOCALEYE bit of the NV2A light control (FUN_003d18e0, method
    // 0x294, set only with lights on) and never writes the texgen view model (method 0x9CC: no such header anywhere in
    // the XBE, no 0x09CC in the D3D section), which keeps its reset value, so reflection vectors (TCI 0x30000) on the
    // Xbox are taken with a local viewer (xemu: CSV0_D TEXGEN_REF, infinite only when set). D3D9 has one state for both:
    // a draw with a reflection stage gets TRUE, the others the game's value. Ninja Gaiden Black sets FALSE on its world
    // geometry; the military base's rails then read one spot of their environment map along their whole length, a flat
    // white line (the user, 2026-10-22; with TRUE the light and dark along them came back). cfg:localviewer:2 sends the
    // game's value as before, 0 / 1 force it.
    {
        DWORD v = render_state()[XRS_LOCALVIEWER] != 0;
        if (g_cfg.force_localviewer == 0 || g_cfg.force_localviewer == 1) {
            v = (DWORD)g_cfg.force_localviewer;
        } else if (g_cfg.force_localviewer != 2) {
            for (int s = 0; s < 4; s++) {
                const uint32_t *ts = texture_state(s);
                if (ts[12] != 1 && (ts[28] & 0xFFFF0000) == 0x30000) v = TRUE; // an enabled stage: reflection vector
            }
        }
        static DWORD sent;
        if (!g_rs_valid[XRS_LOCALVIEWER] || sent != v) {
            set(D3DRS_LOCALVIEWER, v);
            sent = v;
            g_rs_valid[XRS_LOCALVIEWER] = true;
        }
    }
    on_change(XRS_COLORVERTEX, [&](uint32_t v) { set(D3DRS_COLORVERTEX, v != 0); });
    on_change(XRS_SPECULARMATERIALSOURCE, [&](uint32_t v) { set(D3DRS_SPECULARMATERIALSOURCE, v); });
    on_change(XRS_DIFFUSEMATERIALSOURCE, [&](uint32_t v) { set(D3DRS_DIFFUSEMATERIALSOURCE, v); });
    on_change(XRS_AMBIENTMATERIALSOURCE, [&](uint32_t v) { set(D3DRS_AMBIENTMATERIALSOURCE, v); });
    on_change(XRS_EMISSIVEMATERIALSOURCE, [&](uint32_t v) { set(D3DRS_EMISSIVEMATERIALSOURCE, v); });
    on_change(XRS_AMBIENT, [&](uint32_t v) { set(D3DRS_AMBIENT, v); });
    on_change(XRS_NORMALIZENORMALS, [&](uint32_t v) { set(D3DRS_NORMALIZENORMALS, v != 0); });
    on_change(XRS_TEXTUREFACTOR, [&](uint32_t v) { set(D3DRS_TEXTUREFACTOR, v); });
    // Point sprites: Ninja Gaiden Black draws its particles (torch smoke, sparks: FUN_002b9d20) as POINTLIST with
    // POINTSPRITEENABLE. Only POINTSIZE reached the host before, so each particle was one pixel. Sizes are pixels of
    // the Xbox target; POINTSCALEENABLE sizes scale with the viewport height (already the host's), so the host
    // target scale applies to the fixed size and to the min / max clamps. Set every draw: the target can change.
    {
        const uint32_t *rs = render_state();
        float k = target_scale_y();
        static const D3DRENDERSTATETYPE kPoint[8] = {D3DRS_POINTSPRITEENABLE, D3DRS_POINTSCALEENABLE, D3DRS_POINTSIZE,
                                                     D3DRS_POINTSIZE_MIN, D3DRS_POINTSIZE_MAX, D3DRS_POINTSCALE_A,
                                                     D3DRS_POINTSCALE_B, D3DRS_POINTSCALE_C};
        const DWORD v[8] = {rs[XRS_POINTSPRITEENABLE] != 0, rs[XRS_POINTSCALEENABLE] != 0,
                            f2u(as_float(rs[XRS_POINTSIZE]) * (rs[XRS_POINTSCALEENABLE] ? 1.0f : k)),
                            f2u(as_float(rs[XRS_POINTSIZE_MIN]) * k), f2u(as_float(rs[XRS_POINTSIZE_MAX]) * k),
                            rs[XRS_POINTSCALE_A], rs[XRS_POINTSCALE_B], rs[XRS_POINTSCALE_C]};
        for (int i = 0; i < 8; i++)
            if (!g_point_valid || g_point_sent[i] != v[i]) set(kPoint[i], g_point_sent[i] = v[i]);
        g_point_valid = true;
    }
}

// Census (docs/07 #16, #17): each value the game gives a texture stage state that is passed on unconverted or not
// emulated, once, with the stage's texture format and whether a pixel shader draws, to know what needs doing.
void ts_census(const char *name, int s, uint32_t v)
{
    static std::vector<std::pair<const char *, uint32_t>> seen;
    if (seen.size() >= 64 || std::find(seen.begin(), seen.end(), std::make_pair(name, v)) != seen.end()) return;
    seen.push_back({name, v});
    Resource *r = g_st.textures[s];
    uint32_t fmt = r && resource_type(r) == RTYPE_TEXTURE ? (((PixelContainer *)r)->Format >> FMT_FORMAT_SHIFT) & 0xFF : 0;
    logf("[draw] frame %u: stage %d %s = 0x%x (texture format 0x%x, %s)", g_frame_count.load(), s, name, v, fmt,
         g_st.pixel_shader ? "pixel shader" : "fixed function");
}

template <typename F> void ts_on_change(int s, int t, F apply)
{
    uint32_t v = texture_state(s)[t];
    if (g_ts_valid[s][t] && g_ts_sent[s][t] == v) return;
    g_ts_valid[s][t] = true;
    g_ts_sent[s][t] = v;
    apply(v);
}

// A 1x1 opaque white texture for fixed-function stages that use D3DTA_TEXTURE without a texture bound: that reads
// as (1,1,1,1) on the Xbox (Ninja Gaiden Black's nunchaku sweep modulates its alpha by an unset stage 1 and is
// drawn alpha-tested; D3D9 samples an unbound stage as (0,0,0,0) and the sweep vanished, docs/04 §3.9).
host3d::CpuTexture g_white;

IDirect3DTexture9 *white_texture()
{
    host3d::CpuTexture &t = g_white;
    if (t.tex) return t.tex;
    if (!t.make(1, 1)) return nullptr;
    int pitch;
    if (auto *p = (uint32_t *)t.lock(&pitch)) {
        *p = 0xFFFFFFFFu;
        t.unlock();
    }
    return t.tex;
}

bool stage_reads_texture(const uint32_t *ts)
{
    if (ts[12] == 1 && ts[16] == 1) return false; // both ops DISABLE
    for (int a : {13, 14, 15, 17, 18, 19})
        if ((ts[a] & 0xF) == 2) return true; // D3DTA_TEXTURE (with its modifier flags masked)
    return false;
}

// Stage 0 of the draw being set up samples something the game drew (a render target, swizzled or not), not a picture
// it loaded: scale_rhw tells screen copies from the game's art by it
bool g_stage0_rendered;

// Census of the glow's other kind of trick: one draw sampling the same render target in several stages (a tap
// filter: blur, glow, depth of field), each kind (sizes, caller) logged once, with the target it is drawn into
bool g_rt_stage[4]; // which stages of the draw being set up sample a render target (apply_texture_stages)
int first_game_callers(const uint32_t *sp, uint32_t *out, int max); // below
std::string game_callers(const uint32_t *sp);

void note_tap_filter(const bool *rt_stage)
{
    int first = -1, taps = 0;
    for (int s = 0; s < 4; s++)
        if (rt_stage[s] && g_st.textures[s]) {
            if (first < 0) first = s;
            if (g_st.textures[s]->Data == g_st.textures[first]->Data) taps++;
        }
    if (taps < 2) return;
    uint32_t sw = 0, sh = 0, tw = 0, th = 0, dd, l;
    pixel_container_dims((PixelContainer *)g_st.textures[first], &sw, &sh, &dd, &l);
    if (g_st.render_target) pixel_container_dims((PixelContainer *)g_st.render_target, &tw, &th, &dd, &l);
    uint32_t caller = 0;
    first_game_callers((const uint32_t *)_AddressOfReturnAddress(), &caller, 1);
    static std::vector<uint64_t> seen;
    uint64_t k = ((uint64_t)caller << 32) ^ ((uint64_t)sw << 20) ^ ((uint64_t)sh << 8) ^ tw ^ ((uint64_t)th << 44) ^ taps;
    if (seen.size() >= 64 || std::find(seen.begin(), seen.end(), k) != seen.end()) return;
    seen.push_back(k);
    logf("[census] frame %u: a %d-tap filter reads a %ux%u target (data 0x%08x) into %ux%u%s, from %s", g_frame_count.load(), taps,
         sw, sh, g_st.textures[first]->Data, tw, th, is_backbuffer(g_st.render_target) ? " (the picture)" : "",
         game_callers((const uint32_t *)_AddressOfReturnAddress()).c_str());
}

void apply_texture_stages(IDirect3DDevice9 *d, bool fixed_function_pixels)
{
    bool rt_stage[4] = {};
    g_ffp_edge_stages = 0;
    g_stage0_rendered = false;
    for (bool &b : g_rt_stage) b = false;
    for (int s = 0; s < 4; s++) {
        IDirect3DBaseTexture9 *t = nullptr;
        if (!g_st.textures[s] && fixed_function_pixels && stage_reads_texture(texture_state(s))) t = white_texture();
        if (Resource *r = g_st.textures[s]) {
            const uint32_t *ts = texture_state(s); // MAGFILTER 3, MINFILTER 4: 2 = LINEAR and up
            t = rt_texture_for((const PixelContainer *)r, g_draw_serial, ts[3] >= 2 || ts[4] >= 2);
            rt_stage[s] = t != nullptr;
            if (s == 0) g_stage0_rendered = rt_stage[0];
            g_rt_stage[s] = rt_stage[s];
            if (debug_frame_active() && (r->Data == backbuffer_data() || r->Data == depthbuffer_data() || t)) {
                uint32_t w, h, dd, l;
                pixel_container_dims((PixelContainer *)r, &w, &h, &dd, &l);
                logf("[rt] frame %u: stage %d samples %s (texture %p data 0x%08x %ux%u fmt 0x%x)", g_frame_count.load(), s,
                     r->Data == backbuffer_data() ? "the BACK BUFFER" : r->Data == depthbuffer_data() ? "the DEPTH BUFFER" : "a render target",
                     r, r->Data, w, h, (((PixelContainer *)r)->Format >> FMT_FORMAT_SHIFT) & 0xFF);
            }
            if (!t) t = host_texture(r, g_st.palettes[s]);
        }
        if (t != g_tex_sent[s]) {
            d->SetTexture(s, t);
            g_tex_sent[s] = t;
        }
        ts_on_change(s, 0, [&](uint32_t v) { d->SetSamplerState(s, D3DSAMP_ADDRESSU, conv_address(v)); });
        ts_on_change(s, 1, [&](uint32_t v) { d->SetSamplerState(s, D3DSAMP_ADDRESSV, conv_address(v)); });
        ts_on_change(s, 2, [&](uint32_t v) { d->SetSamplerState(s, D3DSAMP_ADDRESSW, conv_address(v)); });
        // A fix of the game, not emulation: the lava heat haze (FUN_00236010+0x130c: sprites, colour = stage 0, a
        // 512x256 copy of the screen read through projected camera-space coordinates; alpha = stage 1's 128x128 DXT5
        // mask x TFACTOR; blended SRCALPHA/INVSRCALPHA) reads a little past the copy's right edge. Clamped, that edge
        // column ran down the right of the screen as a band (black on the Xbox, where the column is never drawn;
        // smeared here once it is; mirrored, a visible mirror; transparent black border, a black band since the alpha
        // is not the copy's: the user, 2026-10-20/22). Such a stage takes the pixel's alpha to 0 where its coordinates
        // leave 0..1 (ffp_ps.cpp): the sprite is not drawn there and the screen shows as it is, like everywhere else.
        // Fixed-function draws and colour targets only (a shadow map is a projected render target read too).
        // cfg:edgemirror:0 clamps.
        {
            const uint32_t *ts = texture_state(s);
            uint32_t fmt = rt_stage[s] ? (((const PixelContainer *)g_st.textures[s])->Format >> FMT_FORMAT_SHIFT) & 0xFF : 0;
            if (g_cfg.edge_mirror && fixed_function_pixels && rt_stage[s] && !(fmt >= 0x2A && fmt <= 0x31) &&
                (ts[21] & D3DTTFF_PROJECTED) && ts[0] == 3 && ts[1] == 3)
                g_ffp_edge_stages |= 1u << s;
        }
        ts_on_change(s, 3, [&](uint32_t v) { d->SetSamplerState(s, D3DSAMP_MAGFILTER, v > 3 ? D3DTEXF_LINEAR : v); });
        // Anisotropic filtering (black.ini aniso, an addition: the game asks for LINEAR): where the game filters linearly
        // when minifying, anisotropically up to g_cfg.aniso; point sampling stays as the game set it
        ts_on_change(s, 4, [&](uint32_t v) {
            DWORD f = v > 3 ? D3DTEXF_LINEAR : v;
            if (g_cfg.aniso > 1 && (f == D3DTEXF_LINEAR || f == D3DTEXF_ANISOTROPIC)) f = D3DTEXF_ANISOTROPIC;
            d->SetSamplerState(s, D3DSAMP_MINFILTER, f);
        });
        ts_on_change(s, 5, [&](uint32_t v) { d->SetSamplerState(s, D3DSAMP_MIPFILTER, v > 2 ? D3DTEXF_LINEAR : v); });
        // LOD bias: Ninja Gaiden Black writes 0xFFFFFFFF (a NaN) into stage 0's for the water's ripple map (FUN_00129b60
        // at 0x12A4CD, chapter 2's pond). The console's D3D gives the NV2A field (signed 5.8 fixed point) 0x1000 for it,
        // -16: xemu's register model shows TEXFILTER0 E2063000 for the pond (ngbrecomp #16). The sampler then always
        // reads the top level, the only one the game fills; with a bias of 0 (2.2.17 and before) the far water at low
        // render scales read the other levels' leftover data, in bands. NaN -> -16; other non-finite values -> 0 as
        // before; finite ones clamped to the field. The host driver's treatment of a NaN is undefined.
        ts_on_change(s, 6, [&](uint32_t v) {
            float b = as_float(v);
            b = std::isnan(b) ? -16.0f : std::isfinite(b) ? std::min(std::max(b, -16.0f), 15.99f) : 0.0f;
            d->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, f2u(b));
        });
        ts_on_change(s, 7, [&](uint32_t v) { d->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, v); });
        ts_on_change(s, 8, [&](uint32_t v) { d->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, std::max<DWORD>(v ? v : 1, g_cfg.aniso)); });
        ts_on_change(s, 29, [&](uint32_t v) { d->SetSamplerState(s, D3DSAMP_BORDERCOLOR, v); });
        // Texture coordinate source and transform belong to the vertex pipeline: they apply to pixel-shader draws with
        // fixed-function vertices too (the glow / blur chain), not only to fixed-function pixels.
        ts_on_change(s, 21, [&](uint32_t v) { set_ttff(d, s, v); });
        ts_on_change(s, 28, [&](uint32_t v) {
            if (v > 7) ts_census("TEXCOORDINDEX", s, v); // a generated coordinate: Xbox and D3D9 numbers differ above 0x30000
            d->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, v);
        });
        ts_on_change(s, 9, [&](uint32_t v) { if (v) ts_census("COLORKEYOP", s, v); });  // not emulated
        ts_on_change(s, 10, [&](uint32_t v) { if (v) ts_census("COLORSIGN", s, v); });  // not emulated
        if (!fixed_function_pixels) continue;
        ts_on_change(s, 12, [&](uint32_t v) { d->SetTextureStageState(s, D3DTSS_COLOROP, conv_texop(v)); });
        ts_on_change(s, 13, [&](uint32_t v) { d->SetTextureStageState(s, D3DTSS_COLORARG0, v); });
        ts_on_change(s, 14, [&](uint32_t v) { d->SetTextureStageState(s, D3DTSS_COLORARG1, v); });
        ts_on_change(s, 15, [&](uint32_t v) { d->SetTextureStageState(s, D3DTSS_COLORARG2, v); });
        ts_on_change(s, 16, [&](uint32_t v) { d->SetTextureStageState(s, D3DTSS_ALPHAOP, conv_texop(v)); });
        ts_on_change(s, 17, [&](uint32_t v) { d->SetTextureStageState(s, D3DTSS_ALPHAARG0, v); });
        ts_on_change(s, 18, [&](uint32_t v) { d->SetTextureStageState(s, D3DTSS_ALPHAARG1, v); });
        ts_on_change(s, 19, [&](uint32_t v) { d->SetTextureStageState(s, D3DTSS_ALPHAARG2, v); });
        ts_on_change(s, 20, [&](uint32_t v) { d->SetTextureStageState(s, D3DTSS_RESULTARG, v); });
    }
}

// A pixel shader that failed to translate: approximate with "texture stage 0 modulated by diffuse".
void apply_pixel_shader_fallback(IDirect3DDevice9 *d)
{
    static bool noted;
    if (!noted) {
        noted = true;
        logf("[d3d] note: untranslatable pixel shader drawn as texture x diffuse");
    }
    bool tex = g_tex_sent[0] != nullptr;
    d->SetTextureStageState(0, D3DTSS_COLOROP, tex ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, tex ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    // the stage-state cache no longer matches what the device has
    for (int s = 0; s < 2; s++)
        for (int t = 12; t < 21; t++) g_ts_valid[s][t] = false;
}

uint32_t fvf_stride(uint32_t fvf)
{
    uint32_t n = 0;
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ: n = 12; break;
    case D3DFVF_XYZRHW: n = 16; break;
    case D3DFVF_XYZB1: n = 16; break;
    case D3DFVF_XYZB2: n = 20; break;
    case D3DFVF_XYZB3: n = 24; break;
    case D3DFVF_XYZB4: n = 28; break;
    case D3DFVF_XYZB5: n = 32; break;
    }
    if (fvf & D3DFVF_NORMAL) n += 12;
    if (fvf & D3DFVF_PSIZE) n += 4;
    if (fvf & D3DFVF_DIFFUSE) n += 4;
    if (fvf & D3DFVF_SPECULAR) n += 4;
    uint32_t tc = (fvf >> 8) & 0xF;
    for (uint32_t i = 0; i < tc; i++) {
        static const uint32_t sizes[4] = {8, 12, 16, 4};
        n += sizes[(fvf >> (16 + 2 * i)) & 3];
    }
    return n;
}

struct Prim {
    D3DPRIMITIVETYPE type;
    uint32_t count;     // primitives
    bool quads;         // QUADLIST: needs index expansion
};

Prim conv_prim(uint32_t xprim, uint32_t verts)
{
    switch (xprim) {
    case XPT_POINTLIST: return {D3DPT_POINTLIST, verts, false};
    case XPT_LINELIST: return {D3DPT_LINELIST, verts / 2, false};
    case XPT_LINELOOP: case XPT_LINESTRIP: return {D3DPT_LINESTRIP, verts > 1 ? verts - 1 : 0, false};
    case XPT_TRIANGLELIST: return {D3DPT_TRIANGLELIST, verts / 3, false};
    case XPT_TRIANGLESTRIP: case XPT_QUADSTRIP: return {D3DPT_TRIANGLESTRIP, verts > 2 ? verts - 2 : 0, false};
    case XPT_TRIANGLEFAN: case XPT_POLYGON: return {D3DPT_TRIANGLEFAN, verts > 2 ? verts - 2 : 0, false};
    case XPT_QUADLIST: return {D3DPT_TRIANGLELIST, verts / 4 * 2, true};
    default: return {D3DPT_TRIANGLELIST, 0, false};
    }
}

std::vector<uint16_t> g_quad_indices;
std::vector<uint16_t> g_tmp_indices;

// Indexed draws through dynamic ring buffers instead of DrawIndexedPrimitiveUP: D3D9On12 (the Iris Xe here) makes
// an allocation and a copy for every UP call, ~6 ms of a 13 ms chapter-1 frame in r121. The vertices and indices are
// appended with D3DLOCK_NOOVERWRITE and the buffers start over with D3DLOCK_DISCARD when full.
constexpr uint32_t kRingVB = 16u << 20, kRingIB = 4u << 20;
IDirect3DVertexBuffer9 *g_ring_vb;
IDirect3DIndexBuffer9 *g_ring_ib;
uint32_t g_ring_vpos, g_ring_ipos;
bool g_ring_failed;

bool ring_draw(D3DPRIMITIVETYPE type, uint32_t nverts, uint32_t prims, const uint16_t *idx, uint32_t nidx, const void *src,
               uint32_t stride)
{
    IDirect3DDevice9 *d = host3d::dev();
    if (!g_ring_vb) {
        if (g_ring_failed) return false;
        if (FAILED(d->CreateVertexBuffer(kRingVB, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_ring_vb, nullptr)) ||
            FAILED(d->CreateIndexBuffer(kRingIB, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &g_ring_ib,
                                        nullptr))) {
            logf("[draw] dynamic ring buffers not available: indexed draws stay on DrawIndexedPrimitiveUP");
            g_ring_failed = true;
            release_draw_buffers();
            return false;
        }
        g_ring_vpos = g_ring_ipos = 0;
        logf("[draw] indexed draws through dynamic ring buffers (%u KB vertices, %u KB indices)", kRingVB >> 10, kRingIB >> 10);
    }
    uint32_t vbytes = nverts * stride, ibytes = nidx * 2;
    if (!stride || vbytes > kRingVB || ibytes > kRingIB) return false;
    uint32_t voff = (g_ring_vpos + stride - 1) / stride * stride;
    DWORD vflag = D3DLOCK_NOOVERWRITE;
    if (voff + vbytes > kRingVB) {
        voff = 0;
        vflag = D3DLOCK_DISCARD;
    }
    uint32_t ioff = g_ring_ipos;
    DWORD iflag = D3DLOCK_NOOVERWRITE;
    if (ioff + ibytes > kRingIB) {
        ioff = 0;
        iflag = D3DLOCK_DISCARD;
    }
    void *p = nullptr;
    if (FAILED(g_ring_vb->Lock(voff, vbytes, &p, vflag))) return false;
    memcpy(p, src, vbytes);
    g_ring_vb->Unlock();
    if (FAILED(g_ring_ib->Lock(ioff, ibytes, &p, iflag))) return false;
    memcpy(p, idx, ibytes);
    g_ring_ib->Unlock();
    g_ring_vpos = voff + vbytes;
    g_ring_ipos = ioff + ibytes;
    d->SetStreamSource(0, g_ring_vb, 0, stride);
    d->SetIndices(g_ring_ib);
    return SUCCEEDED(d->DrawIndexedPrimitive(type, (INT)(voff / stride), 0, nverts, ioff / 2, prims));
}

const uint16_t *quad_indices(uint32_t quads)
{
    if (g_quad_indices.size() < quads * 6) {
        g_quad_indices.resize(quads * 6);
        for (uint32_t q = 0; q < quads; q++) {
            uint16_t b = (uint16_t)(q * 4);
            uint16_t *p = &g_quad_indices[q * 6];
            p[0] = b; p[1] = b + 1; p[2] = b + 2; p[3] = b; p[4] = b + 2; p[5] = b + 3;
        }
    }
    return g_quad_indices.data();
}

// ---- programmable vertex path: Xbox elements -> one float4 per register ----

// Decode one Xbox D3DVSDT_* element to float4 (missing components default to 0,0,0,1).
void decode_element(uint8_t t, const uint8_t *p, float *o)
{
    o[0] = o[1] = o[2] = 0.0f;
    o[3] = 1.0f;
    uint32_t n = t >> 4;
    auto sext = [](uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); };
    switch (t & 0xF) {
    case 0x2: // FLOATn (0x72 = FLOAT2H: x, y, w)
        if (t == 0x72) {
            memcpy(o, p, 8);
            memcpy(&o[3], p + 8, 4);
        } else if (n >= 1 && n <= 4) {
            memcpy(o, p, n * 4);
        }
        break;
    case 0x0: // D3DCOLOR (ARGB dword) -> RGBA
        if (t == 0x40) {
            uint32_t c;
            memcpy(&c, p, 4);
            o[0] = ((c >> 16) & 255) / 255.0f;
            o[1] = ((c >> 8) & 255) / 255.0f;
            o[2] = (c & 255) / 255.0f;
            o[3] = (c >> 24) / 255.0f;
        }
        break;
    case 0x5: // SHORTn
        for (uint32_t i = 0; i < n; i++) o[i] = (float)((const int16_t *)p)[i];
        break;
    case 0x1: // NORMSHORTn
        for (uint32_t i = 0; i < n; i++) o[i] = ((const int16_t *)p)[i] / 32767.0f;
        break;
    case 0x4: // PBYTEn (unsigned normalized)
        for (uint32_t i = 0; i < n; i++) o[i] = p[i] / 255.0f;
        break;
    case 0x6: { // NORMPACKED3: 11:11:10 signed normalized
        uint32_t v;
        memcpy(&v, p, 4);
        o[0] = sext(v & 0x7FF, 11) / 1023.0f;
        o[1] = sext((v >> 11) & 0x7FF, 11) / 1023.0f;
        o[2] = sext(v >> 22, 10) / 511.0f;
        break;
    }
    }
}

// Decode `count` elements of one type, `sstride` bytes apart, into float4s `ostride` floats apart.
void decode_run(uint8_t t, const uint8_t *p, uint32_t sstride, float *o, uint32_t ostride, uint32_t count)
{
    switch (t) {
    case 0x12: // FLOAT1
        for (uint32_t v = 0; v < count; v++, p += sstride, o += ostride) {
            o[0] = *(const float *)p;
            o[1] = o[2] = 0.0f;
            o[3] = 1.0f;
        }
        return;
    case 0x22: // FLOAT2
        for (uint32_t v = 0; v < count; v++, p += sstride, o += ostride) {
            const float *f = (const float *)p;
            o[0] = f[0];
            o[1] = f[1];
            o[2] = 0.0f;
            o[3] = 1.0f;
        }
        return;
    case 0x32: // FLOAT3
        for (uint32_t v = 0; v < count; v++, p += sstride, o += ostride) {
            const float *f = (const float *)p;
            o[0] = f[0];
            o[1] = f[1];
            o[2] = f[2];
            o[3] = 1.0f;
        }
        return;
    case 0x42: // FLOAT4
        for (uint32_t v = 0; v < count; v++, p += sstride, o += ostride) {
            const float *f = (const float *)p;
            o[0] = f[0];
            o[1] = f[1];
            o[2] = f[2];
            o[3] = f[3];
        }
        return;
    case 0x40: // D3DCOLOR (ARGB) -> RGBA
        for (uint32_t v = 0; v < count; v++, p += sstride, o += ostride) {
            uint32_t c = *(const uint32_t *)p;
            constexpr float k = 1.0f / 255.0f;
            o[0] = ((c >> 16) & 255) * k;
            o[1] = ((c >> 8) & 255) * k;
            o[2] = (c & 255) * k;
            o[3] = (c >> 24) * k;
        }
        return;
    case 0x16: // NORMPACKED3: 11:11:10 signed normalized
        for (uint32_t v = 0; v < count; v++, p += sstride, o += ostride) {
            uint32_t c = *(const uint32_t *)p;
            o[0] = ((int32_t)(c << 21) >> 21) * (1.0f / 1023.0f);
            o[1] = ((int32_t)(c << 10) >> 21) * (1.0f / 1023.0f);
            o[2] = ((int32_t)c >> 22) * (1.0f / 511.0f);
            o[3] = 1.0f;
        }
        return;
    default:
        for (uint32_t v = 0; v < count; v++, p += sstride, o += ostride) decode_element(t, p, o);
    }
}

std::vector<float> g_conv; // only grows: a resize per draw zero-filled it every time
std::unordered_map<const VertexShader *, IDirect3DVertexDeclaration9 *> g_decls;
IDirect3DVertexDeclaration9 *g_decl_imm16; // immediate mode: all 16 attributes

// A deleted shader's declaration (vs_delete): its address may be given to another shader with other inputs
void forget_decl(const VertexShader *vs)
{
    auto it = g_decls.find(vs);
    if (it == g_decls.end()) return;
    if (it->second) it->second->Release();
    g_decls.erase(it);
}

IDirect3DVertexDeclaration9 *decl_for(const VertexShader *vs)
{
    auto it = g_decls.find(vs);
    if (it != g_decls.end()) return it->second;
    D3DVERTEXELEMENT9 el[33];
    uint32_t n = 0, seen = 0;
    for (uint32_t i = 0; i < vs->num_elements; i++) {
        uint8_t reg = vs->elements[i].reg;
        if (seen & (1u << reg)) continue;
        seen |= 1u << reg;
        el[n] = {0, (WORD)(16 * i), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, reg};
        n++;
    }
    el[n] = D3DDECL_END();
    IDirect3DVertexDeclaration9 *d = nullptr;
    if (FAILED(host3d::dev()->CreateVertexDeclaration(el, &d))) logf("[d3d] CreateVertexDeclaration failed for shader %p", vs);
    g_decls[vs] = d;
    return d;
}

IDirect3DVertexDeclaration9 *decl_imm16()
{
    if (!g_decl_imm16) {
        D3DVERTEXELEMENT9 el[17];
        for (BYTE i = 0; i < 16; i++) el[i] = {0, (WORD)(16 * i), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, i};
        el[16] = D3DDECL_END();
        host3d::dev()->CreateVertexDeclaration(el, &g_decl_imm16);
    }
    return g_decl_imm16;
}

// Immediate-mode vertices hold all 16 attribute slots as float4 (256 bytes), but D3D9 vertex strides go up to
// 255: Intel's driver overran its vertex buffer with 256 (a laptop crashed at the start of the first level in
// DrawPrimitiveUP). Draws send only the slots the game wrote or the shader declares, with a declaration per set.
std::unordered_map<uint32_t, IDirect3DVertexDeclaration9 *> g_decl_imm;
std::vector<float> g_imm_pack;

IDirect3DVertexDeclaration9 *decl_imm(uint32_t mask)
{
    IDirect3DVertexDeclaration9 *&d = g_decl_imm[mask];
    if (!d) {
        D3DVERTEXELEMENT9 el[17];
        BYTE n = 0;
        for (BYTE i = 0; i < 16; i++)
            if (mask & (1u << i)) {
                el[n] = {0, (WORD)(16 * n), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, i};
                n++;
            }
        el[n] = D3DDECL_END();
        if (FAILED(host3d::dev()->CreateVertexDeclaration(el, &d))) logf("[d3d] CreateVertexDeclaration failed for immediate mask 0x%x", mask);
    }
    return d;
}

const uint8_t *stream_data(uint32_t stream);

// Convert vertices [first, first+count) of the current streams (or user data for stream 0) for `vs`.
// Returns the stride of the converted vertices (16 bytes per declaration element).
uint32_t convert_vertices(const VertexShader *vs, uint32_t first, uint32_t count, const uint8_t *up, uint32_t up_stride)
{
    perf::Scope perf_scope(perf::kConvert);
    uint32_t ne = vs->num_elements, stride = 16 * ne;
    if (g_conv.size() < (size_t)count * ne * 4) g_conv.resize((size_t)count * ne * 4);
    for (uint32_t e = 0; e < ne; e++) {
        const VertexElement &el = vs->elements[e];
        const uint8_t *base;
        uint32_t s_stride;
        if (up && el.stream == 0) {
            base = up;
            s_stride = up_stride;
        } else {
            base = stream_data(el.stream);
            s_stride = g_st.streams[el.stream].stride ? g_st.streams[el.stream].stride : vs->stream_stride[el.stream];
        }
        float *o = &g_conv[e * 4];
        if (!base) {
            for (uint32_t v = 0; v < count; v++, o += ne * 4) { o[0] = o[1] = o[2] = 0; o[3] = 1; }
            continue;
        }
        decode_run(el.xtype, base + (size_t)first * s_stride + el.offset, s_stride, o, ne * 4, count);
    }
    return stride;
}

// A vertex shader with a declaration and no program is the Xbox's fixed-function pipeline fed through that
// declaration: inputs no FVF code describes, such as a packed normal (the Xbox Live dashboard's selected item, which
// went undrawn: the highlight showed black). Its vertices go to the host's fixed-function pipeline converted to the
// FVF layout that holds the same inputs. Registers: 0 position, 2 normal, 3 diffuse, 4 specular, 9-12 texture
// coordinates 0-3. 0 when the declaration has anything else (blend weights, fog, point size, back colours) or no
// 3-float position: the draw stays undrawn, as before.
uint32_t decl_fvf(const VertexShader *vs)
{
    uint32_t fvf = 0, ntex = 0, texsz[4] = {2, 2, 2, 2};
    for (uint32_t i = 0; i < vs->num_elements; i++) {
        uint8_t reg = vs->elements[i].reg, t = vs->elements[i].xtype;
        if (reg == 0 && t == 0x32) fvf |= D3DFVF_XYZ;
        else if (reg == 2) fvf |= D3DFVF_NORMAL;
        else if (reg == 3) fvf |= D3DFVF_DIFFUSE;
        else if (reg == 4) fvf |= D3DFVF_SPECULAR;
        else if (reg >= 9 && reg <= 12) {
            uint32_t s = reg - 9, n = t == 0x40 || t == 0x72 ? 4 : t >> 4;
            texsz[s] = n < 1 ? 1 : n > 4 ? 4 : n;
            if (s + 1 > ntex) ntex = s + 1;
        } else return 0;
    }
    if (!(fvf & D3DFVF_XYZ)) return 0;
    fvf |= ntex << D3DFVF_TEXCOUNT_SHIFT;
    for (uint32_t s = 0; s < ntex; s++)
        fvf |= texsz[s] == 1 ? D3DFVF_TEXCOORDSIZE1(s) : texsz[s] == 3 ? D3DFVF_TEXCOORDSIZE3(s)
             : texsz[s] == 4 ? D3DFVF_TEXCOORDSIZE4(s) : D3DFVF_TEXCOORDSIZE2(s);
    return fvf;
}

std::vector<uint8_t> g_declconv; // only grows
std::vector<float> g_declconv_el;

// Vertices [first, first+count) of the current streams (or user data for stream 0) for decl_fvf(vs) == fvf, in
// g_declconv. Returns the stride.
uint32_t convert_decl_vertices(const VertexShader *vs, uint32_t fvf, uint32_t first, uint32_t count, const uint8_t *up,
                               uint32_t up_stride)
{
    perf::Scope perf_scope(perf::kConvert);
    uint32_t ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT, texsz[4] = {}, texoff[4] = {};
    uint32_t off_n = 12, off_d = off_n + (fvf & D3DFVF_NORMAL ? 12 : 0), off_s = off_d + (fvf & D3DFVF_DIFFUSE ? 4 : 0);
    uint32_t stride = off_s + (fvf & D3DFVF_SPECULAR ? 4 : 0);
    for (uint32_t s = 0; s < ntex; s++) {
        uint32_t code = (fvf >> (16 + 2 * s)) & 3; // 0 two, 1 three, 2 four, 3 one
        texsz[s] = code == 0 ? 2 : code == 1 ? 3 : code == 2 ? 4 : 1;
        texoff[s] = stride;
        stride += texsz[s] * 4;
    }
    if (g_declconv.size() < (size_t)count * stride) g_declconv.resize((size_t)count * stride);
    if (g_declconv_el.size() < (size_t)count * 4) g_declconv_el.resize((size_t)count * 4);
    memset(g_declconv.data(), 0, (size_t)count * stride); // texture coordinate sets the declaration skips
    for (uint32_t e = 0; e < vs->num_elements; e++) {
        const VertexElement &el = vs->elements[e];
        uint32_t off, n;
        bool color = false;
        if (el.reg == 0) off = 0, n = 3;
        else if (el.reg == 2) off = off_n, n = 3;
        else if (el.reg == 3) off = off_d, n = 4, color = true;
        else if (el.reg == 4) off = off_s, n = 4, color = true;
        else off = texoff[el.reg - 9], n = texsz[el.reg - 9];
        const uint8_t *base;
        uint32_t s_stride;
        if (up && el.stream == 0) {
            base = up;
            s_stride = up_stride;
        } else {
            base = stream_data(el.stream);
            s_stride = g_st.streams[el.stream].stride ? g_st.streams[el.stream].stride : vs->stream_stride[el.stream];
        }
        float *f = g_declconv_el.data();
        if (base) decode_run(el.xtype, base + (size_t)first * s_stride + el.offset, s_stride, f, 4, count);
        else
            for (uint32_t v = 0; v < count; v++) f[v * 4] = f[v * 4 + 1] = f[v * 4 + 2] = 0, f[v * 4 + 3] = 1;
        uint8_t *o = g_declconv.data() + off;
        for (uint32_t v = 0; v < count; v++, o += stride, f += 4) {
            if (color) {
                auto c8 = [](float x) { return (uint32_t)(std::min(std::max(x, 0.0f), 1.0f) * 255.0f + 0.5f); };
                uint32_t c = c8(f[3]) << 24 | c8(f[0]) << 16 | c8(f[1]) << 8 | c8(f[2]); // RGBA -> ARGB
                memcpy(o, &c, 4);
            } else {
                memcpy(o, f, n * 4);
            }
        }
    }
    return stride;
}

} // namespace

// Linear (pitch) textures are addressed with texel coordinates on the NV2A; D3D9 wants 0..1.
void linear_texture_scale(int stage, float *sx, float *sy)
{
    *sx = *sy = 1.0f;
    Resource *r = g_st.textures[stage];
    if (!r) return;
    auto *pc = (PixelContainer *)r;
    if (resource_type(r) != RTYPE_TEXTURE && resource_type(r) != RTYPE_SURFACE) return;
    if (!pc->Size) return; // swizzled: normalized coordinates
    uint32_t w, h, dd, l;
    pixel_container_dims(pc, &w, &h, &dd, &l);
    *sx = 1.0f / (float)w;
    *sy = 1.0f / (float)h;
}

namespace {

// All 200 registers on every draw (~1.3 ms a frame in busy scenes). Sending only the registers that changed
// since the last upload broke the picture: the device then held values that were never sent to those
// registers (seen with GetVertexShaderConstantF), not explained by the shaders' def constants; not solved yet.
void upload_vs_constants(IDirect3DDevice9 *d)
{
    d->SetVertexShaderConstantF(0, &g_st.vs_constants[0][0], 192);
    float k[36] = {};
    host_viewport_transform(k, k + 4);
    for (int s = 0; s < 4; s++) {
        linear_texture_scale(s, &k[8 + s * 4], &k[9 + s * 4]);
        k[10 + s * 4] = k[11 + s * 4] = 1.0f;
    }
    // c198 the NV2A fog parameters (NV097_SET_FOG_PARAMS 0, 1), c199 the fog mode (linear, exp, exp2), worked out as the
    // library does (FUN_003D0EC0, x2.1; its x87 runs at 24-bit precision, the game's _controlfp: float arithmetic):
    // none -> linear with 1, 1 (the factor is oFog itself); linear -> k = 1 / (end - start), or 8192 when start = end,
    // p0 = end k + 1, p1 = -k; exp / exp2 -> p0 = 1.5, p1 = density x -0.090168074 / -0.21233003 (0x415778 / 0x415774).
    const uint32_t *rs = render_state();
    float fs, fe, fd;
    memcpy(&fs, &rs[XRS_FOGSTART], 4);
    memcpy(&fe, &rs[XRS_FOGEND], 4);
    memcpy(&fd, &rs[XRS_FOGDENSITY], 4);
    switch (rs[XRS_FOGTABLEMODE]) {
    case D3DFOG_LINEAR: {
        float kl = fs == fe ? 8192.0f : 1.0f / (fe - fs);
        k[24] = fe * kl + 1.0f, k[25] = -kl, k[28] = 1.0f;
        break;
    }
    case D3DFOG_EXP: k[24] = 1.5f, k[25] = fd * -0.0901680737733841f, k[29] = 1.0f; break;
    case D3DFOG_EXP2: k[24] = 1.5f, k[25] = fd * -0.21233002841472626f, k[30] = 1.0f; break;
    default: k[24] = k[25] = 1.0f, k[28] = 1.0f; break;
    }
    k[32] = k[33] = fov_viewmodel_scale(); // c200: the first-person gun keeps the game's FOV
    k[34] = g_cfg.vs_snap ? 1.0f : 0.0f; // c200.z: the 1/8-pixel snap (cfg:vsnap)
    k[35] = 1.0f;
    d->SetVertexShaderConstantF(192, k, 9);
}

// Fixed-function path: express the same scaling as a texture transform (when the game sets none).
// Pre-transformed (XYZRHW) vertices skip the host's vertex pipeline, texture transforms included, so for them the
// scale goes into the vertex data instead (scale_rhw) and the stage transform is off.
void apply_fvf_texture_scale(IDirect3DDevice9 *d, bool rhw)
{
    for (int s = 0; s < 4; s++) {
        if (rhw) {
            set_ttff(d, s, D3DTTFF_DISABLE);
            g_ts_valid[s][21] = false; // the game's flags are sent again on the next draw that uses them
            continue;
        }
        float sx, sy;
        linear_texture_scale(s, &sx, &sy);
        bool scaled = sx != 1.0f || sy != 1.0f;
        uint32_t flags = texture_state(s)[21];
        if (flags != 0) {
            // The game's own texture transform is in effect. A linear texture is still addressed in texels on the
            // NV2A, so the texel-to-0..1 scale has to follow the game's matrix: uv' = (uv * M) * S. Ninja Gaiden
            // Black keeps an identity TEXTURE0 transform set while it copies the back buffer (720x480 linear) into
            // its menu / dialog snapshot with texel coordinates 0..720 x 0..480; without the scale the whole quad
            // sampled the clamped edge, one flat colour (docs/04 §3.7).
            if (!scaled) {
                // The game's matrix as it is: a previous draw may have left M * S or the texel scale on the host
                // (docs/07 #1). Counted when the host held something else (--api-count log).
                const D3DMATRIX &gm = g_st.transforms[2 + s];
                if (g_texm_valid[s] && memcmp(&g_texm_sent[s], &gm, sizeof gm)) g_texm_stale++;
                set_tex_transform(d, s, gm);
                continue;
            }
            D3DMATRIX m = g_st.transforms[2 + s];
            for (int r = 0; r < 4; r++) {
                m.m[r][0] *= sx;
                m.m[r][1] *= sy;
            }
            set_tex_transform(d, s, m);
            continue;
        }
        D3DMATRIX m = {};
        m._11 = sx;
        m._22 = sy;
        m._33 = m._44 = 1.0f;
        set_tex_transform(d, s, m);
        set_ttff(d, s, scaled ? D3DTTFF_COUNT2 : D3DTTFF_DISABLE);
    }
}

enum class Path { None, Fvf, Prog };
struct DrawCtx {
    Path path = Path::None;
    uint32_t fvf = 0;
    VertexShader *vs = nullptr;
};

// --debug-draws N: describe the first N draws (vertex data, constants, key states).
std::atomic<int> g_debug_draws{-1};

// The game code above a draw: return addresses into the XBE's game code found on the stack (values right after a
// call instruction), nearest first. A heuristic, like the profiler's: stale values happen.
// End of the game's own code: the D3D section's start (BLACK 0x269000, Ninja Gaiden Black x2.1 0x3C5100).
static uint32_t game_code_end()
{
    if (const Xbe *x = image_xbe())
        for (const XbeSection &s : x->sections)
            if (s.name == "D3D") return s.vaddr;
    return 0x00269000;
}

std::string game_callers(const uint32_t *sp)
{
    const uint32_t *top = (const uint32_t *)((NT_TIB *)NtCurrentTeb())->StackBase;
    static const uint32_t code_end = game_code_end();
    std::string out;
    int found = 0;
    for (const uint32_t *p = sp; p < top && p < sp + 0x800 && found < 5; p++) {
        uint32_t v = *p;
        if (v < 0x00011005 || v >= code_end) continue; // game code (.text and the libraries before D3D)
        const uint8_t *c = (const uint8_t *)(uintptr_t)v;
        bool call = c[-5] == 0xE8 || (c[-2] == 0xFF && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10) ||
                    (c[-6] == 0xFF && (c[-5] & 0x38) == 0x10);
        if (!call) continue;
        out += (found++ ? " < " : "") + sym_name(v);
    }
    return out;
}

// The return addresses of the game code that made this call (as game_callers, the first `max`); how many were found
int first_game_callers(const uint32_t *sp, uint32_t *out, int max)
{
    const uint32_t *top = (const uint32_t *)((NT_TIB *)NtCurrentTeb())->StackBase;
    static const uint32_t code_end = game_code_end();
    int found = 0;
    for (const uint32_t *p = sp; p < top && p < sp + 0x800 && found < max; p++) {
        uint32_t v = *p;
        if (v < 0x00011005 || v >= code_end) continue;
        const uint8_t *c = (const uint8_t *)(uintptr_t)v;
        if (c[-5] == 0xE8 || (c[-2] == 0xFF && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10) ||
            (c[-6] == 0xFF && (c[-5] & 0x38) == 0x10))
            out[found++] = v;
    }
    return found;
}

static bool write_png_bgra(const wchar_t *path, uint32_t w, uint32_t h, const uint8_t *px)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *f = nullptr;
    IWICStream *st = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *fr = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
        SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h))) {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        fr->SetPixelFormat(&pf);
        ok = SUCCEEDED(fr->WritePixels(h, w * 4, w * h * 4, (BYTE *)px)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (f) f->Release();
    return ok;
}

// --debug-frame: the host copy of a bound (decoded, managed) texture as PNG, colour and alpha-as-grey, once per
// resource, next to the render-target dumps: tex_<header>_<w>x<h>.png.
static void dump_bound_texture(int s, const Resource *r)
{
    static std::vector<const Resource *> dumped;
    if (g_cfg.screenshot_dir.empty() || !g_tex_sent[s] || g_tex_sent[s]->GetType() != D3DRTYPE_TEXTURE) return;
    if (std::find(dumped.begin(), dumped.end(), r) != dumped.end()) return;
    dumped.push_back(r);
    // a game texture is read from its system copy (texture.cpp): the DEFAULT one the draws sample cannot be locked
    IDirect3DBaseTexture9 *sys = texture_system_copy(g_tex_sent[s]);
    auto *t = (IDirect3DTexture9 *)(sys ? sys : g_tex_sent[s]);
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(0, &desc);
    const bool readable = desc.Pool == D3DPOOL_MANAGED || desc.Pool == D3DPOOL_SYSTEMMEM;
    if (readable && (desc.Format == D3DFMT_DXT1 || desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT5)) {
        // compressed: the raw blocks of level 0 as the host holds them (tools/dxt_decode.py decodes them)
        D3DLOCKED_RECT lr;
        if (FAILED(t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) return;
        CreateDirectoryW(g_cfg.screenshot_dir.c_str(), nullptr);
        wchar_t name[96];
        swprintf(name, 96, L"\tex_%08x_%ux%u.dxt%d", (uint32_t)(uintptr_t)r, desc.Width, desc.Height, desc.Format == D3DFMT_DXT1 ? 1 : desc.Format == D3DFMT_DXT3 ? 3 : 5);
        FILE *fp = _wfopen((g_cfg.screenshot_dir + name).c_str(), L"wb");
        if (fp) {
            uint32_t rows = (desc.Height + 3) / 4;
            for (uint32_t y = 0; y < rows; y++) fwrite((const uint8_t *)lr.pBits + y * lr.Pitch, 1, lr.Pitch, fp);
            fclose(fp);
            logf("[draw]   tex%d %p dumped as %s (pitch %d)", s, r, narrow(g_cfg.screenshot_dir + name).c_str(), lr.Pitch);
        }
        t->UnlockRect(0);
        return;
    }
    if (!readable || (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)) {
        logf("[draw]   tex%d %p not dumped (pool %u format %u)", s, r, desc.Pool, desc.Format);
        return;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) return;
    std::vector<uint8_t> px((size_t)desc.Width * desc.Height * 4), al(px.size());
    for (UINT y = 0; y < desc.Height; y++) {
        const uint8_t *row = (const uint8_t *)lr.pBits + y * lr.Pitch;
        for (UINT x = 0; x < desc.Width; x++) {
            const uint8_t *p = row + x * 4;
            uint8_t *c = &px[((size_t)y * desc.Width + x) * 4], *a = &al[((size_t)y * desc.Width + x) * 4];
            c[0] = p[0];
            c[1] = p[1];
            c[2] = p[2];
            c[3] = 0xFF;
            a[0] = a[1] = a[2] = p[3];
            a[3] = 0xFF;
        }
    }
    t->UnlockRect(0);
    CreateDirectoryW(g_cfg.screenshot_dir.c_str(), nullptr);
    wchar_t name[96];
    swprintf(name, 96, L"\\tex_%08x_%ux%u", (uint32_t)(uintptr_t)r, desc.Width, desc.Height);
    std::wstring base = g_cfg.screenshot_dir + name;
    write_png_bgra((base + L".png").c_str(), desc.Width, desc.Height, px.data());
    write_png_bgra((base + L"_alpha.png").c_str(), desc.Width, desc.Height, al.data());
    logf("[draw]   tex%d %p dumped as %s", s, r, narrow(base).c_str());
}

} // namespace

// A BGRA picture as PNG (screenshots of a program drawing its own framebuffer: nv2a_display.cpp)
bool write_png(const wchar_t *path, uint32_t w, uint32_t h, const uint8_t *bgra) { return write_png_bgra(path, w, h, bgra); }

// The game code that called the function whose stack pointer this is (up to five, innermost first): for the kernel's logs
std::string callers_of(const uint32_t *sp) { return game_callers(sp); }

// cfg:pixwatch:X,Y: in a traced frame the back-buffer pixel X,Y (Xbox pixels) is read back before every draw and at
// the swap; a change is put on the draw before (its callers). Finds the draw behind a band with no obvious owner
// (2026-10-22). The 1x1 surfaces are made and released each time, so nothing in the default pool outlives a Reset.
bool probe_bound_depth(LONG x, LONG y, double d[4]); // device.cpp
// cfg:zprobe: a shadow pass is being drawn over the watched pixel, which was cleared to depth 0 first; the depth
// there before (restored after the read)
bool g_zprobe_pending;
double g_zprobe_before, g_zlast = -1;

void pixwatch_check(const char *next, bool depth_read)
{
    static uint32_t last = 0;
    static bool have = false;
    static std::string last_draw;
    if (g_cfg.pix_watch_x < 0 || !debug_frame_active() || !g_st.render_target) return;
    if (g_cfg.pix_watch_rt ? g_st.render_target->Data != g_cfg.pix_watch_rt : !is_backbuffer(g_st.render_target)) return;
    IDirect3DDevice9 *d = host3d::dev();
    IDirect3DSurface9 *rt = nullptr, *one = nullptr, *sys = nullptr;
    if (!d || FAILED(d->GetRenderTarget(0, &rt))) return;
    D3DSURFACE_DESC desc;
    rt->GetDesc(&desc);
    LONG x = lroundf(g_cfg.pix_watch_x * target_scale_x()), y = lroundf(g_cfg.pix_watch_y * target_scale_y());
    // cfg:zprobe: the depth there right after the probed shadow pass (what it wrote; 0: it did not cover the pixel, cleared
    // before it), or at the end of the frame with zprobe:-1 (the ground's: shadow passes do not write depth). Reading
    // draws (device.cpp depth_view), which spoils the draws after it, so never before the probed pass.
    if ((g_zprobe_pending || (g_cfg.z_probe < 0 && !strcmp(next, "swap")))) {
        double z[4];
        if (probe_bound_depth(x, y, z))
            logf("[zpix] %d,%d: depth %.1f (slope x %.1f y %.1f) %s %s", g_cfg.pix_watch_x, g_cfg.pix_watch_y, z[0], z[1] - z[0],
                 z[2] - z[0], g_zprobe_pending ? (z[0] == 0 ? "probed pass did not cover it:" : "written by the probed pass") : "at the end of the frame",
                 g_zprobe_pending ? last_draw.c_str() : "");
        else
            logf("[zpix] %d,%d: depth not readable", g_cfg.pix_watch_x, g_cfg.pix_watch_y);
        g_zprobe_pending = false;
    }
    RECT src = {x, y, x + 1, y + 1};
    if (x >= 0 && y >= 0 && (UINT)x < desc.Width && (UINT)y < desc.Height &&
        SUCCEEDED(d->CreateRenderTarget(1, 1, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &one, nullptr)) &&
        SUCCEEDED(d->CreateOffscreenPlainSurface(1, 1, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(d->StretchRect(rt, &src, one, nullptr, D3DTEXF_POINT)) && SUCCEEDED(d->GetRenderTargetData(one, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            uint32_t v = *(const uint32_t *)lr.pBits & 0xFFFFFF;
            sys->UnlockRect();
            if (have && v != last)
                logf("[pix] %d,%d: %06x -> %06x by %s", g_cfg.pix_watch_x, g_cfg.pix_watch_y, last, v, last_draw.c_str());
            last = v;
            have = true;
        }
    }
    if (sys) sys->Release();
    if (one) one->Release();
    rt->Release();
    last_draw = next;
}

namespace {

void debug_draw(const char *what, const DrawCtx &ctx, uint32_t prim, uint32_t count, const float *verts, uint32_t stride_floats)
{
    if (g_debug_draws < 0) g_debug_draws = (int)g_cfg.debug_draws;
    bool this_frame = debug_frame_active();
    if (this_frame && g_cfg.pix_watch_x >= 0) {
        char b[96];
        snprintf(b, sizeof b, "draw %u (%s, tex0 %p)", g_draw_serial, what, (void *)g_st.textures[0]);
        const uint32_t *zrs = render_state();
        static uint32_t zframe;
        static int zn;
        if (g_frame_count != zframe) zframe = g_frame_count, zn = 0;
        bool probed = g_cfg.z_probe > 0 && zrs[XRS_ZENABLE] && !zrs[XRS_ZWRITEENABLE] && samples_shadow_map() && ++zn == g_cfg.z_probe;
        pixwatch_check((std::string(b) + " from " + game_callers((const uint32_t *)_AddressOfReturnAddress())).c_str(), !probed);
        if (probed) {
            // cfg:zprobe: a shadow pass writes its own depth, untested and unbiased, so the next check reads it
            IDirect3DDevice9 *d = host3d::dev();
            float zero = 0.0f;
            DWORD z0;
            memcpy(&z0, &zero, 4);
            d->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
            d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            d->SetRenderState(D3DRS_DEPTHBIAS, z0);
            d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, z0);
            g_rs_valid[XRS_ZFUNC] = g_rs_valid[XRS_ZWRITEENABLE] = g_rs_valid[XRS_ZBIAS] = false;
            LONG x = lroundf(g_cfg.pix_watch_x * target_scale_x()), y = lroundf(g_cfg.pix_watch_y * target_scale_y());
            g_zprobe_before = g_zlast; // read at an earlier draw; nothing between wrote depth there (the log shows)
            g_zprobe_pending = true;
            D3DRECT r = {x, y, x + 1, y + 1};
            d->Clear(1, &r, D3DCLEAR_ZBUFFER, 0, 0.0f, 0);
        }
    }
    if (this_frame && g_cfg.z_probe) {
        // cfg:zprobe: full-precision inputs of both depth paths, to replay them on the CPU: fixed-function draws (world0,
        // view, projection, the first vertices) and shadow passes (the program's constants, its first vertices)
        auto mat = [](const D3DMATRIX &m) {
            std::string s;
            char b[24];
            for (int i = 0; i < 16; i++) snprintf(b, sizeof b, " %.9g", (&m._11)[i]), s += b;
            return s;
        };
        auto vtx = [&](int n) {
            std::string s;
            char b[24];
            for (int i = 0; verts && i < 3 && (uint32_t)i < count; i++) {
                s += " |";
                for (int j = 0; j < n; j++) snprintf(b, sizeof b, " %.9g", verts[i * stride_floats + j]), s += b;
            }
            return s;
        };
        if (ctx.path == Path::Fvf && verts && (ctx.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZ)
            logf("[zm] ff draw %u: world%s | view%s | proj%s | verts%s", g_draw_serial, mat(g_st.transforms[6]).c_str(),
                 mat(g_st.transforms[0]).c_str(), mat(g_st.transforms[1]).c_str(), vtx(3).c_str());
        else if (ctx.path == Path::Prog && ctx.vs && ctx.vs->program && samples_shadow_map()) {
            std::string c;
            char b[24];
            for (int k : {96, 97, 98, 99, 58, 59})
                for (int j = 0; j < 4; j++) snprintf(b, sizeof b, " %.9g", g_st.vs_constants[k][j]), c += b;
            logf("[zm] prog draw %u (vs %p): c0-c3 c-38 c-37%s | verts%s", g_draw_serial, (void *)ctx.vs, c.c_str(), vtx(4).c_str());
            static std::set<const void *> shown;
            if (shown.insert(ctx.vs).second) {
                std::string dis = vsh_disasm(ctx.vs->program, ctx.vs->program_insts);
                for (size_t pos = 0, nl; pos < dis.size(); pos = nl + 1) {
                    nl = dis.find('\n', pos);
                    if (nl == std::string::npos) nl = dis.size();
                    logf("[zm]   %s", dis.substr(pos, nl - pos).c_str());
                }
            }
        }
    }
    if (g_debug_draws <= 0 && !this_frame) return;
    if (!this_frame) g_debug_draws--;
    const uint32_t *rs = render_state();
    // frame ranges trace the 2D/full-screen draws in full (fades, post passes, HUD); the world geometry (vertex
    // shader path) gets one line per draw with its textures and blend state, no stack walk (that costs seconds
    // a frame and freezes the game under the player's hands).
    if (this_frame && g_cfg.debug_frame_end && !strcmp(what, "DrawIndexedVertices")) {
        char tex[256] = "";
        size_t n = 0;
        for (int s = 0; s < 4 && n < sizeof tex - 1; s++) {
            Resource *r = g_st.textures[s];
            if (!r) continue;
            auto *pc = (PixelContainer *)r;
            uint32_t w, h, dd, l;
            pixel_container_dims(pc, &w, &h, &dd, &l);
            n += snprintf(tex + n, sizeof tex - n, " t%d %p %ux%u %s @%08x", s, r, w, h,
                          format_info((pc->Format >> FMT_FORMAT_SHIFT) & 0xFF).name, pc->Data);
        }
        logf("[draw] %s prim %u count %u, vs 0x%x, ps 0x%x | z %u w %u blend %u 0x%x/0x%x color 0x%08x |%s", what, prim, count,
             g_st.vertex_shader, g_st.pixel_shader, rs[XRS_ZENABLE], rs[XRS_ZWRITEENABLE], rs[XRS_ALPHABLENDENABLE], rs[XRS_SRCBLEND],
             rs[XRS_DESTBLEND], rs[XRS_BLENDCOLOR], tex);
        return;
    }
    if (this_frame && g_cfg.debug_frame_end) logf("[draw] --- frame %u", g_frame_count.load() + 1);
    logf("[draw] %s prim %u count %u, %s 0x%x, ps 0x%x, tex0 %p", what, prim, count, ctx.path == Path::Prog ? "vs" : "fvf",
         g_st.vertex_shader, g_st.pixel_shader, g_st.textures[0]);
    if (this_frame) logf("[draw]   from %s", game_callers((const uint32_t *)_AddressOfReturnAddress()).c_str());
    if (this_frame) {
        for (int s = 0; s < 4; s++) {
            Resource *r = g_st.textures[s];
            if (!r) continue;
            auto *pc = (PixelContainer *)r;
            uint32_t w, h, dd, l;
            pixel_container_dims(pc, &w, &h, &dd, &l);
            logf("[draw]   tex%d %p: %ux%u L%u %s, data 0x%08x size 0x%08x format 0x%08x common 0x%08x", s, r, w, h, l,
                 format_info((pc->Format >> FMT_FORMAT_SHIFT) & 0xFF).name, pc->Data, pc->Size, pc->Format, pc->Common);
            dump_bound_texture(s, r);
        }
    }
    {
        float fs, fe, fd;
        memcpy(&fs, &rs[XRS_FOGSTART], 4);
        memcpy(&fe, &rs[XRS_FOGEND], 4);
        memcpy(&fd, &rs[XRS_FOGDENSITY], 4);
        logf("[draw]   fog %u table %u start %g end %g density %g color 0x%08x range %u | lighting %u ambient 0x%08x specular %u colorvertex %u "
             "sources d%u a%u s%u e%u | vertexblend %u | localviewer %u normalizenormals %u",
             rs[XRS_FOGENABLE], rs[XRS_FOGTABLEMODE], fs, fe, fd, rs[XRS_FOGCOLOR], rs[XRS_RANGEFOGENABLE],
             rs[XRS_LIGHTING], rs[XRS_AMBIENT], rs[XRS_SPECULARENABLE], rs[XRS_COLORVERTEX], rs[XRS_DIFFUSEMATERIALSOURCE],
             rs[XRS_AMBIENTMATERIALSOURCE], rs[XRS_SPECULARMATERIALSOURCE], rs[XRS_EMISSIVEMATERIALSOURCE], rs[XRS_VERTEXBLEND],
             rs[XRS_LOCALVIEWER], rs[XRS_NORMALIZENORMALS]);
        logf("[draw]   point size %g min %g max %g sprite %u scale %u (%g %g %g)", as_float(rs[XRS_POINTSIZE]),
             as_float(rs[XRS_POINTSIZE_MIN]), as_float(rs[XRS_POINTSIZE_MAX]), rs[XRS_POINTSPRITEENABLE], rs[XRS_POINTSCALEENABLE],
             as_float(rs[XRS_POINTSCALE_A]), as_float(rs[XRS_POINTSCALE_B]), as_float(rs[XRS_POINTSCALE_C]));
        if (rs[XRS_LIGHTING]) {
            D3DMATERIAL9 mat = {};
            host3d::dev()->GetMaterial(&mat);
            std::string on;
            for (DWORD i = 0; i < 8; i++) {
                BOOL e = FALSE;
                if (SUCCEEDED(host3d::dev()->GetLightEnable(i, &e)) && e) on += std::to_string(i) + " ";
            }
            logf("[draw]   host material diffuse %g %g %g ambient %g %g %g emissive %g %g %g | host lights on: %s", mat.Diffuse.r, mat.Diffuse.g,
                 mat.Diffuse.b, mat.Ambient.r, mat.Ambient.g, mat.Ambient.b, mat.Emissive.r, mat.Emissive.g, mat.Emissive.b,
                 on.empty() ? "none" : on.c_str());
        }
    }
    if (ctx.path == Path::Fvf && verts && (ctx.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZ) {
        // Where the first vertex lands: v * world0 * view * projection (row vectors, D3D convention), then / w.
        const D3DMATRIX *m[3] = {&g_st.transforms[6], &g_st.transforms[0], &g_st.transforms[1]};
        float v[4] = {verts[0], verts[1], verts[2], 1.0f};
        for (int k = 0; k < 3; k++) {
            const float *a = &m[k]->_11;
            float o[4];
            for (int c = 0; c < 4; c++) o[c] = v[0] * a[c] + v[1] * a[4 + c] + v[2] * a[8 + c] + v[3] * a[12 + c];
            memcpy(v, o, sizeof v);
        }
        logf("[draw]   clip %g %g %g %g -> ndc %g %g %g | world0 row3 %g %g %g | view row3 %g %g %g | proj diag %g %g %g %g", v[0], v[1], v[2],
             v[3], v[3] != 0 ? v[0] / v[3] : 0.f, v[3] != 0 ? v[1] / v[3] : 0.f, v[3] != 0 ? v[2] / v[3] : 0.f, m[0]->_41, m[0]->_42, m[0]->_43,
             m[1]->_41, m[1]->_42, m[1]->_43, m[2]->_11, m[2]->_22, m[2]->_33, m[2]->_34);
    }
    float poff, pslope;
    memcpy(&poff, &rs[XRS_POLYGONOFFSETZOFFSET], 4);
    memcpy(&pslope, &rs[XRS_POLYGONOFFSETZSLOPESCALE], 4);
    logf("[draw]   z %u func 0x%x write %u zbias %u polyoffset %u %g %g | blend %u 0x%x/0x%x op 0x%x color 0x%08x | atest %u func 0x%x ref %u | cull 0x%x | cwrite 0x%x | stencil %u",
         rs[XRS_ZENABLE], rs[XRS_ZFUNC], rs[XRS_ZWRITEENABLE], rs[XRS_ZBIAS], rs[XRS_SOLIDOFFSETENABLE], poff, pslope,
         rs[XRS_ALPHABLENDENABLE], rs[XRS_SRCBLEND], rs[XRS_DESTBLEND], rs[XRS_BLENDOP], rs[XRS_BLENDCOLOR], rs[XRS_ALPHATESTENABLE],
         rs[XRS_ALPHAFUNC], rs[XRS_ALPHAREF], rs[XRS_CULLMODE], rs[XRS_COLORWRITEENABLE], rs[XRS_STENCILENABLE]);
    if (g_st.pixel_shader) { // the combiner program as it is in render states 0-56
        uint32_t n = rs[53] & 0xFF;
        logf("[draw]   psh: combiners 0x%x texmodes 0x%x final 0x%08x 0x%08x c0 0x%08x c1 0x%08x fc0 0x%08x fc1 0x%08x", rs[53],
             rs[XRS_PSTEXTUREMODES], rs[8], rs[9], rs[10], rs[18], rs[43], rs[44]);
        for (uint32_t i = 0; i < n && i < 8; i++)
            logf("[draw]   psh stage %u: rgb in 0x%08x out 0x%08x | alpha in 0x%08x out 0x%08x", i, rs[34 + i], rs[45 + i], rs[i], rs[26 + i]);
    }
    if (!g_st.pixel_shader)
        for (int s = 0; s < 4; s++) {
            const uint32_t *ts = texture_state(s);
            logf("[draw]   stage %d: color op %u args 0x%x 0x%x | alpha op %u args 0x%x 0x%x | tfactor 0x%08x | xform flags %u addr %u/%u tci 0x%x", s, ts[12], ts[14], ts[15],
                 ts[16], ts[18], ts[19], rs[XRS_TEXTUREFACTOR], ts[21], ts[0], ts[1], ts[28]);
            if (ts[21]) { // the stage's texture matrix (applied to XYZRHW coordinates by rhw_tex_scales)
                const D3DMATRIX &M = g_st.transforms[2 + s];
                logf("[draw]     stage %d texture matrix: %g %g | %g %g | %g %g", s, M._11, M._12, M._21, M._22, M._31, M._32);
            }
            if (ts[6] || ts[9] || ts[10] || ts[11] || ts[22] || ts[23] || ts[24] || ts[25] || ts[26] || ts[27])
                logf("[draw]     stage %d: lodbias 0x%08x colorkeyop %u colorsign 0x%08x alphakill %u | bumpenv %g %g %g %g scale %g offset %g", s,
                     ts[6], ts[9], ts[10], ts[11], as_float(ts[22]), as_float(ts[23]), as_float(ts[24]), as_float(ts[25]),
                     as_float(ts[26]), as_float(ts[27]));
            if (this_frame) { // what the host device holds for the stage (cached state application check)
                DWORD hc = 0, ha = 0, h1 = 0, h2 = 0, ha1 = 0, ha2 = 0, hci = 0, htf = 0;
                IDirect3DDevice9 *hd = host3d::dev();
                hd->GetTextureStageState(s, D3DTSS_COLOROP, &hc); hd->GetTextureStageState(s, D3DTSS_ALPHAOP, &ha);
                hd->GetTextureStageState(s, D3DTSS_COLORARG1, &h1); hd->GetTextureStageState(s, D3DTSS_COLORARG2, &h2);
                hd->GetTextureStageState(s, D3DTSS_ALPHAARG1, &ha1); hd->GetTextureStageState(s, D3DTSS_ALPHAARG2, &ha2);
                hd->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &hci); hd->GetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, &htf);
                IDirect3DBaseTexture9 *ht = nullptr;
                hd->GetTexture(s, &ht);
                if (ht) ht->Release();
                logf("[draw]   host stage %d: cop %lu (%lu,%lu) aop %lu (%lu,%lu) tci %lu ttff %lu tex %p (sent %p)", s, hc, h1, h2, ha, ha1, ha2, hci, htf, ht, g_tex_sent[s]);
            }
        }
    float vp[8];
    viewport_transform(vp, vp + 4);
    logf("[draw]   viewport scale %g %g %g offset %g %g %g; c58 %g %g %g %g; c59 %g %g %g %g", vp[0], vp[1], vp[2], vp[4], vp[5],
         vp[6], g_st.vs_constants[58][0], g_st.vs_constants[58][1], g_st.vs_constants[58][2], g_st.vs_constants[58][3],
         g_st.vs_constants[59][0], g_st.vs_constants[59][1], g_st.vs_constants[59][2], g_st.vs_constants[59][3]);
    { // the host's own clip: viewport and scissor in host pixels
        D3DVIEWPORT9 hv = {};
        RECT hs = {};
        DWORD son = 0;
        host3d::dev()->GetViewport(&hv);
        host3d::dev()->GetScissorRect(&hs);
        host3d::dev()->GetRenderState(D3DRS_SCISSORTESTENABLE, &son);
        logf("[draw]   host viewport %lu,%lu %lux%lu, scissor %s %ld,%ld-%ld,%ld (target scale %g x %g)", hv.X, hv.Y, hv.Width,
             hv.Height, son ? "on" : "off", hs.left, hs.top, hs.right, hs.bottom, target_scale_x(), target_scale_y());
    }
    if (ctx.path == Path::Prog) {
        for (int c : {96, 97, 190, 191, 0, 1, 2, 3, 10, 11, 12, 13, 18, 21, 22, 23, 24, 25})
            logf("[draw]   c%d (slot %d) = %g %g %g %g", c - 96, c, g_st.vs_constants[c][0], g_st.vs_constants[c][1],
                 g_st.vs_constants[c][2], g_st.vs_constants[c][3]);
        if (ctx.vs && ctx.vs->program) { // the constants the program reads (relative ones from their base)
            bool used[192] = {};
            for (uint32_t i = 0; i < ctx.vs->program_insts; i++) {
                VshInst in;
                vsh_decode(ctx.vs->program + i * 4, in);
                if ((in.a.mux == MUX_C || in.b.mux == MUX_C || in.c.mux == MUX_C) && in.cnst < 192) used[in.cnst] = true;
                if (in.final) break;
            }
            std::string line;
            for (int c = 0; c < 192; c++) {
                if (!used[c]) continue;
                char b[96];
                snprintf(b, sizeof b, " c%d=(%g %g %g %g)", c - 96, g_st.vs_constants[c][0], g_st.vs_constants[c][1],
                         g_st.vs_constants[c][2], g_st.vs_constants[c][3]);
                line += b;
            }
            logf("[draw]   program reads%s", line.c_str());
        }
    }
    if (verts && stride_floats == 64) // immediate mode: position and texture coordinates (attributes 0, 9-12)
        for (uint32_t v = 0; v < count && v < 4; v++) {
            const float *a = verts + v * 64;
            logf("[draw]   v%u: pos %g %g %g %g | t0 %g %g %g %g | t1 %g %g %g %g | t2 %g %g %g %g", v, a[0], a[1], a[2], a[3], a[36],
                 a[37], a[38], a[39], a[40], a[41], a[42], a[43], a[44], a[45], a[46], a[47]);
        }
    else if (verts)
        for (uint32_t v = 0; v < count && v < 3; v++) {
            std::string s;
            for (uint32_t f = 0; f < stride_floats && f < 16; f++) {
                char b[24];
                snprintf(b, sizeof b, "%s%g", (f && f % 4 == 0) ? " | " : (f ? " " : ""), verts[v * stride_floats + f]);
                s += b;
            }
            logf("[draw]   v%u: %s", v, s.c_str());
        }
}

// Set up state for a draw. Returns false (and counts a skipped draw) if it cannot be drawn.
// The shaders and vertex input last sent to the device. Drivers do not all filter repeats, and Intel's spent
// ~2 ms a frame on them. Fullscreen passes change these and restore them with a state block, so these stay valid.
IDirect3DPixelShader9 *g_ps_sent = (IDirect3DPixelShader9 *)~0u;
IDirect3DVertexShader9 *g_vs_sent = (IDirect3DVertexShader9 *)~0u;
IDirect3DVertexDeclaration9 *g_decl_sent; // null after SetFVF
DWORD g_fvf_sent = ~0u;                   // ~0 after SetVertexDeclaration
DWORD g_fog_table_sent = ~0u;

void set_ps(IDirect3DDevice9 *d, IDirect3DPixelShader9 *ps)
{
    if (ps != g_ps_sent) d->SetPixelShader(g_ps_sent = ps);
}

void set_vs(IDirect3DDevice9 *d, IDirect3DVertexShader9 *vs)
{
    if (vs != g_vs_sent) d->SetVertexShader(g_vs_sent = vs);
}

void set_fvf(IDirect3DDevice9 *d, DWORD fvf)
{
    if (fvf == g_fvf_sent && !g_decl_sent) return;
    d->SetFVF(g_fvf_sent = fvf);
    g_decl_sent = nullptr;
}

// Point sprites of fixed-function vertices without texture coordinates (Ninja Gaiden Black's particles: water drops and
// splashes FUN_00326e70, sparks and smoke FUN_002b9d20; FVF XYZ|DIFFUSE). The host replaces texture coordinates with the
// sprite's own (0..1 across the point), but only where it has some: the port used to give it a generated coordinate
// (D3DTSS_TCI_CAMERASPACEPOSITION) read in TEXCOORD3, the sprite stage on the Xbox. An AMD Radeon RX 9070 XT drew the
// water drops as solid squares (2026-11-10): the dot texture read at one coordinate. Now these draws get what every
// driver handles, the plain use of point sprites: a texture coordinate in the vertices (set 0, appended here), passed
// through by every stage, and the stage shader reads the sprite stage from TEXCOORD0 (d3d/ffp_ps.cpp).
bool g_sprite_tc;        // this draw: append the coordinate (set by begin_draw)
DWORD g_sprite_fvf;      // its FVF without the coordinate
std::vector<uint8_t> g_sprite_buf;

const void *with_sprite_tc(const void *src, uint32_t nverts, uint32_t &stride)
{
    // The coordinate goes right after the FVF's own fields (the game's stride can be larger: padding the host skips)
    uint32_t size = fvf_stride(g_sprite_fvf);
    if (!size || size > stride) return src; // not the layout expected: the vertices go as they are
    uint32_t out = size + 8;
    g_sprite_buf.resize((size_t)nverts * out);
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *o = g_sprite_buf.data();
    for (uint32_t i = 0; i < nverts; i++, s += stride, o += out) {
        memcpy(o, s, size);
        memset(o + size, 0, 8); // (0, 0): replaced by the sprite coordinate on points
    }
    set_fvf(host3d::dev(), g_sprite_fvf | D3DFVF_TEX1);
    static bool noted;
    if (!noted) {
        noted = true;
        logf("[draw] frame %u: point sprites without texture coordinates (fvf 0x%x, %u vertices): one appended for the "
             "sprite coordinates, read from TEXCOORD0", g_frame_count.load(), g_sprite_fvf, nverts);
    }
    stride = out;
    return g_sprite_buf.data();
}

void set_decl(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *decl)
{
    if (decl == g_decl_sent) return;
    d->SetVertexDeclaration(g_decl_sent = decl);
    g_fvf_sent = ~0u;
}

} // namespace

// After a fullscreen pass: set everything again at the next draw instead of trusting the state block's restore
// (a laptop with Intel graphics crashed in a vertex copy inside D3D9 after the cache was added).
void forget_sent_shaders()
{
    g_ps_sent = (IDirect3DPixelShader9 *)~0u;
    g_vs_sent = (IDirect3DVertexShader9 *)~0u;
    g_decl_sent = nullptr;
    g_fvf_sent = ~0u;
}

void host_texture_transform_changed(int stage, const D3DMATRIX *m)
{
    if (stage < 0 || stage >= 4) return;
    if (m) {
        g_texm_sent[stage] = *m; // what the host holds now
        g_texm_valid[stage] = true;
    } else {
        g_texm_valid[stage] = false;
    }
}

uint32_t texture_matrix_stale_count()
{
    uint32_t n = g_texm_stale;
    g_texm_stale = 0;
    return n;
}

void forget_sent_state()
{
    forget_sent_shaders();
    g_point_valid = false;
    memset(g_texm_valid, 0, sizeof g_texm_valid);
    memset(g_ttff_valid, 0, sizeof g_ttff_valid);
    memset(g_rs_valid, 0, sizeof g_rs_valid);
    memset(g_ts_valid, 0, sizeof g_ts_valid);
    for (IDirect3DBaseTexture9 *&t : g_tex_sent) t = nullptr; // Reset unbinds every texture
    g_fog_table_sent = ~0u;
}

namespace {

int g_ms_aa_sent = -1; // D3DRS_MULTISAMPLEANTIALIAS as last sent (-1: unknown, a new device)

bool begin_draw(DrawCtx &ctx, uint32_t xprim)
{
    IDirect3DDevice9 *d = host3d::dev();
    if (!d) return false;
    rt_cpu_upload(); // render targets the CPU wrote into since the last draw (the NG2004 downloader; nothing otherwise)
    g_draw_serial++;
    g_sprite_tc = false;
    g_ffp_quad_stages = g_ffp_quad_weighted = 0; // a 2D quad's stage shader variant is picked by scale_rhw (QuadSnap)
    if (g_cfg.skip_texmodes && g_st.pixel_shader && render_state()[XRS_PSTEXTUREMODES] == g_cfg.skip_texmodes) return false;
    // Stack scans stop at the thread's stack base: a draw near the top of a game thread's stack read past it and
    // crashed the game (cfg:skipcaller, play 2026-10-22).
    const uint32_t *stack_top = (const uint32_t *)((NT_TIB *)NtCurrentTeb())->StackBase;
    if (g_cfg.skip_caller) { // diagnosis: which game routine draws something
        const uint32_t *sp = (const uint32_t *)_AddressOfReturnAddress();
        for (int i = 0; i < 160 && sp + i < stack_top; i++)
            if (sp[i] == g_cfg.skip_caller) return false;
    }
    if (g_cfg.flash_watch) {
        uint32_t caller = 0; // the first game .text return address above us: which game routine submitted the draw
        const uint32_t *sp = (const uint32_t *)_AddressOfReturnAddress();
        for (int i = 0; i < 160 && sp + i < stack_top && !caller; i++)
            if (sp[i] >= 0x00011000 && sp[i] < 0x0037A480) caller = sp[i];
        flash_record_draw(caller, (uint32_t)(uintptr_t)g_st.textures[0], g_st.vertex_shader);
    }
    flash_count_draw(g_st.render_target == nullptr || is_backbuffer(g_st.render_target), g_st.vertex_shader & 1, g_st.pixel_shader != 0);
    apply_render_states(d);
    bool ps = g_st.pixel_shader != 0;
    apply_texture_stages(d, !ps); // may copy a render target (outside the scene) first
    note_tap_filter(g_rt_stage);
    host3d::begin_scene();
    // Fixed-function draws get a generated stage shader too (d3d/ffp_ps.cpp); the host's own fixed-function pixel
    // pipeline (D3D9On12) dropped alpha modulation on pass-through stages (docs/04 §3.9). --host-ffp keeps it.
    uint32_t vsh = g_st.vertex_shader;
    bool fvf_vs = vs_is_fvf(vsh);
    // Point sprites (points drawn with POINTSPRITEENABLE; other primitives with it on are drawn as before): the stage
    // shader reads the sprite stage from TEXCOORD0, the set every driver replaces with the sprite's coordinates, and
    // FVF vertices without texture coordinates get one appended there (with_sprite_tc)
    bool sprite_points = render_state()[XRS_POINTSPRITEENABLE] && xprim == XPT_POINTLIST;
    g_ffp_sprite_tc = sprite_points;
    g_sprite_tc = sprite_points && fvf_vs && vsh && !((vsh >> 8) & 0xF);
    if (sprite_points && !fvf_vs) { // not seen in Ninja Gaiden Black so far: noted, to know if AMD needs more (docs/04 §3.30)
        static bool noted;
        if (!noted) {
            noted = true;
            logf("[draw] frame %u: point sprites through a vertex program or declaration (vs 0x%x, %s), from %s", g_frame_count.load(),
                 vsh, ps ? "the game's pixel shader: its stage 3 reads TEXCOORD3" : "stage shader: the sprite stage reads TEXCOORD0",
                 game_callers((const uint32_t *)_AddressOfReturnAddress()).c_str());
        }
    }
    IDirect3DPixelShader9 *hps = ps ? ps_host_shader()
                                 : g_cfg.host_ffp ? nullptr
                                 : ffp_pixel_shader(!fvf_vs || (vsh & D3DFVF_DIFFUSE) != 0, !fvf_vs || (vsh & D3DFVF_SPECULAR) != 0,
                                                    fvf_vs && (vsh & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW);
    set_ps(d, hps);
    if (ps && render_state()[XRS_FOGENABLE] && psh_reads_fog()) {
        // A pixel shader that reads its FOG register with fog on: FOG.a is the fog factor a translated vertex program
        // passes in TEXCOORD4 (1.1.8-t, the underwater tint); with fixed-function vertices it is 1. Noted once or so.
        static int noted;
        if (noted++ < 4) logf("[draw] frame %u: pixel shader 0x%x reads FOG with fog enabled (%s)", g_frame_count.load(),
                              g_st.pixel_shader, fvf_vs ? "fixed-function vertices: fog factor 1" : "fog factor from the vertex program");
        if (noted == 1 && g_cfg.trace_fog_ps && !g_cfg.debug_frame) g_cfg.debug_frame = g_frame_count + 1; // --trace-fog-ps
    }
    if (ps && hps) upload_ps_constants(d, !fvf_vs);
    else if (ps) apply_pixel_shader_fallback(d);
    else if (hps) upload_ffp_constants(d);
    uint32_t h = g_st.vertex_shader;
    if (vs_is_fvf(h)) {
        if (h) {
            set_vs(d, nullptr);
            set_fvf(d, h);
            apply_fvf_texture_scale(d, (h & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW);
            if (g_sprite_tc) {
                // Point sprites whose vertices carry no texture coordinates: the draw appends one (with_sprite_tc)
                // and every stage passes it through, for the host to replace with the sprite's coordinates.
                g_sprite_fvf = h;
                for (int s = 0; s < 4; s++) {
                    d->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU | 0);
                    g_ts_valid[s][28] = false; // the game's index is sent again on the next draw
                }
            } else if (render_state()[XRS_POINTSPRITEENABLE] && !((h >> 8) & 0xF)) {
                // Other primitives with POINTSPRITEENABLE left on and no texture coordinates: as before 2.2.17, a
                // generated coordinate for the stages with a texture
                for (int s = 0; s < 4; s++) {
                    if (!g_st.textures[s]) continue;
                    d->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION | s);
                    g_ts_valid[s][28] = false;
                }
            }
            ctx.path = Path::Fvf;
            ctx.fvf = h;
        }
    } else if (VertexShader *vs = vs_from_handle(h)) {
        if (!vs->program) { // fixed-function vertices through a declaration (decl_fvf)
            if (uint32_t f = decl_fvf(vs)) {
                set_vs(d, nullptr);
                set_fvf(d, f);
                apply_fvf_texture_scale(d, false);
                ctx.path = Path::Fvf;
                ctx.fvf = f;
                ctx.vs = vs; // the draw converts the vertices (convert_decl_vertices)
            }
        } else if (IDirect3DVertexShader9 *hv = vs_host_shader(vs, bump_divide_stages())) {
            set_vs(d, hv);
            upload_vs_constants(d);
            ctx.path = Path::Prog;
            ctx.vs = vs;
        }
    }
    if (ctx.path == Path::None) {
        g_st.draws_skipped++;
        return false;
    }
    if (ctx.path == Path::Fvf) {
        static uint32_t synced_once;
        if (sync_material_from_device() && debug_frame_active()) logf("[rt] material changed in the device object");
        int changed = sync_transforms_from_device();
        if (changed && (!synced_once || debug_frame_active())) {
            if (!synced_once) logf("[d3d] transforms taken from the device object (mask 0x%x): the game writes them inline", changed);
            else logf("[rt] transforms changed in the device object: mask 0x%x", changed);
            synced_once = 1;
        }
    }
    // Vertex shaders compute the fog factor themselves (oFog -> table function): D3D9 vertex fog.
    DWORD fog_table = ctx.path == Path::Prog ? D3DFOG_NONE : render_state()[XRS_FOGTABLEMODE];
    if (fog_table != g_fog_table_sent) {
        d->SetRenderState(D3DRS_FOGTABLEMODE, fog_table);
        g_fog_table_sent = fog_table;
    }
    if (g_cfg.widescreen && g_cfg.aspect_w * 9 != g_cfg.aspect_h * 16 && g_st.render_target && is_backbuffer(g_st.render_target))
        wide_hud_count(!(ctx.path == Path::Fvf && (ctx.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW));
    if (ms_drawing()) {
        // MSAA: the 3D multisampled, the game's screen-space 2D (HUD, menus, text, screen copies) not: drawn as without
        // MSAA, its quads and 1-pixel rules covering whole pixels. A menu rule (a line primitive) sampled between pixel
        // centres bled half a pixel into the row beside it (pause menu, 4:3 1x MSAA 8x: (74,11,9) -> (114,77,75)).
        bool ms = !(ctx.path == Path::Fvf && (ctx.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW);
        if (g_ms_aa_sent != (int)ms) {
            d->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, ms);
            g_ms_aa_sent = ms;
        }
    }
    ms_note_draw();
    g_st.draws++;
    return true;
}

// Set by scale_rhw for a picture squeezed to 16:9 that would show past it: the host rectangle the next draw is
// clipped to; empty otherwise
RECT g_rhw_clip;

// For one draw: the scissor narrowed to g_rhw_clip (within the game's own, if on), and put back after
struct ClipRhw {
    IDirect3DDevice9 *d = nullptr;
    DWORD on = FALSE;
    RECT old = {};
    ClipRhw()
    {
        if (g_rhw_clip.right <= g_rhw_clip.left) return;
        d = host3d::dev();
        RECT r = g_rhw_clip;
        g_rhw_clip = {};
        d->GetRenderState(D3DRS_SCISSORTESTENABLE, &on);
        d->GetScissorRect(&old);
        if (on) r = {std::max(r.left, old.left), std::max(r.top, old.top), std::min(r.right, old.right), std::min(r.bottom, old.bottom)};
        d->SetScissorRect(&r);
        if (!on) d->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    }
    ~ClipRhw()
    {
        if (!d) return;
        d->SetScissorRect(&old);
        if (!on) d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    }
};

void draw_up(uint32_t xprim, uint32_t verts, const void *data, uint32_t stride)
{
    ClipRhw clip;
    Prim p = conv_prim(xprim, verts);
    if (!p.count) return;
    if (g_sprite_tc) data = with_sprite_tc(data, verts, stride);
    perf::add_draw(verts, verts * stride);
    IDirect3DDevice9 *d = host3d::dev();
    if (p.quads)
        d->DrawIndexedPrimitiveUP(p.type, 0, verts, p.count, quad_indices(verts / 4), D3DFMT_INDEX16, data, stride);
    else
        d->DrawPrimitiveUP(p.type, p.count, data, stride);
}

// Every page of [p, p+n) is committed and readable. Direct3D 9 copies the vertices of a DrawIndexedPrimitiveUP
// itself, so a range that runs past the game's vertex buffer (Ninja Gaiden Black's recorded push buffers index
// past the stream bound at run time, which the NV2A reads as garbage) would fault inside d3d9.dll.
bool pages_readable(const void *p, size_t n)
{
    // VirtualQuery for every draw cost ~15% of a busy frame (r069): remember the committed regions found readable
    // this frame and answer from them; the cache starts over every frame.
    struct Region { const uint8_t *lo, *hi; };
    static Region cache[16];
    static int cached;
    static uint32_t cache_frame = ~0u;
    uint32_t frame = g_frame_count.load(std::memory_order_relaxed);
    if (frame != cache_frame) {
        cache_frame = frame;
        cached = 0;
    }
    const uint8_t *a = (const uint8_t *)p, *end = a + n;
    for (int i = 0; i < cached; i++)
        if (a >= cache[i].lo && end <= cache[i].hi) return true;
    const uint8_t *start = a;
    const uint8_t *region_lo = nullptr;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        if (!region_lo) region_lo = (const uint8_t *)mbi.BaseAddress;
        a = (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
    }
    if (region_lo && start >= region_lo) {
        cache[cached % 16] = {region_lo, a};
        if (cached < 16) cached++;
    }
    return true;
}

void draw_indexed_up(uint32_t xprim, uint32_t nidx, const uint16_t *idx, const void *verts, uint32_t stride)
{
    ClipRhw clip;
    if (!nidx) return;
    uint32_t mn = 0xFFFF, mx = 0;
    for (uint32_t i = 0; i < nidx; i++) {
        if (idx[i] < mn) mn = idx[i];
        if (idx[i] > mx) mx = idx[i];
    }
    Prim p = conv_prim(xprim, nidx);
    if (!p.count) return;
    const uint8_t *src = (const uint8_t *)verts + (size_t)mn * stride;
    size_t bytes = (size_t)(mx - mn + 1) * stride;
    bool in_conv = src >= (const uint8_t *)g_conv.data() && src + bytes <= (const uint8_t *)(g_conv.data() + g_conv.size());
    {
        // What the host runtime was last asked to copy (dump_exception prints it: a fault inside d3d9's vertex copy
        // then tells how far past the block it read).
        LastDraw &ld = g_last_draws[g_last_draw_next++ % 4];
        ld.verts = verts;
        ld.src = src;
        ld.bytes = bytes;
        ld.stride = stride;
        ld.mn = mn;
        ld.mx = mx;
        ld.nidx = nidx;
        ld.prim = xprim;
        ld.conv = g_conv.data();
        ld.conv_bytes = g_conv.size() * 4;
    }
    if (!in_conv && !pages_readable(src, bytes)) {
        static int reported;
        if (reported < 8) {
            reported++;
            logf("[draw] indexed draw skipped: vertices %p stride %u, indices %u..%u (%u of them) need %zu bytes that are not all readable; from %s",
                 verts, stride, mn, mx, nidx, bytes, game_callers((const uint32_t *)_AddressOfReturnAddress()).c_str());
        }
        g_st.draws_skipped++;
        return;
    }
    if (g_sprite_tc) src = (const uint8_t *)with_sprite_tc(src, mx - mn + 1, stride);
    perf::add_draw(mx - mn + 1, (mx - mn + 1) * stride);
    // Indices rebased to the first used vertex, data pointer at that vertex: the host then copies exactly the
    // vertices [mn, mx]. Passing MinVertexIndex = mn with the pointer backed up mn vertices (so index 0 would be
    // "vertex 0") is what d3d9.dll accepts, but D3D9On12 (Windows 11 on GPUs without a native D3D9 driver, the Intel
    // Iris Xe here) copies (MinVertexIndex + NumVertices) vertices from the pointer: 4483 x 64 bytes from a block
    // of 442 that starts 4041 vertices later, reading into unmapped memory (run r013, docs/04).
    uint16_t mnw = (uint16_t)mn;
    if (p.quads) {
        g_tmp_indices.resize(p.count * 3);
        for (uint32_t q = 0; q < nidx / 4; q++) {
            const uint16_t *s = idx + q * 4;
            uint16_t *o = &g_tmp_indices[q * 6];
            o[0] = s[0] - mnw; o[1] = s[1] - mnw; o[2] = s[2] - mnw; o[3] = s[0] - mnw; o[4] = s[2] - mnw; o[5] = s[3] - mnw;
        }
        idx = g_tmp_indices.data();
    } else if (mn) {
        g_tmp_indices.resize(nidx);
        for (uint32_t i = 0; i < nidx; i++) g_tmp_indices[i] = idx[i] - mnw;
        idx = g_tmp_indices.data();
    }
    uint32_t host_nidx = p.quads ? p.count * 3 : nidx;
    if (g_cfg.draw_ring && ring_draw(p.type, mx - mn + 1, p.count, idx, host_nidx, src, stride)) return;
    host3d::dev()->DrawIndexedPrimitiveUP(p.type, 0, mx - mn + 1, p.count, idx, D3DFMT_INDEX16, src, stride);
}

const uint8_t *stream_data(uint32_t stream)
{
    VertexBuffer *vb = g_st.streams[stream].vb;
    return vb ? (const uint8_t *)phys_to_virt(vb->Data) : nullptr;
}

} // namespace

// Texture coordinates of pre-transformed (XYZRHW) vertices: D3D9 applies no texture transform to them (they skip
// the vertex pipeline), so the texel -> 0..1 scale of a linear texture is applied to the vertex data here. The game's
// own texture matrix is not, as on the console: the D3D library gives an XYZRHW format the passthrough program (its
// FVF builder: (fvf & 0xE) == 4 -> flags |= 2, 0x3C85FE in x2.1; SetVertexShader then sets the transform execution
// mode to program, 0x1E94 = 6), and its texture-matrix writer skips everything for such a shader (flags & 0x12,
// 0x3D10C0); the same code in Ninja Gaiden 2004's update and disc. So no matrix and no generated coordinates reach
// screen-space vertices. Applying a matrix left by an environment-mapped draw (0.5 / -0.5, reflection) squeezed
// Ninja Gaiden 2004's HUD emblem onto its texture's transparent top row near the chapter-3 airship (the user,
// 2026-11-02; run/r827). cfg:rhwtexmat:1 applies the game's matrix as before (comparison). Ninja Gaiden Black copies the back buffer
// (720x480 linear) into its 512x512 snapshot and the depth buffer into its 256x256 glow source with XYZRHW quads
// addressed 0..720 x 0..480; through the stage transform both came out as one flat colour (docs/04 §3.8).
struct RhwTexScale {
    uint32_t off; // byte offset of the 2-float set in the vertex
    float m[6];   // u' = u*m0 + v*m1 + m2, v' = u*m3 + v*m4 + m5
};

int rhw_tex_scales(uint32_t fvf, RhwTexScale *out)
{
    int n = 0;
    uint32_t tc = (fvf >> 8) & 0xF;
    uint32_t base = 16 + ((fvf & D3DFVF_NORMAL) ? 12 : 0) + ((fvf & D3DFVF_PSIZE) ? 4 : 0) + ((fvf & D3DFVF_DIFFUSE) ? 4 : 0) +
                    ((fvf & D3DFVF_SPECULAR) ? 4 : 0);
    for (int s = 0; s < 4; s++) {
        if (!g_st.textures[s]) continue;
        float sx, sy;
        linear_texture_scale(s, &sx, &sy);
        const D3DMATRIX &M = g_st.transforms[2 + s];
        bool game_matrix = texture_state(s)[21] != 0 &&
                           !(M._11 == 1 && M._12 == 0 && M._21 == 0 && M._22 == 1 && M._31 == 0 && M._32 == 0);
        bool matrix = g_cfg.rhw_tex_matrix && game_matrix;
        static int noted;
        if (game_matrix && !matrix && noted < 20 && ++noted)
            logf("[draw] XYZRHW draw, stage %d texture matrix %g %g | %g %g | %g %g left out (as the console), tci 0x%x", s,
                 M._11, M._12, M._21, M._22, M._31, M._32, texture_state(s)[28]);
        if (sx == 1.0f && sy == 1.0f && !matrix) continue;
        uint32_t set = texture_state(s)[28] & 0xFFFF; // TEXCOORDINDEX, as sent to the host
        if (set >= tc || ((fvf >> (16 + 2 * set)) & 3) != 0) continue; // only 2-float sets
        uint32_t off = base;
        for (uint32_t i = 0; i < set; i++) {
            static const uint32_t sizes[4] = {8, 12, 16, 4};
            off += sizes[(fvf >> (16 + 2 * i)) & 3];
        }
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= out[i].off == off; // two stages on one set: scale it once
        if (dup) continue;
        RhwTexScale &t = out[n++];
        t.off = off;
        if (matrix) {
            t.m[0] = M._11 * sx; t.m[1] = M._21 * sx; t.m[2] = M._31 * sx;
            t.m[3] = M._12 * sy; t.m[4] = M._22 * sy; t.m[5] = M._32 * sy;
        } else {
            t.m[0] = sx; t.m[1] = 0; t.m[2] = 0;
            t.m[3] = 0; t.m[4] = sy; t.m[5] = 0;
        }
    }
    return n;
}

void apply_rhw_tex_scales(uint8_t *verts, uint32_t count, uint32_t stride, const RhwTexScale *ts, int n)
{
    for (uint32_t v = 0; v < count; v++)
        for (int i = 0; i < n; i++) {
            float *uv = (float *)(verts + (size_t)v * stride + ts[i].off);
            float u = uv[0], w = uv[1];
            uv[0] = u * ts[i].m[0] + w * ts[i].m[1] + ts[i].m[2];
            uv[1] = u * ts[i].m[3] + w * ts[i].m[4] + ts[i].m[5];
        }
}

// The HUD in play on a screen not 16:9 (players' request, 2026-11-09; the idea from ngbrecomp's renderer, docs/TECHNICAL
// "HudPlace"/"HudGroup", written anew here). The game draws one 16:9 layout; squeeze_169 keeps its shape. Menus,
// the title, chapter cards and movies stay whole about the middle, as before. In play (a frame with 3D drawn into the
// back buffer before its 2D) each HUD element instead keeps to its own side: the health bar and karma to the top-left
// corner, prompts and the right-hand gauges to the right, what is about the middle to the middle. An element is a
// group of pieces drawn within a few pixels of each other (a gauge, its caps and the words under it; a line of text),
// found at each frame's end and used for the next frame's pieces; a piece no element of the frame before holds goes by
// its own box (along the top) or to the middle until then. Positions: Xbox pixels of the 720x480 buffer.
struct WideHud {
    struct Box {
        float x0, x1, y0, y1;
        uint32_t at; // its draw's number in the frame
    };
    struct Elem {
        float x0, x1, y0, y1, ax, ay; // its box and the point it is squeezed about (0, w/2, w; 0, h/2, h)
        uint32_t seen;
        bool gauge;
    };
    uint32_t draws = 0, threed = 0, last3d = 0; // this frame's draws into the back buffer; the 3D ones; the last 3D one
    bool covered = false;                       // this frame drew an opaque colour over the whole screen (an item's screen)
    std::vector<Box> boxes, gauge_boxes;        // this frame's HUD pieces; the frame before's pieces of tall gauges
    std::vector<Elem> elems, memory;            // the frame before's elements; those of the last half minute
    bool panel = false;                         // a screen of panels over the game (a chapter clear) is up
    int panel_count = 0;
    Box panel_box{}, panel_seen{};
    uint32_t frame = 0;
};
WideHud g_wh;

void wide_hud_count(bool threed)
{
    g_wh.draws++;
    if (threed) {
        g_wh.threed++;
        g_wh.last3d = g_wh.draws;
    }
}

// The side an element (or a lone piece) keeps to: the middle when it is about the middle, else the nearer side; the
// top when it starts in the top quarter, the bottom when it ends in the bottom quarter.
void hud_side(float x0, float x1, float y0, float y1, float w, float h, float &ax, float &ay)
{
    float cx = (x0 + x1) * 0.5f;
    ax = fabsf(cx - w * 0.5f) < w * 0.12f ? w * 0.5f : cx < w * 0.5f ? 0.0f : w;
    bool top = y0 < h * 0.25f, bottom = y1 > h * 0.75f;
    ay = top && !bottom ? 0.0f : bottom && !top ? h : h * 0.5f;
}

// A tall narrow gauge (the breath meter under water), or a piece inside a gauge's column (its caps, its fill): kept
// apart from the windows drawn across it, which would otherwise join it into one element as tall as the screen.
bool hud_gauge(float x0, float x1, float y0, float y1, float w, float h, const WideHud::Elem *g = nullptr)
{
    if (y1 - y0 > h * 0.15f && x1 - x0 < w * 0.1f) return true;
    return g && x0 >= g->x0 - 4 && x1 <= g->x1 + 4 && y0 >= g->y0 - 14 && y1 <= g->y1 + 14;
}

void wide_hud_place(float x0, float x1, float y0, float y1, float w, float h, float &ax, float &ay)
{
    ax = w * 0.5f, ay = h * 0.5f;
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    if (g_wh.covered) return; // over an opaque cover: the screen's own art, whole about the middle
    if (g_wh.panel && cx >= g_wh.panel_box.x0 - 8 && cx <= g_wh.panel_box.x1 + 8 && cy >= g_wh.panel_box.y0 - 8 &&
        cy <= g_wh.panel_box.y1 + 8)
        return; // inside a chapter clear's panel: with the panel
    bool gauge = hud_gauge(x0, x1, y0, y1, w, h);
    for (const WideHud::Box &b : g_wh.gauge_boxes)
        if (!gauge && fabsf(b.x0 - x0) <= 3 && fabsf(b.x1 - x1) <= 3 && fabsf(b.y0 - y0) <= 3 && fabsf(b.y1 - y1) <= 3)
            gauge = true;
    if (!gauge) {
        bool in_column = false, in_other = false;
        for (const WideHud::Elem &e : g_wh.elems) {
            if (e.gauge && hud_gauge(x0, x1, y0, y1, w, h, &e)) in_column = true;
            if (!e.gauge && cx >= e.x0 - 2 && cx <= e.x1 + 2 && cy >= e.y0 - 2 && cy <= e.y1 + 2) in_other = true;
        }
        gauge = in_column && !in_other;
    }
    const WideHud::Elem *hit = nullptr;
    float best = 1e30f;
    for (const auto *list : {&g_wh.elems, &g_wh.memory}) {
        for (const WideHud::Elem &e : *list) {
            if (e.gauge != gauge || cx < e.x0 - 2 || cx > e.x1 + 2 || cy < e.y0 - 2 || cy > e.y1 + 2) continue;
            float area = (e.x1 - e.x0) * (e.y1 - e.y0);
            if (area < best) best = area, hit = &e;
        }
        if (hit) break; // the frame before's elements first; one seen again within the last half minute after
    }
    if (hit) {
        ax = hit->ax, ay = hit->ay;
        return;
    }
    hud_side(x0, x1, y0, y1, w, h, ax, ay);
    if (y1 >= h * 0.3f) ax = w * 0.5f; // a new piece below the top: the middle until it is grouped
}

// Each frame's end (Swap): this frame's HUD pieces drawn after its last 3D draw, grouped into elements.
void wide_hud_frame_end()
{
    WideHud &s = g_wh;
    s.frame++;
    float w = (float)backbuffer_width(), h = (float)backbuffer_height();
    std::vector<WideHud::Box> &b = s.boxes;
    if (s.last3d) b.erase(std::remove_if(b.begin(), b.end(), [&](const WideHud::Box &x) { return x.at < s.last3d; }), b.end());
    size_t n = b.size();
    std::vector<int> up(n);
    for (size_t i = 0; i < n; i++) up[i] = (int)i;
    auto root = [&](int i) {
        while (up[i] != i) i = up[i] = up[up[i]];
        return i;
    };
    std::vector<char> gauge(n, 0);
    for (size_t i = 0; i < n; i++) gauge[i] = hud_gauge(b[i].x0, b[i].x1, b[i].y0, b[i].y1, w, h);
    for (size_t i = 0; i < n; i++) // pieces in a tall gauge's column, drawn a few draws from it, are the gauge's
        for (size_t j = 0; j < n && !gauge[i]; j++) {
            const WideHud::Box &g = b[j];
            WideHud::Elem col = {g.x0, g.x1, g.y0, g.y1, 0, 0, 0, true};
            if (j != i && g.y1 - g.y0 > h * 0.15f && g.x1 - g.x0 < w * 0.1f && b[i].at + 6 >= g.at && g.at + 6 >= b[i].at &&
                hud_gauge(b[i].x0, b[i].x1, b[i].y0, b[i].y1, w, h, &col))
                gauge[i] = 1;
        }
    s.gauge_boxes.clear();
    for (size_t i = 0; i < n; i++)
        if (gauge[i]) s.gauge_boxes.push_back(b[i]);
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            const WideHud::Box &p = b[i], &q = b[j];
            // a line of text: words of one height join across a gap of up to 0.6 letter heights
            float gx = fabsf(p.y0 - q.y0) <= 2 && fabsf(p.y1 - q.y1) <= 2 ? std::max(10.0f, (p.y1 - p.y0) * 0.6f) : 10.0f;
            if (gauge[i] == gauge[j] && p.x0 - gx <= q.x1 && q.x0 - gx <= p.x1 && p.y0 - 14 <= q.y1 && q.y0 - 14 <= p.y1)
                up[root((int)i)] = root((int)j);
        }
    std::vector<WideHud::Elem> elems;
    std::vector<int> slot(n, -1);
    for (size_t i = 0; i < n; i++) {
        int r = root((int)i);
        if (slot[r] < 0) {
            slot[r] = (int)elems.size();
            elems.push_back({b[i].x0, b[i].x1, b[i].y0, b[i].y1, 0, 0, s.frame, gauge[i] != 0});
        } else {
            WideHud::Elem &e = elems[slot[r]];
            e.x0 = std::min(e.x0, b[i].x0), e.x1 = std::max(e.x1, b[i].x1);
            e.y0 = std::min(e.y0, b[i].y0), e.y1 = std::max(e.y1, b[i].y1);
        }
    }
    bool panel = false;
    WideHud::Box pb = {1e30f, -1e30f, 1e30f, -1e30f, 0};
    for (WideHud::Elem &e : elems) {
        hud_side(e.x0, e.x1, e.y0, e.y1, w, h, e.ax, e.ay);
        // a panel: wide, tall, across the middle (a chapter clear); an area's name in a band is not one
        float ew = std::min(e.x1, w) - std::max(e.x0, 0.0f), eh = std::min(e.y1, h) - std::max(e.y0, 0.0f);
        if (ew > w * 0.35f && eh > h * 0.45f && e.y0 < h * 0.35f && e.y1 > h * 0.65f &&
            fabsf((std::max(e.x0, 0.0f) + std::min(e.x1, w)) * 0.5f - w * 0.5f) < w * 0.12f) {
            panel = true;
            pb.x0 = std::min(pb.x0, e.x0), pb.x1 = std::max(pb.x1, e.x1), pb.y0 = std::min(pb.y0, e.y0), pb.y1 = std::max(pb.y1, e.y1);
        }
    }
    if (panel) { // its reach while it is seen (it slides and grows in)
        if (!s.panel && !s.panel_count) s.panel_seen = pb;
        s.panel_seen.x0 = std::min(s.panel_seen.x0, pb.x0), s.panel_seen.x1 = std::max(s.panel_seen.x1, pb.x1);
        s.panel_seen.y0 = std::min(s.panel_seen.y0, pb.y0), s.panel_seen.y1 = std::max(s.panel_seen.y1, pb.y1);
        s.panel_box = s.panel_seen;
    }
    if (panel != s.panel) { // on after 4 frames running, off after 8 without
        if (++s.panel_count >= (panel ? 4 : 8)) s.panel = panel, s.panel_count = 0;
    } else
        s.panel_count = 0;
    if (!s.panel) {
        for (const WideHud::Elem &e : elems) {
            s.memory.erase(std::remove_if(s.memory.begin(), s.memory.end(), [&](const WideHud::Elem &m) {
                               return m.gauge == e.gauge && m.x0 < e.x1 && e.x0 < m.x1 && m.y0 < e.y1 && e.y0 < m.y1;
                           }),
                           s.memory.end());
            s.memory.push_back(e);
        }
        s.memory.erase(std::remove_if(s.memory.begin(), s.memory.end(), [&](const WideHud::Elem &m) { return s.frame - m.seen > 1800; }),
                       s.memory.end());
        if (s.memory.size() > 64) s.memory.erase(s.memory.begin(), s.memory.end() - 64);
    }
    // evidence: the elements whenever their sides change (a few dozen times a run at most)
    static int logged;
    static std::string last;
    if (logged < 60 && s.threed >= 30) {
        std::string sig;
        for (const WideHud::Elem &e : elems) {
            char t[64];
            snprintf(t, sizeof t, " [%.0f-%.0f,%.0f-%.0f %c%c%s]", e.x0, e.x1, e.y0, e.y1, e.ax == 0 ? 'L' : e.ax == w ? 'R' : 'M',
                     e.ay == 0 ? 'T' : e.ay == h ? 'B' : 'C', e.gauge ? "g" : "");
            sig += t;
        }
        if (s.panel) sig += " PANEL";
        if (sig != last) {
            logged++;
            logf("[hud] frame %u: %zu pieces, 3D draws %u:%s", g_frame_count.load(), n, s.threed, sig.c_str());
            last = sig;
        }
    }
    s.elems.swap(elems);
    s.boxes.clear();
    s.draws = s.threed = s.last3d = 0;
    s.covered = false;
}

// 16:10 / 21:9 (ngb_aspect.cpp): the game lays its 2D out for 16:9 over the 720-wide buffer, so a wider picture
// stretched its HUD, text and menus. Screen-space draws on the back buffer are squeezed back to 16:9 about the
// centre. All of them come from one sprite routine (FUN_00325810).
//  - The game's own pictures (swizzled textures: HUD, text, menus, the title screen, backgrounds) always, and they
//    are clipped to the 16:9 part (g_rhw_clip, draw_up): a menu's pieces that reach or pass the sides (the title's
//    background runs -32..751 at full height, its credits quad out to 744, r368; the main menu's bands and
//    scrolling patterns out to -1153 and 1920, r371) then end at its edges, with black or the game on either side,
//    rather than some stretched and some not (r369).
//  - Untextured quads and screen copies (linear textures: fades, dimming, the glow pass at -0.5..719.5, r340) keep
//    their full width when they reach a side or are nearly full height; smaller ones are squeezed.
//  - A movie frame (linear, the Sofdec player's blit FUN_0031DA50 + 0x13C, the full-screen quad of r346) keeps its
//    16:9 shape, with black on either side.
// 16:10 is narrower than 16:9: there the same 16:9 shape is kept by lowering the 2D instead (see the end).
// Works on Xbox-pixel positions (x, y at the start of each vertex), before the render scale (sx, sy) is applied.
void squeeze_169(uint8_t *verts, uint32_t count, uint32_t stride, float sx, float sy, uint32_t fvf)
{
    g_rhw_clip = {}; // none left from a draw that was not made
    if (!g_cfg.widescreen || g_cfg.aspect_w * 9 == g_cfg.aspect_h * 16 || !g_st.render_target || !is_backbuffer(g_st.render_target)) return;
    float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
    for (uint32_t v = 0; v < count; v++) {
        const float *p = (const float *)&verts[(size_t)v * stride];
        mnx = std::min(mnx, p[0]), mxx = std::max(mxx, p[0]), mny = std::min(mny, p[1]), mxy = std::max(mxy, p[1]);
    }
    float w = (float)backbuffer_width(), h = (float)backbuffer_height();
    Resource *t0 = g_st.textures[0];
    bool art = t0 && (resource_type(t0) == RTYPE_TEXTURE || resource_type(t0) == RTYPE_SURFACE) && !((PixelContainer *)t0)->Size &&
               !g_stage0_rendered;
    bool full_h = mxy - mny >= 0.9f * h, squeeze = art || !(mnx <= 0.5f || mxx >= w - 2.0f || full_h), clip = art;
    if (!t0 && full_h && mnx <= 1.0f && mxx >= w - 1.0f && (fvf & D3DFVF_DIFFUSE) && stride >= 20) {
        uint32_t c;
        memcpy(&c, verts + 16, 4); // XYZRHW, then the colour
        if (c >> 24 == 0xFF || !render_state()[XRS_ALPHABLENDENABLE]) g_wh.covered = true; // an item's screen: art about the middle
    }
    bool centre_only = false; // a menu's piece or a movie: always about the middle
    if (g_stage0_rendered && (fvf >> 8 & 0xF) && !(fvf & 0x30000)) {
        // A piece of the picture put back where it was: every vertex samples the screen copy at its own place (u, v =
        // x / 720, y / 480). The underwater wobble (FUN_0031B460) draws the screen as 12 x 8 tiles of 60 x 60; the ones
        // in the middle, reaching neither side, were each squeezed (two seams down the screen at x 60 and 660, the
        // user, 2026-10-24). The game's 2D never samples the picture so.
        uint32_t uv = 16 + (fvf & D3DFVF_DIFFUSE ? 4 : 0) + (fvf & D3DFVF_SPECULAR ? 4 : 0);
        bool in_place = uv + 8 <= stride;
        for (uint32_t v = 0; v < count && in_place; v++) {
            const float *p = (const float *)&verts[(size_t)v * stride], *t = (const float *)&verts[(size_t)v * stride + uv];
            in_place = fabsf(t[0] - (p[0] + 0.5f) / w) < 0.01f && fabsf(t[1] - (p[1] + 0.5f) / h) < 0.01f;
        }
        if (in_place) return;
    }
    if (art && full_h && mnx <= 0.5f && mxx >= w - 2.0f) {
        // A picture over the whole 3D view is no 2D: the underwater ripples (FUN_0013A050, its draw returning to
        // 0x13A8E6: a 128x128 DXT5 picture, -0.5..719.5 at full height) were squeezed and cut to the 16:9 middle,
        // the picture beside them left without (the user, 2026-10-24, r503 at 64:27)
        uint32_t by[1] = {};
        first_game_callers((const uint32_t *)_AddressOfReturnAddress(), by, 1);
        if (g_cfg.profile && g_cfg.profile->wide2d && by[0] == g_cfg.profile->wide2d->ripples_ret) return;
    }
    if (!squeeze) {
        uint32_t by[3] = {};
        first_game_callers((const uint32_t *)_AddressOfReturnAddress(), by, 3);
        // a menu's band or rule: the menus' 2D (FUN_00222730, 234 bytes; its calls at 0x222795 and 0x222805) draws
        // sprites through FUN_00325810, and their lines through 0x3273B8 (r375)
        const Wide2D *w2 = g_cfg.profile ? g_cfg.profile->wide2d : nullptr;
        auto menu = [w2](uint32_t a) { return w2 && a >= w2->menu_lo && a < w2->menu_hi; };
        if (full_h && w2 && by[0] == w2->movie_ret) squeeze = centre_only = true; // a movie frame
        else if (!full_h && (menu(by[1]) || menu(by[2]))) squeeze = clip = centre_only = true;
    }
    if (!squeeze) return;
    // In play (3D drawn into the back buffer this frame before this 2D) the HUD's elements keep to their sides
    // (wide_hud_place); a picture over most of the screen there is a screen effect and fills it as drawn.
    float ax = w * 0.5f, ay = h * 0.5f;
    if (!centre_only && g_wh.threed >= 30 && !g_cfg.hud_centre) {
        if (art && mxx - mnx >= 0.85f * w && mxy - mny >= 0.75f * h) return;
        wide_hud_place(mnx, mxx, mny, mxy, w, h, ax, ay);
        if (mxx - mnx < 0.89f * w && mxy - mny < 0.9f * h && g_wh.boxes.size() < 2048)
            g_wh.boxes.push_back({mnx, mxx, mny, mxy, g_wh.draws});
    }
    float k = (16.0f / 9.0f) * g_cfg.aspect_h / g_cfg.aspect_w;
    if (k < 1.0f) { // wider than 16:9 (21:9): narrowed about the centre (or the HUD element's side), the game beyond
        float c = ax;
        if (c != w * 0.5f) clip = false; // a corner's piece is whole as narrowed
        for (uint32_t v = 0; v < count; v++) {
            float *p = (float *)&verts[(size_t)v * stride];
            p[0] = c + (p[0] - c) * k;
        }
        if (clip && (mnx < c - c * k || mxx > c + c * k)) // only what would show outside the 16:9 part
            g_rhw_clip = {(LONG)((c - c * k) * sx + 0.5f), 0, (LONG)((c + c * k) * sx + 0.5f), (LONG)(h * sy + 0.5f)};
    } else { // narrower (16:10): widening it would cut its sides off (r392: the title's credits); lowered about the
             // middle instead, the whole 16:9 layout shown with black or the game above and below
        float ky = 1.0f / k, c = ay;
        if (c != h * 0.5f) clip = false;
        for (uint32_t v = 0; v < count; v++) {
            float *p = (float *)&verts[(size_t)v * stride];
            p[1] = c + (p[1] - c) * ky;
        }
        if (clip && (mny < c - c * ky || mxy > c + c * ky))
            g_rhw_clip = {0, (LONG)((c - c * ky) * sy + 0.5f), (LONG)(w * sx + 0.5f), (LONG)((c + c * ky) * sy + 0.5f)};
    }
}

// QuadSnap (ngbrecomp #83: src/d3d/vk/backend.cpp QuadSnap, gpu::QuadHlsl): a 2D quad above 1x drawn as the console covers
// and samples it. The game builds its 2D (the pause menu's dagger and plates, backgrounds, fades, letters) from axis-
// aligned pre-transformed quads that meet between console pixels; scaled up, each quad's edge column of host pixels
// sampled its texture past what the console's pixel centres read, and linear filtering blended a transparent texel's
// colour into its opaque neighbour: a thin light line down the pause menu's dagger where two of its quads meet (x
// 181.4999 and 181.5 of 720, the user's picture, 2026-10-30), lines round letters and along the seams of backgrounds.
// - The quad's edges go to the console pixel boundaries it covers, its texture coordinates along with them: the NV2A
//   keeps 4 fraction bits of a window coordinate, truncated (xemu vsh.c roundScreenCoords), and covers a pixel whose
//   centre is not left of / above the edge (as scale_rhw's border rule).
// - Each stage's coordinates are clamped to the range the console's pixel centres sample (half a console pixel in from
//   each edge), and a linearly filtered stage reads its four texels weighted by alpha (ffp_ps.cpp: the generated
//   stage shader's quad variant). Pixel-shader (combiner) draws keep their coordinates moved but are not clamped.
// An axis-aligned 4-vertex quad: every corner on its box within a hundredth of a pixel (the dagger's 181.4999), taken as
// the box's. cfg:quadsnap:0 draws them as before.
struct QuadSnap {
    int sets = 0;
    uint32_t off[8];       // byte offsets of the 2-float coordinate sets
    float centre[8][4][2]; // per set: the coordinates at the four corner pixels' centres (before rhw_tex_scales)
};

uint32_t g_quad_snaps, g_quad_snap_frame; // quads snapped in the last 600 frames (logged)

bool quad_prim(uint32_t xprim)
{
    return xprim == XPT_TRIANGLESTRIP || xprim == XPT_TRIANGLEFAN || xprim == XPT_QUADLIST || xprim == XPT_QUADSTRIP ||
           xprim == XPT_POLYGON;
}

bool quad_snap(uint32_t fvf, uint8_t *verts, uint32_t stride, float ox, float oy, QuadSnap &q)
{
    float xy[4][2], x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (int v = 0; v < 4; v++) {
        memcpy(xy[v], verts + (size_t)v * stride, 8);
        x0 = std::min(x0, xy[v][0]), x1 = std::max(x1, xy[v][0]), y0 = std::min(y0, xy[v][1]), y1 = std::max(y1, xy[v][1]);
    }
    auto on = [](float a, float b) { return fabsf(a - b) <= 0.01f; }; // a hundredth of a pixel
    for (int v = 0; v < 4; v++) {
        if (!((on(xy[v][0], x0) || on(xy[v][0], x1)) && (on(xy[v][1], y0) || on(xy[v][1], y1)))) return false;
        xy[v][0] = on(xy[v][0], x0) ? x0 : x1;
        xy[v][1] = on(xy[v][1], y0) ? y0 : y1;
    }
    if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return false;
    int c00 = -1, c10 = -1, c01 = -1;
    for (int v = 0; v < 4; v++) {
        if (xy[v][0] == x0 && xy[v][1] == y0) c00 = v;
        if (xy[v][0] == x1 && xy[v][1] == y0) c10 = v;
        if (xy[v][0] == x0 && xy[v][1] == y1) c01 = v;
    }
    if (c00 < 0 || c10 < 0 || c01 < 0) return false;
    // the window coordinate (position + the library's screen-space offset) -> the console pixel boundary it covers from
    auto snap = [](float w) { return ceilf(truncf(w * 16.0f) / 16.0f - 0.5f); };
    float sx0 = snap(x0 + ox), sx1 = snap(x1 + ox), sy0 = snap(y0 + oy), sy1 = snap(y1 + oy);
    if (sx1 - sx0 < 1.0f || sy1 - sy0 < 1.0f) return false;
    // the 2-float coordinate sets (rhw_tex_scales' layout)
    uint32_t tc = (fvf >> 8) & 0xF, off = 16 + ((fvf & D3DFVF_NORMAL) ? 12 : 0) + ((fvf & D3DFVF_PSIZE) ? 4 : 0) +
                                          ((fvf & D3DFVF_DIFFUSE) ? 4 : 0) + ((fvf & D3DFVF_SPECULAR) ? 4 : 0);
    static const uint32_t sizes[4] = {8, 12, 16, 4};
    q.sets = 0;
    float w = x1 - x0, h = y1 - y0;
    for (uint32_t i = 0; i < tc && i < 8; i++) {
        uint32_t code = (fvf >> (16 + 2 * i)) & 3;
        if (code == 0) {
            float uv[4][2];
            for (int v = 0; v < 4; v++) memcpy(uv[v], verts + (size_t)v * stride + off, 8);
            float dudx = (uv[c10][0] - uv[c00][0]) / w, dvdx = (uv[c10][1] - uv[c00][1]) / w;
            float dudy = (uv[c01][0] - uv[c00][0]) / h, dvdy = (uv[c01][1] - uv[c00][1]) / h;
            int k = q.sets++;
            q.off[k] = off;
            for (int v = 0; v < 4; v++) {
                bool left = xy[v][0] == x0, top = xy[v][1] == y0;
                float dx = (left ? sx0 : sx1) - (xy[v][0] + ox), dy = (top ? sy0 : sy1) - (xy[v][1] + oy);
                float u = uv[v][0] + dudx * dx + dudy * dy, t = uv[v][1] + dvdx * dx + dvdy * dy;
                memcpy(verts + (size_t)v * stride + off, &u, 4);
                memcpy(verts + (size_t)v * stride + off + 4, &t, 4);
                // half a console pixel in from each edge: the centre of the corner pixel
                float cx = left ? 0.5f : -0.5f, cy = top ? 0.5f : -0.5f;
                q.centre[k][v][0] = u + dudx * cx + dudy * cy;
                q.centre[k][v][1] = t + dvdx * cx + dvdy * cy;
            }
        }
        off += sizes[code];
    }
    for (int v = 0; v < 4; v++) {
        float p[2] = {(xy[v][0] == x0 ? sx0 : sx1) - ox, (xy[v][1] == y0 ? sy0 : sy1) - oy};
        memcpy(verts + (size_t)v * stride, p, 8);
    }
    return true;
}

// After the coordinates are scaled (rhw_tex_scales): each textured 2D stage's clamp range and texture size for the
// stage shader's quad variant, which is then picked
void quad_snap_shader(const QuadSnap &q, const RhwTexScale *ts, int nts)
{
    IDirect3DDevice9 *d = host3d::dev();
    float clamp[4][4] = {}, size[4][4] = {};
    uint32_t stages = 0, weighted = 0;
    for (int s = 0; s < 4; s++) {
        const uint32_t *tss = texture_state(s);
        if (!g_st.textures[s] || (tss[28] >> 16) || (tss[21] & 0x100)) continue; // no texture, generated or projected
        uint32_t set = tss[28] & 0xFFFF;
        uint32_t tc = (g_st.vertex_shader >> 8) & 0xF;
        if (set >= tc) continue;
        uint32_t off = 16 + ((g_st.vertex_shader & D3DFVF_DIFFUSE) ? 4 : 0) + ((g_st.vertex_shader & D3DFVF_SPECULAR) ? 4 : 0);
        static const uint32_t sizes[4] = {8, 12, 16, 4};
        for (uint32_t i = 0; i < set; i++) off += sizes[(g_st.vertex_shader >> (16 + 2 * i)) & 3];
        int k = -1;
        for (int i = 0; i < q.sets; i++)
            if (q.off[i] == off) k = i;
        if (k < 0) continue;
        // the host texture bound to the stage (apply_texture_stages) and its size, asked of Direct3D once per texture
        IDirect3DBaseTexture9 *bt = g_tex_sent[s];
        // (keyed by the Xbox header's format and size too: a texture made again may come back at the same address)
        static IDirect3DBaseTexture9 *size_of[4];
        static uint32_t shape_of[4][2];
        static D3DSURFACE_DESC desc_of[4];
        if (!bt) continue;
        const PixelContainer *pc = (const PixelContainer *)g_st.textures[s];
        if (bt != size_of[s] || shape_of[s][0] != pc->Format || shape_of[s][1] != pc->Size) {
            shape_of[s][0] = pc->Format;
            shape_of[s][1] = pc->Size;
            desc_of[s] = {};
            if (bt->GetType() == D3DRTYPE_TEXTURE) ((IDirect3DTexture9 *)bt)->GetLevelDesc(0, &desc_of[s]);
            size_of[s] = bt;
        }
        const D3DSURFACE_DESC &desc = desc_of[s];
        if (!desc.Width || !desc.Height) continue;
        float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
        for (int v = 0; v < 4; v++) {
            float u = q.centre[k][v][0], t = q.centre[k][v][1];
            for (int i = 0; i < nts; i++)
                if (ts[i].off == off) {
                    float a = u * ts[i].m[0] + t * ts[i].m[1] + ts[i].m[2], b = u * ts[i].m[3] + t * ts[i].m[4] + ts[i].m[5];
                    u = a, t = b;
                }
            u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, t), v1 = std::max(v1, t);
        }
        clamp[s][0] = u0, clamp[s][1] = v0, clamp[s][2] = u1, clamp[s][3] = v1;
        size[s][0] = (float)desc.Width, size[s][1] = (float)desc.Height, size[s][2] = 1.0f / desc.Width, size[s][3] = 1.0f / desc.Height;
        stages |= 1u << s;
        // linear filtering as apply_texture_stages sets it from the Xbox stage (MAGFILTER 3, MINFILTER 4: 2 or over 3)
        if (g_cfg.quad_weight && (tss[3] == 2 || tss[3] > 3 || tss[4] == 2 || tss[4] > 3)) weighted |= 1u << s;
    }
    if (!stages || g_st.pixel_shader || g_cfg.host_ffp) return;
    g_ffp_quad_stages = stages;
    g_ffp_quad_weighted = weighted;
    if (IDirect3DPixelShader9 *ps = ffp_reselect()) {
        set_ps(d, ps);
        upload_ffp_quad_constants(d, clamp, size);
    }
}

// Pre-transformed (XYZRHW) vertices carry Xbox screen pixels, but host render targets are scale_x/y() times
// larger: copy vertices [first, first+count) with x/y scaled (and texel coordinates normalized, see above).
// Returns a pointer usable with the original indices (the copy offset by -first vertices); other paths get
// `data` back unchanged.
std::vector<uint8_t> g_rhw;
// quad: the primitive can make a 4-vertex quad (a strip, a fan, a quad, a polygon, or indexed triangles) for QuadSnap.
const uint8_t *scale_rhw(const DrawCtx &ctx, const void *data, uint32_t first, uint32_t count, uint32_t stride, bool quad)
{
    auto *src = (const uint8_t *)data;
    float sx = target_scale_x(), sy = target_scale_y();
    if (ctx.path != Path::Fvf || (ctx.fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || !src) return src;
    if (g_dev) {
        // Census (docs/07 #10): the screen-space offset the library's pass-through program adds to these positions
        // (0x3C8F10: device +0xEF8/+0xEFC; 0.53125 by default, the game sets 0.03125 around some passes, 0x314430)
        static std::vector<std::pair<uint32_t, uint32_t>> seen;
        uint32_t ox = *(uint32_t *)(g_dev + 0xEF8), oy = *(uint32_t *)(g_dev + 0xEFC);
        if (seen.size() < 16 && std::find(seen.begin(), seen.end(), std::make_pair(ox, oy)) == seen.end()) {
            seen.push_back({ox, oy});
            logf("[draw] frame %u: XYZRHW draw with screen-space offset %g, %g (fvf 0x%x) from %s", g_frame_count.load(),
                 as_float(ox), as_float(oy), ctx.fvf, game_callers((const uint32_t *)_AddressOfReturnAddress()).c_str());
        }
    }
    RhwTexScale ts[4];
    int nts = rhw_tex_scales(ctx.fvf, ts);
    // The library's pass-through program adds the screen-space offset to these positions (0x3C8F10), and the NV2A
    // samples pixel n at n + 0.5: the Xbox pixel at x + offset, scaled to the host target, minus the host's half pixel
    // (a Direct3D 9 pre-transformed vertex at n lies on pixel n's centre). Scaling x alone left the last host column and
    // row of a scaled target unwritten (copies drawn -0.5 .. 255.5 at the default offset cover all 256 Xbox pixels, but
    // only 4 x 255.5 + 0.5 of 1024 host ones); the chapter-2 water refraction reads its 256x256 screen copy up to the
    // edge and showed those as black / white bands (r245). Ninja Gaiden Black also sets 0.03125 for its texture copies
    // (0x314430). BLACK's library is not checked (offset base 0): scaled as before (docs/07 #10).
    // An edge on the left / top border still covers the Xbox's first pixel: the NV2A keeps 4 fraction bits, truncated
    // (xemu vsh.c roundScreenCoords), so a full-screen quad from 0 lies at 0.5 and takes pixel 0, centre on the edge.
    // Scaled, that edge fell a whole host pixel or two inside the target, and the fades to black / white left the
    // picture under them showing down the left and along the top (the user, 2026-10-23): such an edge goes to the host
    // target's edge (edge_x / edge_y below; only vertices at the border move).
    float dx = 0, dy = 0, ox = 0, oy = 0;
    if (g_dev && g_cfg.rhw_offset && screen_offset_base() != 0.0f) {
        ox = *(const float *)(g_dev + 0xEF8);
        oy = *(const float *)(g_dev + 0xEFC);
        dx = ox * sx - 0.5f;
        dy = oy * sy - 0.5f;
    }
    const bool edges = dx != 0 || dy != 0;
    if (sx == 1 && sy == 1 && !nts && dx == 0 && dy == 0) return src;
    perf::Scope perf_scope(perf::kConvert);
    g_rhw.assign(src + (size_t)first * stride, src + (size_t)(first + count) * stride);
    if (g_cfg.rhw_edge_fix && g_st.render_target && !is_backbuffer(g_st.render_target)) {
        // A game slip, not emulation: the lava heat haze (FUN_00236010) draws the screen into a 512x256 target with a
        // -0.5 .. 510.5 x -0.5 .. 254.5 quad, a pixel short on both sides, also on the Xbox; the last column, never
        // written, came back as a dark bar down the right of the screen (the user, 2026-10-19: in the original too).
        // A quad from -0.5 that stops exactly one pixel short of a texture target is taken to the edge.
        // cfg:rhwedge:0 keeps the game's quad.
        uint32_t tw = 0, th = 0, td, tl;
        pixel_container_dims((const PixelContainer *)g_st.render_target, &tw, &th, &td, &tl);
        float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
        for (uint32_t v = 0; v < count; v++) {
            const float *p = (const float *)&g_rhw[(size_t)v * stride];
            mnx = std::min(mnx, p[0]), mxx = std::max(mxx, p[0]), mny = std::min(mny, p[1]), mxy = std::max(mxy, p[1]);
        }
        bool fx = tw && mnx == -0.5f && mxx == (float)tw - 1.5f, fy = th && mny == -0.5f && mxy == (float)th - 1.5f;
        if (fx || fy) {
            for (uint32_t v = 0; v < count; v++) {
                float *p = (float *)&g_rhw[(size_t)v * stride];
                if (fx && p[0] == mxx) p[0] = (float)tw - 0.5f;
                if (fy && p[1] == mxy) p[1] = (float)th - 0.5f;
            }
            static int noted;
            if (noted++ < 4) logf("[draw] frame %u: XYZRHW quad a pixel short of its %ux%u target, taken to the edge (%s%s)",
                                  g_frame_count.load(), tw, th, fx ? "x" : "", fy ? "y" : "");
        }
    }
    QuadSnap qs;
    bool snapped = quad && count == 4 && edges && g_cfg.quad_snap && (sx > 1.0f || sy > 1.0f) &&
                   quad_snap(ctx.fvf, g_rhw.data(), stride, ox, oy, qs);
    squeeze_169(g_rhw.data(), count, stride, sx, sy, ctx.fvf);
    for (uint32_t v = 0; v < count; v++) {
        float *p = (float *)&g_rhw[(size_t)v * stride];
        bool edge_x = edges && truncf((p[0] + ox) * 16.0f) <= 8.0f, edge_y = edges && truncf((p[1] + oy) * 16.0f) <= 8.0f;
        p[0] = edge_x ? std::min(p[0] * sx + dx, -0.5f) : p[0] * sx + dx;
        p[1] = edge_y ? std::min(p[1] * sy + dy, -0.5f) : p[1] * sy + dy;
    }
    apply_rhw_tex_scales(g_rhw.data(), count, stride, ts, nts);
    if (snapped) {
        quad_snap_shader(qs, ts, nts);
        uint32_t f = g_frame_count.load();
        g_quad_snaps++;
        if (f / 600 != g_quad_snap_frame / 600) {
            logf("[draw] frame %u: %u 2D quads drawn as the console covers and samples them (QuadSnap) in the last 600 frames",
                 f, g_quad_snaps);
            g_quad_snaps = 0;
            g_quad_snap_frame = f;
        }
    }
    return g_rhw.data() - (size_t)first * stride;
}

// Lines. The Xbox rasterizes its lines D3DRS_LINEWIDTH pixels wide at 720x480; a host line is one pixel at the
// render scale, so Ninja Gaiden Black's sparks (2-vertex XYZ|DIFFUSE line strips, additive, FUN_00318410) came
// out hair-thin (docs/04 §3.9). Untextured line primitives are projected on the CPU with the game's matrices and
// drawn as screen-space quads LINEWIDTH x the render scale wide; anything else falls through to the host line.
bool draw_wide_lines(const DrawCtx &ctx, uint32_t xprim, uint32_t count, const void *data, uint32_t stride)
{
    if (ctx.path != Path::Fvf || (xprim != XPT_LINELIST && xprim != XPT_LINESTRIP && xprim != XPT_LINELOOP)) return false;
    bool rhw = (ctx.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    if (ctx.fvf != (D3DFVF_XYZ | D3DFVF_DIFFUSE) && ctx.fvf != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)) return false;
    float sx = target_scale_x(), sy = target_scale_y();
    float lw;
    memcpy(&lw, &render_state()[XRS_LINEWIDTH], 4);
    if (!(lw > 0.0f)) lw = 1.0f;
    float half = lw * (sx + sy) * 0.25f; // half the width, in host pixels
    if (half <= 0.55f || count < 2 || !data) return false;
    IDirect3DDevice9 *d = host3d::dev();
    // world x view x projection (row vectors), from the state the fixed-function draws were given
    float m[16];
    if (!rhw) {
        const D3DMATRIX &W = g_st.transforms[6], &V = g_st.transforms[0], &P = g_st.transforms[1];
        float wv[16];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) wv[r * 4 + c] = W.m[r][0] * V.m[0][c] + W.m[r][1] * V.m[1][c] + W.m[r][2] * V.m[2][c] + W.m[r][3] * V.m[3][c];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) m[r * 4 + c] = wv[r * 4 + 0] * P.m[0][c] + wv[r * 4 + 1] * P.m[1][c] + wv[r * 4 + 2] * P.m[2][c] + wv[r * 4 + 3] * P.m[3][c];
    }
    std::vector<uint8_t> squeezed; // screen-space lines: squeezed to 16:9 as the other 2D (squeeze_169)
    if (rhw) {
        squeezed.assign((const uint8_t *)data, (const uint8_t *)data + (size_t)count * stride);
        squeeze_169(squeezed.data(), count, stride, sx, sy, ctx.fvf);
        data = squeezed.data();
    }
    struct SV { float x, y, z, rhw; uint32_t c; bool ok; };
    std::vector<SV> sv(count);
    const Viewport &vp = g_st.viewport;
    for (uint32_t i = 0; i < count; i++) {
        const float *v = (const float *)((const uint8_t *)data + (size_t)i * stride);
        SV &o = sv[i];
        if (rhw) {
            o = {v[0] * sx, v[1] * sy, v[2], v[3], *(const uint32_t *)(v + 4), true};
            continue;
        }
        float cx = v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12];
        float cy = v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13];
        float cz = v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14];
        float cw = v[0] * m[3] + v[1] * m[7] + v[2] * m[11] + m[15];
        o.ok = cw > 1e-6f;
        if (!o.ok) continue;
        float nx = cx / cw, ny = cy / cw, nz = cz / cw;
        o.x = ((nx * 0.5f + 0.5f) * vp.Width + vp.X) * sx;
        o.y = ((0.5f - ny * 0.5f) * vp.Height + vp.Y) * sy;
        o.z = vp.MinZ + nz * (vp.MaxZ - vp.MinZ);
        o.rhw = 1.0f / cw;
        o.c = *(const uint32_t *)(v + 3);
    }
    struct QV { float x, y, z, rhw; uint32_t c; };
    std::vector<QV> q;
    q.reserve(count * 6);
    auto segment = [&](const SV &a, const SV &b) {
        if (!a.ok || !b.ok) return;
        float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
        float nx, ny;
        if (len < 1e-3f) { nx = half; ny = 0; }
        else { nx = -dy / len * half; ny = dx / len * half; }
        QV a0 = {a.x + nx, a.y + ny, a.z, a.rhw, a.c}, a1 = {a.x - nx, a.y - ny, a.z, a.rhw, a.c};
        QV b0 = {b.x + nx, b.y + ny, b.z, b.rhw, b.c}, b1 = {b.x - nx, b.y - ny, b.z, b.rhw, b.c};
        q.push_back(a0); q.push_back(b0); q.push_back(a1);
        q.push_back(a1); q.push_back(b0); q.push_back(b1);
    };
    if (xprim == XPT_LINELIST) {
        for (uint32_t i = 0; i + 1 < count; i += 2) segment(sv[i], sv[i + 1]);
    } else {
        for (uint32_t i = 0; i + 1 < count; i++) segment(sv[i], sv[i + 1]);
        if (xprim == XPT_LINELOOP) segment(sv[count - 1], sv[0]);
    }
    ClipRhw clip;
    if (q.empty()) return true;
    set_fvf(d, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(q.size() / 3), q.data(), sizeof(QV));
    set_fvf(d, ctx.fvf);
    return true;
}

// ---- HLE draw functions ----

void __stdcall D3DDevice_DrawVerticesUP(uint32_t PrimitiveType, uint32_t VertexCount, const void *pVertexStreamZeroData,
                                        uint32_t VertexStreamZeroStride)
{
    DTRACE("%u, %u, %p, %u", PrimitiveType, VertexCount, pVertexStreamZeroData, VertexStreamZeroStride);
    pb_flush();
    perf::Scope perf_scope(perf::kDraw);
    DrawCtx ctx;
    if (!begin_draw(ctx, PrimitiveType)) return;
    if (ctx.path == Path::Prog) {
        uint32_t stride = convert_vertices(ctx.vs, 0, VertexCount, (const uint8_t *)pVertexStreamZeroData, VertexStreamZeroStride);
        debug_draw("DrawVerticesUP", ctx, PrimitiveType, VertexCount, g_conv.data(), stride / 4);
        set_decl(host3d::dev(), decl_for(ctx.vs));
        draw_up(PrimitiveType, VertexCount, g_conv.data(), stride);
        return;
    }
    if (ctx.vs) { // fixed-function vertices through a declaration
        uint32_t stride = convert_decl_vertices(ctx.vs, ctx.fvf, 0, VertexCount, (const uint8_t *)pVertexStreamZeroData,
                                                VertexStreamZeroStride);
        debug_draw("DrawVerticesUP(decl)", ctx, PrimitiveType, VertexCount, (const float *)g_declconv.data(), stride / 4);
        draw_up(PrimitiveType, VertexCount, g_declconv.data(), stride);
        return;
    }
    debug_draw("DrawVerticesUP", ctx, PrimitiveType, VertexCount, (const float *)pVertexStreamZeroData, VertexStreamZeroStride / 4);
    if (draw_wide_lines(ctx, PrimitiveType, VertexCount, pVertexStreamZeroData, VertexStreamZeroStride)) return;
    draw_up(PrimitiveType, VertexCount,
            scale_rhw(ctx, pVertexStreamZeroData, 0, VertexCount, VertexStreamZeroStride, quad_prim(PrimitiveType)), VertexStreamZeroStride);
}

void __stdcall D3DDevice_DrawVertices(uint32_t PrimitiveType, uint32_t StartVertex, uint32_t VertexCount)
{
    DTRACE("%u, %u, %u", PrimitiveType, StartVertex, VertexCount);
    pb_flush();
    perf::Scope perf_scope(perf::kDraw);
    DrawCtx ctx;
    if (!begin_draw(ctx, PrimitiveType)) return;
    if (ctx.path == Path::Prog) {
        uint32_t stride = convert_vertices(ctx.vs, StartVertex, VertexCount, nullptr, 0);
        debug_draw("DrawVertices", ctx, PrimitiveType, VertexCount, g_conv.data(), stride / 4);
        set_decl(host3d::dev(), decl_for(ctx.vs));
        draw_up(PrimitiveType, VertexCount, g_conv.data(), stride);
        return;
    }
    if (ctx.vs) { // fixed-function vertices through a declaration
        uint32_t stride = convert_decl_vertices(ctx.vs, ctx.fvf, StartVertex, VertexCount, nullptr, 0);
        debug_draw("DrawVertices(decl)", ctx, PrimitiveType, VertexCount, (const float *)g_declconv.data(), stride / 4);
        draw_up(PrimitiveType, VertexCount, g_declconv.data(), stride);
        return;
    }
    debug_draw("DrawVertices", ctx, PrimitiveType, VertexCount, nullptr, 0);
    const uint8_t *base = stream_data(0);
    uint32_t stride = g_st.streams[0].stride;
    if (!base || !stride) return;
    draw_up(PrimitiveType, VertexCount,
            scale_rhw(ctx, base, StartVertex, VertexCount, stride, quad_prim(PrimitiveType)) + (size_t)StartVertex * stride, stride);
}

// --debug-frame: for indexed draws whose declaration has a packed normal (NORMPACKED3), write indices,
// positions and the raw normal dwords to a CSV, to check the normal decoding against the geometry.
void dump_packed_normal_geometry(const VertexShader *vs, uint32_t prim, uint32_t count, const uint16_t *idx, uint32_t mn, uint32_t mx)
{
    const VertexElement *pos = nullptr, *nrm = nullptr;
    for (uint32_t i = 0; i < vs->num_elements; i++) {
        if (vs->elements[i].reg == 0) pos = &vs->elements[i];
        if (vs->elements[i].xtype == 0x16) nrm = &vs->elements[i];
    }
    if (!pos || !nrm || pos->xtype != 0x32 || g_cfg.screenshot_dir.empty()) return;
    char name[64];
    snprintf(name, sizeof name, "\\f%u_d%u_geom.csv", g_frame_count.load() + 1, g_draw_serial);
    FILE *f = _wfopen((g_cfg.screenshot_dir + widen(name)).c_str(), L"w");
    if (!f) return;
    fprintf(f, "prim,%u\n", prim);
    for (uint32_t i = 0; i < count && i < 3000; i++) fprintf(f, "i,%u\n", idx[i]);
    auto data = [&](const VertexElement *e) {
        const uint8_t *b = stream_data(e->stream);
        uint32_t st = g_st.streams[e->stream].stride ? g_st.streams[e->stream].stride : vs->stream_stride[e->stream];
        return std::make_pair(b, st);
    };
    auto [pb, ps] = data(pos);
    auto [nb, ns] = data(nrm);
    if (pb && nb)
        for (uint32_t v = mn; v <= mx && v < mn + 4000; v++) {
            float p[3];
            uint32_t n;
            memcpy(p, pb + (size_t)v * ps + pos->offset, 12);
            memcpy(&n, nb + (size_t)v * ns + nrm->offset, 4);
            fprintf(f, "v,%u,%g,%g,%g,0x%08x\n", v, p[0], p[1], p[2], n);
        }
    fclose(f);
}

void __stdcall D3DDevice_DrawIndexedVertices(uint32_t PrimitiveType, uint32_t VertexCount, const uint16_t *pIndexData)
{
    DTRACE("%u, %u, %p", PrimitiveType, VertexCount, pIndexData);
    pb_flush();
    perf::Scope perf_scope(perf::kDraw);
    DrawCtx ctx;
    if (!begin_draw(ctx, PrimitiveType) || !pIndexData || !VertexCount) return;
    if (ctx.path == Path::Prog) {
        uint32_t mn = 0xFFFF, mx = 0;
        for (uint32_t i = 0; i < VertexCount; i++) {
            if (pIndexData[i] < mn) mn = pIndexData[i];
            if (pIndexData[i] > mx) mx = pIndexData[i];
        }
        uint32_t stride = convert_vertices(ctx.vs, mn, mx - mn + 1, nullptr, 0);
        debug_draw("DrawIndexedVertices", ctx, PrimitiveType, VertexCount, g_conv.data(), stride / 4);
        if (debug_frame_active() && g_cfg.dump_vs_texmodes && g_st.pixel_shader &&
            render_state()[XRS_PSTEXTUREMODES] == g_cfg.dump_vs_texmodes && !g_cfg.screenshot_dir.empty()) {
            // diagnosis (cfg:dumpvs:MODES): the draw's vertex program inputs to replay it on the CPU: header, the 192
            // constants, the indices, the converted vertices [mn, mx]
            static int n;
            wchar_t name[64];
            swprintf(name, 64, L"\\vsdump_%u_%d.bin", g_frame_count.load(), n++);
            if (FILE *f = _wfopen((g_cfg.screenshot_dir + name).c_str(), L"wb")) {
                uint32_t hdr[6] = {0x31535644, PrimitiveType, VertexCount, mn, mx, stride};
                fwrite(hdr, 4, 6, f);
                fwrite(g_st.vs_constants, 4, 192 * 4, f);
                fwrite(pIndexData, 2, VertexCount, f);
                fwrite(g_conv.data(), 1, (size_t)(mx - mn + 1) * stride, f);
                fclose(f);
                logf("[draw]   vertex program inputs dumped (%u indices, vertices %u-%u, stride %u)", VertexCount, mn, mx, stride);
            }
        }
        if (debug_frame_active() && !g_cfg.debug_frame_end) // single debug frames only (ranges stay fast)
            dump_packed_normal_geometry(ctx.vs, PrimitiveType, VertexCount, pIndexData, mn, mx);
        set_decl(host3d::dev(), decl_for(ctx.vs));
        // indices refer to vertex 0: point the data "mn vertices before" the converted block
        draw_indexed_up(PrimitiveType, VertexCount, pIndexData, (const uint8_t *)g_conv.data() - (size_t)mn * stride, stride);
        return;
    }
    if (ctx.vs) { // fixed-function vertices through a declaration
        uint32_t mn = 0xFFFF, mx = 0;
        for (uint32_t i = 0; i < VertexCount; i++) {
            mn = pIndexData[i] < mn ? pIndexData[i] : mn;
            mx = pIndexData[i] > mx ? pIndexData[i] : mx;
        }
        uint32_t stride = convert_decl_vertices(ctx.vs, ctx.fvf, mn, mx - mn + 1, nullptr, 0);
        debug_draw("DrawIndexedVertices(decl)", ctx, PrimitiveType, VertexCount, (const float *)g_declconv.data(), stride / 4);
        draw_indexed_up(PrimitiveType, VertexCount, pIndexData, g_declconv.data() - (size_t)mn * stride, stride);
        return;
    }
    const uint8_t *base = stream_data(0);
    uint32_t stride = g_st.streams[0].stride;
    if (!base || !stride) return;
    if (ctx.path == Path::Fvf && (ctx.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
        uint32_t mn = 0xFFFF, mx = 0;
        for (uint32_t i = 0; i < VertexCount; i++) {
            mn = pIndexData[i] < mn ? pIndexData[i] : mn;
            mx = pIndexData[i] > mx ? pIndexData[i] : mx;
        }
        base = scale_rhw(ctx, base, mn, mx - mn + 1, stride, quad_prim(PrimitiveType) || PrimitiveType == XPT_TRIANGLELIST);
    }
    if (debug_frame_active()) {
        uint32_t mn = 0xFFFF;
        for (uint32_t i = 0; i < VertexCount; i++) mn = pIndexData[i] < mn ? pIndexData[i] : mn;
        debug_draw("DrawIndexedVertices(fvf)", ctx, PrimitiveType, VertexCount, (const float *)(base + (size_t)mn * stride), stride / 4);
    }
    draw_indexed_up(PrimitiveType, VertexCount, pIndexData, base, stride);
}

// ---- push-buffer draws (immediate mode / inline arrays) ----

void draw_immediate(uint32_t prim, const float *verts, uint32_t count, uint32_t attr_mask)
{
    perf::Scope perf_scope(perf::kDraw);
    DrawCtx ctx;
    if (!begin_draw(ctx, prim)) return;
    if (ctx.path == Path::Prog) { // the 16 attribute slots are exactly the shader's v0-v15
        debug_draw("immediate", ctx, prim, count, verts, 64);
        uint32_t mask = (attr_mask | vs_input_mask(ctx.vs) | 1u) & 0xFFFF, n = 0;
        uint8_t slot[16];
        for (uint32_t i = 0; i < 16; i++)
            if (mask & (1u << i)) slot[n++] = (uint8_t)i;
        if (n == 16) { // every slot: no smaller layout (not seen so far)
            KNOTE_ONCE("immediate-mode draw with all 16 attributes (256-byte vertices)");
            set_decl(host3d::dev(), decl_imm16());
            draw_up(prim, count, verts, 256);
            return;
        }
        KNOTE_ONCE("immediate-mode draws send %u of 16 attributes (mask 0x%x, %u-byte vertices)", n, mask, n * 16);
        if (g_imm_pack.size() < (size_t)count * n * 4) g_imm_pack.resize((size_t)count * n * 4);
        float *o = g_imm_pack.data();
        for (uint32_t v = 0; v < count; v++)
            for (uint32_t k = 0; k < n; k++, o += 4) memcpy(o, verts + v * 64 + slot[k] * 4, 16);
        set_decl(host3d::dev(), decl_imm(mask));
        draw_up(prim, count, g_imm_pack.data(), n * 16);
        return;
    }
    uint32_t fvf = ctx.fvf;
    // Rebuild the vertices in the current FVF layout from the 16 attribute slots
    // (0 position, 2 normal, 3 diffuse, 4 specular, 9.. texture coordinates).
    uint32_t stride = fvf_stride(fvf);
    std::vector<uint8_t> buf(stride * count);
    for (uint32_t v = 0; v < count; v++) {
        const float(*a)[4] = (const float(*)[4])(verts + v * 64);
        uint8_t *o = &buf[v * stride];
        auto putf = [&](float f) { memcpy(o, &f, 4); o += 4; };
        auto putc = [&](const float *c) {
            auto b = [](float x) { int i = (int)(x * 255.0f + 0.5f); return (uint32_t)(i < 0 ? 0 : i > 255 ? 255 : i); };
            uint32_t col = b(c[3]) << 24 | b(c[0]) << 16 | b(c[1]) << 8 | b(c[2]);
            memcpy(o, &col, 4);
            o += 4;
        };
        switch (fvf & D3DFVF_POSITION_MASK) {
        case D3DFVF_XYZRHW: // Xbox screen pixels -> render scale
            putf(a[0][0] * target_scale_x()); putf(a[0][1] * target_scale_y()); putf(a[0][2]); putf(a[0][3]);
            break;
        default: putf(a[0][0]); putf(a[0][1]); putf(a[0][2]); break;
        }
        if (fvf & D3DFVF_NORMAL) { putf(a[2][0]); putf(a[2][1]); putf(a[2][2]); }
        if (fvf & D3DFVF_DIFFUSE) putc(a[3]);
        if (fvf & D3DFVF_SPECULAR) putc(a[4]);
        uint32_t tc = (fvf >> 8) & 0xF;
        for (uint32_t i = 0; i < tc; i++) {
            static const uint32_t n[4] = {2, 3, 4, 1};
            uint32_t k = n[(fvf >> (16 + 2 * i)) & 3];
            for (uint32_t c = 0; c < k; c++) putf(a[9 + i][c]);
        }
    }
    if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
        RhwTexScale ts[4];
        apply_rhw_tex_scales(buf.data(), count, stride, ts, rhw_tex_scales(fvf, ts));
    }
    draw_up(prim, count, buf.data(), stride);
}

void draw_inline_array(uint32_t prim, const uint32_t *data, uint32_t dwords)
{
    perf::Scope perf_scope(perf::kDraw);
    DrawCtx ctx;
    if (!begin_draw(ctx, prim)) return;
    if (ctx.path == Path::Prog) { // inline data uses the stream-0 layout of the declaration
        uint32_t in_stride = ctx.vs->stream_stride[0];
        if (!in_stride) return;
        uint32_t count = dwords * 4 / in_stride;
        uint32_t stride = convert_vertices(ctx.vs, 0, count, (const uint8_t *)data, in_stride);
        set_decl(host3d::dev(), decl_for(ctx.vs));
        draw_up(prim, count, g_conv.data(), stride);
        return;
    }
    if (ctx.vs) { // fixed-function vertices through a declaration: the same stream-0 layout
        uint32_t in_stride = ctx.vs->stream_stride[0];
        if (!in_stride) return;
        uint32_t count = dwords * 4 / in_stride;
        uint32_t stride = convert_decl_vertices(ctx.vs, ctx.fvf, 0, count, (const uint8_t *)data, in_stride);
        draw_up(prim, count, g_declconv.data(), stride);
        return;
    }
    uint32_t stride = fvf_stride(ctx.fvf);
    if (!stride) return;
    draw_up(prim, dwords * 4 / stride, scale_rhw(ctx, data, 0, dwords * 4 / stride, stride, quad_prim(prim)), stride);
}

// ---- screenshots ----

void screenshot_now(const char *tag)
{
    if (g_cfg.screenshot_dir.empty() || !host3d::dev()) return;
    CreateDirectoryW(g_cfg.screenshot_dir.c_str(), nullptr);
    std::wstring path = g_cfg.screenshot_dir + L"\\" + widen(tag) + L".png";
    bool ok = save_screenshot(path.c_str());
    logf("screenshot %s: %s", narrow(path).c_str(), ok ? "saved" : "FAILED");
}

bool save_screenshot(const wchar_t *path)
{
    IDirect3DDevice9 *d = host3d::dev();
    if (!d) return false;
    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    RECT frame, pic; // the picture as shown in the window, without black bars
    host3d::frame_layout(&frame, &pic);
    bool ok = save_surface_png(bb, path, false, &pic);
    bb->Release();
    return ok;
}

// A render target surface (32-bit) as PNG: colour (opaque), or with alpha_as_gray its alpha channel;
// only the `crop` rectangle when given.
bool save_surface_png(IDirect3DSurface9 *src, const wchar_t *path, bool alpha_as_gray, const RECT *crop)
{
    IDirect3DDevice9 *d = host3d::dev();
    IDirect3DSurface9 *sys = nullptr;
    D3DSURFACE_DESC desc;
    src->GetDesc(&desc);
    D3DFORMAT format = desc.Format;
    UINT full_w = desc.Width, full_h = desc.Height;
    if (crop) {
        desc.Width = crop->right - crop->left;
        desc.Height = crop->bottom - crop->top;
    }
    bool ok = false;
    HRESULT hr_sys = d->CreateOffscreenPlainSurface(full_w, full_h, format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    HRESULT hr_get = SUCCEEDED(hr_sys) ? d->GetRenderTargetData(src, sys) : hr_sys;
    if (FAILED(hr_get))
        logf("screenshot: CreateOffscreenPlainSurface 0x%08lx, GetRenderTargetData 0x%08lx (%ux%u fmt %u)", hr_sys, hr_get, full_w, full_h, format);
    if (SUCCEEDED(hr_sys) && SUCCEEDED(hr_get)) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, crop, D3DLOCK_READONLY))) {
            std::vector<uint8_t> px(desc.Width * desc.Height * 4);
            for (UINT y = 0; y < desc.Height; y++) {
                memcpy(&px[y * desc.Width * 4], (uint8_t *)lr.pBits + y * lr.Pitch, desc.Width * 4);
                for (UINT x = 0; x < desc.Width; x++) {
                    uint8_t *p = &px[(y * desc.Width + x) * 4];
                    if (alpha_as_gray) p[0] = p[1] = p[2] = p[3];
                    p[3] = 0xFF;
                }
            }
            sys->UnlockRect();
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            IWICImagingFactory *f = nullptr;
            IWICStream *st = nullptr;
            IWICBitmapEncoder *enc = nullptr;
            IWICBitmapFrameEncode *fr = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
                SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) &&
                SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
                SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(desc.Width, desc.Height))) {
                WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
                fr->SetPixelFormat(&pf);
                ok = SUCCEEDED(fr->WritePixels(desc.Height, desc.Width * 4, (UINT)px.size(), px.data())) &&
                     SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
            }
            if (fr) fr->Release();
            if (enc) enc->Release();
            if (st) st->Release();
            if (f) f->Release();
        }
    }
    if (sys) sys->Release();
    return ok;
}

// ---- table ----

#define H(addr, fn, params, ret) {addr, #fn, (void *)&fn, params, ret}

static const Hle kTable[] = {
    H(0x00269c90, D3DDevice_DrawVerticesUP, "s s s s", false),
    H(0x00269e40, D3DDevice_DrawVertices, "s s s", false),
    H(0x00269f10, D3DDevice_DrawIndexedVertices, "s s s", false),
};

const Hle *hle_draw_table(size_t *count)
{
    *count = sizeof kTable / sizeof kTable[0];
    return kTable;
}

static const Hle kNgbTable[] = { // Ninja Gaiden Black x2.1 (tools/gen_ngb_hle.py)
#include "ngb/draw_table.inc"
};

const Hle *hle_draw_table_ngb(size_t *count)
{
    *count = sizeof kNgbTable / sizeof kNgbTable[0];
    return kNgbTable;
}

void invalidate_render_state(uint32_t state)
{
    if (state < XRS_MAX) g_rs_valid[state] = false;
}

void release_draw_buffers()
{
    if (g_ring_vb) g_ring_vb->Release();
    if (g_ring_ib) g_ring_ib->Release();
    g_ring_vb = nullptr;
    g_ring_ib = nullptr;
    g_ring_vpos = g_ring_ipos = 0;
}

void draw_drop_all()
{
    g_ms_aa_sent = -1;
    release_draw_buffers();
    g_ring_failed = false; // tried again on the new device
    for (auto &kv : g_decls)
        if (kv.second) kv.second->Release();
    g_decls.clear();
    for (auto &kv : g_decl_imm)
        if (kv.second) kv.second->Release();
    g_decl_imm.clear();
    if (g_decl_imm16) g_decl_imm16->Release();
    g_decl_imm16 = nullptr;
    g_white.release();
    forget_sent_state();
}

void decl_forget(const VertexShader *vs) { forget_decl(vs); }

#include "auxtab/draw_tables.inc"

} // namespace xd3d
