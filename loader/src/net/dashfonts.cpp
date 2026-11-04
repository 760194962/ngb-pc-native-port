// The Xbox dashboard's fonts for the title updater (net/dashfonts.h): Ninja Gaiden 2004's update.xbe draws its text with
// the console's own fonts, C:\fonts\*.xtf (the dashboard's partition, \Device\Harddisk0\Partition2; r766: without them
// its font object stays null and it faults at 0x334A4). They are the player's: copied once from the registered xemu hard
// disk image (black.ini online_hdd, qcow2 or raw; read only) into the port's own hard disk, never shipped.
#include "net/dashfonts.h"

#include "common.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Reads a disk image: xemu's qcow2 (unallocated clusters read as zeros; no compression, no encryption) or a raw image
struct Disk {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool qcow = false;
    uint32_t cluster_bits = 0, l1_size = 0;
    uint64_t l1_off = 0, cached_l2_off = 0;
    std::vector<uint8_t> l2;
    std::vector<uint64_t> l1;

    ~Disk()
    {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
    static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
    bool raw(uint64_t off, void *buf, uint32_t n)
    {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)off;
        DWORD got = 0;
        return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && ReadFile(h, buf, n, &got, nullptr) && got == n;
    }
    bool open(const std::wstring &path, std::string &why)
    {
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        uint8_t hd[104];
        if (h == INVALID_HANDLE_VALUE || !raw(0, hd, sizeof hd)) {
            why = "cannot read the hard disk image";
            return false;
        }
        qcow = !memcmp(hd, "QFI\xfb", 4);
        if (!qcow) return true;
        cluster_bits = be32(hd + 20);
        l1_size = be32(hd + 36);
        l1_off = be64(hd + 40);
        if (be32(hd + 32) || cluster_bits < 9 || cluster_bits > 21 || l1_size > (1u << 24)) {
            why = "an encrypted or unexpected qcow2 image";
            return false;
        }
        std::vector<uint8_t> t(8ull * l1_size);
        if (!raw(l1_off, t.data(), (uint32_t)t.size())) {
            why = "cannot read the qcow2 L1 table";
            return false;
        }
        l1.resize(l1_size);
        for (uint32_t i = 0; i < l1_size; i++) l1[i] = be64(&t[8ull * i]) & 0x00FFFFFFFFFFFE00ull;
        l2.resize((size_t)1 << cluster_bits);
        return true;
    }
    bool read(uint64_t off, uint8_t *buf, uint32_t n)
    {
        if (!qcow) return raw(off, buf, n);
        uint32_t cs = 1u << cluster_bits, l2_entries = cs / 8;
        while (n) {
            uint64_t vcl = off >> cluster_bits;
            uint32_t within = (uint32_t)(off & (cs - 1)), take = n < cs - within ? n : cs - within;
            uint64_t l1i = vcl / l2_entries, l2i = vcl % l2_entries, host = 0;
            if (l1i < l1.size() && l1[l1i]) {
                if (l1[l1i] != cached_l2_off) {
                    if (!raw(l1[l1i], l2.data(), cs)) return false;
                    cached_l2_off = l1[l1i];
                }
                uint64_t e = be64(&l2[8 * l2i]);
                if (e & (1ull << 62)) return false; // compressed
                host = e & 0x00FFFFFFFFFFFE00ull;
            }
            if (host) {
                if (!raw(host + within, buf, take)) return false;
            } else {
                memset(buf, 0, take);
            }
            off += take, buf += take, n -= take;
        }
        return true;
    }
};

// FATX (the Xbox hard disk's file system): a 4 KB header, a FAT of 16-bit entries (32-bit from 0xFFF0 clusters on),
// then clusters numbered from 1; directories are clusters of 64-byte entries {name length, attributes, name[42],
// first cluster, size, times}
struct Fatx {
    Disk &d;
    uint64_t part, fat, data;
    uint32_t cluster = 0, fat_w = 2;

    Fatx(Disk &disk, uint64_t offset, uint64_t size) : d(disk), part(offset)
    {
        uint8_t sb[16];
        if (!d.read(part, sb, sizeof sb) || memcmp(sb, "FATX", 4)) return;
        uint32_t spc;
        memcpy(&spc, sb + 8, 4);
        cluster = spc * 512;
        if (!cluster) return;
        uint64_t n = size / cluster;
        fat_w = n < 0xFFF0 ? 2 : 4;
        fat = part + 0x1000;
        data = fat + ((n * fat_w + 0xFFF) & ~0xFFFull);
    }
    uint32_t next(uint32_t c)
    {
        uint32_t v = 0;
        d.read(fat + (uint64_t)c * fat_w, (uint8_t *)&v, fat_w);
        return v;
    }
    bool last(uint32_t c) { return !c || c >= (fat_w == 2 ? 0xFFF8u : 0xFFFFFFF8u); }
    struct Entry {
        std::string name;
        bool dir;
        uint32_t first, size;
    };
    std::vector<Entry> list(uint32_t first)
    {
        std::vector<Entry> out;
        std::vector<uint8_t> b(cluster);
        for (uint32_t c = first, guard = 0; !last(c) && guard < 4096; c = next(c), guard++) {
            if (!d.read(data + (uint64_t)(c - 1) * cluster, b.data(), cluster)) break;
            for (uint32_t i = 0; i + 64 <= cluster; i += 64) {
                uint8_t len = b[i];
                if (len == 0x00 || len == 0xFF) return out;
                if (len == 0xE5 || len > 42) continue;
                Entry e;
                e.name.assign((const char *)&b[i + 2], len);
                e.dir = (b[i + 1] & 0x10) != 0;
                memcpy(&e.first, &b[i + 44], 4);
                memcpy(&e.size, &b[i + 48], 4);
                out.push_back(e);
            }
        }
        return out;
    }
    bool copy(const Entry &e, const std::wstring &to)
    {
        HANDLE h = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        std::vector<uint8_t> b(cluster);
        uint32_t left = e.size;
        bool ok = true;
        for (uint32_t c = e.first; left && !last(c); c = next(c)) {
            uint32_t n = left < cluster ? left : cluster;
            DWORD put = 0;
            if (!d.read(data + (uint64_t)(c - 1) * cluster, b.data(), n) || !WriteFile(h, b.data(), n, &put, nullptr) || put != n) {
                ok = false;
                break;
            }
            left -= n;
        }
        CloseHandle(h);
        if (!ok || left) DeleteFileW(to.c_str());
        return ok && !left;
    }
};

bool has_xtf(const std::wstring &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.xtf").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

} // namespace

bool dash_fonts_ensure(const std::wstring &image, const std::wstring &partition2)
{
    std::wstring dest = partition2 + L"\\fonts";
    if (has_xtf(dest)) return true;
    if (image.empty()) {
        logf("dash fonts: none in %s and no registered hard disk image (black.ini online_hdd) to take them from",
             narrow(dest).c_str());
        return false;
    }
    Disk d;
    std::string why;
    if (!d.open(image, why)) {
        logf("dash fonts: %s (%s)", why.c_str(), narrow(image).c_str());
        return false;
    }
    Fatx c(d, 0x8CA80000ull, 0x1F400000ull); // C:, the dashboard's partition
    if (!c.cluster) {
        logf("dash fonts: the image has no FATX C: partition (%s)", narrow(image).c_str());
        return false;
    }
    CreateDirectoryW(partition2.c_str(), nullptr);
    CreateDirectoryW(dest.c_str(), nullptr);
    int copied = 0;
    for (const Fatx::Entry &e : c.list(1)) {
        if (!e.dir || _stricmp(e.name.c_str(), "fonts")) continue;
        for (const Fatx::Entry &f : c.list(e.first)) {
            if (f.dir) continue;
            if (c.copy(f, dest + L"\\" + widen(f.name))) copied++;
            else logf("dash fonts: could not copy %s", f.name.c_str());
        }
    }
    logf("dash fonts: %d file(s) of C:\\fonts copied from %s into %s", copied, narrow(image).c_str(), narrow(dest).c_str());
    return copied > 0;
}

namespace {

// Copies the FATX directory starting at cluster first into the host folder to, recursively; false on any failure
bool copy_tree(Fatx &c, uint32_t first, const std::wstring &to, int depth, int &files)
{
    if (depth > 8 || !CreateDirectoryW(to.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    for (const Fatx::Entry &e : c.list(first)) {
        std::wstring p = to + L"\\" + widen(e.name);
        if (e.dir ? !copy_tree(c, e.first, p, depth + 1, files) : !c.copy(e, p)) {
            logf("dash: could not copy %s", narrow(p).c_str());
            return false;
        }
        if (!e.dir) files++;
    }
    return true;
}

void remove_tree(const std::wstring &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

} // namespace

bool dash_dir_ensure(const std::wstring &image, const std::wstring &partition2, const char *dir, const wchar_t *needed)
{
    std::wstring dest = partition2 + L"\\" + widen(dir), part = dest + L".part";
    if (GetFileAttributesW((dest + L"\\" + needed).c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    if (image.empty()) {
        logf("dash: no %s and no registered hard disk image (black.ini online_hdd) to take it from", narrow(dest).c_str());
        return false;
    }
    Disk d;
    std::string why;
    if (!d.open(image, why)) {
        logf("dash: %s (%s)", why.c_str(), narrow(image).c_str());
        return false;
    }
    Fatx c(d, 0x8CA80000ull, 0x1F400000ull); // C:, the dashboard's partition
    if (!c.cluster) {
        logf("dash: the image has no FATX C: partition (%s)", narrow(image).c_str());
        return false;
    }
    for (const Fatx::Entry &e : c.list(1)) {
        if (!e.dir || _stricmp(e.name.c_str(), dir)) continue;
        CreateDirectoryW(partition2.c_str(), nullptr);
        remove_tree(part); // what an interrupted copy left
        int files = 0;
        bool ok = copy_tree(c, e.first, part, 0, files) && GetFileAttributesW((part + L"\\" + needed).c_str()) != INVALID_FILE_ATTRIBUTES;
        if (ok) {
            remove_tree(dest); // a folder without the needed file
            ok = MoveFileW(part.c_str(), dest.c_str()) != 0;
        }
        if (!ok) remove_tree(part);
        logf("dash: C:\\%s %s (%d files) from %s into %s", dir, ok ? "copied" : "NOT copied", files, narrow(image).c_str(),
             narrow(dest).c_str());
        return ok;
    }
    logf("dash: the image's C: has no %s folder (%s)", dir, narrow(image).c_str());
    return false;
}

bool dash_file_ensure(const std::wstring &image, const std::wstring &partition2, const char *name)
{
    std::wstring dest = partition2 + L"\\" + widen(name), part = dest + L".part";
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    if (image.empty()) {
        logf("dash: no %s and no registered hard disk image (black.ini online_hdd) to take it from", narrow(dest).c_str());
        return false;
    }
    Disk d;
    std::string why;
    if (!d.open(image, why)) {
        logf("dash: %s (%s)", why.c_str(), narrow(image).c_str());
        return false;
    }
    Fatx c(d, 0x8CA80000ull, 0x1F400000ull); // C:, the dashboard's partition
    if (!c.cluster) {
        logf("dash: the image has no FATX C: partition (%s)", narrow(image).c_str());
        return false;
    }
    for (const Fatx::Entry &e : c.list(1)) {
        if (e.dir || _stricmp(e.name.c_str(), name)) continue;
        CreateDirectoryW(partition2.c_str(), nullptr);
        bool ok = c.copy(e, part) && MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        if (!ok) DeleteFileW(part.c_str());
        logf("dash: C:\\%s %s from %s into %s", name, ok ? "copied" : "NOT copied", narrow(image).c_str(), narrow(dest).c_str());
        return ok;
    }
    logf("dash: the image's C: has no %s (%s)", name, narrow(image).c_str());
    return false;
}

static std::string sha256_of(const std::vector<uint8_t> &data)
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

// The disc's two programs with their one changed byte put back (found against the .text digests of their headers,
// 2026-11-01; the Redump image "Ninja Gaiden (USA, Japan)" carries the same files): the title updater's patches are
// made from these bytes (r769: the downloader came out with 0xEB where the update has 0x7D, and the updater gave up)
bool updater_signed_sources(const std::wstring &game_dir, const std::wstring &cache)
{
    struct Fix {
        const wchar_t *name;
        const char *sha256; // the disc's file
        uint32_t offset;    // file offset of the byte
        const char *signed_sha256; // the same file as signed (Redump's "Ninja Gaiden (USA, Asia)": build_profile.h kProfileAliases)
    };
    const Fix fixes[] = {
        {L"default.xbe", "bea2a85eea7e6e0ba6c3e201966a46b24b27e25b3307a29bc3de5b4254791308", 0x2F9D10,
         "86c23d787ecf9f6400b9378544a49b62cfdd478dc04d6ef01ceed2a6429e84c0"},
        {L"downloader.xbe", "fcce4a17781b5a84483b821e0e8e39f4b4ea97e6b386cbb206f2b6df887f04ce", 0xD895,
         "3bc7c979bc0f4b641f25a376afbc98e6ec614b2e5080b64feae022d9cce9e573"},
    };
    int as_signed = 0;
    CreateDirectoryW(cache.c_str(), nullptr);
    int made = 0;
    for (const Fix &f : fixes) {
        std::wstring src = game_dir + L"\\" + f.name, dst = cache + L"\\" + f.name;
        HANDLE h = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER size = {};
        GetFileSizeEx(h, &size);
        std::vector<uint8_t> b((size_t)size.QuadPart);
        DWORD got = 0;
        bool ok = ReadFile(h, b.data(), (DWORD)b.size(), &got, nullptr) && got == b.size();
        CloseHandle(h);
        std::string sha = ok ? sha256_of(b) : std::string();
        if (sha == f.signed_sha256) {
            as_signed++;
            continue;
        }
        if (!ok || sha != f.sha256 || f.offset >= b.size() || b[f.offset] != 0xEB) {
            logf("updater: %s is not the disc's known file; the update is made from it as it is", narrow(f.name).c_str());
            continue;
        }
        b[f.offset] = 0x7D;
        HANDLE o = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD put = 0;
        if (o != INVALID_HANDLE_VALUE && WriteFile(o, b.data(), (DWORD)b.size(), &put, nullptr) && put == b.size()) made++;
        if (o != INVALID_HANDLE_VALUE) CloseHandle(o);
    }
    if (as_signed) logf("updater: %d of the disc's programs already as signed; read from the game folder", as_signed);
    if (made && g_cfg.game_override.empty()) g_cfg.game_override = cache;
    logf("updater: %d of the disc's programs as signed (one byte each put back) in %s, read by the updater instead", made,
         narrow(cache).c_str());
    return made > 0;
}
