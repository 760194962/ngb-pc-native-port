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
//
// A first start had no cache: every shader compiled as an area first showed it. The package carries a seed
// (shader_seed.bin beside the program, tools/make_shader_seed.py): its entries are made at start with the player's.
// A cold start on the Iris Xe, title and attract demo for 150 s: 40 compiles (400 ms, frames up to 282 ms) without,
// the 4 vertex programs the seed leaves out (56 ms, up to 113 ms) with; its 421 entries took 16 ms at start.
#pragma once

#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <string>

namespace xd3d {

// D3DCompile(hlsl, entry "main", profile, flags), from the cache when it holds this exact source.
HRESULT cached_compile(const std::string &hlsl, const char *name, const char *profile, UINT flags, ID3DBlob **code,
                       ID3DBlob **errors);

// At device creation: make the shader objects of every cached entry.
void prewarm_shaders(IDirect3DDevice9 *d);
// The key the shader objects are kept by (an FNV-1a hash of the bytecode), and the object kept for one (a new
// reference), or null: the recorded draw states name their shaders this way (drawwarm.cpp)
uint64_t shader_code_key(const void *code, size_t size);
IUnknown *warm_lookup(IDirect3DDevice9 *d, bool vertex, uint64_t key);
void warm_drop_all(); // a new device: the prewarmed objects not handed out (prewarm_shaders makes them again)

// CreateVertexShader / CreatePixelShader for compiled bytecode, taking a prewarmed object of the same bytecode first.
HRESULT create_vertex_shader(IDirect3DDevice9 *d, ID3DBlob *code, IDirect3DVertexShader9 **out);
HRESULT create_pixel_shader(IDirect3DDevice9 *d, ID3DBlob *code, IDirect3DPixelShader9 **out);

} // namespace xd3d
