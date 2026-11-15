// Recorded draw states drawn once at start (see drawwarm.h).
//
// A record is the host device's state as a draw left it, read back exactly when the draw's signature (draw.cpp) is
// new: the shaders (by their bytecode's key, the same the shader cache keeps its objects by), the vertex layout, the
// primitive type, the render states that go into a pipeline (depth, stencil, blend, raster, fog, the fixed-function
// vertex states and the enabled lights' shapes when no vertex shader is set), each sampler's texture type and format, the target's and the depth
// buffer's formats and sample counts. Replayed: those set on a 16x16 target of that format with 4x4 textures of those
// types and formats, one primitive from zeroed vertices drawn. A record whose shader is not in the cache or the seed
// is left out.
#include "d3d/drawwarm.h"

#include "d3d/draw.h"
#include "d3d/ffp_vs.h"
#include "d3d/host.h"
#include "d3d/shader_cache.h"
#include "d3d/state.h"
#include "common.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xd3d {

void forget_sent_state(); // draw.cpp

namespace {

const D3DRENDERSTATETYPE kRs[] = {
    D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
    D3DRS_CULLMODE, D3DRS_ZFUNC, D3DRS_ALPHAFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_FOGENABLE, D3DRS_FOGTABLEMODE, D3DRS_FOGVERTEXMODE,
    D3DRS_RANGEFOGENABLE, D3DRS_STENCILENABLE, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_STENCILFUNC,
    D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL,
    D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILFUNC, D3DRS_CLIPPING, D3DRS_CLIPPLANEENABLE, D3DRS_POINTSPRITEENABLE,
    D3DRS_POINTSCALEENABLE, D3DRS_MULTISAMPLEANTIALIAS, D3DRS_COLORWRITEENABLE, D3DRS_BLENDOP, D3DRS_SCISSORTESTENABLE,
    D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_DEPTHBIAS, D3DRS_ANTIALIASEDLINEENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA,
    // the fixed-function vertex pipeline's (0 in a record with a vertex shader)
    D3DRS_LIGHTING, D3DRS_SPECULARENABLE, D3DRS_COLORVERTEX, D3DRS_LOCALVIEWER, D3DRS_NORMALIZENORMALS,
    D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE, D3DRS_EMISSIVEMATERIALSOURCE,
    D3DRS_VERTEXBLEND, D3DRS_INDEXEDVERTEXBLENDENABLE};
constexpr int kNumRs = sizeof kRs / sizeof kRs[0];
constexpr int kFirstFixed = kNumRs - 11;

struct Tex {
    uint32_t type, format, usage, srgb; // type: 0 none, D3DRTYPE_TEXTURE / CUBETEXTURE / VOLUMETEXTURE
};

// Written to the files as it is: fixed size, every byte set (memset first)
struct Rec {
    uint64_t vs, ps; // bytecode keys (shader_code_key), 0 = none
    uint32_t fvf;    // or 0: elem
    uint32_t prim;
    uint32_t nelem;
    D3DVERTEXELEMENT9 elem[16];
    uint32_t rs[kNumRs];
    Tex tex[4];
    uint32_t rt_format, rt_ms, rt_msq, z_format; // z_format 0: no depth buffer
    uint32_t tci[4], ttf[4];                     // fixed-function vertices only
    // Fixed-function vertices, what of the host pipeline's state the driver's program depends on beyond the states
    // above (NVIDIA, 2026-11-15: with only those, entering an area of the attract run left a dozen draws each taking
    // the driver 2-6.5 ms to finish, its program built then; with these, 1-3 of 2-4 ms):
    // - with lighting on, the enabled lights' shapes (shape_of), the rest 0;
    uint32_t nlights;
    uint8_t light[8];
    // - which elements of the view, projection, world 0 and texture 0-3 matrices are 0 and which 1 (bit i: element
    //   i / 4, i % 4): a perspective projection (table fog on w) and the texture matrices' zero rows shape the program.
    //   The replay builds matrices of that pattern.
    uint16_t xf_zero[7], xf_one[7];
};
constexpr D3DTRANSFORMSTATETYPE kXf[7] = {D3DTS_VIEW, D3DTS_PROJECTION, D3DTS_WORLD, D3DTS_TEXTURE0, D3DTS_TEXTURE1,
                                          D3DTS_TEXTURE2, D3DTS_TEXTURE3};

// What of a light can shape the driver's fixed-function program, not its colours or place: bits 0-1 the type - 1
// (point, spot, directional), 2-4 attenuation 0/1/2 not zero, 5 a spot falloff other than 1
uint8_t shape_of(const D3DLIGHT9 &l)
{
    uint8_t s = (uint8_t)((l.Type - 1) & 3);
    if (l.Type != D3DLIGHT_DIRECTIONAL) {
        if (l.Attenuation0 != 0) s |= 4;
        if (l.Attenuation1 != 0) s |= 8;
        if (l.Attenuation2 != 0) s |= 16;
    }
    if (l.Type == D3DLIGHT_SPOT && l.Falloff != 1.0f) s |= 32;
    return s;
}

// The enabled lights' shapes, worked out again only when a light, its enabling or the material changed
const uint8_t *light_shapes(uint32_t *n, uint32_t *packed_hash)
{
    static uint32_t gen = ~0u, count, hash;
    static uint8_t shapes[8];
    if (gen != g_ffp_light_gen) {
        gen = g_ffp_light_gen;
        const D3DLIGHT9 *l[8];
        int k = ffp_enabled_lights(l, 8);
        count = (uint32_t)(k < 8 ? k : 8);
        memset(shapes, 0, sizeof shapes);
        hash = 0x811C9DC5u ^ (uint32_t)k;
        for (uint32_t i = 0; i < count; i++) {
            shapes[i] = shape_of(*l[i]);
            hash = (hash ^ shapes[i]) * 0x01000193u;
        }
    }
    *n = count;
    if (packed_hash) *packed_hash = hash;
    return shapes;
}

// A light of that shape for the replay (the values only make each term present or not)
D3DLIGHT9 light_of(uint8_t s)
{
    D3DLIGHT9 l = {};
    l.Type = (D3DLIGHTTYPE)((s & 3) + 1);
    l.Diffuse = l.Specular = l.Ambient = {1, 1, 1, 1};
    l.Direction = {0, 0, 1};
    l.Range = 1000;
    l.Attenuation0 = (s & 4) ? 1.0f : 0.0f;
    l.Attenuation1 = (s & 8) ? 0.1f : 0.0f;
    l.Attenuation2 = (s & 16) ? 0.01f : 0.0f;
    if (l.Type != D3DLIGHT_DIRECTIONAL && !(s & 28)) l.Attenuation0 = 1.0f; // a point or spot light needs one term
    l.Falloff = (s & 32) ? 2.0f : 1.0f;
    l.Theta = 0.5f, l.Phi = 1.0f;
    return l;
}

// File: "NGBDRAW2", u32 record size, u32 count, the program version that wrote it (16 bytes, zero-padded), records.
// Learned records of another version are dropped: a release's generated shaders and translations may differ, and
// the old records (their shaders still in the shader cache, which keeps everything) would be drawn at every start for
// nothing (NVIDIA ~1.3 ms of driver work each: 2,590 records, a third of them current, held the start 3.4 s).
constexpr char kMagic[8] = {'N', 'G', 'B', 'D', 'R', 'A', 'W', '2'};
struct Version {
    char s[16];
};
Version this_version()
{
    Version v = {};
    strncpy(v.s, BLACK_VERSION, sizeof v.s - 1);
    return v;
}

uint64_t fnv(const void *p, size_t n, uint64_t h = 0xCBF29CE484222325ull)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

std::mutex g_lock;
std::vector<Rec> g_recs;                 // seed + learned + new, in that order
std::unordered_set<uint64_t> g_have;     // their hashes
std::vector<Rec> g_learned;              // the learned file's + this run's new ones
// Signatures read already: an open-addressed table of the 64-bit signatures themselves (already hashes; 0 = empty,
// a signature of 0 is taken as 1), looked up at every draw. std::unordered_set's lookup was ~225 TSC ticks a draw
// (hashing the key again, a bucket list in memory far apart), measured on the Iris Xe 2026-11-15.
struct SigSet {
    std::vector<uint64_t> slot = std::vector<uint64_t>(1 << 12);
    size_t used = 0;
    // true when sig was not in the set (it is now)
    bool insert(uint64_t sig)
    {
        if (!sig) sig = 1;
        size_t mask = slot.size() - 1;
        for (size_t i = (size_t)(sig ^ (sig >> 32)) & mask;; i = (i + 1) & mask) {
            if (slot[i] == sig) return false;
            if (!slot[i]) {
                slot[i] = sig;
                if (++used * 2 > slot.size()) grow();
                return true;
            }
        }
    }
    void grow()
    {
        std::vector<uint64_t> old;
        old.swap(slot);
        slot.assign(old.size() * 2, 0);
        used = 0;
        for (uint64_t s : old)
            if (s) insert(s);
    }
    void clear()
    {
        slot.assign(1 << 12, 0);
        used = 0;
    }
};
SigSet g_sig_seen;   // signatures read already
uint64_t g_last_sig; // the previous draw's (most draws repeat it)
std::unordered_map<const void *, uint64_t> g_code_keys; // shader object -> bytecode key
bool g_loaded, g_dirty;
uint32_t g_new;
std::atomic<bool> g_saving;

std::wstring learned_path() { return g_cfg.port_dir + L"\\cache\\draw_states.bin"; }
std::wstring seed_path() { return g_cfg.port_dir + L"\\draw_states.bin"; }

// *version: the file's (empty when it could not be read). same_version: a file of another version is not taken.
bool read_file(const std::wstring &path, std::vector<Rec> &out, Version *version, bool same_version)
{
    *version = {};
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char magic[8];
    uint32_t size = 0, count = 0;
    bool ok = fread(magic, 8, 1, f) == 1 && !memcmp(magic, kMagic, 8) && fread(&size, 4, 1, f) == 1 && size == sizeof(Rec) &&
              fread(&count, 4, 1, f) == 1 && count < 1000000 && fread(version, sizeof *version, 1, f) == 1;
    version->s[sizeof version->s - 1] = 0;
    Version mine = this_version();
    if (ok && same_version && memcmp(version->s, mine.s, sizeof mine.s)) {
        fclose(f);
        return false; // load() says so
    }
    if (ok) {
        size_t at = out.size();
        out.resize(at + count);
        ok = fread(&out[at], sizeof(Rec), count, f) == count;
        if (!ok) out.resize(at);
    }
    fclose(f);
    return ok;
}

void load()
{
    if (g_loaded) return;
    g_loaded = true;
    std::vector<Rec> seed;
    Version sv, lv, mine = this_version();
    // the seed of another version is used too (its records whose shaders this one lacks are left out at replay)
    bool seed_ok = read_file(seed_path(), seed, &sv, false);
    bool learned_ok = read_file(learned_path(), g_learned, &lv, true);
    for (const std::vector<Rec> *v : {&seed, &g_learned})
        for (const Rec &r : *v)
            if (g_have.insert(fnv(&r, sizeof r)).second) g_recs.push_back(r);
    char note[96] = "";
    if (!learned_ok && lv.s[0] && memcmp(lv.s, mine.s, sizeof mine.s))
        snprintf(note, sizeof note, " (those of %s dropped: learned again by %s)", lv.s, mine.s);
    logf("[warm] draw states: %zu from the seed%s%s%s, %zu learned%s%s; %zu in all", seed.size(), seed_ok ? "" : " (none)",
         seed_ok && memcmp(sv.s, mine.s, sizeof mine.s) ? ", made by " : "", seed_ok && memcmp(sv.s, mine.s, sizeof mine.s) ? sv.s : "",
         g_learned.size(), learned_ok || note[0] ? "" : " (none)", note, g_recs.size());
}

void save_learned()
{
    if (g_saving.exchange(true)) return;
    std::vector<Rec> copy = g_learned;
    std::wstring path = learned_path();
    std::thread([copy = std::move(copy), path] {
        CreateDirectoryW((g_cfg.port_dir + L"\\cache").c_str(), nullptr);
        std::wstring tmp = path + L".tmp";
        bool ok = false;
        if (FILE *f = _wfopen(tmp.c_str(), L"wb")) {
            uint32_t size = sizeof(Rec), count = (uint32_t)copy.size();
            Version v = this_version();
            ok = fwrite(kMagic, 8, 1, f) == 1 && fwrite(&size, 4, 1, f) == 1 && fwrite(&count, 4, 1, f) == 1 &&
                 fwrite(&v, sizeof v, 1, f) == 1 &&
                 (count == 0 || fwrite(copy.data(), sizeof(Rec), count, f) == count);
            ok = fclose(f) == 0 && ok;
        }
        if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
        g_saving = false;
    }).detach();
}

uint64_t code_key_of(IUnknown *shader, bool vertex)
{
    if (!shader) return 0;
    auto it = g_code_keys.find(shader);
    if (it != g_code_keys.end()) return it->second;
    UINT size = 0;
    HRESULT hr = vertex ? ((IDirect3DVertexShader9 *)shader)->GetFunction(nullptr, &size) : ((IDirect3DPixelShader9 *)shader)->GetFunction(nullptr, &size);
    uint64_t key = 0;
    if (SUCCEEDED(hr) && size) {
        std::vector<uint8_t> code(size);
        hr = vertex ? ((IDirect3DVertexShader9 *)shader)->GetFunction(code.data(), &size) : ((IDirect3DPixelShader9 *)shader)->GetFunction(code.data(), &size);
        if (SUCCEEDED(hr)) key = shader_code_key(code.data(), size);
    }
    g_code_keys[shader] = key;
    return key;
}

void read_tex(IDirect3DDevice9 *d, int s, Tex &t)
{
    IDirect3DBaseTexture9 *b = nullptr;
    if (FAILED(d->GetTexture(s, &b)) || !b) return;
    t.type = b->GetType();
    if (t.type == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC ds;
        if (SUCCEEDED(((IDirect3DTexture9 *)b)->GetLevelDesc(0, &ds))) t.format = ds.Format, t.usage = ds.Usage;
    } else if (t.type == D3DRTYPE_CUBETEXTURE) {
        D3DSURFACE_DESC ds;
        if (SUCCEEDED(((IDirect3DCubeTexture9 *)b)->GetLevelDesc(0, &ds))) t.format = ds.Format, t.usage = ds.Usage;
    } else if (t.type == D3DRTYPE_VOLUMETEXTURE) {
        D3DVOLUME_DESC vd;
        if (SUCCEEDED(((IDirect3DVolumeTexture9 *)b)->GetLevelDesc(0, &vd))) t.format = vd.Format, t.usage = vd.Usage;
    }
    t.usage &= D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL;
    DWORD srgb = 0;
    d->GetSamplerState(s, D3DSAMP_SRGBTEXTURE, &srgb);
    t.srgb = srgb;
    b->Release();
}

bool capture(IDirect3DDevice9 *d, D3DPRIMITIVETYPE prim, Rec &r)
{
    memset(&r, 0, sizeof r);
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    d->GetVertexShader(&vs);
    d->GetPixelShader(&ps);
    r.vs = code_key_of(vs, true);
    r.ps = code_key_of(ps, false);
    bool fixed_vertices = !vs;
    if (vs) vs->Release();
    if (ps) ps->Release();
    if ((!fixed_vertices && !r.vs)) return false; // a shader whose code could not be read
    DWORD fvf = 0;
    d->GetFVF(&fvf);
    if (fvf) {
        r.fvf = fvf;
    } else {
        IDirect3DVertexDeclaration9 *decl = nullptr;
        if (FAILED(d->GetVertexDeclaration(&decl)) || !decl) return false;
        UINT n = 0;
        decl->GetDeclaration(nullptr, &n);
        if (n > 16 || FAILED(decl->GetDeclaration(r.elem, &n))) {
            decl->Release();
            return false;
        }
        r.nelem = n;
        decl->Release();
    }
    r.prim = prim;
    for (int i = 0; i < kNumRs; i++) {
        if (!fixed_vertices && i >= kFirstFixed) continue; // no part in a draw through a vertex shader
        DWORD v = 0;
        d->GetRenderState(kRs[i], &v);
        r.rs[i] = v;
    }
    for (int s = 0; s < 4; s++) read_tex(d, s, r.tex[s]);
    if (fixed_vertices) {
        for (int s = 0; s < 4; s++) {
            DWORD v = 0;
            d->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &v);
            r.tci[s] = v;
            d->GetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, &v);
            r.ttf[s] = v;
        }
        for (int m = 0; m < 7; m++) {
            D3DMATRIX x;
            d->GetTransform(kXf[m], &x);
            for (int i = 0; i < 16; i++) {
                float v = x.m[i / 4][i % 4];
                if (v == 0) r.xf_zero[m] |= 1 << i;
                if (v == 1) r.xf_one[m] |= 1 << i;
            }
        }
        DWORD lighting = 0;
        d->GetRenderState(D3DRS_LIGHTING, &lighting);
        if (lighting) {
            const uint8_t *s = light_shapes(&r.nlights, nullptr);
            memcpy(r.light, s, sizeof r.light);
        }
    }
    IDirect3DSurface9 *rt = nullptr, *z = nullptr;
    if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return false;
    D3DSURFACE_DESC ds;
    rt->GetDesc(&ds);
    r.rt_format = ds.Format, r.rt_ms = ds.MultiSampleType, r.rt_msq = ds.MultiSampleQuality;
    rt->Release();
    if (SUCCEEDED(d->GetDepthStencilSurface(&z)) && z) {
        z->GetDesc(&ds);
        r.z_format = ds.Format;
        z->Release();
    }
    return true;
}

// ---- replay ----

struct Objects {
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, IDirect3DSurface9 *> rts, zs; // format, samples, quality
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, IDirect3DBaseTexture9 *> texs; // type, format, usage
    std::map<std::vector<uint8_t>, IDirect3DVertexDeclaration9 *> decls;
    IDirect3DVertexBuffer9 *vb = nullptr;
    void release()
    {
        for (auto *m : {&rts, &zs})
            for (auto &kv : *m)
                if (kv.second) kv.second->Release();
        for (auto &kv : texs)
            if (kv.second) kv.second->Release();
        for (auto &kv : decls)
            if (kv.second) kv.second->Release();
        if (vb) vb->Release();
        rts.clear(), zs.clear(), texs.clear(), decls.clear();
        vb = nullptr;
    }
};

uint32_t type_size(uint32_t t)
{
    switch (t) {
    case D3DDECLTYPE_FLOAT1: return 4;
    case D3DDECLTYPE_FLOAT2: return 8;
    case D3DDECLTYPE_FLOAT3: return 12;
    case D3DDECLTYPE_FLOAT4: return 16;
    case D3DDECLTYPE_SHORT4: case D3DDECLTYPE_SHORT4N: case D3DDECLTYPE_USHORT4N: case D3DDECLTYPE_FLOAT16_4: return 8;
    default: return 4;
    }
}

uint32_t fvf_size(uint32_t fvf)
{
    uint32_t n = 0;
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ: n = 12; break;
    case D3DFVF_XYZRHW: case D3DFVF_XYZB1: n = 16; break;
    case D3DFVF_XYZB2: n = 20; break;
    case D3DFVF_XYZB3: n = 24; break;
    case D3DFVF_XYZB4: n = 28; break;
    case D3DFVF_XYZB5: n = 32; break;
    case D3DFVF_XYZW: n = 16; break;
    }
    if (fvf & D3DFVF_NORMAL) n += 12;
    if (fvf & D3DFVF_PSIZE) n += 4;
    if (fvf & D3DFVF_DIFFUSE) n += 4;
    if (fvf & D3DFVF_SPECULAR) n += 4;
    for (uint32_t i = 0; i < ((fvf >> 8) & 0xF); i++) {
        static const uint32_t sizes[4] = {8, 12, 16, 4};
        n += sizes[(fvf >> (16 + 2 * i)) & 3];
    }
    return n;
}

IDirect3DBaseTexture9 *dummy_tex(IDirect3DDevice9 *d, Objects &o, const Tex &t)
{
    auto key = std::make_tuple(t.type, t.format, t.usage);
    auto it = o.texs.find(key);
    if (it != o.texs.end()) return it->second;
    IDirect3DBaseTexture9 *b = nullptr;
    for (DWORD usage : {(DWORD)t.usage, (DWORD)0}) {
        HRESULT hr = E_FAIL;
        if (t.type == D3DRTYPE_TEXTURE) hr = d->CreateTexture(4, 4, 1, usage, (D3DFORMAT)t.format, D3DPOOL_DEFAULT, (IDirect3DTexture9 **)&b, nullptr);
        else if (t.type == D3DRTYPE_CUBETEXTURE) hr = d->CreateCubeTexture(4, 1, usage, (D3DFORMAT)t.format, D3DPOOL_DEFAULT, (IDirect3DCubeTexture9 **)&b, nullptr);
        else if (t.type == D3DRTYPE_VOLUMETEXTURE) hr = d->CreateVolumeTexture(4, 4, 4, 1, usage, (D3DFORMAT)t.format, D3DPOOL_DEFAULT, (IDirect3DVolumeTexture9 **)&b, nullptr);
        if (SUCCEEDED(hr)) break;
        b = nullptr;
        if (!usage) break;
    }
    return o.texs[key] = b;
}

IDirect3DSurface9 *surface(IDirect3DDevice9 *d, Objects &o, bool depth, uint32_t format, uint32_t ms, uint32_t q)
{
    auto &m = depth ? o.zs : o.rts;
    auto key = std::make_tuple(format, ms, q);
    auto it = m.find(key);
    if (it != m.end()) return it->second;
    IDirect3DSurface9 *s = nullptr;
    HRESULT hr = depth ? d->CreateDepthStencilSurface(16, 16, (D3DFORMAT)format, (D3DMULTISAMPLE_TYPE)ms, q, TRUE, &s, nullptr)
                       : d->CreateRenderTarget(16, 16, (D3DFORMAT)format, (D3DMULTISAMPLE_TYPE)ms, q, FALSE, &s, nullptr);
    if (FAILED(hr)) {
        // A format only a texture can have (INTZ: the game's depth buffers are such textures, d3d/device.cpp), or a
        // target the device makes only as a texture: its surface
        s = nullptr;
        IDirect3DTexture9 *t = nullptr;
        if (!ms && SUCCEEDED(d->CreateTexture(16, 16, 1, depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET, (D3DFORMAT)format,
                                              D3DPOOL_DEFAULT, &t, nullptr))) {
            t->GetSurfaceLevel(0, &s);
            t->Release(); // the surface keeps it
        }
        if (!s) logf("[warm] no %s of format 0x%x (%u samples, quality %u) for the replay", depth ? "depth buffer" : "target", format, ms, q);
    }
    return m[key] = s;
}

Objects g_obj;

} // namespace

uint32_t drawwarm_light_sig()
{
    uint32_t n, h;
    light_shapes(&n, &h);
    return h;
}

void drawwarm_note(IDirect3DDevice9 *d, D3DPRIMITIVETYPE prim, uint64_t sig)
{
    if (!g_cfg.draw_warm) return;
    if (sig == g_last_sig) return;
    g_last_sig = sig;
    if (!g_sig_seen.insert(sig)) return;
    Rec r;
    if (!capture(d, prim, r)) return;
    std::lock_guard<std::mutex> g(g_lock);
    load();
    if (!g_have.insert(fnv(&r, sizeof r)).second) return;
    g_recs.push_back(r);
    g_learned.push_back(r);
    g_dirty = true;
    g_new++;
}

void drawwarm_frame()
{
    static uint32_t last;
    uint32_t frame = g_frame_count.load(std::memory_order_relaxed);
    if (frame - last < 600) return; // about every 10 s while new states come
    last = frame;
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_dirty) return;
    g_dirty = false;
    logf("[warm] %u new draw states so far, %zu learned in all: saved", g_new, g_learned.size());
    save_learned();
}

void drawwarm_replay(IDirect3DDevice9 *d)
{
    if (!g_cfg.draw_warm) return;
    std::lock_guard<std::mutex> g(g_lock);
    load();
    if (g_recs.empty()) return;
    LARGE_INTEGER t0, t1, t2, f;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    IDirect3DStateBlock9 *sb = nullptr;
    d->CreateStateBlock(D3DSBT_ALL, &sb);
    IDirect3DSurface9 *old_rt = nullptr, *old_z = nullptr;
    d->GetRenderTarget(0, &old_rt);
    d->GetDepthStencilSurface(&old_z);
    Objects &o = g_obj;
    if (!o.vb && SUCCEEDED(d->CreateVertexBuffer(65536, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &o.vb, nullptr))) {
        void *p = nullptr;
        if (SUCCEEDED(o.vb->Lock(0, 0, &p, 0))) {
            memset(p, 0, 65536);
            o.vb->Unlock();
        }
    }
    uint32_t drawn = 0, no_shader = 0, no_target = 0, failed = 0, no_tex = 0;
    host3d::begin_scene();
    for (const Rec &r : g_recs) {
        IUnknown *vs = r.vs ? warm_lookup(d, true, r.vs) : nullptr;
        IUnknown *ps = r.ps ? warm_lookup(d, false, r.ps) : nullptr;
        if ((r.vs && !vs) || (r.ps && !ps)) {
            no_shader++;
            if (vs) vs->Release();
            if (ps) ps->Release();
            continue;
        }
        IDirect3DSurface9 *rt = surface(d, o, false, r.rt_format, r.rt_ms, r.rt_msq);
        IDirect3DSurface9 *z = r.z_format ? surface(d, o, true, r.z_format, r.rt_ms, r.rt_msq) : nullptr;
        if (!rt || (r.z_format && !z) || !o.vb) {
            no_target++;
            if (vs) vs->Release();
            if (ps) ps->Release();
            continue;
        }
        d->SetRenderTarget(0, rt);
        d->SetDepthStencilSurface(z);
        D3DVIEWPORT9 vp = {0, 0, 16, 16, 0.0f, 1.0f};
        d->SetViewport(&vp);
        for (int i = 0; i < kNumRs; i++)
            if (i < kFirstFixed || !r.vs) d->SetRenderState(kRs[i], r.rs[i]);
        for (int s = 0; s < 4; s++) {
            IDirect3DBaseTexture9 *t = r.tex[s].type ? dummy_tex(d, o, r.tex[s]) : nullptr;
            if (r.tex[s].type && !t) no_tex++;
            d->SetTexture(s, t);
            d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, r.tex[s].srgb);
        }
        if (!r.vs) {
            for (int s = 0; s < 4; s++) {
                d->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, r.tci[s]);
                d->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, r.ttf[s]);
            }
            for (int m = 0; m < 7; m++) { // matrices of the recorded pattern: 0, 1, else 0.5
                D3DMATRIX x;
                for (int i = 0; i < 16; i++)
                    x.m[i / 4][i % 4] = (r.xf_zero[m] >> i & 1) ? 0.0f : (r.xf_one[m] >> i & 1) ? 1.0f : 0.5f;
                d->SetTransform(kXf[m], &x);
            }
            for (uint32_t i = 0; i < 8; i++) { // lights 0..n-1 of the recorded shapes (the state block restores them)
                if (i < r.nlights) {
                    D3DLIGHT9 l = light_of(r.light[i]);
                    d->SetLight(i, &l);
                }
                d->LightEnable(i, i < r.nlights);
            }
        }
        uint32_t stride[16] = {};
        if (r.fvf) {
            d->SetFVF(r.fvf);
            stride[0] = fvf_size(r.fvf);
        } else {
            std::vector<uint8_t> key((const uint8_t *)r.elem, (const uint8_t *)(r.elem + r.nelem));
            IDirect3DVertexDeclaration9 *&decl = o.decls[key];
            if (!decl) d->CreateVertexDeclaration(r.elem, &decl);
            d->SetVertexDeclaration(decl);
            for (uint32_t e = 0; e + 1 < r.nelem && r.elem[e].Stream < 16; e++) {
                uint32_t end = r.elem[e].Offset + type_size(r.elem[e].Type);
                if (end > stride[r.elem[e].Stream]) stride[r.elem[e].Stream] = end;
            }
        }
        for (int s = 0; s < 16; s++) d->SetStreamSource(s, stride[s] ? o.vb : nullptr, 0, stride[s]);
        d->SetVertexShader((IDirect3DVertexShader9 *)vs);
        d->SetPixelShader((IDirect3DPixelShader9 *)ps);
        if (SUCCEEDED(d->DrawPrimitive((D3DPRIMITIVETYPE)r.prim, 0, 1))) drawn++;
        else failed++;
        if (vs) vs->Release();
        if (ps) ps->Release();
    }
    host3d::end_scene();
    QueryPerformanceCounter(&t1);
    // Wait until the driver has done them: an event issued after the last, polled
    bool done = false;
    IDirect3DQuery9 *q = nullptr;
    if (SUCCEEDED(d->CreateQuery(D3DQUERYTYPE_EVENT, &q))) {
        q->Issue(D3DISSUE_END);
        for (int i = 0; i < 30000 && !done; i++) {
            done = q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK;
            if (!done) Sleep(1);
        }
        q->Release();
    }
    QueryPerformanceCounter(&t2);
    for (int s = 0; s < 16; s++) d->SetStreamSource(s, nullptr, 0, 0);
    for (int s = 0; s < 4; s++) d->SetTexture(s, nullptr);
    // The replay's lights off: a device starts with none enabled (the state block puts back those that were)
    for (DWORD i = 0; i < 8; i++) d->LightEnable(i, FALSE);
    d->SetRenderTarget(0, old_rt);
    d->SetDepthStencilSurface(old_z);
    if (old_rt) old_rt->Release();
    if (old_z) old_z->Release();
    if (sb) {
        sb->Apply();
        sb->Release();
    }
    forget_sent_state();
    // The replay's targets and textures are not needed again (a new device replays with new ones)
    o.release();
    logf("[warm] %u draw states drawn in %.0f ms, the driver done %.0f ms later%s; left out: %u (shader not cached), %u (no target), "
         "%u (draw failed); %u stages without their texture",
         drawn, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, (t2.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart,
         done ? "" : " (not confirmed)", no_shader, no_target, failed, no_tex);
}

void drawwarm_drop_all()
{
    std::lock_guard<std::mutex> g(g_lock);
    g_obj.release();
    g_code_keys.clear(); // the shader objects go with the device
    g_sig_seen.clear();
    g_last_sig = 0;
}

} // namespace xd3d
