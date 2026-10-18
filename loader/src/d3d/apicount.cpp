// --api-count (diagnosis): counts the Direct3D 9 calls the port makes, by kind, and logs the per-frame averages every
// 600 frames. The device's vtable entries are replaced by counting thunks (indices from IDirect3DDevice9's declaration
// order in d3d9.h). Used to see where the per-draw cost of D3D9On12 on integrated GPUs comes from.
#include "d3d/apicount.h"

#include "common.h"

#include <d3d9.h>

namespace apicount {
namespace {

enum Kind { kTransform, kMaterial, kRenderState, kTexture, kTss, kSampler, kDraw, kDecl, kFvf, kVs, kVsConst, kStream,
            kIndices, kPs, kPsConst, kKinds };
const char *const kNames[kKinds] = {"SetTransform", "SetMaterial", "SetRenderState", "SetTexture", "SetTextureStageState",
                                    "SetSamplerState", "Draw*", "SetVertexDeclaration", "SetFVF", "SetVertexShader",
                                    "SetVertexShaderConstantF", "SetStreamSource", "SetIndices", "SetPixelShader",
                                    "SetPixelShaderConstantF"};
uint64_t g_count[kKinds];
bool g_installed;

#define THUNK(index, kind, name, params, args)                                                                              \
    typedef HRESULT(__stdcall *name##_t) params;                                                                           \
    name##_t o_##name;                                                                                                      \
    HRESULT __stdcall t_##name params                                                                                       \
    {                                                                                                                       \
        g_count[kind]++;                                                                                                    \
        return o_##name args;                                                                                               \
    }
THUNK(44, kTransform, SetTransform, (IDirect3DDevice9 * d, D3DTRANSFORMSTATETYPE a, const D3DMATRIX *b), (d, a, b))
THUNK(49, kMaterial, SetMaterial, (IDirect3DDevice9 * d, const D3DMATERIAL9 *a), (d, a))
THUNK(57, kRenderState, SetRenderState, (IDirect3DDevice9 * d, D3DRENDERSTATETYPE a, DWORD b), (d, a, b))
THUNK(65, kTexture, SetTexture, (IDirect3DDevice9 * d, DWORD a, IDirect3DBaseTexture9 *b), (d, a, b))
THUNK(67, kTss, SetTextureStageState, (IDirect3DDevice9 * d, DWORD a, D3DTEXTURESTAGESTATETYPE b, DWORD c), (d, a, b, c))
THUNK(69, kSampler, SetSamplerState, (IDirect3DDevice9 * d, DWORD a, D3DSAMPLERSTATETYPE b, DWORD c), (d, a, b, c))
THUNK(81, kDraw, DrawPrimitive, (IDirect3DDevice9 * d, D3DPRIMITIVETYPE a, UINT b, UINT c), (d, a, b, c))
THUNK(82, kDraw, DrawIndexedPrimitive, (IDirect3DDevice9 * d, D3DPRIMITIVETYPE a, INT b, UINT c, UINT e, UINT f, UINT g),
      (d, a, b, c, e, f, g))
THUNK(83, kDraw, DrawPrimitiveUP, (IDirect3DDevice9 * d, D3DPRIMITIVETYPE a, UINT b, const void *c, UINT e), (d, a, b, c, e))
THUNK(84, kDraw, DrawIndexedPrimitiveUP,
      (IDirect3DDevice9 * d, D3DPRIMITIVETYPE a, UINT b, UINT c, UINT e, const void *f, D3DFORMAT g, const void *h, UINT i),
      (d, a, b, c, e, f, g, h, i))
THUNK(87, kDecl, SetVertexDeclaration, (IDirect3DDevice9 * d, IDirect3DVertexDeclaration9 *a), (d, a))
THUNK(89, kFvf, SetFVF, (IDirect3DDevice9 * d, DWORD a), (d, a))
THUNK(92, kVs, SetVertexShader, (IDirect3DDevice9 * d, IDirect3DVertexShader9 *a), (d, a))
THUNK(94, kVsConst, SetVertexShaderConstantF, (IDirect3DDevice9 * d, UINT a, const float *b, UINT c), (d, a, b, c))
THUNK(100, kStream, SetStreamSource, (IDirect3DDevice9 * d, UINT a, IDirect3DVertexBuffer9 *b, UINT c, UINT e), (d, a, b, c, e))
THUNK(104, kIndices, SetIndices, (IDirect3DDevice9 * d, IDirect3DIndexBuffer9 *a), (d, a))
THUNK(107, kPs, SetPixelShader, (IDirect3DDevice9 * d, IDirect3DPixelShader9 *a), (d, a))
THUNK(109, kPsConst, SetPixelShaderConstantF, (IDirect3DDevice9 * d, UINT a, const float *b, UINT c), (d, a, b, c))

template <typename T> void hook(void **vt, int index, T &orig, T thunk)
{
    orig = (T)vt[index];
    vt[index] = (void *)thunk;
}

} // namespace

void install(IDirect3DDevice9 *dev)
{
    if (g_installed || !dev) return;
    void **vt = *(void ***)dev;
    DWORD old;
    if (!VirtualProtect(vt, 119 * sizeof(void *), PAGE_READWRITE, &old)) {
        logf("[api] vtable not writable: --api-count off");
        return;
    }
    hook(vt, 44, o_SetTransform, t_SetTransform);
    hook(vt, 49, o_SetMaterial, t_SetMaterial);
    hook(vt, 57, o_SetRenderState, t_SetRenderState);
    hook(vt, 65, o_SetTexture, t_SetTexture);
    hook(vt, 67, o_SetTextureStageState, t_SetTextureStageState);
    hook(vt, 69, o_SetSamplerState, t_SetSamplerState);
    hook(vt, 81, o_DrawPrimitive, t_DrawPrimitive);
    hook(vt, 82, o_DrawIndexedPrimitive, t_DrawIndexedPrimitive);
    hook(vt, 83, o_DrawPrimitiveUP, t_DrawPrimitiveUP);
    hook(vt, 84, o_DrawIndexedPrimitiveUP, t_DrawIndexedPrimitiveUP);
    hook(vt, 87, o_SetVertexDeclaration, t_SetVertexDeclaration);
    hook(vt, 89, o_SetFVF, t_SetFVF);
    hook(vt, 92, o_SetVertexShader, t_SetVertexShader);
    hook(vt, 94, o_SetVertexShaderConstantF, t_SetVertexShaderConstantF);
    hook(vt, 100, o_SetStreamSource, t_SetStreamSource);
    hook(vt, 104, o_SetIndices, t_SetIndices);
    hook(vt, 107, o_SetPixelShader, t_SetPixelShader);
    hook(vt, 109, o_SetPixelShaderConstantF, t_SetPixelShaderConstantF);
    VirtualProtect(vt, 119 * sizeof(void *), old, &old);
    g_installed = true;
    logf("[api] counting Direct3D 9 calls (--api-count)");
}

void frame(uint32_t frame_no)
{
    if (!g_installed || frame_no % 600) return;
    std::string line;
    char b[64];
    for (int k = 0; k < kKinds; k++) {
        snprintf(b, sizeof b, " %s %.1f", kNames[k], g_count[k] / 600.0);
        line += b;
        g_count[k] = 0;
    }
    logf("[api] frame %u, calls per frame:%s", frame_no, line.c_str());
}

} // namespace apicount
