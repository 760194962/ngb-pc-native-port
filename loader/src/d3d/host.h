// Host side of the graphics layer: the game window (own UI thread) and the Direct3D 9 device.
#pragma once

#include "common.h"

#include <d3d9.h>

namespace host3d {

// Starts the UI thread; returns once the window exists. A zero width fits the largest aspect_w:aspect_h client
// area on the screen; a zero height alone follows from the width and the aspect.
bool open_window(int width, int height, int aspect_w, int aspect_h);
HWND window();
void set_title(const wchar_t *text);
// Copies of the program's icon (group 1), taken before the XBE is mapped over its image (main.cpp); the window's.
void load_program_icons();
// The game's name for the window title: Ninja Gaiden Black x2.1 and the programs it launches, or BLACK.
const wchar_t *game_title();
bool create_device(); // made on the window's thread (exclusive fullscreen recovers only so); back buffer = the largest monitor
// The part of the back buffer this frame uses (the client area, shrunk to fit the back buffer if needed) and
// the picture inside it (the game's aspect, centred; the rest are black bars). present() shows that part.
void frame_layout(RECT *frame, RECT *picture);
// Mouse while captured (a click in the window captures it; focus loss releases): raw counts since the last
// call, wheel notches (+ = away from the user), buttons bit 0-4 = left, right, middle, X1, X2.
bool mouse_captured();
void request_capture(bool on); // from any thread; capturing needs the window in front
void set_fullscreen(bool on);  // from any thread (PC SETTINGS > VIDEO > FULLSCREEN)
bool fullscreen();
void mouse_take(int *dx, int *dy, int *wheel);
int mouse_buttons();
IDirect3DDevice9 *dev();
// INTZ: a depth-stencil format that can also be sampled as a texture (driver extension, all current GPUs)
constexpr D3DFORMAT FMT_INTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');
D3DFORMAT depth_view_format(); // R32F when it can be a filtered render target, else A8R8G8B8 (d3d/host.cpp)
bool intz();
void begin_scene();                         // BeginScene once per frame, lazily
void end_scene();                           // close the scene (before StretchRect); the next draw reopens one
HRESULT present();                          // EndScene + Present (D3DERR_DEVICELOST: see xd3d recover_device)
// Reset with the creation parameters: every D3DPOOL_DEFAULT resource and reference to the back buffer and depth
// surface must be released first; afterwards Direct3D is back at its default states.
bool reset_device();
HRESULT test_cooperative_level(); // TestCooperativeLevel on the window's thread (as reset_device)
bool window_closed();

} // namespace host3d
