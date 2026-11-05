// The console's Xbox Live dashboard ready to run (dashboard.h). Its files are the player's own: C:\xodash (the program,
// its media and sounds) and C:\fonts, copied once from the registered xemu hard disk image (black.ini online_hdd, read
// only) into the port's hard disk, and never shipped. Only the build the port has tables for runs (kProfileXod); any
// other leaves the games' Live boxes answering Ⓐ as Ⓑ (ngb_livedash.cpp), as before.
#include "dashboard.h"

#include "build_profile.h"
#include "common.h"
#include "net/dashfonts.h"
#include "net/online.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <vector>

namespace xk {
bool dashboard_back(const char *why); // kernel/k_misc.cpp (it relaunches as XLaunchNewImage does)
}
bool dashboard_back_to_game(const char *why) { return xk::dashboard_back(why); }

static std::string sha256_hex(const std::vector<uint8_t> &data)
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

std::wstring dashboard_xbe() { return host_full_path(g_cfg.hdd_dir) + L"\\Partition2\\xodash\\xonlinedash.xbe"; }

bool dashboard_available()
{
    static int state = -1; // -1 not decided yet
    if (state >= 0) return state == 1;
    state = 0;
    if (g_cfg.profile == &kProfileXod) return false;
    if (!online_enabled()) return false; // without online play the dashboard has nothing to do (and stops at once)
    std::wstring p2 = host_full_path(g_cfg.hdd_dir) + L"\\Partition2";
    // xboxdash.xbe too: run from the hard disk, xonlinedash stays only when C:\xboxdash.xbe is there with its own
    // certificate version (0x4D1F9; else it starts the main dashboard at once). The port never runs it.
    if (!dash_fonts_ensure(g_cfg.online_hdd, p2) || !dash_dir_ensure(g_cfg.online_hdd, p2, "xodash", L"xonlinedash.xbe") ||
        !dash_file_ensure(g_cfg.online_hdd, p2, "xboxdash.xbe")) {
        logf("dash: the Xbox Live dashboard is not available (its files are not in the registered disk image): Live boxes cancel");
        return false;
    }
    // its sounds (the main dashboard's C:\Audio): it runs silent without them
    if (GetFileAttributesW((p2 + L"\\Audio\\" + DASH_AUDIO_FILE).c_str()) == INVALID_FILE_ATTRIBUTES && !g_cfg.online_hdd.empty())
        dash_dir_ensure(g_cfg.online_hdd, p2, "Audio", DASH_AUDIO_FILE);
    std::vector<uint8_t> data;
    if (FILE *f = _wfopen(dashboard_xbe().c_str(), L"rb")) {
        fseek(f, 0, SEEK_END);
        data.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        fclose(f);
    }
    std::string sha = data.empty() ? std::string() : sha256_hex(data);
    if (sha != kProfileXod.sha256) {
        logf("dash: xonlinedash.xbe is another build (%s), not %s: the Xbox Live dashboard does not run; Live boxes cancel",
             sha.empty() ? "unreadable" : sha.c_str(), kProfileXod.name);
        return false;
    }
    state = 1;
    logf("dash: the Xbox Live dashboard is ready (%s)", narrow(dashboard_xbe()).c_str());
    return true;
}
