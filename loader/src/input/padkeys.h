// Ninja Gaiden Black's controls on PC: keys pressed into the Xbox pad, and the PC pad's buttons remapped. The game
// only reads its pad (it has no keyboard support of its own), so a key simply presses a pad input: a button, a
// trigger, a D-pad direction or a stick fully to one side. black.ini: kb_<input> = Key[,Key] (two keys at most),
// padmap_<input> = <input> (that game input comes from this button of the PC pad; absent = the same button).
// BLACK keeps its own action bindings (input/bindings.cpp).
#pragma once

#include <cstdint>
#include <cstdio>

namespace padkeys {

// The pad inputs, in the order of black.ini and the launcher's Controls page
enum Input {
    kA, kB, kX, kY, kBlack, kWhite, kLT, kRT, kStart, kBack, kLThumb, kRThumb,
    kDUp, kDDown, kDLeft, kDRight,
    kLUp, kLDown, kLLeft, kLRight, kRUp, kRDown, kRLeft, kRRight,
    kInputs
};
constexpr int kButtons = kDUp; // A .. RThumb: the inputs a PC pad button can be remapped to
const char *ini_name(int input);

bool load_ini(const char *key, const char *value); // kb_* and padmap_* lines; false if not one of them
void save_ini(FILE *f);

// Once per pad poll, after the PC pads were read into the Xbox pad (input/xinput.cpp): pc = the PC pads' merged
// state in Xbox layout before remapping; keys apply while the game window has focus.
struct Pad {
    uint16_t buttons;   // Xbox wButtons: D-pad 0x1-0x8, START 0x10, BACK 0x20, thumbs 0x40/0x80
    uint8_t analog[8];  // A B X Y BLACK WHITE LT RT
    int16_t lx, ly, rx, ry;
};
void apply(Pad &pad, bool focus);

} // namespace padkeys
