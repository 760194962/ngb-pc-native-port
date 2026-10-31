#include "d3d/apicount.h"
#include "d3d/draw.h"
#include "d3d/host.h"
#include "d3d/perfhud.h"
#include "d3d/shader_cache.h"

#include "build_profile.h"
#include "settings.h"
#include "hooks.h"
#include "perf.h"

#include <shellapi.h>

#include <algorithm>
#include <string>

// Common controls 6 (themed message boxes): a manifest dependency the linker
// embeds in blackrt.dll as resource 2; ui_thread activates it for every window it makes.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace host3d {
namespace {

HWND g_hwnd;
HICON g_icon, g_icon_sm; // load_program_icons
HANDLE g_window_ready;
std::atomic<bool> g_closed{false};
IDirect3D9Ex *g_d3d;
bool g_intz;
D3DFORMAT g_depth_view_fmt = D3DFMT_A8R8G8B8;
IDirect3DDevice9 *g_dev;
IDirect3DDevice9Ex *g_devex; // the same device
bool g_in_scene;
std::atomic<int> g_win_w, g_win_h; // client area, follows WM_SIZE
int g_aspect_w = 4, g_aspect_h = 3; // the picture's shape; the window keeps it when resized
int g_bb_w, g_bb_h;                 // host back buffer: the largest monitor, so any window size is drawn 1:1
// Exclusive fullscreen (black.ini fullscreen = 2): the device runs with Windowed = FALSE at the desktop's display mode
// while the window is fullscreen. Alt+Enter switches by a Reset at the next Swap (d3d/device.cpp recover_device).
std::atomic<bool> g_exclusive_now{false};
std::atomic<bool> g_dev_exclusive{false}; // the device is exclusive now (g_exclusive_now leads it until the next Reset)
int g_frame_w, g_frame_h;           // the part of the back buffer the current frame uses
int g_out_w, g_out_h;               // the window size as set (open_window): fullscreen's with black.ini fullscreen_res = window
WINDOWPLACEMENT g_windowed_place = {sizeof(WINDOWPLACEMENT)};

// Mouse: raw input counts and buttons, used only while the cursor is captured (click the window; focus loss
// releases it). The click that captures is not passed to the game.
std::atomic<bool> g_captured{false};
std::atomic<int> g_mouse_dx, g_mouse_dy, g_wheel, g_buttons;
bool g_hold_left; // the capturing click: masked until released
constexpr UINT WM_APP_CAPTURE = WM_APP + 0x52; // wParam: 1 = capture, 0 = release (request_capture)
constexpr UINT WM_APP_FULLSCREEN = WM_APP + 0x53; // wParam: 1 = fullscreen, 0 = windowed (set_fullscreen)

void restyle_windowed();
std::atomic<bool> g_restyle_pending{false}; // out of exclusive fullscreen: restyle the window once the device is windowed

// The device is created, tested and Reset on the window's thread (WM_APP_DEVICE; the game's render thread draws).
// Created on the render thread, the device stayed lost after Alt+Tab in exclusive fullscreen: the window did not even
// come back. Created on the window thread but tested and Reset from the render thread, TestCooperativeLevel said
// DEVICELOST for ever. Only with both on the window thread did it say DEVICENOTRESET and Reset succeed (a minimal
// D3D9 program, 2 runs each on the Iris Xe, 2026-10-22; the loader itself r282/r283).
constexpr UINT WM_APP_DEVICE = WM_APP + 0x54; // wParam: kDeviceCreate / kDeviceReset / kDeviceTest
enum { kDeviceCreate, kDeviceReset, kDeviceTest, kDeviceRecreate };
bool create_device_here();
bool reset_device_here();
bool recreate_device_here();
void release_stretch(); // present_stretch's target

// Borderless window over the whole monitor, or back to the window it was.
void toggle_fullscreen()
{
    DWORD style = GetWindowLongW(g_hwnd, GWL_STYLE);
    if (style & WS_CAPTION) {
        MONITORINFO mi = {sizeof mi};
        if (!GetWindowPlacement(g_hwnd, &g_windowed_place) ||
            !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        // WS_MINIMIZEBOX (no frame without a caption) lets Win+D minimize it: without it Show Desktop only covered the
        // window, and it came back over the whole screen behind the next program opened (2026-10-25, iconic stayed 0).
        // Exclusive fullscreen styles the window itself (0x14000000) and minimizes it when it loses the front.
        SetWindowLongW(g_hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_MINIMIZEBOX);
        g_cfg.fullscreen = true;
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        logf("host: fullscreen %ldx%ld%s", mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
             g_cfg.exclusive ? " (exclusive)" : "");
        if (g_cfg.exclusive) {
            g_exclusive_now = true;
            if (g_dev) xd3d::request_device_reset("Alt+Enter: exclusive fullscreen"); // at start: made exclusive (create_device)
        }
    } else {
        if (g_exclusive_now && g_dev) {
            // Out of exclusive fullscreen the device goes windowed first (the Reset at the next Swap) and the window is
            // restyled after that (reset_device_here). Restyled while the device was still exclusive, the runtime's
            // window hook waited on the device (r277, when the render thread did the Reset).
            g_exclusive_now = false;
            g_restyle_pending = true;
            xd3d::request_device_reset("Alt+Enter: a window");
            return;
        }
        g_exclusive_now = false;
        restyle_windowed();
    }
}

// The window back to its framed size and place (from fullscreen)
void restyle_windowed()
{
    DWORD style = GetWindowLongW(g_hwnd, GWL_STYLE);
    SetWindowLongW(g_hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
    g_cfg.fullscreen = false;
    SetWindowPlacement(g_hwnd, &g_windowed_place);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    logf("host: windowed");
}

// Window border sizes (window rect minus client rect).
void frame_extra(int *fw, int *fh)
{
    RECT wr, cr;
    GetWindowRect(g_hwnd, &wr);
    GetClientRect(g_hwnd, &cr);
    *fw = (wr.right - wr.left) - cr.right;
    *fh = (wr.bottom - wr.top) - cr.bottom;
}

// WM_SIZING: keep the client area at the picture's aspect. Edges drive the other dimension; corners follow
// whichever dimension was dragged further.
void keep_aspect(WPARAM edge, RECT *r)
{
    int fw, fh;
    frame_extra(&fw, &fh);
    int cw = r->right - r->left - fw, ch = r->bottom - r->top - fh;
    if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) ch = cw * g_aspect_h / g_aspect_w;
    else if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) cw = ch * g_aspect_w / g_aspect_h;
    else {
        cw = std::max(cw, ch * g_aspect_w / g_aspect_h);
        ch = cw * g_aspect_h / g_aspect_w;
    }
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) r->left = r->right - cw - fw;
    else r->right = r->left + cw + fw;
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) r->top = r->bottom - ch - fh;
    else r->bottom = r->top + ch + fh;
}

void clip_to_client()
{
    RECT r;
    GetClientRect(g_hwnd, &r);
    MapWindowPoints(g_hwnd, nullptr, (POINT *)&r, 2);
    ClipCursor(&r);
}

void capture_mouse(bool on)
{
    if (on == g_captured) return;
    g_captured = on;
    if (on) {
        clip_to_client();
        SetCursor(nullptr);
        g_hold_left = true;
    } else {
        ClipCursor(nullptr);
        g_buttons = 0;
    }
    g_mouse_dx = g_mouse_dy = g_wheel = 0;
}

void raw_input(HRAWINPUT h)
{
    alignas(8) BYTE buf[sizeof(RAWINPUT) + 16];
    UINT size = sizeof buf;
    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    const RAWINPUT *ri = (const RAWINPUT *)buf;
    if (ri->header.dwType != RIM_TYPEMOUSE || !g_captured) return;
    const RAWMOUSE &m = ri->data.mouse;
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        g_mouse_dx += m.lLastX;
        g_mouse_dy += m.lLastY;
    }
    USHORT f = m.usButtonFlags;
    int b = g_buttons;
    for (int i = 0; i < 5; i++) { // left, right, middle, X1, X2: down/up flag pairs
        if (f & (1 << (2 * i))) b |= 1 << i;
        if (f & (2 << (2 * i))) b &= ~(1 << i);
    }
    if (f & RI_MOUSE_LEFT_BUTTON_UP) g_hold_left = false;
    if (g_hold_left) b &= ~1;
    g_buttons = b;
    if (f & RI_MOUSE_WHEEL) { // notches (smooth-scrolling wheels send fractions: count each burst as one)
        SHORT d = (SHORT)m.usButtonData;
        g_wheel += d / WHEEL_DELTA ? d / WHEEL_DELTA : (d > 0 ? 1 : -1);
    }
}

// Fullscreen = no caption: in exclusive fullscreen the runtime sets the style itself (0x14000000, no WS_POPUP), so
// WS_POPUP alone took that for a window and Alt+Enter went into fullscreen again (2026-10-25)
bool is_fullscreen() { return !(GetWindowLongW(g_hwnd, GWL_STYLE) & WS_CAPTION); }

// A performance recording was saved (perf.cpp): windowed, offer to open its folder; fullscreen, the title says it.
void perf_report(HWND hwnd)
{
    if (is_fullscreen() || g_cfg.background) return;
    std::wstring text = widen(perf::last_report_text());
    text += L"\n\nSaved in ";
    text += perf::last_report_dir();
    text += L"\n\nOpen the folder?";
    if (MessageBoxW(hwnd, text.c_str(), L"Performance recording", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(hwnd, L"open", perf::last_report_dir(), nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_POWERBROADCAST: // sleep: the Xbox never sleeps, so the game's clock skips the time asleep (hooks.cpp)
        if (wp == PBT_APMSUSPEND) tsc_host_sleep(false);
        if (wp == PBT_APMRESUMEAUTOMATIC) tsc_host_sleep(true);
        break;
    case WM_CLOSE:
        g_closed = true;
        settings_save();
        request_exit(0, "window closed");
        return 0;
    case WM_APP_FULLSCREEN: // PC SETTINGS > VIDEO > FULLSCREEN (fe/pc_settings.cpp)
        if ((wp != 0) != is_fullscreen()) {
            logf("host: PC SETTINGS fullscreen %d", (int)wp);
            toggle_fullscreen();
            settings_save();
        }
        return 0;
    case perf::WM_APP_PERF:
        capture_mouse(false);
        perf_report(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp)) {
            g_win_w = LOWORD(lp);
            g_win_h = HIWORD(lp);
        }
        if (g_captured) clip_to_client();
        return 0;
    case WM_MOVE:
        if (g_captured) clip_to_client();
        break;
    case WM_SIZING:
        keep_aspect(wp, (RECT *)lp);
        return TRUE;
    case WM_GETMINMAXINFO:
        if (g_hwnd) {
            int fw, fh;
            frame_extra(&fw, &fh);
            ((MINMAXINFO *)lp)->ptMinTrackSize = {320 + fw, 320 * g_aspect_h / g_aspect_w + fh};
        }
        return 0;
    case WM_APP_CAPTURE: // from the input poll: pause frees the mouse, resuming play takes it again
        if (!wp) capture_mouse(false);
        else if (GetForegroundWindow() == h && !IsIconic(h)) capture_mouse(true);
        return 0;
    case WM_SYSKEYDOWN: // Alt+Enter: fullscreen <-> window, as PC SETTINGS > VIDEO > FULLSCREEN (first press, not repeats)
        if (wp == VK_RETURN && (lp & (1 << 29)) && !(lp & (1 << 30))) {
            logf("host: Alt+Enter (key data 0x%08lx, style 0x%08lx)", (unsigned long)lp, GetWindowLongW(h, GWL_STYLE));
            toggle_fullscreen();
            settings_save();
            return 0;
        }
        break;
    case WM_SYSCHAR: // the Alt+Enter character would ring the default beep
        if (wp == VK_RETURN) return 0;
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0; // Alt / F10 must not open the system menu by keyboard and stall input
        break;
    case WM_LBUTTONDOWN:
        capture_mouse(true);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) capture_mouse(false);
        break;
    case WM_APP_DEVICE: // create_device, reset_device, test_cooperative_level: on this thread
        if (wp == kDeviceCreate) return create_device_here();
        if (wp == kDeviceReset) return reset_device_here();
        if (wp == kDeviceRecreate) return recreate_device_here();
        {
            // A 9Ex device is not lost when Windows takes the screen (Alt+Tab in exclusive fullscreen, the lock
            // screen): CheckDeviceState then says S_PRESENT_OCCLUDED, and S_PRESENT_MODE_CHANGED after the desktop's
            // mode changed. Only a hung or removed driver loses it (ex9fs, Iris Xe and RTX 3060, 2026-10-24).
            HRESULT st = g_devex->CheckDeviceState(h);
            return st == D3DERR_DEVICELOST || st == D3DERR_DEVICEHUNG || st == D3DERR_DEVICEREMOVED || st == D3DERR_OUTOFVIDEOMEMORY
                       ? st
                       : D3D_OK;
        }
    case WM_KILLFOCUS:
        capture_mouse(false);
        break;
    case WM_SETCURSOR:
        if (g_captured && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_INPUT:
        raw_input((HRAWINPUT)lp);
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

DWORD WINAPI ui_thread(void *)
{
    using SetDesc = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    if (auto set_desc = (SetDesc)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))
        set_desc(GetCurrentThread(), L"window (messages, raw mouse)");
    ACTCTXW ac = {sizeof ac};
    ac.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    ac.hModule = GetModuleHandleW(L"blackrt.dll");
    ac.lpResourceName = MAKEINTRESOURCEW(2);
    HANDLE ctx = CreateActCtxW(&ac);
    ULONG_PTR cookie = 0;
    if (ctx == INVALID_HANDLE_VALUE || !ActivateActCtx(ctx, &cookie))
        logf("host: common controls 6 not activated (%lu); dialogs keep the classic look", GetLastError());
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(L"blackrt.dll");
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"BlackLoaderWindow";
    // The window and taskbar icon: the program's own (icon group 1), present only in a local build given PORT_ICON, an
    // .ico made from the player's game (loader/CMakeLists.txt). Without it, the default application icon. Read from the
    // copies load_program_icons took before the XBE was mapped over the program's image, resources included.
    wc.hIcon = g_icon;
    wc.hIconSm = g_icon_sm;
    RegisterClassExW(&wc);
    logf("host: window icon %s", wc.hIcon ? "from the program" : "none (no icon group 1: built with PORT_ICON empty)");
    RECT r = {0, 0, g_win_w, g_win_h};
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, FALSE);
    // Centred on the primary monitor's work area (the desktop without the taskbar) at every start (the user,
    // 2026-10-22); a window larger than that area starts at its top left.
    RECT work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int wx = work.left + std::max(0, (int)(work.right - work.left - ww) / 2);
    int wy = work.top + std::max(0, (int)(work.bottom - work.top - wh) / 2);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, game_title(), style, wx, wy, ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
    RAWINPUTDEVICE mouse = {0x01, 0x02, 0, g_hwnd}; // generic desktop / mouse, while the window has focus
    if (!RegisterRawInputDevices(&mouse, 1, sizeof mouse)) logf("host: raw mouse input not available (%lu)", GetLastError());
    ShowWindow(g_hwnd, g_cfg.background ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL);
    // In front of the other windows at every start, the bundled programs (SNES, arcade) included (the user,
    // 2026-10-22). Windows lets a program take the front only when the one in front allows it: the launcher and the
    // program that relaunches into another XBE call AllowSetForegroundWindow for the new process (kernel/k_misc.cpp).
    if (!g_cfg.background) {
        SetForegroundWindow(g_hwnd);
        logf("host: window %s", GetForegroundWindow() == g_hwnd ? "in front" : "not given the front by Windows");
    }
    if (g_cfg.fullscreen) toggle_fullscreen();
    SetEvent(g_window_ready);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

BOOL CALLBACK grow_to_monitor(HMONITOR, HDC, LPRECT r, LPARAM p)
{
    auto *s = (SIZE *)p;
    s->cx = std::max(s->cx, r->right - r->left);
    s->cy = std::max(s->cy, r->bottom - r->top);
    return TRUE;
}

} // namespace

// Real screen pixels (no DPI bitmap stretching by Windows on scaled displays).
void make_dpi_aware()
{
    typedef BOOL(WINAPI * PFN_SetContext)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    auto set_ctx = u ? (PFN_SetContext)GetProcAddress(u, "SetProcessDpiAwarenessContext") : nullptr;
    if (!set_ctx || !set_ctx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
}

bool open_window(int width, int height, int aspect_w, int aspect_h)
{
    if (g_hwnd) return true;
    make_dpi_aware();
    g_aspect_w = aspect_w;
    g_aspect_h = aspect_h;
    if (width > 0 && height <= 0) height = width * aspect_h / aspect_w;
    if (width <= 0 || height <= 0) { // largest client area of the aspect that fits the work area comfortably
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int avail_h = (work.bottom - work.top) * 9 / 10, avail_w = (work.right - work.left) * 9 / 10;
        int step = aspect_h * 2; // whole multiples of the aspect
        height = avail_h / step * step;
        width = height * aspect_w / aspect_h;
        if (width > avail_w) {
            step = aspect_w * 2;
            width = avail_w / step * step;
            height = width * aspect_h / aspect_w;
        }
    }
    g_win_w = width;
    g_win_h = height;
    g_out_w = width; // the launcher's size (or auto's), not later drags: what fullscreen_res = window promises
    g_out_h = height;
    g_window_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, ui_thread, nullptr, 0, nullptr);
    WaitForSingleObject(g_window_ready, 10000);
    if (!g_hwnd) {
        logf("host: could not create the window");
        return false;
    }
    logf("host: window %dx%d (resizable at %d:%d)", width, height, aspect_w, aspect_h);
    return true;
}

HWND window() { return g_hwnd; }

void load_program_icons()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    g_icon = (HICON)LoadImageW(exe, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_icon_sm = (HICON)LoadImageW(exe, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
}

void set_title(const wchar_t *text)
{
    if (g_hwnd) SetWindowTextW(g_hwnd, text);
}

const wchar_t *game_title()
{
    // each Ninja Gaiden Black build has its own (build_profile.h); its bundled programs keep x2.1's
    if (g_cfg.profile && g_cfg.profile->window_title) return g_cfg.profile->window_title;
    if (g_cfg.profile && !g_cfg.profile->black) return kProfileNgbHurricaneX21.window_title;
    return L"BLACK";
}

bool window_closed() { return g_closed; }

void frame_layout(RECT *frame, RECT *picture)
{
    int cw = std::max(1, g_win_w.load()), ch = std::max(1, g_win_h.load());
    if (g_dev_exclusive && g_bb_w > 0 && g_bb_h > 0) { // exclusive: the whole back buffer, the display mode's size
        cw = g_bb_w;
        ch = g_bb_h;
    }
    if (g_cfg.fs_window_res && g_out_w > 0 && g_out_h > 0 && (g_dev_exclusive || is_fullscreen())) {
        // Fullscreen at the window size (black.ini fullscreen_res = window, for slow machines): the frame keeps the
        // screen's shape, shrunk to fit within the window size, and is stretched over the screen evenly (borderless:
        // by Present; exclusive: by present_stretch). The picture (and with render = native the 3D render) follows:
        // fewer pixels for the final passes and the render. The display mode is not changed: a mode of the screen's
        // shape within a 16:9 window size was not there on a 16:10 laptop (RTX 3060, 1920x1200, 2026-10-25).
        float k = std::min({1.0f, (float)g_out_w / cw, (float)g_out_h / ch});
        cw = std::max(1, (int)lroundf(cw * k));
        ch = std::max(1, (int)lroundf(ch * k));
    }
    if (cw > g_bb_w || ch > g_bb_h) { // larger than every monitor was at start: draw smaller, Present stretches
        float k = std::min((float)g_bb_w / cw, (float)g_bb_h / ch);
        cw = std::max(1, (int)(cw * k));
        ch = std::max(1, (int)(ch * k));
    }
    g_frame_w = cw;
    g_frame_h = ch;
    *frame = {0, 0, cw, ch};
    int pw = cw, ph = cw * g_aspect_h / g_aspect_w;
    if (ph > ch) {
        ph = ch;
        pw = ch * g_aspect_w / g_aspect_h;
    }
    int x = (cw - pw) / 2, y = (ch - ph) / 2;
    *picture = {x, y, x + pw, y + ph};
}

bool mouse_captured() { return g_captured; }

void request_capture(bool on)
{
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_CAPTURE, on ? 1 : 0, 0);
}

void set_fullscreen(bool on)
{
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_FULLSCREEN, on ? 1 : 0, 0);
}

bool fullscreen() { return g_hwnd && is_fullscreen(); }
bool exclusive() { return g_exclusive_now; }

int mouse_buttons() { return g_captured ? g_buttons.load() : 0; }

void mouse_take(int *dx, int *dy, int *wheel)
{
    *dx = g_mouse_dx.exchange(0);
    *dy = g_mouse_dy.exchange(0);
    *wheel = g_wheel.exchange(0);
}

D3DPRESENT_PARAMETERS g_pp; // as created; Reset uses the same

// Exclusive fullscreen: the desktop's display mode on the default adapter (if it cannot be read: stays windowed)
D3DPRESENT_PARAMETERS exclusive_params(D3DPRESENT_PARAMETERS pp)
{
    D3DDISPLAYMODE dm = {};
    if (FAILED(g_d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &dm))) return pp;
    pp.Windowed = FALSE;
    pp.BackBufferWidth = dm.Width;
    pp.BackBufferHeight = dm.Height;
    pp.FullScreen_RefreshRateInHz = dm.RefreshRate;
    // Vsync (black.ini vsync) applies here only: an exclusive device shows its frames itself and tears without it.
    // With one copied back buffer Present waited for each frame to be shown before the next could start, so on the
    // Iris Xe at 60 Hz the game ran at 78% (r361); two flipped buffers let the next frame start meanwhile. A flipped
    // buffer is shown whole: the frame fills the screen here (frame_layout), and present() gives no rectangle.
    pp.PresentationInterval = g_cfg.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    if (g_cfg.vsync) {
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferCount = 2;
    }
    return pp;
}

// The display mode of an exclusive device (9Ex takes it besides the parameters): the desktop's, as exclusive_params
D3DDISPLAYMODEEX desktop_mode()
{
    D3DDISPLAYMODEEX m = {sizeof m};
    g_d3d->GetAdapterDisplayModeEx(D3DADAPTER_DEFAULT, &m, nullptr);
    return m;
}

namespace {

// Direct3D 9Ex: no lost devices when Windows takes the screen and no D3DPOOL_DEFAULT resources to give up before a
// Reset (ex9test, ex9fs: Iris Xe and RTX 3060, 2026-10-24). Under plain Direct3D 9 a Reset failed for good while the
// visibility test's query lived and Alt+Tab crashed (r495); minimised, the frames' work piled up (r528).
bool create_device_here()
{
    HRESULT made = g_d3d ? S_OK : Direct3DCreate9Ex(D3D_SDK_VERSION, &g_d3d);
    if (FAILED(made)) {
        logf("host: Direct3DCreate9Ex failed 0x%08lx", made);
        return false;
    }
    SIZE mon = {g_win_w, g_win_h};
    EnumDisplayMonitors(nullptr, nullptr, grow_to_monitor, (LPARAM)&mon);
    g_bb_w = mon.cx;
    g_bb_h = mon.cy;
    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = g_bb_w;
    pp.BackBufferHeight = g_bb_h;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_COPY; // COPY: Present can show just the part of the back buffer a frame used
    pp.hDeviceWindow = g_hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    // Frames are paced by the emulated Xbox vblank (D3DDevice_Swap). A windowed device (window, borderless) never waits
    // for the display: the desktop compositor shows its frames at the display's refresh, so they do not tear anyway,
    // and waiting there made Present block until the next composition. CPU and GPU then could not overlap: on the Iris
    // Xe at 60 Hz the game ran at 88.6% speed with 9.5% late frames (r360). Vsync is for exclusive fullscreen only
    // (exclusive_params).
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    g_pp = pp; // the windowed parameters: Reset goes back to them (reset_device)
    if (g_exclusive_now) pp = exclusive_params(pp);
    // FPU_PRESERVE: the game's x87 math must keep its precision; MULTITHREADED: loader threads may call in.
    // Not NOWINDOWCHANGES: with it the runtime does not minimise the window and bring it back around Alt+Tab in
    // exclusive fullscreen (r279). The window is restyled only while the device is windowed instead.
    DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;
    D3DDISPLAYMODEEX mode = desktop_mode();
    HRESULT hr = g_d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd, flags, &pp, pp.Windowed ? nullptr : &mode, &g_devex);
    if (FAILED(hr)) {
        logf("host: CreateDeviceEx failed 0x%08lx, retrying with software vertex processing", hr);
        flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;
        hr = g_d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd, flags, &pp, pp.Windowed ? nullptr : &mode, &g_devex);
        if (FAILED(hr)) {
            logf("host: CreateDeviceEx failed 0x%08lx", hr);
            return false;
        }
    }
    g_dev = g_devex;
    g_bb_w = (int)pp.BackBufferWidth; // exclusive: the display mode's size
    g_bb_h = (int)pp.BackBufferHeight;
    g_dev_exclusive = !pp.Windowed;
    if (!pp.Windowed)
        logf("host: exclusive fullscreen %ux%u at %u Hz, vsync %s", pp.BackBufferWidth, pp.BackBufferHeight, pp.FullScreen_RefreshRateInHz,
             pp.PresentationInterval == D3DPRESENT_INTERVAL_ONE ? "on" : "off");
    else if (g_cfg.vsync) logf("host: windowed: vsync is not used (the desktop compositor shows the frames without tearing)");
    if (g_cfg.api_count) apicount::install(g_dev);
    D3DADAPTER_IDENTIFIER9 id = {};
    g_d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    perfhud::start(id.Description, id.VendorId, id.DeviceId); // black.ini perfhud
    if (g_cfg.aniso > 1) { // the card's limit
        D3DCAPS9 caps = {};
        g_dev->GetDeviceCaps(&caps);
        int top = (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) ? (int)caps.MaxAnisotropy : 1;
        if (g_cfg.aniso > top) logf("host: anisotropic filtering %dx asked, the card allows %dx", g_cfg.aniso, top);
        g_cfg.aniso = std::max(1, std::min(g_cfg.aniso, top));
        logf("host: anisotropic texture filtering %dx", g_cfg.aniso);
    }
    g_intz = SUCCEEDED(g_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
                                                D3DRTYPE_TEXTURE, FMT_INTZ));
    // Depth views (d3d/device.cpp depth_view): R32F keeps the 24-bit depth the shadow compare needs, where an
    // 8-bit channel quantised it (docs/07 #2). The view is sampled with the game's filter, so R32F must filter.
    if (SUCCEEDED(g_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                           D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_R32F)))
        g_depth_view_fmt = D3DFMT_R32F;
    logf("host: depth views %s", g_depth_view_fmt == D3DFMT_R32F ? "R32F (full depth precision)"
                                                                  : "A8R8G8B8 (R32F render target with filtering not supported: 8-bit depth)");
    logf("host: Direct3D 9Ex device, back buffer %dx%d on '%s'; INTZ depth textures %s", g_bb_w, g_bb_h, id.Description,
         g_intz ? "supported" : "NOT supported (depth-buffer reads return zero)");
    xd3d::prewarm_shaders(g_dev); // the cached shaders' objects now, not in the frame that first draws with them
    return true;
}

// The old device's last reference released (what it still counts are objects d3d/device.cpp drop_device_objects
// left out: the device then stays in memory), and a new one made as at the start
bool recreate_device_here()
{
    release_stretch(); // present_stretch's target: the old device's, so it would keep that device alive
    ULONG left = g_dev ? g_dev->Release() : 0;
    g_dev = nullptr;
    g_devex = nullptr;
    g_in_scene = false;
    logf("host: the old device released: %lu references left%s", left, left ? " (objects not let go: it stays in memory)" : "");
    return create_device_here();
}

bool reset_device_here()
{
    D3DPRESENT_PARAMETERS pp = g_exclusive_now ? exclusive_params(g_pp) : g_pp;
    D3DDISPLAYMODEEX mode = desktop_mode();
    HRESULT hr = g_devex->ResetEx(&pp, pp.Windowed ? nullptr : &mode);
    if (FAILED(hr)) {
        static unsigned failures; // retried every 50 ms until it succeeds (d3d/device.cpp recover_device)
        if (failures++ % 100 < 5)
            logf("host: Reset failed 0x%08lx (%s %ux%u; window %s%s; then TestCooperativeLevel 0x%08lx)", hr,
                 pp.Windowed ? "windowed" : "exclusive", pp.BackBufferWidth, pp.BackBufferHeight, IsIconic(g_hwnd) ? "minimised" : "shown",
                 GetForegroundWindow() == g_hwnd ? ", in front" : ", not in front", g_dev->TestCooperativeLevel());
        return false;
    }
    g_bb_w = (int)pp.BackBufferWidth; // the exclusive mode's size can differ from the largest monitor's
    g_bb_h = (int)pp.BackBufferHeight;
    g_dev_exclusive = !pp.Windowed;
    logf("host: device reset, %s %ux%u, vsync %s", pp.Windowed ? "windowed" : "exclusive fullscreen", pp.BackBufferWidth, pp.BackBufferHeight,
         pp.PresentationInterval == D3DPRESENT_INTERVAL_ONE ? "on" : "off");
    if (pp.Windowed && g_restyle_pending.exchange(false)) restyle_windowed(); // now the window can change
    return true;
}

} // namespace

bool create_device() { return SendMessageW(g_hwnd, WM_APP_DEVICE, kDeviceCreate, 0) != 0; }
HRESULT test_cooperative_level() { return (HRESULT)SendMessageW(g_hwnd, WM_APP_DEVICE, kDeviceTest, 0); }
bool recreate_device() { return SendMessageW(g_hwnd, WM_APP_DEVICE, kDeviceRecreate, 0) != 0; }

IDirect3DDevice9 *dev() { return g_dev; }

bool CpuTexture::make(int width, int height)
{
    if (tex && w == width && h == height) return true;
    release();
    if (!g_dev || FAILED(g_dev->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
        FAILED(g_dev->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr))) {
        release();
        return false;
    }
    w = width;
    h = height;
    return true;
}

void *CpuTexture::lock(int *pitch)
{
    D3DLOCKED_RECT lr;
    if (!sys || FAILED(sys->LockRect(0, &lr, nullptr, 0))) return nullptr;
    *pitch = lr.Pitch;
    return lr.pBits;
}

void CpuTexture::unlock()
{
    sys->UnlockRect(0);
    g_dev->UpdateTexture(sys, tex);
}

void CpuTexture::release()
{
    if (sys) sys->Release();
    if (tex) tex->Release();
    sys = tex = nullptr;
    w = h = 0;
}
bool intz() { return g_intz; }
D3DFORMAT depth_view_format() { return g_depth_view_fmt; }

void begin_scene()
{
    if (!g_in_scene && g_dev) {
        g_dev->BeginScene();
        g_in_scene = true;
    }
}

void end_scene()
{
    if (g_in_scene && g_dev) {
        g_dev->EndScene();
        g_in_scene = false;
    }
}

// Exclusive fullscreen at the window size: the frame, drawn at the back buffer's top left, scaled over all of it (a
// flipped buffer is shown whole, and the frame is the size the final passes and the render work at). StretchRect
// cannot copy within one surface, so through a target of the frame's size; linear, as Present scales borderless.
namespace {
IDirect3DSurface9 *g_stretch;
int g_stretch_w, g_stretch_h;

void release_stretch()
{
    if (g_stretch) g_stretch->Release();
    g_stretch = nullptr;
    g_stretch_w = g_stretch_h = 0;
}

bool present_stretch()
{
    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    if (g_stretch && (g_stretch_w != g_frame_w || g_stretch_h != g_frame_h)) release_stretch();
    if (!g_stretch) {
        D3DSURFACE_DESC d = {};
        bb->GetDesc(&d);
        if (FAILED(g_dev->CreateRenderTarget(g_frame_w, g_frame_h, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_stretch, nullptr))) {
            bb->Release();
            return false;
        }
        g_stretch_w = g_frame_w;
        g_stretch_h = g_frame_h;
        logf("host: exclusive fullscreen at the window size: the %dx%d frame scaled over %dx%d", g_frame_w, g_frame_h, g_bb_w, g_bb_h);
    }
    RECT fr = {0, 0, g_frame_w, g_frame_h};
    bool ok = SUCCEEDED(g_dev->StretchRect(bb, &fr, g_stretch, nullptr, D3DTEXF_NONE)) &&
              (SUCCEEDED(g_dev->StretchRect(g_stretch, nullptr, bb, nullptr, D3DTEXF_LINEAR)) ||
               SUCCEEDED(g_dev->StretchRect(g_stretch, nullptr, bb, nullptr, D3DTEXF_NONE))); // a card without linear
    bb->Release();
    return ok;
}
} // namespace

HRESULT present()
{
    if (!g_dev) return D3D_OK;
    begin_scene();
    g_dev->EndScene();
    g_in_scene = false;
    // Present minimised too (it says S_PRESENT_OCCLUDED there): it also hands the frame's commands to the GPU. Skipped,
    // they piled up in the driver (32 frames and more, r528) and the first Present after the window came back took
    // 1-10 s, a black window with the game stopped (the user, 2026-10-24, borderless; r514-r518). Under plain Direct3D 9
    // an exclusive device was lost there and its Reset dropped the pile; a 9Ex device is not lost.
    bool whole = g_dev_exclusive && g_frame_w > 0 && g_frame_h > 0 && (g_frame_w < g_bb_w || g_frame_h < g_bb_h) && present_stretch();
    if (g_exclusive_now && g_cfg.vsync) return g_dev->Present(nullptr, nullptr, nullptr, nullptr); // flipped (exclusive_params)
    RECT src = {0, 0, g_frame_w && !whole ? g_frame_w : g_bb_w, g_frame_h && !whole ? g_frame_h : g_bb_h};
    return g_dev->Present(&src, nullptr, nullptr, nullptr); // stretched to the client area (1:1 unless it outgrew the monitors)
}

bool reset_device()
{
    end_scene();
    return SendMessageW(g_hwnd, WM_APP_DEVICE, kDeviceReset, 0) != 0;
}

} // namespace host3d
