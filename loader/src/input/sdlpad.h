// The controllers XInput does not see (DualShock 3/4, DualSense, Switch Pro, Joy-Con, DirectInput pads and adapters),
// read through SDL3 on a thread of their own and handed to the pad poll in Xbox layout (input/xinput.cpp).
// XInput pads stay with XInput: SDL skips them, so no pad is read twice.
#pragma once

#include <cstdint>

namespace sdlpad {

// XInput's XINPUT_GAMEPAD layout, the SDL pads merged: buttons as XINPUT_GAMEPAD_* (D-pad 0x1-0x8, START 0x10,
// BACK 0x20, thumbs 0x40/0x80, shoulders 0x100/0x200, A B X Y 0x1000-0x8000)
struct State {
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
};

void start();                          // once, when the game initializes its input (XInitDevices)
bool read(State &s);                   // false while no SDL pad is open
void rumble(uint16_t low, uint16_t high); // the game's motors (left = low frequency), passed on by the pad thread

} // namespace sdlpad
