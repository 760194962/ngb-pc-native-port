#include "input/mouse.h"

#include "common.h"

#include <atomic>

namespace mouse {

namespace {

constexpr UINT WM_APP_MOUSE = WM_APP + 0x52; // wParam: kLetGo / kPlayResumed (from the game's input poll)
enum { kLetGo, kPlayResumed };

// Window thread only
HWND g_hwnd;
bool g_started;  // the window has been in front once: the start's hold is done
bool g_let_go;   // Esc let go of the mouse: play starting again takes it back
int g_ignore;    // buttons down when the game took the mouse: not the game's until released

// Read by the game's thread
std::atomic<bool> g_held{false};
std::atomic<int> g_dx, g_dy, g_wheel, g_buttons;

void clip()
{
    RECT r;
    GetClientRect(g_hwnd, &r);
    MapWindowPoints(g_hwnd, nullptr, (POINT *)&r, 2);
    ClipCursor(&r);
}

int buttons_down_now()
{
    static const int vk[5] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    int b = 0;
    for (int i = 0; i < 5; i++)
        if (GetAsyncKeyState(vk[i]) & 0x8000) b |= 1 << i;
    return b;
}

void hold(bool on, const char *why)
{
    if (on && (GetForegroundWindow() != g_hwnd || IsIconic(g_hwnd))) return; // only a window in front holds it
    if (on == g_held) return;
    g_held = on;
    g_dx = g_dy = g_buttons = 0;
    if (on) {
        g_let_go = false;
        g_ignore = buttons_down_now();
        clip();
        SetCursor(nullptr);
    } else {
        ClipCursor(nullptr);
        // Let go in front (Esc): the cursor shows now, not at the next move (borderless stayed blank until then).
        // Out of the front it is over another program, which sets its own.
        if (GetForegroundWindow() == g_hwnd) SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW));
    }
    logf("[mouse] %s (%s)", on ? "held" : "free", why);
}

void raw_input(HRAWINPUT h)
{
    alignas(8) BYTE buf[sizeof(RAWINPUT) + 16];
    UINT size = sizeof buf;
    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    const RAWINPUT *ri = (const RAWINPUT *)buf;
    if (ri->header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE &m = ri->data.mouse;
    const USHORT f = m.usButtonFlags;
    // The wheel counts whenever the window is in front (raw input only comes then): the key bindings page binds it
    // with the mouse let go too (it was dropped then, and binding the wheel needed the mouse held).
    if (f & RI_MOUSE_WHEEL) { // notches (smooth-scrolling wheels send fractions: count each burst as one)
        const SHORT d = (SHORT)m.usButtonData;
        g_wheel += d / WHEEL_DELTA ? d / WHEEL_DELTA : (d > 0 ? 1 : -1);
    }
    if (!g_held) return;
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        g_dx += m.lLastX;
        g_dy += m.lLastY;
    }
    int b = g_buttons;
    for (int i = 0; i < 5; i++) { // left, right, middle, X1, X2: down/up flag pairs
        if (f & (1 << (2 * i))) b |= 1 << i;
        if (f & (2 << (2 * i))) b &= ~(1 << i), g_ignore &= ~(1 << i);
    }
    g_buttons = b & ~g_ignore;
}

} // namespace

void attach(HWND hwnd)
{
    g_hwnd = hwnd;
    RAWINPUTDEVICE d = {0x01, 0x02, 0, hwnd}; // generic desktop / mouse, while the window is in front
    if (!RegisterRawInputDevices(&d, 1, sizeof d)) logf("[mouse] raw mouse input not available (%lu)", GetLastError());
}

bool on_message(HWND, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    switch (msg) {
    case WM_INPUT:
        raw_input((HRAWINPUT)lp);
        return false; // DefWindowProc cleans up after WM_INPUT
    case WM_SETFOCUS: // the window in front for the first time: the game starts holding the mouse
        if (!g_started && GetForegroundWindow() == g_hwnd) {
            g_started = true;
            // Windows did not give it the front at the start and a click brought it there: on the title bar it would
            // pull the cursor into the picture in the middle of a move, so a click in the picture takes it as before
            // (GetAsyncKeyState reads the physical buttons: the primary one is the right with the buttons swapped)
            const int primary = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
            if (!(GetAsyncKeyState(primary) & 0x8000)) hold(true, "the game started");
        }
        return false;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) hold(false, "the window left the front");
        return false;
    case WM_KILLFOCUS:
        hold(false, "the window lost the keyboard");
        return false;
    case WM_SIZE:
    case WM_MOVE:
        if (g_held) clip(); // the clip follows the picture
        return false;
    case WM_LBUTTONDOWN: // a click in the picture takes the mouse; that click is not the game's (g_ignore)
        hold(true, "a click in the window");
        *result = 0;
        return true;
    case WM_SETCURSOR:
        if (g_held && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            *result = TRUE;
            return true;
        }
        return false;
    case WM_APP_MOUSE:
        if (wp == kLetGo && g_held) {
            hold(false, "Esc");
            g_let_go = true;
        } else if (wp == kPlayResumed && g_let_go) {
            hold(true, "play resumed");
        }
        *result = 0;
        return true;
    }
    return false;
}

bool held() { return g_held; }

void take(int *dx, int *dy, int *wheel)
{
    *dx = g_dx.exchange(0);
    *dy = g_dy.exchange(0);
    *wheel = g_wheel.exchange(0);
}

int buttons() { return g_held ? g_buttons.load() : 0; }

void let_go()
{
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_MOUSE, kLetGo, 0);
}

void play_resumed()
{
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_MOUSE, kPlayResumed, 0);
}

} // namespace mouse
