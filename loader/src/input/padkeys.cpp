#include "input/padkeys.h"

#include "input/bindings.h"

#include <windows.h>

#include <cstring>
#include <string>

namespace padkeys {

namespace {

const char *const kIni[kInputs] = {"a",     "b",      "x",     "y",      "black", "white", "lt",     "rt",
                                   "start", "back",   "lthumb", "rthumb", "dup",   "ddown", "dleft",  "dright",
                                   "lup",   "ldown",  "lleft", "lright", "rup",   "rdown", "rleft",  "rright"};

// Default keys (the user's layout can replace any of them from the launcher's Controls page)
const uint16_t kDefaults[kInputs][2] = {
    {VK_SPACE, VK_RETURN}, {'L', VK_BACK}, {'J', 0}, {'K', 0}, {'R', 0}, {'E', 0}, {'Q', 0}, {VK_LSHIFT, 0},
    {VK_ESCAPE, 0}, {VK_TAB, 0}, {'C', 0}, {'V', 0},
    {'1', 0}, {'2', 0}, {'3', 0}, {'4', 0},
    {'W', 0}, {'S', 0}, {'A', 0}, {'D', 0}, {VK_UP, 0}, {VK_DOWN, 0}, {VK_LEFT, 0}, {VK_RIGHT, 0}};

uint16_t g_keys[kInputs][2];
int g_map[kButtons]; // the PC pad button each game button comes from (kA..kRThumb); its own by default
bool g_ready;

void ensure_defaults()
{
    if (g_ready) return;
    g_ready = true;
    memcpy(g_keys, kDefaults, sizeof g_keys);
    for (int i = 0; i < kButtons; i++) g_map[i] = i;
}

int input_named(const char *s)
{
    for (int i = 0; i < kInputs; i++)
        if (_stricmp(s, kIni[i]) == 0) return i;
    return -1;
}

// Whether a pad button (kA..kRThumb) is pressed in a pad state
bool button_down(const Pad &p, int b)
{
    static const uint16_t kBits[4] = {0x10, 0x20, 0x40, 0x80}; // START BACK LTHUMB RTHUMB
    return b < kStart ? p.analog[b] > 30 : (p.buttons & kBits[b - kStart]) != 0;
}

uint8_t b_value(const Pad &p, int b) { return b < kStart ? p.analog[b] : 255; } // how far a button is pressed

void press_button(Pad &p, int b, bool down, uint8_t analog = 255)
{
    static const uint16_t kBits[4] = {0x10, 0x20, 0x40, 0x80};
    if (b < kStart) p.analog[b] = down ? analog : 0;
    else if (down) p.buttons |= kBits[b - kStart];
    else p.buttons &= (uint16_t)~kBits[b - kStart];
}

bool held(int input)
{
    for (uint16_t k : g_keys[input])
        if (k && (GetAsyncKeyState(k) & 0x8000)) return true;
    return false;
}

} // namespace

const char *ini_name(int input) { return input >= 0 && input < kInputs ? kIni[input] : ""; }

bool load_ini(const char *key, const char *value)
{
    ensure_defaults();
    if (!strncmp(key, "kb_", 3)) {
        int i = input_named(key + 3);
        if (i < 0) return true; // a kb_ line this build does not know
        std::string v = value;
        size_t comma = v.find(',');
        g_keys[i][0] = bindings::key_from_ini(v.substr(0, comma));
        g_keys[i][1] = comma == std::string::npos ? 0 : bindings::key_from_ini(v.substr(comma + 1));
        return true;
    }
    if (!strncmp(key, "padmap_", 7)) {
        int i = input_named(key + 7), from = input_named(value);
        if (i >= 0 && i < kButtons && from >= 0 && from < kButtons) g_map[i] = from;
        return true;
    }
    return false;
}

void save_ini(FILE *f)
{
    ensure_defaults();
    for (int i = 0; i < kInputs; i++) {
        std::string v = bindings::key_ini(g_keys[i][0]);
        if (g_keys[i][1]) v += "," + bindings::key_ini(g_keys[i][1]);
        fprintf(f, "kb_%s = %s\n", kIni[i], v.c_str());
    }
    for (int i = 0; i < kButtons; i++)
        if (g_map[i] != i) fprintf(f, "padmap_%s = %s\n", kIni[i], kIni[g_map[i]]);
}

void apply(Pad &pad, bool focus)
{
    ensure_defaults();
    // the PC pad's buttons remapped: game button i reads PC button g_map[i]
    Pad pc = pad;
    for (int i = 0; i < kButtons; i++)
        if (g_map[i] != i) press_button(pad, i, button_down(pc, g_map[i]), b_value(pc, g_map[i]));
    // Alt held: a Windows shortcut (Alt+Tab, Alt+Enter), not play: Tab and Enter would press Back and A
    if (!focus || (GetAsyncKeyState(VK_MENU) & 0x8000)) return;
    // keys: a button, trigger or D-pad direction pressed; a stick pushed fully to that side (opposite keys cancel)
    for (int i = 0; i < kButtons; i++)
        if (held(i)) press_button(pad, i, true);
    static const uint16_t kDpad[4] = {0x1, 0x2, 0x4, 0x8}; // up down left right
    for (int i = 0; i < 4; i++)
        if (held(kDUp + i)) pad.buttons |= kDpad[i];
    auto stick = [](int16_t &v, bool neg, bool pos) {
        if (neg != pos) v = pos ? 32767 : -32768;
    };
    stick(pad.lx, held(kLLeft), held(kLRight));
    stick(pad.ly, held(kLDown), held(kLUp));
    stick(pad.rx, held(kRLeft), held(kRRight));
    stick(pad.ry, held(kRDown), held(kRUp));
}

} // namespace padkeys
