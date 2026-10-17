#pragma once

#include "build_profile.h"

// Replace XAPI's SwitchToFiber with one that keeps the Windows thread's stack bounds on the fiber stack (fiber.cpp).
// Returns false when the code at the profile's address is not XAPI's; a profile without fibers is a no-op.
bool install_fiber_switch(const BuildProfile &profile);
