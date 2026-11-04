// Decode the faulting instruction of a trapped device-register access and run it on the device (mmio_decode.cpp)
#pragma once

#include <windows.h>

#include <cstdint>

struct MmioOps {
    uint32_t (*read)(uint32_t off, int size);           // off: from the device's register base; size 1, 2 or 4
    void (*write)(uint32_t off, uint32_t v, int size);
};

// Runs the instruction at c->Eip, whose memory operand is the device register at off; steps Eip past it.
// false: a form it does not decode (the caller logs it and lets the fault through).
bool mmio_decode(CONTEXT *c, uint32_t off, const MmioOps &ops);
