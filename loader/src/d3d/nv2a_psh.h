// NV2A register combiners (Xbox pixel shaders) -> HLSL ps_2_0.
#pragma once

#include <cstdint>
#include <string>

namespace xd3d {

// D3DPIXELSHADERDEF (Xbox). The first 57 dwords equal render states 0-56.
struct PixelShaderDef {
    uint32_t PSAlphaInputs[8];          // 0x00
    uint32_t PSFinalCombinerInputsABCD; // 0x20
    uint32_t PSFinalCombinerInputsEFG;  // 0x24
    uint32_t PSConstant0[8];            // 0x28
    uint32_t PSConstant1[8];            // 0x48
    uint32_t PSAlphaOutputs[8];         // 0x68
    uint32_t PSRGBInputs[8];            // 0x88
    uint32_t PSCompareMode;             // 0xA8
    uint32_t PSFinalCombinerConstant0;  // 0xAC
    uint32_t PSFinalCombinerConstant1;  // 0xB0
    uint32_t PSRGBOutputs[8];           // 0xB4
    uint32_t PSCombinerCount;           // 0xD4
    uint32_t PSTextureModes;            // 0xD8
    uint32_t PSDotMapping;              // 0xDC
    uint32_t PSInputTexture;            // 0xE0
    uint32_t PSC0Mapping;               // 0xE4
    uint32_t PSC1Mapping;               // 0xE8
    uint32_t PSFinalCombinerConstants;  // 0xEC
};
static_assert(sizeof(PixelShaderDef) == 0xF0, "");

// HLSL source. Constants: c0-c7 stage C0, c8-c15 stage C1, c16/c17 final combiner C0/C1 (all RGBA 0..1).
// Samplers s0-s3. Inputs: COLOR0/COLOR1 (v0/v1), TEXCOORD0-3, FOG.
// tex_cube: bit n set when stage n samples a cube map.
// tex_depth: stages sampling a depth-format texture (shadow maps: the sample is the SHADOWFUNC comparison, 0 or 1);
// shadow_func: D3DRS_SHADOWFUNC as 0..7 (NEVER .. ALWAYS).
// tex_kill: stages with D3DTSS_ALPHAKILL: a fetched texel with alpha 0 discards the pixel.
std::string psh_to_hlsl(const PixelShaderDef &def, uint32_t tex_cube, uint32_t tex_depth = 0, uint32_t shadow_func = 0,
                        uint32_t tex_kill = 0, uint32_t bump_divide = 0);
// bump_divide: BUMPENVMAP stages whose coordinates come undivided (x, y, z, w): divided per pixel (vshader.cpp).

} // namespace xd3d
