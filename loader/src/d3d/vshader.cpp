#include "d3d/vshader.h"

#include "d3d/host.h"
#include "perf.h"
#include "d3d/nv2a_vsh.h"
#include "d3d/state.h"
#include "common.h"
#include "kernel/kexports.h"

#include <d3dcompiler.h>

#include <cstring>
#include <string>

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

VertexShader *vs_create(const uint32_t *decl, const uint32_t *function, uint32_t usage)
{
    auto *vs = (VertexShader *)xk::ExAllocatePoolWithTag(sizeof(VertexShader), 'hsVX');
    memset(vs, 0, sizeof *vs);
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
        if (vs->program) {
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
    if (vs->host_vs) vs->host_vs->Release();
    if (vs->host_vs_undivided) vs->host_vs_undivided->Release();
    if (vs->program) xk::ExFreePool(vs->program);
    vs->magic = 0;
    xk::ExFreePool(vs);
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

IDirect3DVertexShader9 *vs_host_shader(VertexShader *vs, uint32_t undivided_stages)
{
    if (!vs || !vs->program) return nullptr;
    if (undivided_stages) {
        if (vs->host_vs_undivided && vs->host_vs_undivided_stages == undivided_stages) return vs->host_vs_undivided;
        std::string hlsl = vsh_to_hlsl(vs->program, vs->program_insts, vs_input_mask(vs), undivided_stages);
        ID3DBlob *code = nullptr, *errors = nullptr;
        IDirect3DVertexShader9 *hv = nullptr;
        if (SUCCEEDED(D3DCompile(hlsl.data(), hlsl.size(), "nv2a_vsh", nullptr, nullptr, "main", "vs_2_a",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &code, &errors)) &&
            SUCCEEDED(host3d::dev()->CreateVertexShader((const DWORD *)code->GetBufferPointer(), &hv))) {
            if (vs->host_vs_undivided) vs->host_vs_undivided->Release();
            vs->host_vs_undivided = hv;
            vs->host_vs_undivided_stages = undivided_stages;
            logf("[vsh] shader %p: texture outputs 0x%x handed on undivided (divided per pixel)", vs, undivided_stages);
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
    HRESULT hr = E_FAIL;
    for (const char *profile : {"vs_2_0", "vs_2_a"}) {
        if (errors) { errors->Release(); errors = nullptr; }
        hr = D3DCompile(hlsl.data(), hlsl.size(), "nv2a_vsh", nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL2, 0,
                        &code, &errors);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) {
        vs->translate_failed = true;
        logf("[vsh] compile failed for shader %p: %s", vs, errors ? (const char *)errors->GetBufferPointer() : "?");
        static std::atomic<int> dumped{0};
        if (dumped++ < 3) logf("[vsh] HLSL:\n%s", hlsl.c_str());
    } else if (FAILED(host3d::dev()->CreateVertexShader((const DWORD *)code->GetBufferPointer(), &vs->host_vs))) {
        vs->translate_failed = true;
        logf("[vsh] CreateVertexShader failed for shader %p", vs);
    } else {
        static std::atomic<int> n{0};
        if (++n <= 30) logf("[vsh] shader %p translated (%u instructions)", vs, vs->program_insts);
    }
    if (code) code->Release();
    if (errors) errors->Release();
    return vs->host_vs;
}

} // namespace xd3d
