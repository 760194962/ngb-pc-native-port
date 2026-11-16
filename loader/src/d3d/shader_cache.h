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

// The first of `profiles` (null-ended, tried in order) that compiles this source, as ngbrecomp 0.9.68 does (#94): from
// the cache; else compiled now while this frame has spent less than the compile budget on compiling (--compile-budget
// MS; 0: always later; -1, the default: always now, as before), else on the compile threads: kLater, and the caller
// leaves its draw out (a few frames later the cache has it). A first sight of many shaders at once (an area the seed
// does not cover) then costs the frame the budget, not 10-40 ms each, but the picture misses those draws meanwhile:
// off by default, an option. *profile: the one that compiled (kReady).
enum class Compiled { kReady, kLater, kFailed };
Compiled compile_now_or_later(const std::string &hlsl, const char *name, const char *const *profiles, UINT flags,
                              ID3DBlob **code, ID3DBlob **errors, const char **profile);
// Set by a shader the current draw needs that compiles later (ps_host_shader, ffp_pixel_shader, vs_host_shader):
// begin_draw leaves the draw out; cleared at each begin_draw
extern bool g_shader_later;
void compile_later_report(); // each frame: the background compiles and the draws left out, now and then in the log
void compile_later_count_draw(); // a draw left out for one (begin_draw)

// At device creation: make the shader objects of every cached entry.
void prewarm_shaders(IDirect3DDevice9 *d);
// --compile-port-shaders (tests): every variant of the port's own pixel shaders compiled into the cache (d3d/device.cpp)
void compile_all_port_shaders();
// The key the shader objects are kept by (an FNV-1a hash of the bytecode), and the object kept for one (a new
// reference), or null: the recorded draw states name their shaders this way (drawwarm.cpp)
uint64_t shader_code_key(const void *code, size_t size);
IUnknown *warm_lookup(IDirect3DDevice9 *d, bool vertex, uint64_t key);
void warm_drop_all(); // a new device: the prewarmed objects not handed out (prewarm_shaders makes them again)

// CreateVertexShader / CreatePixelShader for compiled bytecode, taking a prewarmed object of the same bytecode first.
HRESULT create_vertex_shader(IDirect3DDevice9 *d, ID3DBlob *code, IDirect3DVertexShader9 **out);
HRESULT create_pixel_shader(IDirect3DDevice9 *d, ID3DBlob *code, IDirect3DPixelShader9 **out);

} // namespace xd3d
