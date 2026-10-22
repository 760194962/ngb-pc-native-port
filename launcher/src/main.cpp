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
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

// Layout, in DIPs
constexpr float kW = 1180, kH = 700, kNavW = 236;

// The website's palette (:root of akirahayabusa.github.io/ngb-hurricane-patch)
D2D1_COLOR_F rgb(uint32_t v, float a = 1.0f) { return D2D1::ColorF(v, a); }
const uint32_t kInk = 0x0b0a0d, kInk3 = 0x2a2630, kBone = 0xece7dd, kAsh = 0xa39c92, kBlood = 0xcf3640, kGold = 0xcfa650;

enum Page { kStart, kInstall, kDisplay, kControls, kSaves, kAbout, kPageCount };
const wchar_t *const kIcons[kPageCount] = {L"\xE768", L"\xE896", L"\xE7F4", L"\xE7FC", L"\xE74E", L"\xE946"};

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
};
enum { kAskOverwrite = 1, kAskNonEmpty, kAskDelete };
App g;

// Hit ids: 0..kPageCount-1 the navigation, then these
enum {
    kHitStart = 100, kHitLangZh, kHitLangEn, kHitPickIso, kHitPickDest, kHitInstall, kHitCancel, kHitDeleteIso, kHitPickGame,
    kHitHelpImage, kHitHelpInstalled, kHitHelpClose, kHitAskYes, kHitAskNo, kHitDefaultPath
};
constexpr UINT_PTR kTimerInstall = 1;
struct Hit {
    int id;
    D2D1_RECT_F r;
};
std::vector<Hit> g_hits;

bool inside(const D2D1_RECT_F &r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// --- settings ---------------------------------------------------------------------------------------------------

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

void launch_game()
{
    if (!exists(game_exe()) || !exists(g.game)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    // launcher.ini: hdd = the Xbox hard disk (default savedata next to the program); args = extra options for the game
    // (development: a log and a command file elsewhere), which then replace the default log
    std::wstring hdd = ini_get(L"hdd", (g.dir + L"\\savedata").c_str()), args = ini_get(L"args");
    std::wstring cmd = L"\"" + game_exe() + L"\" --windowed --hdd \"" + hdd + L"\" ";
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

struct Painter {
    ID2D1RenderTarget *rt;
    ID2D1Bitmap *hero = nullptr, *texture = nullptr;

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
                                   {S(Str::Gpu), S(gpu_preference() == 1 ? Str::GpuIntegrated
                                                   : gpu_preference() == 2 ? Str::GpuDedicated
                                                                           : Str::GpuAuto)},
                                   {S(Str::Display), std::wstring(S(black_ini("fullscreen") == "1" ? Str::Fullscreen : Str::Windowed)) +
                                                         L"  \x00B7  " + (black_ini("widescreen") == "0" ? L"4:3" : L"16:9")}};
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
        Str title = g.ask == kAskOverwrite ? Str::AskOverwriteTitle : g.ask == kAskNonEmpty ? Str::AskNonEmptyTitle : Str::AskDeleteTitle;
        Str body = g.ask == kAskOverwrite ? Str::AskOverwrite : g.ask == kAskNonEmpty ? Str::AskNonEmpty : Str::DeleteConfirm;
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
               S(g.ask == kAskDelete ? Str::MoveToBin : Str::Continue), true);
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
        else placeholder_page();
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
    case WM_MOUSEMOVE: {
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
        int id = hit_at(GET_X_LPARAM(lp) * scale, GET_Y_LPARAM(lp) * scale);
        if (g.help) { // the panel is modal: it only closes
            if (id == kHitHelpClose) g.help = 0;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g.ask) { // a confirmation: yes does what it asked, no (or outside) closes
            int ask = g.ask;
            if (id == kHitAskYes || id == kHitAskNo) g.ask = 0;
            if (id == kHitAskYes) {
                if (ask == kAskDelete) delete_image();
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
        if (!wcscmp(argv[i], L"--show-ask") && i + 1 < argc) g.ask = _wtoi(argv[i + 1]); // with --snap: a confirmation
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
