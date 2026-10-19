// Xbox vertex shaders: declaration parsing (stream/register layout) and the NV2A microcode program.
#pragma once

#include "d3d/state.h"

namespace xd3d {

VertexShader *vs_create(const uint32_t *declaration, const uint32_t *function, uint32_t usage);
void vs_delete(VertexShader *vs);
uint32_t vsdt_size(uint32_t xtype); // bytes of one Xbox D3DVSDT_* element

// Translate the NV2A program to a host vertex shader (vs_2_0). Returns null if not possible.
IDirect3DVertexShader9 *vs_host_shader(VertexShader *vs, uint32_t undivided_stages = 0);
// Stages whose texture coordinate the bound vertex program divides by w itself and the bound pixel shader reads with
// BUMPENVMAP: those are handed on undivided and divided per pixel instead (cfg:vsdivide, docs/07 #10).
uint32_t bump_divide_stages();
uint32_t vs_input_mask(const VertexShader *vs); // vertex registers the declaration provides

} // namespace xd3d
