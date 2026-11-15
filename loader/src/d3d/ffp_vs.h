// Direct3D 9's fixed-function vertex pipeline as a generated vs_2_0 shader, for the fixed-function draws with 3D
// positions (XYZ, XYZBn), in place of the host's own fixed-function vertex processing.
//
// Why: the drivers build their own program for each fixed-function vertex state (lighting, light types, fog, the
// vertex format, texture coordinate sources and transforms...) together with the pixel shader, at the first draw that
// uses the combination, inside that draw call, at every start of the game: on an RTX 3060 23-37 ms per new area, the
// frames over 33 ms left after the shader seed and the texture pool (2026-11-15, --hitch-detail: every slow draw call
// had the host's fixed-function vertex pipeline; draws through a vertex shader almost never). A generated shader is
// made ahead (the shader cache and the shipped seed), so nothing is built in the frame an area first shows.
//
// What it does is what Direct3D 9 defines for the same state (the state the host would otherwise be given), so the
// picture stays as it was; --ffp-vs-check compares the two draw by draw. Fog is per pixel in the stage shader
// (d3d/ffp_ps.cpp, from the clip z and w passed in TEXCOORD5), as the host's table fog was. Draws with anything not
// done here (ffp_vs_why says what) go to the host pipeline as before.
#pragma once

#include <d3d9.h>

#include <cstdint>

namespace xd3d {

// The fixed-function vertex state of one draw, as the host would be given it (draw.cpp fills it)
struct FfpVsState {
    uint32_t fvf;              // the vertex format sent (with an appended sprite coordinate)
    bool lighting, specular, localviewer, colorvertex, normalize;
    uint32_t msrc[4];          // D3DRS_DIFFUSE / SPECULAR / AMBIENT / EMISSIVEMATERIALSOURCE
    uint32_t ambient;          // D3DRS_AMBIENT
    uint32_t blend;            // D3DRS_VERTEXBLEND as sent: 0 off, 1-3 weights (D3DVBF_nWEIGHTS)
    uint32_t tci[4], ttf[4];   // D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS as sent
    const D3DMATRIX *texm[4];  // D3DTS_TEXTUREn as sent
    bool points;               // a point list: the shader gives the point size
    bool point_scale;          // D3DRS_POINTSCALEENABLE
    float point_size, point_min, point_max, point_a, point_b, point_c; // as sent
    float viewport_height;     // host pixels (point scaling)
};

// The shader for this state, or null when the state has something not done here (the host pipeline draws then).
IDirect3DVertexShader9 *ffp_vertex_shader(const FfpVsState &s);
const char *ffp_vs_why(); // why the last call gave null ("" when it did not)
void ffp_vs_not(const char *why); // a reason found by the caller (draw.cpp) for the host pipeline
// The constants the shader reads, sent when what they come from changed since the last upload
void upload_ffp_vs_constants(IDirect3DDevice9 *d, const FfpVsState &s);
void ffp_vs_forget_constants(); // the vertex constants were written by another (a vertex program's draw, a Reset)
void ffp_vs_drop_all();         // a new device
void ffp_vs_report();           // at exit: shaders made, draws done each way, why the host pipeline drew the others
void ffp_vs_count(bool generated); // one fixed-function draw with 3D positions, by our shader or by the host's pipeline

// Device state the generated shader reads (device.cpp): the material and lights as the game set them
const D3DMATERIAL9 &ffp_material();
int ffp_enabled_lights(const D3DLIGHT9 **out, int max); // in index order; the number enabled (may be more than max)
extern uint32_t g_ffp_light_gen;                         // bumped when a light, its enable or the material changes

} // namespace xd3d
