#pragma once
// Performance overlay (black.ini perfhud = 1): frame rate, average frame interval, input-to-present latency, the
// graphics card and processor with their use, memory and disk, in the top-right corner of the picture. Off: nothing
// runs. On: the figures are read and drawn into a bitmap once a second on a low-priority thread of its own; the render
// thread only uploads that bitmap when it changed and draws one small quad a frame.
#include <d3d9.h>

#include <string>

namespace perfhud {

// At device creation: the card's name and PCI ids (to find its engines in Windows' GPU counters). The thread starts
// with the first draw while the overlay is on.
void start(const char *gpu_name, unsigned vendor_id, unsigned device_id);
void frame();                                      // a Swap (the frame rate)
void input_changed();                              // a controller/key change this poll (the input-to-present clock starts)
void input_presented();                            // after Present: the input-to-present latency of this frame
void draw(IDirect3DDevice9 *d, const RECT &pic);   // over the host back buffer, inside the picture
void release();                                    // before a Reset (a state block must not outlive it)
void release_all();                                // a new device: every object of the overlay (made again)

// A one-line message at the picture's bottom-right for ms milliseconds (online play's status, net/online.cpp), shown
// whether or not the overlay is on; any thread. Nothing is drawn or checked beyond one atomic load while none shows.
void notice(const std::wstring &text, unsigned ms = 5000);
bool active(); // the overlay is on or a message shows: draw() has something to do

} // namespace perfhud
