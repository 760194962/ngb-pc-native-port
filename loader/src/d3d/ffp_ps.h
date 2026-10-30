// The fixed-function texture stages (D3DTSS_COLOROP / ALPHAOP / args of stages 0-3, TEXTUREFACTOR, SPECULARENABLE)
// as a generated ps_2_0 shader, in place of the host's own fixed-function pixel pipeline.
#pragma once

#include <d3d9.h>

#include <cstdint>

namespace xd3d {

// The shader for the texture stage states now in the Xbox arrays (cached by content), or null to let the host's
// fixed-function pipeline draw. Samplers and texture coordinates come from the host's vertex pipeline as usual.
// has_diffuse / has_specular: whether the vertex format carries those colours (with lighting off a missing
// diffuse reads as opaque white and a missing specular as 0, as Direct3D's fixed function defines).
// pretransformed: XYZRHW vertices, which lighting never touches (their colours are the vertex ones or the defaults).
IDirect3DPixelShader9 *ffp_pixel_shader(bool has_diffuse, bool has_specular, bool pretransformed);
// Stages (bit per stage) whose projected coordinates bound the pixel: alpha 0 outside 0..1 (draw.cpp
// apply_texture_stages sets it per draw, before the shader above is picked).
extern uint32_t g_ffp_edge_stages;
// A 2D quad above 1x (draw.cpp quad_snap, ngbrecomp #83): stages (bit per stage) whose coordinates are clamped to the
// range the console's pixel centres sample (c1-c4: u0 v0 u1 v1), and of those the linearly filtered ones read their
// four texels weighted by alpha (c5-c8: width, height, 1/width, 1/height of the host texture). 0 for other draws
// (begin_draw clears them; quad_snap sets them and picks the shader again with ffp_reselect).
extern uint32_t g_ffp_quad_stages, g_ffp_quad_weighted;
IDirect3DPixelShader9 *ffp_reselect(); // the shader for the last ffp_pixel_shader arguments, with the quad bits as now
void upload_ffp_quad_constants(IDirect3DDevice9 *d, const float clamp[4][4], const float size[4][4]);
// c0 = TEXTUREFACTOR for the shader above.
void upload_ffp_constants(IDirect3DDevice9 *d);
void ffp_forget_constants(); // c0 rewritten elsewhere: the next upload sends it
void ffp_drop_all();         // a new device: every shader made (made again on demand)

} // namespace xd3d
