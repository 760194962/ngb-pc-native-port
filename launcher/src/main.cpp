// Ninja Gaiden Black launcher: starts the game and (in later steps) installs it, sets the display, the
// graphics card, the controls and manages the saves (project/docs/08). The window is drawn with Direct2D and
// DirectWrite in the colours and art of the project's website (launcher/art/SOURCES.md).
//
// --snap OUT.png [PAGE]: draw the window at 1180x700 into OUT.png and exit (screenshots for review).
// --extract IMAGE DEST: install from IMAGE into DEST without the window (tests); the result on the console.
#include "install.h"
#include "keys.h"
#include "saves.h"
#include "resource.h"
#include "strings.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <iphlpapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Layout, in DIPs
constexpr float kW = 1180, kH = 700, kNavW = 236;

// The website's palette (:root of akirahayabusa.github.io/ngb-hurricane-patch)
D2D1_COLOR_F rgb(uint32_t v, float a = 1.0f) { return D2D1::ColorF(v, a); }
const uint32_t kInk = 0x0b0a0d, kInk3 = 0x2a2630, kBone = 0xece7dd, kAsh = 0xa39c92, kBlood = 0xcf3640, kGold = 0xcfa650;

enum Page { kStart, kInstall, kDisplay, kGraphics, kControls, kSaves, kOnline, kAbout, kSupport, kPageCount };
const wchar_t *const kIcons[kPageCount] = {L"\xE768", L"\xE896", L"\xE7F4", L"\xE790", L"\xE7FC", L"\xE74E", L"\xE774", L"\xE946", L"\xEB52"};

template <class T> void release(T *&p)
{
    if (p) p->Release();
    p = nullptr;
}

struct App {
    HWND hwnd = nullptr;
    ID2D1Factory *d2d = nullptr;
    IDWriteFactory *dw = nullptr;
    IWICImagingFactory *wic = nullptr;
    ID2D1HwndRenderTarget *rt = nullptr;
    IWICBitmapSource *hero_src = nullptr, *texture_src = nullptr; // decoded once, made into bitmaps per target
    IWICBitmapSource *hero_ng04_src = nullptr; // the key art in the Ninja Gaiden 2004 module: its title screen
    IWICBitmapSource *pay_src[2] = {}; // the author's WeChat Pay and Alipay codes (the Support page)
    Page page = kStart;
    int hover = -1; // hit id under the mouse
    float mouse_x = -1, mouse_y = -1; // where the mouse is (DIPs; -1 outside the window): a cut text's full wording there
    int tip = -1;                     // which one shows (an index into g_tips), -1 none
    bool pressed = false;
    std::wstring dir;  // the launcher's folder
    std::wstring ini;  // launcher.ini
    std::wstring game; // default.xbe
    int module = 0;    // the game: 0 Ninja Gaiden Black, 1 Ninja Gaiden (2004) (launcher.ini module = black | ng04)
    bool game_other = false; // the game folder holds the other game (Black in the 2004 module, or the reverse)
    // Ninja Gaiden 2004's Hurricane Packs (its Install page): the installer folder chosen, what the hard disk has, the
    // last result
    std::wstring dlc_dir;
    install::DlcStatus dlc;
    std::vector<install::ContentItem> content; // what can be uninstalled (read with dlc)
    std::wstring uninstall_id, uninstall_name, uninstall_size; // the one the confirmation asks about
    bool dlc_read = false;
    std::wstring dlc_result;
    bool dlc_bad = false;

    // the install page
    std::wstring iso, dest;        // the chosen image, where it goes
    uint64_t iso_need = 0;         // bytes of its files (0: not an image)
    std::wstring status;           // the last result line
    bool status_bad = false;
    bool offer_delete = false;     // installed from iso: offer to move it to the Recycle Bin
    install::Progress prog;
    std::thread worker;
    uint64_t dest_size = 0;        // bytes already in dest (replaced by an install over them)
    std::wstring checked_game;     // the game folder last checked, and the result
    bool game_ok = false;
    std::wstring game_detail;
    install::Edition game_edition = install::Edition::None; // which supported build the check found
    int help = 0; // the "?" panel open: 0 none, else its hit id
    int ask = 0;  // the confirmation open: 0 none, else kAsk*
    // the Saves page: what is on disk (read when the page is opened and after each action), the backup to restore
    std::vector<saves::Slot> slots;
    std::vector<std::vector<saves::Slot>> same_number; // Black's slots claiming one slot number (saves.h)
    std::vector<saves::Backup> backups;
    bool saves_read = false;
    int ctl_tab = 0;      // the Controls page: 0 keyboard, 1 pad
    int capture = -1;     // a key box waiting for a key: input * 2 + slot
    int restore_index = -1;
    std::wstring save_status; // the last action's result
    bool save_status_bad = false;
    int list_open = 0; // the drop-down list open: 0 none, else its hit id (kHitResList, kHitGpu)
    int list_scroll = 0, list_scroll_max = 0; // the open list's first row and its last possible first row
    int drag = 0;         // the slider being dragged (kHitGain, kHitGamma), its value until the button is released
    float drag_value = 1.0f;
    // the Online page: what the game program found in the chosen files (--online-check), for which files; the network
    // check's result (made on a thread of its own)
    std::map<std::string, std::vector<std::string>> online;
    std::string online_for;
    std::mutex net_lock;
    std::wstring net_result;
    bool net_bad = false, net_busy = false;
    // the last import from xemu: what came over, was already here or did not fit (or why it failed)
    std::wstring import_result;
    bool import_bad = false;
    // the port's own console: why making one failed; the Insignia Setup Assistant while it runs (it registers it)
    std::wstring local_result;
    HANDLE setup_proc = nullptr; // the Setup Assistant or the dashboard (setup_is_dash) while it runs
    bool setup_is_dash = false;
    // the help panel's text scrolls when it is longer than the panel (which help it is for: a new one starts at the top)
    float help_scroll = 0, help_target = 0, help_max = 0;
    int help_for = 0;
    // the page scrolls when it is taller than the window: where it is, where the wheel sends it (it slides there), and
    // how far it can go (the page's height less the window's, found when it is drawn)
    float page_scroll = 0, scroll_target = 0, page_max = 0;
    // the InsigniaDNS address being typed in the DNS card's field (digits and dots), and what was found or wrong
    bool dns_edit = false;
    std::wstring dns_text, dns_note;
    bool dns_note_bad = false;
};
enum { kAskOverwrite = 1, kAskNonEmpty, kAskDelete, kAskReset, kAskUnsavedStart, kAskUnsavedClose, kAskRestore, kAskImport,
       kAskExport, kAskResign, kAskUninstall, kAskNewConsole, kAskRebuildConsole };
App g;

// Hit ids: 0..kPageCount-1 the navigation, then these
enum {
    kHitStart = 100, kHitLangZh, kHitLangEn, kHitPickIso, kHitPickDest, kHitInstall, kHitCancel, kHitDeleteIso, kHitPickGame,
    kHitHelpImage, kHitHelpInstalled, kHitHelpClose, kHitAskYes, kHitAskNo, kHitDefaultPath, kHitResetPage,
    kHitSave, kHitDiscard, kHitAskAlt,
    kHitHelpSet = 400, // + the setting's index in kSettingHelp: its "?"
    kHitMode = 200, kHitAspect = 210, kHitAa = 230, kHitSeam = 240, kHitFx = 250, kHitGpu = 260, kHitListClose = 269,
    kHitGain = 270, kHitGamma, kHitWin = 280, kHitVsync = 290, kHitHud = 294, kHitFsRes = 296, kHitResList = 300, kHitShadow = 320, kHitAniso = 340, kHitSysLang = 350, kHitAudioMode = 360, kHitDigital = 364, kHitShadowSoft = 370, kHitCpuMode = 380,
    kHitCtlTab = 600, kHitKey = 620, kHitPadMap = 700, // + input * 2 + slot; + button * 16 (+ 1 + row: its list)
    kHitWebsite = 490, kHitClearCache = 495, kHitOpenSaves = 500, kHitBackupNow, kHitOpenBackups, kHitPickXrevive, kHitImport, kHitExport, kHitRestore = 510,
    kHitOnline = 520, kHitPickEeprom = 530, kHitPickHdd, kHitImportXemu, kHitNewConsole, kHitRegister, kHitDashboard, kHitRebuildConsole, kHitDnsMode = 540, kHitNetCheck = 545, kHitInsignia, kHitDnsAddr, kHitDnsFind, kHitHelpRegister,
    kHitModule = 580, // + 0 Black, 1 Ninja Gaiden 2004
    kHitPickDlc = 585, kHitInstallDlc, kHitHelpDlc, kHitExportDlc,
    kHitResign = 590,
    kHitUninstall = 2000, // Ninja Gaiden 2004's content to uninstall: its drop-down list (+ 1 + row)
};
// Ids in a range must not run into the next fixed id (2.2.8's kHitClearCache once put Export on Open folder's 500).
static_assert(kHitWebsite + 5 <= kHitClearCache && kHitClearCache < kHitOpenSaves && kHitExport < kHitRestore, "button ids overlap");
constexpr int kMsaaLevels[3] = {2, 4, 8}; // the anti-aliasing list's MSAA rows (black.ini msaa)
// Render resolution: "the window's size" or 1x to 10x the game's 720x480 buffer (the game asks for 720x480 at
// CreateDevice; black.log). Widescreen widens the width by 4/3 (anamorphic); a width is at most 8192, Direct3D 9's
// texture limit (loader d3d/device.cpp setup_render_scale: 10x widescreen renders 8192x4800, r289).
constexpr int kMaxScale = 10;
// The picture shapes (black.ini widescreen + aspect): 16:9, 16:10, 21:9 (the game program widens its projection and
// keeps the HUD and menus 16:9: loader ngb_aspect.cpp, d3d/draw.cpp scale_rhw), 4:3 (widescreen = 0), and the
// monitor's own shape (init_monitor_aspect): an ultrawide is seldom exactly 21:9 (5120x2160 and 2560x1080 are 64:27,
// 3440x1440 43:18, 3840x1600 12:5), and at 21:9 one showed thin black bars at its sides (the user, 2026-10-24)
// 5-7: the common ultrawides by name, whatever this PC's monitor (a player at 5120x2160 saw only 64:27 under "Match
// the monitor" and took 3440x1440 and 3840x1600 for unsupported, 2026-10-24)
int kAspects[8][2] = {{16, 9}, {16, 10}, {21, 9}, {4, 3}, {16, 9}, {64, 27}, {43, 18}, {12, 5}};
constexpr int kAspectMonitor = 4;
constexpr int kAspectCount = 8;
const int kAspectShown[kAspectCount] = {0, 1, 2, 5, 6, 7, 3, 4}; // the list's order (index into kAspects)
bool g_monitor_aspect_ok; // within the shapes the game program takes (loader settings.cpp: 1.5:1 to 2.5:1)
int g_monitor_w, g_monitor_h;
std::wstring render_size(double scale, int aspect)
{
    // the 720x480 buffer widened to the shape: x 0.75 x aw / ah (16:9: 4/3, 4:3: 1; loader setup_render_scale)
    double k = 0.75 * kAspects[aspect][0] / kAspects[aspect][1];
    int w = (int)std::lround(std::min(720.0 * scale * k, 8192.0)), h = (int)std::lround(480.0 * scale);
    return std::to_wstring(w) + L"\u00D7" + std::to_wstring(h);
}
std::wstring aspect_name(int aspect) { return std::to_wstring(kAspects[aspect][0]) + L":" + std::to_wstring(kAspects[aspect][1]); }
// Every setting's "?": its label and the help it opens (what it does and, where it matters, what it costs;
// the figures are measurements on the Iris Xe in the chapter-1 bench, project/docs/08)
const struct {
    Str label, help;
} kSettingHelp[] = {
    {Str::DisplayMode, Str::HelpDisplayMode},
    {Str::Aspect, Str::HelpAspect},
    {Str::WindowSize, Str::HelpWindowSize},
    {Str::FsRes, Str::HelpFsRes},
    {Str::Brightness, Str::HelpBrightness},
    {Str::Gamma, Str::HelpGamma},
    {Str::Resolution, Str::HelpResolution},
    {Str::ShadowQuality, Str::HelpShadowQuality},
    {Str::ShadowSoft, Str::HelpShadowSoft},
    {Str::TexFilter, Str::HelpTexFilter},
    {Str::AntiAlias, Str::HelpAntiAlias},
    {Str::VSync, Str::HelpVSync},
    {Str::SeamFill, Str::HelpSeamFill},
    {Str::EffectsNative, Str::HelpEffectsNative},
    {Str::PerfHud, Str::HelpPerfHud},
    {Str::GpuCard, Str::HelpGpuCard},
    {Str::ConsoleCard, Str::HelpConsole},
    {Str::SavesCard, Str::HelpSavesCard},
    {Str::BackupCard, Str::HelpBackupCard},
    {Str::XreviveCard, Str::HelpXreviveCard},
    {Str::KeyboardCard, Str::HelpKeyboard},
    {Str::PadCard, Str::HelpPad},
    {Str::OnlineCard, Str::HelpOnline},
    {Str::DnsCard, Str::HelpDns}
};
constexpr int kSettingHelpCount = (int)(sizeof kSettingHelp / sizeof kSettingHelp[0]);
int help_id(Str label)
{
    for (int i = 0; i < kSettingHelpCount; i++)
        if (kSettingHelp[i].label == label) return kHitHelpSet + i;
    return 0;
}
// Texture filtering levels (1 = off, the game's own linear filtering)
const int kAnisoLevels[5] = {1, 2, 4, 8, 16};
// Shadow quality: the shadow map's scale for each level (low = the Xbox's 128x128)
const int kShadowScales[4] = {1, 4, 8, 16};
// The project's site (the author's; About page)
const wchar_t kSite[] = L"https://akirahayabusa.github.io/ngb-hurricane-patch/";
// The author's links on the About page (the user, 2026-10-22): signed "bilibili 哈雅布撒", with the release and source
// repositories (the source public since 2026-11-11, GPL) and contacts. Clicking one opens it (kHitWebsite + index).
const wchar_t *const kLinks[5] = {kSite, L"https://github.com/akirahayabusa/ngb-pc-native-port-release",
                                  L"https://github.com/akirahayabusa/ngb-pc-native-port",
                                  L"https://space.bilibili.com/20292587", L"mailto:2682557218@qq.com"};
// Brightness and gamma sliders: the loader's range (settings.cpp clamps gain and gamma to 0.5 - 3.0), 0.05 steps
constexpr float kSliderMin = 0.5f, kSliderMax = 3.0f, kSliderW = 260;
// The window sizes offered (client area) for each shape; 0 x 0 = auto (the largest that fits the screen). The
// monitor's shape: half, two thirds, three quarters and all of the monitor's height (init_monitor_aspect).
// 21:9 had 2560x1080 and 3440x1440, which are 64:27 and 43:18: the picture had bars inside such a window.
int kWins[kAspectCount][5][2] = {{{0, 0}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}},
                                 {{0, 0}, {1280, 800}, {1440, 900}, {1680, 1050}, {1920, 1200}},
                                 {{0, 0}, {1680, 720}, {2240, 960}, {2520, 1080}, {3360, 1440}},
                                 {{0, 0}, {960, 720}, {1280, 960}, {1440, 1080}, {1920, 1440}},
                                 {{0, 0}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}},
                                 {{0, 0}, {1280, 540}, {1920, 810}, {2560, 1080}, {5120, 2160}},
                                 {{0, 0}, {1290, 540}, {1720, 720}, {2580, 1080}, {3440, 1440}},
                                 {{0, 0}, {1920, 800}, {2400, 1000}, {2880, 1200}, {3840, 1600}}};

// The primary monitor's shape in pixels (not scaled by DPI), as the nearest W:H with H up to 64: the game program
// steps the window in whole multiples of H (loader d3d/host.cpp open_window), so 1366x768 is 16:9, not 683:384
// (8:5 is named 16:10, as the list's own row)
void init_monitor_aspect(int test_w = 0, int test_h = 0) // test_w x test_h: --monitor (with --snap)
{
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    if (test_w > 0 && test_h > 0) dm.dmPelsWidth = test_w, dm.dmPelsHeight = test_h;
    else if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) return;
    if (!dm.dmPelsWidth || !dm.dmPelsHeight) return;
    g_monitor_w = (int)dm.dmPelsWidth;
    g_monitor_h = (int)dm.dmPelsHeight;
    double r = (double)g_monitor_w / g_monitor_h, best = 1e9;
    for (int h = 1; h <= 64; h++) {
        int w = (int)std::lround(r * h);
        if (fabs((double)w / h - r) < best - 1e-9) best = fabs((double)w / h - r), kAspects[kAspectMonitor][0] = w, kAspects[kAspectMonitor][1] = h;
    }
    if (kAspects[kAspectMonitor][0] == 8 && kAspects[kAspectMonitor][1] == 5) kAspects[kAspectMonitor][0] = 16, kAspects[kAspectMonitor][1] = 10;
    int aw = kAspects[kAspectMonitor][0], ah = kAspects[kAspectMonitor][1];
    g_monitor_aspect_ok = aw * 10 >= ah * 15 && aw * 10 <= ah * 25;
    const int parts[4][2] = {{1, 2}, {2, 3}, {3, 4}, {1, 1}};
    for (int i = 0; i < 4; i++) {
        int h = std::max(1, (int)std::lround((double)g_monitor_h * parts[i][0] / parts[i][1] / ah)) * ah;
        kWins[kAspectMonitor][i + 1][0] = h / ah * aw;
        kWins[kAspectMonitor][i + 1][1] = h;
    }
}
constexpr UINT_PTR kTimerInstall = 1;
constexpr UINT_PTR kTimerSetup = 2; // the Insignia Setup Assistant running: look again at the console when it ends
constexpr UINT_PTR kTimerScroll = 3; // the page sliding to where the wheel sent it
struct Hit {
    int id;
    D2D1_RECT_F r;
};
std::vector<Hit> g_hits;
// One-line texts cut short (an ellipsis): where they are on the window and their whole wording, shown when the mouse
// rests on them (Painter::tip_box)
struct Tip {
    D2D1_RECT_F r;
    std::wstring s;
};
std::vector<Tip> g_tips;

bool inside(const D2D1_RECT_F &r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// --- settings ---------------------------------------------------------------------------------------------------

std::string black_ini(const char *key);
std::wstring save_hdd();
int aspect_index(bool saved = false);
bool game_running();
uint16_t kb_key(int input, int slot);
int pad_source(int button);
void refresh_saves();
float slider_value(int id)
{
    if (g.drag == id) return g.drag_value;
    std::string v = black_ini(id == kHitGain ? "gain" : "gamma");
    return v.empty() ? 1.0f : (float)atof(v.c_str());
}

std::wstring ini_get(const wchar_t *key, const wchar_t *def = L"")
{
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"launcher", key, def, buf, 1024, g.ini.c_str());
    return buf;
}

void ini_set(const wchar_t *key, const std::wstring &v) { WritePrivateProfileStringW(L"launcher", key, v.c_str(), g.ini.c_str()); }

std::wstring game_exe() { return g.dir + L"\\Ninja Gaiden Black Any.exe"; }

// The default game folder: game next to the launcher (the release layout; the game program looks there too when
// double-clicked). 0.1.0 used game\x21: a launcher.ini from then keeps its path.
bool ng04() { return g.module == 1; } // the Ninja Gaiden 2004 module
// launcher.ini's key for the game's default.xbe, and its default folder: each game its own (2004: game_ng04)
const wchar_t *game_key() { return ng04() ? L"game_ng04" : L"game"; }
std::wstring default_game_folder() { return g.dir + (ng04() ? L"\\game_ng04" : L"\\game"); }
std::wstring save_hdd(); // the Xbox hard disk (below)

bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// black.ini next to the game program: one "key = value" per line
// The settings pages change these, not the files: black.ini and the graphics card preference are written only when
// the player saves (the user, 2026-10-22). black_ini / gpu_preference give the page's value (changed or saved).
std::map<std::string, std::string> g_pending; // black.ini key -> value not saved yet
int g_pending_gpu = -1;                       // a graphics card preference not saved yet
bool unsaved() { return !g_pending.empty() || g_pending_gpu >= 0; }

// black.ini as saved
std::string black_ini_saved(const char *key)
{
    FILE *f = _wfopen((g.dir + L"\\black.ini").c_str(), L"r");
    if (!f) return "";
    char line[1024];
    std::string v;
    size_t n = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, n) && (line[n] == ' ' || line[n] == '=')) {
            const char *eq = strchr(line, '=');
            if (eq) {
                v = eq + 1;
                v.erase(0, v.find_first_not_of(" \t"));
                v.erase(v.find_last_not_of(" \t\r\n") + 1);
            }
        }
    }
    fclose(f);
    return v;
}

std::string black_ini(const char *key)
{
    auto it = g_pending.find(key);
    return it != g_pending.end() ? it->second : black_ini_saved(key);
}

// Sets key = value in black.ini, keeping every other line (the game writes the file on exit too)
void black_ini_write(const char *key, const std::string &value)
{
    std::wstring path = g.dir + L"\\black.ini";
    std::vector<std::string> lines;
    FILE *f = _wfopen(path.c_str(), L"r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            std::string l = line;
            l.erase(l.find_last_not_of("\r\n") + 1);
            lines.push_back(l);
        }
        fclose(f);
    }
    size_t n = strlen(key);
    bool done = false;
    for (std::string &l : lines)
        if (!l.compare(0, n, key) && l.size() > n && (l[n] == ' ' || l[n] == '=')) {
            l = std::string(key) + " = " + value;
            done = true;
        }
    if (!done) lines.push_back(std::string(key) + " = " + value);
    f = _wfopen(path.c_str(), L"w");
    if (!f) return;
    for (const std::string &l : lines) fprintf(f, "%s\n", l.c_str());
    fclose(f);
}

// A change on a settings page: kept until saved (no change when it is the saved value)
void black_ini_set(const char *key, const std::string &value)
{
    if (value == black_ini_saved(key)) g_pending.erase(key);
    else g_pending[key] = value;
}

void write_gpu_preference(int v) // 2 high performance, 1 power saving, 0 let Windows decide
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &k, nullptr))
        return;
    wchar_t s[32];
    swprintf(s, 32, L"GpuPreference=%d;", v);
    RegSetValueExW(k, game_exe().c_str(), 0, REG_SZ, (const BYTE *)s, (DWORD)((wcslen(s) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

// Windows' per-program graphics preference for the game (the game sets "high performance" on its first run)
int gpu_preference_saved()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, KEY_QUERY_VALUE, &k)) return -1;
    wchar_t v[64] = L"";
    DWORD size = sizeof v - sizeof(wchar_t), type;
    LONG r = RegQueryValueExW(k, game_exe().c_str(), nullptr, &type, (BYTE *)v, &size);
    RegCloseKey(k);
    if (r) return -1;
    const wchar_t *p = wcsstr(v, L"GpuPreference=");
    return p ? _wtoi(p + 14) : -1;
}

int gpu_preference() { return g_pending_gpu >= 0 ? g_pending_gpu : gpu_preference_saved(); }
void set_gpu_preference(int v) { g_pending_gpu = v == gpu_preference_saved() ? -1 : v; }

// Save: the settings pages' changes into black.ini and Windows' preference; discard: forget them
void save_settings()
{
    for (auto &kv : g_pending) black_ini_write(kv.first.c_str(), kv.second);
    g_pending.clear();
    if (g_pending_gpu >= 0) write_gpu_preference(g_pending_gpu);
    g_pending_gpu = -1;
}
void discard_settings()
{
    g_pending.clear();
    g_pending_gpu = -1;
}

// The graphics cards the game can be given: Windows' per-program preference picks the high performance card, the
// power saving card, or leaves it to the system (DXGI 1.6 EnumAdapterByGpuPreference says which card each gives).
// With one card (a desktop with one GPU) high performance and power saving are the same card, listed once. A third
// card that no preference reaches cannot be chosen per program, so it is not offered. Read once.
struct GpuChoice {
    std::wstring name;
    int pref; // GpuPreference: 2 high performance, 1 power saving, 0 let Windows decide
};
const std::vector<GpuChoice> &gpu_choices()
{
    static std::vector<GpuChoice> v;
    static bool done;
    if (done) return v;
    done = true;
    IDXGIFactory6 *f6 = nullptr;
    IDXGIFactory1 *f = nullptr;
    std::wstring name[3];
    LUID luid[3] = {};
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) && SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&f6)))) {
        const DXGI_GPU_PREFERENCE prefs[3] = {DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, DXGI_GPU_PREFERENCE_MINIMUM_POWER,
                                              DXGI_GPU_PREFERENCE_UNSPECIFIED};
        for (int i = 0; i < 3; i++) {
            IDXGIAdapter1 *a = nullptr;
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(f6->EnumAdapterByGpuPreference(0, prefs[i], IID_PPV_ARGS(&a))) && SUCCEEDED(a->GetDesc1(&d))) {
                name[i] = d.Description;
                luid[i] = d.AdapterLuid;
            }
            release(a);
        }
    }
    release(f6);
    release(f);
    if (!name[0].empty()) v.push_back({name[0], 2});
    if (!name[1].empty() && (luid[1].LowPart != luid[0].LowPart || luid[1].HighPart != luid[0].HighPart))
        v.push_back({name[1], 1});
    v.push_back({name[2], 0});
    return v;
}

// The choice in effect: not set yet is high performance (the game sets that on its first run)
int gpu_choice_index(bool saved = false)
{
    const std::vector<GpuChoice> &v = gpu_choices();
    int pref = saved ? gpu_preference_saved() : gpu_preference();
    if (pref < 0) pref = 2;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].pref == pref) return (int)i;
    return 0; // power saving with a single card: that card
}

// "Let Windows decide · Intel ..." / the card's name
std::wstring gpu_label(const GpuChoice &ch)
{
    if (ch.pref) return ch.name;
    return std::wstring(S(Str::GpuWindows)) + (ch.name.empty() ? L"" : L"  \x00B7  " + ch.name);
}

void launch_game()
{
    if (!exists(game_exe()) || !exists(g.game) || g.game_other) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    // launcher.ini: hdd = the Xbox hard disk (default savedata next to the program); args = extra options for the game
    // (development: a log and a command file elsewhere), which then replace the default log
    std::wstring hdd = ini_get(L"hdd", (g.dir + L"\\savedata").c_str()), args = ini_get(L"args");
    std::wstring cmd = L"\"" + game_exe() + L"\" --hdd \"" + hdd + L"\" "; // display settings come from black.ini
    if (args.empty()) {
        CreateDirectoryW((g.dir + L"\\logs").c_str(), nullptr);
        cmd += L"--log \"" + g.dir + L"\\logs\\black.log\" ";
    } else {
        cmd += args + L" ";
    }
    cmd += L"\"" + g.game + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g.dir.c_str(), &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId); // the game's window comes to the front (d3d/host.cpp ui_thread)
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        PostQuitMessage(0); // the game's window takes over
    }
}

// --- install --------------------------------------------------------------------------------------------------------

std::wstring folder_of(const std::wstring &p) { return p.substr(0, p.find_last_of(L"\\/")); }

// The game folder's check (SHA-256 of default.xbe and its programs), redone only when the folder changes
void check_game()
{
    std::wstring dir = folder_of(g.game);
    if (dir == g.checked_game) return;
    g.checked_game = dir;
    g.game_ok = exists(g.game) && install::check_game(dir, &g.game_detail, &g.game_edition);
    if (!exists(g.game)) g.game_detail.clear();
    g.game_other = g.game_ok && (g.game_edition == install::Edition::Ng04) != ng04();
    if (g.game_other) g.game_ok = false;
}

// A folder or a file (filter: the file types offered first, default disc images)
bool pick(bool folder, std::wstring *out, const COMDLG_FILTERSPEC *filter = nullptr)
{
    IFileOpenDialog *d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
    DWORD opt = 0;
    d->GetOptions(&opt);
    d->SetOptions(opt | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    COMDLG_FILTERSPEC spec[] = {filter ? *filter : COMDLG_FILTERSPEC{S(Str::ImageFilter), L"*.iso"}, {L"*.*", L"*.*"}};
    if (!folder) d->SetFileTypes(2, spec);
    bool ok = false;
    IShellItem *item = nullptr;
    PWSTR path = nullptr;
    if (SUCCEEDED(d->Show(g.hwnd)) && SUCCEEDED(d->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        *out = path;
        ok = true;
    }
    if (path) CoTaskMemFree(path);
    release(item);
    release(d);
    return ok;
}

uint64_t free_space(std::wstring dir)
{
    ULARGE_INTEGER avail;
    while (!dir.empty()) {
        if (GetDiskFreeSpaceExW(dir.c_str(), &avail, nullptr, nullptr)) return avail.QuadPart;
        size_t s = dir.find_last_of(L"\\/");
        if (s == std::wstring::npos) break;
        dir.resize(s);
    }
    return 0;
}

bool installing() { return g.prog.state == 1; }

void choose_image()
{
    std::wstring p;
    if (installing() || !pick(false, &p)) return;
    g.iso = p;
    std::wstring err;
    g.iso_need = install::image_size(p, &err);
    g.status = g.iso_need ? L"" : S(Str::NotAnImage);
    g.status_bad = !g.iso_need;
    g.offer_delete = false;
}

// Bytes in a folder (all files below it)
uint64_t folder_size(const std::wstring &dir)
{
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += folder_size(dir + L"\\" + fd.cFileName);
        else total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

bool folder_empty(const std::wstring &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool empty = true;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) empty = false;
    } while (empty && FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

// The space the install still needs: an existing copy's files are replaced in place
uint64_t space_needed() { return g.iso_need > g.dest_size ? g.iso_need - g.dest_size : 0; }

void start_install();

void set_dest(const std::wstring &d)
{
    g.dest = d;
    g.dest_size = folder_size(d);
}

// "Install" pressed: ask first when the folder already holds a game or other files
void request_install()
{
    if (installing() || !g.iso_need || free_space(g.dest) < space_needed()) return;
    std::wstring detail;
    if (exists(g.dest + L"\\default.xbe") && install::check_game(g.dest, &detail)) g.ask = kAskOverwrite;
    else if (!folder_empty(g.dest)) g.ask = kAskNonEmpty;
    else start_install();
}

void start_install()
{
    if (installing() || !g.iso_need || free_space(g.dest) < space_needed()) return;
    if (g.worker.joinable()) g.worker.join();
    g.prog.done = 0;
    g.prog.files = 0;
    g.prog.cancel = false;
    g.prog.state = 1;
    g.status.clear();
    g.offer_delete = false;
    std::wstring iso = g.iso, dest = g.dest;
    g.worker = std::thread([iso, dest] { install::extract(iso, dest, g.prog); });
    SetTimer(g.hwnd, kTimerInstall, 100, nullptr);
}

// The worker stopped (timer): take the result
void install_finished()
{
    KillTimer(g.hwnd, kTimerInstall);
    if (g.worker.joinable()) g.worker.join();
    int st = g.prog.state;
    if (st == 2) {
        g.game = g.dest + L"\\default.xbe";
        ini_set(game_key(), g.game);
        g.checked_game.clear();
        check_game();
        g.status = S(Str::InstallDone);
        g.status_bad = false;
        g.offer_delete = true;
    } else if (st == 4) {
        g.status = S(Str::InstallCancelled);
        g.status_bad = false;
    } else {
        std::lock_guard<std::mutex> l(g.prog.lock);
        g.status = std::wstring(S(Str::InstallFailed)) + g.prog.message;
        g.status_bad = true;
    }
}

void delete_image()
{
    if (!g.offer_delete) return;
    std::wstring from = g.iso;
    from.push_back(L'\0'); // a double-null-terminated list
    SHFILEOPSTRUCTW op = {};
    op.hwnd = g.hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT; // to the Recycle Bin: the player can take it back
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) {
        g.status = S(Str::ImageDeleted);
        g.offer_delete = false;
        g.iso.clear();
        g.iso_need = 0;
    }
}

// About: the shader cache can grow over upgrades; deleting it is safe (the next start compiles again).
void clear_shader_cache()
{
    std::wstring dir = g.dir + L"\\cache\\shaders";
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g.hwnd, S(Str::ClearCacheDone), S(Str::ClearCache), MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxW(g.hwnd, S(Str::ClearCacheConfirm), S(Str::ClearCache), MB_YESNO | MB_ICONWARNING) != IDYES) return;
    std::wstring from = dir;
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op = {};
    op.hwnd = g.hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    bool ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    MessageBoxW(g.hwnd, ok ? S(Str::ClearCacheDone) : S(Str::ClearCacheFailed), S(Str::ClearCache),
                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
}

// The game folder ("Change"): where an install goes and where the game starts from
void set_game_folder(const std::wstring &dir)
{
    g.game = dir + L"\\default.xbe";
    ini_set(game_key(), g.game);
    set_dest(dir);
    g.checked_game.clear();
    check_game();
}

void update_title(); // the window's title (below)

// The game shown (the switch under the brand): its folder, check, saves and install page
void switch_module(int m)
{
    if (m == g.module || installing()) return;
    g.module = m;
    ini_set(L"module", m ? L"ng04" : L"black");
    saves::set_title(m ? L"54430003" : L"5443000d");
    update_title();
    g.game = ini_get(game_key(), (default_game_folder() + L"\\default.xbe").c_str());
    set_dest(folder_of(g.game));
    g.checked_game.clear();
    check_game();
    g.iso.clear();
    g.iso_need = 0;
    g.status.clear();
    g.offer_delete = false;
    g.saves_read = false;
    g.save_status.clear();
    g.restore_index = -1;
    g.dlc_read = false;
    g.dlc_result.clear();
    g.dlc_dir.clear();
    g.list_open = 0;
    g.help = 0;
}

// Ninja Gaiden 2004: the Hurricane Packs' installer content onto the Xbox hard disk (install.cpp install_dlc)
void install_dlc_now()
{
    std::wstring err;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = install::install_dlc(g.dlc_dir, save_hdd(), &err);
    SetCursor(old);
    g.dlc_read = false;
    g.dlc_result = ok ? std::wstring(S(Str::DlcDone)) : std::wstring(S(Str::DlcFailed)) + err;
    g.dlc_bad = !ok;
}

std::wstring content_size_text(uint64_t bytes)
{
    wchar_t b[32];
    if (bytes >= (1ull << 20)) swprintf(b, 32, L"%.1f MB", bytes / 1048576.0);
    else swprintf(b, 32, L"%u KB", (unsigned)((bytes + 1023) / 1024));
    return b;
}

// Ninja Gaiden 2004: everything installed, into a folder another player installs from (install.cpp export_dlc)
void export_dlc_now()
{
    std::wstring dir, made, err;
    if (!pick(true, &dir)) return;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = install::export_dlc(save_hdd(), dir, &made, &err);
    SetCursor(old);
    wchar_t b[1024];
    swprintf(b, 1024, S(ok ? Str::DlcExported : Str::DlcExportFailed), ok ? made.c_str() : err.c_str());
    g.dlc_result = b;
    g.dlc_bad = !ok;
}

// Ninja Gaiden 2004: one installed item off the Xbox hard disk (the update, a pack, content the game downloaded)
void uninstall_now()
{
    std::wstring err;
    bool ok = install::remove_content(save_hdd(), g.uninstall_id, &err);
    g.dlc_read = false;
    wchar_t b[1024];
    swprintf(b, 1024, S(ok ? Str::UninstallDone : Str::UninstallFailed), ok ? g.uninstall_name.c_str() : err.c_str());
    g.dlc_result = b;
    g.dlc_bad = !ok;
}

void choose_existing()
{
    std::wstring dir;
    if (!installing() && pick(true, &dir)) set_game_folder(dir);
}

// --- the Online page -----------------------------------------------------------------------------------------------
// black.ini online = 1 with online_eeprom / online_hdd (the xemu files the player registered with Insignia) and
// online_dns_mode (insignia / insigniadns with online_insigniadns; the older console): the game program's
// net/online.cpp and net/vnet.cpp, project/docs/09.

std::wstring from_utf8(const std::string &s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], (int)w.size());
    return w;
}

std::string to_utf8(const std::wstring &w)
{
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
    if (!s.empty()) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], (int)s.size(), nullptr, nullptr);
    return s;
}

// What the game program finds in the chosen files (--online-check: whether the EEPROM verifies, the disk has a
// registered console and which gamertags, the console's DNS setting, whether xemu runs; nothing secret), asked again
// only when the files change. One "key = value" a line; a key may repeat (account).
// Runs the game program with the xemu files chosen here (black.ini online_eeprom / online_hdd, maybe not saved yet)
// and one of its online options writing "key = value" lines to cache\<name>; those lines, empty if it did not run
std::map<std::string, std::vector<std::string>> online_tool(const wchar_t *option, const wchar_t *name, DWORD wait_ms)
{
    std::map<std::string, std::vector<std::string>> r;
    std::string ee = black_ini("online_eeprom"), hdd = black_ini("online_hdd");
    CreateDirectoryW((g.dir + L"\\cache").c_str(), nullptr);
    std::wstring out = g.dir + L"\\cache\\" + name;
    DeleteFileW(out.c_str());
    // the save folder the game will be started with (launch_game): the port's own console is there
    std::wstring cmd = L"\"" + game_exe() + L"\" --background --hdd \"" + ini_get(L"hdd", (g.dir + L"\\savedata").c_str()) + L"\"";
    if (!ee.empty()) cmd += L" --online-eeprom \"" + from_utf8(ee) + L"\"";
    if (!hdd.empty()) cmd += L" --online-hdd \"" + from_utf8(hdd) + L"\"";
    cmd += std::wstring(L" ") + option + L" \"" + out + L"\" \"" + g.game + L"\""; // option may carry an argument before out
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g.dir.c_str(), &si, &pi)) return r;
    WaitForSingleObject(pi.hProcess, wait_ms);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    FILE *f = _wfopen(out.c_str(), L"r");
    if (!f) return r;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        size_t eq = l.find(" = ");
        if (eq == std::string::npos) continue;
        l.erase(l.find_last_not_of("\r\n") + 1);
        r[l.substr(0, eq)].push_back(l.substr(eq + 3));
    }
    fclose(f);
    return r;
}

// The port's own console in the save folder and what the chosen xemu files hold (--online-check: whether the EEPROM
// verifies, the disk has a registered console and which gamertags, which of them are not here yet, the DNS setting,
// whether xemu runs; nothing secret), asked again only when the files change or after an import
void online_check()
{
    std::string key = black_ini("online_eeprom") + "|" + black_ini("online_hdd");
    if (key == g.online_for) return;
    g.online_for = key;
    g.online = online_tool(L"--online-check", L"online_check.txt", 10000);
}

// "Import from xemu": the xemu console's accounts the save folder does not have yet (the game program's
// --online-import; the first time the console itself). The result stays on the page until the next import.
void online_import()
{
    auto r = online_tool(L"--online-import", L"online_import.txt", 60000);
    auto names = [&](const char *k) {
        std::wstring s;
        for (const std::string &n : r[k]) s += (s.empty() ? L"" : L", ") + from_utf8(n);
        return s;
    };
    g.import_bad = r.empty() || r.count("error");
    if (r.empty()) g.import_result = std::wstring(S(Str::OnlineImportFailed)) + S(Str::OnlineCheckFailed);
    else if (r.count("error")) g.import_result = S(Str::OnlineImportFailed) + names("error");
    else {
        std::wstring s;
        auto part = [&](Str label, const char *k) {
            if (!r.count(k)) return;
            s += (s.empty() ? L"" : L"\uFF1B") + std::wstring(S(label)) + names(k);
        };
        part(Str::OnlineImported, "imported");
        part(Str::OnlineExists, "exists");
        part(Str::OnlineFull, "full");
        part(Str::OnlineDamaged, "damaged");
        g.import_result = s.empty() ? S(Str::OnlineNothing) : s;
        g.import_bad = r.count("full") && !r.count("imported");
    }
    g.online_for.clear(); // look again: the accounts here changed
}

// "New console": a console of the port's own in the save folder (the game program's --online-new-console), not yet
// registered with Insignia; only when there is none (asked first: kAskNewConsole)
void new_console()
{
    auto r = online_tool(L"--online-new-console", L"online_new.txt", 15000);
    g.local_result.clear();
    if (!r.count("created")) g.local_result = std::wstring(S(Str::NewConsoleFailed)) + (r.count("error") ? from_utf8(r["error"][0]) : S(Str::OnlineCheckFailed));
    g.online_for.clear();
}

// "Rebuild": the console's files damaged or not matching (no import can mend that without xemu): they are kept as
// *.old in the save folder (an earlier *.old replaced) and a new console is made, to be registered again
void rebuild_console()
{
    std::wstring hdd = ini_get(L"hdd", (g.dir + L"\\savedata").c_str());
    for (const wchar_t *f : {L"online_eeprom.bin", L"online_accounts.bin", L"online_accounts.bak", L"online_registered.txt"}) {
        std::wstring p = hdd + L"\\" + f;
        if (exists(p)) MoveFileExW(p.c_str(), (p + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    new_console();
}

// The Insignia Setup Assistant the player gave (cache\insignia\default.xbe, taken from Insignia-Setup.iso): its title ID
// is FFFF0002 (the certificate's, at 8)
std::wstring setup_xbe() { return g.dir + L"\\cache\\insignia\\default.xbe"; }
bool is_setup_assistant(const std::wstring &xbe)
{
    FILE *f = _wfopen(xbe.c_str(), L"rb");
    if (!f) return false;
    uint8_t h[0x1000];
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < 0x180 || memcmp(h, "XBEH", 4)) return false;
    uint32_t base, cert, id;
    memcpy(&base, h + 0x104, 4);
    memcpy(&cert, h + 0x118, 4);
    if (cert < base || cert - base + 12 > n) return false;
    memcpy(&id, h + cert - base + 8, 4);
    return id == 0xFFFF0002;
}

// "Register": the Insignia Setup Assistant on the port's own console, online (whatever the switch: registering needs
// the network), with the DNS chosen here; its Register Xbox writes the machine account the console needs (the game
// program keeps it in the save folder). Asked for once (the ISO or its default.xbe), kept in cache\insignia.
void register_console()
{
    if (g.setup_proc || game_running()) return;
    g.local_result.clear();
    std::wstring xbe = setup_xbe();
    if (!is_setup_assistant(xbe)) {
        COMDLG_FILTERSPEC spec = {S(Str::InsigniaFilter), L"*.iso;*.xbe"};
        std::wstring f;
        if (!pick(false, &f, &spec)) return;
        std::wstring dir = g.dir + L"\\cache\\insignia";
        CreateDirectoryW((g.dir + L"\\cache").c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        if (f.size() > 4 && !_wcsicmp(f.c_str() + f.size() - 4, L".xbe")) CopyFileW(f.c_str(), xbe.c_str(), FALSE);
        else {
            install::Progress p;
            install::extract(f, dir, p); // its own files; the check for the game's that follows fails, as it should
        }
        if (!is_setup_assistant(xbe)) {
            DeleteFileW(xbe.c_str());
            g.local_result = S(Str::NotSetupAssistant);
            return;
        }
    }
    std::wstring hdd = ini_get(L"hdd", (g.dir + L"\\savedata").c_str());
    std::wstring cmd = L"\"" + game_exe() + L"\" --hdd \"" + hdd + L"\" --online --force";
    std::string mode = black_ini("online_dns_mode"), addr = black_ini("online_insigniadns");
    if (!mode.empty()) cmd += L" --online-dns-mode " + from_utf8(mode);
    if (!addr.empty()) cmd += L" --online-insigniadns " + from_utf8(addr);
    CreateDirectoryW((g.dir + L"\\logs").c_str(), nullptr);
    cmd += L" --log \"" + g.dir + L"\\logs\\insignia-setup.log\" \"" + xbe + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g.dir.c_str(), &si, &pi)) return;
    CloseHandle(pi.hThread);
    g.setup_proc = pi.hProcess;
    g.setup_is_dash = false;
    SetTimer(g.hwnd, kTimerSetup, 500, nullptr);
}

// "Xbox Live": the Xbox Live dashboard on the port's own console, online, at its XBOX LIVE menu (NETWORK SETUP's
// connection test makes the machine account; NEW ACCOUNT, ACCOUNT MANAGEMENT). Its files come once from an Xbox hard
// disk image: xemu's chosen here, else one the player picks (the game program's --online-dash-from checks the build).
std::string dns_server(bool *pc);
bool dns_ask(const std::string &server, bool pc, std::wstring *result);

void open_dashboard()
{
    if (g.setup_proc || game_running()) return;
    g.local_result.clear();
    // Its network test needs the DNS to give Insignia's servers: with a proxy's TUN on, Insignia's public DNS is
    // captured and the test makes no machine account, though registering worked (HTTPS goes through the proxy)
    {
        bool pc = false;
        std::wstring why;
        if (dns_ask(dns_server(&pc), pc, &why)) {
            g.local_result = std::wstring(S(Str::DashNoDns)) + why;
            return;
        }
    }
    std::wstring hdd = ini_get(L"hdd", (g.dir + L"\\savedata").c_str());
    std::wstring xbe = hdd + L"\\Partition2\\xodash\\xonlinedash.xbe";
    // its sounds are the main dashboard's C:\Audio (silent without them; 2.2.0-2.2.2 did not take them): taken with the
    // rest, or alone for a dashboard set up before. Asked for once then (an image again); not given, it runs silent.
    const bool need_files = !exists(xbe);
    const bool need_audio = !exists(hdd + L"\\Partition2\\Audio\\mainaudio\\Global A Button Select.wav") &&
                            (need_files || black_ini("online_dash_audio_asked") != "1");
    if (need_files || need_audio) {
        std::wstring image = from_utf8(black_ini("online_hdd"));
        if (!need_files) black_ini_set("online_dash_audio_asked", "1");
        if (image.empty() || !exists(image)) {
            // an image, or in a copy of an Xbox's C: (FTP from a real Xbox) its xboxdash.xbe or xodash\xonlinedash.xbe:
            // the game program takes the three from that folder (net/dashfonts.cpp dash_from_folder)
            COMDLG_FILTERSPEC spec = {S(Str::DashFilter), L"*.qcow2;*.img;*.bin;*.raw;xboxdash.xbe;xonlinedash.xbe"};
            if (!pick(false, &image, &spec)) {
                if (need_files) return;
                image.clear(); // no sounds this time: open it silent
            }
            size_t slash = image.find_last_of(L"\\/");
            std::wstring name = slash == std::wstring::npos ? image : image.substr(slash + 1);
            if (!_wcsicmp(name.c_str(), L"xboxdash.xbe")) image.resize(slash);
            else if (!_wcsicmp(name.c_str(), L"xonlinedash.xbe")) {
                image.resize(slash);                      // ...\xodash
                image.resize(image.find_last_of(L"\\/")); // ...: the copy of C:
            }
        }
        auto r = image.empty() ? decltype(online_tool(L"", L"", 0)){}
                               : online_tool((L"--online-dash-from \"" + image + L"\"").c_str(), L"online_dash.txt", 120000);
        if (need_files && (!r.count("dashboard") || r["dashboard"][0] != "ok" || !exists(xbe))) {
            g.local_result = S(Str::DashMissing);
            return;
        }
    }
    // the launch data of a title asking for its XBOX LIVE menu (LDT_LAUNCH_DASHBOARD 1, reason 0xC; loader kernel/k_misc.cpp
    // dashboard_open): B at that menu ends the dashboard, back here. With the main dashboard's (type 2, title FFFE0000)
    // B there did nothing: it would go back to the main dashboard, which the port does not run (a player, 2026-11-05)
    std::vector<uint8_t> page(4096, 0);
    uint32_t type = 1, title = 0x5443000D, menu = 0xC; // the title: Ninja Gaiden Black's (any but the dashboard's)
    memcpy(&page[0], &type, 4);
    memcpy(&page[4], &title, 4);
    strcpy((char *)&page[8], "\\Device\\Harddisk0\\Partition2\\XODash;xonlinedash.xbe");
    memcpy(&page[0x400], &menu, 4);
    CreateDirectoryW((g.dir + L"\\cache").c_str(), nullptr);
    std::wstring page_file = g.dir + L"\\cache\\dashboard.launchdata";
    if (FILE *f = _wfopen(page_file.c_str(), L"wb")) {
        fwrite(page.data(), 1, page.size(), f);
        fclose(f);
    } else return;
    std::wstring cmd = L"\"" + game_exe() + L"\" --hdd \"" + hdd + L"\" --online";
    std::string mode = black_ini("online_dns_mode"), addr = black_ini("online_insigniadns");
    if (!mode.empty()) cmd += L" --online-dns-mode " + from_utf8(mode);
    if (!addr.empty()) cmd += L" --online-insigniadns " + from_utf8(addr);
    CreateDirectoryW((g.dir + L"\\logs").c_str(), nullptr);
    cmd += L" --log \"" + g.dir + L"\\logs\\dashboard.log\" --launch-data \"" + page_file + L"\" \"" + xbe + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g.dir.c_str(), &si, &pi)) return;
    CloseHandle(pi.hThread);
    g.setup_proc = pi.hProcess;
    g.setup_is_dash = true;
    SetTimer(g.hwnd, kTimerSetup, 500, nullptr);
}

std::string online_value(const char *key)
{
    auto it = g.online.find(key);
    return it == g.online.end() || it->second.empty() ? "" : it->second[0];
}

// The lookups go to a PC running InsigniaDNS: online_dns_mode = insigniadns, or the first launcher's console (the
// console's manual DNS, which names a PC as well)
bool dns_by_pc()
{
    std::string m = black_ini("online_dns_mode");
    return m == "insigniadns" || m == "console";
}

// A dotted IPv4 address, written as Windows writes it back (no "1.2.3", no leading zeros)
bool ipv4(const std::string &s)
{
    in_addr a;
    char b[16];
    return inet_pton(AF_INET, s.c_str(), &a) == 1 && inet_ntop(AF_INET, &a, b, sizeof b) && s == b;
}

// The DNS server the game will ask first: Insignia's (black.ini online_dns, default 46.101.64.175), with
// online_dns_mode = insigniadns online_insigniadns (the game program falls back to Insignia's when it is no address),
// with console the console's first manual server
std::string dns_server(bool *pc)
{
    std::string relay = black_ini("online_dns");
    if (relay.empty()) relay = "46.101.64.175";
    std::string mode = black_ini("online_dns_mode"), addr = black_ini("online_insigniadns");
    *pc = true;
    if (mode == "insigniadns" && ipv4(addr)) return addr;
    if (mode == "console" && online_value("dns") == "manual" && !online_value("dns1").empty()) return online_value("dns1");
    *pc = false;
    return relay;
}

// This PC's InsigniaDNS: a program on UDP port 53 at one of the PC's addresses (InsigniaDNS listens on the LAN address
// only, not on 127.0.0.1: the author's PC, 2026-10-23), else the address of the connection with a gateway (Wi-Fi or
// Ethernet; a proxy's TUN adapter is neither). Returns whether something listens on port 53.
bool find_insigniadns(std::string *addr)
{
    addr->clear();
    bool any = false; // a program on 0.0.0.0:53 (every address)
    char b[16];
    ULONG size = 0;
    GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    std::vector<uint8_t> buf(size + 1024);
    size = (ULONG)buf.size();
    if (GetExtendedUdpTable(buf.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
        const auto *t = (const MIB_UDPTABLE_OWNER_PID *)buf.data();
        for (DWORD i = 0; i < t->dwNumEntries && addr->empty(); i++) {
            const MIB_UDPROW_OWNER_PID &r = t->table[i];
            if (ntohs((u_short)r.dwLocalPort) != 53) continue;
            in_addr a;
            a.s_addr = r.dwLocalAddr;
            if (!r.dwLocalAddr) any = true;
            else if (ntohl(r.dwLocalAddr) >> 24 != 127 && inet_ntop(AF_INET, &a, b, sizeof b)) *addr = b;
        }
    }
    if (!addr->empty()) return true;
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    size = 0;
    GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size);
    std::vector<uint64_t> abuf(size / 8 + 1);
    size = (ULONG)(abuf.size() * 8);
    auto *ad = (IP_ADAPTER_ADDRESSES *)abuf.data();
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, ad, &size) == NO_ERROR)
        for (; ad && addr->empty(); ad = ad->Next) {
            if (ad->OperStatus != IfOperStatusUp || !ad->FirstGatewayAddress ||
                (ad->IfType != IF_TYPE_ETHERNET_CSMACD && ad->IfType != IF_TYPE_IEEE80211))
                continue;
            for (auto *u = ad->FirstUnicastAddress; u && addr->empty(); u = u->Next) {
                const auto *sin = (const sockaddr_in *)u->Address.lpSockaddr;
                if (sin->sin_family == AF_INET && ntohl(sin->sin_addr.s_addr) >> 16 != 0xA9FE &&
                    inet_ntop(AF_INET, &sin->sin_addr, b, sizeof b))
                    *addr = b;
            }
        }
    return any && !addr->empty();
}

// "This PC": the address found goes into the field, with whether InsigniaDNS seems to run
void dns_find()
{
    std::string a;
    bool found = find_insigniadns(&a);
    g.dns_edit = false;
    g.dns_note_bad = !found;
    if (a.empty()) {
        g.dns_note = S(Str::DnsFindNone);
        return;
    }
    black_ini_set("online_insigniadns", a);
    g.dns_note = S(found ? Str::DnsFound : Str::DnsFoundNoListener);
}

// The field typed in: kept (Enter, a click elsewhere) when it is an address, else what it was stays
void dns_edit_end(bool keep)
{
    if (!g.dns_edit) return;
    g.dns_edit = false;
    std::string v = to_utf8(g.dns_text);
    if (!keep || v == black_ini("online_insigniadns")) return;
    if (ipv4(v)) {
        black_ini_set("online_insigniadns", v);
        g.dns_note.clear();
    } else {
        g.dns_note = S(Str::DnsAddrBad);
        g.dns_note_bad = true;
    }
}

// Asks server for AS.XBOXLIVE.COM (the game's first lookup), 3 s at most: what to show, and whether it is a problem
// (no answer, no address, or a proxy's fake address: 198.18.0.0/15, TUN mode)
bool dns_ask(const std::string &server, bool pc, std::wstring *result);

// Check network: dns_ask on a thread of its own
void net_check()
{
    if (g.net_busy) return;
    bool pc = false;
    std::string server = dns_server(&pc);
    g.net_busy = true;
    {
        std::lock_guard<std::mutex> l(g.net_lock);
        g.net_result = S(Str::NetChecking);
        g.net_bad = false;
    }
    std::thread([server, pc] {
        std::wstring result;
        bool bad = dns_ask(server, pc, &result);
        {
            std::lock_guard<std::mutex> l(g.net_lock);
            g.net_result = result;
            g.net_bad = bad;
        }
        g.net_busy = false;
        InvalidateRect(g.hwnd, nullptr, FALSE);
    }).detach();
}

bool dns_ask(const std::string &server, bool pc, std::wstring *out)
{
    {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        uint8_t q[64] = {0x4E, 0x47, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0}; // id "NG", recursion desired, one question
        size_t n = 12;
        for (const char *label : {"AS", "XBOXLIVE", "COM"}) {
            q[n++] = (uint8_t)strlen(label);
            memcpy(q + n, label, strlen(label));
            n += strlen(label);
        }
        q[n++] = 0;
        const uint8_t tail[4] = {0, 1, 0, 1}; // type A, class IN
        memcpy(q + n, tail, 4);
        n += 4;
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        DWORD timeout = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(53);
        inet_pton(AF_INET, server.c_str(), &to.sin_addr);
        uint8_t r[512];
        int got = sendto(s, (const char *)q, (int)n, 0, (sockaddr *)&to, sizeof to) > 0 ? recv(s, (char *)r, sizeof r, 0) : -1;
        closesocket(s);
        std::string addr; // the first address in the answer
        if (got > 12 && r[0] == 0x4E && r[1] == 0x47) {
            int answers = r[6] << 8 | r[7];
            size_t i = 12;
            while (i < (size_t)got && r[i]) i += r[i] >= 0xC0 ? 2 : 1 + r[i];
            i += 5; // the name's end, type, class
            for (int k = 0; k < answers && addr.empty() && i + 12 <= (size_t)got; k++) {
                if (r[i] >= 0xC0) i += 2;
                else {
                    while (i < (size_t)got && r[i]) i += 1 + r[i];
                    i++;
                }
                if (i + 10 > (size_t)got) break;
                int type = r[i] << 8 | r[i + 1], len = r[i + 8] << 8 | r[i + 9];
                if (type == 1 && len == 4 && i + 14 <= (size_t)got) {
                    char b[16];
                    snprintf(b, sizeof b, "%u.%u.%u.%u", r[i + 10], r[i + 11], r[i + 12], r[i + 13]);
                    addr = b;
                }
                i += 10 + len;
            }
        }
        std::wstring w_server = from_utf8(server), w_addr = from_utf8(addr), result;
        bool bad = true;
        wchar_t b[256];
        if (got <= 0) {
            swprintf(b, 256, S(pc ? Str::NetSilentPc : Str::NetSilent), w_server.c_str());
            result = b;
        } else if (addr.empty()) {
            result = S(Str::NetNoAddr);
        } else if (addr.rfind("198.18.", 0) == 0 || addr.rfind("198.19.", 0) == 0) {
            swprintf(b, 256, S(Str::NetFake), w_addr.c_str());
            result = b;
        } else {
            result = S(Str::NetOk) + w_addr + L"  (" + w_server + L")";
            bad = false;
        }
        *out = result;
        return bad;
    }
}

// --- resources --------------------------------------------------------------------------------------------------

IWICBitmapSource *load_jpeg(int id)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!r) return nullptr;
    void *data = LockResource(LoadResource(nullptr, r));
    DWORD size = SizeofResource(nullptr, r);
    IWICStream *stream = nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    IWICBitmapSource *out = nullptr;
    if (SUCCEEDED(g.wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory((BYTE *)data, size)) &&
        SUCCEEDED(g.wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(g.wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeMedianCut))) {
        IWICBitmap *bmp = nullptr; // decode now: the resource stream is only read once
        if (SUCCEEDED(g.wic->CreateBitmapFromSource(conv, WICBitmapCacheOnLoad, &bmp))) out = bmp;
    }
    release(conv);
    release(frame);
    release(dec);
    release(stream);
    return out;
}

// --- drawing ----------------------------------------------------------------------------------------------------

// A row of a drop-down list (Painter::drop_list): the choice, then two details (strong: 1 or 2 = that one brighter)
struct ListRow {
    std::wstring a, b, c;
    int strong = 0;
    bool off = false; // shown dimmed, cannot be chosen
};
// A drop-down list's shape: the box's width, the open list's width, where its b and c columns start (0: c is
// right-aligned)
struct ListShape {
    float box_w, list_w, col_b, col_c;
};

// The icon font: Segoe Fluent Icons comes with Windows 11 only. Windows 10 has the same icons at the same code points in
// Segoe MDL2 Assets; without it the menu's icons were boxes there (players, 2026-10-24).
const wchar_t *g_icon_font; // --mdl2 (with --snap): Segoe MDL2 Assets, as on Windows 10
const wchar_t *icon_font()
{
    const wchar_t *&font = g_icon_font;
    if (!font) {
        font = L"Segoe MDL2 Assets";
        IDWriteFontCollection *fc = nullptr;
        UINT32 index = 0;
        BOOL found = FALSE;
        if (g.dw && SUCCEEDED(g.dw->GetSystemFontCollection(&fc, FALSE)) &&
            SUCCEEDED(fc->FindFamilyName(L"Segoe Fluent Icons", &index, &found)) && found)
            font = L"Segoe Fluent Icons";
        if (fc) fc->Release();
    }
    return font;
}

const DWRITE_TRIMMING kNoTrim = {DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};

struct Painter {
    ID2D1RenderTarget *rt;
    ID2D1Bitmap *hero = nullptr, *texture = nullptr;
    ID2D1Bitmap *const *pay = nullptr; // [2]: WeChat Pay, Alipay
    std::function<void()> overlay; // an open drop-down list, drawn after the page
    // while the page is drawn: the lowest thing on it (text as laid out, filled shapes), for how far it scrolls
    bool measuring = false;
    float content_bottom = 0;

    void text(const std::wstring &s, D2D1_RECT_F r, float size, uint32_t color, float alpha = 1.0f,
              DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL, const wchar_t *family = nullptr,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, float spacing = 0.0f, bool one_line = false)
    {
        IDWriteTextFormat *fmt = nullptr;
        if (!family) family = g_lang == Lang::Zh ? L"Microsoft YaHei UI" : L"Segoe UI";
        if (FAILED(g.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                          g_lang == Lang::Zh ? L"zh-cn" : L"en-us", &fmt)))
            return;
        fmt->SetTextAlignment(align);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(one_line ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
        IDWriteInlineObject *dots = nullptr; // one line: what does not fit ends in an ellipsis
        if (one_line && SUCCEEDED(g.dw->CreateEllipsisTrimmingSign(fmt, &dots))) {
            DWRITE_TRIMMING t = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            fmt->SetTrimming(&t, dots);
        }
        IDWriteTextLayout *layout = nullptr;
        if (one_line && !s.empty()) { // cut short? (laid out without the ellipsis, wider than its place): its tip
            IDWriteTextLayout *whole = nullptr;
            fmt->SetTrimming(&kNoTrim, nullptr);
            DWRITE_TEXT_METRICS m;
            if (SUCCEEDED(g.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, 100000.0f, r.bottom - r.top, &whole)) &&
                SUCCEEDED(whole->GetMetrics(&m)) && m.width > r.right - r.left + 0.5f) {
                D2D1_MATRIX_3X2_F t;
                rt->GetTransform(&t); // the page's scroll: tips are kept where they are on the window
                D2D1_RECT_F w = {r.left + t._31, std::max(r.top + t._32, 0.0f), r.right + t._31, std::min(r.bottom + t._32, kH)};
                if (w.bottom > w.top) g_tips.push_back({w, s});
            }
            release(whole);
            if (dots) {
                DWRITE_TRIMMING tr = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                fmt->SetTrimming(&tr, dots);
            }
        }
        if (SUCCEEDED(g.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, r.right - r.left, r.bottom - r.top, &layout))) {
            if (spacing != 0.0f) {
                IDWriteTextLayout1 *l1 = nullptr;
                if (SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1), (void **)&l1))) {
                    l1->SetCharacterSpacing(0, spacing, 0, {0, (UINT32)s.size()});
                    l1->Release();
                }
            }
            ID2D1SolidColorBrush *b = nullptr;
            rt->CreateSolidColorBrush(rgb(color, alpha), &b);
            if (b) {
                rt->DrawTextLayout({r.left, r.top}, layout, b, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                b->Release();
            }
            DWRITE_TEXT_METRICS m;
            if (measuring && !s.empty() && SUCCEEDED(layout->GetMetrics(&m)))
                content_bottom = std::max(content_bottom, r.top + m.top + m.height);
            layout->Release();
        }
        release(dots);
        fmt->Release();
    }

    void fill(D2D1_RECT_F r, uint32_t color, float alpha = 1.0f, float radius = 0.0f)
    {
        if (measuring) content_bottom = std::max(content_bottom, r.bottom);
        ID2D1SolidColorBrush *b = nullptr;
        rt->CreateSolidColorBrush(rgb(color, alpha), &b);
        if (!b) return;
        if (radius > 0) rt->FillRoundedRectangle({r, radius, radius}, b);
        else rt->FillRectangle(r, b);
        b->Release();
    }

    void stroke(D2D1_RECT_F r, uint32_t color, float alpha = 1.0f, float radius = 0.0f, float width = 1.0f)
    {
        ID2D1SolidColorBrush *b = nullptr;
        rt->CreateSolidColorBrush(rgb(color, alpha), &b);
        if (!b) return;
        if (radius > 0) rt->DrawRoundedRectangle({r, radius, radius}, b, width);
        else rt->DrawRectangle(r, b, width);
        b->Release();
    }

    // A linear gradient from (x0,y0) colour a to (x1,y1) colour b over rect r
    void gradient(D2D1_RECT_F r, D2D1_POINT_2F p0, D2D1_COLOR_F a, D2D1_POINT_2F p1, D2D1_COLOR_F b)
    {
        D2D1_GRADIENT_STOP stops[2] = {{0.0f, a}, {1.0f, b}};
        ID2D1GradientStopCollection *coll = nullptr;
        ID2D1LinearGradientBrush *br = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, &coll)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush({p0, p1}, coll, &br)))
            rt->FillRectangle(r, br);
        release(br);
        release(coll);
    }

    void background()
    {
        rt->Clear(rgb(kInk));
        if (texture) { // the website's body texture, tiled, under its dark veil (rgba(11,10,13,.80 -> .92))
            ID2D1BitmapBrush *bb = nullptr;
            D2D1_BITMAP_BRUSH_PROPERTIES bp = {D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR};
            if (SUCCEEDED(rt->CreateBitmapBrush(texture, bp, &bb))) {
                rt->FillRectangle({0, 0, kW, kH}, bb);
                bb->Release();
            }
            gradient({0, 0, kW, kH}, {0, 0}, rgb(kInk, 0.80f), {0, kH}, rgb(kInk, 0.92f));
        }
        if (hero) { // the key art at the top right, fading into the page on the left and at the bottom
            D2D1_RECT_F r = {kW - 900, 0, kW, 450};
            rt->DrawBitmap(hero, r, 0.62f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            gradient({r.left, r.top, r.left + 420, r.bottom}, {r.left, 0}, rgb(kInk, 1.0f), {r.left + 420, 0}, rgb(kInk, 0.0f));
            gradient({r.left, r.bottom - 200, r.right, r.bottom}, {0, r.bottom - 200}, rgb(kInk, 0.0f), {0, r.bottom}, rgb(kInk, 0.88f));
            fill({r.left, r.bottom, r.right, kH}, kInk, 0.88f);
            gradient({r.left, r.bottom, r.right, kH}, {0, r.bottom}, rgb(kInk, 0.0f), {0, kH}, rgb(kInk, 0.4f));
        }
    }

    void nav()
    {
        fill({0, 0, kNavW, kH}, 0x16141a, 0.94f);
        fill({kNavW - 1, 0, kNavW, kH}, kInk3);
        // brand
        text(L"\x5FCD\x8005\x9F99\x5251\x4F20", {28, 30, kNavW - 12, 72}, 30, kBone, 1.0f, DWRITE_FONT_WEIGHT_BLACK, L"Microsoft YaHei UI");
        text(ng04() ? L"NINJA GAIDEN" : L"NINJA GAIDEN BLACK", {29, 74, kNavW - 12, 94}, 11.5f, kGold, 1.0f,
             DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI", DWRITE_TEXT_ALIGNMENT_LEADING, 2.2f);
        // the game: Black | 2004
        for (int i = 0; i < 2; i++) {
            D2D1_RECT_F r = {28.0f + i * 92, 100, 28.0f + i * 92 + 88, 124};
            bool on = g.module == i, hov = g.hover == kHitModule + i;
            g_hits.push_back({kHitModule + i, r});
            if (on) fill(r, kBlood, 0.16f, 3);
            stroke(r, on ? kBlood : kInk3, on ? 0.9f : 1.0f, 3);
            text(S(i ? Str::ModuleNg04 : Str::ModuleBlack), r, 12.5f, on || hov ? kBone : kAsh, 1.0f,
                 on ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, nullptr, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        fill({28, 138, 64, 140}, kBlood);
        // pages
        for (int i = 0; i < kPageCount; i++) {
            D2D1_RECT_F r = {12, 160.0f + i * 50, kNavW - 13, 160.0f + i * 50 + 44};
            g_hits.push_back({i, r});
            bool sel = g.page == i, hov = g.hover == i;
            if (sel) {
                fill(r, kBlood, 0.13f, 4);
                fill({r.left, r.top + 9, r.left + 3, r.bottom - 9}, kBlood);
            } else if (hov) {
                fill(r, kBone, 0.05f, 4);
            }
            uint32_t c = sel || hov ? kBone : i == kSupport ? kGold : kAsh; // Support stands out in gold, heart and all
            text(kIcons[i], {r.left + 18, r.top, r.left + 44, r.bottom}, 17, sel ? kBlood : i == kSupport ? kGold : c, 1.0f,
                 DWRITE_FONT_WEIGHT_NORMAL, icon_font());
            text(S((Str)((int)Str::NavStart + i)), {r.left + 52, r.top, r.right - 8, r.bottom}, 15, c, 1.0f,
                 sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        }
        // language
        D2D1_RECT_F zh = {28, kH - 56, 76, kH - 28}, en = {80, kH - 56, 128, kH - 28};
        g_hits.push_back({kHitLangZh, zh});
        g_hits.push_back({kHitLangEn, en});
        for (int i = 0; i < 2; i++) {
            D2D1_RECT_F r = i ? en : zh;
            bool on = (g_lang == Lang::En) == (i == 1), hov = g.hover == (i ? kHitLangEn : kHitLangZh);
            if (on) fill(r, kBone, 0.08f, 3);
            stroke(r, on ? kGold : kInk3, on ? 0.9f : 1.0f, 3);
            text(i ? L"EN" : L"\x4E2D\x6587", r, 12.5f, on || hov ? kBone : kAsh, 1.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 i ? L"Segoe UI" : L"Microsoft YaHei UI", DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    // "Window 1600x900 · 16:9" / "Fullscreen · 16:9" / "Borderless · 4:3"
    std::wstring display_summary()
    {
        std::string fs = black_ini_saved("fullscreen"), win = black_ini_saved("window"); // what the game will use
        std::wstring s = S(fs == "2" ? Str::ModeFullscreen : fs == "1" ? Str::ModeBorderless : Str::ModeWindow);
        if (fs != "1" && fs != "2" && !win.empty() && win != "auto") {
            std::wstring w(win.begin(), win.end());
            size_t x = w.find(L'x');
            if (x != std::wstring::npos) w[x] = L'\u00D7';
            s += L" " + w;
        }
        return s + L"  \x00B7  " + aspect_name(aspect_index(true));
    }

    void start_page()
    {
        const float x = kNavW + 56;
        text(ng04() ? L"NINJA GAIDEN  \x00B7  NATIVE PC PORT" : L"NINJA GAIDEN BLACK  \x00B7  NATIVE PC PORT", {x, 104, x + 600, 126}, 12.5f, kGold, 1.0f,
             DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI", DWRITE_TEXT_ALIGNMENT_LEADING, 2.6f);
        // the title with a soft shadow (the website's h1: text-shadow 0 2px 20px #000)
        for (int i = 0; i < 3; i++)
            text(S(ng04() ? Str::TitleNg04 : Str::Title), {x + 1.0f + i, 132.0f + 2 + i, x + 760, 196.0f + 2 + i}, 46, 0x000000, 0.28f,
                 DWRITE_FONT_WEIGHT_BLACK, L"Microsoft YaHei UI");
        text(S(ng04() ? Str::TitleNg04 : Str::Title), {x, 132, x + 760, 196}, 46, kBone, 1.0f, DWRITE_FONT_WEIGHT_BLACK, L"Microsoft YaHei UI");
        // the subtitle and the not-ready note cross the 2004 key art's logo: a shadow like the title's, and a dark plate
        for (int i = 0; i < 2; i++)
            text(S(ng04() ? Str::SubtitleNg04 : Str::Subtitle), {x + 1.0f + i, 201.0f + i, x + 761 + i, 229.0f + i}, 17, 0x000000, 0.55f);
        text(S(ng04() ? Str::SubtitleNg04 : Str::Subtitle), {x, 200, x + 760, 228}, 17, kAsh);

        // the start button
        bool ready = exists(game_exe()) && exists(g.game) && !g.game_other;
        D2D1_RECT_F b = {x, 268, x + 236, 326};
        g_hits.push_back({kHitStart, b});
        bool hov = g.hover == kHitStart;
        if (ready) {
            gradient(b, {0, b.top}, rgb(hov ? 0xe0484f : kBlood), {0, b.bottom}, rgb(hov ? 0xb82e37 : 0xa8262f));
            stroke(b, 0xe0484f, 0.9f, 0, 1);
        } else {
            fill(b, kInk3, 0.9f);
            stroke(b, kAsh, 0.35f);
        }
        text(L"\xE768", {b.left + 26, b.top, b.left + 56, b.bottom}, 18, ready ? kBone : kAsh, 1.0f, DWRITE_FONT_WEIGHT_NORMAL,
             icon_font());
        text(S(Str::Play), {b.left + 58, b.top, b.right - 16, b.bottom}, 20, ready ? kBone : kAsh, 1.0f, DWRITE_FONT_WEIGHT_BOLD,
             nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, g_lang == Lang::Zh ? 4.0f : 1.0f);
        if (!ready) {
            fill({b.right + 10, b.top, b.right + 430, b.bottom}, kInk, 0.78f, 4);
            text(S(Str::NotReady), {b.right + 20, b.top, b.right + 420, b.bottom}, 13.5f, kAsh);
        }

        // the summary card
        D2D1_RECT_F c = {x, 372, x + 620, 372 + 3 * 46 + 24};
        fill(c, kInk, 0.62f);
        stroke(c, kInk3);
        fill({c.left, c.top, c.left + 3, c.bottom}, kGold, 0.8f);
        std::wstring rows[3][2] = {{S(Str::Game), ready ? folder_of(g.game) : S(Str::GameMissing)},
                                   {S(Str::Gpu), gpu_label(gpu_choices()[gpu_choice_index(true)])},
                                   {S(Str::Display), display_summary()}};
        for (int i = 0; i < 3; i++) {
            float y = c.top + 12 + i * 46;
            text(rows[i][0], {c.left + 26, y, c.left + 130, y + 46}, 13.5f, kAsh);
            text(rows[i][1], {c.left + 132, y, c.right - 20, y + 46}, 14.5f, kBone, 1.0f, DWRITE_FONT_WEIGHT_NORMAL, nullptr,
                 DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
            if (i < 2) fill({c.left + 26, y + 46, c.right - 20, y + 47}, kInk3);
        }
        // CPU cores (black.ini multicore): one core like the Xbox (default), or Windows spreads the game's threads
        // (experimental). Saved at once: this page has no Save button.
        float cy = c.bottom + 22;
        text(S(Str::CpuMode), {x, cy, x + 130, cy + 30}, 13.5f, kAsh);
        segmented(kHitCpuMode, x + 130, cy, {S(Str::CpuSingle), S(Str::CpuAuto)}, black_ini("multicore") == "1" ? 1 : 0, 170);
        text(S(Str::CpuNote), {x + 130, cy + 38, x + 760, cy + 94}, 12, kAsh, 0.8f); // three lines (the footer is below at kH - 44)

        wchar_t foot[160];
        swprintf(foot, 160, S(Str::Footer), L"" GAME_VERSION);
        text(foot, {x, kH - 44, x + 700, kH - 20}, 12, kAsh, 0.75f);
    }

    // A button: primary = blood red, else outlined. Registers its hit when enabled.
    void button(int id, D2D1_RECT_F r, const std::wstring &label, bool primary, bool enabled = true)
    {
        bool hov = enabled && g.hover == id;
        if (enabled) g_hits.push_back({id, r});
        if (primary && enabled) {
            gradient(r, {0, r.top}, rgb(hov ? 0xe0484f : kBlood), {0, r.bottom}, rgb(hov ? 0xb82e37 : 0xa8262f));
            stroke(r, 0xe0484f, 0.9f);
        } else {
            fill(r, hov ? kInk3 : kInk, enabled ? 0.85f : 0.5f);
            stroke(r, hov ? kGold : kInk3, enabled ? 1.0f : 0.6f);
        }
        text(label, r, primary ? 15.5f : 13.5f, enabled ? kBone : kAsh, enabled ? 1.0f : 0.6f,
             primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, nullptr, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Support: the author's note, then the WeChat Pay and Alipay codes side by side under it
    void support_page()
    {
        const float x = kNavW + 56, w = 800;
        text(S(Str::SponsorTitle), {x, 46, x + 700, 98}, 32, kBone, 1.0f, DWRITE_FONT_WEIGHT_BOLD);
        fill({x, 102, x + 36, 104}, kBlood);
        D2D1_RECT_F c = {x, 116, x + w, 326};
        fill(c, kInk, 0.66f);
        stroke(c, kInk3);
        fill({c.left, c.top, c.left + 3, c.bottom}, kGold, 0.8f);
        IDWriteTextFormat *fmt = nullptr;
        if (SUCCEEDED(g.dw->CreateTextFormat(g_lang == Lang::Zh ? L"Microsoft YaHei UI" : L"Segoe UI", nullptr,
                                             DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14,
                                             g_lang == Lang::Zh ? L"zh-cn" : L"en-us", &fmt))) {
            fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 22, 17);
            ID2D1SolidColorBrush *b = nullptr;
            rt->CreateSolidColorBrush(rgb(kBone, 0.92f), &b);
            const wchar_t *t = S(Str::SponsorText);
            if (b) rt->DrawText(t, (UINT32)wcslen(t), fmt, {c.left + 26, c.top + 18, c.right - 26, c.bottom - 12}, b), b->Release();
            fmt->Release();
        }
        if (!pay) return;
        const float top = c.bottom + 16, bottom = kH - 16, gap = 40;
        float ws[2] = {}, total = gap;
        for (int i = 0; i < 2; i++)
            if (pay[i]) {
                D2D1_SIZE_F sz = pay[i]->GetSize();
                total += ws[i] = (bottom - top) * sz.width / sz.height;
            }
        float px = x + (w - total) / 2;
        for (int i = 0; i < 2; i++) {
            if (!pay[i]) continue;
            rt->DrawBitmap(pay[i], {px, top, px + ws[i], bottom}, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            px += ws[i] + gap;
        }
    }

    // A card with a gold edge and a heading; returns the content's top
    // The width of a line of text
    float measure(const std::wstring &s, float size, DWRITE_FONT_WEIGHT weight)
    {
        IDWriteTextFormat *fmt = nullptr;
        IDWriteTextLayout *layout = nullptr;
        float w = 0;
        if (SUCCEEDED(g.dw->CreateTextFormat(g_lang == Lang::Zh ? L"Microsoft YaHei UI" : L"Segoe UI", nullptr, weight,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt)) &&
            SUCCEEDED(g.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, 4000, 100, &layout))) {
            DWRITE_TEXT_METRICS m;
            if (SUCCEEDED(layout->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
        }
        release(layout);
        release(fmt);
        return w;
    }

    // A card with a gold edge and a heading, a "?" after it when help is set; returns the content's top
    float card(D2D1_RECT_F c, const std::wstring &heading, int help = 0)
    {
        fill(c, kInk, 0.66f);
        stroke(c, kInk3);
        fill({c.left, c.top, c.left + 3, c.bottom}, kGold, 0.8f);
        text(heading, {c.left + 26, c.top + 10, c.right - 20, c.top + 44}, 16, kBone, 1.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (help) help_mark(help, c.left + 26 + measure(heading, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 10, c.top + 17, 9.5f);
        return c.top + 50;
    }

    // A round "?" at (x, y) (its box's top-left) opening help `id`
    void help_mark(int id, float x, float y, float r)
    {
        D2D1_RECT_F q = {x, y, x + 2 * r + 1, y + 2 * r + 1};
        bool hov = g.hover == id;
        g_hits.push_back({id, {q.left - 4, q.top - 4, q.right + 4, q.bottom + 4}});
        ID2D1SolidColorBrush *b = nullptr;
        rt->CreateSolidColorBrush(rgb(hov ? kGold : kAsh, hov ? 1.0f : 0.8f), &b);
        if (b) {
            D2D1_ELLIPSE e = {{(q.left + q.right) / 2, (q.top + q.bottom) / 2}, r, r};
            if (hov) {
                ID2D1SolidColorBrush *f = nullptr;
                rt->CreateSolidColorBrush(rgb(kGold, 0.16f), &f);
                if (f) rt->FillEllipse(e, f), f->Release();
            }
            rt->DrawEllipse(e, b, 1.2f);
            b->Release();
        }
        text(L"?", q, r * 1.3f, hov ? kGold : kAsh, 1.0f, DWRITE_FONT_WEIGHT_BOLD, L"Segoe UI", DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // A setting's name on the left of its row, with its "?" after it
    void setting_label(Str s, float lx, float y, float cx)
    {
        text(S(s), {lx, y, cx - 10, y + 30}, 13.5f, kAsh);
        if (int id = help_id(s)) help_mark(id, lx + measure(S(s), 13.5f, DWRITE_FONT_WEIGHT_NORMAL) + 7, y + 8, 7.0f);
    }

    // A confirmation over the window: a title, the question, Continue / Cancel
    void ask_panel()
    {
        fill({0, 0, kW, kH}, kInk, 0.72f);
        g_hits.push_back({kHitAskNo, {0, 0, kW, kH}}); // outside: cancel
        Str title = g.ask == kAskOverwrite ? Str::AskOverwriteTitle : g.ask == kAskNonEmpty ? Str::AskNonEmptyTitle
                  : g.ask == kAskReset   ? Str::ResetTitle
                                         : Str::AskDeleteTitle;
        Str body = g.ask == kAskOverwrite ? Str::AskOverwrite : g.ask == kAskNonEmpty ? Str::AskNonEmpty
                 : g.ask == kAskReset     ? (g.page == kDisplay ? Str::ResetDisplay : g.page == kControls ? Str::ResetControls
                                            : g.page == kOnline ? Str::ResetOnline : Str::ResetGraphics)
                 : g.ask == kAskUnsavedStart ? Str::UnsavedStart
                 : g.ask == kAskUnsavedClose ? Str::UnsavedClose
                                          : Str::DeleteConfirm;
        if (g.ask == kAskUnsavedStart || g.ask == kAskUnsavedClose) title = Str::UnsavedTitle;
        if (g.ask == kAskRestore) title = Str::AskRestoreTitle, body = Str::AskRestore;
        if (g.ask == kAskImport) title = Str::ImportX, body = Str::AskImport;
        if (g.ask == kAskExport) title = Str::ExportX, body = Str::AskExport;
        if (g.ask == kAskResign) title = Str::AskResignTitle, body = Str::AskResign;
        if (g.ask == kAskUninstall) title = Str::AskUninstallTitle, body = Str::AskUninstall;
        if (g.ask == kAskNewConsole) title = Str::AskNewConsoleTitle, body = Str::AskNewConsole;
        if (g.ask == kAskRebuildConsole) title = Str::AskRebuildTitle, body = Str::AskRebuild;
        std::wstring body_text = S(body);
        if (g.ask == kAskUninstall) {
            wchar_t b[1024];
            swprintf(b, 1024, S(body), g.uninstall_name.c_str(), g.uninstall_size.c_str());
            body_text = b;
        }
        const float pw = 560, ph = 300, px = (kW - pw) / 2, py = (kH - ph) / 2;
        D2D1_RECT_F p = {px, py, px + pw, py + ph};
        fill(p, 0x16141a, 0.98f, 6);
        stroke(p, kInk3, 1.0f, 6);
        fill({p.left, p.top + 6, p.left + 3, p.bottom - 6}, g.ask == kAskDelete || g.ask == kAskUninstall ? kBlood : kGold, 0.85f);
        g_hits.push_back({0x7fff, p});
        text(S(title), {p.left + 34, p.top + 22, p.right - 30, p.top + 58}, 19, kBone, 1.0f, DWRITE_FONT_WEIGHT_BOLD);
        fill({p.left + 34, p.top + 62, p.left + 64, p.top + 64}, kBlood);
        IDWriteTextFormat *fmt = nullptr;
        if (SUCCEEDED(g.dw->CreateTextFormat(g_lang == Lang::Zh ? L"Microsoft YaHei UI" : L"Segoe UI", nullptr,
                                             DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14,
                                             g_lang == Lang::Zh ? L"zh-cn" : L"en-us", &fmt))) {
            fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 23, 18);
            ID2D1SolidColorBrush *b = nullptr;
            rt->CreateSolidColorBrush(rgb(kBone, 0.92f), &b);
            const wchar_t *s = body_text.c_str();
            if (b) rt->DrawText(s, (UINT32)wcslen(s), fmt, {p.left + 34, p.top + 78, p.right - 34, p.bottom - 74}, b), b->Release();
            fmt->Release();
        }
        bool unsaved_ask = g.ask == kAskUnsavedStart || g.ask == kAskUnsavedClose; // save / don't save; outside cancels
        button(unsaved_ask ? kHitAskAlt : kHitAskNo, {p.right - 34 - 120 - 12 - 160, p.bottom - 56, p.right - 34 - 120 - 12, p.bottom - 20},
               S(unsaved_ask ? Str::DontSave : Str::Cancel), false);
        button(kHitAskYes, {p.right - 34 - 120, p.bottom - 56, p.right - 34, p.bottom - 20},
               S(g.ask == kAskDelete ? Str::MoveToBin : g.ask == kAskReset ? Str::ResetYes : unsaved_ask ? Str::Save : Str::Continue), true);
    }

    // The "?" panel over the window
    void help_panel()
    {
        fill({0, 0, kW, kH}, kInk, 0.72f);
        g_hits.push_back({kHitHelpClose, {0, 0, kW, kH}}); // a click outside the panel closes it (the panel's own hits come later)
        bool image = g.help == kHitHelpImage, setting = g.help >= kHitHelpSet && g.help < kHitHelpSet + kSettingHelpCount;
        bool reg = g.help == kHitHelpRegister; // the registration steps: a larger panel
        bool big = reg || (setting && kSettingHelp[g.help - kHitHelpSet].label == Str::DnsCard); // so is DNS's
        bool dlc = g.help == kHitHelpDlc;
        Str title = setting ? kSettingHelp[g.help - kHitHelpSet].label : image ? Str::FromImage : reg ? Str::RegisterTitle
                  : dlc ? Str::DlcCard : Str::InstalledGame;
        Str body_text = setting ? kSettingHelp[g.help - kHitHelpSet].help : image ? (ng04() ? Str::HelpFromImageNg04 : Str::HelpFromImage)
                      : reg ? Str::HelpRegister : dlc ? Str::HelpDlc : Str::HelpInstalled;
        const float pw = big ? 820.0f : 640.0f, ph = big ? 640.0f : 460.0f, px = (kW - pw) / 2, py = (kH - ph) / 2;
        D2D1_RECT_F p = {px, py, px + pw, py + ph};
        fill(p, 0x16141a, 0.98f, 6);
        stroke(p, kInk3, 1.0f, 6);
        fill({p.left, p.top + 6, p.left + 3, p.bottom - 6}, kGold, 0.85f);
        g_hits.push_back({0x7fff, p}); // inside the panel: nothing (the topmost hit wins over the close area)
        text(S(title), {p.left + 34, p.top + 22, p.right - 30, p.top + 58}, 19, kBone, 1.0f, DWRITE_FONT_WEIGHT_BOLD);
        fill({p.left + 34, p.top + 62, p.left + 64, p.top + 64}, kBlood);
        D2D1_RECT_F body = {p.left + 34, p.top + 78, p.right - 34, p.bottom - 74};
        IDWriteTextFormat *fmt = nullptr;
        if (SUCCEEDED(g.dw->CreateTextFormat(g_lang == Lang::Zh ? L"Microsoft YaHei UI" : L"Segoe UI", nullptr,
                                             DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14,
                                             g_lang == Lang::Zh ? L"zh-cn" : L"en-us", &fmt))) {
            fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 23, 18);
            // laid out to its full height; what does not fit scrolls with the wheel (a thin bar at the panel's edge)
            if (g.help != g.help_for) g.help_for = g.help, g.help_scroll = g.help_target = 0;
            const wchar_t *s = S(body_text);
            IDWriteTextLayout *layout = nullptr;
            ID2D1SolidColorBrush *b = nullptr;
            rt->CreateSolidColorBrush(rgb(kBone, 0.92f), &b);
            if (b && SUCCEEDED(g.dw->CreateTextLayout(s, (UINT32)wcslen(s), fmt, body.right - body.left, 100000.0f, &layout))) {
                DWRITE_TEXT_METRICS m = {};
                layout->GetMetrics(&m);
                float view = body.bottom - body.top;
                g.help_max = std::max(0.0f, m.height - view);
                g.help_target = std::min(g.help_target, g.help_max);
                g.help_scroll = std::min(g.help_scroll, g.help_max);
                rt->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);
                rt->DrawTextLayout({body.left, body.top - g.help_scroll}, layout, b);
                rt->PopAxisAlignedClip();
                if (g.help_max > 0) {
                    float track = view, thumb = std::max(40.0f, track * view / m.height);
                    float ty = body.top + (track - thumb) * g.help_scroll / g.help_max;
                    fill({p.right - 16, body.top, p.right - 11, body.bottom}, kInk3, 0.5f, 2);
                    fill({p.right - 16, ty, p.right - 11, ty + thumb}, kBone, 0.55f, 2);
                }
                layout->Release();
            }
            if (b) b->Release();
            fmt->Release();
        }
        button(kHitHelpClose, {p.right - 34 - 140, p.bottom - 56, p.right - 34, p.bottom - 20}, S(Str::GotIt), true);
    }

    void install_page()
    {
        const float x = kNavW + 56, w = 760;
        text(S(Str::NavInstall), {x, 64, x + 700, 116}, 32, kBone, 1.0f, DWRITE_FONT_WEIGHT_BOLD);
        fill({x, 120, x + 36, 122}, kBlood);

        // from an image
        D2D1_RECT_F c = {x, 146, x + w, 146 + 214};
        float y = card(c, S(Str::FromImage), kHitHelpImage);
        bool busy = installing();
        auto row = [&](const wchar_t *label, const std::wstring &value, int hit, const wchar_t *btn) {
            text(label, {c.left + 26, y, c.left + 110, y + 36}, 13.5f, kAsh);
            text(value.empty() ? S(Str::NotChosen) : value, {c.left + 112, y, c.right - 150, y + 36}, 13.5f,
                 value.empty() ? kAsh : kBone, value.empty() ? 0.7f : 1.0f, DWRITE_FONT_WEIGHT_NORMAL, nullptr,
                 DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
            button(hit, {c.right - 136, y + 3, c.right - 20, y + 33}, btn, false, !busy);
            y += 42;
        };
        row(S(Str::Image), g.iso, kHitPickIso, S(Str::Choose));
        uint64_t avail = free_space(g.dest);
        bool room = g.iso_need && avail >= space_needed();
        if (g.iso_need) {
            wchar_t b[128];
            swprintf(b, 128, S(Str::Space), space_needed() / 1073741824.0, avail / 1073741824.0);
            text(std::wstring(b) + (room ? L"" : std::wstring(L"  \x00B7  ") + S(Str::NoSpace)), {c.left + 112, y, c.right - 20, y + 26},
                 12.5f, room ? kAsh : kBlood);
        }
        y += 34;
        if (busy) { // progress
            double f = g.prog.total ? (double)g.prog.done / (double)g.prog.total : 0.0;
            D2D1_RECT_F bar = {c.left + 26, y + 6, c.right - 150, y + 14};
            fill(bar, kInk3, 1.0f, 4);
            if (f > 0) gradient({bar.left, bar.top, bar.left + (float)((bar.right - bar.left) * f), bar.bottom}, {bar.left, 0},
                                rgb(kBlood), {bar.right, 0}, rgb(kGold));
            wchar_t b[160];
            swprintf(b, 160, S(Str::Installing), g.prog.files.load(), g.prog.file_count.load());
            std::wstring cur;
            {
                std::lock_guard<std::mutex> l(g.prog.lock);
                cur = g.prog.message;
            }
            wchar_t pct[64];
            swprintf(pct, 64, L"  \x00B7  %.1f / %.1f GB  \x00B7  %d%%", g.prog.done / 1073741824.0, g.prog.total / 1073741824.0, (int)(f * 100));
            text(std::wstring(b) + pct + L"  \x00B7  " + cur, {c.left + 26, y + 18, c.right - 150, y + 44}, 12.5f, kAsh, 1.0f,
                 DWRITE_FONT_WEIGHT_NORMAL, nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
            button(kHitCancel, {c.right - 136, y - 6, c.right - 20, y + 26}, S(Str::Cancel), false);
        } else {
            button(kHitInstall, {c.left + 26, y - 6, c.left + 206, y + 34}, S(Str::StartInstall), true, room);
            if (!g.status.empty())
                text(g.status, {c.left + 222, y - 6, c.right - 20, y + 34}, 13, g.status_bad ? kBlood : kBone, g.status_bad ? 1.0f : 0.9f);
        }
        if (g.offer_delete && !busy)
            button(kHitDeleteIso, {c.left + 26, c.bottom - 46, c.left + 286, c.bottom - 16}, S(Str::DeleteImage), false);

        // the game folder: where an install goes and where the game starts from
        D2D1_RECT_F c2 = {x, 382, x + w, 382 + 128};
        float y2 = card(c2, S(Str::InstalledGame), kHitHelpInstalled);
        bool have = exists(g.game);
        text(folder_of(g.game), {c2.left + 26, y2, c2.right - 270, y2 + 30}, 13.5f, kBone, 1.0f, DWRITE_FONT_WEIGHT_NORMAL,
             nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
        bool is_default = _wcsicmp(folder_of(g.game).c_str(), default_game_folder().c_str()) == 0;
        button(kHitDefaultPath, {c2.right - 256, y2 + 1, c2.right - 146, y2 + 31}, S(Str::DefaultPath), false, !busy && !is_default);
        button(kHitPickGame, {c2.right - 136, y2 + 1, c2.right - 20, y2 + 31}, S(Str::Change), false, !busy);
        text(!have ? std::wstring(S(Str::NotInstalledHere))
                   : g.game_other ? std::wstring(S(ng04() ? Str::GameIsBlack : Str::GameIsNg04))
                   : g.game_ok ? std::wstring(S(g.game_edition == install::Edition::Ng04      ? Str::GameCheckedNg04
                                         : g.game_edition == install::Edition::Us        ? Str::GameCheckedUs
                                         : g.game_edition == install::Edition::X21Jp   ? Str::GameCheckedJp
                                         : g.game_edition == install::Edition::BasicX2 ? Str::GameCheckedBasic
                                                                                       : Str::GameChecked))
                              : std::wstring(S(Str::GameWrong)) + g.game_detail,
             {c2.left + 26, y2 + 36, c2.right - 20, y2 + 64}, 12.5f, !have ? kAsh : g.game_ok ? kGold : kBlood);

        if (!ng04()) return;
        // Ninja Gaiden 2004: the Hurricane Packs (optional; they bring the 1.06 update)
        if (!g.dlc_read) {
            g.dlc = install::dlc_status(save_hdd());
            g.content = install::installed_content(save_hdd());
            g.dlc_read = true;
        }
        D2D1_RECT_F c3 = {x, 526, x + w, 526 + 142};
        float y3 = card(c3, S(Str::DlcCard), kHitHelpDlc);
        text(S(Str::DlcFolder), {c3.left + 26, y3, c3.left + 110, y3 + 36}, 13.5f, kAsh);
        text(g.dlc_dir.empty() ? S(Str::NotChosen) : g.dlc_dir, {c3.left + 112, y3, c3.right - 150, y3 + 36}, 13.5f,
             g.dlc_dir.empty() ? kAsh : kBone, g.dlc_dir.empty() ? 0.7f : 1.0f, DWRITE_FONT_WEIGHT_NORMAL, nullptr,
             DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
        button(kHitPickDlc, {c3.right - 136, y3 + 3, c3.right - 20, y3 + 33}, S(Str::ChooseFolder), false, !busy);
        y3 += 44;
        button(kHitInstallDlc, {c3.left + 26, y3, c3.left + 176, y3 + 36}, S(Str::DlcInstall), true, !busy && !g.dlc_dir.empty());
        // what is installed, for other players (the installer's layout)
        button(kHitExportDlc, {c3.left + 184, y3, c3.left + 284, y3 + 36}, S(Str::DlcExport), false, !busy && !g.content.empty());
        std::wstring st;
        bool have_dlc = g.dlc.update || g.dlc.pack1 || g.dlc.pack2;
        if (!g.dlc_result.empty()) st = g.dlc_result;
        else if (!have_dlc) st = S(Str::DlcNone);
        else {
            const wchar_t *sep = g_lang == Lang::Zh ? L"\x3001" : L", ";
            st = S(Str::DlcHave);
            if (g.dlc.update) st += S(Str::DlcUpdate);
            if (g.dlc.pack1) st += std::wstring(g.dlc.update ? sep : L"") + S(Str::DlcPack1);
            if (g.dlc.pack2) st += std::wstring(g.dlc.update || g.dlc.pack1 ? sep : L"") + S(Str::DlcPack2);
        }
        text(st, {c3.left + 296, y3, c3.right - 186, y3 + 36}, 13, g.dlc_bad ? kBlood : have_dlc ? kGold : kAsh, 1.0f,
             DWRITE_FONT_WEIGHT_NORMAL, nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
        // what is installed, to uninstall one (a confirmation first): the update, the packs, what the game downloaded
        std::vector<ListRow> rows;
        for (const install::ContentItem &c : g.content)
            rows.push_back({c.id == L"$u" ? std::wstring(S(Str::DlcUpdate)) : c.name, L"", content_size_text(c.bytes)});
        if (rows.empty()) rows.push_back({S(Str::UninstallNone), L"", L"", 0, true});
        wchar_t count[32];
        swprintf(count, 32, S(Str::UninstallCount), (int)g.content.size());
        drop_list(kHitUninstall, c3.right - 176, y3 + 2, {156, 320, 0, 0}, {S(Str::UninstallBox), L"", g.content.empty() ? L"" : count},
                  rows, -1, L"");
    }

    // A row of options; returns nothing, registers base + i for each
    // enabled false: dimmed and not clickable, the choice still shown faintly (the console's analog sound with digital on)
    void segmented(int base, float x, float y, const std::vector<std::wstring> &opts, int sel, float w = 104, bool enabled = true)
    {
        for (size_t i = 0; i < opts.size(); i++) {
            D2D1_RECT_F r = {x + i * (w + 6), y, x + i * (w + 6) + w, y + 30};
            bool on = (int)i == sel, hov = enabled && g.hover == base + (int)i;
            if (!enabled) {
                fill(r, on ? kBlood : kInk, on ? 0.08f : 0.45f, 3);
                stroke(r, on ? kBlood : kInk3, on ? 0.35f : 0.6f, 3);
                text(opts[i], r, 13, kAsh, 0.45f, DWRITE_FONT_WEIGHT_NORMAL, nullptr, DWRITE_TEXT_ALIGNMENT_CENTER);
                continue;
            }
            g_hits.push_back({base + (int)i, r});
            if (on) {
                fill(r, kBlood, 0.22f, 3);
                stroke(r, kBlood, 0.9f, 3);
            } else {
                fill(r, hov ? kInk3 : kInk, 0.85f, 3);
                stroke(r, hov ? kGold : kInk3, 1.0f, 3);
            }
            text(opts[i], r, 13, on || hov ? kBone : kAsh, 1.0f, on ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                 nullptr, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    void stepper(int down, int up, float x, float y, float value)
    {
        button(down, {x, y, x + 30, y + 30}, L"\x2212", false);
        wchar_t b[16];
        swprintf(b, 16, L"%.2f", value);
        text(b, {x + 34, y, x + 104, y + 30}, 14, kBone, 1.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI", DWRITE_TEXT_ALIGNMENT_CENTER);
        button(up, {x + 108, y, x + 138, y + 30}, L"+", false);
    }

    // A settings page's title, its note, and its card; returns the card's content top. The rows are 40 high.
    float settings_page(Str title, Str heading, int rows, D2D1_RECT_F *card_out)
    {
        const float x = kNavW + 56, w = 800;
        text(S(title), {x, 46, x + 700, 98}, 32, kBone, 1.0f, DWRITE_FONT_WEIGHT_BOLD);
        fill({x, 102, x + 36, 104}, kBlood);
        // under the title: when the settings take effect, or that there are changes still to save
        if (unsaved()) text(S(Str::Unsaved), {x + 60, 88, x + w - 380, 112}, 12.5f, kGold, 1.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        else if (game_running()) // the game read its settings when it started; it keeps what is saved now (loader settings_save)
            text(S(Str::GameRunningNote), {x + 60, 88, x + w - 380, 112}, 12.5f, kGold, 1.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        else text(S(Str::AppliesNext), {x + 60, 88, x + w - 380, 112}, 12.5f, kAsh, 0.85f);
        // Save / Discard (enabled with unsaved changes, on any page) and Restore defaults (this page only, reset_page)
        bool dirty = unsaved();
        button(kHitSave, {x + w - 110, 66, x + w, 102}, S(Str::Save), true, dirty);
        button(kHitDiscard, {x + w - 110 - 10 - 120, 66, x + w - 120, 102}, S(Str::Discard), false, dirty);
        button(kHitResetPage, {x + w - 230 - 10 - 120, 66, x + w - 240, 102}, S(Str::ResetPage), false);
        *card_out = {x, 124, x + w, 124 + 50 + rows * 40.0f + 8};
        return card(*card_out, S(heading), help_id(heading));
    }

    // Display: how the picture reaches the screen
    void display_page()
    {
        D2D1_RECT_F c;
        float y = settings_page(Str::NavDisplay, Str::Picture, 7, &c); // 6 rows and the note under the sliders
        const float lx = c.left + 26, cx = c.left + 210;
        auto label = [&](Str s) { setting_label(s, lx, y, cx); };
        const float right = c.right - 26;
        label(Str::DisplayMode);
        mode_list(cx, y - 2, right);
        y += 40;
        label(Str::Aspect); // above the window size: the sizes offered follow it
        aspect_list(cx, y - 2, right);
        y += 40;
        label(Str::WindowSize);
        window_list(cx, y - 2, right);
        y += 40;
        // black.ini fullscreen_res: fullscreen and borderless at the screen's size (default) or at the window size,
        // scaled up (loader d3d/host.cpp frame_layout, exclusive_mode), for slow machines
        label(Str::FsRes);
        segmented(kHitFsRes, cx, y, {S(Str::SizeScreen), S(Str::FsAtWindow)}, black_ini("fullscreen_res") == "window" ? 1 : 0, 96);
        text(S(Str::FsResNote), {cx + 2 * 102 + 12, y, right, y + 30}, 12, kAsh, 0.7f);
        y += 40;
        label(Str::Brightness);
        slider(kHitGain, cx, y, slider_value(kHitGain));
        y += 40;
        label(Str::Gamma);
        slider(kHitGamma, cx, y, slider_value(kHitGamma));
        text(S(Str::GainNote), {cx, y + 34, right, y + 56}, 12, kAsh, 0.75f);

        // the console's settings the game reads at start (loader kernel/k_misc.cpp ExQueryNonVolatileSetting)
        D2D1_RECT_F c2 = {c.left, c.bottom + 14, c.right, c.bottom + 14 + 172};
        float y2 = card(c2, S(Str::ConsoleCard), help_id(Str::ConsoleCard));
        setting_label(Str::SysLang, lx, y2, cx);
        sys_lang_list(cx, y2 - 2);
        // the dashboard's Audio: analog mono / stereo / Dolby Surround, digital Dolby Digital and DTS (black.ini audio_*)
        // with the digital output on, the port's one output is 5.1 and the analog choice has no effect (the loader's
        // audio/dsound.cpp): shown dimmed with a note
        bool ac3 = black_ini("audio_ac3") == "1", dts = black_ini("audio_dts") == "1";
        y2 += 40;
        setting_label(Str::AudioRow, lx, y2, cx);
        std::string am = black_ini("audio_mode");
        segmented(kHitAudioMode, cx, y2, {S(Str::AudioMono), S(Str::AudioStereo), S(Str::AudioSurround)},
                  am == "mono" ? 0 : am == "surround" ? 2 : 1, 104, !ac3 && !dts);
        if (ac3 || dts) text(S(Str::AudioDigitalNote), {cx + 3 * 110 + 6, y2, c2.right - 20, y2 + 30}, 12, kAsh, 0.7f);
        y2 += 40;
        setting_label(Str::DigitalRow, lx, y2, cx);
        segmented(kHitDigital, cx, y2, {S(Str::Off), L"Dolby Digital", L"DTS", S(Str::DigitalBoth)},
                  ac3 && dts ? 3 : dts ? 2 : ac3 ? 1 : 0);
    }

    // The console's language (black.ini language: the Xbox's XC_LANGUAGE numbers 1-9), each in its own words
    void sys_lang_list(float x, float y)
    {
        static const wchar_t *const names[9] = {L"English", L"\u65E5\u672C\u8A9E", L"Deutsch", L"Fran\u00E7ais", L"Espa\u00F1ol",
                                                L"Italiano", L"\uD55C\uAD6D\uC5B4", L"\u4E2D\u6587", L"Portugu\u00EAs"};
        std::string v = black_ini("language");
        int sel = v.empty() ? 0 : atoi(v.c_str()) - 1;
        if (sel < 0 || sel > 8) sel = 0;
        std::vector<ListRow> rows;
        for (int i = 0; i < 9; i++) rows.push_back({names[i], L"", i == 0 ? S(Str::ShadowDefault) : L""});
        drop_list(kHitSysLang, x, y, {200, 340, 0, 0}, {rows[sel].a, L"", L""}, rows, sel, S(Str::SysLangNote));
    }

    // A value slider over the loader's range (kSliderMin - kSliderMax), with a mark at 1.00 (no change) and the
    // value on the right; dragged with the mouse (wndproc: WM_LBUTTONDOWN / WM_MOUSEMOVE / WM_LBUTTONUP)
    void slider(int id, float x, float y, float value)
    {
        const float w = kSliderW, cy = y + 15;
        bool hot = g.hover == id || g.drag == id;
        g_hits.push_back({id, {x - 10, y, x + w + 10, y + 30}});
        auto px = [&](float v) { return x + (v - kSliderMin) / (kSliderMax - kSliderMin) * w; };
        fill({x, cy - 2, x + w, cy + 2}, kInk3, 1.0f, 2);
        float one = px(1.0f), at = px(value);
        fill({std::min(one, at), cy - 2, std::max(one, at), cy + 2}, kBlood, 0.9f, 2);
        fill({one - 1, cy - 8, one + 1, cy + 8}, kAsh, 0.8f); // 1.00
        ID2D1SolidColorBrush *b = nullptr;
        rt->CreateSolidColorBrush(rgb(hot ? kGold : kBone), &b);
        if (b) {
            rt->FillEllipse({{at, cy}, 8, 8}, b);
            b->Release();
        }
        wchar_t s[16];
        swprintf(s, 16, L"%.2f", value);
        text(s, {x + w + 20, y, x + w + 80, y + 30}, 14, kBone, 1.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI");
    }

    // Display mode: black.ini fullscreen = 0 window, 2 exclusive fullscreen, 1 borderless
    void mode_list(float x, float y, float right)
    {
        std::string fs = black_ini("fullscreen");
        std::vector<ListRow> rows = {{S(Str::ModeWindow), S(Str::ModeWindowNote)},
                                     {S(Str::ModeFullscreen), S(Str::ModeFullscreenNote)},
                                     {S(Str::ModeBorderless), S(Str::ModeBorderlessNote)}};
        int sel = fs == "2" ? 1 : fs == "1" ? 2 : 0;
        drop_list(kHitMode, x, y, {200, 540, 110, 0}, rows[sel], rows, sel, L"");
    }

    // Aspect ratio: black.ini widescreen = 1 (16:9) / 0 (4:3); the window sizes below follow it
    void aspect_list(float x, float y, float right)
    {
        const Str notes[kAspectCount] = {Str::Aspect169Note, Str::Aspect1610Note, Str::Aspect219Note, Str::Aspect43Note,
                                         Str::AspectMonitorNote, Str::Aspect6427Note, Str::Aspect4318Note, Str::Aspect125Note};
        std::vector<ListRow> rows;
        int chosen = aspect_index(), sel = 0;
        for (int i = 0; i < kAspectCount; i++) {
            int a = kAspectShown[i];
            ListRow r = {aspect_name(a), S(notes[a])};
            if (a == kAspectMonitor && !g_monitor_aspect_ok) r = {aspect_name(a), S(Str::AspectMonitorOff)}, r.off = true;
            rows.push_back(r);
            if (a == chosen) sel = i;
        }
        drop_list(kHitAspect, x, y, {200, 340, 80, 0},
                  {rows[sel].a, L"", chosen == kAspectMonitor ? S(Str::AspectMonitorBox) : rows[sel].b}, rows, sel, S(Str::AspectSoon));
    }

    // Window size (the client area, used in window mode): auto = what the game picks, the largest whole multiple of
    // the aspect within 90% of the desktop's work area (loader d3d/host.cpp open_window), or a fixed size of the
    // chosen aspect; sizes larger than the work area cannot be chosen
    void window_list(float x, float y, float right)
    {
        int asp = aspect_index();
        const int(*sizes)[2] = kWins[asp];
        int aw = kAspects[asp][0], ah = kAspects[asp][1];
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int avail_w = (work.right - work.left) * 9 / 10, avail_h = (work.bottom - work.top) * 9 / 10;
        int auto_h = avail_h / (ah * 2) * (ah * 2), auto_w = auto_h * aw / ah;
        if (auto_w > avail_w) {
            auto_w = avail_w / (aw * 2) * (aw * 2);
            auto_h = auto_w * ah / aw;
        }
        auto dims = [](int w, int h) { return std::to_wstring(w) + L"\u00D7" + std::to_wstring(h); };
        std::string cur = black_ini("window");
        // Fullscreen and borderless show the picture at the screen's size, whatever is chosen here: the box says that
        // size and the list is for window mode only. "Auto 4352x1836" there read as the size the game would use, and
        // the screen's own size, too large for a framed window, as one that could not be had (a player at 5120x2160,
        // 2026-10-24).
        std::string mode = black_ini("fullscreen");
        bool screen = !mode.empty() && mode != "0";
        // With "Fullscreen size" at the window size, fullscreen shows the picture at this size scaled up: chosen as in
        // window mode
        bool at_window = screen && black_ini("fullscreen_res") == "window";
        if (at_window) screen = false;
        std::vector<ListRow> rows;
        int sel = 0;
        for (int i = 0; i < 5; i++) {
            if (!i) {
                rows.push_back({S(Str::SizeAuto), S(Str::SizeAutoNote), dims(auto_w, auto_h)});
                rows.back().off = screen;
                continue;
            }
            bool fits = sizes[i][0] <= work.right - work.left && sizes[i][1] <= work.bottom - work.top;
            bool whole = sizes[i][0] == g_monitor_w && sizes[i][1] == g_monitor_h; // the screen's own size
            ListRow r = {dims(sizes[i][0], sizes[i][1]), fits ? L"" : whole ? S(Str::TooLargeBorderless) : S(Str::TooLarge)};
            r.off = !fits || screen;
            rows.push_back(r);
            if (cur == std::to_string(sizes[i][0]) + "x" + std::to_string(sizes[i][1])) sel = i;
        }
        ListRow shown = sel ? rows[sel] : ListRow{S(Str::SizeAuto), L"", dims(auto_w, auto_h)};
        if (screen && g_monitor_w > 0) shown = {S(Str::SizeScreen), L"", dims(g_monitor_w, g_monitor_h)};
        drop_list(kHitWin, x, y, {200, 380, 110, 270}, shown, rows, sel, S(at_window ? Str::WindowFsRes : Str::WindowOnly));
    }

    // Graphics: how the picture is made
    void graphics_page()
    {
        D2D1_RECT_F c;
        float y = settings_page(Str::NavGraphics, Str::Rendering, 9, &c);
        const float lx = c.left + 26, cx = c.left + 210;
        auto label = [&](Str s) { setting_label(s, lx, y, cx); };
        label(Str::Resolution);
        resolution_list(cx, y - 2, c.right - 26);
        y += 40;
        label(Str::ShadowQuality);
        shadow_list(cx, y - 2);
        y += 40;
        label(Str::ShadowSoft); // black.ini shadowsoft: the shadow map's percentage-closer filtering (loader d3d/nv2a_psh.cpp)
        segmented(kHitShadowSoft, cx, y, {S(Str::On), S(Str::Off)}, black_ini("shadowsoft") == "0" ? 1 : 0, 72);
        y += 40;
        label(Str::TexFilter);
        aniso_list(cx, y - 2);
        y += 40;
        label(Str::AntiAlias);
        aa_list(cx, y - 2);
        {
            // MSAA 4x / 8x with a render scale of 3x or more: measured on the iGPU (chapter 1) 4x 9.8-15 ms and 8x 24 ms
            // a frame against 5.7 off, the game slowing down; at 1x-2x it costs at most 0.8 ms (docs/04 3.28)
            std::string rd = black_ini("render"), sc = black_ini("scale");
            int msaa = atoi(black_ini("msaa").c_str());
            double scale = sc.empty() ? 3.0 : atof(sc.c_str());
            if (msaa >= 4 && (rd.empty() || rd == "scale") && scale >= 3.0)
                text(S(Str::AaHeavy), {cx + 212, y, c.right - 20, y + 30}, 12, kGold, 0.95f);
        }
        y += 40;
        label(Str::VSync); // black.ini vsync: off by default (frames paced by the Xbox vblank alone)
        segmented(kHitVsync, cx, y, {S(Str::On), S(Str::Off)}, black_ini("vsync") == "1" ? 0 : 1, 72);
        text(S(Str::VSyncNote), {cx + 2 * 78 + 12, y, c.right - 20, y + 30}, 12, kAsh, 0.7f);
        y += 40;
        label(Str::SeamFill);
        segmented(kHitSeam, cx, y, {S(Str::On), S(Str::Off)}, black_ini("seamfill") == "0" ? 1 : 0, 72);
        y += 40;
        label(Str::EffectsNative);
        segmented(kHitFx, cx, y, {S(Str::On), S(Str::Off)}, black_ini("effectsnative") == "1" ? 0 : 1, 72);
        y += 40;
        label(Str::PerfHud); // black.ini perfhud: the loader's overlay (d3d/perfhud.cpp)
        segmented(kHitHud, cx, y, {S(Str::On), S(Str::Off)}, black_ini("perfhud") == "1" ? 0 : 1, 72);
        text(S(Str::PerfHudNote), {cx + 2 * 78 + 12, y, c.right - 20, y + 30}, 12, kAsh, 0.7f);

        D2D1_RECT_F c2 = {c.left, c.bottom + 14, c.right, c.bottom + 14 + 92};
        float y2 = card(c2, S(Str::GpuCard), help_id(Str::GpuCard));
        gpu_list(cx, y2, c2.right - 26);

        D2D1_RECT_F c3 = {c.left, c2.bottom + 14, c.right, c2.bottom + 14 + 100};
        float y3 = card(c3, S(Str::ClearCache));
        text(S(Str::ClearCacheNote), {lx, y3, c3.right - 280, y3 + 48}, 12.5f, kAsh, 0.8f);
        button(kHitClearCache, {c3.right - 246, y3, c3.right - 26, y3 + 32}, S(Str::ClearCacheButton), false);
    }

    // Anti-aliasing: off, FXAA (black.ini aa = 1, the default) or MSAA 2x / 4x / 8x (msaa; the highest Direct3D 9 counts
    // the cards here offer, host: MSAA sample counts in the log)
    void aa_list(float x, float y)
    {
        int msaa = atoi(black_ini("msaa").c_str()), sel = black_ini("aa") == "0" ? 0 : 1;
        std::vector<ListRow> rows = {{S(Str::AnisoOff), L""}, {L"FXAA", S(Str::AaFxaaRow)}};
        for (int i = 0; i < 3; i++) {
            rows.push_back({L"MSAA " + std::to_wstring(kMsaaLevels[i]) + L"\u00D7", S(Str::AaMsaaRow)});
            if (msaa == kMsaaLevels[i]) sel = 2 + i;
        }
        drop_list(kHitAa, x, y, {200, 380, 110, 0}, {rows[sel].a, L"", L""}, rows, sel, S(Str::AaNote));
    }

    // Texture filtering: off (as the game sets: linear) or anisotropic 2x-16x (black.ini aniso)
    void aniso_list(float x, float y)
    {
        std::string v = black_ini("aniso");
        int cur = v.empty() ? 1 : atoi(v.c_str()), sel = 0;
        std::vector<ListRow> rows = {{S(Str::AnisoOff), S(Str::ShadowXbox)}};
        for (int i = 1; i < 5; i++) {
            rows.push_back({std::to_wstring(kAnisoLevels[i]) + L"\u00D7", S(Str::AnisoRow)});
            if (cur == kAnisoLevels[i]) sel = i;
        }
        drop_list(kHitAniso, x, y, {200, 340, 90, 0}, {rows[sel].a, L"", L""}, rows, sel, S(Str::AnisoNote));
    }

    // Shadow quality: the game's 128x128 shadow map made at 1, 4, 8 or 16 times its size (black.ini shadow; 4 by default)
    void shadow_list(float x, float y)
    {
        std::string v = black_ini("shadow");
        int cur = v.empty() ? 4 : atoi(v.c_str());
        std::vector<ListRow> rows = {{S(Str::ShadowLow), S(Str::ShadowXbox), L"128\u00D7128"},
                                     {S(Str::ShadowMid), S(Str::ShadowDefault), L"512\u00D7512"},
                                     {S(Str::ShadowHigh), L"", L"1024\u00D71024"},
                                     {S(Str::ShadowUltra), L"", L"2048\u00D72048"}};
        int sel = 1;
        for (int i = 0; i < 4; i++)
            if (cur == kShadowScales[i]) sel = i;
        drop_list(kHitShadow, x, y, {200, 340, 90, 0}, {rows[sel].a, L"", rows[sel].c}, rows, sel, S(Str::ShadowNote));
    }

    // Render resolution: a list of the window's size and 1x - 10x, each with its size in 16:9 and in 4:3 (the
    // current aspect's brighter). black.ini: render = native, or render = scale + scale = N. A "render = 1080" from
    // the game's own PC SETTINGS shows as 1080p until a choice here replaces it.
    void resolution_list(float x, float y, float right)
    {
        int asp = aspect_index(), wasp = asp == 3 ? 0 : asp; // the widescreen column: the chosen shape (16:9 when 4:3)
        bool wide = asp != 3;
        std::string rd = black_ini("render"), sc = black_ini("scale");
        double scale = sc.empty() ? 3.0 : atof(sc.c_str());
        std::vector<ListRow> rows;
        rows.push_back({S(Str::ResWindow), S(Str::ResWindowNote), L"", 0});
        for (int i = 1; i <= kMaxScale; i++) {
            std::wstring a = std::to_wstring(i) + L"\u00D7";
            if (i == 1) a += S(Str::ResOriginal);
            if (i == 3) a += S(Str::ResDefault);
            rows.push_back({a, aspect_name(wasp) + L"  " + render_size(i, wasp), L"4:3  " + render_size(i, 3), wide ? 1 : 2});
        }
        int sel = -1;
        ListRow shown;
        if (rd == "native") sel = 0;
        else if (rd.empty() || rd == "scale") {
            if (scale == std::floor(scale) && scale >= 1 && scale <= kMaxScale) sel = (int)scale;
            wchar_t b[16];
            swprintf(b, 16, L"%g\u00D7", scale);
            shown = {b, L"", render_size(scale, asp), 0};
        } else
            shown = {std::wstring(rd.begin(), rd.end()) + L"p", L"", L"", 0};
        if (sel >= 0) {
            shown = rows[sel];
            if (sel) shown = {shown.a, L"", wide ? shown.b : shown.c, 0}; // the box: the current aspect's size only
        }
        drop_list(kHitResList, x, y, {240, 440, 140, 290}, shown, rows, sel, S(Str::ResFooter));
    }

    // The graphics card: the cards that can be chosen (gpu_choices), and what Windows calls each choice
    void gpu_list(float x, float y, float right)
    {
        std::vector<ListRow> rows;
        for (const GpuChoice &ch : gpu_choices())
            rows.push_back({gpu_label(ch), L"", ch.pref ? S(ch.pref == 2 ? Str::GpuHigh : Str::GpuLow) : L"", 0});
        int sel = gpu_choice_index();
        drop_list(kHitGpu, x, y, {440, 440, 0, 0}, rows[sel], rows, sel, L"");
    }

    // One row of a list: the choice on the left; b in a column, c on the right (strong: 1 or 2 = that one brighter)
    // One row of an open list: a, then b and c in the shape's columns
    void list_row(D2D1_RECT_F r, const ListRow &row, bool on, const ListShape &sh)
    {
        const float dim = row.off ? 0.4f : 1.0f, l = r.left;
        float a_end = !row.b.empty()             ? l + sh.col_b - 8
                      : !row.c.empty() && sh.col_c ? l + sh.col_c - 8
                      : !row.c.empty()             ? r.right - 22 - measure(row.c, 12.5f, DWRITE_FONT_WEIGHT_NORMAL) // c on the right
                                                   : r.right - 14;
        text(row.a, {l + 14, r.top, a_end, r.bottom}, 13.5f, on ? kBone : kAsh, dim,
             on ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
        if (!row.b.empty())
            text(row.b, {l + sh.col_b, r.top, !row.c.empty() && sh.col_c ? l + sh.col_c - 8 : r.right - 14, r.bottom}, 12.5f,
                 row.strong == 1 ? kBone : kAsh, (row.strong == 1 ? 1.0f : 0.7f) * dim, DWRITE_FONT_WEIGHT_NORMAL, nullptr,
                 DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
        if (!row.c.empty())
            text(row.c, {sh.col_c ? l + sh.col_c : l + 14, r.top, r.right - 14, r.bottom}, 12.5f,
                 row.strong == 2 || !row.strong ? kBone : kAsh, (row.strong == 2 ? 1.0f : 0.7f) * dim, DWRITE_FONT_WEIGHT_NORMAL,
                 nullptr, sh.col_c ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_TRAILING, 0.0f, true);
    }

    // A drop-down list: the box shows `shown`; a click opens the rows (hit id + 1 + row) under it, over the rest of
    // the page (drawn last, see draw), with an optional note at the bottom. A click anywhere else closes it.
    // The box shows the choice (`shown.a`, and `shown.c` dimmer on the right); the open list is as wide as its rows need
    void drop_list(int id, float x, float y, const ListShape &sh, const ListRow &shown, const std::vector<ListRow> &rows,
                   int sel, const std::wstring &note)
    {
        D2D1_RECT_F box = {x, y, x + sh.box_w, y + 32};
        bool open = g.list_open == id, hov = g.hover == id || open;
        fill(box, hov ? kInk3 : kInk, 0.9f, 3);
        stroke(box, hov ? kGold : kInk3, 1.0f, 3);
        float a_end = shown.c.empty() ? box.right - 34 : box.right - 40 - measure(shown.c, 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
        text(shown.a, {box.left + 12, box.top, a_end, box.bottom}, 13.5f, kBone, 1.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
             nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
        if (!shown.c.empty())
            text(shown.c, {box.left + 12, box.top, box.right - 32, box.bottom}, 12.5f, kAsh, 0.85f, DWRITE_FONT_WEIGHT_NORMAL,
                 nullptr, DWRITE_TEXT_ALIGNMENT_TRAILING, 0.0f, true);
        text(L"\xE70D", {box.right - 28, box.top, box.right - 8, box.bottom}, 11, hov ? kGold : kAsh, 1.0f,
             DWRITE_FONT_WEIGHT_NORMAL, icon_font(), DWRITE_TEXT_ALIGNMENT_CENTER);
        g_hits.push_back({id, box});
        if (!open) return;
        overlay = [this, id, box, sh, rows, sel, note] {
            g_hits.push_back({kHitListClose, {0, 0, kW, kH}}); // outside the list: close it
            g_hits.push_back({id, box});                       // the box again: close it
            const float rh = 30, note_h = note.empty() ? 0.0f : 40.0f;
            float w = std::max(sh.list_w, sh.box_w);
            const int total = (int)rows.size();
            const float space_below = kH - (box.bottom + 4) - 8;
            const float space_above = box.top - 4 - 8;
            const float full_h = total * rh + 8 + note_h;
            // Open upward when the list would run off the bottom and there is more room above.
            const bool up = full_h > space_below && space_above > space_below;
            const float space = up ? space_above : space_below;
            int visible = (int)((space - 8 - note_h) / rh);
            if (visible < 1) visible = 1;
            if (visible > total) visible = total;
            const int max_first = total > visible ? total - visible : 0;
            if (g.list_scroll < 0) g.list_scroll = 0;
            if (g.list_scroll > max_first) g.list_scroll = max_first;
            g.list_scroll_max = max_first;
            const int first = g.list_scroll;
            const float h = visible * rh + 8 + note_h;
            // wider than the box and past the window's right edge (the uninstall list): line up with the box's right edge
            const float lx = box.left + w > kW - 8 ? box.right - w : box.left;
            D2D1_RECT_F list;
            if (up) list = {lx, box.top - 4 - h, lx + w, box.top - 4};
            else list = {lx, box.bottom + 4, lx + w, box.bottom + 4 + h};
            fill(list, 0x16141a, 1.0f, 4);
            stroke(list, kGold, 0.8f, 4);
            const bool scroll = total > visible;
            const float row_right = list.right - 4 - (scroll ? 12 : 0);
            for (int vi = 0; vi < visible; vi++) {
                const int i = first + vi;
                D2D1_RECT_F r = {list.left + 4, list.top + 4 + vi * rh, row_right, list.top + 4 + (vi + 1) * rh};
                int rid = id + 1 + i;
                bool on = i == sel, over = !rows[i].off && g.hover == rid;
                g_hits.push_back({rows[i].off ? 0x7fff : rid, r});
                if (on) fill({r.left, r.top + 7, r.left + 3, r.bottom - 7}, kBlood);
                if (over) fill(r, kBone, 0.06f, 3);
                list_row(r, rows[i], on || over, sh);
            }
            if (scroll) {
                const float bar_x = list.right - 9, bar_top = list.top + 6, bar_h = h - 12 - note_h;
                fill({bar_x, bar_top, bar_x + 3, bar_top + bar_h}, kInk3, 0.8f, 2);
                const float thumb_h = std::max(18.0f, bar_h * visible / total);
                const float thumb_y = bar_top + (bar_h - thumb_h) * (max_first ? (float)first / max_first : 0.0f);
                fill({bar_x, thumb_y, bar_x + 3, thumb_y + thumb_h}, kGold, 0.8f, 2);
            }
            if (!note.empty()) {
                float ny = list.bottom - 4 - note_h;
                fill({list.left + 14, ny, list.right - 14, ny + 1}, kInk3);
                g_hits.push_back({0x7fff, {list.left, ny, list.right, list.bottom}});
                text(note, {list.left + 14, ny + 6, list.right - 14, list.bottom - 2}, 11.5f, kAsh, 0.85f); // up to two lines
            }
        };
    }

    // Saves: where they are, backups, and xRevive's saves (saves.cpp does the files)
    void saves_page()
    {
        if (!g.saves_read) refresh_saves();
        const float x = kNavW + 56, w = 800;
        text(S(Str::NavSaves), {x, 46, x + 700, 98}, 32, kBone, 1.0f, DWRITE_FONT_WEIGHT_BOLD);
        fill({x, 102, x + 36, 104}, kBlood);
        const float lx = x + 26, vx = x + 150, bw = 150;
        auto value = [&](const std::wstring &s, float y, float right, bool dim) {
            text(s, {vx, y, right, y + 32}, 13.5f, dim ? kAsh : kBone, dim ? 0.75f : 1.0f, DWRITE_FONT_WEIGHT_NORMAL, nullptr,
                 DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
        };
        // where the saves are
        // the same-number warning (saves.h same_number): each save's name, time and folder
        std::wstring dup_msg; // the advice, then each number and its saves on lines of their own
        int dup_lines = 0;
        if (!g.same_number.empty()) {
            dup_msg = S(Str::SavesDup);
            dup_lines = 1;
            for (const auto &grp : g.same_number) {
                wchar_t b[64];
                swprintf(b, 64, S(Str::SavesDupGroup), (int)wcstol(grp[0].name.c_str(), nullptr, 10));
                dup_msg += L"\n" + std::wstring(b);
                dup_lines++;
                for (const saves::Slot &sl : grp) {
                    wchar_t e[512];
                    swprintf(e, 512, S(Str::SavesDupSlot), sl.name.c_str(), when(sl.time).c_str(), sl.folder.c_str());
                    dup_msg += L"\n" + std::wstring(e);
                    dup_lines++;
                }
            }
        }
        const float dup_h = dup_lines ? 10.0f + 16.5f * dup_lines : 0.0f; // 12.5 px text: about 16.5 px a line
        D2D1_RECT_F c = {x, 116, x + w, 116 + 50 + 2 * 40 + 6 + dup_h};
        float y = card(c, S(Str::SavesCard), help_id(Str::SavesCard));
        text(S(Str::SavesWhere), {lx, y, vx - 10, y + 32}, 13.5f, kAsh);
        value(save_hdd(), y, c.right - bw - 30, false);
        button(kHitOpenSaves, {c.right - bw - 20, y, c.right - 20, y + 32}, S(Str::OpenFolder), false);
        y += 40;
        text(S(Str::SavesContents), {lx, y, vx - 10, y + 32}, 13.5f, kAsh);
        std::wstring sum = S(Str::SavesNone);
        if (!g.slots.empty()) {
            wchar_t b[256];
            swprintf(b, 256, S(Str::SavesSummary), (int)g.slots.size(), g.slots[0].name.c_str(), when(g.slots[0].time).c_str());
            sum = b;
        }
        value(sum, y, c.right - 20, g.slots.empty());
        if (!dup_msg.empty())
            text(dup_msg, {lx, y + 38, c.right - 20, y + 38 + dup_h}, 12.5f, kBlood, 1.0f); // wraps
        // backups
        const int shown = std::min<int>(3, (int)g.backups.size());
        D2D1_RECT_F c2 = {x, c.bottom + 12, x + w, c.bottom + 12 + 50 + 40 + std::max(1, shown) * 34 + 8};
        y = card(c2, S(Str::BackupCard), help_id(Str::BackupCard));
        button(kHitBackupNow, {lx, y, lx + bw, y + 32}, S(Str::BackupNow), true, !g.slots.empty());
        button(kHitOpenBackups, {lx + bw + 12, y, lx + 2 * bw + 52, y + 32}, S(Str::OpenBackups), false);
        button(kHitResign, {lx + 2 * bw + 64, y, lx + 3 * bw + 64, y + 32}, S(Str::ResignBtn), false, !g.slots.empty());
        y += 42;
        if (!shown) text(S(Str::NoBackups), {lx, y, c2.right - 20, y + 30}, 13, kAsh, 0.75f);
        for (int i = 0; i < shown; i++) {
            const saves::Backup &bk = g.backups[i];
            Str kind = bk.note == L"before-restore" ? Str::NoteBeforeRestore : bk.note == L"before-import" ? Str::NoteBeforeImport
                     : bk.note == L"before-resign" ? Str::NoteBeforeResign
                     : bk.note == L"xrevive-before-export" ? Str::NoteXreviveBefore
                                                           : Str::NoteByHand;
            wchar_t b[64];
            swprintf(b, 64, S(Str::BackupSlots), bk.slots);
            text(when(bk.time), {lx, y, lx + 170, y + 30}, 13, kBone);
            text(S(kind), {lx + 175, y, lx + 420, y + 30}, 12.5f, kAsh, 0.85f, DWRITE_FONT_WEIGHT_NORMAL, nullptr,
                 DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
            text(b, {lx + 425, y, c2.right - 130, y + 30}, 12.5f, kAsh, 0.85f);
            button(kHitRestore + i, {c2.right - 120, y + 1, c2.right - 20, y + 29}, S(Str::RestoreBtn), false);
            y += 34;
        }
        // xRevive (Black only: where it keeps Ninja Gaiden 2004's saves is not known here)
        float bottom = c2.bottom;
        if (ng04()) {
            text(S(Str::NoXreviveNg04), {x, c2.bottom + 12, x + w, c2.bottom + 36}, 12.5f, kAsh, 0.85f);
            bottom = c2.bottom + 36;
        }
        std::wstring xr = ini_get(L"xrevive");
        bool xr_ok = !xr.empty() && saves::has_saves(saves::xrevive_root(xr));
        D2D1_RECT_F c3 = {x, c2.bottom + 12, x + w, c2.bottom + 12 + 50 + 2 * 40 + 4};
        if (!ng04()) {
        y = card(c3, S(Str::XreviveCard), help_id(Str::XreviveCard));
        text(S(Str::XreviveFolder), {lx, y, vx - 10, y + 32}, 13.5f, kAsh);
        value(xr.empty() ? std::wstring(S(Str::NotChosen)) : xr + (xr_ok ? L"" : std::wstring(L"  \x00B7  ") + S(Str::XreviveNoSaves)),
              y, c3.right - bw - 30, xr.empty() || !xr_ok);
        button(kHitPickXrevive, {c3.right - bw - 20, y, c3.right - 20, y + 32}, S(Str::ChooseFolder), false);
        y += 40;
        button(kHitImport, {lx, y, lx + 180, y + 32}, S(Str::ImportX), false, xr_ok);
        button(kHitExport, {lx + 192, y, lx + 372, y + 32}, S(Str::ExportX), false, !xr.empty() && !g.slots.empty());
        bottom = c3.bottom;
        }
        if (!g.save_status.empty())
            text(g.save_status, {x, bottom + 8, x + w, bottom + 32}, 12.5f, g.save_status_bad ? kBlood : kGold, 1.0f,
                 DWRITE_FONT_WEIGHT_NORMAL, nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
    }

    // About: what this is, its version and licence, the legal note (the README's words)
    // Controls: keys pressed into the Xbox pad, and the PC pad's buttons remapped (black.ini kb_*, padmap_*; the game
    // program's input/padkeys.cpp). Saved like the other settings pages.
    void controls_page()
    {
        D2D1_RECT_F c;
        settings_page(Str::NavControls, g.ctl_tab ? Str::PadCard : Str::KeyboardCard, 10, &c);
        segmented(kHitCtlTab, c.right - 26 - 2 * 110 - 6, c.top + 12, {S(Str::KeyboardCard), S(Str::PadCard)}, g.ctl_tab, 110);
        const float top = c.top + 52, rh = 32;
        if (g.ctl_tab == 0) {
            for (int i = 0; i < keys::kInputs; i++) {
                int col = i < 12 ? 0 : 1, row = i % 12;
                float x = c.left + 26 + col * 390, y = top + row * rh;
                text(S((Str)((int)Str::InA + i)), {x, y, x + 150, y + 28}, 13, kAsh, 1.0f, DWRITE_FONT_WEIGHT_NORMAL, nullptr,
                     DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
                for (int s = 0; s < 2; s++) {
                    D2D1_RECT_F r = {x + 160 + s * 92, y + 1, x + 160 + s * 92 + 84, y + 27};
                    int id = kHitKey + i * 2 + s;
                    bool cap = g.capture == i * 2 + s, hov = g.hover == id;
                    g_hits.push_back({id, r});
                    fill(r, cap ? kBlood : hov ? kInk3 : kInk, cap ? 0.3f : 0.9f, 3);
                    stroke(r, cap || hov ? kGold : kInk3, 1.0f, 3);
                    uint16_t k = kb_key(i, s);
                    text(cap ? S(Str::PressKey) : k ? keys::display(k) : L"\u2014", r, 12.5f, cap ? kGold : k ? kBone : kAsh,
                         k || cap ? 1.0f : 0.5f, DWRITE_FONT_WEIGHT_NORMAL, nullptr, DWRITE_TEXT_ALIGNMENT_CENTER, 0.0f, true);
                }
            }
            text(S(Str::KeyHint), {c.left, c.bottom + 8, c.right, c.bottom + 30}, 12, kAsh, 0.8f);
        } else {
            std::vector<ListRow> sources;
            for (int b = 0; b < keys::kButtons; b++) sources.push_back({S((Str)((int)Str::InA + b))});
            for (int i = keys::kButtons - 1; i >= 0; i--) { // drawn bottom up: an open list lies over the rows below it
                int col = i < 6 ? 0 : 1, row = i % 6;
                float x = c.left + 26 + col * 390, y = top + row * 52;
                text(S((Str)((int)Str::InA + i)), {x, y, x + 150, y + 30}, 13, kAsh);
                int src = pad_source(i);
                drop_list(kHitPadMap + i * 16, x + 160, y - 1, {200, 200, 0, 0}, sources[src], sources, src, L"");
                if (src != i) text(S(Str::PadChanged), {x + 160, y + 32, x + 370, y + 50}, 11.5f, kGold, 0.9f);
            }
            text(S(Str::PadHint), {c.left, c.bottom + 8, c.right, c.bottom + 30}, 12, kAsh, 0.8f);
        }
    }

    void about_page()
    {
        const float x = kNavW + 56, w = 800, lx = x + 26, vx = x + 150;
        text(S(Str::NavAbout), {x, 46, x + 700, 98}, 32, kBone, 1.0f, DWRITE_FONT_WEIGHT_BOLD);
        fill({x, 102, x + 36, 104}, kBlood);
        D2D1_RECT_F c = {x, 116, x + w, 116 + 50 + 72 + 8 * 31 + 6}; // the description: four lines of English
        float y = card(c, S(Str::AboutTitle));
        text(S(Str::AboutDesc), {lx, y, c.right - 20, y + 68}, 13.5f, kBone, 0.92f);
        y += 72;
        auto row = [&](Str label, const std::wstring &v, int link) { // link: kLinks index to open on a click, or -1
            text(S(label), {lx, y, vx - 10, y + 28}, 13.5f, kAsh);
            D2D1_RECT_F r = {vx, y, vx + std::min(c.right - 26 - vx, measure(v, 13.5f, DWRITE_FONT_WEIGHT_NORMAL) + 4), y + 28};
            bool hov = link >= 0 && g.hover == kHitWebsite + link;
            if (link >= 0) g_hits.push_back({kHitWebsite + link, r});
            text(v, {vx, y, c.right - 26, y + 28}, 13.5f, link >= 0 ? (hov ? kGold : kBone) : kBone, 1.0f,
                 DWRITE_FONT_WEIGHT_NORMAL, nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
            if (link >= 0) fill({r.left, r.bottom - 5, r.right - 4, r.bottom - 4}, hov ? kGold : kAsh, hov ? 1.0f : 0.5f);
            y += 31;
        };
        wchar_t ver[128];
        swprintf(ver, 128, S(Str::AboutVersion), L"" GAME_VERSION, L"" GAME_VERSION);
        row(Str::AboutVersionLabel, ver, -1);
        row(Str::AboutAuthorLabel, L"bilibili \u54C8\u96C5\u5E03\u6492", -1);
        row(Str::AboutSiteLabel, kLinks[0], 0);
        row(Str::AboutSourceLabel, kLinks[1], 1);
        row(Str::AboutGithubLabel, kLinks[2], 2);
        row(Str::AboutBilibiliLabel, kLinks[3], 3);
        row(Str::AboutEmailLabel, L"2682557218@qq.com", 4);
        row(Str::AboutLicenseLabel, S(Str::AboutLicense), -1);
        D2D1_RECT_F c2 = {x, c.bottom + 10, x + w, c.bottom + 10 + 46 + 130}; // five lines of English
        y = card(c2, S(Str::LegalTitle));
        IDWriteTextFormat *fmt = nullptr;
        if (SUCCEEDED(g.dw->CreateTextFormat(g_lang == Lang::Zh ? L"Microsoft YaHei UI" : L"Segoe UI", nullptr,
                                             DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13,
                                             g_lang == Lang::Zh ? L"zh-cn" : L"en-us", &fmt))) {
            fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 21, 16);
            ID2D1SolidColorBrush *b = nullptr;
            rt->CreateSolidColorBrush(rgb(kBone, 0.85f), &b);
            const wchar_t *s = S(Str::LegalText);
            if (b) rt->DrawText(s, (UINT32)wcslen(s), fmt, {lx, y - 6, c2.right - 26, c2.bottom - 6}, b), b->Release();
            fmt->Release();
        }
    }

    static std::wstring when(long long t)
    {
        if (t <= 0) return L"";
        time_t tt = (time_t)t;
        tm lt;
        localtime_s(&lt, &tt);
        wchar_t b[32];
        wcsftime(b, 32, L"%Y-%m-%d %H:%M", &lt);
        return b;
    }

    // Online: the switch, the registered console's files, DNS and a network check, how to sign in (black.ini online*)
    void online_page()
    {
        online_check();
        D2D1_RECT_F c;
        // the console card: the switch, the console, then its gamertags one a row (8 at most)
        auto it_acc = g.online.find("local_account");
        size_t n_acc = it_acc == g.online.end() ? 0 : it_acc->second.size();
        bool local_ok = online_value("local") == "ok";
        float y = settings_page(Str::NavOnline, Str::OnlineCard, 2 + (local_ok ? (int)std::max<size_t>(n_acc, 1) : 0), &c);
        const float lx = c.left + 26, cx = c.left + 210, right = c.right - 26, bw = 120;
        auto label = [&](Str s) { text(S(s), {lx, y, cx - 10, y + 30}, 13.5f, kAsh); };
        auto path_row = [&](Str s, const char *key, int hit) {
            label(s);
            std::string v = black_ini(key);
            text(v.empty() ? std::wstring(S(Str::NotChosen)) : from_utf8(v), {cx, y, right - bw - 12, y + 30}, 13.5f,
                 v.empty() ? kAsh : kBone, v.empty() ? 0.7f : 1.0f, DWRITE_FONT_WEIGHT_NORMAL, nullptr, DWRITE_TEXT_ALIGNMENT_LEADING,
                 0.0f, true);
            button(hit, {right - bw, y, right, y + 30}, S(Str::ChooseFile), false);
            y += 40;
        };
        label(Str::OnlineSwitch);
        segmented(kHitOnline, cx, y, {S(Str::On), S(Str::Off)}, black_ini("online") == "1" ? 0 : 1, 72);
        y += 40;
        auto names = [&](const char *k) {
            std::wstring s;
            auto it = g.online.find(k);
            if (it != g.online.end())
                for (size_t i = 0; i < it->second.size(); i++) s += (i ? L", " : L"") + from_utf8(it->second[i]);
            return s;
        };
        // the port's own console, what the game uses: registered and its gamertags, or what to do
        label(Str::OnlineLocal);
        {
            std::string local = online_value("local");
            std::wstring st;
            bool good = false;
            if (g.online.empty()) st = S(Str::OnlineCheckFailed);
            else if (local == "none" || local.empty()) st = S(Str::OnlineLocalNone);
            else if (local == "other") st = S(Str::OnlineLocalOther);
            else if (local != "ok") st = S(Str::OnlineLocalBad);
            else if (online_value("local_registered") != "1")
                st = S(online_value("local_insignia") == "1" ? Str::OnlineLocalRegistered : Str::OnlineLocalNotReg);
            else st = S(Str::OnlineLocalOk), good = true;
            // xemu running matters only for the console that came from it (one console must not sign in twice)
            bool xemu_runs = online_value("xemu") == "1" && online_value("same_console") == "1";
            if (good && xemu_runs) st = S(Str::OnlineXemu), good = false;
            // None yet but xemu's registered console chosen below (a player from before 2.2): the game imports it at its
            // first online start, so no New console here (a new one would stop that and need registering)
            bool will_import = online_value("eeprom") == "ok" && online_value("hdd") == "ok" && online_value("registered") == "1";
            // its buttons: New console when there is none; with one, Register while Insignia has not said yes (the
            // registration steps: the Setup Assistant, then the dashboard's network test) and the Xbox Live dashboard;
            // Rebuild for a console whose files are damaged or do not match
            bool none = !g.online.empty() && (local == "none" || local.empty()), ok = local == "ok";
            bool broken = !g.online.empty() && !none && !ok;
            if (none && will_import) st = S(Str::OnlineLocalWillImport);
            bool unreg = ok && online_value("local_registered") != "1";
            bool show_register = unreg && online_value("local_insignia") != "1";
            const float dw = 140; // the dashboard's button
            bool show_new = none && !will_import;
            float text_right = show_new || broken ? right - bw - 12 : ok ? right - dw - 12 - (show_register ? bw + 10 : 0) : right;
            bool busy = g.setup_proc != nullptr;
            if (busy) st = S(g.setup_is_dash ? Str::DashRunning : Str::RegisterRunning), good = true;
            if (!g.local_result.empty()) st = g.local_result, good = false;
            text(st, {cx, y, text_right, y + 30}, 13, good ? kGold : !g.local_result.empty() ? kBlood : none ? kAsh : unreg ? kBone : kBlood,
                 1.0f, DWRITE_FONT_WEIGHT_NORMAL, nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
            bool can = !busy && !xemu_runs && !game_running();
            if (show_new) button(kHitNewConsole, {right - bw, y, right, y + 30}, S(Str::NewConsole), false, can);
            if (broken) button(kHitRebuildConsole, {right - bw, y, right, y + 30}, S(Str::RebuildConsole), false, can);
            if (ok) {
                if (show_register) button(kHitRegister, {right - dw - 10 - bw, y, right - dw - 10, y + 30}, S(Str::RegisterConsole), can, can);
                button(kHitDashboard, {right - dw, y, right, y + 30}, S(Str::OpenDashboard), false, can);
            }
        }
        y += 40;
        if (local_ok) { // its gamertags, or how to make one
            label(Str::OnlineAccountsRow);
            if (!n_acc) {
                text(S(Str::OnlineNoGamertag), {cx, y, right, y + 30}, 13, kAsh, 1.0f, DWRITE_FONT_WEIGHT_NORMAL, nullptr,
                     DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
                y += 40;
            }
            for (size_t i = 0; i < n_acc; i++) {
                text(L"\u2022  " + from_utf8(it_acc->second[i]), {cx, y, right, y + 30}, 13.5f, kBone, 1.0f, DWRITE_FONT_WEIGHT_NORMAL,
                     nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
                y += 40;
            }
        }

        // importing from xemu: its files and what they hold (only for a console registered in xemu)
        D2D1_RECT_F cx2 = {c.left, c.bottom + 12, c.right, c.bottom + 12 + 50 + 3 * 40 + 8};
        y = card(cx2, S(Str::OnlineXemuCard));
        path_row(Str::OnlineEeprom, "online_eeprom", kHitPickEeprom);
        path_row(Str::OnlineHdd, "online_hdd", kHitPickHdd);
        label(Str::OnlineStatus);
        // the xemu console: what is wrong first, else valid, its gamertags and which are not here yet; the import's
        // result in its place once there is one
        std::wstring st;
        bool good = false, can_import = false;
        std::string ee = online_value("eeprom"), hdd = online_value("hdd");
        bool none = black_ini("online_eeprom").empty() || black_ini("online_hdd").empty();
        if (none) st = S(online_value("local") == "ok" ? Str::OnlineXemuOptional : Str::OnlineFilesNone);
        else if (g.online.empty()) st = S(Str::OnlineCheckFailed);
        else if (ee == "unreadable") st = S(Str::OnlineEepromUnreadable);
        else if (ee != "ok") st = S(Str::OnlineEepromBad);
        else if (hdd != "ok") st = S(Str::OnlineHddBad) + from_utf8(hdd);
        else if (online_value("registered") != "1") st = S(Str::OnlineNotRegistered);
        else {
            st = S(Str::OnlineXemuOk);
            if (g.online.count("importable")) { // another console's accounts move onto this one (re-encrypted: loader net/online.cpp)
                st += S(Str::OnlineImportable) + names("importable"), can_import = true;
                if (online_value("same_console") == "0") st += S(Str::OnlineOtherConsole);
            }
            else if (g.online.count("account")) st = S(Str::OnlineAllHere);
            can_import = can_import || online_value("local") != "ok"; // the console itself comes over the first time
            good = true;
        }
        if (good && online_value("xemu") == "1") st = S(Str::OnlineXemu), good = false, can_import = false;
        bool show_import = !g.import_result.empty();
        if (show_import) st = g.import_result;
        text(st, {cx, y, right - bw - 12, y + 30}, 13,
             show_import ? (g.import_bad ? kBlood : kGold) : good ? kGold : none ? kAsh : kBlood, 1.0f, DWRITE_FONT_WEIGHT_NORMAL,
             nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
        button(kHitImportXemu, {right - bw, y, right, y + 30}, S(Str::OnlineImport), can_import, can_import);

        // DNS
        bool pc = dns_by_pc(); // a row more: the InsigniaDNS address
        D2D1_RECT_F c2 = {c.left, cx2.bottom + 12, c.right, cx2.bottom + 12 + 50 + (pc ? 4 : 3) * 40 + 8};
        y = card(c2, S(Str::DnsCard), help_id(Str::DnsCard));
        // the DNS the game will use, as chosen below (not the console's own setting: the port redirects every lookup)
        label(Str::DnsConsole);
        bool by_pc = false;
        std::wstring server = from_utf8(dns_server(&by_pc));
        bool no_addr = !by_pc && black_ini("online_dns_mode") == "insigniadns"; // the game falls back to Insignia's
        Str whose = no_addr ? Str::DnsFallback : !by_pc ? Str::DnsInsigniaOwn
                  : black_ini("online_dns_mode") == "console" ? Str::DnsLegacy : Str::DnsPc;
        text(S(by_pc ? Str::DnsManual : Str::DnsAuto) + server + S(whose), {cx, y, right, y + 30}, 13.5f, no_addr ? kBlood : kBone);
        y += 40;
        label(Str::DnsMode);
        segmented(kHitDnsMode, cx, y, {S(Str::DnsInsignia), S(Str::DnsByConsole)}, pc ? 1 : 0, 170);
        text(S(pc ? Str::DnsConsoleNote : Str::DnsInsigniaNote), {cx + 2 * 176 + 10, y - 4, right, y + 34}, 12, kAsh, 0.8f);
        y += 40;
        if (pc) { // the address, typed (WM_CHAR) or found on this PC
            label(Str::DnsAddr);
            D2D1_RECT_F f = {cx, y, cx + 200, y + 30};
            g_hits.push_back({kHitDnsAddr, f});
            std::string addr = black_ini("online_insigniadns");
            fill(f, kInk, 0.9f);
            stroke(f, g.dns_edit ? kGold : g.hover == kHitDnsAddr ? kAsh : kInk3);
            bool hint = !g.dns_edit && addr.empty();
            text(g.dns_edit ? g.dns_text + L"\u258F" : hint ? std::wstring(S(Str::DnsAddrHint)) : from_utf8(addr),
                 {f.left + 10, f.top, f.right - 6, f.bottom}, 13.5f, hint ? kAsh : kBone, hint ? 0.7f : 1.0f);
            button(kHitDnsFind, {cx + 212, y, cx + 212 + 100, y + 30}, S(Str::DnsFind), false);
            if (!g.dns_note.empty())
                text(g.dns_note, {cx + 324, y - 5, right, y + 35}, 12, g.dns_note_bad ? kBlood : kGold);
            y += 40;
        }
        button(kHitNetCheck, {cx, y, cx + 170, y + 30}, S(Str::NetCheck), false, !g.net_busy);
        {
            std::lock_guard<std::mutex> l(g.net_lock);
            if (!g.net_result.empty())
                text(g.net_result, {cx + 182, y - 5, right, y + 35}, 12.5f, g.net_busy ? kAsh : g.net_bad ? kBlood : kGold);
        }

        // how to sign in
        D2D1_RECT_F c3 = {c.left, c2.bottom + 12, c.right, c2.bottom + 12 + 50 + 48};
        y = card(c3, S(Str::LoginCard));
        text(S(Str::LoginHow), {lx, y - 6, right - 300, y + 42}, 12.5f, kBone, 0.9f);
        button(kHitHelpRegister, {right - 280, y + 2, right - 152, y + 32}, S(Str::RegisterSteps), false);
        button(kHitInsignia, {right - 140, y + 2, right, y + 32}, S(Str::InsigniaSite), false);
    }

    void placeholder_page()
    {
        const float x = kNavW + 56;
        text(S((Str)((int)Str::NavStart + g.page)), {x, 110, x + 700, 170}, 34, kBone, 1.0f, DWRITE_FONT_WEIGHT_BOLD);
        fill({x, 176, x + 36, 178}, kBlood);
        text(S(Str::ComingSoon), {x, 196, x + 700, 230}, 15, kAsh);
    }

    // The whole wording of the cut text under the mouse, in a box beside it (wrapped, kept inside the window)
    void tip_box()
    {
        g.tip = -1;
        if (g.mouse_x < 0 || g.drag || g.list_open) return;
        for (size_t i = g_tips.size(); i-- > 0;)
            if (inside(g_tips[i].r, g.mouse_x, g.mouse_y)) { g.tip = (int)i; break; }
        if (g.tip < 0) return;
        const std::wstring &s = g_tips[g.tip].s;
        IDWriteTextFormat *fmt = nullptr;
        if (FAILED(g.dw->CreateTextFormat(g_lang == Lang::Zh ? L"Microsoft YaHei UI" : L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13, g_lang == Lang::Zh ? L"zh-cn" : L"en-us", &fmt)))
            return;
        IDWriteTextLayout *l = nullptr;
        DWRITE_TEXT_METRICS m = {};
        if (SUCCEEDED(g.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, 520.0f, 1000.0f, &l)) && SUCCEEDED(l->GetMetrics(&m))) {
            float w = m.width + 24, hgt = m.height + 16;
            float x = std::min(g.mouse_x + 14, kW - w - 8), y = g.mouse_y + 22;
            if (y + hgt > kH - 8) y = g.mouse_y - hgt - 10; // above the mouse near the bottom
            x = std::max(x, 8.0f), y = std::max(y, 8.0f);
            D2D1_RECT_F b = {x, y, x + w, y + hgt};
            fill(b, 0x16141a, 0.97f, 4);
            stroke(b, kInk3, 1.0f, 4);
            ID2D1SolidColorBrush *br = nullptr;
            rt->CreateSolidColorBrush(rgb(kBone, 0.95f), &br);
            if (br) rt->DrawTextLayout({x + 12, y + 8}, l, br), br->Release();
        }
        release(l);
        fmt->Release();
    }

    void draw()
    {
        g_hits.clear();
        g_tips.clear();
        background();
        nav();
        // the page, moved up by how far it is scrolled and kept right of the navigation; its hits moved with it after
        const size_t first_hit = g_hits.size();
        const float sc = g.page_scroll;
        rt->PushAxisAlignedClip({kNavW, 0, kW, kH}, D2D1_ANTIALIAS_MODE_ALIASED);
        rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -sc));
        measuring = true;
        content_bottom = 0;
        if (g.page == kStart) start_page();
        else if (g.page == kInstall) install_page();
        else if (g.page == kDisplay) display_page();
        else if (g.page == kGraphics) graphics_page();
        else if (g.page == kSaves) saves_page();
        else if (g.page == kAbout) about_page();
        else if (g.page == kSupport) support_page();
        else if (g.page == kControls) controls_page();
        else if (g.page == kOnline) online_page();
        else placeholder_page();
        if (overlay) overlay(); // an open drop-down list, over the page
        measuring = false;
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->PopAxisAlignedClip();
        for (size_t i = first_hit; i < g_hits.size(); i++) {
            D2D1_RECT_F &r = g_hits[i].r;
            if (r.left <= 0 && r.top <= 0 && r.right >= kW && r.bottom >= kH) continue; // a whole-window catch (a list's outside)
            r.top = std::max(r.top - sc, 0.0f);
            r.bottom = std::min(r.bottom - sc, kH);
            if (r.bottom <= r.top) r = {0, 0, 0, 0}; // scrolled out of the window
        }
        g.page_max = content_bottom + 20 - kH; // a margin under the last thing; a few pixels more are not worth a scroll
        if (g.page_max < 12) g.page_max = 0;
        if (g.scroll_target > g.page_max) g.scroll_target = g.page_max;
        if (g.page_scroll > g.page_max) g.page_scroll = g.page_max;
        if (g.page_max > 0) { // a thin bar at the right edge: how much of the page shows, and where
            const float top = 10, track = kH - 20, thumb = std::max(40.0f, track * kH / (kH + g.page_max));
            float y = top + (track - thumb) * g.page_scroll / g.page_max;
            fill({kW - 9, top, kW - 3, top + track}, kInk3, 0.5f, 3);
            fill({kW - 9, y, kW - 3, y + thumb}, kBone, 0.55f, 3);
        }
        if (g.help) help_panel();
        if (g.ask) ask_panel();
        if (!g.help && !g.ask) tip_box();
    }
};

ID2D1Bitmap *to_bitmap(ID2D1RenderTarget *rt, IWICBitmapSource *src)
{
    ID2D1Bitmap *b = nullptr;
    if (src) rt->CreateBitmapFromWicBitmap(src, nullptr, &b);
    return b;
}

void paint()
{
    if (!g.rt) {
        RECT rc;
        GetClientRect(g.hwnd, &rc);
        if (FAILED(g.d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                                 D2D1::HwndRenderTargetProperties(g.hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g.rt)))
            return;
        float dpi = (float)GetDpiForWindow(g.hwnd);
        g.rt->SetDpi(dpi, dpi);
        g.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    static ID2D1Bitmap *hero, *hero_ng04, *texture, *pay[2];
    static ID2D1RenderTarget *made_for;
    if (made_for != g.rt) {
        release(hero);
        release(hero_ng04);
        release(texture);
        for (auto &b : pay) release(b);
        hero = to_bitmap(g.rt, g.hero_src);
        hero_ng04 = to_bitmap(g.rt, g.hero_ng04_src);
        texture = to_bitmap(g.rt, g.texture_src);
        for (int i = 0; i < 2; i++) pay[i] = to_bitmap(g.rt, g.pay_src[i]);
        made_for = g.rt;
    }
    g.rt->BeginDraw();
    Painter p{g.rt, g.module == 1 && hero_ng04 ? hero_ng04 : hero, texture, pay};
    p.draw();
    if (g.rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        release(hero);
        release(hero_ng04);
        release(texture);
        for (auto &b : pay) release(b);
        release(g.rt);
        made_for = nullptr;
    }
}

int hit_at(float x, float y)
{
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it)
        if (inside(it->r, x, y)) return it->id;
    return -1;
}

// The window's title: the game shown (Black | 2004) in the language chosen
void update_title()
{
    if (g.hwnd) SetWindowTextW(g.hwnd, S(ng04() ? Str::WindowTitleNg04 : Str::WindowTitle));
}

void set_lang(Lang l)
{
    g_lang = l;
    ini_set(L"lang", l == Lang::Zh ? L"zh" : L"en");
    update_title();
}

// A dragged slider follows the mouse: x (in DIPs) over its track, in 0.05 steps
void drag_to(float x)
{
    for (const Hit &hit : g_hits)
        if (hit.id == g.drag) {
            float left = hit.r.left + 10, t = (x - left) / kSliderW;
            float v = kSliderMin + std::min(1.0f, std::max(0.0f, t)) * (kSliderMax - kSliderMin);
            g.drag_value = std::round(v * 20.0f) / 20.0f;
            InvalidateRect(g.hwnd, nullptr, FALSE);
            return;
        }
}

// --- the Saves page -----------------------------------------------------------------------------------------------

// The Xbox hard disk the game uses (launcher.ini hdd, relative to the launcher's folder; default savedata beside it)
std::wstring save_hdd()
{
    std::wstring h = ini_get(L"hdd", (g.dir + L"\\savedata").c_str());
    if (h.size() < 2 || (h[1] != L':' && h[0] != L'\\')) h = g.dir + L"\\" + h;
    return h;
}
std::wstring backup_root() { return g.dir + (ng04() ? L"\\savedata_backups_ng2004" : L"\\savedata_backups"); }
bool game_running() { return FindWindowW(L"BlackLoaderWindow", nullptr) != nullptr; } // the game's window class (d3d/host.cpp)
void open_folder(const std::wstring &dir) { ShellExecuteW(g.hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void refresh_saves()
{
    g.slots = saves::slots(saves::our_root(save_hdd()));
    g.same_number.clear(); // 2004 names its saves "SAVE GAME NO01": how it files them is not checked here
    if (!ng04()) g.same_number = saves::same_number(g.slots);
    g.backups = saves::backups(backup_root());
    g.saves_read = true;
}

void saves_result(bool ok, const std::wstring &msg)
{
    g.save_status = msg;
    g.save_status_bad = !ok;
}

// 0 back up now; kAskRestore / kAskImport / kAskExport once confirmed. What gets replaced is moved into a backup first.
void saves_action(int what)
{
    if (game_running()) return saves_result(false, S(Str::GameRunning));
    std::wstring ours = saves::our_root(save_hdd()), xr = saves::xrevive_root(ini_get(L"xrevive")), made, err;
    bool ok = false;
    Str done = Str::BackupDone;
    if (what == 0) ok = saves::backup(ours, backup_root(), L"", &made, &err);
    if (what == kAskRestore && g.restore_index >= 0 && g.restore_index < (int)g.backups.size())
        ok = saves::replace(g.backups[g.restore_index].dir, ours, backup_root(), L"before-restore", &made, &err), done = Str::RestoreDone;
    if (what == kAskImport) ok = saves::replace(xr, ours, backup_root(), L"before-import", &made, &err), done = Str::ImportDone;
    if (what == kAskExport) ok = saves::replace(ours, xr, backup_root(), L"xrevive-before-export", &made, &err), done = Str::ExportDone;
    if (what == kAskResign) {
        int n = 0;
        ok = saves::resign(ours, backup_root(), &n, &made, &err);
        wchar_t r[1024];
        if (ok && n) swprintf(r, 1024, S(Str::ResignDone), n, made.c_str());
        else if (ok) wcscpy_s(r, S(Str::ResignNone));
        else swprintf(r, 1024, S(Str::SavesFailed), err.c_str());
        saves_result(ok, r);
        refresh_saves();
        return;
    }
    wchar_t b[1024];
    swprintf(b, 1024, S(ok ? (made.empty() && done != Str::BackupDone ? Str::SavesDoneNoneMoved : done) : Str::SavesFailed),
             ok ? made.c_str() : err.c_str());
    saves_result(ok, b);
    refresh_saves();
}

// --- the Controls page ---------------------------------------------------------------------------------------------

// The key in a slot (the page's value: changed or saved; no kb_ line = the game program's default layout)
uint16_t kb_key(int input, int slot)
{
    std::string v = black_ini((std::string("kb_") + keys::input_ini(input)).c_str());
    if (v.empty()) return keys::default_key(input, slot);
    size_t comma = v.find(',');
    return keys::parse(slot == 0 ? v.substr(0, comma) : comma == std::string::npos ? "" : v.substr(comma + 1));
}

void kb_write(int input, uint16_t k0, uint16_t k1)
{
    if (!k0) k0 = k1, k1 = 0;
    std::string v = k0 ? keys::ini_name(k0) : "none";
    if (k1) v += "," + keys::ini_name(k1);
    black_ini_set((std::string("kb_") + keys::input_ini(input)).c_str(), v);
}

// A key into a slot (0: clear it); taken from wherever else it was, so one key means one input
void kb_set(int input, int slot, uint16_t k)
{
    for (int i = 0; k && i < keys::kInputs; i++)
        for (int s = 0; s < 2; s++)
            if ((i != input || s != slot) && kb_key(i, s) == k) kb_write(i, s ? kb_key(i, 0) : 0, s ? 0 : kb_key(i, 1));
    uint16_t a = kb_key(input, 0), b = kb_key(input, 1);
    if (slot == 0) a = k;
    else b = k;
    kb_write(input, a, b);
}

// The PC pad button a game button comes from (padmap_<button>; its own by default)
int pad_source(int button)
{
    std::string v = black_ini((std::string("padmap_") + keys::input_ini(button)).c_str());
    for (int b = 0; b < keys::kButtons; b++)
        if (v == keys::input_ini(b)) return b;
    return button;
}

// The shape chosen: 0 16:9, 1 16:10, 2 21:9, 3 4:3, 4 the monitor's, 5 64:27, 6 43:18, 7 12:5 (black.ini widescreen,
// aspect); a named shape before the monitor's when they are the same
int aspect_index(bool saved)
{
    std::string wide = saved ? black_ini_saved("widescreen") : black_ini("widescreen");
    std::string a = saved ? black_ini_saved("aspect") : black_ini("aspect");
    if (wide == "0") return 3;
    if (a == "16:10") return 1;
    if (a == "21:9") return 2;
    for (int i = 5; i < kAspectCount; i++)
        if (a == std::to_string(kAspects[i][0]) + ":" + std::to_string(kAspects[i][1])) return i;
    if (a != "16:9" && a == std::to_string(kAspects[kAspectMonitor][0]) + ":" + std::to_string(kAspects[kAspectMonitor][1]))
        return kAspectMonitor;
    return 0;
}

// "Restore defaults" on a settings page: that page's settings only, back to what the game uses with no black.ini
// (loader common.h Config) and, for the graphics card, what the game sets on its first run (high performance)
void reset_page(Page p)
{
    if (p == kDisplay) {
        black_ini_set("fullscreen", "0");
        black_ini_set("widescreen", "1");
        black_ini_set("aspect", "16:9");
        black_ini_set("window", "auto");
        black_ini_set("fullscreen_res", "screen");
        black_ini_set("gain", "1.00");
        black_ini_set("gamma", "1.00");
        black_ini_set("language", "1");
        black_ini_set("audio_mode", "stereo");
        black_ini_set("audio_ac3", "0");
        black_ini_set("audio_dts", "0");
    } else if (p == kGraphics) {
        black_ini_set("render", "scale");
        black_ini_set("render_factor", "1.00");
        black_ini_set("scale", "3");
        black_ini_set("shadow", "4");
        black_ini_set("shadowsoft", "1");
        black_ini_set("aniso", "1");
        black_ini_set("aa", "1");
        black_ini_set("msaa", "0");
        black_ini_set("vsync", "0");
        black_ini_set("seamfill", "1");
        black_ini_set("effectsnative", "0");
        black_ini_set("perfhud", "0");
        set_gpu_preference(2);
    } else if (p == kControls) {
        for (int i = 0; i < keys::kInputs; i++) kb_write(i, keys::default_key(i, 0), keys::default_key(i, 1));
        for (int b = 0; b < keys::kButtons; b++) black_ini_set((std::string("padmap_") + keys::input_ini(b)).c_str(), keys::input_ini(b));
    } else if (p == kOnline) { // the chosen files stay
        black_ini_set("online", "0");
        black_ini_set("online_dns_mode", "insignia");
    }
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    float scale = 96.0f / (float)GetDpiForWindow(h);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        if (g.rt) g.rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        release(g.rt);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (g.list_open && g.list_scroll_max > 0) {
            static int rest; // a high-resolution wheel (touchpads, the Steam Deck's) sends parts of a notch
            rest += GET_WHEEL_DELTA_WPARAM(wp);
            int delta = rest / WHEEL_DELTA;
            rest -= delta * WHEEL_DELTA;
            g.list_scroll -= delta;
            if (g.list_scroll < 0) g.list_scroll = 0;
            if (g.list_scroll > g.list_scroll_max) g.list_scroll = g.list_scroll_max;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g.help && g.help_max > 0) { // the help panel's text: the same, inside it
            g.help_target -= GET_WHEEL_DELTA_WPARAM(wp) * 80.0f / WHEEL_DELTA;
            g.help_target = std::min(std::max(g.help_target, 0.0f), g.help_max);
            SetTimer(h, kTimerScroll, 15, nullptr);
            return 0;
        }
        if (!g.list_open && !g.help && !g.ask && g.page_max > 0) { // the page: 80 pixels a notch, sliding there
            g.scroll_target -= GET_WHEEL_DELTA_WPARAM(wp) * 80.0f / WHEEL_DELTA;
            g.scroll_target = std::min(std::max(g.scroll_target, 0.0f), g.page_max);
            SetTimer(h, kTimerScroll, 15, nullptr);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN: { // a slider: dragged until the button is released
        int id = hit_at(GET_X_LPARAM(lp) * scale, GET_Y_LPARAM(lp) * scale);
        if ((id == kHitGain || id == kHitGamma) && !g.list_open && !g.help && !g.ask) {
            g.drag = id;
            SetCapture(h);
            drag_to(GET_X_LPARAM(lp) * scale);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g.drag) {
            drag_to(GET_X_LPARAM(lp) * scale);
            return 0;
        }
        int id = hit_at(GET_X_LPARAM(lp) * scale, GET_Y_LPARAM(lp) * scale);
        g.mouse_x = GET_X_LPARAM(lp) * scale, g.mouse_y = GET_Y_LPARAM(lp) * scale;
        int tip = -1;
        for (size_t i = g_tips.size(); i-- > 0;)
            if (inside(g_tips[i].r, g.mouse_x, g.mouse_y)) { tip = (int)i; break; }
        if (tip != g.tip || tip >= 0) InvalidateRect(h, nullptr, FALSE); // shown, moved with the mouse, or gone
        if (id != g.hover) {
            g.hover = id;
            SetCursor(LoadCursorW(nullptr, id == kHitDnsAddr ? (LPCWSTR)IDC_IBEAM : id >= 0 && id != 0x7fff ? (LPCWSTR)IDC_HAND : (LPCWSTR)IDC_ARROW));
            InvalidateRect(h, nullptr, FALSE);
        }
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        g.hover = -1;
        g.mouse_x = g.mouse_y = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, g.hover == kHitDnsAddr ? (LPCWSTR)IDC_IBEAM
                                           : g.hover >= 0 && g.hover != 0x7fff ? (LPCWSTR)IDC_HAND : (LPCWSTR)IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: // a key box waiting: Esc cancels, Delete clears, any other key binds
        if (g.capture >= 0) {
            uint16_t k = keys::from_message(wp, lp);
            if (k == VK_LMENU || k == VK_RMENU) return 0; // Alt: the game drops every key while it is held; still waiting
            if (k != VK_ESCAPE) kb_set(g.capture / 2, g.capture % 2, k == VK_DELETE ? 0 : k);
            g.capture = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g.list_open && (wp == VK_UP || wp == VK_DOWN)) {
            g.list_scroll += wp == VK_DOWN ? 1 : -1;
            if (g.list_scroll < 0) g.list_scroll = 0;
            if (g.list_scroll > g.list_scroll_max) g.list_scroll = g.list_scroll_max;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CHAR: // the InsigniaDNS address being typed: digits and dots; Enter keeps it, Esc drops it, Ctrl+V pastes
        if (g.dns_edit) {
            auto add = [](wchar_t c) {
                if (((c >= L'0' && c <= L'9') || c == L'.') && g.dns_text.size() < 15) g.dns_text += c;
            };
            wchar_t c = (wchar_t)wp;
            if (c == L'\r') dns_edit_end(true);
            else if (c == 27) dns_edit_end(false);
            else if (c == L'\b') {
                if (!g.dns_text.empty()) g.dns_text.pop_back();
            } else if (c == 22 && OpenClipboard(h)) {
                if (HANDLE d = GetClipboardData(CF_UNICODETEXT))
                    if (const wchar_t *p = (const wchar_t *)GlobalLock(d)) {
                        for (; *p; p++) add(*p);
                        GlobalUnlock(d);
                    }
                CloseClipboard();
            } else add(c);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONUP: {
        if (g.capture >= 0 && !g.drag) { // a click elsewhere cancels a waiting key box
            int cid = hit_at(GET_X_LPARAM(lp) * scale, GET_Y_LPARAM(lp) * scale);
            if (cid < kHitKey || cid >= kHitKey + keys::kInputs * 2) g.capture = -1;
        }
        if (g.drag) { // the slider's value is saved once it is let go
            char b[16];
            sprintf_s(b, "%.2f", g.drag_value);
            black_ini_set(g.drag == kHitGain ? "gain" : "gamma", b);
            g.drag = 0;
            ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        int id = hit_at(GET_X_LPARAM(lp) * scale, GET_Y_LPARAM(lp) * scale);
        if (id != kHitDnsAddr) dns_edit_end(true); // a click elsewhere ends the typing
        if (g.help) { // the panel is modal: it only closes
            if (id == kHitHelpClose) g.help = 0;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g.list_open) { // a drop-down list: a row chooses it, any click closes the list
            int row = id - g.list_open - 1;
            if (g.list_open == kHitGpu && row >= 0 && row < (int)gpu_choices().size())
                set_gpu_preference(gpu_choices()[row].pref);
            if (g.list_open == kHitMode && row >= 0 && row < 3) black_ini_set("fullscreen", row == 0 ? "0" : row == 1 ? "2" : "1");
            if (g.list_open == kHitAspect && row >= 0 && row < kAspectCount) row = kAspectShown[row]; // the list's order
            if (g.list_open == kHitAspect && row >= 0 && row < kAspectCount && row != aspect_index() && (row != kAspectMonitor || g_monitor_aspect_ok)) {
                black_ini_set("widescreen", row == 3 ? "0" : "1");
                if (row != 3) black_ini_set("aspect", std::to_string(kAspects[row][0]) + ":" + std::to_string(kAspects[row][1]));
                black_ini_set("window", "auto"); // a fixed size belongs to the other shape
            }
            if (g.list_open == kHitWin && row >= 0 && row < 5) {
                const int(*sizes)[2] = kWins[aspect_index()];
                black_ini_set("window", row ? std::to_string(sizes[row][0]) + "x" + std::to_string(sizes[row][1]) : std::string("auto"));
            }
            if (g.list_open >= kHitPadMap && g.list_open < kHitPadMap + keys::kButtons * 16 && row >= 0 && row < keys::kButtons)
                black_ini_set((std::string("padmap_") + keys::input_ini((g.list_open - kHitPadMap) / 16)).c_str(), keys::input_ini(row));
            if (g.list_open == kHitAniso && row >= 0 && row < 5) black_ini_set("aniso", std::to_string(kAnisoLevels[row]));
            if (g.list_open == kHitAa && row >= 0 && row < 5) { // off, FXAA, MSAA 2x / 4x / 8x
                black_ini_set("aa", row == 1 ? "1" : "0");
                black_ini_set("msaa", row >= 2 ? std::to_string(kMsaaLevels[row - 2]) : std::string("0"));
            }
            if (g.list_open == kHitSysLang && row >= 0 && row < 9) black_ini_set("language", std::to_string(row + 1));
            if (g.list_open == kHitShadow && row >= 0 && row < 4) black_ini_set("shadow", std::to_string(kShadowScales[row]));
            if (g.list_open == kHitResList && row == 0) {
                black_ini_set("render", "native");
                black_ini_set("render_factor", "1.00"); // the in-game PC SETTINGS can leave 1.5 or 2 (x the window)
            }
            if (g.list_open == kHitResList && row >= 1 && row <= kMaxScale) {
                black_ini_set("render", "scale");
                black_ini_set("scale", std::to_string(row));
            }
            if (g.list_open == kHitUninstall && row >= 0 && row < (int)g.content.size()) { // asked first
                const install::ContentItem &c = g.content[row];
                g.uninstall_id = c.id;
                g.uninstall_name = c.id == L"$u" ? std::wstring(S(Str::DlcUpdate)) : c.name;
                g.uninstall_size = content_size_text(c.bytes);
                g.ask = kAskUninstall;
            }
            g.list_open = 0;
            g.list_scroll = g.list_scroll_max = 0;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g.ask) { // a confirmation: yes does what it asked, no (or outside) closes
            int ask = g.ask;
            if (id == kHitAskYes || id == kHitAskNo || id == kHitAskAlt) g.ask = 0;
            if (ask == kAskUnsavedStart || ask == kAskUnsavedClose) { // save or not, then go on; outside: stay
                if (id == kHitAskYes) save_settings();
                if (id == kHitAskAlt) discard_settings();
                if (id == kHitAskYes || id == kHitAskAlt) {
                    if (ask == kAskUnsavedStart) launch_game();
                    else PostMessageW(h, WM_CLOSE, 0, 0);
                }
            } else if (id == kHitAskYes) {
                if (ask == kAskRestore || ask == kAskImport || ask == kAskExport || ask == kAskResign) saves_action(ask);
                else if (ask == kAskDelete) delete_image();
                else if (ask == kAskReset) reset_page(g.page);
                else if (ask == kAskUninstall) uninstall_now();
                else if (ask == kAskNewConsole) new_console();
                else if (ask == kAskRebuildConsole) rebuild_console();
                else start_install();
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (id == kHitHelpImage || id == kHitHelpInstalled || id == kHitHelpRegister || id == kHitHelpDlc ||
            (id >= kHitHelpSet && id < kHitHelpSet + kSettingHelpCount))
            g.help = id;
        else if (id == kHitModule || id == kHitModule + 1) switch_module(id - kHitModule);
        else if (id == kHitPickDlc) {
            std::wstring d;
            if (!installing() && pick(true, &d)) {
                g.dlc_dir = d;
                g.dlc_result.clear();
            }
        }
        else if (id == kHitInstallDlc && !g.dlc_dir.empty() && !installing()) install_dlc_now();
        else if (id == kHitExportDlc && !g.content.empty() && !installing()) export_dlc_now();
        else if (id >= 0 && id < kPageCount) {
            if (g.page != (Page)id) g.page_scroll = g.scroll_target = 0; // another page opens at its top
            g.page = (Page)id;
            if (g.page == kSaves) g.saves_read = false; // read the disk again when the page is opened
        }
        else if (id == kHitLangZh) set_lang(Lang::Zh);
        else if (id == kHitLangEn) set_lang(Lang::En);
        else if (id == kHitStart) {
            if (unsaved()) g.ask = kAskUnsavedStart; // the game reads the saved settings
            else launch_game();
        }
        else if (id == kHitSave) save_settings();
        else if (id >= kHitCtlTab && id < kHitCtlTab + 2) g.ctl_tab = id - kHitCtlTab;
        else if (id >= kHitKey && id < kHitKey + keys::kInputs * 2) {
            g.capture = id - kHitKey;
            SetFocus(h);
        }
        else if (id >= kHitPadMap && id < kHitPadMap + keys::kButtons * 16 && (id - kHitPadMap) % 16 == 0) {
            g.list_open = id;
            g.list_scroll = g.list_scroll_max = 0;
        }
        else if (id == kHitOpenSaves) open_folder(save_hdd());
        else if (id == kHitClearCache) clear_shader_cache();
        else if (id >= kHitWebsite && id < kHitWebsite + 5) ShellExecuteW(h, L"open", kLinks[id - kHitWebsite], nullptr, nullptr, SW_SHOWNORMAL);
        else if (id == kHitOpenBackups) {
            CreateDirectoryW(backup_root().c_str(), nullptr);
            open_folder(backup_root());
        }
        else if (id == kHitBackupNow) saves_action(0);
        else if (id == kHitResign) g.ask = kAskResign;
        else if (id >= kHitRestore && id < kHitRestore + 3) {
            g.restore_index = id - kHitRestore;
            if (game_running()) saves_result(false, S(Str::GameRunning));
            else g.ask = kAskRestore;
        }
        else if (id == kHitPickXrevive) {
            std::wstring dir;
            if (pick(true, &dir)) {
                ini_set(L"xrevive", dir);
                g.save_status.clear();
            }
        }
        else if (id == kHitImport || id == kHitExport) {
            if (game_running()) saves_result(false, S(Str::GameRunning));
            else g.ask = id == kHitImport ? kAskImport : kAskExport;
        }
        else if (id == kHitDiscard) discard_settings();
        else if (id == kHitPickIso) choose_image();
        else if (id == kHitInstall) request_install();
        else if (id == kHitCancel) g.prog.cancel = true;
        else if (id == kHitDeleteIso && g.offer_delete) g.ask = kAskDelete;
        else if (id == kHitPickGame) choose_existing();
        else if (id == kHitDefaultPath && !installing()) set_game_folder(default_game_folder());
        else if (id == kHitResetPage) g.ask = kAskReset;
        else if (id >= kHitSeam && id < kHitSeam + 2) black_ini_set("seamfill", id == kHitSeam ? "1" : "0");
        else if (id >= kHitFx && id < kHitFx + 2) black_ini_set("effectsnative", id == kHitFx ? "1" : "0");
        else if (id >= kHitVsync && id < kHitVsync + 2) black_ini_set("vsync", id == kHitVsync ? "1" : "0");
        else if (id >= kHitFsRes && id < kHitFsRes + 2) black_ini_set("fullscreen_res", id == kHitFsRes ? "screen" : "window");
        else if (id >= kHitCpuMode && id < kHitCpuMode + 2) {
            g_pending.erase("multicore");
            black_ini_write("multicore", id == kHitCpuMode ? "0" : "1");
        }
        else if (id >= kHitShadowSoft && id < kHitShadowSoft + 2) black_ini_set("shadowsoft", id == kHitShadowSoft ? "1" : "0");
        else if (id >= kHitAudioMode && id < kHitAudioMode + 3)
            black_ini_set("audio_mode", id == kHitAudioMode ? "mono" : id == kHitAudioMode + 1 ? "stereo" : "surround");
        else if (id >= kHitDigital && id < kHitDigital + 4) {
            int k = id - kHitDigital; // off, Dolby Digital, DTS, both
            black_ini_set("audio_ac3", k == 1 || k == 3 ? "1" : "0");
            black_ini_set("audio_dts", k >= 2 ? "1" : "0");
        }
        else if (id >= kHitHud && id < kHitHud + 2) black_ini_set("perfhud", id == kHitHud ? "1" : "0");
        else if (id >= kHitOnline && id < kHitOnline + 2) black_ini_set("online", id == kHitOnline ? "1" : "0");
        else if (id >= kHitDnsMode && id < kHitDnsMode + 2) {
            black_ini_set("online_dns_mode", id == kHitDnsMode ? "insignia" : "insigniadns");
            if (id != kHitDnsMode && black_ini("online_insigniadns").empty()) dns_find(); // first time: this PC's
        }
        else if (id == kHitDnsAddr && !g.dns_edit) {
            g.dns_edit = true;
            g.dns_text = from_utf8(black_ini("online_insigniadns"));
            g.dns_note.clear();
            SetFocus(h);
        }
        else if (id == kHitDnsFind) dns_find();
        else if (id == kHitPickEeprom || id == kHitPickHdd) {
            COMDLG_FILTERSPEC spec = {S(Str::OnlineFilter), id == kHitPickEeprom ? L"*.bin;*.rom" : L"*.qcow2;*.img;*.bin"};
            std::wstring f;
            if (pick(false, &f, &spec)) {
                black_ini_set(id == kHitPickEeprom ? "online_eeprom" : "online_hdd", to_utf8(f));
                g.import_result.clear(); // it was about the files before
            }
        }
        else if (id == kHitNetCheck) net_check();
        else if (id == kHitNewConsole) g.ask = kAskNewConsole;
        else if (id == kHitRebuildConsole) g.ask = kAskRebuildConsole;
        else if (id == kHitRegister) register_console();
        else if (id == kHitDashboard) open_dashboard();
        else if (id == kHitImportXemu) {
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            online_import();
        }
        else if (id == kHitInsignia) ShellExecuteW(h, L"open", L"https://insignia.live/", nullptr, nullptr, SW_SHOWNORMAL);
        else if (id == kHitGpu || id == kHitResList || id == kHitMode || id == kHitAspect || id == kHitWin || id == kHitShadow || id == kHitAniso || id == kHitAa || id == kHitSysLang || id == kHitUninstall) {
            g.list_open = id;
            g.list_scroll = g.list_scroll_max = 0;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerInstall && !installing()) install_finished();
        if (wp == kTimerScroll) { // a third of the way each tick: quick at first, then settling
            float d = g.scroll_target - g.page_scroll, e = g.help_target - g.help_scroll;
            g.page_scroll = std::fabs(d) < 0.5f ? g.scroll_target : g.page_scroll + d * 0.33f;
            g.help_scroll = std::fabs(e) < 0.5f ? g.help_target : g.help_scroll + e * 0.33f;
            if (g.page_scroll == g.scroll_target && g.help_scroll == g.help_target) KillTimer(h, kTimerScroll);
        }
        if (wp == kTimerSetup && g.setup_proc && WaitForSingleObject(g.setup_proc, 0) == WAIT_OBJECT_0) {
            CloseHandle(g.setup_proc); // the Setup Assistant ended: what it registered shows now
            g.setup_proc = nullptr;
            KillTimer(h, kTimerSetup);
            g.online_for.clear();
            online_check();
            // the dashboard closed and the console still has no machine account: its network test did not get through
            if (g.setup_is_dash && online_value("local") == "ok" && online_value("local_registered") != "1")
                g.local_result = S(Str::DashNoMachine);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        if (unsaved()) { // ask first: save, don't save, or stay
            g.ask = kAskUnsavedClose;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (installing()) { // stop the worker before the window goes (a half-written file stays: the next install overwrites it)
            g.prog.cancel = true;
            if (g.worker.joinable()) g.worker.join();
        }
        DestroyWindow(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// --snap: the window drawn off screen into a PNG
int snap(const wchar_t *out, int page)
{
    g.page = (Page)std::min(std::max(page, 0), kPageCount - 1);
    IWICBitmap *bmp = nullptr;
    ID2D1RenderTarget *rt = nullptr;
    const UINT W = (UINT)kW, H = (UINT)kH;
    if (FAILED(g.wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return 1;
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(g.d2d->CreateWicBitmapRenderTarget(bmp, p, &rt))) return 1;
    ID2D1Bitmap *hero = to_bitmap(rt, g.module == 1 && g.hero_ng04_src ? g.hero_ng04_src : g.hero_src), *texture = to_bitmap(rt, g.texture_src);
    ID2D1Bitmap *pay[2] = {to_bitmap(rt, g.pay_src[0]), to_bitmap(rt, g.pay_src[1])};
    rt->BeginDraw();
    Painter pt{rt, hero, texture, pay};
    pt.draw();
    HRESULT hr = rt->EndDraw();
    IWICStream *st = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *fr = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(g.wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(out, GENERIC_WRITE)) &&
        SUCCEEDED(g.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
        SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->WriteSource(bmp, nullptr)) && SUCCEEDED(fr->Commit()))
        hr = enc->Commit();
    release(fr);
    release(enc);
    release(st);
    release(hero);
    release(texture);
    for (auto *b : pay) release(b);
    release(rt);
    release(bmp);
    return SUCCEEDED(hr) ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    init_monitor_aspect();
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g.dir = exe;
    g.dir = g.dir.substr(0, g.dir.find_last_of(L"\\/"));
    g.ini = g.dir + L"\\launcher.ini";
    std::wstring lang = ini_get(L"lang");
    if (lang.empty()) g_lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Lang::Zh : Lang::En;
    else g_lang = lang == L"en" ? Lang::En : Lang::Zh;
    g.module = ini_get(L"module") == L"ng04" ? 1 : 0;
    saves::set_title(ng04() ? L"54430003" : L"5443000d");
    g.game = ini_get(game_key(), (default_game_folder() + L"\\default.xbe").c_str());
    set_dest(folder_of(g.game)); // installs go to the game folder
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g.d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&g.dw)) ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g.wic))))
        return 1;
    g.hero_src = load_jpeg(IDR_HERO);
    g.hero_ng04_src = load_jpeg(IDR_HERO_NG04);
    g.texture_src = load_jpeg(IDR_TEXTURE);
    g.pay_src[0] = load_jpeg(IDR_PAY_WECHAT);
    g.pay_src[1] = load_jpeg(IDR_PAY_ALIPAY);

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--resign") && i + 3 < argc) { // tests: --resign <hdd> <title id folder> <backup root>
            if (AttachConsole(ATTACH_PARENT_PROCESS)) freopen("CONOUT$", "w", stdout);
            saves::set_title(argv[i + 2]);
            int n = 0;
            std::wstring made, err;
            bool ok = saves::resign(saves::our_root(argv[i + 1]), argv[i + 3], &n, &made, &err);
            wprintf(L"resign: %ls, %d files signed again, backup %ls %ls\n", ok ? L"ok" : L"failed", n, made.c_str(), err.c_str());
            fflush(stdout);
            return ok ? 0 : 1;
        }
        if (!wcscmp(argv[i], L"--extract") && i + 2 < argc) {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) freopen("CONOUT$", "w", stdout);
            install::Progress pr;
            DWORD t0 = GetTickCount();
            install::extract(argv[i + 1], argv[i + 2], pr);
            wprintf(L"state %d, %d of %d files, %llu of %llu bytes, %.1f s: %ls\n", pr.state.load(), pr.files.load(),
                    pr.file_count.load(), (unsigned long long)pr.done.load(), (unsigned long long)pr.total.load(),
                    (GetTickCount() - t0) / 1000.0, pr.message.c_str());
            fflush(stdout);
            return pr.state == 2 ? 0 : 1;
        }
        if (!wcscmp(argv[i], L"--iso") && i + 1 < argc) { // with --snap: show this image chosen on the install page
            g.iso = argv[i + 1];
            std::wstring err;
            g.iso_need = install::image_size(g.iso, &err);
        }
        if (!wcscmp(argv[i], L"--show-ask") && i + 1 < argc) g.ask = _wtoi(argv[i + 1]); // with --snap: a confirmation (4 reset)
        if (!wcscmp(argv[i], L"--tab") && i + 1 < argc) g.ctl_tab = _wtoi(argv[i + 1]); // with --snap: the Controls page's tab
        if (!wcscmp(argv[i], L"--show-list") && i + 1 < argc) { // with --snap: a drop-down list open (gpu | res | mode | aspect | window | padmapN)
            const wchar_t *v = argv[i + 1];
            if (!wcsncmp(v, L"padmap", 6) && v[6]) g.list_open = kHitPadMap + _wtoi(v + 6) * 16;
            else g.list_open = !wcscmp(v, L"gpu")    ? kHitGpu
                            : !wcscmp(v, L"mode")   ? kHitMode
                            : !wcscmp(v, L"aspect") ? kHitAspect
                            : !wcscmp(v, L"window") ? kHitWin
                            : !wcscmp(v, L"shadow") ? kHitShadow
                            : !wcscmp(v, L"aniso")  ? kHitAniso
                            : !wcscmp(v, L"aa")     ? kHitAa
                            : !wcscmp(v, L"language") ? kHitSysLang
                            : !wcscmp(v, L"padmap") ? kHitPadMap
                            : !wcscmp(v, L"uninstall") ? kHitUninstall
                                                    : kHitResList;
        }
        if (!wcscmp(argv[i], L"--show-help") && i + 1 < argc) // with --snap: open a "?" panel (image | installed | register | 0..12 a setting's)
            g.help = !wcscmp(argv[i + 1], L"image")       ? kHitHelpImage
                     : !wcscmp(argv[i + 1], L"installed") ? kHitHelpInstalled
                     : !wcscmp(argv[i + 1], L"register")  ? kHitHelpRegister
                                                          : kHitHelpSet + _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--game") && i + 1 < argc) { // with --snap: this game folder
            g.game = std::wstring(argv[i + 1]) + L"\\default.xbe";
            set_dest(argv[i + 1]);
        }
        if (!wcscmp(argv[i], L"--module") && i + 1 < argc) { // with --snap; the saved choice stays as it was
            std::wstring keep = ini_get(L"module", L"black");
            switch_module(!wcscmp(argv[i + 1], L"ng04") ? 1 : 0);
            ini_set(L"module", keep);
        }
        if (!wcscmp(argv[i], L"--lang") && i + 1 < argc) g_lang = !wcscmp(argv[i + 1], L"en") ? Lang::En : Lang::Zh;
        if (!wcscmp(argv[i], L"--scroll") && i + 1 < argc) g.page_scroll = g.scroll_target = (float)_wtof(argv[i + 1]); // with --snap: the page scrolled down
        if (!wcscmp(argv[i], L"--help-scroll") && i + 1 < argc) g.help_for = g.help, g.help_scroll = g.help_target = (float)_wtof(argv[i + 1]); // with --snap, after --show-help
        if (!wcscmp(argv[i], L"--mouse") && i + 2 < argc) g.mouse_x = (float)_wtof(argv[i + 1]), g.mouse_y = (float)_wtof(argv[i + 2]); // with --snap: the mouse there (a cut text's tip)
        if (!wcscmp(argv[i], L"--mdl2")) g_icon_font = L"Segoe MDL2 Assets";
        if (!wcscmp(argv[i], L"--monitor") && i + 1 < argc) { // with --snap: as if the monitor were WxH (the shape list)
            int w = 0, h = 0;
            if (swscanf_s(argv[i + 1], L"%dx%d", &w, &h) == 2) init_monitor_aspect(w, h);
        }
    }
    check_game();
    for (int i = 1; i < argc; i++)
        if (!wcscmp(argv[i], L"--import-now")) online_import(); // with --snap, before it on the line: the Import button pressed
        else if (!wcscmp(argv[i], L"--dash-now")) open_dashboard(); // the same, the Xbox Live button
        else if (!wcscmp(argv[i], L"--rebuild-now")) rebuild_console(); // the same, Rebuild (confirmed)
        else if (!wcscmp(argv[i], L"--snap") && i + 1 < argc)
            return snap(argv[i + 1], i + 2 < argc && argv[i + 2][0] != L'-' ? _wtoi(argv[i + 2]) : 0);

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.lpszClassName = L"NgbLauncher";
    RegisterClassExW(&wc);
    // the window at 1180x700 DIPs, centred on the primary monitor's work area
    UINT dpi = GetDpiForSystem();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, MulDiv((int)kW, dpi, 96), MulDiv((int)kH, dpi, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, S(ng04() ? Str::WindowTitleNg04 : Str::WindowTitle), style, work.left + std::max(0, (int)(work.right - work.left - w) / 2),
                             work.top + std::max(0, (int)(work.bottom - work.top - hgt) / 2), w, hgt, nullptr, nullptr, inst, nullptr);
    ShowWindow(g.hwnd, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
