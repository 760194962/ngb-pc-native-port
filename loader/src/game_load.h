// The game's own load meter (Ninja Gaiden Black): what the time the port spends in Direct3D does to it.
#pragma once

#include <cstdint>

namespace game_load {

void install(bool ngb);              // from install_d3d_hooks; on for Ninja Gaiden Black unless --original-load
bool active();                       // D3D library thunks time their calls (dispatch.cpp)
void on_swap();                      // D3DDevice_Swap, on the thread that presents
bool gpu_callback(uint32_t fn);      // a push-buffer callback: true when it is the GPU timer, handled here

} // namespace game_load

// Called by the D3D library's thunks around each call (dispatch.cpp), registers saved by the caller.
extern "C" void __cdecl game_load_span_enter();
extern "C" void __cdecl game_load_span_leave();
