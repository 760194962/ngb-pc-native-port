// Fixed-function texture stages as a pixel shader.
//
// The host's fixed-function pixel pipeline (D3D9On12 on this machine) dropped a stage's alpha MODULATE by its
// texture when the stage's colour op only passed CURRENT through: Ninja Gaiden Black's sword trail (stage 1:
// colour SELECTARG1 current, alpha MODULATE current x texture, its 128x128 DXT5 alpha ramp) came out as an opaque
// white band (docs/04 §3.9). Generating the stage program ourselves follows the Direct3D 8/9 stage semantics
// exactly, and reads an unbound stage's texture as opaque white (as the Xbox does; the nunchaku sweep). Fog and
// the alpha test stay with the host (applied after the shader), lighting, texture transforms and coordinate
// generation stay in the host's vertex pipeline.
#include "d3d/ffp_ps.h"

#include "d3d/host.h"
#include "d3d/shader_cache.h"
#include "d3d/state.h"
#include "common.h"
#include "perf.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace xd3d {

D3DTEXTUREOP conv_texop(uint32_t v); // draw.cpp: the Xbox op number -> D3DTEXTUREOP

uint32_t g_ffp_edge_stages;
uint32_t g_ffp_quad_stages, g_ffp_quad_weighted;
bool g_ffp_sprite_tc;

namespace {

std::unordered_map<std::string, IDirect3DPixelShader9 *> g_ffp_cache;
std::string g_ffp_last_key; // the previous draw's key and shader
IDirect3DPixelShader9 *g_ffp_last_ps;
float g_ffp_c0[4] = {-1, -1, -1, -1}; // the texture factor last put in c0 (upload_ffp_constants)

struct StageKey {
    uint32_t cop, c0, c1, c2, aop, a0, a1, a2, result;
    uint32_t tex;  // 0 none, 1 2D, 2 cube
    uint32_t kill; // D3DTSS_ALPHAKILL set (texture_state 11)
    uint32_t proj; // D3DTTFF_PROJECTED: 3 = divide by z, 4 = divide by w, 0 = none
    uint32_t edge; // g_ffp_edge_stages: alpha 0 where this stage's coordinates leave 0..1
    uint32_t quad; // g_ffp_quad_stages / g_ffp_quad_weighted: 1 coordinates clamped, 2 also alpha-weighted texels
};

// An argument (D3DTA_*) with its modifiers as an HLSL expression.
std::string arg(uint32_t a, int stage)
{
    std::string e;
    switch (a & 0xF) {
    case 0: e = "v0"; break;                                      // D3DTA_DIFFUSE
    case 1: e = "cur"; break;                                     // D3DTA_CURRENT
    case 2: e = "t" + std::to_string(stage); break;               // D3DTA_TEXTURE
    case 3: e = "tfactor"; break;                                 // D3DTA_TFACTOR
    case 4: e = "v1"; break;                                      // D3DTA_SPECULAR
    case 5: e = "temp"; break;                                    // D3DTA_TEMP
    default: e = "cur"; break;
    }
    if (a & 0x20) e = "(" + e + ").aaaa"; // D3DTA_ALPHAREPLICATE
    if (a & 0x10) e = "(1 - " + e + ")";  // D3DTA_COMPLEMENT
    return e;
}

// One operation on float4 operands (the caller picks .rgb or .a of the result).
std::string op_expr(D3DTEXTUREOP op, const std::string &a0, const std::string &a1, const std::string &a2, int stage, const std::string &cur_before)
{
    std::string t = "t" + std::to_string(stage);
    switch (op) {
    case D3DTOP_SELECTARG1: return a1;
    case D3DTOP_SELECTARG2: return a2;
    case D3DTOP_MODULATE: return "(" + a1 + " * " + a2 + ")";
    case D3DTOP_MODULATE2X: return "(" + a1 + " * " + a2 + " * 2)";
    case D3DTOP_MODULATE4X: return "(" + a1 + " * " + a2 + " * 4)";
    case D3DTOP_ADD: return "(" + a1 + " + " + a2 + ")";
    case D3DTOP_ADDSIGNED: return "(" + a1 + " + " + a2 + " - 0.5)";
    case D3DTOP_ADDSIGNED2X: return "((" + a1 + " + " + a2 + " - 0.5) * 2)";
    case D3DTOP_SUBTRACT: return "(" + a1 + " - " + a2 + ")";
    case D3DTOP_ADDSMOOTH: return "(" + a1 + " + " + a2 + " - " + a1 + " * " + a2 + ")";
    case D3DTOP_BLENDDIFFUSEALPHA: return "lerp(" + a2 + ", " + a1 + ", v0.a)";
    case D3DTOP_BLENDTEXTUREALPHA: return "lerp(" + a2 + ", " + a1 + ", " + t + ".a)";
    case D3DTOP_BLENDFACTORALPHA: return "lerp(" + a2 + ", " + a1 + ", tfactor.a)";
    case D3DTOP_BLENDCURRENTALPHA: return "lerp(" + a2 + ", " + a1 + ", " + cur_before + ".a)";
    case D3DTOP_BLENDTEXTUREALPHAPM: return "(" + a1 + " + " + a2 + " * (1 - " + t + ".a))";
    case D3DTOP_PREMODULATE: return a1;
    case D3DTOP_MODULATEALPHA_ADDCOLOR: return "(" + a1 + " + " + a1 + ".aaaa * " + a2 + ")";
    case D3DTOP_MODULATECOLOR_ADDALPHA: return "(" + a1 + " * " + a2 + " + " + a1 + ".aaaa)";
    case D3DTOP_MODULATEINVALPHA_ADDCOLOR: return "((1 - " + a1 + ".aaaa) * " + a2 + " + " + a1 + ")";
    case D3DTOP_MODULATEINVCOLOR_ADDALPHA: return "((1 - " + a1 + ") * " + a2 + " + " + a1 + ".aaaa)";
    case D3DTOP_DOTPRODUCT3: return "dot((" + a1 + ".rgb - 0.5) * 2, (" + a2 + ".rgb - 0.5) * 2).xxxx";
    case D3DTOP_MULTIPLYADD: return "(" + a0 + " + " + a1 + " * " + a2 + ")";
    case D3DTOP_LERP: return "lerp(" + a2 + ", " + a1 + ", " + a0 + ")";
    default: return cur_before; // BUMPENVMAP and unknown: pass through
    }
}

// Whether one channel's operation reads the stage texture: only the arguments the operation uses count, as in the
// Xbox D3D library's combiner builder (FUN_003d4340 of Ninja Gaiden Black x2.1, arguments mapped by FUN_003d42a0).
bool op_reads_texture(D3DTEXTUREOP op, uint32_t a0, uint32_t a1, uint32_t a2, bool first)
{
    auto tex = [](uint32_t a) { return (a & 0xF) == 2; }; // D3DTA_TEXTURE
    switch (op) {
    case D3DTOP_DISABLE: case D3DTOP_BUMPENVMAP: case D3DTOP_BUMPENVMAPLUMINANCE: return false;
    case D3DTOP_SELECTARG1: return tex(a1);
    case D3DTOP_SELECTARG2: return tex(a2);
    case D3DTOP_PREMODULATE: return tex(a1) || first; // the first stage modulates by its own texture
    case D3DTOP_BLENDTEXTUREALPHA: case D3DTOP_BLENDTEXTUREALPHAPM: return true;
    case D3DTOP_MULTIPLYADD: case D3DTOP_LERP: return tex(a0) || tex(a1) || tex(a2);
    default: return tex(a1) || tex(a2);
    }
}

std::string build_hlsl(const StageKey (&k)[4], bool specular, bool def_diffuse, bool def_specular, int first)
{
    std::string s =
        "float4 tfactor : register(c0);\n"
        "float4 qc[4] : register(c1); float4 qs[4] : register(c5);\n" // a 2D quad's clamp ranges and texture sizes
        "sampler2D s2d0 : register(s0); sampler2D s2d1 : register(s1); sampler2D s2d2 : register(s2); sampler2D s2d3 : register(s3);\n"
        "samplerCUBE scu0 : register(s0); samplerCUBE scu1 : register(s1); samplerCUBE scu2 : register(s2); samplerCUBE scu3 : register(s3);\n"
        "struct PSIn { float4 v0 : COLOR0; float4 v1 : COLOR1; float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1;\n"
        "              float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; };\n"
        "float4 main(PSIn I) : COLOR0 {\n"
        "  float4 tc[4] = { I.tc0, I.tc1, I.tc2, I.tc3 };\n";
    // Point sprites: the sprite stage (3) reads the coordinate every driver replaces with the sprite's, the first set
    // (the draw passes one vertex coordinate through every stage: d3d/draw.cpp with_sprite_tc)
    if (first == 3 && g_ffp_sprite_tc) s += "  tc[3] = I.tc0;\n";
    s += def_diffuse ? "  float4 v0 = 1;\n" : "  float4 v0 = I.v0;\n";  // no diffuse in the vertex: opaque white
    s += def_specular ? "  float4 v1 = 0;\n" : "  float4 v1 = I.v1;\n"; // no specular in the vertex: 0
    s += "  float4 cur = v0, temp = 0, prev;\n";
    // A stage that reads its texture with none bound (cfg:untex): 3 = that channel passes the current value on, as the
    // Xbox library does (below), 1 = the texture reads as the diffuse colour, 0 = opaque white. 3 matches xemu in four cases (docs/04
    // §3.14 of the port project): the health bar and the SNES trilogy page (stage 0 MODULATE(texture, diffuse): 1 gives
    // the diffuse colour squared), the Game Over overlay (stage 0 MODULATE(texture, tfactor), nearly transparent
    // diffuse: 0 gives a yellow layer) and the nunchaku sweep (stage 1, must show).
    if (g_cfg.untex == 1) s += "  float4 t0 = v0, t1 = v0, t2 = v0, t3 = v0;\n";
    else s += "  float4 t0 = 1, t1 = 1, t2 = 1, t3 = 1;\n";
    char b[256];
    for (int i = 0; i < 4; i++) {
        if (k[i].tex == 2) snprintf(b, sizeof b, "  t%d = texCUBE(scu%d, tc[%d].xyz);\n", i, i, i);
        else if (k[i].tex == 1 && k[i].proj == 4) snprintf(b, sizeof b, "  t%d = tex2Dproj(s2d%d, tc[%d]);\n", i, i, i);
        else if (k[i].tex == 1 && k[i].proj == 3) snprintf(b, sizeof b, "  t%d = tex2D(s2d%d, tc[%d].xy / tc[%d].z);\n", i, i, i, i);
        else if (k[i].tex == 1 && k[i].quad) {
            // A 2D quad above 1x (draw.cpp quad_snap): the coordinates no nearer the texture's edge than the console's
            // pixel centres read (the line along a seam of the pause menu's dagger, a font cell's or tile's edge)
            snprintf(b, sizeof b, "  { float2 uv = clamp(tc[%d].xy, qc[%d].xy, qc[%d].zw);\n", i, i, i);
            s += b;
            if (k[i].quad == 2) {
                // Linear filtering: the four texels around, weighted by their alpha, so a transparent texel's colour
                // does not fringe its opaque neighbour (between texels, where an upscaled picture samples; at the
                // console's pixel centres it is the same texel). An on-or-off mask (a cut-out's 0 and 255) takes the
                // nearest texel's alpha: its edge as sharp as the console's; soft alpha (text) stays filtered.
                // The four reads take plain tex2D. With explicit gradients (tex2Dgrad, those of the clamped coordinate:
                // the texel corners' jump) the pause menu's dozen full-screen layers at 10x kept an RTX 3060 77% busy,
                // 50% this way, 40% with no weighting (2026-11-05). The quad variant runs only above 1x, where 2D is
                // magnified: neighbouring pixels' corners differ by a texel at most, the level of detail stays at the
                // top level either way. cfg:quadgrad:1 reads with the gradients again, for comparison.
                std::string q =
                    "    float2 gx = ddx(uv), gy = ddy(uv), f = uv * qs[#].xy - 0.5, fb = floor(f), w = f - fb;\n"
                    "    float2 c = (fb + 0.5) * qs[#].zw;\n"
                    + std::string(g_cfg.quad_grad
                    ? "    float4 q0 = tex2Dgrad(s2d#, c, gx, gy), q1 = tex2Dgrad(s2d#, c + float2(qs[#].z, 0), gx, gy);\n"
                      "    float4 q2 = tex2Dgrad(s2d#, c + float2(0, qs[#].w), gx, gy), q3 = tex2Dgrad(s2d#, c + qs[#].zw, gx, gy);\n"
                    : "    float4 q0 = tex2D(s2d#, c), q1 = tex2D(s2d#, c + float2(qs[#].z, 0));\n"
                      "    float4 q2 = tex2D(s2d#, c + float2(0, qs[#].w)), q3 = tex2D(s2d#, c + qs[#].zw);\n") +
                    "    float4 wq = float4((1 - w.x) * (1 - w.y), w.x * (1 - w.y), (1 - w.x) * w.y, w.x * w.y);\n"
                    "    float4 qa = float4(q0.a, q1.a, q2.a, q3.a), aw = wq * qa;\n"
                    "    float sa = dot(aw, 1);\n"
                    "    float3 rgb = sa > 0 ? (q0.rgb * aw.x + q1.rgb * aw.y + q2.rgb * aw.z + q3.rgb * aw.w) / sa\n"
                    "                        : q0.rgb * wq.x + q1.rgb * wq.y + q2.rgb * wq.z + q3.rgb * wq.w;\n"
                    "    if (all((qa < 0.02) + (qa > 0.98))) sa = w.y < 0.5 ? (w.x < 0.5 ? q0.a : q1.a) : (w.x < 0.5 ? q2.a : q3.a);\n"
                    "    t# = float4(rgb, sa); }\n";
                for (size_t at; (at = q.find('#')) != std::string::npos;) q.replace(at, 1, std::to_string(i));
                s += q;
            } else {
                snprintf(b, sizeof b, "    t%d = tex2D(s2d%d, uv); }\n", i, i);
                s += b;
            }
            b[0] = 0;
        }
        else if (k[i].tex == 1) snprintf(b, sizeof b, "  t%d = tex2D(s2d%d, tc[%d].xy);\n", i, i, i);
        else continue;
        s += b;
        if (k[i].kill) { // see the note at StageKey's use in ffp_pixel_shader
            snprintf(b, sizeof b, "  clip(t%d.a > 0 ? 1 : -1);\n", i);
            s += b;
        }
    }
    for (int i = first; i < 4; i++) {
        D3DTEXTUREOP cop = conv_texop(k[i].cop), aop = conv_texop(k[i].aop);
        if (cop == D3DTOP_DISABLE) break; // the first disabled colour op ends the stages
        // cfg:untex:3: a channel whose operation reads an unbound texture passes the current value on (the first
        // stage: the diffuse colour). The Xbox library maps the missing texture to register -1 and replaces that
        // channel's combiner inputs with current * 1; colour and alpha are separate passes.
        bool skip_c = g_cfg.untex == 3 && !k[i].tex && op_reads_texture(cop, k[i].c0, k[i].c1, k[i].c2, i == first);
        bool skip_a = g_cfg.untex == 3 && !k[i].tex && op_reads_texture(aop, k[i].a0, k[i].a1, k[i].a2, i == first);
        s += "  prev = cur;\n";
        std::string c = skip_c ? std::string("prev") : op_expr(cop, arg(k[i].c0, i), arg(k[i].c1, i), arg(k[i].c2, i), i, "prev");
        std::string a = aop == D3DTOP_DISABLE || skip_a ? std::string("prev")
                                                         : op_expr(aop, arg(k[i].a0, i), arg(k[i].a1, i), arg(k[i].a2, i), i, "prev");
        // Two operations the Xbox library builds differently from Direct3D 9's description (FUN_003D4340):
        // PREMODULATE on the first stage is arg1 * this stage's texture (B input = TEXTURE when the first-stage flag is
        // set, 0x3D457C), on later stages arg1; DOTPRODUCT3 writes the dot product with AB blue-to-alpha and clears
        // the stage's alpha combiner, so the alpha is the dot product too.
        std::string ti = "t" + std::to_string(i);
        if (!skip_c && cop == D3DTOP_PREMODULATE && i == first) c = "(" + arg(k[i].c1, i) + " * " + ti + ")";
        if (!skip_a && aop == D3DTOP_PREMODULATE && i == first) a = "(" + arg(k[i].a1, i) + " * " + ti + ")";
        if (!skip_c && cop == D3DTOP_DOTPRODUCT3) a = c;
        const char *dst = (k[i].result & 0xF) == 5 ? "temp" : "cur";
        s += std::string("  ") + dst + " = saturate(float4((" + c + ").rgb, (" + a + ").a));\n";
    }
    for (int i = 0; i < 4; i++) {
        if (!k[i].edge) continue;
        snprintf(b, sizeof b, "  { float2 st = tc[%d].xy / tc[%d].%c; cur.a *= step(0, st.x) * step(st.x, 1) * step(0, st.y) * step(st.y, 1); }\n",
                 i, i, k[i].proj == 4 ? 'w' : 'z');
        s += b;
    }
    if (g_cfg.ffp_debug == 1 && k[1].tex) s += "  return float4(t1.rgb, 1);\n}\n";        // cfg:ffpdebug:1
    else if (g_cfg.ffp_debug == 2 && k[1].tex) s += "  return float4(t1.aaa, 1);\n}\n";   // cfg:ffpdebug:2
    else s += specular ? "  return float4(saturate(cur.rgb + v1.rgb), cur.a);\n}\n" : "  return cur;\n}\n";
    return s;
}

} // namespace

namespace {
bool g_last_diffuse, g_last_specular, g_last_pretransformed; // ffp_pixel_shader's last arguments (ffp_reselect)
}

IDirect3DPixelShader9 *ffp_pixel_shader(bool has_diffuse, bool has_specular, bool pretransformed)
{
    g_last_diffuse = has_diffuse;
    g_last_specular = has_specular;
    g_last_pretransformed = pretransformed;
    // lit: the vertex pipeline computes both colours; pre-transformed vertices are never lit (Ninja Gaiden Black's
    // movie quad: XYZRHW|TEX1 with LIGHTING on, MODULATE(texture, diffuse) -> diffuse is the opaque white default)
    bool lighting = render_state()[XRS_LIGHTING] != 0 && !pretransformed;
    bool def_diffuse = !lighting && !has_diffuse, def_specular = !lighting && !has_specular;
    StageKey k[4] = {};
    for (int i = 0; i < 4; i++) {
        const uint32_t *ts = texture_state(i);
        StageKey &e = k[i];
        e.cop = ts[12]; e.c0 = ts[13]; e.c1 = ts[14]; e.c2 = ts[15];
        e.aop = ts[16]; e.a0 = ts[17]; e.a1 = ts[18]; e.a2 = ts[19];
        e.result = ts[20];
        Resource *r = g_st.textures[i];
        e.tex = !r ? 0 : (resource_type(r) == RTYPE_TEXTURE && (((PixelContainer *)r)->Format & FMT_CUBEMAP)) ? 2 : 1;
        e.proj = (ts[21] & 0x100) ? (ts[21] & 0xFF) : 0;
        // D3DTSS_ALPHAKILL (Xbox texture stage state 11, D3DTALPHAKILL_ENABLE = 4) sets the NV2A ALPHAKILLEN of the
        // stage: a fetched texel with alpha exactly 0 discards the pixel (xemu psh.c: "if (tN.a == 0.0) discard"),
        // whatever the combiners do. Ninja Gaiden Black enables it on stage 1 of a projected-texture pass
        // (FUN_00236010 at 0x2370D8).
        e.kill = e.tex && ts[11] != 0;
        e.edge = e.tex == 1 && e.proj && ((g_ffp_edge_stages >> i) & 1);
        e.quad = e.tex == 1 && !e.proj && ((g_ffp_quad_stages >> i) & 1) ? 1 + ((g_ffp_quad_weighted >> i) & 1) : 0;
    }
    bool specular = render_state()[XRS_SPECULARENABLE] != 0;
    // Point sprites: the Xbox D3D library builds the combiner stages from stage 3 on (FUN_003d4340 of Ninja Gaiden
    // Black x2.1 starts its stage loop at 3 when D3DRS_POINTSPRITEENABLE is set), the stage the NV2A gives the sprite
    // coordinates; stages 0-2 take no part. Ninja Gaiden Black's particles keep a fire texture in stage 0 and the
    // round dot (16x16) in stage 3: colour = diffuse, alpha = dot * diffuse.
    int first = render_state()[XRS_POINTSPRITEENABLE] ? 3 : 0;
    std::string key((const char *)k, sizeof k);
    key.push_back((char)first);
    key.push_back((char)(first == 3 && g_ffp_sprite_tc));
    key.push_back((char)((specular ? 1 : 0) | (def_diffuse ? 2 : 0) | (def_specular ? 4 : 0)));
    key.push_back((char)g_cfg.ffp_debug);
    key.push_back((char)g_cfg.untex);
    key.push_back((char)g_cfg.quad_grad);
    if (key == g_ffp_last_key) return g_ffp_last_ps;
    auto it = g_ffp_cache.find(key);
    if (it != g_ffp_cache.end()) {
        g_ffp_last_key = key;
        return g_ffp_last_ps = it->second;
    }
    std::string hlsl = build_hlsl(k, specular, def_diffuse, def_specular, first);
    perf::Scope perf_scope(perf::kCompile);
    perf::add_compile();
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = cached_compile(hlsl, "ffp_ps", "ps_2_0", D3DCOMPILE_OPTIMIZATION_LEVEL2, &code, &errors);
    if (FAILED(hr)) {
        if (errors) errors->Release();
        hr = cached_compile(hlsl, "ffp_ps", "ps_2_a", D3DCOMPILE_OPTIMIZATION_LEVEL2, &code, &errors);
    }
    IDirect3DPixelShader9 *ps = nullptr;
    static int logged;
    if (FAILED(hr)) {
        logf("[ffp] stage shader compile failed: %s", errors ? (const char *)errors->GetBufferPointer() : "?");
        if (logged++ < 3) logf("[ffp] HLSL:\n%s", hlsl.c_str());
    } else if (FAILED(create_pixel_shader(host3d::dev(), code, &ps))) {
        logf("[ffp] CreatePixelShader failed");
        ps = nullptr;
    } else if (logged++ < 12) {
        logf("[ffp] stage shader #%zu: stage0 cop %u(%x,%x,%x) aop %u(%x,%x,%x) tex %u | stage1 cop %u(%x,%x,%x) aop %u(%x,%x,%x) tex %u | specular %d",
             g_ffp_cache.size() + 1, k[0].cop, k[0].c0, k[0].c1, k[0].c2, k[0].aop, k[0].a0, k[0].a1, k[0].a2, k[0].tex, k[1].cop, k[1].c0, k[1].c1,
             k[1].c2, k[1].aop, k[1].a0, k[1].a1, k[1].a2, k[1].tex, specular);
    }
    if (code) code->Release();
    if (errors) errors->Release();
    g_ffp_cache[key] = ps; // a failure is cached too: the host pipeline draws those
    g_ffp_last_key = key;
    return g_ffp_last_ps = ps;
}

IDirect3DPixelShader9 *ffp_reselect() { return ffp_pixel_shader(g_last_diffuse, g_last_specular, g_last_pretransformed); }

namespace {
float g_quad_sent[2][4][4]; // c1-c4, c5-c8 as last sent
bool g_quad_valid;          // false after another writer (the combiners' constants) or a new device
}

void upload_ffp_quad_constants(IDirect3DDevice9 *d, const float clamp[4][4], const float size[4][4])
{
    if (g_quad_valid && !memcmp(g_quad_sent[0], clamp, sizeof g_quad_sent[0]) && !memcmp(g_quad_sent[1], size, sizeof g_quad_sent[1]))
        return;
    memcpy(g_quad_sent[0], clamp, sizeof g_quad_sent[0]);
    memcpy(g_quad_sent[1], size, sizeof g_quad_sent[1]);
    g_quad_valid = true;
    d->SetPixelShaderConstantF(1, &clamp[0][0], 4);
    d->SetPixelShaderConstantF(5, &size[0][0], 4);
}

void upload_ffp_constants(IDirect3DDevice9 *d)
{
    uint32_t c = render_state()[XRS_TEXTUREFACTOR];
    float f[4] = {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, (c >> 24) / 255.0f};
    if (memcmp(f, g_ffp_c0, sizeof f) == 0) return;
    memcpy(g_ffp_c0, f, sizeof f);
    d->SetPixelShaderConstantF(0, f, 1);
}

// c0 rewritten by another (the combiners' constants, pshader.cpp) or a new device: the next upload sends it
void ffp_forget_constants()
{
    g_ffp_c0[0] = -1;
    g_quad_valid = false;
}

void ffp_drop_all()
{
    for (auto &kv : g_ffp_cache)
        if (kv.second) kv.second->Release();
    g_ffp_cache.clear();
    g_ffp_last_key.clear();
    g_ffp_last_ps = nullptr;
    ffp_forget_constants();
}

} // namespace xd3d
