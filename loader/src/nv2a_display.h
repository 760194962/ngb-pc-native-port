// The NV2A's display registers and vertical blank for programs that drive the hardware themselves (nv2a_display.cpp)
#pragma once

#include <cstdint>

void nv2a_display_install();                     // main: only for a program without a build profile
void nv2a_display_mode(uint32_t mode, uint32_t format, uint32_t pitch, uint32_t framebuffer); // AvSetDisplayMode
bool nv2a_connect_interrupt(void *kinterrupt);   // KeConnectInterrupt: true if it is the GPU's (bus level 3)
