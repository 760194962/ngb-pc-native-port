// The Xbox's Ethernet controller (nForce MCP "nvnet") for online play (project/docs/09). XNET's own driver programs its
// registers at 0xFEF00000 directly: that page is reserved inaccessible and every access traps to a model of the
// controller (after xemu's hw/xbox/mcpx/nvnet, LGPL 2+), which moves frames between the driver's descriptor rings in
// contiguous memory and the host's virtual network (net/vnet.cpp), and raises its interrupt by calling the driver's
// service routine. Installed only when online play is on.
#pragma once

#include <cstddef>
#include <cstdint>

bool nvnet_install();                             // online_init: reserve the register page
void nvnet_install_trap();                        // after the crash handlers: the trap (the model starts on first access)
bool nvnet_connect_interrupt(void *kinterrupt);   // KeConnectInterrupt: true if it is the NIC's (bus level 4)
uint32_t nvnet_link_state();                      // PhyGetLinkState: link up, 100 Mbit/s, full duplex (0 = offline)
void nvnet_to_guest(const uint8_t *frame, size_t len); // net thread: a frame for the Xbox (queued if no buffer is free)
