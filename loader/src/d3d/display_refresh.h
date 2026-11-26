// Borderless fullscreen on a screen whose refresh the game's clock cannot follow (144, 165, 75 Hz...): the screen is
// switched to 60 Hz at the same size while the game fills it, as exclusive fullscreen does (host.cpp exclusive_mode).
// 60 frames a second on a fixed 165 Hz screen stay for 3, 3, 3, 2 refreshes whatever the clock does; at 60 Hz the clock
// follows the vblanks and each frame stays for one (d3d/frame_clock.h).
//
// The change is temporary (ChangeDisplaySettingsEx with CDS_FULLSCREEN): Windows does not move the desktop's windows for
// it, and puts the desktop's mode back if the program ends without doing so. The loader does it itself on leaving
// borderless, on minimising and on closing; back from minimised it switches again. Same switch as exclusive fullscreen:
// black.ini exclusive_60hz (the key's name since 2.2.34) / --keep-refresh. UI thread only.
#pragma once

#include <windows.h>

namespace display_refresh {

void enter(HWND window); // the window's monitor to ~60 Hz, when its refresh cannot be followed and it has such a mode
void leave();            // the monitor back to the desktop's mode, if enter switched it

} // namespace display_refresh
