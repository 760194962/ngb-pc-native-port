// SDL3's gamepad side on its own thread (see sdlpad.h). Only joystick / gamepad code is built (loader/CMakeLists.txt):
// HIDAPI drivers for Sony and Nintendo pads (DualShock 4, DualSense, Switch Pro, Joy-Con pairs, ...), DirectInput for
// the rest (adapters for older pads, DualShock 3 with its driver). The thread polls every 2 ms off the game's CPU, so
// the game thread only copies the merged state.
#include "input/sdlpad.h"
#include "common.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace sdlpad {
namespace {

std::mutex g_mu;
State g_state; // under g_mu
bool g_open;   // under g_mu: at least one SDL pad
std::atomic<uint32_t> g_rumble; // low << 16 | high, from the game thread
std::atomic<bool> g_started;

struct Pad {
    SDL_JoystickID id;
    SDL_Gamepad *pad;
};

// SDL's own XInput driver marks its pads with 'x' in byte 14 of the GUID (SDL_CreateJoystickGUID's driver signature);
// those are read through XInput already (input/xinput.cpp)
bool is_xinput(SDL_JoystickID id) { return SDL_GetJoystickGUIDForID(id).data[14] == 'x'; }

void scan(std::vector<Pad> &pads)
{
    for (size_t i = 0; i < pads.size();) {
        if (SDL_GamepadConnected(pads[i].pad)) {
            i++;
            continue;
        }
        logf("[input] SDL gamepad %u removed", (unsigned)pads[i].id);
        SDL_CloseGamepad(pads[i].pad);
        pads.erase(pads.begin() + i);
    }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        SDL_JoystickID id = ids[i];
        bool known = false;
        for (const Pad &p : pads) known |= p.id == id;
        if (known || is_xinput(id)) continue;
        SDL_Gamepad *pad = SDL_OpenGamepad(id);
        if (!pad) {
            logf("[input] SDL gamepad %u: cannot open (%s)", (unsigned)id, SDL_GetError());
            continue;
        }
        pads.push_back({id, pad});
        const char *name = SDL_GetGamepadName(pad), *type = SDL_GetGamepadStringForType(SDL_GetGamepadType(pad));
        logf("[input] SDL gamepad %u: '%s' (%s, %04x:%04x)%s", (unsigned)id, name ? name : "?", type ? type : "unknown",
             SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad),
             SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? ", rumble" : "");
    }
    SDL_free(ids);
}

int16_t flip(int16_t v) { return v == -32768 ? 32767 : (int16_t)-v; } // SDL's y grows downwards, XInput's upwards

// Keep whichever stick is pushed further (as with several XInput pads)
void merge_stick(int16_t &x, int16_t &y, int16_t px, int16_t py)
{
    if ((int64_t)px * px + (int64_t)py * py > (int64_t)x * x + (int64_t)y * y) x = px, y = py;
}

void read_pad(SDL_Gamepad *p, State &s)
{
    // SDL's face buttons are by position (south = Xbox A = Switch B), so every pad has the Xbox layout
    static const struct { SDL_GamepadButton b; uint16_t x; } kMap[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
        {SDL_GAMEPAD_BUTTON_START, 0x0010}, {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
        {SDL_GAMEPAD_BUTTON_TOUCHPAD, 0x0020}, // DualShock 4 / DualSense: the pad's click is also BACK
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000}, {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
    };
    for (const auto &m : kMap)
        if (SDL_GetGamepadButton(p, m.b)) s.buttons |= m.x;
    auto trigger = [&](SDL_GamepadAxis a) { return (uint8_t)(SDL_clamp(SDL_GetGamepadAxis(p, a), 0, 32767) * 255 / 32767); };
    s.lt = std::max(s.lt, trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    s.rt = std::max(s.rt, trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    merge_stick(s.lx, s.ly, SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), flip(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY)));
    merge_stick(s.rx, s.ry, SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX), flip(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY)));
}

DWORD WINAPI pad_thread(void *)
{
    { // host code: keep it off the game's CPU (as the mixer, audio/dsound.cpp)
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        if (g_game_cpu && (proc & ~g_game_cpu)) SetThreadAffinityMask(GetCurrentThread(), proc & ~g_game_cpu);
    }
    // XInput pads are XInput's (input/xinput.cpp): SDL's XInput driver stays on so DirectInput leaves them alone, its
    // pads are skipped (is_xinput); the other routes to the same pads are off. Pads work with the window in the
    // background, as XInput's do.
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        logf("[input] SDL gamepads not available: %s", SDL_GetError());
        return 0;
    }
    SDL_SetJoystickEventsEnabled(false); // polled: SDL_UpdateGamepads below
    SDL_SetGamepadEventsEnabled(false);
    int v = SDL_GetVersion();
    logf("[input] SDL %d.%d.%d: DualShock, DualSense, Switch Pro, Joy-Con and DirectInput pads", SDL_VERSIONNUM_MAJOR(v),
         SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::vector<Pad> pads;
    ULONGLONG next_scan = 0, rumble_ms = 0;
    uint32_t rumble_sent = 0;
    for (;;) {
        SDL_UpdateGamepads();
        ULONGLONG now = GetTickCount64();
        if (now >= next_scan) {
            scan(pads);
            SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST); // nothing reads the queue
            next_scan = now + 500;
        }
        State s = {};
        for (const Pad &p : pads) read_pad(p.pad, s);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_state = s;
            g_open = !pads.empty();
        }
        // the motors on a change, and again every second (each send runs for 2 s, so a held rumble does not stop)
        uint32_t r = g_rumble.load();
        if (!pads.empty() && (r != rumble_sent || now - rumble_ms > 1000)) {
            for (const Pad &p : pads) SDL_RumbleGamepad(p.pad, (Uint16)(r >> 16), (Uint16)r, r ? 2000 : 0);
            rumble_sent = r;
            rumble_ms = now;
        }
        Sleep(pads.empty() ? 50 : 2);
    }
}

} // namespace

void start()
{
    if (!g_cfg.sdl_pads || g_started.exchange(true)) return;
    if (HANDLE t = CreateThread(nullptr, 0, pad_thread, nullptr, 0, nullptr)) CloseHandle(t);
}

bool read(State &s)
{
    std::lock_guard<std::mutex> lock(g_mu);
    s = g_state;
    return g_open;
}

void rumble(uint16_t low, uint16_t high) { g_rumble = (uint32_t)low << 16 | high; }

} // namespace sdlpad
