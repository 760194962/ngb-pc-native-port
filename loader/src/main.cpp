// blackrt.dll entry: BlackMain is called by black.exe (the address-space stub) and never returns.
#include "common.h"
#include "fiber.h"

#include <algorithm>
#include <vector>

#include "build_profile.h"
#include "d3d/dispatch.h"
#include "d3d/host.h"
#include "fe/calls.h"
#include "fe/pages.h"
#include "fov.h"
#include "hooks.h"
#include "image.h"
#include "timing.h"
#include "settings.h"
#include "input/autoaim.h"
#include "input/mouselook.h"
#include "kernel/ktable.h"
#include "kernel/xboxkrnl.h"
#include "net/nvnet.h"
#include "nv2a_display.h"
#include "usb_ohci.h"
#include "net/online.h"
#include "net/dashfonts.h"
#include "xbe.h"
#include "xthread.h"

namespace xk {
void load_launch_data(const std::wstring &file); // kernel/k_misc.cpp
void set_image_file_name(const std::wstring &xbe_path, const std::wstring &game_dir); // kernel/k_misc.cpp
}

#include <bcrypt.h>
#include <shellapi.h>

namespace {

// The builds the address tables are made for are in build_profile.h (BLACK NTSC-U, Ninja Gaiden Black x2.1).

HANDLE g_exit_event;
std::atomic<int> g_exit_code{-1};
Xbe g_xbe;

// Ninja Gaiden Black's music and movie player on surround speakers (Dolby Digital / DTS / Dolby Surround): it splits the
// sound into six channels in three buffers and gives each channel the music volume from its table (0x44D926 in x2.1,
// hundredths of a dB: -962 is -9.62 dB), but the function that sets one channel's (FUN_00331940) multiplies it by ten
// first (lea ecx,[ecx+ecx*4]; shl ecx,1): -96.2 dB, the opening movie and the title music silent with 5.1 on. The stereo
// path sets the buffer's volume and never comes here. The ten goes (five NOPs); found by its bytes, the same at 0x331958
// in every Black build; Ninja Gaiden 2004 has no such code.
static void install_music51_fix()
{
    static const uint8_t pat[] = {0x8D, 0x0C, 0x89, 0xD1, 0xE1, 0x85, 0xC9, 0x7E, 0x04, 0x33, 0xC9,
                                  0xEB, 0x0D, 0x81, 0xF9, 0xF0, 0xD8, 0xFF, 0xFF};
    const XbeSection *t = g_xbe.section_named(".text");
    if (!t) return;
    uint8_t *p = (uint8_t *)(uintptr_t)t->vaddr, *hit = nullptr;
    int hits = 0;
    for (uint32_t i = 0; i + sizeof pat <= t->vsize; i++)
        if (p[i] == pat[0] && !memcmp(p + i, pat, sizeof pat)) hit = p + i, hits++;
    if (hits != 1) {
        if (hits) logf("music 5.1: the x10 found %d times, left as it is", hits);
        return;
    }
    DWORD old;
    VirtualProtect(hit, 5, PAGE_EXECUTE_READWRITE, &old);
    memset(hit, 0x90, 5);
    VirtualProtect(hit, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), hit, 5);
    logf("music 5.1: the channel volumes' x10 at 0x%08x removed", (uint32_t)(uintptr_t)hit);
}

// black.exe is a Windows-subsystem program, so a double-click shows only the game window. Started from a command
// prompt it attaches to that console for the short summary (logc lines, --help, --info); output redirected to a file
// or pipe by the parent stays there. False when there is nowhere to print.
bool console_init()
{
    if (_fileno(stdout) >= 0) return true; // the parent passed a file or pipe
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    FILE *f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    fputs("\n", stdout); // the prompt does not wait for a windowed program: start below it
    return true;
}

bool g_console;

void usage()
{
    static const char kUsage[] =
           "BLACK PC port " BLACK_VERSION "\n"
           "usage: black.exe [options] [path\\to\\default.xbe | game folder]\n"
           "  --info            print the XBE structure and exit\n"
           "  --hdd <dir>       host folder for the Xbox hard disk (saved games; default savedata next to black.exe)\n"
           "  --log <file>      log file (default black.log next to black.exe)\n"
           "  --symbols <csv>   functions.csv for names in logs (default notes\\functions.csv if present)\n"
           "  --trace           log every kernel call, not only the first of each kind\n"
           "  --trace-d3d       log every D3D call\n"
           "  --watch <hexaddr> log every write to a dword (hardware breakpoint, up to 4); <hexaddr>r: reads too\n"
           "  --probe <hexaddr> log registers/arguments each time code reaches an address (up to 16)\n"
           "  --debug-draws <n> describe the first n draws; --debug-frame <f> describe every draw of frame f\n"
           "  --press <f:btn,...> scripted controller presses at frame f (start a b x y back up down left right)\n"
           "  --screenshot <dir> save frames as PNG (frame 1, every 60th, and when the run stops)\n"
           "  --strict          stop at the first call to an unimplemented kernel function\n"
           "  --timeout <sec>   exit after this many seconds\n"
           "  --multicore       do not pin the process to one CPU\n"
           "  --original-load   the engine measures its load with the port's Direct3D time in it (it then sheds\n"
           "                    the shadow and HUD on a slower PC; comparing)\n"
           "  --game-cpu N      run the game threads on logical CPU N (default: the last physical core)\n"
           "  --skip-videos     play the 9-second logo video instead of the others (dev: unskippable intro/briefings)\n"
           "  --shot-every N    with --screenshot: every N frames (default 60)\n"
           "  --shot-start S    with --screenshot: start the cadence S seconds after the first frame\n"
           "  --gamma G         extra output gamma after the game's ramp (default 1.0; >1 brighter; F7/F8 adjust live)\n"
           "  --scale K         render at K x the Xbox resolution, any factor 0.5-10 (default 3 = 2880x1440 widescreen)\n"
           "  --resolution R    render size instead of --scale: WxH (e.g. 1920x1080), H (width from the aspect)\n"
           "                    or native (the window's size at start, the screen's with --fullscreen)\n"
           "  --window WxH      window client size (default: largest size of the game's aspect that fits the screen)\n"
           "  --normal-aspect   4:3 like an original Xbox on a standard TV (default: widescreen 16:9)\n"
           "  --fullscreen      start in borderless fullscreen\n"
           "  --fullscreen-res window|screen  fullscreen and borderless at the window size scaled up to the screen\n"
           "                    (the screen's shape within it; the display mode stays), or at the screen's (default)\n"
           "  --fps 30|60       frame rate (default 60; 30 is the original)\n"
           "  --no-reverb       do not mix the room reverb\n"
           "  --reverb-gain K   room reverb level (default 1.0)\n"
           "  --background      (tests) start minimised without focus, audio muted\n"
           "  --wav <file>      record the audio output as a WAV file\n"
           "  --rawvideo <file> record the picture as raw BGRX frames from --shot-start for --clip-seconds (default 4)\n"
           "  --mouse-sens K    mouse look sensitivity, K x 0.022 degrees per count (default 2.0)\n"
           "  --mouse-invert    mouse forward looks down\n"
           "  --autoaim         the game's auto-aim (the view pulling toward enemies), off by default; --no-autoaim\n"
           "  --fov D           horizontal field of view in play, 50-120 degrees (default 70, the game's; PC SETTINGS > VIDEO)\n"
           "  --language N      the console's language: 1 English (default), 2 Japanese, 3 German, 4 French, 5 Spanish,\n"
           "                    6 Italian, 7 Korean, 8 Chinese, 9 Portuguese (black.ini language)\n"
           "  --audio M         the console's audio: stereo (default), mono, surround (Dolby Surround: rear matrixed\n"
           "                    into the two channels), ac3 or dts (Dolby Digital / DTS: 5.1 out) (black.ini audio_mode...)\n"
           "  --no-movie-skip   movies skip only as in the original (default: A / START skip every movie)\n"
           "  --no-sdl-pads     XInput controllers only (default: also DualShock, DualSense, Switch and DirectInput pads)\n"
           "  --online-eeprom F --online-hdd F  online play through Insignia with this registered EEPROM and xemu disk\n"
           "                    image (both only read; black.ini online = 1, online_eeprom, online_hdd); --offline: off\n"
           "  --online-check F  check the online files (black.ini or the flags above), write what was found to F, exit\n"
           "  --online-dns-mode M  insignia (default): every DNS query to Insignia's; insigniadns: every query to the\n"
           "                    InsigniaDNS at --online-insigniadns IP; console: as the console's settings\n"
           "  --no-pc-menus     no PC SETTINGS pages in the game's Options menu (built at start into cache\\menus\n"
           "                    next to black.exe)\n"
           "  --gain K          output gain after the game's gamma ramp (default 1.0; F5/F6 adjust live)\n"
           "  --no-ramp         ignore the game's gamma ramp (no fades, half brightness: the raw framebuffer)\n"
           "  --no-aa           no anti-aliasing (default: edge-directed post filter)\n"
           "  --vsync           wait for the display's refresh when presenting (black.ini vsync)\n"
           "  --perfhud         the performance overlay: frame rate, GPU, CPU, memory, disk (black.ini perfhud)\n"
           "  --aniso N         anisotropic texture filtering 2-16 (black.ini aniso; 1 = as the game sets)\n"
           "  --no-seamfill     do not fill hairline cracks between geometry pieces (destructible walls)\n"
           "  --perf-at S:D     record performance for D seconds from S seconds after the first frame (perf\\)\n"
           "  --timer-vblank    vblank from a 59.94 Hz timer instead of the display's refresh (default: follow a ~60 Hz display)\n"
           "  --force           run an XBE other than the known NTSC-U build\n"
           "  --verbose         print the whole log on the console too (default: startup, errors, exit; all in the log file)\n";
    if (g_console) fputs(kUsage, stdout);
    else if (!g_cfg.background) MessageBoxW(nullptr, widen(kUsage).c_str(), L"BLACK - command-line options", MB_OK);
}

bool parse_args()
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring target;
    for (int i = 1; i < argc; i++) g_cfg.args.push_back(argv[i]);
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&](std::wstring &out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (a == L"--info") g_cfg.info_only = true;
        else if (a == L"--trace") g_cfg.trace = true;
        else if (a == L"--verbose") g_cfg.verbose = true;
        else if (a == L"--trace-d3d") g_trace_d3d = true;
        else if (a == L"--screenshot") { if (!next(g_cfg.screenshot_dir)) return false; }
        else if (a == L"--watch") {
            std::wstring v;
            if (!next(v) || g_cfg.watch_count >= 4) return false;
            if (!v.empty() && (v.back() == L'r' || v.back() == L'R')) g_cfg.watch_rw |= 1u << g_cfg.watch_count;
            g_cfg.watch[g_cfg.watch_count++] = (uint32_t)wcstoul(v.c_str(), nullptr, 16);
        } else if (a == L"--debug-frame") {
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.debug_frame = (unsigned)_wtoi(v.c_str());
            size_t dash = v.find(L'-');
            if (dash != std::wstring::npos) g_cfg.debug_frame_end = (unsigned)_wtoi(v.c_str() + dash + 1);
        } else if (a == L"--watch-read") {
            std::wstring v;
            if (!next(v)) return false;
            const wchar_t *q = v.c_str();
            if (*q == L'w') { g_cfg.watch_write_only = true; q++; }
            if (*q == L'*') { g_cfg.watch_read_deref = true; q++; }
            wchar_t *end = nullptr;
            g_cfg.watch_read = (uint32_t)wcstoul(q, &end, 16);
            if (end && *end == L'+') g_cfg.watch_read_off = (uint32_t)wcstoul(end + 1, nullptr, 16);
        } else if (a == L"--watch-pad") {
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.watch_pad = (unsigned)_wtoi(v.c_str());
        } else if (a == L"--memdump") {
            if (!next(g_cfg.memdump)) return false;
        } else if (a == L"--press") {
            if (!next(g_cfg.press_script)) return false;
        } else if (a == L"--debug-draws") {
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.debug_draws = (unsigned)_wtoi(v.c_str());
        } else if (a == L"--probe") {
            std::wstring v;
            if (!next(v) || g_cfg.probe_count >= 16) return false;
            g_cfg.probe[g_cfg.probe_count++] = (uint32_t)wcstoul(v.c_str(), nullptr, 16);
        }
        else if (a == L"--strict") g_cfg.strict = true;
        else if (a == L"--online-eeprom" || a == L"--online-hdd" || a == L"--online-dns") { // Insignia (project/docs/09)
            std::wstring &dst = a == L"--online-eeprom" ? g_cfg.online_eeprom : a == L"--online-hdd" ? g_cfg.online_hdd : g_cfg.online_dns;
            if (!next(dst)) return false;
            g_cfg.online = true;
            settings_mark_cli("online");
            settings_mark_cli(a == L"--online-eeprom" ? "online_eeprom" : a == L"--online-hdd" ? "online_hdd" : "online_dns");
        }
        else if (a == L"--offline") { g_cfg.online = false; settings_mark_cli("online"); }
        else if (a == L"--online-unplugged") g_cfg.online_unplugged = true; // with the online files: no cable
        else if (a == L"--online-check") { if (!next(g_cfg.online_check)) return false; }
        else if (a == L"--online-dns-mode") { // insignia / insigniadns / console (black.ini online_dns_mode)
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.online_dns_console = v == L"console";
            g_cfg.online_dns_pc = v == L"insigniadns";
            settings_mark_cli("online_dns_mode");
        }
        else if (a == L"--online-insigniadns") {
            if (!next(g_cfg.online_insigniadns)) return false;
            settings_mark_cli("online_insigniadns");
        }
        else if (a == L"--multicore") g_cfg.multicore = true;
        else if (a == L"--game-cpu") {
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.game_cpu = _wtoi(v.c_str());
        }
        else if (a == L"--skip-videos") g_cfg.skip_videos = true;
        else if (a == L"--normal-aspect") { g_cfg.widescreen = false; settings_mark_cli("widescreen"); }
        else if (a == L"--aspect") { // W:H widescreen shape (Ninja Gaiden Black: 16:9, 16:10, 21:9)
            std::wstring v;
            if (!next(v)) return false;
            int w = 0, h = 0;
            if (swscanf_s(v.c_str(), L"%d:%d", &w, &h) == 2 && w > 0 && h > 0) g_cfg.aspect_w = w, g_cfg.aspect_h = h;
            settings_mark_cli("aspect");
        }
        else if (a == L"--fullscreen") { g_cfg.fullscreen = true; settings_mark_cli("fullscreen"); }
        else if (a == L"--windowed") { g_cfg.fullscreen = false; settings_mark_cli("fullscreen"); }
        else if (a == L"--fullscreen-res") {
            settings_mark_cli("fullscreen_res");
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.fs_window_res = v == L"window";
        }
        else if (a == L"--no-reverb") { g_cfg.reverb = false; settings_mark_cli("reverb"); }
        else if (a == L"--no-fog") g_cfg.no_fog = true;
        else if (a == L"--host-ffp") g_cfg.host_ffp = true;
        else if (a == L"--no-rdtsc") g_cfg.no_rdtsc = true;
        else if (a == L"--coverage") {
            if (!next(g_cfg.coverage)) return false;
        }
        else if (a == L"--original-load") g_cfg.original_load = true;
        else if (a == L"--tsc-divisor") {
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.tsc_divisor = (unsigned)_wtoi(v.c_str());
            if (!g_cfg.tsc_divisor) g_cfg.tsc_divisor = 1;
        }
        else if (a == L"--no-ramp") g_cfg.ramp = false;
        else if (a == L"--no-aa") { g_cfg.aa = false; settings_mark_cli("aa"); }
        else if (a == L"--no-seamfill") { g_cfg.seam_fill = false; settings_mark_cli("seamfill"); }
        else if (a == L"--vsync") { g_cfg.vsync = true; settings_mark_cli("vsync"); }
        else if (a == L"--perfhud") { g_cfg.perfhud = true; settings_mark_cli("perfhud"); }
        else if (a == L"--aniso") {
            settings_mark_cli("aniso");
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.aniso = std::clamp(_wtoi(v.c_str()), 1, 16);
        }
        else if (a == L"--rescale-at") { if (!next(g_cfg.rescale_at)) return false; }
        else if (a == L"--reverb-gain") {
            settings_mark_cli("reverb_gain");
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.reverb_gain = (float)_wtof(v.c_str());
            if (g_cfg.reverb_gain < 0.0f || g_cfg.reverb_gain > 8.0f) g_cfg.reverb_gain = 1.0f;
        }
        else if (a == L"--background") g_cfg.background = true;
        else if (a == L"--api-count") g_cfg.api_count = true;
        else if (a == L"--trace-fog-ps") g_cfg.trace_fog_ps = true;
        else if (a == L"--wav") { if (!next(g_cfg.wav_path)) return false; }
        else if (a == L"--rawvideo") { if (!next(g_cfg.rawvideo)) return false; }
        else if (a == L"--clip-seconds") {
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.clip_seconds = (uint32_t)_wtoi(v.c_str());
        }
        else if (a == L"--fps") {
            settings_mark_cli("fps");
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.fps = _wtoi(v.c_str()) == 60 ? 60 : 30;
        }
        else if (a == L"--mouse-sens") {
            settings_mark_cli("mouse_sens");
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.mouse_sens = (float)_wtof(v.c_str());
            if (g_cfg.mouse_sens < 0.05f || g_cfg.mouse_sens > 50.0f) g_cfg.mouse_sens = 2.0f;
        } else if (a == L"--mouse-invert") { g_cfg.mouse_invert = true; settings_mark_cli("mouse_invert"); }
        else if (a == L"--no-autoaim") { g_cfg.autoaim = false; settings_mark_cli("autoaim"); }
        else if (a == L"--autoaim") { g_cfg.autoaim = true; settings_mark_cli("autoaim"); }
        else if (a == L"--audio") { // stereo | mono | surround | ac3 | dts (black.ini audio_mode, audio_ac3, audio_dts)
            settings_mark_cli("audio_mode");
            settings_mark_cli("audio_ac3");
            settings_mark_cli("audio_dts");
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.audio_mode = v == L"mono" ? 1 : v == L"surround" ? 2 : 0;
            g_cfg.audio_ac3 = v == L"ac3";
            g_cfg.audio_dts = v == L"dts";
        } else if (a == L"--language") { // the console's language, 1-9 (black.ini language)
            settings_mark_cli("language");
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.language = std::clamp(_wtoi(v.c_str()), 1, 9);
        } else if (a == L"--fov") {
            settings_mark_cli("fov");
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.fov = std::clamp((float)_wtof(v.c_str()), 50.0f, 120.0f);
        } else if (a == L"--game-override") {
            if (!next(g_cfg.game_override)) return false;
        } else if (a == L"--press-file") {
            if (!next(g_cfg.press_file)) return false;
        } else if (a == L"--no-movie-skip") { g_cfg.skip_movies = false; settings_mark_cli("skipanymovie"); }
        else if (a == L"--no-sdl-pads") g_cfg.sdl_pads = false;
        else if (a == L"--no-pc-menus") g_cfg.pc_menus = false;
        else if (a == L"--timer-vblank") g_cfg.timer_vblank = true;
        else if (a == L"--display-vblank") g_cfg.display_vblank = true;
        else if (a == L"--perf-at") {
            if (!next(g_cfg.perf_at)) return false;
        }
        else if (a == L"--gamma") {
            settings_mark_cli("gamma");
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.gamma = (float)_wtof(v.c_str());
            if (g_cfg.gamma < 0.2f || g_cfg.gamma > 5.0f) g_cfg.gamma = 1.0f;
        } else if (a == L"--scale") {
            settings_mark_cli("render");
            settings_mark_cli("scale"); // else black.ini's scale replaced it (r287)
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.scale = (float)_wtof(v.c_str());
            if (g_cfg.scale < 0.5f || g_cfg.scale > 10.0f) g_cfg.scale = 3.0f;
        } else if (a == L"--resolution") {
            settings_mark_cli("render");
            std::wstring v;
            if (!next(v)) return false;
            if (v == L"native") g_cfg.render_native = true;
            else {
                size_t x = v.find_first_of(L"xX");
                if (x == std::wstring::npos) g_cfg.render_h = _wtoi(v.c_str()); // height only: width from the aspect
                else {
                    g_cfg.render_w = _wtoi(v.c_str());
                    g_cfg.render_h = _wtoi(v.c_str() + x + 1);
                }
                if (g_cfg.render_h < 240 || g_cfg.render_h > 8192) g_cfg.render_w = g_cfg.render_h = 0;
            }
        } else if (a == L"--window") {
            settings_mark_cli("window");
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.window_w = _wtoi(v.c_str());
            size_t x = v.find_first_of(L"xX");
            g_cfg.window_h = x == std::wstring::npos ? 0 : _wtoi(v.c_str() + x + 1); // 0: from the aspect
        } else if (a == L"--gain") {
            settings_mark_cli("gain");
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.gain = (float)_wtof(v.c_str());
            if (g_cfg.gain < 0.2f || g_cfg.gain > 5.0f) g_cfg.gain = 1.0f;
        } else if (a == L"--shot-every") {
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.shot_every = _wtoi(v.c_str()) > 0 ? (uint32_t)_wtoi(v.c_str()) : 60;
        } else if (a == L"--shot-start") {
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.shot_start = (uint32_t)_wtoi(v.c_str());
        }
        else if (a == L"--force") g_cfg.force = true;
        else if (a == L"--hdd") { if (!next(g_cfg.hdd_dir)) return false; }
        else if (a == L"--log") { if (!next(g_cfg.log_path)) return false; }
        else if (a == L"--launch-data") { if (!next(g_cfg.launch_data)) return false; }
        else if (a == L"--symbols") { if (!next(g_cfg.symbols_path)) return false; }
        else if (a == L"--timeout") {
            std::wstring v;
            if (!next(v)) return false;
            g_cfg.timeout_s = (unsigned)_wtoi(v.c_str());
        } else if (a == L"--help" || a == L"-h" || a == L"/?") return false;
        else if (!a.empty() && a[0] == L'-') return false;
        else {
            target = a;
            g_cfg.xbe_arg = i - 1;
        }
    }
    LocalFree(argv);
    // The port keeps its own files beside the program (the release's folder, with the launcher).
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring exe_path(exe, n);
    g_cfg.port_dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
    // No game given (the program double-clicked rather than started by the launcher, which failed: the user's
    // friend, 2026-10-22): the game the launcher installed (launcher.ini beside the program: game, and hdd for the
    // saves), else game\default.xbe (the release layout), else game\x21\default.xbe (run.bat's and 0.1.0's).
    if (target.empty()) {
        std::wstring ini = g_cfg.port_dir + L"\\launcher.ini";
        wchar_t v[MAX_PATH] = L"";
        GetPrivateProfileStringW(L"launcher", L"game", L"", v, MAX_PATH, ini.c_str());
        if (v[0] && GetFileAttributesW(v) != INVALID_FILE_ATTRIBUTES) target = v;
        if (g_cfg.hdd_dir.empty()) {
            v[0] = 0;
            GetPrivateProfileStringW(L"launcher", L"hdd", L"", v, MAX_PATH, ini.c_str());
            if (v[0]) g_cfg.hdd_dir = (v[1] == L':' || v[0] == L'\\') ? std::wstring(v) : g_cfg.port_dir + L"\\" + v;
        }
        for (const wchar_t *rel : {L"game\\default.xbe", L"game\\x21\\default.xbe"})
            if (target.empty() && GetFileAttributesW((g_cfg.port_dir + L"\\" + rel).c_str()) != INVALID_FILE_ATTRIBUTES)
                target = g_cfg.port_dir + L"\\" + rel;
        if (target.empty()) target = L"game\\default.xbe";
    }
    // Started from a shortcut or another folder: if a relative game path is not found here, look for it
    // from black.exe's own folder upwards (port -> the folder with game; work\run -> work -> project root) and make
    // that folder current, so relative paths given on the command line (--hdd, --log) and notes\ resolve against it.
    bool relative = !(target.size() >= 2 && (target[1] == L':' || (target[0] == L'\\' && target[1] == L'\\')));
    if (relative && GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring dir = exe_path;
        for (int up = 0; up < 4; up++) {
            size_t cut = dir.find_last_of(L"\\/");
            if (cut == std::wstring::npos) break;
            dir.resize(cut);
            if (GetFileAttributesW((dir + L"\\" + target).c_str()) != INVALID_FILE_ATTRIBUTES) {
                SetCurrentDirectoryW(dir.c_str());
                break;
            }
        }
    }
    DWORD attr = GetFileAttributesW(target.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) target += L"\\default.xbe";
    g_cfg.xbe_path = target;
    size_t slash = target.find_last_of(L"\\/");
    g_cfg.game_dir = slash == std::wstring::npos ? L"." : target.substr(0, slash);
    if (g_cfg.hdd_dir.empty()) g_cfg.hdd_dir = g_cfg.port_dir + L"\\savedata";
    if (g_cfg.log_path.empty()) g_cfg.log_path = g_cfg.port_dir + L"\\black.log";
    settings_load(); // black.ini in the port folder; flags given above keep their values
    if (g_cfg.symbols_path.empty() && GetFileAttributesW(L"notes\\functions.csv") != INVALID_FILE_ATTRIBUTES)
        g_cfg.symbols_path = L"notes\\functions.csv";
    return true;
}

std::string sha256_hex(const std::vector<uint8_t> &data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    uint8_t digest[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "?";
    BCryptHash(alg, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), digest, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

// Threads that run game code share one CPU like the single-core Xbox (XAPI/DSound code assumes it); every
// Xbox thread takes this mask in xthread_bind. Host threads (audio mixer and XAudio2, the graphics driver,
// the window) stay free to use the other CPUs, so the game keeping its core busy at 60 fps does not starve
// the sound.
void pin_to_one_cpu()
{
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    // Physical cores (logical CPU masks); hyperthreads of one core share it. Only the fastest kind of core on a hybrid
    // CPU (the highest EfficiencyClass): on Intel's 12th generation and later "the last core" is an efficiency core,
    // and the game thread there took 8.5 ms a frame against 6.2 ms on a performance core (i5-12500H, chapter 1:
    // r655 / r662). On other CPUs every core has the same class and the choice is as before.
    std::vector<DWORD_PTR> cores;
    int best_class = -1;
    {
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        std::vector<uint8_t> buf(len);
        std::vector<std::pair<DWORD_PTR, int>> all;
        if (len && GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf.data(), &len)) {
            for (DWORD at = 0; at < len;) {
                auto *r = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf.data() + at);
                if (r->Relationship == RelationProcessorCore && r->Processor.GroupCount && r->Processor.GroupMask[0].Group == 0 &&
                    (r->Processor.GroupMask[0].Mask & proc)) {
                    int cls = r->Processor.EfficiencyClass;
                    all.push_back({(DWORD_PTR)(r->Processor.GroupMask[0].Mask & proc), cls});
                    best_class = std::max(best_class, cls);
                }
                at += r->Size;
            }
        }
        for (auto &c : all)
            if (c.second == best_class) cores.push_back(c.first);
    }
    if (cores.empty()) { // the older call (no core classes)
        DWORD len = 0;
        GetLogicalProcessorInformation(nullptr, &len);
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
        if (len && GetLogicalProcessorInformation(info.data(), &len))
            for (DWORD i = 0; i < len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); i++)
                if (info[i].Relationship == RelationProcessorCore && (info[i].ProcessorMask & proc)) cores.push_back(info[i].ProcessorMask & proc);
    }
    DWORD_PTR one = 0, core = 0;
    if (g_cfg.game_cpu >= 0 && g_cfg.game_cpu < (int)(8 * sizeof(DWORD_PTR)) && (proc & ((DWORD_PTR)1 << g_cfg.game_cpu))) {
        one = (DWORD_PTR)1 << g_cfg.game_cpu;
        for (DWORD_PTR c : cores)
            if (c & one) core = c;
    } else if (cores.size() > 1) { // the last core: CPU 0 takes most interrupts and is where Windows puts work first
        core = cores.back();
        one = core & (~core + 1);
    }
    if (!one) one = proc & (~proc + 1); // lowest available CPU
    if (!one) return;
    g_game_cpu = one;
    SetThreadAffinityMask(GetCurrentThread(), one);
    // Keep the core's other hyperthread free: threads created from now on (driver, audio) use the other cores.
    DWORD_PTR sibling = core & ~one;
    if (sibling && (proc & ~core)) SetProcessAffinityMask(GetCurrentProcess(), proc & ~sibling);
    logf("cpu: game threads pinned to CPU mask 0x%lx like the single-core Xbox%s%s; host threads use the others; --game-cpu N to "
         "choose, --multicore to disable", (unsigned long)one, sibling && (proc & ~core) ? " (its hyperthread sibling left idle)" : "",
         best_class > 0 ? " (a performance core of this hybrid CPU)" : "");
}

} // namespace

void request_exit(int code, const char *why)
{
    int expected = -1;
    if (g_exit_code.compare_exchange_strong(expected, code)) logc("exit requested: %s (code %d)", why, code);
    if (g_exit_event) SetEvent(g_exit_event);
}

bool exit_requested() { return g_exit_code.load() >= 0; }

namespace {
// Proton/Wine can leave the last thread stuck on the way out (a D3D or audio thread in a driver call), so the
// process would stay alive after EXIT GAME. Once the exit is asked for, give the normal path a few seconds and
// then terminate from this thread as well. On Windows the normal path is already gone before this runs.
DWORD WINAPI exit_watchdog(void *)
{
    WaitForSingleObject(g_exit_event, INFINITE);
    Sleep(3000);
    TerminateProcess(GetCurrentProcess(), (UINT)g_exit_code.load());
    return 0;
}
} // namespace

namespace {

bool g_image_mapped; // once true, black.exe's code is gone and BlackMain must not return

// The graphics card: the dedicated one where there is one, else the integrated one (the user, 2026-10-22). That is
// Windows' per-program "high performance" preference (HKCU\Software\Microsoft\DirectX\UserGpuPreferences, a value
// named by the program's path, "GpuPreference=2;"), which Direct3D 9 follows here: 82 runs logged the RTX 3060 with
// it, 185 the Iris Xe without (run\*\black.log, 2026-10-17..22). Written only when nothing is set for this program, so
// a choice made in Windows' graphics settings stays.
void prefer_dedicated_gpu()
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    wchar_t cur[64] = L"";
    DWORD type = 0, size = sizeof cur - sizeof(wchar_t);
    LONG q = RegQueryValueExW(k, exe, nullptr, &type, (BYTE *)cur, &size);
    if (q == ERROR_FILE_NOT_FOUND) {
        static const wchar_t v[] = L"GpuPreference=2;";
        LONG r = RegSetValueExW(k, exe, 0, REG_SZ, (const BYTE *)v, sizeof v);
        logf("gpu: no Windows graphics preference for this program; set to high performance (%s)", r == ERROR_SUCCESS ? "ok" : "failed");
    } else {
        logf("gpu: Windows graphics preference for this program kept: %s", q == ERROR_SUCCESS ? narrow(cur).c_str() : "(unreadable)");
    }
    RegCloseKey(k);
}

// Setup errors a player can fix (no game files, another build): started by a double-click, the console closes with
// the process, so they also get a message box.
void startup_error(const std::string &text)
{
    logc("%s", text.c_str());
    if (!g_cfg.background) MessageBoxW(nullptr, widen(text).c_str(), L"Ninja Gaiden Black", MB_OK | MB_ICONERROR);
}

int run(void *reserve, unsigned reserve_size)
{
    if (!parse_args()) {
        usage();
        return 2;
    }
    if (g_cfg.info_only) {
        std::string err;
        if (!g_xbe.load(g_cfg.xbe_path, err)) {
            fprintf(stderr, "%s: %s\n", narrow(g_cfg.xbe_path).c_str(), err.c_str());
            return 1;
        }
        printf("File            %s\n", narrow(g_cfg.xbe_path).c_str());
        g_xbe.print_info(&kernel_export_name);
        return 0;
    }
    if (!g_cfg.online_check.empty()) return online_check(g_cfg.online_check) ? 0 : 1; // the launcher's 联网 page

    log_init(g_cfg.log_path);
    g_exit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_exit_event) CloseHandle(CreateThread(nullptr, 0, exit_watchdog, nullptr, 0, nullptr));
    logc("BLACK loader " BLACK_VERSION " (blackrt) - XBE %s, hdd %s, log %s%s%s", narrow(g_cfg.xbe_path).c_str(), narrow(g_cfg.hdd_dir).c_str(),
         narrow(g_cfg.log_path).c_str(), g_cfg.trace ? ", trace" : "", g_cfg.strict ? ", strict" : "");
    logf("address-space stub: reserve %p + 0x%x", reserve, reserve_size);
    // What black.ini (the launcher's pages) and the command line set, as read: the launcher's choices can be checked here
    logf("settings: render %s, scale %.2f, aa %d, seamfill %d, effectsnative %d, shadow %d, vsync %d, perfhud %d, aniso %d, widescreen %d, fullscreen %d, "
         "window %dx%d, fullscreen_res %s, gain %.2f, gamma %.2f",
         g_cfg.render_native ? "native" : g_cfg.render_h ? std::to_string(g_cfg.render_h).c_str() : "scale", g_cfg.scale,
         g_cfg.aa, g_cfg.seam_fill, g_cfg.effects_native, g_cfg.shadow_scale, g_cfg.vsync, g_cfg.perfhud, g_cfg.aniso, g_cfg.widescreen, g_cfg.fullscreen ? (g_cfg.exclusive ? 2 : 1) : 0,
         g_cfg.window_w, g_cfg.window_h, g_cfg.fs_window_res ? "window" : "screen", g_cfg.gain, g_cfg.gamma);

    std::string err;
    if (!g_xbe.load(g_cfg.xbe_path, err)) {
        wchar_t full[MAX_PATH];
        if (!GetFullPathNameW(g_cfg.xbe_path.c_str(), MAX_PATH, full, nullptr)) wcscpy_s(full, g_cfg.xbe_path.c_str());
        startup_error("The game was not found (" + narrow(full) + ": " + err + ").\n\n"
                      "Install it with Ninja Gaiden Black Any Launcher.exe (Install page), or give the folder with "
                      "the game's files on the command line.\n\n"
                      "找不到游戏。请先用启动器（Ninja Gaiden Black Any Launcher.exe）的「安装」页安装游戏。");
        return 1;
    }
    std::string title = narrow(g_xbe.title);
    logf("xbe: '%s' title id %08X, base 0x%08x size 0x%x entry 0x%08x, %zu sections, %zu kernel imports", title.c_str(),
         g_xbe.title_id, g_xbe.base, g_xbe.size_of_image, g_xbe.entry, g_xbe.sections.size(), g_xbe.kernel_imports().size());
    std::string hash = sha256_hex(g_xbe.data);
    // Ninja Gaiden 2004's disc program (1.02.04): on the console, with the title update on the hard disk
    // (TDATA\54430003\$u, from Xbox Live or the Hurricane Packs' installer) the update runs instead; without it the
    // disc's own program does (the user, 2026-10-31). The disc stays D: either way. The same for the disc's
    // downloader.xbe (Xbox Live "NEW NOTICE!"): the update carries a newer one (1.06.04), and Xbox Live (Insignia) turns
    // the disc's away with "update required" (r755).
    const BuildProfile *as = build_profile_for(hash.c_str()); // the disc's file or the Redump one (kProfileAliases)
    const BuildProfile *disc_prog = as == &kProfileNg04Disc         ? &kProfileNg04Tu
                                    : as == &kProfileNg04Downloader ? &kProfileNg04DownloaderTu
                                                                    : nullptr;
    if (disc_prog) {
        std::wstring file = g_cfg.xbe_path.substr(g_cfg.xbe_path.find_last_of(L"\\/") + 1);
        std::wstring tu = host_full_path(g_cfg.hdd_dir) + L"\\Partition1\\TDATA\\54430003\\$u\\" + file;
        Xbe update;
        std::string why;
        if (update.load(tu, why) && sha256_hex(update.data) == disc_prog->sha256) {
            logf("xbe: Ninja Gaiden 2004's disc program 1.02.04; the title update on the hard disk runs instead (%s)", narrow(tu).c_str());
            g_xbe = std::move(update);
            g_cfg.xbe_path = tu;
            hash = sha256_hex(g_xbe.data);
        } else {
            logf("xbe: Ninja Gaiden 2004's disc program 1.02.04 runs: no title update 1.06.04 on the hard disk (%s: %s)",
                 narrow(tu).c_str(), why.empty() ? "another build" : why.c_str());
        }
    }
    const BuildProfile *profile = build_profile_for(hash.c_str());
    bool known_build = profile != nullptr;
    g_cfg.known_build = known_build;
    g_cfg.profile = profile;
    logc("xbe: sha256 %s (%s)", hash.c_str(), profile ? profile->name : "UNKNOWN build");
    if (!known_build && !g_cfg.force) {
        std::string known;
        for (const BuildProfile *p : kProfiles) known += std::string("\n  ") + p->name + ": " + p->sha256;
        startup_error("This default.xbe is not a build the port supports (its SHA-256 is " + hash + "). Supported:" + known +
                      "\nThe port's address tables are for those builds only; --force tries anyway.");
        return 1;
    }
    if (profile && profile->title_id != g_xbe.title_id)
        logf("warning: profile %s expects title id %08X, the XBE says %08X", profile->name, profile->title_id, g_xbe.title_id);

    if (!g_cfg.symbols_path.empty()) symbols_load(g_cfg.symbols_path);
    if (!g_cfg.multicore) pin_to_one_cpu();
    if (!kmem_init()) return 1;
    if (!xthread_init_slots()) return 1;
    prefer_dedicated_gpu();
    host3d::load_program_icons(); // the program's resources are gone once the XBE is mapped over its image
    g_image_mapped = true; // set before mapping: a partial copy also destroys the stub
    if (!image_map(g_xbe)) return 1;
    int patched = patch_fs_accesses(g_xbe);
    if (profile && patched != profile->fs_patch_count)
        logf("warning: expected %d fs patches for this build, made %d", profile->fs_patch_count, patched);
    if (!kernel_resolve_thunks(g_xbe)) return 1;
    if (!kfile_init()) return 1;
    online_init(); // before the hooks: online play keeps the game's own XNetStartup (d3d/dispatch.cpp)
    // Ninja Gaiden 2004's title updater draws with the dashboard's fonts (C:\fonts): from the registered xemu disk, once;
    // and it patches the disc's programs as they were signed (net/dashfonts.h)
    if (profile == &kProfileNg04Updater) {
        dash_fonts_ensure(g_cfg.online_hdd, host_full_path(g_cfg.hdd_dir) + L"\\Partition2");
        updater_signed_sources(host_full_path(g_cfg.game_dir), g_cfg.port_dir + L"\\cache\\ng04_signed");
    }
    sign_title_content(g_xbe.title_id); // after online_init: XboxHDKey is the EEPROM's with online play
    install_crash_handlers();
    install_probes();
    install_coverage();
    if (profile && !xd3d::install_d3d_hooks(*profile)) return 1;
    if (profile && !install_fiber_switch(*profile)) return 1;
    if (profile && !g_cfg.no_rdtsc && !install_rdtsc_emulation(*profile)) return 1;
    bool black = profile && profile->black; // BLACK's own game patches and front-end work; nothing of it applies elsewhere
    // The 1/8-pixel vertex snap (d3d/nv2a_vsh.cpp) came with BLACK's port for its destructible walls (163c55e); in
    // Ninja Gaiden it moved only the vertex-program path, and the shadow pass over fixed-function ground flickered
    g_cfg.vs_snap = black;
    if (black) install_timing_patches();
    if (black) install_mouselook_patches();
    if (black) install_autoaim_patch();
    if (black) install_fov_patch();
    if (black) install_fe_calls();
    if (black && g_cfg.pc_menus) { // the in-game PC SETTINGS pages, from the user's own front-end file
        std::wstring cache = g_cfg.port_dir + L"\\cache\\menus";
        if (fe_build_pages(host_full_path(g_cfg.game_dir), cache)) g_cfg.menu_cache = cache;
    }
    // BLACK runs at 30 fps and needs patches for 60; Ninja Gaiden Black is a 60 fps game as it is.
    if (black && g_cfg.fps == 60 && !install_60fps_patches()) g_cfg.fps = 30;
    // A widescreen shape other than 16:9: the builds whose 16/9 constant is known (the bundled programs and BLACK keep 16:9)
    if (profile && profile->aspect_const) install_ngb_aspect(); // Ninja Gaiden Black's builds and Ninja Gaiden 2004
    else g_cfg.aspect_w = 16, g_cfg.aspect_h = 9;
    if (profile && !black && !profile->aux) install_ngb_exitmenu(); // the main menu's EXIT GAME row
    install_ng04_exitmenu(); // Ninja Gaiden (2004)'s title menu: the same, its own way
    if (profile && !black && !profile->aux) install_music51_fix(); // the music player's six channels at -96 dB
    if (profile && !black && !profile->aux) install_ngb_livedash(); // Ⓐ in Ranking's Live boxes: cancel, not the dashboard
    install_ng04_livedash(); // and in Ninja Gaiden (2004)'s Master Ninja Tournament
    if (!profile && !online_enabled()) nvnet_install_unplugged(); // a program driving the NIC itself, its cable out
    nvnet_install_trap(); // online play: the NIC's registers (net/nvnet.cpp), after every other exception handler
    nv2a_display_install(); // a program without a build profile that drives the display itself (nv2a_display.cpp)
    usb_ohci_install();     // ... and USB, with an Xbox controller on it (usb_ohci.cpp)
    if (!profile) g_cfg.fps = 30;
    if (!kdpc_init()) {
        logf("could not start the kernel DPC/timer thread");
        return 1;
    }

    xk::set_image_file_name(g_cfg.xbe_path, g_cfg.game_dir);
    if (!g_cfg.launch_data.empty()) xk::load_launch_data(g_cfg.launch_data);
    logf("starting XBE entry point 0x%08x", g_xbe.entry);
    HANDLE boot = nullptr;
    if (!NT_OK(kthread_create(&boot, nullptr, g_xbe.stack_commit, 0, g_xbe.entry, 0, 0, false, "boot", true))) {
        logf("could not create the boot thread");
        return 1;
    }

    DWORD wait = g_cfg.timeout_s ? g_cfg.timeout_s * 1000 : INFINITE;
    if (WaitForSingleObject(g_exit_event, wait) == WAIT_TIMEOUT) {
        request_exit(5, "timeout");
        dump_threads();
    }
    return g_exit_code.load();
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl BlackMain(void *reserve, unsigned reserve_size)
{
    g_console = console_init();
    int code = run(reserve, reserve_size);
    if (!g_image_mapped) return code; // --info, bad arguments, or failure before the stub was overwritten
    logc("exiting with code %d", code);
    // The caller's code has been replaced by the XBE, and game threads may hold locks: terminate here.
    // logc above already flushed stdout and the log. If a write to a pipe Proton stopped reading ever blocks, it blocks
    // there; the exit watchdog ends the process 3 s after the exit was asked for.
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    for (;;) Sleep(INFINITE);
}
