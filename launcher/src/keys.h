#pragma once
// Keys for the Controls page: the same black.ini spelling as the game program (loader input/bindings.cpp ini_name /
// parse_key) and the same default layout (loader input/padkeys.cpp kDefaults), so what the page writes the game reads.
#include <windows.h>

#include <cstdint>
#include <string>

namespace keys {

constexpr int kInputs = 24, kButtons = 12; // the pad inputs (padkeys::Input order) and the remappable buttons among them
const char *input_ini(int input);           // "a", "b", ... "rright": kb_<this>, padmap_<this>
uint16_t default_key(int input, int slot);
std::string ini_name(uint16_t vk);          // "Space", "J", "LShift", "F5", "0xBA"; "" for none
uint16_t parse(const std::string &s);
std::wstring display(uint16_t vk);          // what the key box shows
uint16_t from_message(WPARAM vk, LPARAM lp); // a WM_KEYDOWN / WM_SYSKEYDOWN key, Shift / Ctrl / Alt as left or right

} // namespace keys
