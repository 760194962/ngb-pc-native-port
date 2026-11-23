#include "d3d/apicount.h"
#include "d3d/draw.h"
#include "d3d/drawwarm.h"
#include "d3d/host.h"
#include "d3d/perfhud.h"
#include "d3d/shader_cache.h"
#include "d3d/vshader.h"

#include "build_profile.h"
#include "settings.h"
#include "hooks.h"
#include "perf.h"

#include <shellapi.h>

#include <algorithm>
#include <string>

namespace xd3d {
void texture_pool_start(IDirect3DDevice9 *dev); // texture.cpp: the texture pool filled to its targets
void draw_ring_prepare(IDirect3DDevice9 *d);    // draw.cpp: the draw ring sets made and written at device creation
}

// Common controls 6 (themed message boxes): a manifest dependency the linker
// embeds in blackrt.dll as resource 2; ui_thread activates it for every window it makes.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace host3d {
WindowState &window_state()
{
    static WindowState state;
    return state;
}

namespace {

WindowState &g_window = window_state();

HWND g_hwnd;
HICON g_icon, g_icon_sm; // load_program_icons
HANDLE g_window_ready;
std::atomic<bool> g_closed{false};
IDirect3D9Ex *g_d3d;
bool g_intz;
D3DFORMAT g_depth_view_fmt = D3DFMT_A8R8G8B8;
uint32_t g_msaa_mask; // bit n: n-sample MSAA supported for the frame's colour and depth
IDirect3DDevice9 *g_dev;
IDirect3DDevice9Ex *g_devex; // the same device
bool g_in_scene;
std::atomic<int> g_win_w, g_win_h; // client area, follows WM_SIZE
int g_aspect_w = 4, g_aspect_h = 3; // the picture's shape; the window keeps it when resized
std::atomic<int> g_bb_w, g_bb_h; // host back buffer: the largest monitor, so any window size is drawn 1:1
// Exclusive fullscreen (black.ini fullscreen = 2): the device runs with Windowed = FALSE at the desktop's display mode
// while the window is fullscreen. Alt+Enter switches by a Reset at the next Swap (d3d/device.cpp recover_device).
// One state machine owns the window: request, the device that actually exists and its presentation path.
// A request may lead the device by one Swap; Present and frame sizing follow the device, never the request.
int g_frame_w, g_frame_h;           // the part of the back buffer the current frame uses
std::atomic<int> g_out_w, g_out_h; // the window size as set (open_window): fullscreen's with black.ini fullscreen_res = window
WINDOWPLACEMENT g_windowed_place = {sizeof(WINDOWPLACEMENT)};

// Mouse: raw input counts and buttons, used only while the cursor is captured (click the window; focus loss
// releases it). The click that captures is not passed to the game.
std::atomic<bool> g_captured{false};
std::atomic<int> g_mouse_dx, g_mouse_dy, g_wheel, g_buttons;
bool g_hold_left; // the capturing click: masked until released
constexpr UINT WM_APP_CAPTURE = WM_APP + 0x52; // wParam: 1 = capture, 0 = release (request_capture)
constexpr UINT WM_APP_FULLSCREEN = WM_APP + 0x53; // wParam: 1 = fullscreen, 0 = windowed (set_fullscreen)

void restyle_windowed();

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
    if (!g_window.is_fullscreen()) {
        MONITORINFO mi = {sizeof mi};
        if (!GetWindowPlacement(g_hwnd, &g_windowed_place) ||
            !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        // WS_MINIMIZEBOX (no frame without a caption) lets Win+D minimize it: without it Show Desktop only covered the
        // window, and it came back over the whole screen behind the next program opened (2026-10-25, iconic stayed 0).
        // Exclusive fullscreen styles the window itself (0x14000000) and minimizes it when it loses the front.
        SetWindowLongW(g_hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_MINIMIZEBOX);
        const BOOL sized = SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                                        mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_cfg.fullscreen = sized != FALSE;
        g_window.set_fullscreen(sized != FALSE);
        if (!sized) {
            logf("host: fullscreen SetWindowPos failed (%lu); going back to a window", GetLastError());
            restyle_windowed();
            return;
        }
        logf("host: fullscreen %ldx%ld%s", mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
             g_cfg.exclusive ? " (exclusive)" : "");
        if (g_cfg.exclusive && sized) {
            g_window.request(WindowMode::Exclusive);
            if (g_dev) xd3d::request_device_reset("Alt+Enter: exclusive fullscreen"); // at start: made exclusive (create_device)
        }
    } else {
        if (g_window.actual_exclusive()) {
            // The device is exclusive now: go back to a windowed device first, then restyle in reset_device_here.
            g_window.request(WindowMode::Windowed);
            g_window.restyle_pending = true;
            g_cfg.fullscreen = false; // the user's choice is windowed now, even though the device resets first
            xd3d::request_device_reset("Alt+Enter: a window");
            return;
        }
        // The device is still windowed (an exclusive request has not been committed yet): only undo the request.
        g_window.request(WindowMode::Windowed);
        restyle_windowed();
    }
}

// The window back to its framed size and place (from fullscreen)
void restyle_windowed()
{
    DWORD style = GetWindowLongW(g_hwnd, GWL_STYLE);
    SetWindowLongW(g_hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
    g_cfg.fullscreen = false;
    g_window.set_fullscreen(false);
    const BOOL placed = SetWindowPlacement(g_hwnd, &g_windowed_place);
    const BOOL sized = SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    if (placed && sized) g_window.restyle_pending = false;
    else logf("host: windowed style restored, but the saved placement failed (placement %d, frame %d)", (int)placed, (int)sized);
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
bool is_fullscreen() { return g_window.is_fullscreen(); }

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
    // No input method in the game: nothing here takes typed text, and a window taking the focus first talks to the
    // input method's service. With that service hung (2026-11-10, a Chinese IME), every window of the PC that took the
    // front hung with it: the game window stayed white "not responding" at start. Also no IME candidate box when a
    // player's keys switch the input mode mid-game.
    if (HMODULE imm = LoadLibraryW(L"imm32.dll"))
        if (auto disable = (BOOL(WINAPI *)(DWORD))GetProcAddress(imm, "ImmDisableIME")) disable((DWORD)-1); // every thread of the process
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
    if (g_window.actual_exclusive() && g_bb_w > 0 && g_bb_h > 0) { // exclusive: the whole back buffer, the display mode's size
        cw = g_bb_w;
        ch = g_bb_h;
    }
    if (g_cfg.fs_window_res && g_out_w > 0 && g_out_h > 0 && (g_window.actual_exclusive() || is_fullscreen())) {
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
bool exclusive() { return g_window.actual_exclusive(); }

int mouse_buttons() { return g_captured ? g_buttons.load() : 0; }

void mouse_take(int *dx, int *dy, int *wheel)
{
    *dx = g_mouse_dx.exchange(0);
    *dy = g_mouse_dy.exchange(0);
    *wheel = g_wheel.exchange(0);
}

D3DPRESENT_PARAMETERS g_pp; // as created; Reset uses the same

// The display mode of an exclusive device (9Ex takes it besides the parameters): the desktop's. With vsync on and a desktop
// refreshing outside 59-61 Hz (a 144 or 165 Hz panel), a mode of the same size at 59-61 Hz when the display has one: the
// game makes 60 frames a second, and at 165 Hz vsync showed them for 3, 3, 3, 2 refreshes (18.2 / 12.1 ms, judder); at
// 60 Hz each is shown for one. black.ini exclusive_60hz = 0 (the launcher's Graphics page) or --keep-refresh keeps the
// desktop's.
D3DDISPLAYMODEEX exclusive_mode()
{
    D3DDISPLAYMODEEX m = {sizeof m};
    if (FAILED(g_d3d->GetAdapterDisplayModeEx(D3DADAPTER_DEFAULT, &m, nullptr))) m.Width = 0;
    if (!m.Width || !g_cfg.vsync || !g_cfg.exclusive_60hz || (m.RefreshRate >= 59 && m.RefreshRate <= 61)) return m;
    D3DDISPLAYMODEFILTER f = {sizeof f, m.Format, D3DSCANLINEORDERING_PROGRESSIVE};
    UINT n = g_d3d->GetAdapterModeCountEx(D3DADAPTER_DEFAULT, &f);
    D3DDISPLAYMODEEX best = m;
    for (UINT i = 0; i < n; i++) {
        D3DDISPLAYMODEEX c = {sizeof c};
        if (FAILED(g_d3d->EnumAdapterModesEx(D3DADAPTER_DEFAULT, &f, i, &c))) continue;
        if (c.Width != m.Width || c.Height != m.Height || c.RefreshRate < 59 || c.RefreshRate > 61) continue;
        if (best.RefreshRate == m.RefreshRate || abs((int)c.RefreshRate - 60) < abs((int)best.RefreshRate - 60)) best = c;
    }
    static UINT logged;
    if (logged != m.RefreshRate) {
        logged = m.RefreshRate;
        if (best.RefreshRate != m.RefreshRate)
            logf("host: exclusive fullscreen with vsync: %ux%u at %u Hz instead of the desktop's %u Hz (60 frames a second shown evenly)",
                 best.Width, best.Height, best.RefreshRate, m.RefreshRate);
        else
            logf("host: exclusive fullscreen with vsync: no 60 Hz mode at %ux%u, the desktop's %u Hz kept", m.Width, m.Height, m.RefreshRate);
    }
    return best;
}

// Exclusive fullscreen: exclusive_mode() on the default adapter. *available says whether a display mode could be
// read; when false the caller must fall back to a window, not keep a fullscreen style on a windowed device.
D3DPRESENT_PARAMETERS exclusive_params(D3DPRESENT_PARAMETERS pp, bool *available)
{
    D3DDISPLAYMODEEX dm = exclusive_mode();
    if (available) *available = dm.Width != 0;
    if (!dm.Width) return pp;
    pp.Windowed = FALSE;
    pp.BackBufferWidth = dm.Width;
    pp.BackBufferHeight = dm.Height;
    pp.FullScreen_RefreshRateInHz = dm.RefreshRate;
    // Vsync (black.ini vsync) applies here only: an exclusive device shows its frames itself and tears without it.
    // With one copied back buffer Present waited for each frame to be shown before the next could start, so on the
    // Iris Xe at 60 Hz the game ran at 78% (r361); two flipped buffers let the next frame start meanwhile. A flipped
    // buffer is shown whole: the frame fills the screen here (frame_layout), and present() gives no rectangle.
    // Both vsync states use the flip chain; PresentationInterval is the only difference (ONE waits, IMMEDIATE does not).
    pp.PresentationInterval = g_cfg.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferCount = 2;
    return pp;
}

// The presentation path is a property of the device that was actually created, not of the Alt+Enter request.
static const char *presentation_name(bool flip) { return flip ? "flip" : "copy (window)"; }

// Direct3D 9 has no query for the swap effect it actually made. At least say when the accepted parameters are not the
// exclusive flip chain this code assumes; a silent copy chain would make the state lie.
static void check_exclusive_present(const D3DPRESENT_PARAMETERS &pp)
{
    if (!pp.Windowed && !(pp.SwapEffect == D3DSWAPEFFECT_DISCARD && pp.BackBufferCount >= 2))
        logf("host: exclusive device accepted %ux%u with swap effect %u / %u buffers, not the flip chain", pp.BackBufferWidth,
             pp.BackBufferHeight, (unsigned)pp.SwapEffect, pp.BackBufferCount);
}


namespace {

// Direct3D 9Ex: no lost devices when Windows takes the screen and no D3DPOOL_DEFAULT resources to give up before a
// Reset (ex9test, ex9fs: Iris Xe and RTX 3060, 2026-10-24). Under plain Direct3D 9 a Reset failed for good while the
// visibility test's query lived and Alt+Tab crashed (r495); minimised, the frames' work piled up (r528).
// Why the first device could not be made, for device_failure_text: the step, its results, the card.
struct DeviceFailure {
    bool runtime;           // Direct3DCreate9Ex failed (no Direct3D 9 at all), else CreateDeviceEx
    HRESULT hr_hw, hr_sw;   // CreateDeviceEx with hardware / software vertex processing (runtime: hr_hw only)
    UINT adapters;          // the Direct3D 9 adapters seen
    D3DADAPTER_IDENTIFIER9 id;
    UINT w, h;
    bool exclusive;
} g_device_failure;

bool create_device_here()
{
    HRESULT made = g_d3d ? S_OK : Direct3DCreate9Ex(D3D_SDK_VERSION, &g_d3d);
    if (FAILED(made)) {
        logf("host: Direct3DCreate9Ex failed 0x%08lx", made);
        g_device_failure = {};
        g_device_failure.runtime = true;
        g_device_failure.hr_hw = made;
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
    bool exclusive_ok = true;
    if (g_window.requested_exclusive()) pp = exclusive_params(pp, &exclusive_ok);
    if (g_window.requested_exclusive() && !exclusive_ok) {
        // No display mode to switch to: a fullscreen style on a windowed device is the trap this state exists to avoid.
        logf("host: exclusive display mode not available; starting windowed");
        g_window.request(WindowMode::Windowed);
        g_window.restyle_pending = true;
    }
    // FPU_PRESERVE: the game's x87 math must keep its precision; MULTITHREADED: loader threads may call in.
    // Not NOWINDOWCHANGES: with it the runtime does not minimise the window and bring it back around Alt+Tab in
    // exclusive fullscreen (r279). The window is restyled only while the device is windowed instead.
    DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;
    D3DDISPLAYMODEEX mode = exclusive_mode();
    HRESULT hr = g_d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd, flags, &pp, pp.Windowed ? nullptr : &mode, &g_devex);
    if (FAILED(hr)) {
        logf("host: CreateDeviceEx failed 0x%08lx, retrying with software vertex processing", hr);
        const HRESULT hr_hw = hr;
        flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;
        hr = g_d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd, flags, &pp, pp.Windowed ? nullptr : &mode, &g_devex);
        if (FAILED(hr)) {
            logf("host: CreateDeviceEx failed 0x%08lx", hr);
            DeviceFailure &f = g_device_failure;
            f = {};
            f.hr_hw = hr_hw;
            f.hr_sw = hr;
            f.adapters = g_d3d->GetAdapterCount();
            if (f.adapters) g_d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &f.id);
            f.w = pp.BackBufferWidth;
            f.h = pp.BackBufferHeight;
            f.exclusive = !pp.Windowed;
            return false;
        }
    }
    g_dev = g_devex;
    g_bb_w = (int)pp.BackBufferWidth; // exclusive: the display mode's size
    g_bb_h = (int)pp.BackBufferHeight;
    g_window.commit(pp);
    check_exclusive_present(pp);
    if (!pp.Windowed)
        logf("host: exclusive fullscreen %ux%u at %u Hz, vsync %s, present %s", pp.BackBufferWidth, pp.BackBufferHeight, pp.FullScreen_RefreshRateInHz,
             pp.PresentationInterval == D3DPRESENT_INTERVAL_ONE ? "on" : "off", presentation_name(g_window.present_flip()));
    else if (g_cfg.vsync) logf("host: windowed: vsync is not used (the desktop compositor shows the frames without tearing)");
    if (pp.Windowed) logf("host: present %s", presentation_name(g_window.present_flip()));
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
    // MSAA (black.ini msaa): the sample counts this card draws colour and depth with (the frame's targets)
    g_msaa_mask = 0;
    std::string ms;
    for (int n = 2; n <= 16; n++) {
        D3DMULTISAMPLE_TYPE t = (D3DMULTISAMPLE_TYPE)n;
        if (SUCCEEDED(g_d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE, t, nullptr)) &&
            SUCCEEDED(g_d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_D24S8, TRUE, t, nullptr))) {
            g_msaa_mask |= 1u << n;
            ms += (ms.empty() ? "" : ", ") + std::to_string(n) + "x";
        }
    }
    logf("host: MSAA sample counts %s", ms.empty() ? "none" : ms.c_str());
    logf("host: Direct3D 9Ex device, back buffer %dx%d on '%s'; INTZ depth textures %s", g_bb_w.load(), g_bb_h.load(), id.Description,
         g_intz ? "supported" : "NOT supported (depth-buffer reads return zero)");
    xd3d::prewarm_shaders(g_dev); // the cached shaders' objects now, not in the frame that first draws with them
    if (g_cfg.compile_port_shaders) xd3d::compile_all_port_shaders(); // tests: for the shader seed (device.cpp)
    xd3d::vsh_seed_prewarm(g_dev); // the game's vertex programs the package locates in default.xbe (vshader.cpp)
    xd3d::texture_pool_start(g_dev); // textures made ahead by shape, not in the frame an area first shows them
    if (g_cfg.ring_rotate) xd3d::draw_ring_prepare(g_dev); // the draw ring sets made and written now (draw.cpp)
    xd3d::drawwarm_replay(g_dev); // the recorded draw states drawn once, for the driver to build them now (drawwarm.cpp)
    // restyle_windowed clears the flag only once the placement succeeded (exchanged here first, a failure lost it)
    if (!g_window.actual_exclusive() && g_window.restyle_pending) restyle_windowed();
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
    bool exclusive_ok = true;
    D3DPRESENT_PARAMETERS pp = g_window.requested_exclusive() ? exclusive_params(g_pp, &exclusive_ok) : g_pp;
    if (g_window.requested_exclusive() && !exclusive_ok) {
        logf("host: exclusive display mode not available; falling back to a window");
        g_window.request(WindowMode::Windowed);
        g_window.restyle_pending = true;
        pp = g_pp;
    }
    D3DDISPLAYMODEEX mode = exclusive_mode();
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
    const bool was_exclusive = g_window.actual_exclusive();
    g_window.commit(pp);
    if (pp.Windowed && was_exclusive) g_window.restyle_pending = true; // leaving exclusive always restores the frame
    check_exclusive_present(pp);
    logf("host: device reset, %s %ux%u, vsync %s, present %s", pp.Windowed ? "windowed" : "exclusive fullscreen", pp.BackBufferWidth,
         pp.BackBufferHeight, pp.PresentationInterval == D3DPRESENT_INTERVAL_ONE ? "on" : "off", presentation_name(g_window.present_flip()));
    if (pp.Windowed && g_window.restyle_pending) restyle_windowed(); // now the window can change; it clears the flag on success
    return true;
}

} // namespace

bool create_device() { return SendMessageW(g_hwnd, WM_APP_DEVICE, kDeviceCreate, 0) != 0; }

namespace {
const char *d3d_error_name(HRESULT hr)
{
    switch ((unsigned long)hr) {
    case 0x8876086Aul: return "D3DERR_NOTAVAILABLE: the driver does not offer what was asked";
    case 0x8876086Cul: return "D3DERR_INVALIDCALL: the request was refused as invalid";
    case 0x8876017Cul: return "D3DERR_OUTOFVIDEOMEMORY: not enough video memory";
    case 0x88760868ul: return "D3DERR_DEVICELOST: the device was lost (another program has the screen, or a remote session)";
    case 0x88760870ul: return "D3DERR_DEVICEREMOVED: the graphics card was removed or its driver restarted";
    case 0x88760874ul: return "D3DERR_DEVICEHUNG: the graphics card stopped responding";
    case 0x88760827ul: return "D3DERR_DRIVERINTERNALERROR: an error inside the graphics driver";
    case 0x88760866ul: return "D3DERR_NOTFOUND: not found";
    case 0x8007000Eul: return "E_OUTOFMEMORY: out of memory";
    case 0x80004005ul: return "E_FAIL: an unspecified failure";
    case 0x80004001ul: return "E_NOTIMPL: not implemented (no Direct3D 9Ex in this system)";
    }
    return "an unlisted error";
}
} // namespace

// The message for a player when the first device could not be made (Direct3D_CreateDevice then stops the game rather
// than let it run on without one: a Wine without the 32-bit graphics libraries crashed at the first Clear, 2026-11-18).
// Which step failed, its result named, the card, and what to try for that case; Chinese first, then English.
std::string device_failure_text()
{
    const DeviceFailure &f = g_device_failure;
    typedef const char *(__cdecl * WineVersion)();
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    WineVersion wine = ntdll ? (WineVersion)GetProcAddress(ntdll, "wine_get_version") : nullptr;
    const std::string under = wine ? std::string("Wine ") + wine() : "Windows";
    char hr1[24], hr2[24];
    snprintf(hr1, sizeof hr1, "0x%08lX", (unsigned long)f.hr_hw);
    snprintf(hr2, sizeof hr2, "0x%08lX", (unsigned long)f.hr_sw);
    std::string zh = "无法启动 Direct3D 9，游戏不能运行。\n\n";
    std::string en = "Direct3D 9 could not be started, so the game cannot run.\n\n";
    if (f.runtime) {
        zh += "失败的步骤：创建 Direct3D 9 本身（Direct3DCreate9Ex），结果 " + std::string(hr1) + "（" + d3d_error_name(f.hr_hw) + "）。\n";
        en += "Failed step: making Direct3D 9 itself (Direct3DCreate9Ex), result " + std::string(hr1) + " (" + d3d_error_name(f.hr_hw) + ").\n";
    } else {
        char what[200];
        snprintf(what, sizeof what, "%ux%u %s", f.w, f.h, f.exclusive ? "exclusive fullscreen" : "windowed");
        zh += "失败的步骤：在显卡上创建绘图设备（CreateDeviceEx，" + std::string(what) + "）。\n";
        zh += "硬件顶点处理：" + std::string(hr1) + "（" + d3d_error_name(f.hr_hw) + "）\n";
        zh += "软件顶点处理：" + std::string(hr2) + "（" + d3d_error_name(f.hr_sw) + "）\n";
        en += "Failed step: making the drawing device on the graphics card (CreateDeviceEx, " + std::string(what) + ").\n";
        en += "Hardware vertex processing: " + std::string(hr1) + " (" + d3d_error_name(f.hr_hw) + ")\n";
        en += "Software vertex processing: " + std::string(hr2) + " (" + d3d_error_name(f.hr_sw) + ")\n";
        char card[400];
        if (f.adapters)
            snprintf(card, sizeof card, "%s (vendor %04lX, device %04lX, driver %u.%u.%u.%u), %u adapter(s)", f.id.Description,
                     f.id.VendorId, f.id.DeviceId, HIWORD(f.id.DriverVersion.HighPart), LOWORD(f.id.DriverVersion.HighPart),
                     HIWORD(f.id.DriverVersion.LowPart), LOWORD(f.id.DriverVersion.LowPart), f.adapters);
        else
            snprintf(card, sizeof card, "none: Direct3D 9 sees no graphics adapter");
        zh += std::string("显卡：") + card + "\n";
        en += std::string("Graphics card: ") + card + "\n";
    }
    zh += "系统：" + under + "\n\n";
    en += "System: " + under + "\n\n";
    // Wine with no 3D renderer at all (renderer=no3d/gdi, or no OpenGL / Vulkan driver it can use): the adapter is
    // wined3d's 2D-only "DirectDraw Emulation" with vendor 0 (WSL test, 2026-11-18).
    const bool wine_no3d = wine && !f.runtime && f.adapters && !f.id.VendorId && strstr(f.id.Description, "DirectDraw Emulation");
    if (wine_no3d) {
        zh += "Wine 没有找到任何可用的 3D 渲染方式：显卡显示为“WineD3D DirectDraw Emulation”（只有 2D）。可能是：\n"
              "• Wine 的 Direct3D 设置被改成了不用 3D（注册表 HKCU\\Software\\Wine\\Direct3D 的 renderer 为 no3d 或 gdi），请删除这一项；\n"
              "• 系统没有可用的 32 位 OpenGL / Vulkan 显卡驱动（见下）。\n\n";
        en += "Wine found no 3D renderer it can use: the card shows as \"WineD3D DirectDraw Emulation\" (2D only). Either:\n"
              "- Wine's Direct3D was set to no 3D (HKCU\\Software\\Wine\\Direct3D, renderer = no3d or gdi): remove that value;\n"
              "- or there is no 32-bit OpenGL / Vulkan driver for the card (below).\n\n";
    }
    if (wine) {
        zh += "在 Wine 下，Direct3D 9 要通过显卡的 OpenGL（Wine 自带的 wined3d）或 Vulkan（DXVK）驱动来画。本程序是 32 位的，"
              "需要显卡的 32 位驱动：\n"
              "• Debian / Ubuntu：sudo apt install libgl1-mesa-dri:i386 mesa-vulkan-drivers:i386（NVIDIA 专有驱动另装其 32 位库）\n"
              "• Arch：lib32-mesa、lib32-vulkan-radeon / lib32-vulkan-intel / lib32-nvidia-utils\n"
              "• Batocera / Steam Deck 等整合系统：换一个 Wine 运行器，或切换 DXVK 开关后再试\n";
        en += "Under Wine, Direct3D 9 draws through the card's OpenGL (Wine's own wined3d) or Vulkan (DXVK) driver. This program "
              "is 32-bit, so the card's 32-bit driver is needed:\n"
              "- Debian / Ubuntu: sudo apt install libgl1-mesa-dri:i386 mesa-vulkan-drivers:i386 (NVIDIA's own driver: its 32-bit libraries)\n"
              "- Arch: lib32-mesa, lib32-vulkan-radeon / lib32-vulkan-intel / lib32-nvidia-utils\n"
              "- Batocera, Steam Deck and similar: try another Wine runner, or switch DXVK on or off\n";
    } else if (f.runtime) {
        zh += "Windows 的 Direct3D 9 组件不可用。请重新安装或更新显卡驱动；在远程桌面中运行时，请改在本机上运行。\n";
        en += "Windows' Direct3D 9 is not available. Reinstall or update the graphics driver; if this is a Remote Desktop session, "
              "run the game on the PC itself.\n";
    } else if ((unsigned long)f.hr_hw == 0x8876017Cul || (unsigned long)f.hr_sw == 0x8876017Cul) {
        zh += "显存不足。请关闭其他占用显卡的程序，或在启动器的「图形」页调低渲染倍数和抗锯齿后再试。\n";
        en += "Out of video memory. Close other programs using the graphics card, or lower the render scale and anti-aliasing "
              "on the launcher's Graphics page, and try again.\n";
    } else {
        zh += "请更新或重新安装显卡驱动（从显卡厂商官网下载）；笔记本请确认独立显卡没有被禁用；在远程桌面中运行时，请改在本机上运行。\n";
        en += "Update or reinstall the graphics driver (from the card maker's site); on a laptop, check that the graphics card is "
              "not disabled; if this is a Remote Desktop session, run the game on the PC itself.\n";
    }
    return zh + "\n日志：black.log\n\n---\n\n" + en + "\nLog: black.log";
}
HRESULT test_cooperative_level() { return (HRESULT)SendMessageW(g_hwnd, WM_APP_DEVICE, kDeviceTest, 0); }
bool recreate_device() { return SendMessageW(g_hwnd, WM_APP_DEVICE, kDeviceRecreate, 0) != 0; }

IDirect3DDevice9 *dev() { return g_dev; }

// CPU texture steps over 2 ms logged (the first 20): where the render thread waited on the driver for one
static void cpu_texture_slow(const char *what, int w, int h, const LARGE_INTEGER &t0)
{
    static int logged;
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    if (ms > 2.0 && logged < 20) {
        logged++;
        logf("[d3d] CPU texture %dx%d: %s took %.1f ms, frame %u", w, h, what, ms, g_frame_count.load());
    }
}

bool CpuTexture::make(int width, int height)
{
    if (tex && w == width && h == height) return true;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    release();
    if (!g_dev || FAILED(g_dev->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
        FAILED(g_dev->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr))) {
        release();
        return false;
    }
    w = width;
    h = height;
    cpu_texture_slow("making it", w, h, t0);
    return true;
}

void *CpuTexture::lock(int *pitch)
{
    D3DLOCKED_RECT lr;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    if (!sys || FAILED(sys->LockRect(0, &lr, nullptr, 0))) return nullptr;
    cpu_texture_slow("the lock", w, h, t0);
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
bool msaa_supported(int samples) { return samples >= 2 && samples <= 16 && (g_msaa_mask >> samples & 1); }

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
        logf("host: exclusive fullscreen at the window size: the %dx%d frame scaled over %dx%d", g_frame_w, g_frame_h, g_bb_w.load(), g_bb_h.load());
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
    bool whole = g_window.actual_exclusive() && g_frame_w > 0 && g_frame_h > 0 && (g_frame_w < g_bb_w || g_frame_h < g_bb_h) && present_stretch();
    // The device decides this, not the Alt+Enter request: until ResetEx succeeds, Present still uses the old device.
    if (g_window.present_flip()) return g_dev->Present(nullptr, nullptr, nullptr, nullptr);
    RECT src = {0, 0, g_frame_w && !whole ? g_frame_w : g_bb_w, g_frame_h && !whole ? g_frame_h : g_bb_h};
    return g_dev->Present(&src, nullptr, nullptr, nullptr); // stretched to the client area (1:1 unless it outgrew the monitors)
}

bool reset_device()
{
    end_scene();
    return SendMessageW(g_hwnd, WM_APP_DEVICE, kDeviceReset, 0) != 0;
}

} // namespace host3d
