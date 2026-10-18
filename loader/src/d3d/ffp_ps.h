// The fixed-function texture stages (D3DTSS_COLOROP / ALPHAOP / args of stages 0-3, TEXTUREFACTOR, SPECULARENABLE)
// as a generated ps_2_0 shader, in place of the host's own fixed-function pixel pipeline.
#pragma once

#include <d3d9.h>

namespace xd3d {

// The shader for the texture stage states now in the Xbox arrays (cached by content), or null to let the host's
// fixed-function pipeline draw. Samplers and texture coordinates come from the host's vertex pipeline as usual.
// has_diffuse / has_specular: whether the vertex format carries those colours (with lighting off a missing
// diffuse reads as opaque white and a missing specular as 0, as Direct3D's fixed function defines).
// pretransformed: XYZRHW vertices, which lighting never touches (their colours are the vertex ones or the defaults).
IDirect3DPixelShader9 *ffp_pixel_shader(bool has_diffuse, bool has_specular, bool pretransformed);
// c0 = TEXTUREFACTOR for the shader above.
void upload_ffp_constants(IDirect3DDevice9 *d);

} // namespace xd3d
