// Shared declarations for blackrt.dll: config, logging, shutdown, symbols.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

typedef LONG NTSTATUS;

// NTSTATUS values used by the HLE kernel (Xbox uses the NT values).
constexpr NTSTATUS XSTATUS_SUCCESS = 0x00000000;
constexpr NTSTATUS XSTATUS_ALERTED = 0x00000101;
constexpr NTSTATUS XSTATUS_TIMEOUT = 0x00000102;
constexpr NTSTATUS XSTATUS_PENDING = 0x00000103;
constexpr NTSTATUS XSTATUS_USER_APC = 0x000000C0;
constexpr NTSTATUS XSTATUS_NO_MORE_FILES = (NTSTATUS)0x80000006;
constexpr NTSTATUS XSTATUS_UNSUCCESSFUL = (NTSTATUS)0xC0000001;
constexpr NTSTATUS XSTATUS_NOT_IMPLEMENTED = (NTSTATUS)0xC0000002;
constexpr NTSTATUS XSTATUS_INVALID_HANDLE = (NTSTATUS)0xC0000008;
constexpr NTSTATUS XSTATUS_INVALID_PARAMETER = (NTSTATUS)0xC000000D;
constexpr NTSTATUS XSTATUS_NO_MEMORY = (NTSTATUS)0xC0000017;
constexpr NTSTATUS XSTATUS_ACCESS_DENIED = (NTSTATUS)0xC0000022;
constexpr NTSTATUS XSTATUS_BUFFER_TOO_SMALL = (NTSTATUS)0xC0000023;
constexpr NTSTATUS XSTATUS_OBJECT_TYPE_MISMATCH = (NTSTATUS)0xC0000024;
constexpr NTSTATUS XSTATUS_OBJECT_NAME_INVALID = (NTSTATUS)0xC0000033;
constexpr NTSTATUS XSTATUS_OBJECT_NAME_NOT_FOUND = (NTSTATUS)0xC0000034;
constexpr NTSTATUS XSTATUS_OBJECT_NAME_COLLISION = (NTSTATUS)0xC0000035;
constexpr NTSTATUS XSTATUS_OBJECT_PATH_NOT_FOUND = (NTSTATUS)0xC000003A;
constexpr NTSTATUS XSTATUS_NOT_SUPPORTED = (NTSTATUS)0xC00000BB;
constexpr NTSTATUS XSTATUS_INVALID_DEVICE_REQUEST = (NTSTATUS)0xC0000010;

#define NT_OK(s) ((NTSTATUS)(s) >= 0)

struct BuildProfile; // build_profile.h: per-build facts (hash, fs patch count, initializer tables, hooked sections)

struct Config {
    const BuildProfile *profile = nullptr; // the build default.xbe matched (main.cpp); nullptr with --force on an unknown build
    std::wstring xbe_path;     // game\default.xbe
    std::wstring game_dir;     // directory holding default.xbe = \Device\CdRom0
    std::wstring port_dir;     // black.exe's folder: the port's own files (savedata, black.ini, black.log, cache, perf)
    std::wstring hdd_dir;      // host folder backing \Device\Harddisk0\PartitionN (default port_dir\savedata)
    std::wstring log_path;     // default port_dir\black.log
    std::wstring launch_data;  // --launch-data FILE: the 4 KB XLAUNCH_DATA_PAGE the previous XBE left (XLaunchNewImage)
    std::vector<std::wstring> args; // the command line as given (without the program), for relaunching another XBE
    int xbe_arg = -1;               // index in args of the XBE path, -1 = none given
    std::wstring symbols_path; // optional functions.csv for crash reports
    std::wstring screenshot_dir; // --screenshot: PNG frames go here
    bool trace = false;        // log every kernel call, not just the first of each kind
    bool verbose = false;      // --verbose: the whole log on the console too (default: only logc lines)
    bool strict = false;       // stop at the first call to an unimplemented kernel function
    bool multicore = false;    // default: pin to one CPU like the Xbox
    int game_cpu = -1;         // --game-cpu N: the logical CPU for the game threads (default: first CPU of the last core)
    bool skip_videos = false;  // dev: unskippable videos are replaced by the 9-second logo video
    uint32_t shot_every = 60;  // --screenshot cadence in frames
    uint32_t shot_start = 0;   // --shot-start: no cadence screenshots before this many seconds
    unsigned debug_frame_end = 0; // --debug-frame N-M: last frame of a traced range (0 = single frame)
    float gamma = 1.0f;        // output gamma applied when presenting (F7/F8 adjust it live)
    float scale = 3.0f;        // render resolution = Xbox resolution x scale (any factor, e.g. 2.25 = 1080 lines)
    int render_w = 0, render_h = 0; // --resolution WxH or H: the picture's render size instead of a scale
    bool render_native = false;     // --resolution native: render at the window's (or screen's) size
    float render_factor = 1.0f;     // with native: render at this multiple of the window
    std::wstring rescale_at;        // tests: --rescale-at "S:factor,..." = native at that factor from S seconds
    int window_w = 0, window_h = 0; // window client size; 0 = fit the screen
    bool fs_window_res = false;     // black.ini fullscreen_res = window: fullscreen at the window size (host frame_layout)
    // The console's video settings report a widescreen TV (EEPROM XC_VIDEO bit 16), as the Xbox Series does
    // for original Xbox games: BLACK then uses a 16:9 projection and its `_wide` UI layouts in the same 640x480
    // buffer (anamorphic), and the loader renders and shows that buffer at 16:9. --normal-aspect turns it off.
    bool widescreen = true;
    // The widescreen shape (black.ini aspect, Ninja Gaiden Black only): 16:9 as the game has it, or 16:10 / 21:9 by
    // replacing the game's 16:9 constant (ngb_aspect.cpp). 4:3 is widescreen = false.
    int aspect_w = 16, aspect_h = 9;
    bool fullscreen = false;   // --fullscreen: start borderless fullscreen
    bool exclusive = false;    // black.ini "fullscreen = 2": fullscreen is exclusive (D3D9 Windowed = FALSE), not borderless
    // Mouse look rotates the player directly (input/mouselook.cpp): mouse_sens x 0.022 degrees per count, the
    // scale most PC shooters use.
    float mouse_sens = 2.0f;
    bool mouse_invert = false;
    bool timer_vblank = false; // --timer-vblank: free-running 59.94 Hz vblank instead of following the display
    bool display_vblank = false; // --display-vblank (tests): follow the display even with --background
    bool effects_native = false; // the small effect buffers at their Xbox size (PC SETTINGS); default: render scale, smoothed
    int shadow_scale = 4;        // shadow maps at this many times their Xbox size (black.ini shadow: 1, 2, 4, 8, 16)
    bool vsync = false;          // present on the display's refresh (black.ini vsync); off: frames paced by the Xbox vblank only
    bool perfhud = false;        // the performance overlay (black.ini perfhud; d3d/perfhud.cpp)
    int aniso = 1;               // anisotropic texture filtering (black.ini aniso: 1 off = as the game sets, 2-16; d3d/draw.cpp)
    bool seam_fill = true;     // fill hairline cracks between geometry pieces (PC SETTINGS, --no-seamfill)
    bool autoaim = false;      // the game's view pull toward targets, off by default (input/autoaim.cpp; --autoaim, PC SETTINGS)
    float fov = 70.0f;         // horizontal field of view in play, degrees (fov.cpp; --fov, PC SETTINGS; the game's is 70)
    int language = 1;          // the console's language (EEPROM XC_LANGUAGE, kernel/k_misc.cpp): 1 English, 2 Japanese,
                               // 3 German, 4 French, 5 Spanish, 6 Italian, 7 Korean, 8 Chinese, 9 Portuguese (--language)
    int audio_mode = 0;        // the console's analog audio (EEPROM XC_AUDIO, kernel/k_misc.cpp): 0 stereo, 1 mono,
                               // 2 Dolby Surround (black.ini audio_mode; --audio)
    bool audio_ac3 = false;    // digital output: Dolby Digital / DTS on (audio_ac3 / audio_dts): the mix goes out as 5.1
    bool audio_dts = false;
    bool skip_movies = true;   // A / START skip any movie, also unseen ones (input/movies.cpp; --no-movie-skip, PC SETTINGS)
    // Online play through Insignia (net/online.cpp, project/docs/09): off unless black.ini online = 1 (or --online-eeprom
    // and --online-hdd) names the player's registered xemu EEPROM and hard disk image; both are only read.
    bool online = false;
    std::wstring return_xbe; // --return-xbe F: the game the port's Xbox Live dashboard goes back to (kernel/k_misc.cpp)
    std::wstring online_config_out; // --online-config-out F (tests): the disk's config area as the game left it, saved to F
                                    // after each write (net/online.cpp; the registered disk stays unchanged)
    bool online_unplugged = false; // --online-unplugged (tests): the console and its accounts as online, the cable out
                                   // (the NIC reports no link and drops what it is given: net/nvnet.cpp)
    std::wstring online_eeprom, online_hdd, online_dns; // online_dns: Insignia's DNS server (default 46.101.64.175)
    // black.ini online_dns_mode: insignia (default) relays every DNS query to online_dns; console sends them where the
    // console's network settings say (manual DNS such as a PC running InsigniaDNS), as the Xbox / xemu would
    bool online_dns_console = false;
    // insigniadns: every DNS query to online_insigniadns, the address of a PC running InsigniaDNS (for a proxy that
    // captures public DNS: the launcher's 联网 page); the console's settings are not used
    bool online_dns_pc = false;
    std::wstring online_insigniadns;
    std::wstring online_check; // --online-check FILE: check the online files, write the result there and exit (the launcher)
    std::wstring online_import; // --online-import FILE: import from xemu's files into the hard disk folder, report there, exit
    std::wstring online_new_console; // --online-new-console FILE: a console of the port's own, report there, exit
    std::wstring online_dash_from, online_dash_out; // --online-dash-from IMAGE FILE: the dashboard's files from IMAGE
    bool known_build = false;  // the XBE is the NTSC-U build the address tables are for (main.cpp)
    std::wstring game_override; // --game-override: files there replace the game's of the same relative path
    std::wstring menu_cache;    // the port's rebuilt front-end files (fe/pages.cpp), served like --game-override after it
    bool pc_menus = true;       // build the in-game PC SETTINGS pages (--no-pc-menus: the game's own menus only)
    std::wstring press_file;    // --press-file (tests): presses and screenshot requests appended by a driver
    int fps = 60;              // 60 (default): swap interval TWO paced as ONE, world step 1/60 s, weapon refire kept at
                               // whole 1/30 s steps (timing.cpp); --fps 30 = the original
    // Menu choices for widescreen and fps, which the game reads at boot: saved to black.ini for the next run while
    // the running session keeps the values above (0 / -1 = no change chosen).
    int fps_next = 0;
    int widescreen_next = -1;
    bool reverb = true;        // mix the I3DL2 reverb send (--no-reverb to compare)
    bool hrtf = true;          // 3D voices through the Xbox HRTF when the game asks for it (audio/hrtf.h); black.ini hrtf,
                               // press-file "cfg:hrtf:0/1"
    float reverb_gain = 1.0f;  // --reverb-gain: overall reverb level
    bool background = false;   // --background (tests): window minimised without focus, audio muted
    bool trace_fog_ps = false; // --trace-fog-ps (diagnosis): trace the frame after the first fogged pixel-shader draw
    bool vs_divide_fix = true; // press-file "cfg:vsdivide:0": refraction coordinates divided per vertex, as the game does
    uint32_t dump_vs_texmodes = 0; // press-file "cfg:dumpvs:HEX": in a traced frame, dump the vertex program inputs of pixel-shader draws with these texture modes
    uint32_t skip_caller = 0;  // press-file "cfg:skipcaller:HEX": drop draws with this game return address on the stack (diagnosis)
    uint32_t skip_texmodes = 0; // press-file "cfg:skiptexmodes:HEX": drop pixel-shader draws with these PSTextureModes (diagnosis)
    int pix_watch_x = -1, pix_watch_y = -1; // press-file "cfg:pixwatch:X,Y" (Xbox back-buffer pixel): traced frames log
                                            // the draws that change it (d3d/draw.cpp pixwatch_check; diagnosis)
    int force_localviewer = -1; // press-file "cfg:localviewer:N": 0 / 1 force D3DRS_LOCALVIEWER, 2 the game's value,
                                // -1 (default, any other N) TRUE for draws with a reflection stage (d3d/draw.cpp)
    uint32_t pix_watch_rt = 0; // press-file "cfg:pixwatchrt:HEX": watch that render target (its data address) instead
    int z_probe = 0; // press-file "cfg:zprobe:N" (with pixwatch): also log the depth there, and the N-th shadow-map pass
                     // of the frame is drawn ALWAYS / depth-writing / unbiased to read its own depth (diagnosis)
    bool edge_mirror = true;  // press-file "cfg:edgemirror:0": projected reads past a render target clamp as on the Xbox
                              // (on: alpha 0 past the edge, d3d/draw.cpp apply_texture_stages)
    bool sdl_pads = true;     // --no-sdl-pads: XInput pads only (input/sdlpad.cpp)
    bool rhw_edge_fix = true; // press-file "cfg:rhwedge:0": keep XYZRHW quads that stop a pixel short of their target (d3d/draw.cpp)
    int shadow_pull_bias = -1; // press-file "cfg:shadowpullc:N" / "cfg:shadowpulls:N": the pull's constant and slope
    int shadow_pull_slope = -1; // parts in steps on their own (-1: shadow_pull's); tests
    int zoffset_mode = 2; // the polygon offset of vertex-program draws (the shadow passes; d3d/draw.cpp apply_render_states):
                          // 2 the host's depth bias, slope per Xbox pixel taken on the smaller render-scale axis, no pull;
                          // 1 per pixel in the pixel shader as the NV2A does (2.1.2: about 2 ms more on the iGPU at 4K,
                          // it loses early depth rejection); 0 as before 2.1.2 (larger axis + the pull). cfg:zoffset:N
    int shadow_pull = 1;       // press-file "cfg:shadowpull:N": Ryu's shadow pass pulled toward the camera, N steps (d3d/draw.cpp)
    bool vs_snap = false;      // vertex-program positions snapped to 1/8 Xbox pixel (d3d/nv2a_vsh.cpp): BLACK only
                               // (main.cpp, its destructible walls); press-file "cfg:vsnap:0/1"
    bool rhw_tex_matrix = false; // press-file "cfg:rhwtexmat:1": XYZRHW coordinates through the game's texture matrix
                                 // (before 2.1.1; the console never applies it: d3d/draw.cpp rhw_tex_scales)
    int shadow_pcf = 1;        // shadow maps read with linear filtering: 2x2 percentage-closer filtering, as the NV2A
                               // does (d3d/nv2a_psh.cpp); 1 over the host map's texels (since 2.2.18: smooth at every
                               // shadow resolution, as ngbrecomp), 2 over the Xbox map's (the console's width, stepped like
                               // a 128x128 map above 1x), 0 one sample; black.ini shadowsoft = 1 / 0; press-file "cfg:shadowpcf:N"
    bool quad_snap = true;     // press-file "cfg:quadsnap:0": 2D quads above 1x drawn as before (d3d/draw.cpp quad_snap)
    bool quad_weight = true;   // press-file "cfg:quadweight:0": QuadSnap without the alpha-weighted four-texel read (diagnosis)
    int msaa = 0;              // black.ini msaa: 0 off, 2 / 4 / 8 samples for the frame's targets (d3d/device.cpp)
    bool hud_centre = false;   // black.ini hud_centre = 1 / press-file "cfg:hudcentre:1": on a 21:9 or 16:10 screen the whole HUD
                               // about the middle 16:9, as before 2.2.12 (d3d/draw.cpp wide_hud_place)
    bool quad_grad = false;    // press-file "cfg:quadgrad:1": QuadSnap's four texel reads with explicit gradients, as before 2.2.5 (comparison; d3d/ffp_ps.cpp)
    bool resolve = true;       // press-file "cfg:resolve:0": the frame to the window by the old chain (seam fill, ramp and FXAA at the frame's size, then a bilinear scale; d3d/device.cpp present_backbuffer)
    bool rhw_offset = true;    // press-file "cfg:rhwoffset:0": XYZRHW positions ignore the screen-space offset (comparison)
    bool zview8 = false;       // press-file "cfg:zview8:1": depth views back to A8R8G8B8 (8-bit depth) for comparison
    bool api_count = false;    // --api-count (diagnosis): Direct3D 9 calls by kind per frame (d3d/apicount.cpp)
    std::wstring wav_path;     // --wav: record the mixer output (16-bit stereo 48 kHz)
    std::wstring rawvideo;     // --rawvideo: the shown picture as raw BGRX frames (from --shot-start, --clip-seconds)
    uint32_t clip_seconds = 4;
    // Output gain after the game's gamma ramp (F5/F6 adjust it live). The ramp (d3d/device.cpp, ramp_texture)
    // already doubles the framebuffer like the Xbox's video DAC did (BLACK's art uses 0x80 = full brightness).
    float gain = 1.0f;
    bool ramp = true;          // apply the game's gamma ramp (fades, brightness); --no-ramp = identity
    bool aa = true;            // post-process anti-aliasing on the final picture (--no-aa)
    bool info_only = false;    // print XBE info and exit
    bool force = false;        // run even if the XBE is not the build the address tables were made for
    unsigned timeout_s = 0;    // exit after N seconds (0 = never)
    uint32_t watch[4] = {};    // --watch: hardware write breakpoints (Xbox addresses)
    int watch_count = 0;
    uint32_t watch_rw = 0;     // bit i: --watch address i had an "r" suffix (break on reads too)
    uint32_t probe[16] = {};   // --probe: log each time execution reaches these addresses
    int probe_count = 0;
    unsigned debug_draws = 0;  // --debug-draws: describe the first N draws
    unsigned debug_frame = 0;  // --debug-frame: describe every draw of this frame
    bool no_fog = false;       // --no-fog: never enable host fog (diagnosis of dark scenes)
    bool no_specular = false;  // press-file "spec:0": never enable host specular lighting (diagnosis of light patches)
    bool original_load = false; // --original-load: the engine's own load figure, Direct3D time in it (game_load.cpp)
    unsigned tsc_divisor = 1;  // --tsc-divisor N: the engine's frame timer (rdtsc) runs N times slower than the Xbox TSC (docs/04 §3.17)
    std::wstring coverage;     // --coverage FILE: one-shot probes at the hex addresses listed (first word of each line)
    int untex = 3;             // press-file "cfg:untex:N": a stage reading an unbound texture is 3 skipped, or the texture reads as 1 the diffuse colour, 0 white (d3d/ffp_ps.cpp)
    bool flash_watch = false;  // press-file "cfg:flashwatch:1": detect single dark / flat frames, save them with draw statistics
    bool vis_all = false;      // press-file "cfg:visall:1": visibility tests report every pixel visible (diagnosis of culling)
    bool draw_ring = true;        // press-file "cfg:ring:0": indexed draws through DrawIndexedPrimitiveUP again (d3d/draw.cpp)
    bool tex_periodic = false;    // press-file "cfg:texperiodic:1": also re-hash stable textures every 16th frame (write-watch fallback)
    bool tex_every_frame = false; // press-file "cfg:texcheck:1": check every texture's contents every frame (diagnosis of stale images)
    int zmode = 3;             // press-file "cfg:zmode:N": fixed-function sampling of a depth-format header (0 depth, 1 bytes, 2 shadow func vs 0, 3 back buffer colour)
    bool no_rdtsc = false;     // --no-rdtsc: leave rdtsc to the host TSC (diagnosis)
    bool host_ffp = false;     // --host-ffp: fixed-function draws use the host's pixel pipeline, not the generated stage shader
    unsigned ffp_debug = 0;    // press-file "cfg:ffpdebug:N": the stage shader shows stage 1's texture colour (1) or alpha (2)
    std::wstring press_script; // --press: "frame:button,frame:button" scripted controller input
    std::wstring perf_at;      // --perf-at S:D: record performance for D seconds from S seconds after the first frame
    std::wstring memdump;      // --memdump [*]ADDR:SIZE:F1,F2,...: raw memory to the --screenshot dir at those frames
    unsigned watch_pad = 0;    // --watch-pad N: from frame N, log game code that reads the right stick (DR3)
    uint32_t watch_read = 0;   // --watch-read [w][*]ADDR[+OFF]: with --watch-pad, watch these 4 bytes instead
    uint32_t watch_read_off = 0; // (* = ADDR holds a pointer, read when the watch starts; w = writes only)
    bool watch_read_deref = false, watch_write_only = false;
};
extern Config g_cfg;
extern DWORD_PTR g_game_cpu; // affinity of the threads that run game code (0 = any, --multicore)
extern DWORD_PTR g_game_threads_mask; // --multicore on a hybrid CPU: its performance cores for the game threads (0 = any)
extern uint32_t g_pad_watch; // address DR3 watches for --watch-pad (input/xinput.cpp), 0 = off
extern std::atomic<uint32_t> g_frame_count; // frames presented so far (d3d/device.cpp)

// Logging: every line goes to the log file and the console, prefixed with time and host thread id.
void log_init(const std::wstring &path);
void logf(const char *fmt, ...); // the log file (and the console with --verbose)
void install_ngb_exitmenu();      // the main menu's EXIT GAME row for Ninja Gaiden Black x2.1 (ngb_exitmenu.cpp)
void install_ng04_exitmenu();     // the title menu's EXIT GAME row for Ninja Gaiden (2004) (ng04_exitmenu.cpp)
void install_ngb_aspect();        // black.ini aspect 16:10 / 21:9 for Ninja Gaiden Black x2.1 (ngb_aspect.cpp)
void install_ngb_livedash();      // Ⓐ cancels Ranking's Xbox Live boxes instead of opening the dashboard (ngb_livedash.cpp)
void install_ng04_livedash();     // the same in Ninja Gaiden (2004)'s Master Ninja Tournament (ngb_livedash.cpp)
void logv(const char *fmt, va_list ap);
void logc(const char *fmt, ...); // the log file and the console: startup, setup errors, exit, crashes
[[noreturn]] void fatal(const char *fmt, ...);
std::string narrow(const std::wstring &w);
std::wstring widen(const std::string &s);
std::wstring host_full_path(const std::wstring &p); // absolute (kernel/k_file.cpp)

// Request process exit; the host main thread performs it.
void request_exit(int code, const char *why);
bool exit_requested();

// Address -> "name+off" using functions.csv if it was loaded (crash reports and hook logs).
void symbols_load(const std::wstring &csv_path);
std::string sym_name(uint32_t addr);
bool sym_function(uint32_t addr, std::string *name, uint32_t *start); // the function containing addr, if known

// Kernel call tracing: first call of each function is always logged; with --trace every call.
#define KTRACE(fmt, ...)                                                                    \
    do {                                                                                    \
        static std::atomic<bool> s_seen_{false};                                            \
        if (g_cfg.trace || !s_seen_.exchange(true))                                         \
            logf("[k] %s(" fmt ")%s", __func__, ##__VA_ARGS__, g_cfg.trace ? "" : "  <first call>"); \
    } while (0)

// One-shot note about a partially implemented path.
#define KNOTE_ONCE(fmt, ...)                                     \
    do {                                                         \
        static std::atomic<bool> s_noted_{false};                \
        if (!s_noted_.exchange(true)) logf("[k] note: " fmt, ##__VA_ARGS__); \
    } while (0)
