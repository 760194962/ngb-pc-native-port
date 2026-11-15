// Direct3D 9's fixed-function vertex pipeline as a generated vertex shader (see ffp_vs.h).
//
// What is done, as Direct3D 9 defines it (the lighting, fog and texture coordinate pages of its documentation):
//  - position: world x view x projection; with D3DRS_VERTEXBLEND, the weighted sum of the position through
//    WORLD0..n x VIEW (the last weight 1 - the others), then the projection;
//  - normals through the inverse transpose of the same matrices, normalized with D3DRS_NORMALIZENORMALS;
//  - lighting in camera space: ambient Ma (Ga + sum of the lights' ambient), diffuse Md sum(N.L), specular
//    Ms sum((N.H)^P) where N.L > 0, emissive Me; point and spot lights attenuated 1 / (a0 + a1 d + a2 d^2) within
//    their range, spot lights by their cone; material sources (D3DMCS) from the vertex colours with
//    D3DRS_COLORVERTEX when the vertex has them; colours clamped to 0..1, diffuse alpha from the diffuse source;
//  - texture coordinates: a vertex set or a camera-space normal / position / reflection vector, through the stage's
//    texture matrix when D3DTSS_TEXTURETRANSFORMFLAGS gives a count (a 1-, 2-element set taken as (u, 1, 0, 0),
//    (u, v, 1, 0), a 3-element one (u, v, w, 1)), the count's elements kept (the stage shader divides projected ones);
//  - point size: the vertex's or D3DRS_POINTSIZE, scaled with the distance (D3DRS_POINTSCALE_A/B/C) when
//    D3DRS_POINTSCALEENABLE, clamped to D3DRS_POINTSIZE_MIN..MAX;
//  - TEXCOORD5 = clip z, w: the stage shader's per-pixel fog (as the host's table fog), and the depth a game pixel
//    shader's polygon offset reads (as after a vertex program, nv2a_vsh.cpp); TEXCOORD4 = 1, the fog factor a game
//    pixel shader is given after fixed-function vertices.
// Not done (the host pipeline draws those): sphere-map coordinates, more than 8 lights, indexed blending, fewer
// blend weights in the vertex than the blend state takes.
#include "d3d/ffp_vs.h"

#include "d3d/host.h"
#include "d3d/shader_cache.h"
#include "d3d/state.h"
#include "common.h"
#include "perf.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>

namespace xd3d {

namespace {

// Constant registers
// The blend matrices: per matrix WORLDn x VIEW (4), its normal matrix (3), WORLDn x VIEW x PROJECTION (4).
// Ordered by how often they change, so a draw's changes are one run from c0 (upload_ffp_vs_constants sends each run
// with one SetVertexShaderConstantF, ~800 TSC ticks a call on the Iris Xe, measured): the unblended transforms
// (c0..c14, a new world matrix nearly every draw), the material and ambient colour (per object), the lights, the
// texture matrices, the point parameters, the blend matrices last. With the projection at c55 behind the blend
// matrices every world change sent 59 registers (86 per upload on average in the attract run).
constexpr int kWvp = 0, kWv = 4, kNrm = 8, kProj = 11, kMat = 15 /* diffuse, ambient, specular, emissive, power */,
              kGlobalAmbient = 20, kLights = 21 /* 8 x 7 */, kTexm = 77 /* 4 x 4 */,
              kPoint = 93 /* size, min, max; A, B, C, viewport height */, kBlend = 95, kBlendRegs = 11, kRegs = 139;
static_assert(kBlend + 4 * kBlendRegs == kRegs, "the blend matrices are the last registers");
constexpr int kLightRegs = 7;

// What the shader depends on (the constants carry the rest). memcmp-able.
struct Key {
    uint32_t fvf;
    uint16_t types;   // 2 bits per enabled light in order: 1 point, 2 spot, 3 directional
    uint8_t lights;   // how many
    uint8_t flags;    // 1 lighting, 2 specular, 4 local viewer, 8 normalize, 16 points, 32 point scale, 64 eye +z
    uint8_t msrc;     // resolved sources, 2 bits each: diffuse, specular, ambient, emissive (0 material, 1 diffuse, 2 specular)
    uint8_t blend;    // weights 0..3
    uint8_t tci[4];   // set | gen << 4
    uint8_t ttf[4];   // count | projected << 4
    uint8_t blend_mode; // the blended clip position (cfg:ffpvsblend, tests): 0 the camera-space sum x P, 1 the sum of the
                        // position through each (WORLDn x VIEW) x P, 2 through each WORLDn x (VIEW x P), 3 the world-space
                        // sum x (VIEW x P)
    uint8_t pad;
};
static_assert(sizeof(Key) == 20, "Key is compared as bytes");

// Keyed by the Key's bytes directly: draws alternate between keys, and a std::string of 20 bytes was a heap
// allocation at each such draw (ffp_vertex_shader ~640 TSC ticks a call, measured)
struct KeyHash {
    size_t operator()(const Key &k) const
    {
        uint32_t w[sizeof(Key) / 4], h = 0x811C9DC5u;
        memcpy(w, &k, sizeof w);
        for (uint32_t x : w) h = (h ^ x) * 0x01000193u;
        return h ^ (h >> 15);
    }
};
struct KeyEq {
    bool operator()(const Key &a, const Key &b) const { return !memcmp(&a, &b, sizeof(Key)); }
};
static_assert(sizeof(Key) % 4 == 0, "hashed as 32-bit words");
std::unordered_map<Key, IDirect3DVertexShader9 *, KeyHash, KeyEq> g_cache;
Key g_last_k; // the previous draw's key and shader
bool g_last_valid;
IDirect3DVertexShader9 *g_last_vs;
const char *g_why = "";
std::map<const char *, uint32_t> g_why_count; // reason (a string literal: counted by its address, no string per draw) -> draws
uint32_t g_made, g_failed, g_draws_gen, g_draws_host;

int tex_count(uint32_t fvf) { return (int)((fvf >> 8) & 0xF); }
int tex_size(uint32_t fvf, int set)
{
    static const int n[4] = {2, 3, 4, 1};
    return n[(fvf >> (16 + 2 * set)) & 3];
}
int betas(uint32_t fvf)
{
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZB1: return 1;
    case D3DFVF_XYZB2: return 2;
    case D3DFVF_XYZB3: return 3;
    case D3DFVF_XYZB4: return 4;
    default: return 0;
    }
}

std::string F(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return b;
}

// v * M for a matrix whose columns are in registers r..r+3 (or r..r+2 for 3 columns)
std::string xf4(const char *v, int r) { return F("float4(dot(%s, c[%d]), dot(%s, c[%d]), dot(%s, c[%d]), dot(%s, c[%d]))", v, r, v, r + 1, v, r + 2, v, r + 3); }
std::string xf3(const char *v, int r) { return F("float3(dot(%s, c[%d].xyz), dot(%s, c[%d].xyz), dot(%s, c[%d].xyz))", v, r, v, r + 1, v, r + 2); }

std::string build_hlsl(const Key &k)
{
    const uint32_t fvf = k.fvf;
    const int nb = betas(fvf), ntex = tex_count(fvf);
    const bool has_n = fvf & D3DFVF_NORMAL, has_ps = fvf & D3DFVF_PSIZE, has_d = fvf & D3DFVF_DIFFUSE, has_s = fvf & D3DFVF_SPECULAR;
    const bool lit = k.flags & 1, points = k.flags & 16;
    std::string s = "// ffp_vs: Direct3D 9's fixed-function vertex pipeline (d3d/ffp_vs.cpp)\n";
    s += F("float4 c[%d] : register(c0);\n", kRegs);
    s += "struct VSIn {\n  float4 p : POSITION;\n";
    if (nb) s += "  float4 bw : BLENDWEIGHT;\n";
    if (has_n) s += "  float4 n : NORMAL;\n";
    if (has_ps) s += "  float4 ps : PSIZE;\n";
    if (has_d) s += "  float4 c0 : COLOR0;\n";
    if (has_s) s += "  float4 c1 : COLOR1;\n";
    for (int i = 0; i < ntex; i++) s += F("  float4 t%d : TEXCOORD%d;\n", i, i);
    s += "};\n"
         "struct VSOut {\n"
         "  float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
         "  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
         "  float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5;\n";
    if (points) s += "  float pts : PSIZE;\n";
    s += "};\n"
         "VSOut main(VSIn I) {\n"
         "  VSOut o;\n"
         "  float4 ip = float4(I.p.xyz, 1);\n";
    s += has_n ? "  float4 in_n = float4(I.n.xyz, 0);\n" : "  float4 in_n = 0;\n";
    if (!k.blend) {
        s += "  float4 cp = " + xf4("ip", kWvp) + ";\n";
        s += "  float3 vp = " + xf4("ip", kWv) + ".xyz;\n";
        s += "  float3 vn = " + xf3("in_n", kNrm) + ";\n";
    } else {
        // Weights 0..n-1 from the vertex, the last matrix's 1 - their sum. The clip position per blend_mode (Key).
        // Registers b + 7..10 per matrix: (WORLDn x VIEW) x P (mode 1), WORLDn x (VIEW x P) (2), WORLDn (3); kProj:
        // P (0), VIEW x P (3).
        int mode = k.blend_mode;
        s += "  float3 vp = 0, vn = 0;\n  float lw = 1;\n";
        s += "  float4 cp = 0, wp = 0;\n";
        for (int m = 0; m <= k.blend; m++) {
            int b = kBlend + m * kBlendRegs;
            std::string w = m < k.blend ? F("I.bw[%d]", m) : std::string("lw");
            if (m < k.blend) s += F("  lw -= I.bw[%d];\n", m);
            s += "  vp += " + xf4("ip", b) + ".xyz * " + w + ";\n";
            s += "  vn += " + xf3("in_n", b + 4) + " * " + w + ";\n";
            if (mode == 1 || mode == 2) s += "  cp += " + xf4("ip", b + 7) + " * " + w + ";\n";
            if (mode == 3) s += "  wp += " + xf4("ip", b + 7) + " * " + w + ";\n";
        }
        if (mode == 0) {
            s += "  float4 vp4 = float4(vp, 1);\n";
            s += "  cp = " + xf4("vp4", kProj) + ";\n";
        } else if (mode == 3) {
            s += "  cp = " + xf4("wp", kProj) + ";\n";
        }
    }
    if (k.flags & 8) s += "  vn = dot(vn, vn) > 0 ? normalize(vn) : vn;\n";
    if (lit) {
        auto src = [&](int field, int mat) {
            int m = (k.msrc >> (field * 2)) & 3;
            return m == 1 ? std::string("I.c0") : m == 2 ? std::string("I.c1") : F("c[%d]", kMat + mat);
        };
        // material order in the registers: diffuse, ambient, specular, emissive
        s += "  float4 md = " + src(0, 0) + ", ms = " + src(1, 2) + ", ma = " + src(2, 1) + ", me = " + src(3, 3) + ";\n";
        s += "  float3 la = 0, ld = 0, ls = 0;\n";
        if (k.flags & 2) {
            if (k.flags & 4) s += "  float3 eye = -normalize(vp);\n";
            else s += (k.flags & 64) ? "  float3 eye = float3(0, 0, 1);\n" : "  float3 eye = float3(0, 0, -1);\n";
        }
        for (int n = 0; n < k.lights; n++) {
            int type = (k.types >> (n * 2)) & 3, b = kLights + n * kLightRegs;
            s += "  {\n    float3 l;\n    float att = 1;\n";
            if (type == 3) { // directional: towards the light, the negated direction (c[b+4], made so on the CPU)
                s += F("    l = c[%d].xyz;\n", b + 4);
            } else {
                s += F("    float3 dl = c[%d].xyz - vp;\n    float dist = length(dl);\n    l = dl / max(dist, 1e-30);\n", b + 3);
                s += F("    att = dist > c[%d].w ? 0 : 1 / (c[%d].x + c[%d].y * dist + c[%d].z * dist * dist);\n", b + 3, b + 5, b + 5, b + 5);
                if (type == 2) { // the cone: rho = (-direction) . l against cos(theta/2) (inner), cos(phi/2) (outer)
                    s += F("    float rho = dot(c[%d].xyz, l);\n", b + 4);
                    s += F("    att *= rho > c[%d].x ? 1 : rho <= c[%d].y ? 0 : pow(saturate((rho - c[%d].y) / (c[%d].x - c[%d].y)), c[%d].w);\n",
                           b + 6, b + 6, b + 6, b + 6, b + 6, b + 4);
                }
            }
            s += F("    la += att * c[%d].rgb;\n", b + 2);
            s += "    float nl = dot(vn, l);\n";
            s += "    if (nl > 0) {\n";
            s += F("      ld += att * nl * c[%d].rgb;\n", b);
            if (k.flags & 2) {
                s += "      float nh = dot(vn, normalize(l + eye));\n";
                s += F("      if (nh > 0) ls += att * pow(nh, c[%d].x) * c[%d].rgb;\n", kMat + 4, b + 1);
            }
            s += "    }\n  }\n";
        }
        s += F("  o.d0 = saturate(float4(me.rgb + ma.rgb * (c[%d].rgb + la) + md.rgb * ld, md.a));\n", kGlobalAmbient);
        s += (k.flags & 2) ? "  o.d1 = saturate(float4(ms.rgb * ls, ms.a));\n" : "  o.d1 = 0;\n";
    } else {
        s += has_d ? "  o.d0 = I.c0;\n" : "  o.d0 = 1;\n";
        s += has_s ? "  o.d1 = I.c1;\n" : "  o.d1 = 0;\n";
    }
    for (int t = 0; t < 4; t++) {
        int set = k.tci[t] & 15, gen = k.tci[t] >> 4, count = k.ttf[t] & 15;
        std::string in = set < ntex ? F("I.t%d", set) : std::string("float4(0, 0, 0, 1)");
        int size = set < ntex ? tex_size(fvf, set) : 2;
        std::string g = gen == 1 ? "float4(vn, 1)"
                        : gen == 2 ? "float4(vp, 1)"
                        : gen == 3 ? "float4(reflect(normalize(vp), vn), 1)" // R = 2 (E.N) N - E with E = -normalize(vp)
                                   : "";
        if (!count) {
            s += F("  o.t%d = %s;\n", t, gen ? g.c_str() : in.c_str());
            continue;
        }
        std::string e = gen ? g
                        : size == 1 ? "float4(" + in + ".x, 1, 0, 0)"
                        : size == 2 ? "float4(" + in + ".xy, 1, 0)"
                        : size == 3 ? "float4(" + in + ".xyz, 1)"
                                    : in;
        s += "  {\n    float4 e = " + e + ";\n";
        s += "    float4 tc = " + xf4("e", kTexm + t * 4) + ";\n";
        s += F("    o.t%d = %s;\n  }\n", t,
               count == 1 ? "float4(tc.x, 0, 0, 1)" : count == 2 ? "float4(tc.xy, 0, 1)" : count == 3 ? "float4(tc.xyz, 1)" : "tc");
    }
    s += "  o.pos = cp;\n"
         "  o.t4 = float4(1, 0, 0, 1);\n"
         "  o.t5 = float4(cp.z, cp.w, 0, 1);\n";
    if (points) {
        s += has_ps ? "  float si = I.ps.x;\n" : F("  float si = c[%d].x;\n", kPoint);
        if (k.flags & 32)
            s += F("  { float de = length(vp); si = c[%d].w * si * sqrt(1 / (c[%d].x + c[%d].y * de + c[%d].z * de * de)); }\n", kPoint + 1,
                   kPoint + 1, kPoint + 1, kPoint + 1);
        s += F("  o.pts = clamp(si, c[%d].y, c[%d].z);\n", kPoint, kPoint);
    }
    s += "  return o;\n}\n";
    return s;
}

bool make_key(const FfpVsState &st, Key &k)
{
    memset(&k, 0, sizeof k);
    uint32_t pos = st.fvf & D3DFVF_POSITION_MASK;
    if (pos != D3DFVF_XYZ && !betas(st.fvf)) return g_why = "position type", false;
    if (st.fvf & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR)) return g_why = "indexed blending", false;
    k.fvf = st.fvf;
    if (st.blend > 3) return g_why = "blend state", false;
    if (st.blend && betas(st.fvf) < (int)st.blend) return g_why = "fewer blend weights than the blend state", false;
    k.blend = (uint8_t)st.blend;
    if (k.blend) k.blend_mode = (uint8_t)std::min(std::max(g_cfg.ffp_vs_blend, 0), 3); // cfg:ffpvsblend:N (tests)
    if (st.lighting) {
        k.flags |= 1;
        if (st.specular) k.flags |= 2;
        if (st.localviewer) k.flags |= 4;
        if (g_cfg.ffp_vs_eye > 0) k.flags |= 64; // cfg:ffpvseye:1 (tests): a viewer at infinity towards +z
        if (st.colorvertex) {
            for (int f = 0; f < 4; f++) {
                uint32_t m = st.msrc[f];
                if (m == D3DMCS_COLOR1 && (st.fvf & D3DFVF_DIFFUSE)) k.msrc |= 1 << (f * 2);
                else if (m == D3DMCS_COLOR2 && (st.fvf & D3DFVF_SPECULAR)) k.msrc |= 2 << (f * 2);
            }
        }
        // The enabled lights' part, worked out again only when a light, its enabling or the material changed
        // (g_ffp_light_gen): walking the device's light maps at every lit draw was part of the per-draw cost
        static uint32_t s_gen = ~0u;
        static uint16_t s_types;
        static uint8_t s_n;
        static const char *s_fail;
        if (s_gen != g_ffp_light_gen) {
            s_gen = g_ffp_light_gen;
            s_types = 0, s_n = 0, s_fail = nullptr;
            const D3DLIGHT9 *l[8];
            int n = ffp_enabled_lights(l, 8);
            if (n > 8) s_fail = "more than 8 lights";
            for (int i = 0; i < n && !s_fail; i++) {
                if (l[i]->Type < D3DLIGHT_POINT || l[i]->Type > D3DLIGHT_DIRECTIONAL) s_fail = "light type";
                else s_types |= (uint16_t)(l[i]->Type << (i * 2));
            }
            s_n = (uint8_t)n;
        }
        if (s_fail) return g_why = s_fail, false;
        k.types = s_types;
        k.lights = s_n;
    }
    if (st.normalize) k.flags |= 8;
    if (st.points) {
        k.flags |= 16;
        if (st.point_scale) k.flags |= 32;
    }
    for (int t = 0; t < 4; t++) {
        uint32_t tci = st.tci[t], gen = tci >> 16, ttf = st.ttf[t];
        if (gen > 3 || (tci & 0xFFF8u & 0xFFFF)) return g_why = gen == 4 ? "sphere-map coordinates" : "texture coordinate index", false;
        if ((ttf & 0xFF) > 4 || (ttf & ~0x1FFu)) return g_why = "texture transform flags", false;
        k.tci[t] = (uint8_t)((tci & 7) | gen << 4);
        k.ttf[t] = (uint8_t)((ttf & 0xFF) | ((ttf & D3DTTFF_PROJECTED) ? 16 : 0));
    }
    return true;
}

// ---- constants ----

using M4 = D3DMATRIX;

M4 mul(const M4 &a, const M4 &b)
{
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

// Registers r..r+3 = the columns of m (a row vector times m is then four dot products)
void put_columns(float (*c)[4], int r, const M4 &m)
{
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) c[r + j][i] = m.m[i][j];
}

// Registers r..r+2 = the rows of the inverse of m's upper 3x3: the normal n goes to n (M^-1)^T, whose element j is
// n . (row j of M^-1)
void put_normal(float (*c)[4], int r, const M4 &m)
{
    double a = m._11, b = m._12, cc = m._13, d = m._21, e = m._22, f = m._23, g = m._31, h = m._32, i = m._33;
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + cc * C;
    double inv[3][3];
    if (std::fabs(det) < 1e-30) {
        memset(inv, 0, sizeof inv);
    } else {
        double k = 1.0 / det;
        inv[0][0] = A * k, inv[0][1] = -(b * i - cc * h) * k, inv[0][2] = (b * f - cc * e) * k;
        inv[1][0] = B * k, inv[1][1] = (a * i - cc * g) * k, inv[1][2] = -(a * f - cc * d) * k;
        inv[2][0] = C * k, inv[2][1] = -(a * h - b * g) * k, inv[2][2] = (a * e - b * d) * k;
    }
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) c[r + row][col] = (float)inv[row][col];
        c[r + row][3] = 0;
    }
}

void color(float *o, const D3DCOLORVALUE &v) { o[0] = v.r, o[1] = v.g, o[2] = v.b, o[3] = v.a; }

alignas(16) float g_c[kRegs][4];
bool g_valid;
// What the uploaded groups came from
M4 g_src_xf[6]; // view, projection, world 0-3
uint8_t g_src_blend = 0xFF;
int g_src_mode = -1;
uint32_t g_src_light_gen, g_src_ambient;
M4 g_src_view_for_lights;
M4 g_src_texm[4];
float g_src_point[8];

} // namespace

const char *ffp_vs_why() { return g_why; }

IDirect3DVertexShader9 *ffp_vertex_shader(const FfpVsState &st)
{
    g_why = "";
    Key k;
    if (!make_key(st, k)) return nullptr;
    // The previous draw's shader compared as bytes first: a std::string of the 20-byte key is past the short-string
    // buffer, a heap allocation at every draw
    if (g_last_valid && !memcmp(&k, &g_last_k, sizeof k)) return g_last_vs;
    auto it = g_cache.find(k);
    if (it == g_cache.end()) {
        std::string hlsl = build_hlsl(k);
        perf::Scope perf_scope(perf::kCompile);
        ID3DBlob *code = nullptr, *errors = nullptr;
        HRESULT hr = cached_compile(hlsl, "ffp_vs", "vs_2_0", D3DCOMPILE_OPTIMIZATION_LEVEL2, &code, &errors);
        if (FAILED(hr)) {
            if (errors) errors->Release();
            errors = nullptr;
            hr = cached_compile(hlsl, "ffp_vs", "vs_2_a", D3DCOMPILE_OPTIMIZATION_LEVEL2, &code, &errors);
        }
        IDirect3DVertexShader9 *vs = nullptr;
        if (FAILED(hr)) {
            static int logged;
            if (logged++ < 3)
                logf("[ffpvs] compile failed: %s\nHLSL:\n%s", errors ? (const char *)errors->GetBufferPointer() : "?", hlsl.c_str());
            g_failed++;
        } else if (FAILED(create_vertex_shader(host3d::dev(), code, &vs))) {
            logf("[ffpvs] CreateVertexShader failed");
            vs = nullptr;
            g_failed++;
        } else {
            g_made++;
            if (g_made <= 40 || g_cfg.debug_frame)
                logf("[ffpvs] shader #%u: fvf 0x%x lighting %d (%u lights, types 0x%x, msrc 0x%x, flags 0x%x) blend %u tci %x %x %x %x ttf %x %x %x %x",
                     g_made, k.fvf, k.flags & 1, k.lights, k.types, k.msrc, k.flags, k.blend, k.tci[0], k.tci[1], k.tci[2], k.tci[3],
                     k.ttf[0], k.ttf[1], k.ttf[2], k.ttf[3]);
        }
        if (code) code->Release();
        if (errors) errors->Release();
        it = g_cache.emplace(k, vs).first; // a failure is kept too: the host pipeline draws those
    }
    if (!it->second) g_why = "shader failed";
    g_last_k = k;
    g_last_valid = true;
    return g_last_vs = it->second;
}

void upload_ffp_vs_constants(IDirect3DDevice9 *d, const FfpVsState &st)
{
    perf::DetScope ds(perf::kDetConstants);
    const M4 *T = g_st.transforms; // 0 view, 1 projection, 6-9 world 0-3
    const M4 &V = T[0], &P = T[1];
    // The registers changed, as runs sent one call each: the groups below are touched in register order (the blend
    // matrices, computed with the transforms, last), so a group that starts at or next to the previous run's end
    // extends it (no registers between them sent for nothing)
    struct Run {
        int lo, hi;
    } runs[8];
    int nruns = 0;
    auto touched = [&](int a, int b) {
        if (nruns && a <= runs[nruns - 1].hi + 1) runs[nruns - 1].hi = std::max(runs[nruns - 1].hi, b);
        else runs[nruns++] = {a, b};
    };
    // Transforms
    bool blend_changed = false;
    uint8_t blend = (uint8_t)st.blend;
    const int mode = blend ? std::min(std::max(g_cfg.ffp_vs_blend, 0), 3) : 0;
    if (!g_valid || blend != g_src_blend || mode != g_src_mode || memcmp(&g_src_xf[0], &V, sizeof(M4)) || memcmp(&g_src_xf[1], &P, sizeof(M4)) ||
        memcmp(&g_src_xf[2], &T[6], sizeof(M4) * (blend ? blend + 1 : 1))) {
        g_src_xf[0] = V;
        g_src_xf[1] = P;
        for (int i = 0; i < 4; i++) g_src_xf[2 + i] = T[6 + i];
        g_src_blend = blend;
        g_src_mode = mode;
        M4 wv = mul(T[6], V);
        put_columns(g_c, kWvp, mul(wv, P));
        put_columns(g_c, kWv, wv);
        put_normal(g_c, kNrm, wv);
        M4 vpm = mode >= 2 ? mul(V, P) : P; // VIEW x P only for the blend modes that read it
        put_columns(g_c, kProj, mode == 3 ? vpm : P);
        touched(kWvp, kProj + 3); // c0..c14
        for (int m = 0; m <= blend && blend; m++) {
            M4 bwv = mul(T[6 + m], V);
            put_columns(g_c, kBlend + m * kBlendRegs, bwv);
            put_normal(g_c, kBlend + m * kBlendRegs + 4, bwv);
            put_columns(g_c, kBlend + m * kBlendRegs + 7, mode == 2 ? mul(T[6 + m], vpm) : mode == 3 ? T[6 + m] : mul(bwv, P));
        }
        blend_changed = blend != 0; // sent last (the blend matrices are the last registers)
    }
    // The material and the lights (camera space: they follow the view), each sent only when its registers come out
    // different: the game sets a material per object, which counts as a light change (g_ffp_light_gen)
    if (st.lighting && (!g_valid || g_src_light_gen != g_ffp_light_gen || g_src_ambient != st.ambient ||
                        memcmp(&g_src_view_for_lights, &V, sizeof(M4)))) {
        g_src_light_gen = g_ffp_light_gen;
        g_src_ambient = st.ambient;
        g_src_view_for_lights = V;
        float mat[6][4];
        const D3DMATERIAL9 &m = ffp_material();
        color(mat[0], m.Diffuse);
        color(mat[1], m.Ambient);
        color(mat[2], m.Specular);
        color(mat[3], m.Emissive);
        mat[4][0] = m.Power, mat[4][1] = mat[4][2] = mat[4][3] = 0;
        uint32_t a = st.ambient;
        mat[5][0] = ((a >> 16) & 255) / 255.0f, mat[5][1] = ((a >> 8) & 255) / 255.0f;
        mat[5][2] = (a & 255) / 255.0f, mat[5][3] = (a >> 24) / 255.0f;
        static_assert(kGlobalAmbient == kMat + 5, "the material and the ambient colour are one run");
        if (!g_valid || memcmp(mat, g_c[kMat], sizeof mat)) {
            memcpy(g_c[kMat], mat, sizeof mat);
            touched(kMat, kGlobalAmbient);
        }
        float lc[8 * kLightRegs][4];
        const D3DLIGHT9 *l[8];
        int n = std::min(ffp_enabled_lights(l, 8), 8);
        for (int i = 0; i < n; i++) {
            float(*c)[4] = lc + i * kLightRegs;
            color(c[0], l[i]->Diffuse);
            color(c[1], l[i]->Specular);
            color(c[2], l[i]->Ambient);
            const D3DVECTOR &p = l[i]->Position, &dir = l[i]->Direction;
            for (int j = 0; j < 3; j++) c[3][j] = p.x * V.m[0][j] + p.y * V.m[1][j] + p.z * V.m[2][j] + V.m[3][j];
            c[3][3] = l[i]->Range;
            float dv[3];
            for (int j = 0; j < 3; j++) dv[j] = dir.x * V.m[0][j] + dir.y * V.m[1][j] + dir.z * V.m[2][j];
            float len = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
            for (int j = 0; j < 3; j++) c[4][j] = len > 0 ? -dv[j] / len : 0.0f; // towards the light
            c[4][3] = l[i]->Falloff;
            c[5][0] = l[i]->Attenuation0, c[5][1] = l[i]->Attenuation1, c[5][2] = l[i]->Attenuation2, c[5][3] = 0;
            c[6][0] = std::cos(l[i]->Theta * 0.5f), c[6][1] = std::cos(l[i]->Phi * 0.5f), c[6][2] = c[6][3] = 0;
        }
        size_t bytes = sizeof(float[4]) * kLightRegs * n;
        if (n && (!g_valid || memcmp(lc, g_c[kLights], bytes))) {
            memcpy(g_c[kLights], lc, bytes);
            touched(kLights, kLights + n * kLightRegs - 1);
        }
    }
    // Texture matrices (only the stages with a count read theirs)
    for (int t = 0; t < 4; t++) {
        if (!(st.ttf[t] & 0xFF) || !st.texm[t]) continue;
        if (g_valid && !memcmp(&g_src_texm[t], st.texm[t], sizeof(M4))) continue;
        g_src_texm[t] = *st.texm[t];
        put_columns(g_c, kTexm + t * 4, *st.texm[t]);
        touched(kTexm + t * 4, kTexm + t * 4 + 3);
    }
    if (st.points) {
        float p[8] = {st.point_size, st.point_min, st.point_max, 0, st.point_a, st.point_b, st.point_c, st.viewport_height};
        if (!g_valid || memcmp(p, g_src_point, sizeof p)) {
            memcpy(g_src_point, p, sizeof p);
            memcpy(g_c[kPoint], p, sizeof p);
            touched(kPoint, kPoint + 1);
        }
    }
    if (blend_changed) touched(kBlend, kBlend + (blend + 1) * kBlendRegs - 1);
    if (!g_valid) {
        runs[0] = {0, kRegs - 1}, nruns = 1; // everything once (registers no group wrote are 0)
        g_valid = true;
    }
    for (int i = 0; i < nruns; i++)
        d->SetVertexShaderConstantF((UINT)runs[i].lo, g_c[runs[i].lo], (UINT)(runs[i].hi - runs[i].lo + 1));
}

void ffp_vs_forget_constants() { g_valid = false; }

void ffp_vs_drop_all()
{
    for (auto &kv : g_cache)
        if (kv.second) kv.second->Release();
    g_cache.clear();
    g_last_valid = false;
    g_last_vs = nullptr;
    g_valid = false;
}

void ffp_vs_not(const char *why) { g_why = why; }

void ffp_vs_count(bool generated)
{
    static uint32_t reported;
    uint32_t frame = g_frame_count.load(std::memory_order_relaxed);
    if (frame / 1800 != reported) { // every 30 s or so: the totals so far
        reported = frame / 1800;
        ffp_vs_report();
    }
    if (generated) {
        g_draws_gen++;
    } else {
        g_draws_host++;
        g_why_count[*g_why ? g_why : "off"]++;
    }
}

void ffp_vs_report()
{
    if (!g_draws_gen && !g_draws_host) return;
    std::string why;
    for (auto &kv : g_why_count) why += F("%s%s %u", why.empty() ? "" : ", ", kv.first, kv.second);
    logf("[ffpvs] fixed-function draws with 3D positions: %u through generated shaders (%u made, %u failed), %u through the host pipeline%s%s",
         g_draws_gen, g_made, g_failed, g_draws_host, why.empty() ? "" : ": ", why.c_str());
}

} // namespace xd3d
