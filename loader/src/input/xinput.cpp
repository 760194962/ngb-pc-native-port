// HLE replacement for the Xbox input library (XPP section: USB host controller + XID gamepads).
// Xbox port 0 is fed by every PC XInput controller and the pads SDL3 reads (input/sdlpad.cpp), merged with keyboard
// and mouse while the game window has focus: the mouse rotates the view directly (input/mouselook.cpp; a click in
// the window captures the mouse),
// keys and mouse buttons are bound to the game's actions (input/bindings.cpp, PC SETTINGS > CONTROLS > KEY BINDINGS).
#include "audio/output.h"
#include "build_profile.h"
#include "d3d/dispatch.h"
#include "d3d/xd3d.h"
#include "hooks.h"
#include "kernel/kexports.h"
#include "d3d/device.h"
#include "d3d/draw.h"
#include "d3d/host.h"
#include "d3d/perfhud.h"
#include "input/bindings.h"
#include "input/padkeys.h"
#include "input/mouselook.h"
#include "input/movies.h"
#include "input/sdlpad.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

uint32_t g_pad_watch;

namespace xinput {
namespace {

#pragma pack(push, 1)
struct XGamepad { // Xbox XINPUT_GAMEPAD
    uint16_t wButtons;
    uint8_t bAnalogButtons[8]; // A B X Y BLACK WHITE LTRIGGER RTRIGGER
    int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XState {
    uint32_t dwPacketNumber;
    XGamepad Gamepad;
};
#pragma pack(pop)

// PC XInput (loaded dynamically: xinput1_4.dll ships with Windows 10)
struct PcGamepad {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct PcState {
    DWORD dwPacketNumber;
    PcGamepad Gamepad;
};
struct PcVibration {
    WORD wLeftMotorSpeed, wRightMotorSpeed;
};
typedef DWORD(WINAPI *PFN_GetState)(DWORD, PcState *);
typedef DWORD(WINAPI *PFN_SetState)(DWORD, PcVibration *);
PFN_GetState g_pc_get;
PFN_SetState g_pc_set;

struct Device {
    uint32_t magic;
    uint32_t port;
    uint32_t packet;
    XGamepad last;
};
constexpr uint32_t DEV_MAGIC = 0x44495858; // 'XXID'

bool g_reported_insertion;

// The slow XInput calls run on a host thread, not the game thread: asking an empty slot whether a pad is there
// (Microsoft: do not poll empty slots every frame; how long it takes depends on the PC's HID and Bluetooth
// devices) and setting the motors (~0.4 ms per slot with a pad attached, r119; the game changes the speeds every
// frame while a charge builds). The game thread still reads the attached pads itself each poll, so input is as
// fresh as before; it only skips the slots this thread has not found a pad in.
std::atomic<uint32_t> g_pc_present{0};               // bit i: XInput slot i has a pad
std::atomic<uint32_t> g_pc_motors{0xFFFFFFFFu};      // the game's latest speeds, left << 16 | right; all ones: none yet
HANDLE g_input_wake;                                 // set when the speeds change

uint32_t probe_slots(uint32_t present)
{
    for (DWORD i = 0; i < 4; i++) {
        if (present & (1u << i)) continue;
        PcState pc = {};
        if (g_pc_get(i, &pc) == ERROR_SUCCESS) present |= 1u << i;
    }
    return present;
}

DWORD WINAPI xinput_thread(void *)
{
    { // host code: keep it off the game's CPU (as the SDL pad thread, input/sdlpad.cpp)
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        if (g_game_cpu && (proc & ~g_game_cpu)) SetThreadAffinityMask(GetCurrentThread(), proc & ~g_game_cpu);
    }
    ULONGLONG probed = GetTickCount64();
    uint32_t sent = 0xFFFFFFFFu, sent_to = 0;
    for (;;) {
        // asleep until the speeds change or the next probe is due
        ULONGLONG since = GetTickCount64() - probed;
        WaitForSingleObject(g_input_wake, since >= 2000 ? 0 : (DWORD)(2000 - since));
        ULONGLONG now = GetTickCount64();
        if (now - probed >= 2000) { // a pad plugged in later (the game thread clears a slot that stops answering)
            uint32_t found = probe_slots(g_pc_present.load()) & ~g_pc_present.load();
            if (found) {
                g_pc_present.fetch_or(found);
                sent_to &= ~found; // a pad plugged back in has lost the motors' speeds
                logf("[input] XInput pad found in slot mask 0x%x", found);
            }
            probed = now;
        }
        uint32_t m = g_pc_motors.load(), present = g_pc_present.load();
        if (g_pc_set && m != 0xFFFFFFFFu && (m != sent || (present & ~sent_to))) { // a change, or a pad that has not had it
            PcVibration v = {(WORD)(m >> 16), (WORD)m};
            for (DWORD i = 0; i < 4; i++)
                if (present & (1u << i)) g_pc_set(i, &v); // Xbox port 0 = every PC pad
            sent = m;
            sent_to = present;
        }
    }
}

void load_xinput()
{
    static bool tried;
    if (tried) return;
    tried = true;
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
    if (m) {
        g_pc_get = (PFN_GetState)GetProcAddress(m, "XInputGetState");
        g_pc_set = (PFN_SetState)GetProcAddress(m, "XInputSetState");
    }
    logf("[input] PC XInput %s; keyboard mapping active when the game window has focus", g_pc_get ? "loaded" : "not available");
    if (g_pc_get) {
        g_pc_present = probe_slots(0); // once here, so pads already attached work from the first poll
        g_input_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (HANDLE t = CreateThread(nullptr, 0, xinput_thread, nullptr, 0, nullptr)) CloseHandle(t);
        logf("[input] XInput pads in slot mask 0x%x; empty slots and the motors are handled on a host thread", g_pc_present.load());
    }
    sdlpad::start(); // the pads XInput does not see
}

// --press "frame:button,..." or "SECONDSs:button,...": scripted presses for unattended test runs, held for
// 10 frames, or 300 ms when timed in seconds since the first input poll. "button*MS" holds for MS
// milliseconds; lx/ly/rx/ry+NN or -NN pushes a stick to NN percent (e.g. "80s:rx+25*2000"); mx/my+NN or -NN
// moves the mouse NN counts per poll.
struct Press {
    uint32_t frame;   // or milliseconds when timed
    bool timed;
    std::string button;
    uint32_t hold_ms;
};
std::vector<Press> g_presses;
ULONGLONG g_first_poll_ms;
int g_script_mx, g_script_my; // scripted mouse counts for this poll

void parse_presses()
{
    static bool done;
    if (done) return;
    done = true;
    std::string s = narrow(g_cfg.press_script);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            bool timed = colon > 0 && item[colon - 1] == 's';
            uint32_t at = timed ? (uint32_t)(atof(item.c_str()) * 1000.0) : (uint32_t)atoi(item.c_str());
            std::string b = item.substr(colon + 1);
            size_t star = b.find('*');
            uint32_t hold = 300;
            if (star != std::string::npos) {
                hold = (uint32_t)atoi(b.c_str() + star + 1);
                b.resize(star);
            }
            g_presses.push_back({at, timed, b, hold});
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    if (!g_presses.empty()) logf("[input] %zu scripted presses", g_presses.size());
}

// Tests: show in-game hint N as the game does (FUN_0012B3C0: eax = N, arguments the HUD (*0x2D12E4 + 0xC) and a
// 16-byte vector it keeps at +0x22B0; FUN_0012C110 is the game's own caller). Hints 12, 13, 23, 87 and 24 stop the game
// until A (PRESS A TO CONTINUE).
void show_hint(int id)
{
    uint32_t root = *(const uint32_t *)(uintptr_t)0x002D12E4;
    uint32_t hud = root ? *(const uint32_t *)(uintptr_t)(root + 0xC) : 0;
    if (!hud) {
        logf("[input] tests: no HUD for hint %d", id);
        return;
    }
    alignas(16) float vec[4];
    memcpy(vec, (const void *)(uintptr_t)(hud + 0x22B0), sizeof vec);
    float *v = vec;
    uint32_t fn = 0x0012B3C0;
    logf("[input] tests: showing hint %d", id);
    __asm {
        push v
        push hud
        mov eax, id
        call fn
    }
}

// --press-file: a test driver appends lines while the game runs: a press as in --press without the time ("k:Enter",
// "a", "lx+80*500"), "shot:NAME" for a screenshot of the next whole frame, "reset" (a Direct3D device Reset as after
// sleep), "visquery" (a visibility test: the query a Reset must not find), "recreate" (a new Direct3D device),
// "audio-lost" (the XAudio2 device gone) or
// "hint:N" (show_hint). Presses read together are 250 ms apart.
void read_press_file(uint32_t now_ms)
{
    static ULONGLONG next_check;
    static long long pos;
    static uint32_t queue_end;
    if (g_cfg.press_file.empty() || GetTickCount64() < next_check) return;
    next_check = GetTickCount64() + 50;
    HANDLE h = CreateFileW(g_cfg.press_file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    if (size.QuadPart > pos) {
        std::string buf((size_t)(size.QuadPart - pos), '\0');
        LARGE_INTEGER p;
        p.QuadPart = pos;
        SetFilePointerEx(h, p, nullptr, FILE_BEGIN);
        DWORD got = 0;
        ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr);
        buf.resize(got);
        size_t last = buf.rfind('\n');
        if (last != std::string::npos) {
            pos += (long long)last + 1;
            uint32_t at = std::max(now_ms, queue_end);
            for (size_t s = 0; s <= last;) {
                size_t e = buf.find('\n', s);
                std::string line = buf.substr(s, e - s);
                s = e + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.empty()) continue;
                if (line.rfind("shot:", 0) == 0) {
                    xd3d::request_screenshot(line.substr(5).c_str());
                    continue;
                }
                if (line == "trace" || line.rfind("trace:", 0) == 0) {
                    // --debug-frame at run time: trace the frame after next (or N frames from it with "trace:N"),
                    // with its render-target dumps, so a state the player reached by hand can be examined.
                    uint32_t n = line.size() > 6 ? (uint32_t)atoi(line.c_str() + 6) : 1;
                    g_cfg.debug_frame = g_frame_count + 2;
                    g_cfg.debug_frame_end = n > 1 ? g_cfg.debug_frame + n - 1 : 0;
                    logf("[input] trace requested: frame %u%s", g_cfg.debug_frame, n > 1 ? " (range)" : "");
                    continue;
                }
                if (line.rfind("probe:", 0) == 0) {
                    probe_add((uint32_t)strtoul(line.c_str() + 6, nullptr, 16));
                    continue;
                }
                if (line.rfind("logvar:", 0) == 0) {
                    xd3d::logvar_add((uint32_t)strtoul(line.c_str() + 7, nullptr, 16));
                    continue;
                }
                if (line.rfind("flashvar:", 0) == 0) {
                    xd3d::flash_add_var((uint32_t)strtoul(line.c_str() + 9, nullptr, 16));
                    continue;
                }
                if (line == "flashdump") {
                    xd3d::flash_dump_worst();
                    continue;
                }
                if (line == "waits") {
                    xk::ksync_dump_waits();
                    continue;
                }
                if (line == "coverage") {
                    log_coverage_summary();
                    continue;
                }
                if (line.rfind("spec:", 0) == 0) {
                    // "spec:0" / "spec:1": host specular lighting off / as the game sets it (diagnosis)
                    g_cfg.no_specular = line[5] == '0';
                    xd3d::invalidate_render_state(xd3d::XRS_SPECULARENABLE);
                    logf("[input] specular %s", g_cfg.no_specular ? "forced off" : "as the game sets it");
                    continue;
                }
                if (line.rfind("cfg:", 0) == 0) {
                    // "cfg:aa:0" / "cfg:seamfill:0" / "cfg:fog:0" (and :1): post filters and host fog at run time (diagnosis)
                    size_t c = line.rfind(':');
                    std::string k = line.substr(4, c > 4 ? c - 4 : 0);
                    bool v = c != std::string::npos && line[c + 1] == '1';
                    if (k == "aa") g_cfg.aa = v;
                    else if (k == "seamfill") g_cfg.seam_fill = v;
                    else if (k == "perfhud") g_cfg.perfhud = v; // the performance overlay (black.ini perfhud)
                    else if (k == "aniso") { // anisotropic filtering N (black.ini aniso), sent again with every sampler state
                        g_cfg.aniso = std::clamp(atoi(line.c_str() + c + 1), 1, 16);
                        xd3d::forget_sent_state();
                    }
                    else if (k == "shadow") { // shadow maps at N x their Xbox size (black.ini shadow)
                        g_cfg.shadow_scale = std::clamp(atoi(line.c_str() + c + 1), 1, 16);
                        xd3d::request_render_rebuild();
                    }
                    else if (k == "effectsnative") { // as PC SETTINGS > EFFECTS AT XBOX SIZE
                        g_cfg.effects_native = v;
                        xd3d::request_render_rebuild();
                    }
                    else if (k == "hostffp") g_cfg.host_ffp = !v ? false : true;
                    else if (k == "ffpdebug") g_cfg.ffp_debug = (unsigned)atoi(line.c_str() + c + 1);
                    else if (k == "zmode") g_cfg.zmode = atoi(line.c_str() + c + 1);
                    else if (k == "texcheck") g_cfg.tex_every_frame = v;
                    else if (k == "texperiodic") g_cfg.tex_periodic = v;
                    else if (k == "ring") g_cfg.draw_ring = v;
                    else if (k == "zview8") g_cfg.zview8 = v;
                    else if (k == "rhwoffset") g_cfg.rhw_offset = v;
                    else if (k == "rhwedge") g_cfg.rhw_edge_fix = v;
                    else if (k == "quadsnap") g_cfg.quad_snap = v;
                    else if (k == "quadweight") g_cfg.quad_weight = v;
                    else if (k == "quadgrad") g_cfg.quad_grad = v;
                    else if (k == "hudcentre") g_cfg.hud_centre = v;
                    else if (k == "resolve") g_cfg.resolve = v;
                    else if (k == "shadowpullc") g_cfg.shadow_pull_bias = atoi(line.c_str() + c + 1);
                    else if (k == "shadowpulls") g_cfg.shadow_pull_slope = atoi(line.c_str() + c + 1);
                    else if (k == "shadowpcf") g_cfg.shadow_pcf = std::clamp(atoi(line.c_str() + c + 1), 0, 2);
                    else if (k == "hrtf") g_cfg.hrtf = v;
                    else if (k == "rhwtexmat") g_cfg.rhw_tex_matrix = v;
                    else if (k == "vsnap") g_cfg.vs_snap = v;
                    else if (k == "shadowpull") g_cfg.shadow_pull = atoi(line.c_str() + c + 1); // N times the pull (0 off)
                    else if (k == "edgemirror") g_cfg.edge_mirror = v;
                    else if (k == "vsdivide") g_cfg.vs_divide_fix = v;
                    else if (k == "dumpvs") g_cfg.dump_vs_texmodes = (uint32_t)strtoul(line.c_str() + c + 1, nullptr, 16);
                    else if (k == "skipcaller") g_cfg.skip_caller = (uint32_t)strtoul(line.c_str() + c + 1, nullptr, 16);
                    else if (k == "pixwatch") {
                        if (sscanf(line.c_str() + c + 1, "%d,%d", &g_cfg.pix_watch_x, &g_cfg.pix_watch_y) != 2)
                            g_cfg.pix_watch_x = g_cfg.pix_watch_y = -1;
                    }
                    else if (k == "localviewer") {
                        int lv = atoi(line.c_str() + c + 1);
                        g_cfg.force_localviewer = lv >= 0 && lv <= 2 ? lv : -1; // other values: the NV2A rule
                    }
                    else if (k == "zoffset") g_cfg.zoffset_mode = std::clamp(atoi(line.c_str() + c + 1), 0, 2);
                    else if (k == "zprobe") g_cfg.z_probe = atoi(line.c_str() + c + 1);
                    else if (k == "pixwatchrt") g_cfg.pix_watch_rt = (uint32_t)strtoul(line.c_str() + c + 1, nullptr, 16);
                    else if (k == "skiptexmodes") g_cfg.skip_texmodes = (uint32_t)strtoul(line.c_str() + c + 1, nullptr, 16);
                    else if (k == "visall") g_cfg.vis_all = v;
                    else if (k == "flashwatch") g_cfg.flash_watch = v;
                    else if (k == "untex") g_cfg.untex = atoi(line.c_str() + c + 1);
                    else if (k == "tscdiv") g_cfg.tsc_divisor = std::max(1, atoi(line.c_str() + c + 1));
                    else if (k == "fog") {
                        g_cfg.no_fog = !v;
                        xd3d::invalidate_render_state(xd3d::XRS_FOGENABLE);
                    }
                    logf("[input] cfg %s = %s", k.c_str(), line.c_str() + c + 1);
                    continue;
                }
                if (line.rfind("memdump:", 0) == 0) {
                    // --memdump at run time: "memdump:ADDR:SIZE" (hex, virtual address), written at the frame after
                    // next as mem_<frame>.bin in the screenshot directory.
                    char spec[96];
                    snprintf(spec, sizeof spec, "%s:%u", line.c_str() + 8, g_frame_count + 2);
                    g_cfg.memdump = widen(spec);
                    logf("[input] memdump requested: %s", spec);
                    continue;
                }
                if (line == "reset") {
                    xd3d::request_device_reset("tests: press-file reset");
                    continue;
                }
                if (line == "visquery") {
                    xd3d::request_visibility_test();
                    continue;
                }
                if (line == "recreate") {
                    xd3d::request_device_recreate();
                    continue;
                }
                if (line == "audio-lost") {
                    audio_out::simulate_lost();
                    continue;
                }
                if (line.rfind("hint:", 0) == 0) {
                    show_hint(atoi(line.c_str() + 5));
                    continue;
                }
                uint32_t hold = 150;
                size_t star = line.find('*');
                if (star != std::string::npos) {
                    hold = (uint32_t)atoi(line.c_str() + star + 1);
                    line.resize(star);
                }
                g_presses.push_back({at, true, line, hold});
                logf("[input] press-file '%s' at %u ms", line.c_str(), at);
                at += std::max(250u, hold + 100);
            }
            queue_end = at;
        }
    }
    CloseHandle(h);
}

void apply_presses(XGamepad &g)
{
    parse_presses();
    g_script_mx = g_script_my = 0;
    uint32_t f = g_frame_count;
    if (!g_first_poll_ms) g_first_poll_ms = GetTickCount64();
    uint32_t ms = (uint32_t)(GetTickCount64() - g_first_poll_ms);
    read_press_file(ms);
    for (const Press &p : g_presses) {
        if (p.timed ? (ms < p.frame || ms >= p.frame + p.hold_ms) : (f < p.frame || f >= p.frame + 10)) continue;
        const std::string &b = p.button;
        if (b.size() > 3 && (b[0] == 'l' || b[0] == 'r') && (b[1] == 'x' || b[1] == 'y') && (b[2] == '+' || b[2] == '-')) {
            int16_t v = (int16_t)std::clamp(atoi(b.c_str() + 2) * 32767 / 100, -32767, 32767);
            int16_t &axis = b[0] == 'l' ? (b[1] == 'x' ? g.sThumbLX : g.sThumbLY) : (b[1] == 'x' ? g.sThumbRX : g.sThumbRY);
            axis = v;
        }
        if (b.size() > 3 && b[0] == 'm' && (b[1] == 'x' || b[1] == 'y') && (b[2] == '+' || b[2] == '-'))
            (b[1] == 'x' ? g_script_mx : g_script_my) += atoi(b.c_str() + 2);
        if (b.size() > 2 && b[0] == 'k' && b[1] == ':') bindings::script_hold(b.substr(2)); // a bound key
        if (b == "up") g.wButtons |= 0x01;
        else if (b == "down") g.wButtons |= 0x02;
        else if (b == "left") g.wButtons |= 0x04;
        else if (b == "right") g.wButtons |= 0x08;
        else if (b == "start") g.wButtons |= 0x10;
        else if (b == "back") g.wButtons |= 0x20;
        else if (b == "a") g.bAnalogButtons[0] = 255;
        else if (b == "b") g.bAnalogButtons[1] = 255;
        else if (b == "x") g.bAnalogButtons[2] = 255;
        else if (b == "y") g.bAnalogButtons[3] = 255;
        else if (b == "black") g.bAnalogButtons[4] = 255;
        else if (b == "white") g.bAnalogButtons[5] = 255;
        else if (b == "lt") g.bAnalogButtons[6] = 255;
        else if (b == "rt") g.bAnalogButtons[7] = 255;
        else if (b == "lthumb") g.wButtons |= 0x40;
        else if (b == "rthumb") g.wButtons |= 0x80;
        static uint32_t logged_frame;
        if (logged_frame != p.frame) {
            logged_frame = p.frame;
            logf("[input] scripted press '%s' at frame %u", b.c_str(), f);
        }
    }
}

uint8_t max8(uint8_t a, uint8_t b) { return a > b ? a : b; }

// Keep whichever stick is pushed further.
void merge_stick(int16_t &x, int16_t &y, int16_t px, int16_t py)
{
    if ((int64_t)px * px + (int64_t)py * py > (int64_t)x * x + (int64_t)y * y) {
        x = px;
        y = py;
    }
}

// Mouse: movement since the previous poll (the game polls once per frame) becomes this frame's rotation
// (input/mouselook.cpp); buttons and the wheel are bindable keys (input/bindings.cpp). Returns the wheel notches.
int mouse_look(bool live, int *raw_wheel)
{
    int dx, dy, wheel;
    host3d::mouse_take(&dx, &dy, &wheel);
    *raw_wheel = wheel; // the key bindings page takes the wheel without the mouse captured
    if (!live) dx = dy = wheel = 0;
    dx += g_script_mx; // scripted moves work without focus
    dy += g_script_my;
    float k = 0.022f * g_cfg.mouse_sens; // degrees per count
    // The game's yaw rate is positive to the left and its pitch rate positive downwards (mouse dy > 0 = towards
    // the user = look down).
    mouselook_set_frame(-dx * k, (g_cfg.mouse_invert ? -dy : dy) * k);
    return wheel;
}

// The game plays on Xbox port 0; every connected PC controller (any XInput user index) feeds it, so a
// pad that Windows put on index 1-3 works without setup.
void read_pad(uint32_t port, XGamepad &g)
{
    memset(&g, 0, sizeof g);
    if (port != 0) return;
    static XGamepad last;
    static bool have_last;
    auto note_input = [&]() {
        bool changed = !have_last || g.wButtons != last.wButtons || memcmp(g.bAnalogButtons, last.bAnalogButtons, 8) != 0 ||
                       std::abs((int)g.sThumbLX - last.sThumbLX) > 384 || std::abs((int)g.sThumbLY - last.sThumbLY) > 384 ||
                       std::abs((int)g.sThumbRX - last.sThumbRX) > 384 || std::abs((int)g.sThumbRY - last.sThumbRY) > 384;
        if (changed) perfhud::input_changed();
        last = g;
        have_last = true;
    };
    const uint32_t present = g_pc_present.load(); // empty slots are probed by xinput_thread, not here
    for (DWORD i = 0; g_pc_get && i < 4; i++) {
        if (!(present & (1u << i))) continue;
        PcState pc = {};
        if (g_pc_get(i, &pc) != ERROR_SUCCESS) {
            g_pc_present.fetch_and(~(1u << i)); // unplugged: back to the host thread's probing
            continue;
        }
        const PcGamepad &p = pc.Gamepad;
        g.wButtons |= p.wButtons & 0x00FF; // D-pad, START, BACK, thumb clicks map 1:1
        if (p.wButtons & 0x1000) g.bAnalogButtons[0] = 255; // A
        if (p.wButtons & 0x2000) g.bAnalogButtons[1] = 255; // B
        if (p.wButtons & 0x4000) g.bAnalogButtons[2] = 255; // X
        if (p.wButtons & 0x8000) g.bAnalogButtons[3] = 255; // Y
        if (p.wButtons & 0x0200) g.bAnalogButtons[4] = 255; // BLACK = right shoulder
        if (p.wButtons & 0x0100) g.bAnalogButtons[5] = 255; // WHITE = left shoulder
        g.bAnalogButtons[6] = max8(g.bAnalogButtons[6], p.bLeftTrigger);
        g.bAnalogButtons[7] = max8(g.bAnalogButtons[7], p.bRightTrigger);
        merge_stick(g.sThumbLX, g.sThumbLY, p.sThumbLX, p.sThumbLY);
        merge_stick(g.sThumbRX, g.sThumbRY, p.sThumbRX, p.sThumbRY);
    }
    sdlpad::State s;
    if (sdlpad::read(s)) { // DualShock, DualSense, Switch, DirectInput pads: already in XInput layout
        g.wButtons |= s.buttons & 0x00FF;
        for (int b = 0; b < 4; b++)
            if (s.buttons & (0x1000 << b)) g.bAnalogButtons[b] = 255; // A B X Y
        if (s.buttons & 0x0200) g.bAnalogButtons[4] = 255;            // BLACK = right shoulder
        if (s.buttons & 0x0100) g.bAnalogButtons[5] = 255;            // WHITE = left shoulder
        g.bAnalogButtons[6] = max8(g.bAnalogButtons[6], s.lt);
        g.bAnalogButtons[7] = max8(g.bAnalogButtons[7], s.rt);
        merge_stick(g.sThumbLX, g.sThumbLY, s.lx, s.ly);
        merge_stick(g.sThumbRX, g.sThumbRY, s.rx, s.ry);
    }
    apply_presses(g);
    // Mouse look, the key bindings and the movie skip read BLACK's game objects (binding table 0x2F3BD4, the
    // movie player at *0x2D45F0): BLACK only. Ninja Gaiden Black and its bundled programs: keys pressed into the pad
    // and the PC pad's buttons remapped (input/padkeys.cpp).
    if (!g_cfg.profile || !g_cfg.profile->black) {
        padkeys::Pad p = {g.wButtons, {}, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY};
        memcpy(p.analog, g.bAnalogButtons, 8);
        padkeys::apply(p, GetForegroundWindow() == host3d::window());
        g.wButtons = p.buttons;
        memcpy(g.bAnalogButtons, p.analog, 8);
        g.sThumbLX = p.lx, g.sThumbLY = p.ly, g.sThumbRX = p.rx, g.sThumbRY = p.ry;
        note_input();
        return;
    }
    bool focus = GetForegroundWindow() == host3d::window();
    bool live = focus && host3d::mouse_captured();
    int raw_wheel;
    int wheel = mouse_look(live, &raw_wheel);
    if (!bindings::capture_poll(focus, raw_wheel)) // while the key bindings page waits for a key, keys bind instead
        bindings::apply(g.wButtons, g.bAnalogButtons, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY, focus, live, wheel);
    movies_poll(g.wButtons, g.bAnalogButtons, bindings::menu_select_held());
    note_input();
}

} // namespace

void __stdcall XInitDevices(uint32_t dwPreallocTypeCount, void *PreallocTypes)
{
    DTRACE("%u, %p", dwPreallocTypeCount, PreallocTypes);
    load_xinput();
}

// Port bitmasks: bit n = port n (gamepads). One gamepad on port 0, reported once as an insertion.
BOOL __stdcall XGetDeviceChanges(void *DeviceType, uint32_t *pdwInsertions, uint32_t *pdwRemovals)
{
    DTRACE("%p", DeviceType);
    *pdwInsertions = 0;
    *pdwRemovals = 0;
    bool gamepad = g_cfg.profile && DeviceType == (void *)(uintptr_t)g_cfg.profile->xdevice_type_gamepad; // XDEVICE_TYPE_GAMEPAD
    if (gamepad && !g_reported_insertion) {
        g_reported_insertion = true;
        *pdwInsertions = 1;
        return TRUE;
    }
    return FALSE;
}

// Ports with a device of this type right now: port 0 for gamepads. It does not consume the insertion
// notification: the game asks XGetDevices first and XGetDeviceChanges after, and only the latter lets it open the
// pad (genma-pc's finding; Ninja Gaiden Black calls both).
uint32_t __stdcall XGetDevices(void *DeviceType)
{
    DTRACE("%p", DeviceType);
    bool gamepad = g_cfg.profile && DeviceType == (void *)(uintptr_t)g_cfg.profile->xdevice_type_gamepad;
    return gamepad ? 1u : 0u;
}

// Ninja Gaiden Black 0x3DA598 (unnamed; XONLINE asks it for memory units, where Live accounts can also be kept):
// the ports with a device of the type, and two more masks the caller may ask for. Gamepad: port 0; others: none.
uint32_t __stdcall XGetDevicesWithMasks(void *DeviceType, uint32_t *pMask1, uint32_t *pMask2)
{
    DTRACE("%p, %p, %p", DeviceType, pMask1, pMask2);
    uint32_t ports = XGetDevices(DeviceType);
    if (pMask1) *pMask1 = ports;
    if (pMask2) *pMask2 = ports;
    return ports;
}

uint32_t __stdcall XGetDeviceEnumerationStatus()
{
    DTRACE("");
    return 0; // XDEVICE_ENUMERATION_IDLE
}

// XINPUT_CAPABILITIES: {BYTE SubType; WORD Reserved; XINPUT_GAMEPAD In; XINPUT_RUMBLE Out}: In/Out hold the
// supported fields with every bit set; SubType 1 = XINPUT_DEVSUBTYPE_GC_GAMEPAD (the Controller S).
uint32_t __stdcall XInputGetCapabilities(Device *h, uint8_t *pCapabilities)
{
    DTRACE("%p, %p", h, pCapabilities);
    if (!h || h->magic != DEV_MAGIC) return ERROR_DEVICE_NOT_CONNECTED;
    memset(pCapabilities, 0, 4 + sizeof(XGamepad) + 4);
    pCapabilities[0] = 1;
    memset(pCapabilities + 4, 0xFF, sizeof(XGamepad)); // packed after SubType + Reserved (4 bytes)
    memset(pCapabilities + 4 + sizeof(XGamepad), 0xFF, 4);
    return ERROR_SUCCESS;
}

HANDLE __stdcall XInputOpen(void *DeviceType, uint32_t dwPort, uint32_t dwSlot, void *pPollingParameters)
{
    auto *d = new Device();
    d->magic = DEV_MAGIC;
    d->port = dwPort;
    logf("[input] XInputOpen(type %p, port %u, slot %u) -> %p", DeviceType, dwPort, dwSlot, d);
    return d;
}

void __stdcall XInputClose(Device *h)
{
    DTRACE("%p", h);
    if (h && h->magic == DEV_MAGIC) h->magic = 0;
}

// XInputPoll: titles that turn auto-polling off ask for a fresh state with it (the SNES programs bundled with Ninja
// Gaiden Black); XInputGetState here always reads the pad as it is now, so there is nothing to do.
uint32_t __stdcall XInputPoll(Device *h)
{
    if (!h || h->magic != DEV_MAGIC) return ERROR_DEVICE_NOT_CONNECTED;
    return ERROR_SUCCESS;
}

uint32_t __stdcall XInputGetState(Device *h, XState *pState)
{
    if (!h || h->magic != DEV_MAGIC) return ERROR_DEVICE_NOT_CONNECTED;
    if (g_cfg.watch_pad && !g_pad_watch && g_frame_count >= g_cfg.watch_pad) {
        // Debug: hardware read/write breakpoint (DR3, 2 bytes) on this thread over sThumbRX of the game's buffer.
        uint32_t base = g_cfg.watch_read_deref ? *(uint32_t *)(uintptr_t)g_cfg.watch_read : g_cfg.watch_read;
        g_pad_watch = g_cfg.watch_read ? base + g_cfg.watch_read_off : (uint32_t)(uintptr_t)&pState->Gamepad.sThumbRX;
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        GetThreadContext(GetCurrentThread(), &c);
        c.Dr3 = g_pad_watch;
        uint32_t len = g_cfg.watch_read ? 0x3u : 0x1u; // LEN3: 4 bytes for --watch-read, else 2
        uint32_t rw = g_cfg.watch_write_only ? 0x1u : 0x3u;
        c.Dr7 = (c.Dr7 & ~(0xFu << 28)) | (1u << 6) | (rw << 28) | (len << 30); // L3, RW3 = write or read/write
        BOOL ok = SetThreadContext(GetCurrentThread(), &c);
        logf("[input] watching right-stick reads at %08x (%s)", g_pad_watch, ok ? "set" : "FAILED");
    }
    static std::atomic<uint32_t> polls{0};
    if ((++polls % 600) == 1) logf("[input] XInputGetState poll #%u (frame %u)", polls.load(), g_frame_count.load());
    XGamepad g;
    read_pad(h->port, g);
    if (memcmp(&g, &h->last, sizeof g)) {
        h->last = g;
        h->packet++;
    }
    pState->dwPacketNumber = h->packet;
    pState->Gamepad = g;
    return ERROR_SUCCESS;
}

// XINPUT_FEEDBACK: header {dwStatus, hEvent, Reserved[58]} packed to 66 bytes, then rumble {wLeftMotorSpeed,
// wRightMotorSpeed} at +66 / +68: XAPI's XInputSetState (x2.1 0x3DBBDE) puts the HID report id and length in the
// last two Reserved bytes (+0x40, +0x41) so the motors follow as the report body, and the game's per-frame sender
// (0x30DE40) keeps the feedback at esi-0x4D with the motors at esi-0xB / esi-9 (+0x42 / +0x44).
uint32_t __stdcall XInputSetState(Device *h, uint8_t *pFeedback)
{
    if (!h || h->magic != DEV_MAGIC) return ERROR_DEVICE_NOT_CONNECTED;
    PcVibration v = {*(uint16_t *)(pFeedback + 66), *(uint16_t *)(pFeedback + 68)};
    // The game sets the motors every frame; the host call costs ~0.4 ms per pad slot (11% of the frame time in
    // chapter 1, r119 perf), so a change only wakes xinput_thread, which sends it to the attached pads.
    if (h->port == 0) {
        uint32_t m = (uint32_t)v.wLeftMotorSpeed << 16 | v.wRightMotorSpeed;
        if (g_pc_motors.exchange(m) != m) {
            if (g_input_wake) SetEvent(g_input_wake);
            sdlpad::rumble(v.wLeftMotorSpeed, v.wRightMotorSpeed);
            static int logged; // the first changes, as evidence in the log
            if (logged < 40) {
                logged++;
                logf("[input] rumble left %u right %u (frame %u)", v.wLeftMotorSpeed, v.wRightMotorSpeed, g_frame_count.load());
            }
        }
    }
    *(uint32_t *)pFeedback = ERROR_SUCCESS; // completed immediately
    if (HANDLE ev = *(HANDLE *)(pFeedback + 4)) SetEvent(ev);
    return ERROR_IO_PENDING;
}

#define H(addr, fn, params, ret) {addr, #fn, (void *)&fn, params, ret}

// BLACK NTSC-U (addresses from black-pc's notes/functions.csv).
static const xd3d::Hle kTable[] = {
    H(0x0027dd24, XInitDevices, "s s", false),
    H(0x0027dd29, XGetDeviceChanges, "s s s", true),
    H(0x0027dd96, XInputOpen, "s s s s", true),
    H(0x0027ddec, XInputClose, "s", false),
    H(0x0027ddf8, XInputGetState, "s s", true),
    H(0x0027de6b, XInputSetState, "s s", true),
    H(0x00000000, XInputPoll, "s", true), // not called by BLACK (bundled programs)
};

// NINJA GAIDEN BLACK x2.1: the entry points its code calls (docs/02 §2; 0x3DA5D4 is XAPI's `jmp XInitDevices`
// thunk, the body 0x3D996F is what XbSymbolDatabase names). All stdcall with stack parameters (XbSymbolDatabase).
static const xd3d::Hle kNgbTable[] = {
    H(0x003da5d4, XInitDevices, "s s", false),
    H(0x003da5d9, XGetDevices, "s", true),
    H(0x003da598, XGetDevicesWithMasks, "s s s", true),
    H(0x003da5fb, XGetDeviceChanges, "s s s", true),
    H(0x003dad86, XGetDeviceEnumerationStatus, "", true),
    H(0x003db931, XInputOpen, "s s s s", true),
    H(0x003db987, XInputClose, "s", false),
    H(0x003db993, XInputGetCapabilities, "s s", true),
    H(0x003dbb6b, XInputGetState, "s s", true),
    H(0x003dbbde, XInputSetState, "s s", true),
};

using Hle = xd3d::Hle;
#include "auxtab/input_tables.inc"
// The first pad as an Xbox XINPUT_GAMEPAD (18 bytes: buttons, 8 analog buttons, 4 stick axes): the USB controller
// model's gamepad reports (usb_ohci.cpp)
void pad_report(uint8_t out[18])
{
    // The USB model's controller (nxdk programs: the Insignia Setup Assistant) is read without XInitDevices, which loads
    // the PC pads for the games: only scripted presses reached it (the user's pad did nothing, r969)
    load_xinput();
    XGamepad g;
    read_pad(0, g);
    memcpy(out, &g, 18);
}

} // namespace xinput

namespace xd3d {
const Hle *hle_input_table_aux(const char *xbe, size_t *count) { return xinput::hle_input_table_aux(xbe, count); }
const Hle *hle_input_table(size_t *count)
{
    *count = sizeof xinput::kTable / sizeof xinput::kTable[0];
    return xinput::kTable;
}
const Hle *hle_input_table_ngb(size_t *count)
{
    *count = sizeof xinput::kNgbTable / sizeof xinput::kNgbTable[0];
    return xinput::kNgbTable;
}
} // namespace xd3d
