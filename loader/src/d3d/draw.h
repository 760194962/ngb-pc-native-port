// Draw paths: immediate mode and inline arrays from the push buffer, plus the HLE Draw* functions.
#pragma once

#include <cstddef>
#include <cstdint>

namespace xd3d {
// The last indexed draws handed to the host runtime (ring of 4, newest at g_last_draw_next - 1), for crash dumps.
struct LastDraw {
    const void *verts, *src, *conv;
    size_t bytes, conv_bytes;
    uint32_t stride, mn, mx, nidx, prim;
};
extern LastDraw g_last_draws[4];
extern unsigned g_last_draw_next;
} // namespace xd3d

#include "common.h"

#include <d3d9.h>

namespace xd3d {

// verts: count vertices of 16 attributes x float4; attr_mask: attributes written since SET_BEGIN_END.
void draw_immediate(uint32_t prim, const float *verts, uint32_t count, uint32_t attr_mask);
// dwords of vertex data in the current vertex format.
void draw_inline_array(uint32_t prim, const uint32_t *data, uint32_t dwords);

// cfg:pixwatch: read the watched back-buffer pixel back; a change is logged against the previous caller (draw.cpp).
void pixwatch_check(const char *next, bool depth_read = true);
bool write_png(const wchar_t *path, uint32_t w, uint32_t h, const uint8_t *bgra); // BGRA, top row first
std::string callers_of(const uint32_t *sp); // the game code up the stack (for kernel logs)
// The shaders / vertex input the draws remember having sent are unknown again (after a fullscreen pass).
void forget_sent_shaders();
// After a device Reset: Direct3D is back at its defaults, so every state, texture and shader the draws remember
// having sent is unknown again.
void forget_sent_state();
struct VertexShader;
void decl_forget(const VertexShader *vs); // vs_delete: its vertex declaration
void ms_note_draw();
bool ms_drawing();                       // the bound target is multisampled (MSAA)                     // a draw into the bound target: its MSAA surface is ahead (device.cpp)
void wide_hud_count(bool threed);         // a draw into the back buffer (on a screen not 16:9): the HUD's frame model
void wide_hud_frame_end();                // at Swap: this frame's HUD pieces grouped into elements, each with its side
void draw_drop_all();                     // a new device: the draw ring, declarations, white texel; sent state forgotten
// Another path set the host's D3DTS_TEXTUREn: the stage texture-scale cache sends its matrix again (d3d/draw.cpp).
void host_texture_transform_changed(int stage, const D3DMATRIX *m = nullptr);
uint32_t texture_matrix_stale_count(); // since the last call (diagnosis, logged with --api-count)
// Before a device Reset: the dynamic vertex / index ring buffers of the indexed draws (D3DPOOL_DEFAULT) go away.
void release_draw_buffers();

// Save the host back buffer as a PNG (used by --screenshot).
bool save_screenshot(const wchar_t *path);
// A 32-bit render target surface as PNG: colour, or with alpha_as_gray its alpha channel.
bool save_surface_png(IDirect3DSurface9 *src, const wchar_t *path, bool alpha_as_gray, const RECT *crop = nullptr);
void invalidate_render_state(uint32_t state); // re-send this Xbox render state to the host on the next draw
// --screenshot <dir>: save "<dir>\<tag>.png" now (no-op without the option).
void screenshot_now(const char *tag);
void request_screenshot(const char *tag); // tests: "<dir>\<tag>.png" of the next whole frame (any thread)
void request_device_reset(const char *why = "tests"); // at the next Swap: ResetEx (Alt+Enter, tests) as after a loss
void request_visibility_test();           // tests: at the next Swap, a visibility test (its query then exists)
void request_device_recreate();           // tests: at the next Swap, a new device as for a hung or removed driver

} // namespace xd3d
