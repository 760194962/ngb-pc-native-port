#pragma once

#include "d3d/xd3d.h"

#include <d3d9.h>

namespace xd3d {

extern uint8_t *g_dev;        // Xbox device object
// The screen-space offset the library starts with and adds to SetScreenSpaceOffset (Ninja Gaiden Black: 0.53125,
// CreateDevice 0x3CD008 / SetScreenSpaceOffset 0x3C82F0; BLACK's library is not checked: 0)
float screen_offset_base();
extern uint32_t *g_push_base; // push buffer start
extern uint32_t g_push_dwords;
extern int g_width, g_height; // back buffer size

void hle_flush(); // interpret pending inline push-buffer commands
int sync_transforms_from_device(); // take matrices game code wrote into the device object (bit mask of the ones that changed)
bool sync_material_from_device();  // same for the material at device+0xF00 (true when it changed)
void log_frame_mean_color(uint32_t frame); // --debug-frame N-M: mean colour of each presented frame (flash detector)
constexpr int kDepthViewBytes = -2; // depth memory read through a colour header: Z bytes split into A/R/G, stencil in B
constexpr int kDepthViewRaw = -1;   // depth-format header, combiner program: the depth in all four channels
int shadow_func_code();            // D3DRS_SHADOWFUNC as 0..7 (NEVER .. ALWAYS)
// --flash watch (cfg:flashwatch): per-frame draw statistics, and at Swap a tiny copy of the frame checked for a sudden
// dark or flat frame; such a frame is saved as a PNG with the statistics of the frames around it.
void flash_count_draw(bool to_backbuffer, bool program, bool pixel_shader);
void flash_count_rt();
void flash_record_draw(uint32_t caller, uint32_t tex0, uint32_t vs);
void flash_check(uint32_t frame);
void flash_dump_worst();
void flash_add_var(uint32_t addr); // press-file "flashvar:ADDR"
void logvar_add(uint32_t addr);    // press-file "logvar:ADDR": log a guest dword at each Swap where it changed
void logvar_check(uint32_t frame); // called at every Swap
bool is_backbuffer(const Surface *s);
int backbuffer_width(); // the game's back buffer in Xbox pixels (720 x 480 for Ninja Gaiden Black)
int backbuffer_height();
void backbuffer_cpu_lock(); // D3DSurface_LockRect on the back buffer: the title draws it with the CPU
void vis_begin();                  // NV097_SET_ZPASS_PIXEL_COUNT_ENABLE = 1: start counting (host occlusion query)
HRESULT vis_end(uint32_t Index);   // D3DDevice_EndVisibilityTest: finish the query, write the report slot
void vis_release();                // device reset: the query goes (vis_begin makes it again)
void wait_next_vblank(); // block until the next emulated vblank (D3DDevice_BlockUntilVerticalBlank)

// Render resolution: host render targets are scale_x() x scale_y() times the Xbox size (--scale; in widescreen
// the Xbox pixels are 4/3 wider than tall, so x is scaled 4/3 more to keep host pixels square).
float scale_x();
float scale_y();
float target_scale_x(); // the bound render target's scale (1 for the small post-processing buffers)
float target_scale_y();
uint32_t host_w(uint32_t xbox_w);
uint32_t host_h(uint32_t xbox_h);
void request_render_change(); // the render-size settings in g_cfg changed (PC SETTINGS): applied after the next Swap
void request_render_rebuild(); // re-create every host target after the next Swap, also at the same render size

} // namespace xd3d
