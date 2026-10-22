// Ninja Gaiden Black HurricaneX21 launcher: starts the game and (in later steps) installs it, sets the display, the
// graphics card, the controls and manages the saves (project/docs/08). The window is drawn with Direct2D and
// DirectWrite in the colours and art of the project's website (launcher/art/SOURCES.md).
//
// --snap OUT.png [PAGE]: draw the window at 1180x700 into OUT.png and exit (screenshots for review).
// --extract IMAGE DEST: install from IMAGE into DEST without the window (tests); the result on the console.
#include "install.h"
#include "resource.h"
#include "strings.h"

#include <windows.h>
#include <windowsx.h>
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
#include <string>
#include <thread>
#include <vector>

namespace {

// Layout, in DIPs
constexpr float kW = 1180, kH = 700, kNavW = 236;

// The website's palette (:root of akirahayabusa.github.io/ngb-hurricane-patch)
D2D1_COLOR_F rgb(uint32_t v, float a = 1.0f) { return D2D1::ColorF(v, a); }
const uint32_t kInk = 0x0b0a0d, kInk3 = 0x2a2630, kBone = 0xece7dd, kAsh = 0xa39c92, kBlood = 0xcf3640, kGold = 0xcfa650;

enum Page { kStart, kInstall, kDisplay, kGraphics, kControls, kSaves, kAbout, kPageCount };
const wchar_t *const kIcons[kPageCount] = {L"\xE768", L"\xE896", L"\xE7F4", L"\xE790", L"\xE7FC", L"\xE74E", L"\xE946"};

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
    Page page = kStart;
    int hover = -1; // hit id under the mouse
    bool pressed = false;
    std::wstring dir;  // the launcher's folder
    std::wstring ini;  // launcher.ini
    std::wstring game; // default.xbe

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
    int help = 0; // the "?" panel open: 0 none, else its hit id
    int ask = 0;  // the confirmation open: 0 none, else kAsk*
    int list_open = 0; // the drop-down list open: 0 none, else its hit id (kHitResList, kHitGpu)
    int drag = 0;         // the slider being dragged (kHitGain, kHitGamma), its value until the button is released
    float drag_value = 1.0f;
};
enum { kAskOverwrite = 1, kAskNonEmpty, kAskDelete, kAskReset };
App g;

// Hit ids: 0..kPageCount-1 the navigation, then these
enum {
    kHitStart = 100, kHitLangZh, kHitLangEn, kHitPickIso, kHitPickDest, kHitInstall, kHitCancel, kHitDeleteIso, kHitPickGame,
    kHitHelpImage, kHitHelpInstalled, kHitHelpClose, kHitAskYes, kHitAskNo, kHitDefaultPath, kHitResetPage,
    kHitMode = 200, kHitAspect = 210, kHitAa = 230, kHitSeam = 240, kHitFx = 250, kHitGpu = 260, kHitListClose = 269,
    kHitGain = 270, kHitGamma, kHitWin = 280, kHitVsync = 290, kHitHud = 294, kHitResList = 300, kHitShadow = 320
};
// Render resolution: "the window's size" or 1x to 10x the game's 720x480 buffer (the game asks for 720x480 at
// CreateDevice; black.log). Widescreen widens the width by 4/3 (anamorphic); a width is at most 8192, Direct3D 9's
// texture limit (loader d3d/device.cpp setup_render_scale: 10x widescreen renders 8192x4800, r289).
constexpr int kMaxScale = 10;
std::wstring render_size(double scale, bool wide)
{
    int w = (int)std::lround(std::min(720.0 * scale * (wide ? 4.0 / 3.0 : 1.0), 8192.0)), h = (int)std::lround(480.0 * scale);
    return std::to_wstring(w) + L"\u00D7" + std::to_wstring(h);
}
// Shadow quality: the shadow map's scale for each level (low = the Xbox's 128x128)
const int kShadowScales[4] = {1, 4, 8, 16};
// Brightness and gamma sliders: the loader's range (settings.cpp clamps gain and gamma to 0.5 - 3.0), 0.05 steps
constexpr float kSliderMin = 0.5f, kSliderMax = 3.0f, kSliderW = 260;
// The window sizes offered (client area) for 16:9 and for 4:3; 0 x 0 = auto (the largest that fits the screen)
const int kWin169[][2] = {{0, 0}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
const int kWin43[][2] = {{0, 0}, {960, 720}, {1280, 960}, {1440, 1080}, {1920, 1440}};
constexpr UINT_PTR kTimerInstall = 1;
struct Hit {
    int id;
    D2D1_RECT_F r;
};
std::vector<Hit> g_hits;

bool inside(const D2D1_RECT_F &r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// --- settings ---------------------------------------------------------------------------------------------------

std::string black_ini(const char *key);
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

std::wstring game_exe() { return g.dir + L"\\Ninja Gaiden Black HurricaneX21.exe"; }

// The default game folder: game\x21 next to the launcher (the layout run.bat and the README describe)
std::wstring default_game_folder() { return g.dir + L"\\game\\x21"; }

bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// black.ini next to the game program: one "key = value" per line
std::string black_ini(const char *key)
{
    FILE *f = _wfopen((g.dir + L"\\black.ini").c_str(), L"r");
    if (!f) return "";
    char line[512];
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

// Sets key = value in black.ini, keeping every other line (the game writes the file on exit too)
void black_ini_set(const char *key, const std::string &value)
{
    std::wstring path = g.dir + L"\\black.ini";
    std::vector<std::string> lines;
    FILE *f = _wfopen(path.c_str(), L"r");
    if (f) {
        char line[512];
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

void set_gpu_preference(int v) // 2 high performance, 1 power saving, 0 let Windows decide
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
int gpu_preference()
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
int gpu_choice_index()
{
    const std::vector<GpuChoice> &v = gpu_choices();
    int pref = gpu_preference();
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
    if (!exists(game_exe()) || !exists(g.game)) {
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

// The game folder's check (5 SHA-256), redone only when the folder changes
void check_game()
{
    std::wstring dir = folder_of(g.game);
    if (dir == g.checked_game) return;
    g.checked_game = dir;
    g.game_ok = exists(g.game) && install::check_game(dir, &g.game_detail);
    if (!exists(g.game)) g.game_detail.clear();
}

bool pick(bool folder, std::wstring *out)
{
    IFileOpenDialog *d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
    DWORD opt = 0;
    d->GetOptions(&opt);
    d->SetOptions(opt | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    COMDLG_FILTERSPEC spec[] = {{S(Str::ImageFilter), L"*.iso"}, {L"*.*", L"*.*"}};
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
        ini_set(L"game", g.game);
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

// The game folder ("Change"): where an install goes and where the game starts from
void set_game_folder(const std::wstring &dir)
{
    g.game = dir + L"\\default.xbe";
    ini_set(L"game", g.game);
    set_dest(dir);
    g.checked_game.clear();
    check_game();
}

void choose_existing()
{
    std::wstring dir;
    if (!installing() && pick(true, &dir)) set_game_folder(dir);
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

struct Painter {
    ID2D1RenderTarget *rt;
    ID2D1Bitmap *hero = nullptr, *texture = nullptr;
    std::function<void()> overlay; // an open drop-down list, drawn after the page

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
            layout->Release();
        }
        release(dots);
        fmt->Release();
    }

    void fill(D2D1_RECT_F r, uint32_t color, float alpha = 1.0f, float radius = 0.0f)
    {
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
        text(L"NINJA GAIDEN BLACK", {29, 74, kNavW - 12, 94}, 11.5f, kGold, 1.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI",
             DWRITE_TEXT_ALIGNMENT_LEADING, 2.2f);
        text(S(Str::Edition), {29, 96, kNavW - 12, 118}, 12.5f, kAsh);
        fill({28, 134, 64, 136}, kBlood);
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
            uint32_t c = sel || hov ? kBone : kAsh;
            text(kIcons[i], {r.left + 18, r.top, r.left + 44, r.bottom}, 17, sel ? kBlood : c, 1.0f, DWRITE_FONT_WEIGHT_NORMAL,
                 L"Segoe Fluent Icons");
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
        std::string fs = black_ini("fullscreen"), win = black_ini("window");
        std::wstring s = S(fs == "2" ? Str::ModeFullscreen : fs == "1" ? Str::ModeBorderless : Str::ModeWindow);
        if (fs != "1" && fs != "2" && !win.empty() && win != "auto") {
            std::wstring w(win.begin(), win.end());
            size_t x = w.find(L'x');
            if (x != std::wstring::npos) w[x] = L'\u00D7';
            s += L" " + w;
        }
        return s + L"  \x00B7  " + (black_ini("widescreen") == "0" ? L"4:3" : L"16:9");
    }

    void start_page()
    {
        const float x = kNavW + 56;
        text(L"NINJA GAIDEN BLACK  \x00B7  HURRICANE X21", {x, 104, x + 600, 126}, 12.5f, kGold, 1.0f,
             DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI", DWRITE_TEXT_ALIGNMENT_LEADING, 2.6f);
        // the title with a soft shadow (the website's h1: text-shadow 0 2px 20px #000)
        for (int i = 0; i < 3; i++)
            text(S(Str::Title), {x + 1.0f + i, 132.0f + 2 + i, x + 760, 196.0f + 2 + i}, 46, 0x000000, 0.28f,
                 DWRITE_FONT_WEIGHT_BLACK, L"Microsoft YaHei UI");
        text(S(Str::Title), {x, 132, x + 760, 196}, 46, kBone, 1.0f, DWRITE_FONT_WEIGHT_BLACK, L"Microsoft YaHei UI");
        text(S(Str::Subtitle), {x, 200, x + 640, 228}, 17, kAsh);

        // the start button
        bool ready = exists(game_exe()) && exists(g.game);
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
             L"Segoe Fluent Icons");
        text(S(Str::Play), {b.left + 58, b.top, b.right - 16, b.bottom}, 20, ready ? kBone : kAsh, 1.0f, DWRITE_FONT_WEIGHT_BOLD,
             nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, g_lang == Lang::Zh ? 4.0f : 1.0f);
        if (!ready) text(S(Str::NotReady), {b.right + 20, b.top, b.right + 420, b.bottom}, 13.5f, kAsh);

        // the summary card
        D2D1_RECT_F c = {x, 372, x + 620, 372 + 3 * 46 + 24};
        fill(c, kInk, 0.62f);
        stroke(c, kInk3);
        fill({c.left, c.top, c.left + 3, c.bottom}, kGold, 0.8f);
        std::wstring rows[3][2] = {{S(Str::Game), ready ? folder_of(g.game) : S(Str::GameMissing)},
                                   {S(Str::Gpu), gpu_label(gpu_choices()[gpu_choice_index()])},
                                   {S(Str::Display), display_summary()}};
        for (int i = 0; i < 3; i++) {
            float y = c.top + 12 + i * 46;
            text(rows[i][0], {c.left + 26, y, c.left + 130, y + 46}, 13.5f, kAsh);
            text(rows[i][1], {c.left + 132, y, c.right - 20, y + 46}, 14.5f, kBone, 1.0f, DWRITE_FONT_WEIGHT_NORMAL, nullptr,
                 DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
            if (i < 2) fill({c.left + 26, y + 46, c.right - 20, y + 47}, kInk3);
        }
        text(S(Str::Footer), {x, kH - 44, x + 700, kH - 20}, 12, kAsh, 0.75f);
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
        if (help) {
            float hx = c.left + 26 + measure(heading, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 10;
            D2D1_RECT_F q = {hx, c.top + 17, hx + 20, c.top + 37};
            bool hov = g.hover == help;
            g_hits.push_back({help, {q.left - 4, q.top - 4, q.right + 4, q.bottom + 4}});
            ID2D1SolidColorBrush *b = nullptr;
            rt->CreateSolidColorBrush(rgb(hov ? kGold : kAsh, hov ? 1.0f : 0.8f), &b);
            if (b) {
                D2D1_ELLIPSE e = {{(q.left + q.right) / 2, (q.top + q.bottom) / 2}, 9.5f, 9.5f};
                if (hov) {
                    ID2D1SolidColorBrush *f = nullptr;
                    rt->CreateSolidColorBrush(rgb(kGold, 0.16f), &f);
                    if (f) rt->FillEllipse(e, f), f->Release();
                }
                rt->DrawEllipse(e, b, 1.2f);
                b->Release();
            }
            text(L"?", q, 12.5f, hov ? kGold : kAsh, 1.0f, DWRITE_FONT_WEIGHT_BOLD, L"Segoe UI", DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        return c.top + 50;
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
                 : g.ask == kAskReset     ? (g.page == kDisplay ? Str::ResetDisplay : Str::ResetGraphics)
                                          : Str::DeleteConfirm;
        const float pw = 560, ph = 300, px = (kW - pw) / 2, py = (kH - ph) / 2;
        D2D1_RECT_F p = {px, py, px + pw, py + ph};
        fill(p, 0x16141a, 0.98f, 6);
        stroke(p, kInk3, 1.0f, 6);
        fill({p.left, p.top + 6, p.left + 3, p.bottom - 6}, g.ask == kAskDelete ? kBlood : kGold, 0.85f);
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
            const wchar_t *s = S(body);
            if (b) rt->DrawText(s, (UINT32)wcslen(s), fmt, {p.left + 34, p.top + 78, p.right - 34, p.bottom - 74}, b), b->Release();
            fmt->Release();
        }
        button(kHitAskNo, {p.right - 34 - 120 - 12 - 160, p.bottom - 56, p.right - 34 - 120 - 12, p.bottom - 20}, S(Str::Cancel), false);
        button(kHitAskYes, {p.right - 34 - 120, p.bottom - 56, p.right - 34, p.bottom - 20},
               S(g.ask == kAskDelete ? Str::MoveToBin : g.ask == kAskReset ? Str::ResetYes : Str::Continue), true);
    }

    // The "?" panel over the window
    void help_panel()
    {
        fill({0, 0, kW, kH}, kInk, 0.72f);
        g_hits.push_back({kHitHelpClose, {0, 0, kW, kH}}); // a click outside the panel closes it (the panel's own hits come later)
        bool image = g.help == kHitHelpImage;
        const float pw = 620, ph = 420, px = (kW - pw) / 2, py = (kH - ph) / 2;
        D2D1_RECT_F p = {px, py, px + pw, py + ph};
        fill(p, 0x16141a, 0.98f, 6);
        stroke(p, kInk3, 1.0f, 6);
        fill({p.left, p.top + 6, p.left + 3, p.bottom - 6}, kGold, 0.85f);
        g_hits.push_back({0x7fff, p}); // inside the panel: nothing (the topmost hit wins over the close area)
        text(S(image ? Str::FromImage : Str::InstalledGame), {p.left + 34, p.top + 22, p.right - 30, p.top + 58}, 19, kBone, 1.0f,
             DWRITE_FONT_WEIGHT_BOLD);
        fill({p.left + 34, p.top + 62, p.left + 64, p.top + 64}, kBlood);
        D2D1_RECT_F body = {p.left + 34, p.top + 78, p.right - 34, p.bottom - 74};
        IDWriteTextFormat *fmt = nullptr;
        if (SUCCEEDED(g.dw->CreateTextFormat(g_lang == Lang::Zh ? L"Microsoft YaHei UI" : L"Segoe UI", nullptr,
                                             DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14,
                                             g_lang == Lang::Zh ? L"zh-cn" : L"en-us", &fmt))) {
            fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 23, 18);
            ID2D1SolidColorBrush *b = nullptr;
            rt->CreateSolidColorBrush(rgb(kBone, 0.92f), &b);
            const wchar_t *s = S(image ? Str::HelpFromImage : Str::HelpInstalled);
            if (b) rt->DrawText(s, (UINT32)wcslen(s), fmt, body, b), b->Release();
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
                   : g.game_ok ? std::wstring(S(Str::GameChecked)) : std::wstring(S(Str::GameWrong)) + g.game_detail,
             {c2.left + 26, y2 + 36, c2.right - 20, y2 + 64}, 12.5f, !have ? kAsh : g.game_ok ? kGold : kBlood);
    }

    // A row of options; returns nothing, registers base + i for each
    void segmented(int base, float x, float y, const std::vector<std::wstring> &opts, int sel, float w = 104)
    {
        for (size_t i = 0; i < opts.size(); i++) {
            D2D1_RECT_F r = {x + i * (w + 6), y, x + i * (w + 6) + w, y + 30};
            bool on = (int)i == sel, hov = g.hover == base + (int)i;
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
        text(S(Str::AppliesNext), {x + 60, 88, x + w, 112}, 12.5f, kAsh, 0.85f);
        button(kHitResetPage, {x + w - 140, 70, x + w, 104}, S(Str::ResetPage), false); // this page only (reset_page)
        *card_out = {x, 124, x + w, 124 + 50 + rows * 40.0f + 8};
        return card(*card_out, S(heading));
    }

    // Display: how the picture reaches the screen
    void display_page()
    {
        D2D1_RECT_F c;
        float y = settings_page(Str::NavDisplay, Str::Picture, 6, &c); // 5 rows and the note under the sliders
        const float lx = c.left + 26, cx = c.left + 210;
        auto label = [&](Str s) { text(S(s), {lx, y, cx - 10, y + 30}, 13.5f, kAsh); };
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
        label(Str::Brightness);
        slider(kHitGain, cx, y, slider_value(kHitGain));
        y += 40;
        label(Str::Gamma);
        slider(kHitGamma, cx, y, slider_value(kHitGamma));
        text(S(Str::GainNote), {cx, y + 34, right, y + 56}, 12, kAsh, 0.75f);
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
        std::vector<ListRow> rows = {{L"16:9", S(Str::Aspect169Note)}, {L"4:3", S(Str::Aspect43Note)}};
        int sel = black_ini("widescreen") == "0" ? 1 : 0;
        drop_list(kHitAspect, x, y, {200, 300, 80, 0}, {rows[sel].a, L"", rows[sel].b}, rows, sel, S(Str::AspectSoon));
    }

    // Window size (the client area, used in window mode): auto = what the game picks, the largest whole multiple of
    // the aspect within 90% of the desktop's work area (loader d3d/host.cpp open_window), or a fixed size of the
    // chosen aspect; sizes larger than the work area cannot be chosen
    void window_list(float x, float y, float right)
    {
        bool wide = black_ini("widescreen") != "0";
        const int(*sizes)[2] = wide ? kWin169 : kWin43;
        const int aw = wide ? 16 : 4, ah = wide ? 9 : 3;
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
        std::vector<ListRow> rows;
        int sel = 0;
        for (int i = 0; i < 5; i++) {
            if (!i) {
                rows.push_back({S(Str::SizeAuto), S(Str::SizeAutoNote), dims(auto_w, auto_h)});
                continue;
            }
            bool fits = sizes[i][0] <= work.right - work.left && sizes[i][1] <= work.bottom - work.top;
            ListRow r = {dims(sizes[i][0], sizes[i][1]), fits ? L"" : S(Str::TooLarge)};
            r.off = !fits;
            rows.push_back(r);
            if (cur == std::to_string(sizes[i][0]) + "x" + std::to_string(sizes[i][1])) sel = i;
        }
        ListRow shown = sel ? rows[sel] : ListRow{S(Str::SizeAuto), L"", dims(auto_w, auto_h)};
        drop_list(kHitWin, x, y, {200, 380, 110, 270}, shown, rows, sel, S(Str::WindowOnly));
    }

    // Graphics: how the picture is made
    void graphics_page()
    {
        D2D1_RECT_F c;
        float y = settings_page(Str::NavGraphics, Str::Rendering, 7, &c);
        const float lx = c.left + 26, cx = c.left + 210;
        auto label = [&](Str s) { text(S(s), {lx, y, cx - 10, y + 30}, 13.5f, kAsh); };
        label(Str::Resolution);
        resolution_list(cx, y - 2, c.right - 26);
        y += 40;
        label(Str::ShadowQuality);
        shadow_list(cx, y - 2);
        y += 40;
        label(Str::AntiAlias);
        segmented(kHitAa, cx, y, {S(Str::On), S(Str::Off)}, black_ini("aa") == "0" ? 1 : 0, 72);
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
        float y2 = card(c2, S(Str::GpuCard));
        gpu_list(cx, y2, c2.right - 26);
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
        bool wide = black_ini("widescreen") != "0";
        std::string rd = black_ini("render"), sc = black_ini("scale");
        double scale = sc.empty() ? 3.0 : atof(sc.c_str());
        std::vector<ListRow> rows;
        rows.push_back({S(Str::ResWindow), S(Str::ResWindowNote), L"", 0});
        for (int i = 1; i <= kMaxScale; i++) {
            std::wstring a = std::to_wstring(i) + L"\u00D7";
            if (i == 1) a += S(Str::ResOriginal);
            if (i == 3) a += S(Str::ResDefault);
            rows.push_back({a, L"16:9  " + render_size(i, true), L"4:3  " + render_size(i, false), wide ? 1 : 2});
        }
        int sel = -1;
        ListRow shown;
        if (rd == "native") sel = 0;
        else if (rd.empty() || rd == "scale") {
            if (scale == std::floor(scale) && scale >= 1 && scale <= kMaxScale) sel = (int)scale;
            wchar_t b[16];
            swprintf(b, 16, L"%g\u00D7", scale);
            shown = {b, L"", render_size(scale, wide), 0};
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
        drop_list(kHitGpu, x, y, {380, 380, 0, 0}, rows[sel], rows, sel, L"");
    }

    // One row of a list: the choice on the left; b in a column, c on the right (strong: 1 or 2 = that one brighter)
    // One row of an open list: a, then b and c in the shape's columns
    void list_row(D2D1_RECT_F r, const ListRow &row, bool on, const ListShape &sh)
    {
        const float dim = row.off ? 0.4f : 1.0f, l = r.left;
        float a_end = !row.b.empty() ? l + sh.col_b - 8 : !row.c.empty() && sh.col_c ? l + sh.col_c - 8 : r.right - 14;
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
        text(shown.a, {box.left + 12, box.top, box.right - 34, box.bottom}, 13.5f, kBone, 1.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
             nullptr, DWRITE_TEXT_ALIGNMENT_LEADING, 0.0f, true);
        if (!shown.c.empty())
            text(shown.c, {box.left + 12, box.top, box.right - 32, box.bottom}, 12.5f, kAsh, 0.85f, DWRITE_FONT_WEIGHT_NORMAL,
                 nullptr, DWRITE_TEXT_ALIGNMENT_TRAILING, 0.0f, true);
        text(L"\xE70D", {box.right - 28, box.top, box.right - 8, box.bottom}, 11, hov ? kGold : kAsh, 1.0f,
             DWRITE_FONT_WEIGHT_NORMAL, L"Segoe Fluent Icons", DWRITE_TEXT_ALIGNMENT_CENTER);
        g_hits.push_back({id, box});
        if (!open) return;
        overlay = [this, id, box, sh, rows, sel, note] {
            g_hits.push_back({kHitListClose, {0, 0, kW, kH}}); // outside the list: close it
            g_hits.push_back({id, box});                       // the box again: close it
            const float rh = 30, note_h = note.empty() ? 0.0f : 40.0f;
            float w = std::max(sh.list_w, sh.box_w);
            D2D1_RECT_F list = {box.left, box.bottom + 4, box.left + w, box.bottom + 4 + rows.size() * rh + 8 + note_h};
            fill(list, 0x16141a, 1.0f, 4);
            stroke(list, kGold, 0.8f, 4);
            for (size_t i = 0; i < rows.size(); i++) {
                D2D1_RECT_F r = {list.left + 4, list.top + 4 + i * rh, list.right - 4, list.top + 4 + (i + 1) * rh};
                int rid = id + 1 + (int)i;
                bool on = (int)i == sel, over = !rows[i].off && g.hover == rid;
                g_hits.push_back({rows[i].off ? 0x7fff : rid, r});
                if (on) fill({r.left, r.top + 7, r.left + 3, r.bottom - 7}, kBlood);
                if (over) fill(r, kBone, 0.06f, 3);
                list_row(r, rows[i], on || over, sh);
            }
            if (!note.empty()) {
                float ny = list.bottom - 4 - note_h;
                fill({list.left + 14, ny, list.right - 14, ny + 1}, kInk3);
                g_hits.push_back({0x7fff, {list.left, ny, list.right, list.bottom}});
                text(note, {list.left + 14, ny + 6, list.right - 14, list.bottom - 2}, 11.5f, kAsh, 0.85f); // up to two lines
            }
        };
    }

    void placeholder_page()
    {
        const float x = kNavW + 56;
        text(S((Str)((int)Str::NavStart + g.page)), {x, 110, x + 700, 170}, 34, kBone, 1.0f, DWRITE_FONT_WEIGHT_BOLD);
        fill({x, 176, x + 36, 178}, kBlood);
        text(S(Str::ComingSoon), {x, 196, x + 700, 230}, 15, kAsh);
    }

    void draw()
    {
        g_hits.clear();
        background();
        nav();
        if (g.page == kStart) start_page();
        else if (g.page == kInstall) install_page();
        else if (g.page == kDisplay) display_page();
        else if (g.page == kGraphics) graphics_page();
        else placeholder_page();
        if (overlay) overlay(); // an open drop-down list, over the page
        if (g.help) help_panel();
        if (g.ask) ask_panel();
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
    static ID2D1Bitmap *hero, *texture;
    static ID2D1RenderTarget *made_for;
    if (made_for != g.rt) {
        release(hero);
        release(texture);
        hero = to_bitmap(g.rt, g.hero_src);
        texture = to_bitmap(g.rt, g.texture_src);
        made_for = g.rt;
    }
    g.rt->BeginDraw();
    Painter p{g.rt, hero, texture};
    p.draw();
    if (g.rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        release(hero);
        release(texture);
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

void set_lang(Lang l)
{
    g_lang = l;
    ini_set(L"lang", l == Lang::Zh ? L"zh" : L"en");
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

// "Restore defaults" on a settings page: that page's settings only, back to what the game uses with no black.ini
// (loader common.h Config) and, for the graphics card, what the game sets on its first run (high performance)
void reset_page(Page p)
{
    if (p == kDisplay) {
        black_ini_set("fullscreen", "0");
        black_ini_set("widescreen", "1");
        black_ini_set("window", "auto");
        black_ini_set("gain", "1.00");
        black_ini_set("gamma", "1.00");
    } else if (p == kGraphics) {
        black_ini_set("render", "scale");
        black_ini_set("scale", "3");
        black_ini_set("shadow", "4");
        black_ini_set("aa", "1");
        black_ini_set("vsync", "0");
        black_ini_set("seamfill", "1");
        black_ini_set("effectsnative", "0");
        black_ini_set("perfhud", "0");
        set_gpu_preference(2);
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
        if (id != g.hover) {
            g.hover = id;
            SetCursor(LoadCursorW(nullptr, id >= 0 && id != 0x7fff ? (LPCWSTR)IDC_HAND : (LPCWSTR)IDC_ARROW));
            InvalidateRect(h, nullptr, FALSE);
        }
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        g.hover = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, g.hover >= 0 && g.hover != 0x7fff ? (LPCWSTR)IDC_HAND : (LPCWSTR)IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP: {
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
            if (g.list_open == kHitAspect && row >= 0 && row < 2 && (row == 0) != (black_ini("widescreen") != "0")) {
                black_ini_set("widescreen", row == 0 ? "1" : "0");
                black_ini_set("window", "auto"); // a fixed size belongs to the other aspect
            }
            if (g.list_open == kHitWin && row >= 0 && row < 5) {
                const int(*sizes)[2] = black_ini("widescreen") == "0" ? kWin43 : kWin169;
                black_ini_set("window", row ? std::to_string(sizes[row][0]) + "x" + std::to_string(sizes[row][1]) : std::string("auto"));
            }
            if (g.list_open == kHitShadow && row >= 0 && row < 4) black_ini_set("shadow", std::to_string(kShadowScales[row]));
            if (g.list_open == kHitResList && row == 0) black_ini_set("render", "native");
            if (g.list_open == kHitResList && row >= 1 && row <= kMaxScale) {
                black_ini_set("render", "scale");
                black_ini_set("scale", std::to_string(row));
            }
            g.list_open = 0;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g.ask) { // a confirmation: yes does what it asked, no (or outside) closes
            int ask = g.ask;
            if (id == kHitAskYes || id == kHitAskNo) g.ask = 0;
            if (id == kHitAskYes) {
                if (ask == kAskDelete) delete_image();
                else if (ask == kAskReset) reset_page(g.page);
                else start_install();
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (id == kHitHelpImage || id == kHitHelpInstalled) g.help = id;
        else if (id >= 0 && id < kPageCount) g.page = (Page)id;
        else if (id == kHitLangZh) set_lang(Lang::Zh);
        else if (id == kHitLangEn) set_lang(Lang::En);
        else if (id == kHitStart) launch_game();
        else if (id == kHitPickIso) choose_image();
        else if (id == kHitInstall) request_install();
        else if (id == kHitCancel) g.prog.cancel = true;
        else if (id == kHitDeleteIso && g.offer_delete) g.ask = kAskDelete;
        else if (id == kHitPickGame) choose_existing();
        else if (id == kHitDefaultPath && !installing()) set_game_folder(default_game_folder());
        else if (id == kHitResetPage) g.ask = kAskReset;
        else if (id >= kHitAa && id < kHitAa + 2) black_ini_set("aa", id == kHitAa ? "1" : "0");
        else if (id >= kHitSeam && id < kHitSeam + 2) black_ini_set("seamfill", id == kHitSeam ? "1" : "0");
        else if (id >= kHitFx && id < kHitFx + 2) black_ini_set("effectsnative", id == kHitFx ? "1" : "0");
        else if (id >= kHitVsync && id < kHitVsync + 2) black_ini_set("vsync", id == kHitVsync ? "1" : "0");
        else if (id >= kHitHud && id < kHitHud + 2) black_ini_set("perfhud", id == kHitHud ? "1" : "0");
        else if (id == kHitGpu || id == kHitResList || id == kHitMode || id == kHitAspect || id == kHitWin || id == kHitShadow)
            g.list_open = id;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerInstall && !installing()) install_finished();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
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
    ID2D1Bitmap *hero = to_bitmap(rt, g.hero_src), *texture = to_bitmap(rt, g.texture_src);
    rt->BeginDraw();
    Painter pt{rt, hero, texture};
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
    release(rt);
    release(bmp);
    return SUCCEEDED(hr) ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g.dir = exe;
    g.dir = g.dir.substr(0, g.dir.find_last_of(L"\\/"));
    g.ini = g.dir + L"\\launcher.ini";
    std::wstring lang = ini_get(L"lang");
    if (lang.empty()) g_lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Lang::Zh : Lang::En;
    else g_lang = lang == L"en" ? Lang::En : Lang::Zh;
    g.game = ini_get(L"game", (default_game_folder() + L"\\default.xbe").c_str());
    set_dest(folder_of(g.game)); // installs go to the game folder
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g.d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&g.dw)) ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g.wic))))
        return 1;
    g.hero_src = load_jpeg(IDR_HERO);
    g.texture_src = load_jpeg(IDR_TEXTURE);

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
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
        if (!wcscmp(argv[i], L"--show-list") && i + 1 < argc) // with --snap: a drop-down list open (gpu | res | mode | aspect | window)
            g.list_open = !wcscmp(argv[i + 1], L"gpu")    ? kHitGpu
                          : !wcscmp(argv[i + 1], L"mode")   ? kHitMode
                          : !wcscmp(argv[i + 1], L"aspect") ? kHitAspect
                          : !wcscmp(argv[i + 1], L"window") ? kHitWin
                          : !wcscmp(argv[i + 1], L"shadow") ? kHitShadow
                                                            : kHitResList;
        if (!wcscmp(argv[i], L"--show-help") && i + 1 < argc) // with --snap: open a "?" panel (image | installed)
            g.help = !wcscmp(argv[i + 1], L"image") ? kHitHelpImage : kHitHelpInstalled;
        if (!wcscmp(argv[i], L"--game") && i + 1 < argc) { // with --snap: this game folder
            g.game = std::wstring(argv[i + 1]) + L"\\default.xbe";
            set_dest(argv[i + 1]);
        }
        if (!wcscmp(argv[i], L"--lang") && i + 1 < argc) g_lang = !wcscmp(argv[i + 1], L"en") ? Lang::En : Lang::Zh;
    }
    check_game();
    for (int i = 1; i < argc; i++)
        if (!wcscmp(argv[i], L"--snap") && i + 1 < argc)
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
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, S(Str::WindowTitle), style, work.left + std::max(0, (int)(work.right - work.left - w) / 2),
                             work.top + std::max(0, (int)(work.bottom - work.top - hgt) / 2), w, hgt, nullptr, nullptr, inst, nullptr);
    ShowWindow(g.hwnd, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
