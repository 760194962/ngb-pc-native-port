// D3D entry points Ninja Gaiden Black x2.1 calls that BLACK's tables do not cover (docs/02 §2, §9; docs/04).
// The library is the same D3D8LTCG 5849 build, so most of these are thin adapters onto the HLE in d3d/*.cpp:
// LTCG kept the per-state setters as functions here (BLACK inlined them), and a few functions have a different
// register convention. New behaviour (gamma ramp, vertical blank wait, pixel shader creation, vertex shader
// slots) is kept minimal and logged on first use.
#include "common.h"
#include "d3d/device.h"
#include "d3d/perfhud.h"
#include "d3d/dispatch.h"
#include "d3d/nv2a_psh.h"
#include "d3d/pushbuf.h"
#include "d3d/state.h"
#include "d3d/xd3d.h"
#include "kernel/kexports.h"
#include "net/online.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace xd3d {

// Implemented in d3d/device.cpp, d3d/resources.cpp (same translation unit visibility rules as BLACK's tables).
void __stdcall D3DDevice_SetRenderStateNotInline(uint32_t State, uint32_t Value);
void __stdcall D3DDevice_SetTextureStageStateNotInline(uint32_t Stage, uint32_t Type, uint32_t Value);
HRESULT __stdcall Direct3D_CreateDevice(uint32_t Adapter, uint32_t DeviceType, uint32_t hFocusWindow, uint32_t BehaviorFlags,
                                        PresentParameters *pp, uint8_t **ppDevice);
void __stdcall D3DDevice_SetVertexShader(uint32_t Handle);
uint32_t *__stdcall D3D_WriteViewportTransform(uint32_t *pPush);
void __stdcall D3D_UpdateFogState();
Surface *__stdcall D3D_CreateSurface2(uint32_t Width, uint32_t Height, uint32_t Usage, uint32_t Format);

void __stdcall D3DDevice_DrawIndexedVertices(uint32_t PrimitiveType, uint32_t VertexCount, const uint16_t *pIndexData); // d3d/draw.cpp

namespace ngb {

// ---- render states the game sets through the library's per-state functions ----

#define RS_SETTER(fn, state)                                 \
    void __stdcall fn(uint32_t Value)                        \
    {                                                        \
        D3DDevice_SetRenderStateNotInline(state, Value);     \
    }
RS_SETTER(SetRenderState_EdgeAntiAlias, XRS_EDGEANTIALIAS)
RS_SETTER(SetRenderState_ShadowFunc, XRS_SHADOWFUNC)
RS_SETTER(SetRenderState_FogColor, XRS_FOGCOLOR)
RS_SETTER(SetRenderState_CullMode, XRS_CULLMODE)
RS_SETTER(SetRenderState_FrontFace, XRS_FRONTFACE)
RS_SETTER(SetRenderState_ZEnable, XRS_ZENABLE)
RS_SETTER(SetRenderState_LineWidth, XRS_LINEWIDTH)
RS_SETTER(SetRenderState_Dxt1NoiseEnable, XRS_DXT1NOISEENABLE)
RS_SETTER(SetRenderState_YuvEnable, XRS_YUVENABLE)
RS_SETTER(SetRenderState_OcclusionCullEnable, XRS_OCCLUSIONCULLENABLE)
RS_SETTER(SetRenderState_StencilCullEnable, XRS_STENCILCULLENABLE)
RS_SETTER(SetRenderState_RopZCmpAlwaysRead, XRS_ROPZCMPALWAYSREAD)
RS_SETTER(SetRenderState_RopZRead, XRS_ROPZREAD)
RS_SETTER(SetRenderState_DoNotCullUncompressed, XRS_DONOTCULLUNCOMPRESSED)
// D3DRS_ZBIAS as the library does it (D3DDevice_SetRenderState_ZBias, 0x3C5920): z becomes an NV2A polygon offset,
// slope scale -z * 0.25 (constant at 0x4149C0) and -z depth units, with the point / wireframe / solid offset
// enables set to z != 0; ZBIAS itself is only stored. The draw-time depth bias reads the polygon offset states.
void __stdcall SetRenderState_ZBias(uint32_t Value)
{
    float f = -(float)Value, slope = f * 0.25f;
    uint32_t on = Value != 0, fs, fo;
    memcpy(&fs, &slope, 4);
    memcpy(&fo, &f, 4);
    D3DDevice_SetRenderStateNotInline(XRS_POLYGONOFFSETZSLOPESCALE, fs);
    D3DDevice_SetRenderStateNotInline(XRS_POLYGONOFFSETZOFFSET, fo);
    D3DDevice_SetRenderStateNotInline(XRS_POINTOFFSETENABLE, on);
    D3DDevice_SetRenderStateNotInline(XRS_WIREFRAMEOFFSETENABLE, on);
    D3DDevice_SetRenderStateNotInline(XRS_SOLIDOFFSETENABLE, on);
    D3DDevice_SetRenderStateNotInline(XRS_ZBIAS, Value);
}
RS_SETTER(SetRenderState_MultiSampleMode, XRS_MULTISAMPLEMODE)
RS_SETTER(SetRenderState_MultiSampleRenderTargetMode, XRS_MULTISAMPLERENDERTARGETMODE)
RS_SETTER(SetRenderState_MultiSampleAntiAlias, XRS_MULTISAMPLEANTIALIAS)
RS_SETTER(SetRenderState_MultiSampleMask, XRS_MULTISAMPLEMASK)
RS_SETTER(SetRenderState_SampleAlpha, XRS_SAMPLEALPHA)
RS_SETTER(SetRenderState_LogicOp, XRS_LOGICOP)
RS_SETTER(SetRenderState_FillMode, XRS_FILLMODE)
RS_SETTER(SetRenderState_BackFillMode, XRS_BACKFILLMODE)
RS_SETTER(SetRenderState_TwoSidedLighting, XRS_TWOSIDEDLIGHTING)
RS_SETTER(SetRenderState_VertexBlend, XRS_VERTEXBLEND)
RS_SETTER(SetRenderState_PSTextureModes, XRS_PSTEXTUREMODES)

// ---- texture stage states (XDK 5849 D3DTSS_: 22-27 bump env, 28 TEXCOORDINDEX, 29 BORDERCOLOR, 30 COLORKEYCOLOR) ----

void __stdcall SetTextureState_TexCoordIndex(uint32_t Stage, uint32_t Value) { D3DDevice_SetTextureStageStateNotInline(Stage, 28, Value); }
void __stdcall SetTextureState_BumpEnv(uint32_t Stage, uint32_t Type, uint32_t Value) { D3DDevice_SetTextureStageStateNotInline(Stage, Type, Value); }
void __stdcall SetTextureState_BorderColor(uint32_t Stage, uint32_t Value) { D3DDevice_SetTextureStageStateNotInline(Stage, 29, Value); }
void __stdcall SetTextureState_ColorKeyColor(uint32_t Stage, uint32_t Value) { D3DDevice_SetTextureStageStateNotInline(Stage, 30, Value); }

// ---- device ----

// LTCG dropped Adapter, DeviceType and hFocusWindow: eax = BehaviorFlags, stack = pPresentationParameters, ecx = ppDevice.
HRESULT __stdcall CreateDevice(uint32_t BehaviorFlags, PresentParameters *pp, uint8_t **ppDevice)
{
    return Direct3D_CreateDevice(0, 0, 0, BehaviorFlags, pp, ppDevice);
}

// D3D_UpdateProjectionViewportTransform (0x3C67B0, 26 call sites): the library recomputes the screen-space
// transform from the device's viewport and scale fields after game code changed them inline.
void __stdcall UpdateProjectionViewportTransform()
{
    DTRACE("");
    update_viewport_constants();
}

// 0x3C6300 (93 bytes, = BLACK's unnamed 0x26ECF0): emits NV097_SET_CONTROL0 from D3DRS_YuvEnable and ZENABLE == W.
// The HLE reads both render states at draw time.
void __stdcall SetControl0(uint32_t Value) { DTRACE("0x%x", Value); }

// 0x3CA0E0 (ecx = resource): is the resource still in use by the GPU (its fence after the device's GPU time)?
// Nothing is ever busy on the host.
uint32_t __stdcall IsResourceBusy(Resource *pResource)
{
    DTRACE("%p", pResource);
    return 0;
}

// D3D_BlockOnResource (eax = resource): wait until the GPU is done with it before the CPU writes it. The host
// copies every draw's data when it is submitted, so there is nothing to wait for (BLACK's BlockUntilNotBusy).
void __stdcall BlockOnResource(Resource *pResource) { DTRACE("%p", pResource); }

// D3DResource_GetType (ecx = resource): D3DRESOURCETYPE from the header's type bits, textures split by dimension
// (Xbox: SURFACE 1, VOLUME 2, TEXTURE 3, VOLUMETEXTURE 4, CUBETEXTURE 5, VERTEXBUFFER 6, INDEXBUFFER 7, PUSHBUFFER 8,
// PALETTE 9, FIXUP 10).
uint32_t __stdcall GetType(const Resource *pResource)
{
    uint32_t type = (pResource->Common & COMMON_TYPE_MASK) >> COMMON_TYPE_SHIFT;
    uint32_t r;
    switch (type) {
    case RTYPE_VERTEXBUFFER: r = 6; break;
    case RTYPE_INDEXBUFFER: r = 7; break;
    case RTYPE_PUSHBUFFER: r = 8; break;
    case RTYPE_PALETTE: r = 9; break;
    case RTYPE_SURFACE: r = 1; break;
    case RTYPE_FIXUP: r = 10; break;
    default: {
        uint32_t fmt = ((const PixelContainer *)pResource)->Format;
        r = (fmt & FMT_CUBEMAP) ? 5 : (((fmt >> FMT_DIMENSION_SHIFT) & 0xF) == 3) ? 4 : 3;
        break;
    }
    }
    DTRACE("%p) -> %u (", pResource, r);
    return r;
}

// D3D_CreateSurface2 as LTCG linked it here (0x3CA1E0): Usage dropped, Format in edx, Width and Height on the stack.
Surface *__stdcall CreateSurface2(uint32_t Width, uint32_t Height, uint32_t Format)
{
    DTRACE("%u, %u, fmt 0x%x", Width, Height, Format);
    return D3D_CreateSurface2(Width, Height, 0, Format);
}

// 0x3CECF0 (517 bytes, = BLACK's unnamed 0x26D9A0; disassembly in docs/04): the library's texture header
// calculator. LTCG inlined D3DDevice_CreateTexture into the game, which allocates the 0x14-byte header itself and
// asks this for the Format and Size words and the data size (first caller 0x324410: four 256x512 / 512x512
// A8R8G8B8 textures). eax = Width, edx = Height; stack: Depth, Levels (0 = full chain), Format, Pitch (0 = computed),
// Cube, Volume, *pFormat, *pSize; returns the data size in bytes. Same arithmetic as black-pc's init_pixel_container.
static uint32_t log2u(uint32_t v)
{
    uint32_t r = 0;
    while ((1u << r) < v) r++;
    return r;
}

uint32_t __stdcall ComputeTextureHeader(uint32_t Width, uint32_t Height, uint32_t Depth, uint32_t Levels, uint32_t Format,
                                        uint32_t Pitch, uint32_t Cube, uint32_t Volume, uint32_t *pFormat, uint32_t *pSize)
{
    const FormatInfo &fi = format_info(Format);
    if (!Depth) Depth = 1;
    PixelContainer t = {};
    uint32_t bytes;
    if (fi.linear) {
        if (!Pitch) Pitch = ((Width * fi.bpp / 8) + 63) & ~63u;
        t.Format = 1 | 8 | (2u << FMT_DIMENSION_SHIFT) | (Format << FMT_FORMAT_SHIFT) | (1u << FMT_MIPMAP_SHIFT);
        t.Size = (Width - 1) | ((Height - 1) << SIZE_HEIGHT_SHIFT) | ((Pitch / 64 - 1) << SIZE_PITCH_SHIFT);
        bytes = Pitch * Height;
    } else {
        uint32_t maxl = log2u(Width > Height ? Width : Height) + 1;
        if (!Levels || Levels > maxl) Levels = maxl;
        t.Format = 1 | 8 | (Cube ? FMT_CUBEMAP : 0) | ((Volume ? 3u : 2u) << FMT_DIMENSION_SHIFT) | (Format << FMT_FORMAT_SHIFT) |
                   (Levels << FMT_MIPMAP_SHIFT) | (log2u(Width) << FMT_USIZE_SHIFT) | (log2u(Height) << FMT_VSIZE_SHIFT) |
                   (log2u(Depth) << FMT_PSIZE_SHIFT);
        t.Size = 0;
        uint32_t pitch, w, h;
        uint32_t last = texture_level_offset(&t, Cube ? 5 : 0, Levels - 1, &pitch, &w, &h);
        bytes = last + pitch * (fi.compressed ? (h + 3) / 4 : h);
        if (Volume) bytes *= Depth;
    }
    *pFormat = t.Format;
    *pSize = t.Size;
    DTRACE("%ux%ux%u, levels %u, %s, pitch %u, cube %u, volume %u) -> fmt 0x%08x size 0x%08x, %u bytes (", Width, Height, Depth,
           Levels, fi.name, Pitch, Cube, Volume, t.Format, t.Size, bytes);
    return bytes;
}

// Vertex shader slots: LoadVertexShader(Handle, Address) copies a created shader's program into the microcode slot
// `Address`, SelectVertexShader(Handle, Address) runs it. The HLE keeps programs per handle, so a slot just
// remembers the handle loaded into it and selecting is SetVertexShader of that handle.
static uint32_t g_slot_handle[136];

void __stdcall LoadVertexShader(uint32_t Handle, uint32_t Address)
{
    DTRACE("0x%x, slot %u", Handle, Address);
    if (Address < 136) g_slot_handle[Address] = Handle;
}

void __stdcall SelectVertexShader(uint32_t Handle, uint32_t Address)
{
    DTRACE("0x%x, slot %u", Handle, Address);
    if (!Handle && Address < 136) Handle = g_slot_handle[Address];
    D3DDevice_SetVertexShader(Handle);
}

// Constant addressing mode (D3DSCM_96CONSTANTS = 0, 192CONSTANTS = 1, NORESERVEDCONSTANTS = 2): decides how
// SetVertexShaderConstant register numbers map to hardware slots. Logged; the HLE applies BLACK's rule until a
// run shows the game uses another mode.
void __stdcall SetShaderConstantMode(uint32_t Mode)
{
    static uint32_t last = 0xFFFFFFFF;
    if (Mode != last) {
        last = Mode;
        logf("[d3d] SetShaderConstantMode(%u)", Mode);
    }
}

// D3DDevice_CreatePixelShader(pPSDef, pHandle): the library allocates {RefCount, D3DOwned, pDef} and copies the
// definition. SetPixelShader (device.cpp) reads the definition through the handle's third word.
HRESULT __stdcall CreatePixelShader(const PixelShaderDef *pPSDef, uint32_t *pHandle)
{
    auto *def = (PixelShaderDef *)xk::ExAllocatePoolWithTag(sizeof(PixelShaderDef), 'fedP');
    auto *h = (uint32_t *)xk::ExAllocatePoolWithTag(12, 'rdhS');
    if (!def || !h) return D3DERR_OUTOFVIDEOMEMORY;
    memcpy(def, pPSDef, sizeof(PixelShaderDef));
    h[0] = 1;
    h[1] = 1;
    h[2] = (uint32_t)(uintptr_t)def;
    *pHandle = (uint32_t)(uintptr_t)h;
    DTRACE("%p, %p) -> 0x%x (", pPSDef, pHandle, *pHandle);
    return D3D_OK;
}

// D3DDevice_SetGammaRamp(Flags, pRamp): the game's Screen Adjustment (brightness, contrast, gamma). BLACK wrote
// the device's ramp fields inline; here the library function does it. Present applies the current ramp (device.cpp).
void __stdcall SetGammaRamp(uint32_t Flags, const uint8_t *pRamp)
{
    DTRACE("0x%x, %p", Flags, pRamp);
    memcpy(g_dev + DEV_GAMMA_RAMP, pRamp, 0x300);
    *(uint32_t *)(g_dev + DEV_GAMMA_INDEX) = 0;
    *(uint32_t *)(g_dev + DEV_GAMMA_DIRTY) = 1;
}

// D3DDevice_BlockUntilVerticalBlank: wait for the device's vblank event, which the vblank thread signals 60 times a
// second (device.cpp). The game calls it before Swap each frame (genma-pc's double-wait lesson: measured later
// with the perf recorder).
void __stdcall BlockUntilVerticalBlank()
{
    DTRACE("");
    wait_next_vblank(); // not the KEVENT: it is a notification event nothing resets, a wait on it returns at once
}

void __stdcall PersistDisplay() { DTRACE(""); } // keeps the front buffer across a reboot: nothing to keep here

// D3DDevice_RunPushBuffer(pPushBuffer, pFixup): the game queues a pre-recorded NV2A method stream (a D3DPushBuffer
// resource: Common, Data, Lock, Size, AllocationSize; six call sites, first one from 0x30BCA0 when chapter 1 loads).
// Every buffer seen while chapter 1 loads (35 distinct, docs/04) is what D3DDevice_DrawIndexedVertices records:
// NV097_SET_BEGIN_END(primitive), NV097_ARRAY_ELEMENT16 pairs of indices (non-incrementing), one
// NV097_ARRAY_ELEMENT32 for an odd last index, NV097_SET_BEGIN_END(0). The indices refer to the vertex streams bound
// at the time the buffer runs, so the whole thing is an indexed draw with the current state. Anything else in a
// buffer (other methods, jumps, a fixup table) is logged once and skipped.
struct PushBufferHeader {
    uint32_t Common, Data, Lock, Size, AllocationSize;
};

void __stdcall RunPushBuffer(const PushBufferHeader *pPushBuffer, const void *pFixup)
{
    enum { NV097_SET_BEGIN_END = 0x17FC, NV097_ARRAY_ELEMENT16 = 0x1800, NV097_ARRAY_ELEMENT32 = 0x1808 };
    static std::set<uint32_t> reported;
    static std::vector<uint16_t> indices;
    static uint32_t calls, draws;
    calls++;
    DTRACE("%p, %p", pPushBuffer, pFixup);
    if (pFixup && reported.insert(0xFFFFFFFFu).second) logf("[pb] RunPushBuffer: fixup table %p ignored", pFixup);
    const uint32_t *p = (const uint32_t *)phys_to_virt(pPushBuffer->Data);
    uint32_t n = pPushBuffer->Size / 4;
    if (calls == 1 && !g_cfg.screenshot_dir.empty()) { // docs/04 §6: is the stream disc data or built by the game?
        std::wstring path = g_cfg.screenshot_dir + L"\\pb_first.bin";
        if (FILE *f = _wfopen(path.c_str(), L"wb")) {
            fwrite(p, 1, pPushBuffer->Size, f);
            fclose(f);
            logf("[pb] first push buffer (%u bytes at 0x%08x) written to %s", pPushBuffer->Size, pPushBuffer->Data, narrow(path).c_str());
        }
    }
    uint32_t prim = 0;
    bool unknown = false;
    indices.clear();
    for (uint32_t i = 0; i < n;) {
        uint32_t cmd = p[i++];
        if (cmd == 0) continue;
        if ((cmd & 3) == 1 || (cmd & 3) == 2 || (cmd & 0xE0000000) == 0x20000000) { unknown = true; break; }
        uint32_t count = (cmd >> 18) & 0x7FF, m = cmd & 0x1FFC;
        bool noninc = (cmd & 0x40000000) != 0;
        if (i + count > n) { unknown = true; break; }
        if (((cmd >> 13) & 7) != 0) { unknown = true; i += count; continue; }
        for (uint32_t k = 0; k < count; k++) {
            uint32_t method = noninc ? m : m + 4 * k, v = p[i + k];
            if (method == NV097_SET_BEGIN_END) {
                if (v) {
                    prim = v;
                    indices.clear();
                } else if (!indices.empty()) {
                    D3DDevice_DrawIndexedVertices(prim, (uint32_t)indices.size(), indices.data());
                    draws++;
                    indices.clear();
                }
            } else if (method == NV097_ARRAY_ELEMENT16) {
                indices.push_back((uint16_t)v);
                indices.push_back((uint16_t)(v >> 16));
            } else if (method == NV097_ARRAY_ELEMENT32) {
                indices.push_back((uint16_t)v);
            } else {
                unknown = true;
            }
        }
        i += count;
    }
    if (unknown && reported.insert((uint32_t)(uintptr_t)pPushBuffer).second)
        logf("[pb] RunPushBuffer: buffer %p (data 0x%08x, %u bytes) holds methods other than an indexed draw; those were skipped",
             pPushBuffer, pPushBuffer->Data, pPushBuffer->Size);
    if (calls == 1 || calls == 1000 || calls == 10000) logf("[pb] RunPushBuffer: %u calls, %u draws so far", calls, draws);
}

// D3DDevice_EndVisibilityTest (0x3C7C50, eax = Index): see vis_end in d3d/device.cpp.
HRESULT __stdcall EndVisibilityTest(uint32_t Index)
{
    DTRACE("%u", Index);
    pb_flush(); // the inlined begin and the test's draws are in the inline push buffer
    return vis_end(Index);
}

// DSOUND: DirectSoundUseFullHRTF selects the head-related transfer function of the 3D sound hardware; the software
// mixer has distance and panning only (BLACK's DirectSoundUseLightHRTF is the same no-op).
void __stdcall DirectSoundUseFullHRTF() { DTRACE(""); }

// XNET: XNetStartup (0x3AFB34) brings up the network stack, and XnInit programs the NIC's registers directly
// (0xFEF00110 written by FUN_003B786C: an access violation in r116, when the Ranking menu started the online
// module). There is no network here: report it down. The caller FUN_00300BA0 checks the result and gives up
// (0x300C91), the same as with a console without a working connection.
int __stdcall XNetStartup(const void *pParams)
{
    DTRACE("%p", pParams);
    static bool noted;
    if (!noted) {
        noted = true;
        logf("[net] XNetStartup: no network (WSAENETDOWN); the game's online features stay unavailable");
        if (!online_off_reason().empty()) perfhud::notice(online_off_reason(), 8000); // online = 1 but it could not start
    }
    return 10050; // WSAENETDOWN
}

// XNET's WSAStartup brings up the same stack (XnInit, the NIC's registers): Ninja Gaiden 2004's XOnlineStartup calls it
// straight (FUN_00381477 in the title update, when a save is loaded), past XNetStartup, and offline that wrote
// 0xFEF00110 and the game died (the user, chapter 3, 2026-11-02; run/r820). Not in Black's programs (found by name in
// 2004's: 0x3979EB in the update, 0x383E2B on the disc). Offline: the network subsystem is not ready, which
// XOnlineStartup takes as a console without a connection; online play keeps the game's own (d3d/dispatch.cpp).
int __stdcall XnetWsaStartup(uint16_t wVersionRequested, void *lpWSAData)
{
    DTRACE("0x%x, %p", wVersionRequested, lpWSAData);
    static bool noted;
    if (!noted) {
        noted = true;
        logf("[net] WSAStartup: no network (WSASYSNOTREADY); the game's online features stay unavailable");
    }
    return 10091; // WSASYSNOTREADY
}

#undef RS_SETTER
#define H(addr, fn, params, ret) {addr, #fn, (void *)&fn, params, ret}
#define HN(addr, name, fn, params, ret) {addr, name, (void *)&fn, params, ret}

static const Hle kTable[] = {
    H(0x003c5260, SetRenderState_EdgeAntiAlias, "s", false),
    H(0x003c52a0, SetRenderState_ShadowFunc, "s", false),
    H(0x003c52e0, SetRenderState_FogColor, "s", false),
    H(0x003c5340, SetRenderState_CullMode, "s", false),
    H(0x003c53c0, SetRenderState_FrontFace, "s", false),
    H(0x003c5460, SetRenderState_ZEnable, "s", false),
    H(0x003c56a0, SetRenderState_LineWidth, "s", false),
    H(0x003c5710, SetRenderState_Dxt1NoiseEnable, "s", false),
    H(0x003c57a0, SetRenderState_YuvEnable, "s", false),
    H(0x003c57e0, SetRenderState_OcclusionCullEnable, "s", false),
    H(0x003c5850, SetRenderState_StencilCullEnable, "s", false),
    H(0x003c58c0, SetRenderState_RopZCmpAlwaysRead, "s", false),
    H(0x003c58e0, SetRenderState_RopZRead, "s", false),
    H(0x003c5900, SetRenderState_DoNotCullUncompressed, "s", false),
    H(0x003c5920, SetRenderState_ZBias, "s", false),
    H(0x003c59d0, SetRenderState_MultiSampleMode, "s", false),
    H(0x003c5a00, SetRenderState_MultiSampleRenderTargetMode, "s", false),
    H(0x003c5a30, SetRenderState_MultiSampleAntiAlias, "s", false),
    H(0x003c5a70, SetRenderState_MultiSampleMask, "s", false),
    H(0x003c5a90, SetRenderState_SampleAlpha, "s", false),
    H(0x003c5ab0, SetRenderState_LogicOp, "s", false),
    H(0x003c5b10, SetRenderState_FillMode, "s", false),
    H(0x003c5b60, SetRenderState_BackFillMode, "s", false),
    H(0x003c5b80, SetRenderState_TwoSidedLighting, "s", false),
    H(0x003c5be0, SetRenderState_VertexBlend, "s", false),
    H(0x003c5c30, SetRenderState_PSTextureModes, "s", false),
    H(0x003c5c90, SetTextureState_TexCoordIndex, "esi s", false),
    H(0x003c5da0, SetTextureState_BumpEnv, "eax s s", false),
    H(0x003c5e10, SetTextureState_BorderColor, "eax ebx", false),
    H(0x003c5e60, SetTextureState_ColorKeyColor, "eax ebx", false),
    H(0x003cc310, CreateDevice, "eax s ecx", true),
    H(0x003c67b0, UpdateProjectionViewportTransform, "", false),
    H(0x003c6100, D3D_WriteViewportTransform, "eax", true),
    H(0x003c6300, SetControl0, "s", false),
    H(0x003c8f10, D3D_UpdateFogState, "", false),
    H(0x003ca0e0, IsResourceBusy, "ecx", true),
    H(0x003ca060, GetType, "ecx", true),
    H(0x003cadf0, BlockOnResource, "eax", false),
    H(0x003ca1e0, CreateSurface2, "s s edx", true),
    H(0x003cecf0, ComputeTextureHeader, "eax edx s s s s s s s s", true),
    H(0x003c88a0, LoadVertexShader, "eax s", false),
    H(0x003c8930, SelectVertexShader, "eax ebx", false),
    H(0x003c89f0, SetShaderConstantMode, "eax", false),
    H(0x003c92b0, CreatePixelShader, "s s", true),
    H(0x003c6d90, SetGammaRamp, "s s", false),
    H(0x003c7cb0, BlockUntilVerticalBlank, "", false),
    H(0x003c8120, PersistDisplay, "", false),
    H(0x0037a978, DirectSoundUseFullHRTF, "", false),
    H(0x003cafc0, RunPushBuffer, "s s", false),
    H(0x003c7c50, EndVisibilityTest, "eax", true),
    H(0x003afb34, XNetStartup, "s", true),
    HN(0x00000000, "WSAStartup", XnetWsaStartup, "s s", true), // Ninja Gaiden 2004 only, by name
};

#include "auxtab/ngbextra_tables.inc"

} // namespace ngb

const Hle *hle_ngb_extra_table_aux(const char *xbe, size_t *count) { return ngb::hle_ngb_extra_table_aux(xbe, count); }

const Hle *hle_ngb_extra_table(size_t *count)
{
    *count = sizeof ngb::kTable / sizeof ngb::kTable[0];
    return ngb::kTable;
}

} // namespace xd3d
