#include "keys.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace keys {

namespace {

const char *const kIni[kInputs] = {"a",     "b",     "x",      "y",      "black", "white", "lt",    "rt",
                                   "start", "back",  "lthumb", "rthumb", "dup",   "ddown", "dleft", "dright",
                                   "lup",   "ldown", "lleft",  "lright", "rup",   "rdown", "rleft", "rright"};

// = loader input/padkeys.cpp kDefaults
const uint16_t kDefaults[kInputs][2] = {
    {'L', 0}, {'I', 0}, {'J', 0}, {'K', 0}, {'Z', 0}, {'X', 0}, {VK_SPACE, 0}, {'Q', 0},
    {'B', 0}, {'V', 0}, {'C', 0}, {'N', 0},
    {'T', 0}, {'G', 0}, {'F', 0}, {'H', 0},
    {'W', 0}, {'S', 0}, {'A', 0}, {'D', 0}, {VK_UP, 0}, {VK_DOWN, 0}, {VK_LEFT, 0}, {VK_RIGHT, 0}};

struct Named {
    uint16_t key;
    const char *name;
};
// = loader input/bindings.cpp kNames (keyboard part)
const Named kNames[] = {
    {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Esc"}, {VK_TAB, "Tab"}, {VK_BACK, "Backspace"},
    {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"}, {VK_LSHIFT, "LShift"}, {VK_RSHIFT, "RShift"},
    {VK_LCONTROL, "LCtrl"}, {VK_RCONTROL, "RCtrl"}, {VK_LMENU, "LAlt"}, {VK_RMENU, "RAlt"}, {VK_CAPITAL, "CapsLock"},
    {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"}, {VK_INSERT, "Insert"},
    {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"}, {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
    {VK_MULTIPLY, "Num*"}, {VK_ADD, "Num+"}, {VK_SUBTRACT, "Num-"}, {VK_DECIMAL, "Num."}, {VK_DIVIDE, "Num/"},
    {VK_PAUSE, "Pause"}, {VK_SCROLL, "ScrollLock"}, {VK_NUMLOCK, "NumLock"}, {VK_APPS, "Menu"},
};

} // namespace

const char *input_ini(int input) { return input >= 0 && input < kInputs ? kIni[input] : ""; }
uint16_t default_key(int input, int slot) { return input >= 0 && input < kInputs && slot >= 0 && slot < 2 ? kDefaults[input][slot] : 0; }

std::string ini_name(uint16_t k)
{
    if (!k) return "";
    for (const Named &n : kNames)
        if (n.key == k) return n.name;
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')) return std::string(1, (char)k);
    if (k >= VK_F1 && k <= VK_F24) return "F" + std::to_string(k - VK_F1 + 1);
    if (k >= VK_NUMPAD0 && k <= VK_NUMPAD9) return "Num" + std::to_string(k - VK_NUMPAD0);
    char buf[8];
    snprintf(buf, sizeof buf, "0x%02X", k);
    return buf;
}

uint16_t parse(const std::string &s)
{
    if (s.empty() || _stricmp(s.c_str(), "none") == 0) return 0;
    for (const Named &n : kNames)
        if (_stricmp(n.name, s.c_str()) == 0) return n.key;
    if (s.size() == 1 && isalnum((unsigned char)s[0])) return (uint16_t)toupper((unsigned char)s[0]);
    if ((s[0] == 'F' || s[0] == 'f') && s.size() <= 3 && isdigit((unsigned char)s[1])) {
        int n = atoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) return (uint16_t)(VK_F1 + n - 1);
    }
    if (_strnicmp(s.c_str(), "Num", 3) == 0 && s.size() == 4 && isdigit((unsigned char)s[3])) return (uint16_t)(VK_NUMPAD0 + s[3] - '0');
    if (_strnicmp(s.c_str(), "0x", 2) == 0) return (uint16_t)strtoul(s.c_str(), nullptr, 16);
    return 0;
}

std::wstring display(uint16_t k)
{
    if (!k) return L"";
    bool oem = (k >= 0xBA && k <= 0xC0) || (k >= 0xDB && k <= 0xDF) || k == 0xE2;
    if (oem) { // the character printed on the key in the current layout
        wchar_t buf[32];
        UINT sc = MapVirtualKeyW(k, MAPVK_VK_TO_VSC);
        if (sc && GetKeyNameTextW((LONG)(sc << 16), buf, 32) > 0) return buf;
    }
    std::string n = ini_name(k);
    return std::wstring(n.begin(), n.end());
}

uint16_t from_message(WPARAM vk, LPARAM lp)
{
    UINT sc = (lp >> 16) & 0xFF;
    bool ext = (lp >> 24) & 1;
    if (vk == VK_SHIFT) return (uint16_t)MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);
    if (vk == VK_CONTROL) return ext ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return ext ? VK_RMENU : VK_LMENU;
    return (uint16_t)vk;
}

} // namespace keys
