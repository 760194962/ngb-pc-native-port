#include "install.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace install {

namespace {

// XDVDFS: the volume descriptor is at sector 32 of the game partition: "MICROSOFT*XBOX*MEDIA", the root directory's
// sector and size. A directory is a binary tree of entries {u16 left, u16 right (in dwords from the directory's
// start, 0 = none), u32 sector, u32 size, u8 attributes (0x10 = directory), u8 name length, name}, each dword
// aligned; the rest of a sector is padding (0xFF). An extract-xiso image starts with the partition; a full disc
// image has it further in (XGD1 0x18300000, XGD2 0xFD90000, XGD3 0x2080000).
constexpr uint32_t kSector = 2048;
const uint64_t kPartitions[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};
const char kMagic[] = "MICROSOFT*XBOX*MEDIA";

struct Entry {
    std::wstring path; // relative, with backslashes
    uint64_t offset;   // in the image
    uint32_t size;
    bool dir;
};

bool read_at(HANDLE h, uint64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    DWORD got = 0;
    return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && ReadFile(h, buf, n, &got, nullptr) && got == n;
}

bool find_partition(HANDLE h, uint64_t *part, uint32_t *root_sector, uint32_t *root_size)
{
    for (uint64_t p : kPartitions) {
        uint8_t vd[kSector];
        if (!read_at(h, p + 32ull * kSector, vd, kSector)) continue;
        if (memcmp(vd, kMagic, 20) || memcmp(vd + 0x7EC, kMagic, 20)) continue;
        memcpy(root_sector, vd + 20, 4);
        memcpy(root_size, vd + 24, 4);
        *part = p;
        return true;
    }
    return false;
}

std::wstring widen(const char *s, size_t n)
{
    std::wstring w;
    for (size_t i = 0; i < n; i++) w += (wchar_t)(uint8_t)s[i]; // XDVDFS names are 8-bit (ASCII in practice)
    return w;
}

bool walk(HANDLE h, uint64_t part, uint32_t sector, uint32_t size, const std::wstring &prefix, std::vector<Entry> &out, int depth);

bool walk_tree(HANDLE h, uint64_t part, const std::vector<uint8_t> &dir, uint32_t off, const std::wstring &prefix,
               std::vector<Entry> &out, int depth, int guard)
{
    if (guard > 100000 || (size_t)off * 4 + 14 > dir.size()) return false;
    const uint8_t *e = &dir[(size_t)off * 4];
    uint16_t left, right;
    uint32_t sector, size;
    memcpy(&left, e, 2);
    memcpy(&right, e + 2, 2);
    memcpy(&sector, e + 4, 4);
    memcpy(&size, e + 8, 4);
    uint8_t attr = e[12], len = e[13];
    if ((size_t)off * 4 + 14 + len > dir.size()) return false;
    if (left && left != 0xFFFF && !walk_tree(h, part, dir, left, prefix, out, depth, guard + 1)) return false;
    std::wstring name = prefix + widen((const char *)e + 14, len);
    bool is_dir = (attr & 0x10) != 0;
    out.push_back({name, part + (uint64_t)sector * kSector, size, is_dir});
    if (is_dir && size && !walk(h, part, sector, size, name + L"\\", out, depth + 1)) return false;
    if (right && right != 0xFFFF && !walk_tree(h, part, dir, right, prefix, out, depth, guard + 1)) return false;
    return true;
}

bool walk(HANDLE h, uint64_t part, uint32_t sector, uint32_t size, const std::wstring &prefix, std::vector<Entry> &out, int depth)
{
    if (depth > 32 || size > 64u * 1024 * 1024) return false;
    std::vector<uint8_t> dir((size + kSector - 1) / kSector * kSector);
    if (!read_at(h, part + (uint64_t)sector * kSector, dir.data(), (DWORD)dir.size())) return false;
    return walk_tree(h, part, dir, 0, prefix, out, depth, 0);
}

bool list(const std::wstring &iso, std::vector<Entry> &out, std::wstring *error)
{
    HANDLE h = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *error = L"open";
        return false;
    }
    uint64_t part;
    uint32_t root, root_size;
    bool ok = find_partition(h, &part, &root, &root_size);
    if (!ok) *error = L"not an Xbox disc image";
    else if (!(ok = walk(h, part, root, root_size, L"", out, 0))) *error = L"damaged directory";
    CloseHandle(h);
    return ok;
}

bool make_dirs(const std::wstring &path)
{
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash < 3) return false;
    return make_dirs(path.substr(0, slash)) && (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
}

std::string sha256_file(const std::wstring &path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (!BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) &&
        !BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0)) {
        std::vector<uint8_t> buf(1 << 20);
        DWORD got;
        while (ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) BCryptHashData(hash, buf.data(), got, 0);
        uint8_t d[32];
        if (!BCryptFinishHash(hash, d, 32, 0)) {
            char hex[65];
            for (int i = 0; i < 32; i++) sprintf_s(hex + i * 2, 3, "%02x", d[i]);
            out = hex;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return out;
}

// The builds the port knows (loader/src/build_profile.h): default.xbe tells which, then the programs it launches. The
// US release and the basic edition have the NES trilogies only; the Hurricane editions added the SNES ones.
const struct {
    const char *sha256;
    Edition edition;
    bool snes; // has the SNES trilogy
    const wchar_t *name;
} kBuilds[] = {
    {"aa3c5bec2923f83ccb37fe6a63ea5516b26d477b53c679a1620d19e4a8e4e8f6", Edition::X21, true, L"x2.1 zh"},
    {"535778c8a4623195c8f320b85d2d139f20ff13c020d40150adaed54cfdeffc38", Edition::X21Jp, true, L"x2.1 jp"},
    {"00cef52546618eb0149d7895f9042403e95f361b5483aac38933e799c67a50e1", Edition::BasicX2, false, L"x2 basic"},
    {"df1d2af0154d771f42b77ec4df03a862417ac457ff0d39fec846c40cb55ffbda", Edition::Us, false, L"US"},
};
const struct {
    const wchar_t *file;
    const char *sha256;
    bool snes;
} kFiles[] = {
    {L"NinjaTomoe.xbe", "2aff93ad098b3b5762d294d9c17a44c36d04bfa31843e413cf5e2963f14282ff", false},
    {L"NinjaTrilogy.xbe", "31f8efd073abba2ac339ee44f4b13011844afc470b166f826dc12f6b79ff5621", false},
    {L"NinjaFCJ.xbe", "b5b8754e6008f3b2e773c1eb9afe50ee9decebf0eadb945a9953369b1b9f4f31", true},
    {L"NinjaFCU.xbe", "9fac4bb5162b8a99c65c0fbb6289ee81db4c42bdbd253807e74261a68e3399b1", true},
};

} // namespace

uint64_t image_size(const std::wstring &iso, std::wstring *error)
{
    std::vector<Entry> files;
    if (!list(iso, files, error)) return 0;
    uint64_t total = 0;
    for (const Entry &e : files)
        if (!e.dir) total += e.size;
    return total;
}

bool check_game(const std::wstring &dir, std::wstring *detail, Edition *edition)
{
    if (edition) *edition = Edition::None;
    std::wstring xbe = dir + L"\\default.xbe";
    if (GetFileAttributesW(xbe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        *detail = L"default.xbe: missing";
        return false;
    }
    std::string hash = sha256_file(xbe);
    const auto *build = std::find_if(std::begin(kBuilds), std::end(kBuilds), [&](const auto &b) { return hash == b.sha256; });
    if (build == std::end(kBuilds)) {
        *detail = L"default.xbe: another build";
        return false;
    }
    for (const auto &f : kFiles) {
        if (f.snes && !build->snes) continue;
        std::wstring p = dir + L"\\" + f.file;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
            *detail = std::wstring(f.file) + L": missing";
            return false;
        }
        if (sha256_file(p) != f.sha256) {
            *detail = std::wstring(f.file) + L": another build";
            return false;
        }
    }
    *detail = std::wstring(build->name) + (build->snes ? L": default.xbe + 4" : L": default.xbe + 2");
    if (edition) *edition = build->edition;
    return true;
}

void extract(const std::wstring &iso, const std::wstring &dest, Progress &p)
{
    auto fail = [&](const std::wstring &why) {
        std::lock_guard<std::mutex> g(p.lock);
        p.message = why;
        p.state = 3;
    };
    p.state = 1;
    std::vector<Entry> files;
    std::wstring error;
    if (!list(iso, files, &error)) return fail(error);
    uint64_t total = 0;
    int count = 0;
    for (const Entry &e : files)
        if (!e.dir) total += e.size, count++;
    p.total = total;
    p.file_count = count;
    if (!make_dirs(dest)) return fail(L"cannot make " + dest);
    HANDLE h = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail(L"cannot open the image");
    std::vector<uint8_t> buf(4 << 20);
    for (const Entry &e : files) {
        if (p.cancel) {
            CloseHandle(h);
            p.state = 4;
            return;
        }
        std::wstring out = dest + L"\\" + e.path;
        if (e.dir) {
            if (!make_dirs(out)) return CloseHandle(h), fail(L"cannot make " + out);
            continue;
        }
        {
            std::lock_guard<std::mutex> g(p.lock);
            p.message = e.path;
        }
        HANDLE o = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (o == INVALID_HANDLE_VALUE) return CloseHandle(h), fail(L"cannot write " + out);
        uint64_t left = e.size, at = e.offset;
        bool ok = true;
        while (left && ok && !p.cancel) {
            DWORD n = (DWORD)std::min<uint64_t>(left, buf.size()), wrote = 0;
            ok = read_at(h, at, buf.data(), n) && WriteFile(o, buf.data(), n, &wrote, nullptr) && wrote == n;
            left -= n;
            at += n;
            p.done += n;
        }
        CloseHandle(o);
        if (!ok) return CloseHandle(h), fail(L"read / write error at " + e.path);
        p.files++;
    }
    CloseHandle(h);
    if (p.cancel) {
        p.state = 4;
        return;
    }
    std::wstring detail;
    if (!check_game(dest, &detail)) return fail(L"installed, but " + detail);
    p.state = 2;
}

} // namespace install
