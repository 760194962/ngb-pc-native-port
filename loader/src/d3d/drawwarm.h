// Draw states recorded and drawn once at start, so the driver builds its programs for them then and not in the frame
// an area first shows them.
//
// An RTX 3060 builds the program of a draw state -- shaders, vertex layout and the fixed-function state around them --
// at the first draw that uses it, every start of the game: through the host's fixed-function vertex pipeline inside
// that draw call (23-37 ms per new area), through vertex shaders on its own thread with the next Present waiting for
// it (72-132 ms in the attract demo's first scenes with the generated fixed-function shaders, 2026-11-15). Making the
// shader objects at start (prewarm_shaders) does not build them. So each new draw state the game uses is recorded
// (cache\draw_states.bin, and the package's draw_states.bin made from test runs: tools/make_draw_seed.py), and at
// device creation every recorded state is set on a small target and drawn once, and the start waits until the driver
// is done with them.
#pragma once

#include <d3d9.h>

#include <cstdint>

namespace xd3d {

// Before a draw call: record the host's draw state if it is one not seen yet. sig: a hash of what the draw state comes
// from (draw.cpp), so a draw whose state was seen costs a lookup; a new one is read from the device exactly.
void drawwarm_note(IDirect3DDevice9 *d, D3DPRIMITIVETYPE prim, uint64_t sig);
// At device creation, after prewarm_shaders: every recorded state drawn once
void drawwarm_replay(IDirect3DDevice9 *d);
void drawwarm_drop_all(); // a new device: the replay's own objects
void drawwarm_frame();    // each frame: the new states written to the learned file now and then
// The enabled lights' shapes hashed (what of them shapes the driver's fixed-function program), for draw.cpp's
// signature of a lit fixed-function draw; recomputed only when the lights or the material changed
uint32_t drawwarm_light_sig();

} // namespace xd3d
