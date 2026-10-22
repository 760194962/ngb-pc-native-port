// The virtual network the emulated NIC is plugged into (project/docs/09), laid out like xemu's user-mode network: the
// Xbox is 10.0.2.15, the gateway 10.0.2.2 answers ARP and DHCP, 10.0.2.3 is the DNS server (queries go to Insignia's,
// black.ini online_dns, default 46.101.64.175), and every other UDP datagram leaves through a host socket (NAT).
// Runs on the NIC's network thread only.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

void vnet_start();
void vnet_from_guest(const uint8_t *frame, size_t len);
void vnet_wait(HANDLE wake, int timeout_ms); // wait for the guest (wake) or the host sockets, handle what came in
