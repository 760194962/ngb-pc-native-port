#include "d3d/pshader.h"

#include "d3d/device.h"
#include "d3d/ffp_ps.h"
#include "d3d/host.h"
#include "perf.h"
#include "d3d/nv2a_psh.h"
#include "d3d/shader_cache.h"
#include "d3d/state.h"
#include "d3d/vshader.h"

#include <d3dcompiler.h>

#include <cstring>
#include <string>
#include <unordered_map>

namespace xd3d {
namespace {

std::unordered_map<std::string, IDirect3DPixelShader9 *> g_cache; // key: program dwords (no constants) + cube mask
uint32_t g_last_key[48], g_last_n; // the previous draw's key and shader: most draws keep it
IDirect3DPixelShader9 *g_last_ps;

uint32_t cube_mask()
{
    uint32_t m = 0;
    for (int s = 0; s < 4; s++) {
        Resource *r = g_st.textures[s];
        if (r && resource_type(r) == RTYPE_TEXTURE && (((PixelContainer *)r)->Format & FMT_CUBEMAP)) m |= 1u << s;
    }
    return m;
}

// Stages whose texture is a depth-format header (D24S8 .. LIN_F16): the NV2A samples those as shadow maps.
uint32_t depth_mask()
{
    uint32_t m = 0;
    for (int s = 0; s < 4; s++) {
        Resource *r = g_st.textures[s];
        if (!r || resource_type(r) != RTYPE_TEXTURE) continue;
        uint32_t fmt = (((PixelContainer *)r)->Format >> FMT_FORMAT_SHIFT) & 0xFF;
        if (fmt >= 0x2A && fmt <= 0x31) m |= 1u << s;
    }
    return m;
}

uint32_t pack_color(const float *v)
{
    auto c = [](float f) {
        f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
        return (uint32_t)(f * 255.0f + 0.5f);
    };
    return (c(v[3]) << 24) | (c(v[0]) << 16) | (c(v[1]) << 8) | c(v[2]);
}

void unpack_color(uint32_t c, float *o)
{
    o[0] = ((c >> 16) & 255) / 255.0f;
    o[1] = ((c >> 8) & 255) / 255.0f;
    o[2] = (c & 255) / 255.0f;
    o[3] = (c >> 24) / 255.0f;
}

void log_def(const PixelShaderDef &d)
{
    logf("[psh] combiners 0x%x texmodes 0x%x dotmap 0x%x inputtex 0x%x compare 0x%x final 0x%08x 0x%08x", d.PSCombinerCount,
         d.PSTextureModes, d.PSDotMapping, d.PSInputTexture, d.PSCompareMode, d.PSFinalCombinerInputsABCD, d.PSFinalCombinerInputsEFG);
    for (uint32_t i = 0; i < (d.PSCombinerCount & 0xFF) && i < 8; i++)
        logf("[psh]   stage %u: rgb in 0x%08x out 0x%08x | alpha in 0x%08x out 0x%08x", i, d.PSRGBInputs[i], d.PSRGBOutputs[i],
             d.PSAlphaInputs[i], d.PSAlphaOutputs[i]);
}

} // namespace

const PixelShaderDef *ps_def(uint32_t handle); // d3d/device.cpp

IDirect3DPixelShader9 *ps_host_shader()
{
    const uint32_t *rs = render_state();
    PixelShaderDef def = {};
    memcpy(&def, rs, 57 * 4);
    def.PSTextureModes = rs[XRS_PSTEXTUREMODES];
    uint32_t cube = cube_mask();
    uint32_t depth = depth_mask();
    // The library fits the modes to the bound textures only for a shader with PS_GLOBALFLAGS_TEXMODE_ADJUST
    // (PSFinalCombinerConstants bit 8): SetPixelShader keeps that bit at device+0x78C (0x3C93AC) and FUN_003D0940,
    // which writes NV097_SET_SHADER_STAGE_PROGRAM, uses PSTextureModes as it is when the bit is clear (0x3D0AA1).
    // With it, per stage: no texture bound (SetTexture(NULL) marks the stage 0x80000000, 0x3C7A24) -> NONE, except
    // PASSTHRU 4, CLIPPLANE 5, DOT_ZW 0xA, DOTPRODUCT 0x11; PROJECT2D / PROJECT3D / CUBEMAP -> CUBEMAP for a cube
    // map, PROJECT3D for a depth format or a volume, else PROJECT2D; DOT_STR_3D / DOT_STR_CUBE -> CUBE for a cube
    // map, else 3D (0x3D0AC0-0x3D0B29). docs/07 #4, #5.
    const PixelShaderDef *src = ps_def(g_st.pixel_shader);
    bool adjust = src && (src->PSFinalCombinerConstants & 0x100);
    for (int s = 0; adjust && s < 4; s++) {
        uint32_t m = (def.PSTextureModes >> (5 * s)) & 0x1F, to = m;
        Resource *r = g_st.textures[s];
        if (!r) {
            if (m != 4 && m != 5 && m != 0xA && m != 0x11) to = 0;
        } else if (m >= 1 && m <= 3) {
            uint32_t fmt = resource_type(r) == RTYPE_TEXTURE ? ((PixelContainer *)r)->Format : 0;
            to = (cube >> s) & 1 ? 3 : ((depth >> s) & 1) || ((fmt >> FMT_DIMENSION_SHIFT) & 0xF) == 3 ? 2 : 1;
        } else if (m == 0xD || m == 0xE) {
            to = (cube >> s) & 1 ? 0xE : 0xD;
        }
        if (to != m) {
            def.PSTextureModes = (def.PSTextureModes & ~(0x1Fu << (5 * s))) | to << (5 * s);
            static int noted;
            if (noted++ < 8) logf("[psh] frame %u: stage %d texture mode %u -> %u for the bound texture (%s), as the library adjusts",
                                  g_frame_count.load(), s, m, to, r ? "texture" : "none");
        }
    }
    uint32_t shadow = depth ? (uint32_t)shadow_func_code() : 0;
    uint32_t kill = 0; // D3DTSS_ALPHAKILL per stage (texture_state 11)
    for (int s = 0; s < 4; s++)
        if (texture_state(s)[11]) kill |= 1u << s;
    // key: everything except the constant values (10-25, 43, 44)
    uint32_t key[48];
    uint32_t n = 0;
    for (uint32_t i = 0; i < 57; i++)
        if (!(i >= 10 && i <= 25) && i != 43 && i != 44) key[n++] = rs[i];
    key[n++] = def.PSTextureModes;
    uint32_t bump_div = bump_divide_stages();
    key[n++] = cube | depth << 8 | shadow << 16 | kill << 24;
    key[n++] = bump_div;
    if (n == g_last_n && !memcmp(key, g_last_key, n * 4)) return g_last_ps; // most draws keep the previous shader
    std::string k((const char *)key, n * 4);
    auto it = g_cache.find(k);
    if (it != g_cache.end()) {
        memcpy(g_last_key, key, n * 4);
        g_last_n = n;
        return g_last_ps = it->second;
    }

    for (int s = 0; s < 4; s++)
        if (((def.PSTextureModes >> (5 * s)) & 0x1F) == 5) // not seen in this game yet: note where it comes
            logf("[psh] frame %u: new shader with CLIPPLANE on stage %d, compare mode 0x%x (docs/07 #17)", g_frame_count.load(), s,
                 (def.PSCompareMode >> (4 * s)) & 0xF);
    for (int s = 1; s < 4; s++) {
        uint32_t m = (def.PSTextureModes >> (5 * s)) & 0x1F;
        if (m != 6 && m != 7) continue;
        // BUMPENVMAP: the previous stage's texels offset this stage's coordinates; whether they are signed decides
        // the offset (docs/07 #16). Note the formats and sign flags the game uses.
        static int noted;
        auto fmt = [](int t) {
            Resource *r = g_st.textures[t];
            return r && resource_type(r) == RTYPE_TEXTURE ? (((PixelContainer *)r)->Format >> FMT_FORMAT_SHIFT) & 0xFF : 0u;
        };
        int in = s == 1 ? 0 : s == 2 ? (def.PSInputTexture >> 16) & 1 : (int)((def.PSInputTexture >> 20) & 3) % 3;
        if (noted++ < 8)
            logf("[psh] frame %u: stage %d BUMPENVMAP%s reads stage %d (format 0x%x, COLORSIGN 0x%x), own format 0x%x, "
                 "matrix %g %g %g %g", g_frame_count.load(), s, m == 7 ? "_LUM" : "", in, fmt(in), texture_state(in)[10],
                 fmt(s), *(const float *)&texture_state(s)[22], *(const float *)&texture_state(s)[23],
                 *(const float *)&texture_state(s)[24], *(const float *)&texture_state(s)[25]);
    }
    std::string hlsl = psh_to_hlsl(def, cube, depth, shadow, kill, bump_div);
    perf::Scope perf_scope(perf::kCompile);
    perf::add_compile();
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = E_FAIL;
    for (const char *profile : {"ps_2_0", "ps_2_a", "ps_2_b"}) {
        if (errors) { errors->Release(); errors = nullptr; }
        hr = cached_compile(hlsl, "nv2a_psh", profile, D3DCOMPILE_OPTIMIZATION_LEVEL2, &code, &errors);
        if (SUCCEEDED(hr)) break;
    }
    IDirect3DPixelShader9 *ps = nullptr;
    static int logged;
    if (FAILED(hr)) {
        logf("[psh] compile failed: %s", errors ? (const char *)errors->GetBufferPointer() : "?");
        log_def(def);
        static int dumped;
        if (dumped++ < 3) logf("[psh] HLSL:\n%s", hlsl.c_str());
    } else if (FAILED(create_pixel_shader(host3d::dev(), code, &ps))) {
        logf("[psh] CreatePixelShader failed");
        ps = nullptr;
    } else if (logged++ < 40) {
        logf("[psh] pixel shader #%zu translated (cube mask 0x%x, depth mask 0x%x, shadow func %u, texmodes 0x%x -> 0x%x, %s)",
             g_cache.size() + 1, cube, depth, shadow, rs[XRS_PSTEXTUREMODES], def.PSTextureModes,
             adjust ? "TEXMODE_ADJUST" : "modes as given");
        log_def(def);
    }
    if (code) code->Release();
    if (errors) errors->Release();
    g_cache[k] = ps; // failures are cached too (drawn with the fallback)
    memcpy(g_last_key, key, n * 4);
    g_last_n = n;
    return g_last_ps = ps;
}

void linear_texture_scale(int stage, float *sx, float *sy); // d3d/draw.cpp

// Census of the glow's kind of trick (see the glow blur below): a combiner program whose constants take one set on
// even frames and another on odd ones, a pair of frames meant to be seen as one on a 480i television. Each program is
// looked at once a frame (its first draw); after 8 frames in a row of A, B, A, B it is logged once with its constants.
void note_alternating_constants(const uint32_t *rs)
{
    struct Seen {
        uint32_t frame = ~0u, run = 0;
        uint64_t now = 0, last = 0, before = 0;
        bool logged = false;
    };
    static std::unordered_map<uint64_t, Seen> seen;
    static uint64_t last_program;
    static uint32_t last_frame = ~0u;
    uint32_t frame = g_frame_count.load(std::memory_order_relaxed);
    uint64_t prog = 1469598103934665603ull, consts = 1469598103934665603ull;
    auto mix = [](uint64_t &h, uint32_t v) { h = (h ^ v) * 1099511628211ull; };
    for (int i = 0; i < 10; i++) mix(prog, rs[i]);  // alpha inputs, final combiner
    for (int i = 26; i < 42; i++) mix(prog, rs[i]); // alpha outputs, rgb inputs
    for (int i = 45; i < 54; i++) mix(prog, rs[i]); // rgb outputs, control
    mix(prog, rs[XRS_PSTEXTUREMODES]);
    if (prog == last_program && frame == last_frame) return; // the same program again in this frame
    last_program = prog;
    last_frame = frame;
    for (int i = 10; i < 26; i++) mix(consts, rs[i]);
    mix(consts, rs[43]);
    mix(consts, rs[44]);
    Seen &e = seen[prog];
    if (e.frame == frame) return;
    if (e.frame + 1 != frame) e.run = 0; // not drawn in the frame before
    e.before = e.last;
    e.last = e.now;
    e.now = consts;
    e.frame = frame;
    e.run = (e.now != e.last && e.now == e.before) ? e.run + 1 : 0;
    if (e.run >= 8 && !e.logged) {
        e.logged = true;
        logf("[census] frame %u: combiner program 0x%x (texmodes 0x%x, stage 0 rgb in 0x%08x) alternates its constants every "
             "frame (c0 0x%08x c1 0x%08x now): a pair of frames meant to be seen as one", frame, rs[53], rs[XRS_PSTEXTUREMODES],
             rs[34], rs[XRS_PSCONSTANT0_0], rs[XRS_PSCONSTANT1_0]);
    }
}

void pshader_drop_all()
{
    for (auto &kv : g_cache)
        if (kv.second) kv.second->Release();
    g_cache.clear();
    g_last_n = 0;
    g_last_ps = nullptr;
}

void upload_ps_constants(IDirect3DDevice9 *d, bool vertex_fog)
{
    const uint32_t *rs = render_state();
    note_alternating_constants(rs);
    float c[27][4];
    for (int i = 0; i < 8; i++) {
        unpack_color(rs[XRS_PSCONSTANT0_0 + i], c[i]);
        unpack_color(rs[XRS_PSCONSTANT1_0 + i], c[8 + i]);
    }
    // Ninja Gaiden Black's glow blur (FUN_00314430: three combiner stages summing c0 * (taps 2 texels out) and c1 * (taps
    // 6 texels out), texture modes 0x8421) alternates its weights every frame: 0.25 / 0.25, then 0.33 / 0.17 (r423,
    // frames 9161-9164 traced in a row). On a 480i television the two frames merged into one soft glow; on a PC screen
    // a still lamp's halo flickered between wide and narrow at 30 Hz (the user, 2026-10-22). Both frames get the mean
    // kernel instead, 0.2922 / 0.2098: the glow they made together, every frame and without delay. (A mean of the
    // last two frames' results did the same for a still picture but left a second halo behind when the view turned.)
    if (rs[53] == 0x11103 && rs[XRS_PSTEXTUREMODES] == 0x8421 && rs[34] == 0xC1C8C2C9) {
        for (int i = 0; i < 8; i++) {
            uint32_t k0 = rs[XRS_PSCONSTANT0_0 + i], k1 = rs[XRS_PSCONSTANT1_0 + i];
            if ((k0 == 0x40404040 && k1 == 0x40404040) || (k0 == 0x55555555 && k1 == 0x2B2B2B2B)) {
                const float inner = 0.5f * (64.0f + 85.0f) / 255.0f, outer = 0.5f * (64.0f + 43.0f) / 255.0f;
                c[i][0] = c[i][1] = c[i][2] = c[i][3] = inner;
                c[8 + i][0] = c[8 + i][1] = c[8 + i][2] = c[8 + i][3] = outer;
            }
        }
    }
    unpack_color(rs[43], c[16]); // PSFinalCombinerConstant0
    unpack_color(rs[44], c[17]); // PSFinalCombinerConstant1
    unpack_color(rs[XRS_FOGCOLOR], c[18]);
    // FOG.a is the fog factor: from TEXCOORD4 after a translated vertex program; fixed-function vertices (FVF) write
    // no TEXCOORD4, and an unwritten input could read 0, a draw all in the fog colour, so their factor is 1 (no fog)
    c[18][3] = vertex_fog ? 1.0f : 0.0f;
    for (int s = 0; s < 4; s++) {
        const uint32_t *ts = texture_state(s);
        memcpy(&c[19 + s][0], &ts[22], 4); // BUMPENVMAT00
        memcpy(&c[19 + s][1], &ts[23], 4); // BUMPENVMAT01
        memcpy(&c[19 + s][2], &ts[25], 4); // BUMPENVMAT10
        memcpy(&c[19 + s][3], &ts[24], 4); // BUMPENVMAT11
    }
    for (int s = 0; s < 4; s++) { // c23-26: 1 / size of a linear texture (nv2a_psh.cpp rectScale)
        linear_texture_scale(s, &c[23 + s][0], &c[23 + s][1]);
        c[23 + s][2] = c[23 + s][3] = 1.0f;
    }
    d->SetPixelShaderConstantF(0, &c[0][0], 27);
    ffp_forget_constants(); // c0 was the fixed-function pixels' texture factor
}

void ps_set_constant(const PixelShaderDef *def, uint32_t reg, const float *v)
{
    uint32_t color = pack_color(v);
    uint32_t *rs = render_state();
    for (int i = 0; i < 8; i++) {
        if (((def->PSC0Mapping >> (4 * i)) & 0xF) == reg) rs[XRS_PSCONSTANT0_0 + i] = color;
        if (((def->PSC1Mapping >> (4 * i)) & 0xF) == reg) rs[XRS_PSCONSTANT1_0 + i] = color;
    }
    if ((def->PSFinalCombinerConstants & 0xF) == reg) rs[43] = color;
    if (((def->PSFinalCombinerConstants >> 4) & 0xF) == reg) rs[44] = color;
}

} // namespace xd3d
