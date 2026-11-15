#include "d3d/vshader.h"

#include "d3d/draw.h"
#include "d3d/host.h"
#include "perf.h"
#include "d3d/nv2a_vsh.h"
#include "d3d/shader_cache.h"
#include "d3d/state.h"
#include "common.h"
#include "kernel/kexports.h"

#include <d3dcompiler.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace xd3d {

uint32_t vsdt_size(uint32_t t)
{
    switch (t) {
    case 0x12: return 4;  // FLOAT1
    case 0x22: return 8;  // FLOAT2
    case 0x32: return 12; // FLOAT3
    case 0x42: return 16; // FLOAT4
    case 0x40: return 4;  // D3DCOLOR
    case 0x25: return 4;  // SHORT2
    case 0x45: return 8;  // SHORT4
    case 0x11: return 2;  // NORMSHORT1
    case 0x21: return 4;  // NORMSHORT2
    case 0x31: return 6;  // NORMSHORT3
    case 0x41: return 8;  // NORMSHORT4
    case 0x16: return 4;  // NORMPACKED3
    case 0x15: return 2;  // SHORT1
    case 0x35: return 6;  // SHORT3
    case 0x14: return 1;  // PBYTE1
    case 0x24: return 2;  // PBYTE2
    case 0x34: return 3;  // PBYTE3
    case 0x44: return 4;  // PBYTE4
    case 0x72: return 12; // FLOAT2H
    case 0x02: return 0;  // NONE
    default: return 0;
    }
}

// Every vertex shader object alive, for a new device (vs_drop_host_all): their host shaders belong to the old one
std::unordered_set<VertexShader *> g_live_vs;
std::mutex g_live_lock;

VertexShader *vs_create(const uint32_t *decl, const uint32_t *function, uint32_t usage)
{
    auto *vs = (VertexShader *)xk::ExAllocatePoolWithTag(sizeof(VertexShader), 'hsVX');
    memset(vs, 0, sizeof *vs);
    {
        std::lock_guard<std::mutex> g(g_live_lock);
        g_live_vs.insert(vs);
    }
    vs->magic = VSH_MAGIC;
    vs->usage = usage;
    uint32_t stream = 0, offset[16] = {};
    for (int guard = 0; decl && guard < 512; guard++) {
        uint32_t tok = *decl++;
        uint32_t type = tok >> 29;
        if (tok == 0xFFFFFFFF || type == 7) break;
        switch (type) {
        case 0: // NOP
            break;
        case 1: // STREAM
            stream = tok & 0xF;
            break;
        case 2: // STREAMDATA
            if (tok & 0x10000000) { // skip
                uint32_t n = (tok >> 16) & 0xF;
                offset[stream] += (tok & 0x08000000) ? n : n * 4;
            } else if (vs->num_elements < 32) {
                VertexElement &e = vs->elements[vs->num_elements++];
                e.stream = (uint8_t)stream;
                e.reg = (uint8_t)(tok & 0x1F);
                e.xtype = (uint8_t)((tok >> 16) & 0xFF);
                e.offset = (uint16_t)offset[stream];
                offset[stream] += vsdt_size(e.xtype);
            }
            break;
        case 3: // TESSELLATOR
            break;
        case 4: { // CONSTMEM: count in bits 25-28, address in bits 0-7, then 4 dwords per constant
            uint32_t count = (tok >> 25) & 0xF, reg = tok & 0xFF;
            KNOTE_ONCE("vertex shader declaration loads %u constants at %u (applied at creation)", count, reg);
            decl += count * 4;
            break;
        }
        case 5: // EXT
            decl += (tok >> 24) & 0x1F;
            break;
        }
    }
    for (int s = 0; s < 16; s++) vs->stream_stride[s] = offset[s];
    if (function) {
        uint32_t header = function[0];
        vs->program_type = header & 0xFFFF;
        vs->program_insts = header >> 16;
        vs->program = (uint32_t *)xk::ExAllocatePoolWithTag(vs->program_insts * 16 + 16, 'grPV');
        memcpy(vs->program, function + 1, vs->program_insts * 16);
    }
    static std::atomic<int> logged{0};
    if (logged++ < 1000) {
        std::string decl;
        for (uint32_t i = 0; i < vs->num_elements; i++) {
            char b[48];
            snprintf(b, sizeof b, "%sv%u=s%u+%u:%02x", i ? " " : "", vs->elements[i].reg, vs->elements[i].stream, vs->elements[i].offset,
                     vs->elements[i].xtype);
            decl += b;
        }
        logf("[vsh] shader %p: type 0x%x, %u instructions, decl [%s]", vs, vs->program_type, vs->program_insts, decl.c_str());
        // The disassembly only with --log-vsh: a line per instruction, flushed each, and the game makes its shaders in
        // bursts (a cutscene's first frame wrote 290 lines, 2026-11-15)
        if (vs->program && g_cfg.log_vsh) {
            std::string dis = vsh_disasm(vs->program, vs->program_insts);
            size_t pos = 0;
            while (pos < dis.size()) {
                size_t nl = dis.find('\n', pos);
                logf("[vsh] %s", dis.substr(pos, nl - pos).c_str());
                pos = nl + 1;
            }
        }
    }
    return vs;
}

void vs_delete(VertexShader *vs)
{
    if (!vs) return;
    {
        std::lock_guard<std::mutex> g(g_live_lock);
        g_live_vs.erase(vs);
    }
    decl_forget(vs); // its address may come back as another shader with other inputs
    if (vs->host_vs) vs->host_vs->Release();
    if (vs->host_vs_undivided) vs->host_vs_undivided->Release();
    if (vs->program) xk::ExFreePool(vs->program);
    vs->magic = 0;
    xk::ExFreePool(vs);
}

void vs_drop_host_all()
{
    std::lock_guard<std::mutex> g(g_live_lock);
    for (VertexShader *vs : g_live_vs) {
        if (vs->host_vs) vs->host_vs->Release();
        if (vs->host_vs_undivided) vs->host_vs_undivided->Release();
        vs->host_vs = vs->host_vs_undivided = nullptr;
        vs->host_vs_undivided_stages = 0;
        vs->translate_failed = false; // a failed CreateVertexShader is tried again on the new device
    }
}

uint32_t vs_input_mask(const VertexShader *vs)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < vs->num_elements; i++) m |= 1u << vs->elements[i].reg;
    return m;
}

// The water of chapter 2 computes its refraction coordinates as the vertex's own screen position (x/w, y/w) in the
// vertex program and hands them on with w = 1. Where the water passes the camera a vertex lies behind it (w < 0) and
// its coordinates are a mirrored projection; interpolated across the triangle they read far outside the screen copy
// (r247: 0.13 .. 1.38 where 0.68 is right), a stretched band whenever the camera is at the water line. xemu shows the
// same band (the user, 2026-10-19). The fix keeps the intent: hand on (x, y, z, w) and divide per pixel, which is
// exact everywhere. Only for outputs the program divides itself and the pixel shader reads with BUMPENVMAP.
uint32_t bump_divide_stages()
{
    if (!g_cfg.vs_divide_fix || !g_st.pixel_shader || vs_is_fvf(g_st.vertex_shader)) return 0;
    VertexShader *vs = vs_from_handle(g_st.vertex_shader);
    if (!vs || !vs->program) return 0;
    if (!vs->divided_scanned) {
        vs->divided_tex = vsh_divided_tex_outputs(vs->program, vs->program_insts);
        vs->divided_scanned = true;
    }
    uint32_t modes = render_state()[XRS_PSTEXTUREMODES], r = 0;
    for (int s = 1; s < 4; s++) {
        uint32_t m = (modes >> (5 * s)) & 0x1F;
        if ((vs->divided_tex >> s & 1) && (m == 6 || m == 7)) r |= 1u << s;
    }
    return r;
}

// ---- The game's vertex programs in the shipped seed ----
// The shader seed (shader_seed.bin) cannot hold them: their HLSL is the game's code translated. So the package
// carries only where each program is in the player's own default.xbe (vertex_programs.bin, tools/make_vsh_seed.py,
// as ngbrecomp's d3d-seed.bin does): "NGBVSED1", count, then per program u32 words, input mask, variant (undivided
// stages), profile (0 vs_2_0, 1 vs_2_a), u64 hash of the words (FNV-1a), u64 the bytecode key, u32 offsets, and its
// offset in every build of the game the tool was given. At start the words are read from
// the player's default.xbe at the first offset whose hash matches, translated and compiled into the player's cache
// (a first start only; later starts find them there) and made into their shader objects; a copy of the game where
// no offset matches is left out. A first start otherwise compiled each program the first time an area showed it,
// on the render thread (17-23 ms in the attract demo's first scene, Iris Xe and RTX 3060, 2026-11-14).
//
// --vsh-record FILE (test runs) appends every program translated: the tool's input. Records: "NGBVREC1" once, then
// u32 words, mask, variant (undivided stages), profile (0 vs_2_0, 1 vs_2_a), u64 bytecode key, the words.
namespace {
uint64_t words_hash(const uint32_t *w, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    const uint8_t *b = (const uint8_t *)w;
    for (size_t i = 0; i < (size_t)n * 4; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

void vsh_record(const VertexShader *vs, uint32_t undivided, const char *profile, ID3DBlob *code)
{
    if (g_cfg.vsh_record.empty() || !code) return;
    static std::mutex m;
    static std::unordered_set<uint64_t> seen;
    uint32_t words = vs->program_insts * 4, mask = vs_input_mask(vs), prof = strcmp(profile, "vs_2_a") == 0;
    uint64_t id = words_hash(vs->program, words) ^ ((uint64_t)mask << 32 | undivided << 8 | prof);
    std::lock_guard<std::mutex> g(m);
    if (!seen.insert(id).second) return;
    FILE *f = _wfopen(g_cfg.vsh_record.c_str(), L"ab");
    if (!f) return;
    _fseeki64(f, 0, SEEK_END); // (an append stream's position reads 0 until its first write)
    if (_ftelli64(f) == 0) fwrite("NGBVREC1", 8, 1, f);
    uint64_t key = shader_code_key(code->GetBufferPointer(), code->GetBufferSize());
    uint32_t head[4] = {words, mask, undivided, prof};
    fwrite(head, sizeof head, 1, f);
    fwrite(&key, 8, 1, f);
    fwrite(vs->program, 4, words, f);
    fclose(f);
}
} // namespace

void vsh_seed_prewarm(IDirect3DDevice9 *d)
{
    std::wstring path = g_cfg.port_dir + L"\\vertex_programs.bin";
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    std::vector<uint8_t> s;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
        s.resize((size_t)size);
        if (fread(s.data(), 1, s.size(), f) != s.size()) s.clear();
    }
    fclose(f);
    if (s.size() < 12 || memcmp(s.data(), "NGBVSED1", 8)) {
        logf("[vsh] %s: not a vertex program seed", narrow(path).c_str());
        return;
    }
    LARGE_INTEGER t0, t1, fr;
    QueryPerformanceCounter(&t0);
    std::vector<uint8_t> xbe;
    if (FILE *x = _wfopen(g_cfg.xbe_path.c_str(), L"rb")) {
        fseek(x, 0, SEEK_END);
        long n = ftell(x);
        fseek(x, 0, SEEK_SET);
        if (n > 0) {
            xbe.resize((size_t)n);
            if (fread(xbe.data(), 1, xbe.size(), x) != xbe.size()) xbe.clear();
        }
        fclose(x);
    }
    uint32_t count;
    memcpy(&count, &s[8], 4);
    size_t at = 12;
    unsigned made = 0, missing = 0, failed = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t head[4], noffs;
        uint64_t hash; // then the bytecode key the tool's runs made (make_draw_seed.py's; not read here)
        if (at + 36 > s.size()) break;
        memcpy(head, &s[at], 16);
        memcpy(&hash, &s[at + 16], 8);
        memcpy(&noffs, &s[at + 32], 4);
        at += 36;
        if (noffs > 64 || at + (size_t)noffs * 4 > s.size()) break;
        const uint32_t words = head[0], mask = head[1], undivided = head[2];
        const char *profile = head[3] ? "vs_2_a" : "vs_2_0";
        const uint32_t *prog = nullptr;
        for (uint32_t k = 0; k < noffs && !prog; k++) {
            uint32_t off;
            memcpy(&off, &s[at + k * 4], 4);
            if (words && (size_t)off + (size_t)words * 4 <= xbe.size() && words_hash((const uint32_t *)&xbe[off], words) == hash)
                prog = (const uint32_t *)&xbe[off];
        }
        at += (size_t)noffs * 4;
        if (!prog) { // another copy of the game, or no default.xbe readable
            missing++;
            continue;
        }
        std::vector<uint32_t> w(prog, prog + words); // aligned
        std::string hlsl = vsh_to_hlsl(w.data(), words / 4, mask, undivided);
        ID3DBlob *code = nullptr, *errors = nullptr;
        HRESULT hr = cached_compile(hlsl, "nv2a_vsh", profile, D3DCOMPILE_OPTIMIZATION_LEVEL2, &code, &errors);
        IDirect3DVertexShader9 *obj = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(create_vertex_shader(d, code, &obj))) {
            obj->Release(); // the warm set keeps it for the game's first draw (take_warm)
            made++;
        } else {
            failed++;
        }
        if (code) code->Release();
        if (errors) errors->Release();
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fr);
    logf("[vsh] seed: %u of %u game vertex programs made in %.0f ms (%u not in this copy of the game%s)", made, count,
         (t1.QuadPart - t0.QuadPart) * 1000.0 / fr.QuadPart, missing, failed ? ", some failed to compile" : "");
}

IDirect3DVertexShader9 *vs_host_shader(VertexShader *vs, uint32_t undivided_stages)
{
    if (!vs || !vs->program) return nullptr;
    if (undivided_stages) {
        if (vs->host_vs_undivided && vs->host_vs_undivided_stages == undivided_stages) return vs->host_vs_undivided;
        std::string hlsl = vsh_to_hlsl(vs->program, vs->program_insts, vs_input_mask(vs), undivided_stages);
        ID3DBlob *code = nullptr, *errors = nullptr;
        IDirect3DVertexShader9 *hv = nullptr;
        static const char *const kUndivided[] = {"vs_2_a", nullptr};
        const char *used = nullptr;
        Compiled c = compile_now_or_later(hlsl, "nv2a_vsh", kUndivided, D3DCOMPILE_OPTIMIZATION_LEVEL2, &code, &errors, &used);
        if (c == Compiled::kLater) { // on the compile thread: this draw is left out
            g_shader_later = true;
            return nullptr;
        }
        if (c == Compiled::kReady && SUCCEEDED(create_vertex_shader(host3d::dev(), code, &hv))) {
            if (vs->host_vs_undivided) vs->host_vs_undivided->Release();
            vs->host_vs_undivided = hv;
            vs->host_vs_undivided_stages = undivided_stages;
            logf("[vsh] shader %p: texture outputs 0x%x handed on undivided (divided per pixel)", vs, undivided_stages);
            vsh_record(vs, undivided_stages, "vs_2_a", code);
        }
        if (code) code->Release();
        if (errors) errors->Release();
        if (hv) return hv; // else fall back to the plain translation
    }
    if (vs->host_vs || vs->translate_failed) return vs->host_vs;
    std::string hlsl = vsh_to_hlsl(vs->program, vs->program_insts, vs_input_mask(vs));
    perf::Scope perf_scope(perf::kCompile);
    perf::add_compile();
    ID3DBlob *code = nullptr, *errors = nullptr;
    static const char *const kProfiles[] = {"vs_2_0", "vs_2_a", nullptr};
    const char *used = "vs_2_0";
    Compiled c = compile_now_or_later(hlsl, "nv2a_vsh", kProfiles, D3DCOMPILE_OPTIMIZATION_LEVEL2, &code, &errors, &used);
    if (c == Compiled::kLater) { // on the compile thread: this draw is left out (not marked failed: asked again)
        g_shader_later = true;
        return nullptr;
    }
    HRESULT hr = c == Compiled::kReady ? S_OK : E_FAIL;
    if (FAILED(hr)) {
        vs->translate_failed = true;
        logf("[vsh] compile failed for shader %p: %s", vs, errors ? (const char *)errors->GetBufferPointer() : "?");
        static std::atomic<int> dumped{0};
        if (dumped++ < 3) logf("[vsh] HLSL:\n%s", hlsl.c_str());
    } else if (FAILED(create_vertex_shader(host3d::dev(), code, &vs->host_vs))) {
        vs->translate_failed = true;
        logf("[vsh] CreateVertexShader failed for shader %p", vs);
    } else {
        static std::atomic<int> n{0};
        if (++n <= 30) logf("[vsh] shader %p translated (%u instructions)", vs, vs->program_insts);
        vsh_record(vs, 0, used, code);
    }
    if (code) code->Release();
    if (errors) errors->Release();
    return vs->host_vs;
}

} // namespace xd3d
