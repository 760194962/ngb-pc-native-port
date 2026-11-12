// Checks the launcher's patching (launcher/src/patch.cpp) from the command line: configure with -DNGB_PATCH_TEST=ON.
//   ngb_patch_test <image: base iso or Redump disc> <patch.xdelta> <out.iso>
#include "../launcher/src/patch.h"

#include <windows.h>

#include <cstdio>
#include <thread>

int wmain(int argc, wchar_t **argv)
{
    if (argc != 4) {
        fwprintf(stderr, L"usage: ngb_patch_test image patch.xdelta out.iso\n");
        return 2;
    }
    std::wstring why;
    patch::Source s = patch::identify(argv[1], &why);
    wprintf(L"source: %s %s\n", s == patch::Source::Base ? L"base image" : s == patch::Source::Redump ? L"Redump disc" : L"none",
            why.c_str());
    if (s == patch::Source::None) return 1;
    install::Progress p;
    std::wstring err;
    DWORD t0 = GetTickCount();
    bool ok = false;
    std::thread w([&] { ok = patch::apply(argv[1], argv[2], argv[3], p, &err); });
    while (true) {
        DWORD r = WaitForSingleObject((HANDLE)w.native_handle(), 2000);
        wprintf(L"  %.2f / %.2f GB\n", p.done / 1073741824.0, p.total / 1073741824.0);
        if (r == WAIT_OBJECT_0) break;
    }
    w.join();
    wprintf(L"%s in %lu s %s\n", ok ? L"done" : L"FAILED", (GetTickCount() - t0) / 1000, err.c_str());
    return ok ? 0 : 1;
}
