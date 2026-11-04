// The first USB host controller (OHCI) with an Xbox controller on it, for programs that drive USB themselves (usb_ohci.cpp)
#pragma once

void usb_ohci_install();                        // main: only for a program without a build profile
bool usb_ohci_connect_interrupt(void *kinterrupt); // KeConnectInterrupt: true if it is the controller's (bus level 1)
