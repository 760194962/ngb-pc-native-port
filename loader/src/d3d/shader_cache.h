// HLSL compiles with an on-disk cache of their bytecode (cache\shaders next to the program).
//
// The game's vertex programs and register combiner setups are translated to HLSL when first drawn, and D3DCompile
// takes tens of milliseconds each: a new game's opening cutscene met 42 new shaders in two frames, 263 + 74 ms
// ([hitch] lines, 2026-10-22), and every start of the program compiled them again. A cache entry holds the HLSL it
// was made from and is used only when that text, the profile and the flags match exactly.
//
// With the bytecode cached, the driver's own work to make the shader object was what remained (about 20 ms in that
// frame on the RTX 3060, perf 2026-10-22_07-19-32), so at device creation every cached shader is made once
// (prewarm_shaders) and handed out when the same bytecode is asked for.
#pragma once

#include <d3d9.h>
#include <d3dcompiler.h>

#include <string>

namespace xd3d {

// D3DCompile(hlsl, entry "main", profile, flags), from the cache when it holds this exact source.
HRESULT cached_compile(const std::string &hlsl, const char *name, const char *profile, UINT flags, ID3DBlob **code,
                       ID3DBlob **errors);

// At device creation: make the shader objects of every cached entry.
void prewarm_shaders(IDirect3DDevice9 *d);

// CreateVertexShader / CreatePixelShader for compiled bytecode, taking a prewarmed object of the same bytecode first.
HRESULT create_vertex_shader(IDirect3DDevice9 *d, ID3DBlob *code, IDirect3DVertexShader9 **out);
HRESULT create_pixel_shader(IDirect3DDevice9 *d, ID3DBlob *code, IDirect3DPixelShader9 **out);

} // namespace xd3d
