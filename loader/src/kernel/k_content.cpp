// Downloadable content on the hard disk, signed for this console as the Xbox does when it installs content.
//
// XAPI checks each TDATA\<title>\$c\<offer>\ContentMeta.xbx before the game may use it (Ninja Gaiden 2004 1.06.04:
// FUN_0031A55A / 0x31A475, 2026-10-31): the header is "XCSF" at +0x14 and its size at +0x18; the first 20 bytes must
// be HMAC-SHA1(K, bytes 0x14 .. size), K = HMAC-SHA1(XboxHDKey, the title id at +0x24). Content copied from elsewhere
// (the Hurricane Packs' installer leaves the signature zero) fails with "content damaged", so at start every meta
// file of the running title whose signature does not match is signed again, with the key XAPI reads there: the
// saves' console key (ktable.cpp xapi_hdkey: fixed, whatever online play does). Only those 20 bytes change.
#include "common.h"
#include "kernel/kexports.h"
#include "kernel/xboxkrnl.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

void sign_title_content(uint32_t title_id)
{
    wchar_t t[16];
    swprintf(t, 16, L"%08x", title_id);
    std::wstring dir = host_full_path(g_cfg.hdd_dir) + L"\\Partition1\\TDATA\\" + t + L"\\$c";
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    int good = 0, signed_now = 0, other = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::wstring meta = dir + L"\\" + fd.cFileName + L"\\ContentMeta.xbx";
        HANDLE h = CreateFileW(meta.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            other++;
            continue;
        }
        std::vector<uint8_t> b(GetFileSize(h, nullptr));
        DWORD got = 0;
        bool ok = !b.empty() && ReadFile(h, b.data(), (DWORD)b.size(), &got, nullptr) && got == b.size() && b.size() >= 0x28;
        uint32_t magic = 0, size = 0, title = 0;
        if (ok) {
            memcpy(&magic, &b[0x14], 4);
            memcpy(&size, &b[0x18], 4);
            memcpy(&title, &b[0x24], 4);
            ok = magic == 0x46534358 && size >= 0x28 && size <= b.size(); // "XCSF"
        }
        if (!ok) {
            other++;
            CloseHandle(h);
            continue;
        }
        uint8_t key[20], sig[20];
        xk::XcHMAC(xapi_hdkey(), 16, (const uint8_t *)&title, 4, nullptr, 0, key);
        xk::XcHMAC(key, 20, &b[0x14], size - 0x14, nullptr, 0, sig);
        if (!memcmp(sig, b.data(), 20)) {
            good++;
        } else {
            DWORD wrote = 0;
            SetFilePointer(h, 0, nullptr, FILE_BEGIN);
            if (WriteFile(h, sig, 20, &wrote, nullptr) && wrote == 20) signed_now++;
            else other++;
        }
        CloseHandle(h);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    logf("content: TDATA\\%08x\\$c: %d signed for this console now, %d already were, %d not content (or unwritable)",
         title_id, signed_now, good, other);
}
