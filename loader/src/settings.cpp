#include "settings.h"

#include "common.h"
#include "input/bindings.h"
#include "input/padkeys.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

std::wstring ini_file() { return g_cfg.port_dir.empty() ? L"black.ini" : g_cfg.port_dir + L"\\black.ini"; }
std::set<std::string> g_cli; // keys given on the command line
std::map<std::string, std::string> g_base; // each key's line as the settings stood once loaded (settings_save)
std::mutex g_lock;           // the menu (window thread) and the exit path may both save

bool cli(const char *key) { return g_cli.count(key) != 0; }

} // namespace

void settings_mark_cli(const char *key) { g_cli.insert(key); }

void load_file();
void take_base();

void settings_load()
{
    if (g_cfg.background) return;
    load_file();
    take_base();
}

void load_file()
{
    FILE *f = nullptr;
    if (_wfopen_s(&f, ini_file().c_str(), L"r") || !f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64] = "", val[128] = "";
        if (line[0] == '#' || sscanf_s(line, " %63[^= ] = %127s", key, (unsigned)sizeof key, val, (unsigned)sizeof val) != 2) continue;
        std::string k = key;
        if (cli(k.c_str())) continue; // the command line wins for this run; the file keeps its value (settings_save)
        float v = (float)atof(val); // the C runtime's default "C" locale: '.' decimals
        if (k == "render") {
            g_cfg.render_native = !strcmp(val, "native");
            g_cfg.render_h = g_cfg.render_native ? 0 : std::clamp(atoi(val), 0, 8192);
            g_cfg.render_w = 0;
            if (g_cfg.render_h && g_cfg.render_h < 240) g_cfg.render_h = 0;
        } else if (k == "render_factor") g_cfg.render_factor = std::clamp(v, 1.0f, 2.0f);
        else if (k == "scale") g_cfg.scale = std::clamp(v, 0.5f, 10.0f);
        else if (k == "aa") g_cfg.aa = v != 0;
        else if (k == "seamfill") g_cfg.seam_fill = v != 0;
        else if (k == "effectsnative") g_cfg.effects_native = v != 0;
        else if (k == "shadow") g_cfg.shadow_scale = std::clamp(atoi(val), 1, 16);
        else if (k == "vsync") g_cfg.vsync = v != 0;
        else if (k == "perfhud") g_cfg.perfhud = v != 0;
        else if (k == "aniso") g_cfg.aniso = std::clamp(atoi(val), 1, 16);
        else if (k == "fps") g_cfg.fps = atoi(val) == 60 ? 60 : 30;
        else if (k == "widescreen") g_cfg.widescreen = v != 0;
        else if (k == "fullscreen") { // 0 window, 1 borderless, 2 exclusive
            g_cfg.fullscreen = v != 0;
            g_cfg.exclusive = (int)v == 2;
        } else if (k == "window") { // the window's client size: "auto" (fit the screen) or WxH
            int w = 0, h = 0;
            if (sscanf_s(val, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) g_cfg.window_w = w, g_cfg.window_h = h;
            else g_cfg.window_w = g_cfg.window_h = 0;
        }
        else if (k == "gain") g_cfg.gain = std::clamp(v, 0.5f, 3.0f);
        else if (k == "gamma") g_cfg.gamma = std::clamp(v, 0.5f, 3.0f);
        else if (k == "mouse_sens") g_cfg.mouse_sens = std::clamp(v, 0.1f, 50.0f);
        else if (k == "mouse_invert") g_cfg.mouse_invert = v != 0;
        else if (k == "autoaim") g_cfg.autoaim = v != 0;
        else if (k == "fov") g_cfg.fov = std::clamp(v, 50.0f, 120.0f);
        else if (k == "skipanymovie") g_cfg.skip_movies = v != 0;
        else if (k == "reverb") g_cfg.reverb = v != 0;
        else if (k == "reverb_gain") g_cfg.reverb_gain = std::clamp(v, 0.0f, 8.0f);
        else if (padkeys::load_ini(k.c_str(), val)) {} // kb_<input> = Key[,Key], padmap_<input> = <input> (Ninja Gaiden Black)
        else bindings::load_ini(k.c_str(), val); // key_<action> = Key[,Key] (BLACK)
    }
    fclose(f);
}

// Every setting as black.ini lines, in file order: (key, whole line)
void write_all(FILE *f);
std::vector<std::pair<std::string, std::string>> all_lines()
{
    std::vector<std::pair<std::string, std::string>> v;
    std::wstring tmp = ini_file() + L".new";
    FILE *f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"w") || !f) return v;
    write_all(f);
    fclose(f);
    if (_wfopen_s(&f, tmp.c_str(), L"r") || !f) return v;
    char line[256], key[64];
    while (fgets(line, sizeof line, f))
        if (line[0] != '#' && sscanf_s(line, " %63[^= ]", key, (unsigned)sizeof key) == 1) v.push_back({key, line});
    fclose(f);
    _wremove(tmp.c_str());
    return v;
}

// The settings as loaded: what this run has not changed is not this run's to write (settings_save)
void take_base()
{
    for (auto &kv : all_lines()) g_base[kv.first] = kv.second;
}

// Writes back only what changed in this run (Alt+Enter's fullscreen, the in-game menus) into black.ini as it is now;
// every other line stays as the file has it, so settings the launcher saved while the game ran are not overwritten
// with this run's older values (they were, when the whole file was written). Keys the file lacks are added (the first
// run writes them all); keys given on the command line are this run's only and never written.
void settings_save()
{
    if (g_cfg.background) return;
    std::lock_guard<std::mutex> g(g_lock);
    std::vector<std::string> file; // black.ini's lines now
    FILE *f = nullptr;
    char line[256], key[64];
    if (!_wfopen_s(&f, ini_file().c_str(), L"r") && f) {
        while (fgets(line, sizeof line, f)) file.push_back(line);
        fclose(f);
    }
    if (file.empty()) file.push_back("# BLACK PC port settings (written by the launcher, the game's menus and on exit)\n");
    auto find = [&](const std::string &k) -> int {
        for (size_t i = 0; i < file.size(); i++)
            if (file[i][0] != '#' && sscanf_s(file[i].c_str(), " %63[^= ]", key, (unsigned)sizeof key) == 1 && k == key) return (int)i;
        return -1;
    };
    for (auto &kv : all_lines()) {
        if (cli(kv.first.c_str())) continue;
        int at = find(kv.first);
        auto base = g_base.find(kv.first);
        bool changed = base == g_base.end() || base->second != kv.second;
        if (at < 0) file.push_back(kv.second);
        else if (changed) file[at] = kv.second;
    }
    if (_wfopen_s(&f, ini_file().c_str(), L"w") || !f) return;
    for (const std::string &l : file) fputs(l.c_str(), f);
    fclose(f);
}

void write_all(FILE *f)
{
    if (g_cfg.render_native) fprintf(f, "render = native\n");
    else if (g_cfg.render_h) fprintf(f, "render = %d\n", g_cfg.render_h);
    else fprintf(f, "render = scale\n");
    fprintf(f, "render_factor = %.2f\nscale = %.3f\n", g_cfg.render_factor, g_cfg.scale);
    int fps = g_cfg.fps_next ? g_cfg.fps_next : g_cfg.fps; // a menu choice for the next run wins
    bool wide = g_cfg.widescreen_next >= 0 ? g_cfg.widescreen_next != 0 : g_cfg.widescreen;
    fprintf(f, "aa = %d\nfps = %d\nwidescreen = %d\nfullscreen = %d\n", g_cfg.aa ? 1 : 0, fps, wide ? 1 : 0,
            g_cfg.fullscreen ? (g_cfg.exclusive ? 2 : 1) : 0);
    if (g_cfg.window_w > 0 && g_cfg.window_h > 0) fprintf(f, "window = %dx%d\n", g_cfg.window_w, g_cfg.window_h);
    else fprintf(f, "window = auto\n");
    fprintf(f, "gain = %.2f\ngamma = %.2f\n", g_cfg.gain, g_cfg.gamma);
    fprintf(f, "mouse_sens = %.3f\nmouse_invert = %d\n", g_cfg.mouse_sens, g_cfg.mouse_invert ? 1 : 0);
    fprintf(f, "reverb = %d\nreverb_gain = %.2f\n", g_cfg.reverb ? 1 : 0, g_cfg.reverb_gain);
    fprintf(f, "autoaim = %d\n", g_cfg.autoaim ? 1 : 0);
    fprintf(f, "fov = %.1f\n", g_cfg.fov);
    fprintf(f, "skipanymovie = %d\n", g_cfg.skip_movies ? 1 : 0);
    fprintf(f, "seamfill = %d\n", g_cfg.seam_fill ? 1 : 0);
    fprintf(f, "effectsnative = %d\n", g_cfg.effects_native ? 1 : 0);
    fprintf(f, "shadow = %d\nvsync = %d\nperfhud = %d\naniso = %d\n", g_cfg.shadow_scale, g_cfg.vsync ? 1 : 0,
            g_cfg.perfhud ? 1 : 0, g_cfg.aniso);
    padkeys::save_ini(f);
    bindings::save_ini(f);
}
