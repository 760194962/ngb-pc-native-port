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

namespace {

std::unordered_map<std::string, IDirect3DPixelShader9 *> g_ffp_cache;

struct StageKey {
    uint32_t cop, c0, c1, c2, aop, a0, a1, a2, result;
    uint32_t tex;  // 0 none, 1 2D, 2 cube
    uint32_t proj; // D3DTTFF_PROJECTED: 3 = divide by z, 4 = divide by w, 0 = none
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

bool stage_reads_texture_key(const StageKey &e)
{
    uint32_t args[6] = {e.c0, e.c1, e.c2, e.a0, e.a1, e.a2};
    for (uint32_t a : args)
        if ((a & 0xF) == 2) return true; // D3DTA_TEXTURE
    D3DTEXTUREOP c = conv_texop(e.cop), al = conv_texop(e.aop);
    return c == D3DTOP_BLENDTEXTUREALPHA || c == D3DTOP_BLENDTEXTUREALPHAPM || al == D3DTOP_BLENDTEXTUREALPHA || al == D3DTOP_BLENDTEXTUREALPHAPM;
}

std::string build_hlsl(const StageKey (&k)[4], bool specular, bool def_diffuse, bool def_specular, int first)
{
    std::string s =
        "float4 tfactor : register(c0);\n"
        "sampler2D s2d0 : register(s0); sampler2D s2d1 : register(s1); sampler2D s2d2 : register(s2); sampler2D s2d3 : register(s3);\n"
        "samplerCUBE scu0 : register(s0); samplerCUBE scu1 : register(s1); samplerCUBE scu2 : register(s2); samplerCUBE scu3 : register(s3);\n"
        "struct PSIn { float4 v0 : COLOR0; float4 v1 : COLOR1; float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1;\n"
        "              float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; };\n"
        "float4 main(PSIn I) : COLOR0 {\n"
        "  float4 tc[4] = { I.tc0, I.tc1, I.tc2, I.tc3 };\n";
    s += def_diffuse ? "  float4 v0 = 1;\n" : "  float4 v0 = I.v0;\n";  // no diffuse in the vertex: opaque white
    s += def_specular ? "  float4 v1 = 0;\n" : "  float4 v1 = I.v1;\n"; // no specular in the vertex: 0
    s += "  float4 cur = v0, temp = 0, prev;\n";
    // A stage that reads its texture with none bound (cfg:untex): 3 = the stage is skipped, the current colour passes on
    // (below), 1 = the texture reads as the diffuse colour, 0 = opaque white. 3 matches xemu in four cases (docs/04
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
        else if (k[i].tex == 1) snprintf(b, sizeof b, "  t%d = tex2D(s2d%d, tc[%d].xy);\n", i, i, i);
        else continue;
        s += b;
    }
    for (int i = first; i < 4; i++) {
        D3DTEXTUREOP cop = conv_texop(k[i].cop), aop = conv_texop(k[i].aop);
        if (cop == D3DTOP_DISABLE) break; // the first disabled colour op ends the stages
        if (g_cfg.untex == 3 && !k[i].tex && stage_reads_texture_key(k[i])) continue; // unbound texture: skip the stage
        s += "  prev = cur;\n";
        std::string c = op_expr(cop, arg(k[i].c0, i), arg(k[i].c1, i), arg(k[i].c2, i), i, "prev");
        std::string a = aop == D3DTOP_DISABLE ? std::string("prev") : op_expr(aop, arg(k[i].a0, i), arg(k[i].a1, i), arg(k[i].a2, i), i, "prev");
        const char *dst = (k[i].result & 0xF) == 5 ? "temp" : "cur";
        s += std::string("  ") + dst + " = saturate(float4((" + c + ").rgb, (" + a + ").a));\n";
    }
    if (g_cfg.ffp_debug == 1 && k[1].tex) s += "  return float4(t1.rgb, 1);\n}\n";        // cfg:ffpdebug:1
    else if (g_cfg.ffp_debug == 2 && k[1].tex) s += "  return float4(t1.aaa, 1);\n}\n";   // cfg:ffpdebug:2
    else s += specular ? "  return float4(saturate(cur.rgb + v1.rgb), cur.a);\n}\n" : "  return cur;\n}\n";
    return s;
}

} // namespace

IDirect3DPixelShader9 *ffp_pixel_shader(bool has_diffuse, bool has_specular, bool pretransformed)
{
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
    }
    bool specular = render_state()[XRS_SPECULARENABLE] != 0;
    // Point sprites: the Xbox D3D library builds the combiner stages from stage 3 on (FUN_003d4340 of Ninja Gaiden
    // Black x2.1 starts its stage loop at 3 when D3DRS_POINTSPRITEENABLE is set), the stage the NV2A gives the sprite
    // coordinates; stages 0-2 take no part. Ninja Gaiden Black's particles keep a fire texture in stage 0 and the
    // round dot (16x16) in stage 3: colour = diffuse, alpha = dot * diffuse.
    int first = render_state()[XRS_POINTSPRITEENABLE] ? 3 : 0;
    std::string key((const char *)k, sizeof k);
    key.push_back((char)first);
    key.push_back((char)((specular ? 1 : 0) | (def_diffuse ? 2 : 0) | (def_specular ? 4 : 0)));
    key.push_back((char)g_cfg.ffp_debug);
    key.push_back((char)g_cfg.untex);
    static std::string last_key;
    static IDirect3DPixelShader9 *last_ps;
    if (key == last_key) return last_ps;
    auto it = g_ffp_cache.find(key);
    if (it != g_ffp_cache.end()) {
        last_key = key;
        return last_ps = it->second;
    }
    std::string hlsl = build_hlsl(k, specular, def_diffuse, def_specular, first);
    perf::Scope perf_scope(perf::kCompile);
    perf::add_compile();
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), "ffp_ps", nullptr, nullptr, "main", "ps_2_0", D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &code, &errors);
    if (FAILED(hr)) {
        errors ? void() : void();
        hr = D3DCompile(hlsl.data(), hlsl.size(), "ffp_ps", nullptr, nullptr, "main", "ps_2_a", D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &code, &errors);
    }
    IDirect3DPixelShader9 *ps = nullptr;
    static int logged;
    if (FAILED(hr)) {
        logf("[ffp] stage shader compile failed: %s", errors ? (const char *)errors->GetBufferPointer() : "?");
        if (logged++ < 3) logf("[ffp] HLSL:\n%s", hlsl.c_str());
    } else if (FAILED(host3d::dev()->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &ps))) {
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
    last_key = key;
    return last_ps = ps;
}

void upload_ffp_constants(IDirect3DDevice9 *d)
{
    uint32_t c = render_state()[XRS_TEXTUREFACTOR];
    float f[4] = {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, (c >> 24) / 255.0f};
    static float last[4] = {-1, -1, -1, -1};
    if (memcmp(f, last, sizeof f) == 0) return;
    memcpy(last, f, sizeof f);
    d->SetPixelShaderConstantF(0, f, 1);
}

} // namespace xd3d
