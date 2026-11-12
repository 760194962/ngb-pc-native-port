#include "patch.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <memory>
#include <vector>

extern "C" {
#include "xdelta3.h"
}

namespace patch {
namespace {

#include "ngb_base_layout.inc"

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~File() { close(); }
    void close()
    {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    bool open(const std::wstring &p)
    {
        h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        return h != INVALID_HANDLE_VALUE;
    }
    uint64_t size() const
    {
        LARGE_INTEGER s{};
        GetFileSizeEx(h, &s);
        return (uint64_t)s.QuadPart;
    }
    bool read(uint64_t at, void *buf, uint32_t n) const
    {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)at;
        DWORD got = 0;
        return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && ReadFile(h, buf, n, &got, nullptr) && got == n;
    }
};

// An XDVDFS directory table's entries (pre-order: left, right subtrees) with a visitor
struct Entry {
    std::string name;
    uint32_t sector, size;
    bool dir;
};
void each_entry(const uint8_t *t, uint32_t size, const std::function<void(const Entry &)> &f)
{
    std::function<void(uint32_t, int)> node = [&](uint32_t o, int depth) {
        if (depth > 64 || o * 4 + 14 > size) return;
        uint16_t left, right;
        memcpy(&left, t + o * 4, 2);
        memcpy(&right, t + o * 4 + 2, 2);
        if (left == 0xFFFF) return;
        Entry e;
        memcpy(&e.sector, t + o * 4 + 4, 4);
        memcpy(&e.size, t + o * 4 + 8, 4);
        e.dir = (t[o * 4 + 12] & 0x10) != 0;
        e.name.assign((const char *)t + o * 4 + 14, t[o * 4 + 13]);
        f(e);
        if (left) node(left, depth + 1);
        if (right) node(right, depth + 1);
    };
    node(0, 0);
}

std::string lower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// The Redump disc's files: path (lower case) -> byte offset in the image, size
bool redump_files(const File &f, std::map<std::string, std::pair<uint64_t, uint32_t>> &out)
{
    uint8_t hdr[2048];
    if (!f.read(kRedumpPartition + 0x10000ull, hdr, sizeof hdr) || memcmp(hdr, "MICROSOFT*XBOX*MEDIA", 20)) return false;
    uint32_t root, rsize;
    memcpy(&root, hdr + 20, 4);
    memcpy(&rsize, hdr + 24, 4);
    bool ok = true;
    std::function<void(uint32_t, uint32_t, const std::string &, int)> walk = [&](uint32_t sec, uint32_t size,
                                                                               const std::string &path, int depth) {
        if (depth > 16 || size > (16u << 20)) {
            ok = false;
            return;
        }
        std::vector<uint8_t> t(size);
        if (!f.read(kRedumpPartition + (uint64_t)sec * 2048, t.data(), size)) {
            ok = false;
            return;
        }
        each_entry(t.data(), size, [&](const Entry &e) {
            if (e.dir) {
                if (e.size) walk(e.sector, e.size, path + e.name + "/", depth + 1);
            } else
                out[lower(path + e.name)] = {kRedumpPartition + (uint64_t)e.sector * 2048, e.size};
        });
    };
    walk(root, rsize, "/", 0);
    return ok;
}

// The base image as pieces: fixed bytes (banner, header, directory tables), the Redump disc's files (0xFF to their
// sector's end), zeros between
struct Piece {
    uint64_t at, len;
    const uint8_t *bytes; // or nullptr: a file
    uint64_t from;        // the file's offset in the Redump image
    uint64_t pad;         // 0xFF bytes after a file
};

struct Base {
    File image;
    bool redump = false;
    std::vector<Piece> pieces; // by at
    std::vector<std::pair<uint64_t, uint8_t>> fixes; // base image offset, byte (sorted)

    bool make(std::wstring *why)
    {
        std::map<std::string, std::pair<uint64_t, uint32_t>> files;
        if (!redump_files(image, files)) {
            *why = L"no Xbox game partition at the Redump offset";
            return false;
        }
        for (const auto &b : kBanner) pieces.push_back({b.offset, b.size, b.bytes, 0, 0});
        pieces.push_back({0x10000, sizeof kBaseHeader, kBaseHeader, 0, 0});
        std::map<std::string, uint64_t> starts; // base offsets of the files the fixes name
        for (const auto &d : kDirs) {
            uint32_t len = (d.size + 2047) / 2048 * 2048;
            pieces.push_back({(uint64_t)d.sector * 2048, len, d.table, 0, 0});
            bool ok = true;
            each_entry(d.table, d.size, [&](const Entry &e) {
                if (e.dir) return;
                std::string path = std::string(d.path) + e.name;
                auto it = files.find(lower(path));
                if (it == files.end() || it->second.second != e.size) {
                    *why = std::wstring(L"the disc has no ") + std::wstring(path.begin(), path.end()) + L" as the patches expect";
                    ok = false;
                    return;
                }
                uint64_t at = (uint64_t)e.sector * 2048;
                pieces.push_back({at, e.size, nullptr, it->second.first, (2048 - e.size % 2048) % 2048});
                starts[path] = at;
            });
            if (!ok) return false;
        }
        for (const auto &f : kFixes) fixes.push_back({starts[f.path] + f.offset, f.value});
        std::sort(pieces.begin(), pieces.end(), [](const Piece &a, const Piece &b) { return a.at < b.at; });
        std::sort(fixes.begin(), fixes.end());
        return true;
    }

    bool read(uint64_t at, uint8_t *buf, uint32_t n) const
    {
        if (!redump) {
            uint64_t sz = kBaseSize;
            uint32_t got = at >= sz ? 0 : (uint32_t)std::min<uint64_t>(n, sz - at);
            if (got && !image.read(at, buf, got)) return false;
            memset(buf + got, 0, n - got);
            return true;
        }
        memset(buf, 0, n);
        uint64_t end = at + n;
        // the first piece that may overlap: the last one starting at or before at
        auto it = std::upper_bound(pieces.begin(), pieces.end(), at, [](uint64_t v, const Piece &p) { return v < p.at; });
        if (it != pieces.begin()) --it;
        for (; it != pieces.end() && it->at < end; ++it) {
            uint64_t a = std::max(at, it->at), b = std::min(end, it->at + it->len);
            if (a < b) {
                if (it->bytes) memcpy(buf + (a - at), it->bytes + (a - it->at), (size_t)(b - a));
                else if (!image.read(it->from + (a - it->at), buf + (a - at), (uint32_t)(b - a))) return false;
            }
            uint64_t pa = std::max(at, it->at + it->len), pb = std::min(end, it->at + it->len + it->pad);
            if (pa < pb) memset(buf + (pa - at), 0xFF, (size_t)(pb - pa));
        }
        auto f = std::lower_bound(fixes.begin(), fixes.end(), std::make_pair(at, (uint8_t)0));
        for (; f != fixes.end() && f->first < end; ++f) buf[f->first - at] = f->second;
        return true;
    }
};

bool open_base(Base &b, const std::wstring &image, std::wstring *why)
{
    if (!b.image.open(image)) {
        *why = L"cannot open the image";
        return false;
    }
    uint8_t hdr[2048];
    if (b.image.size() == kBaseSize && b.image.read(0x10000, hdr, sizeof hdr) && !memcmp(hdr, kBaseHeader, sizeof hdr))
        return true; // the base image itself (the patch's checksums tell if it is not)
    b.redump = true;
    return b.make(why);
}

std::wstring base_name(const std::wstring &p)
{
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

} // namespace

uint64_t image_bytes() { return kBaseSize; }

Source identify(const std::wstring &image, std::wstring *why)
{
    Base b;
    if (!open_base(b, image, why)) return Source::None;
    return b.redump ? Source::Redump : Source::Base;
}

bool apply(const std::wstring &image, const std::wstring &xdelta, const std::wstring &out_iso, install::Progress &p,
           std::wstring *error)
{
    auto fail = [&](const std::wstring &why) {
        *error = why;
        DeleteFileW(out_iso.c_str());
        return false;
    };
    Base base;
    std::wstring why;
    if (!open_base(base, image, &why)) return fail(why);
    File in;
    if (!in.open(xdelta)) return fail(L"cannot open " + base_name(xdelta));
    HANDLE out = CreateFileW(out_iso.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (out == INVALID_HANDLE_VALUE) return fail(L"cannot write " + out_iso);
    {
        std::lock_guard<std::mutex> l(p.lock);
        p.message = base_name(xdelta);
    }
    p.total = kBaseSize;
    p.done = 0;

    // The source's blocks, a few kept: a window's copies come from around one place
    constexpr uint32_t kBlock = 1u << 20;
    constexpr size_t kKeep = 64;
    struct Blk {
        xoff_t no;
        std::unique_ptr<uint8_t[]> data;
        uint64_t used;
    };
    std::vector<Blk> cache;
    uint64_t tick = 0;

    xd3_stream stream;
    xd3_config config;
    xd3_source source;
    memset(&stream, 0, sizeof stream);
    memset(&source, 0, sizeof source);
    xd3_init_config(&config, XD3_ADLER32);
    config.winsize = 8u << 20;
    if (xd3_config_stream(&stream, &config)) {
        CloseHandle(out);
        return fail(L"xdelta3: " + std::wstring(L"configuration"));
    }
    source.blksize = kBlock;
    source.curblkno = (xoff_t)-1;
    xd3_set_source(&stream, &source);

    std::vector<uint8_t> inbuf(1u << 20);
    uint64_t in_size = in.size(), in_at = 0;
    bool ok = true, done = false;
    std::wstring err;
    while (ok && !done) {
        if (p.cancel) {
            ok = false;
            err.clear();
            break;
        }
        uint32_t n = (uint32_t)std::min<uint64_t>(inbuf.size(), in_size - in_at);
        if (n && !in.read(in_at, inbuf.data(), n)) {
            ok = false;
            err = L"cannot read " + base_name(xdelta);
            break;
        }
        in_at += n;
        if (in_at >= in_size) xd3_set_flags(&stream, XD3_FLUSH | stream.flags);
        xd3_avail_input(&stream, inbuf.data(), n);
        for (;;) {
            int r = xd3_decode_input(&stream);
            if (r == XD3_INPUT) {
                if (in_at >= in_size) done = true;
                break;
            }
            if (r == XD3_OUTPUT) {
                DWORD put = 0;
                if (!WriteFile(out, stream.next_out, (DWORD)stream.avail_out, &put, nullptr) || put != stream.avail_out) {
                    ok = false;
                    err = L"cannot write " + out_iso + L" (disk full?)";
                    break;
                }
                p.done += stream.avail_out;
                xd3_consume_output(&stream);
                if (p.cancel) {
                    ok = false;
                    break;
                }
                continue;
            }
            if (r == XD3_GETSRCBLK) {
                xoff_t no = source.getblkno;
                Blk *b = nullptr;
                for (Blk &c : cache)
                    if (c.no == no) b = &c;
                if (!b) {
                    if (cache.size() < kKeep) cache.push_back({0, std::make_unique<uint8_t[]>(kBlock), 0});
                    b = &*std::min_element(cache.begin(), cache.end(), [](const Blk &a, const Blk &c) { return a.used < c.used; });
                    b->no = no;
                    if (!base.read((uint64_t)no * kBlock, b->data.get(), kBlock)) {
                        ok = false;
                        err = L"cannot read the image";
                        break;
                    }
                }
                b->used = ++tick;
                uint64_t left = kBaseSize - std::min<uint64_t>(kBaseSize, (uint64_t)no * kBlock);
                source.curblkno = no;
                source.curblk = b->data.get();
                source.onblk = (usize_t)std::min<uint64_t>(kBlock, left);
                continue;
            }
            if (r == XD3_GOTHEADER || r == XD3_WINSTART || r == XD3_WINFINISH) continue;
            ok = false;
            std::string m = stream.msg ? stream.msg : "decode error";
            err = L"xdelta3: " + std::wstring(m.begin(), m.end()) +
                  L" (is this the patch for the US release? the image may differ)";
            break;
        }
    }
    xd3_close_stream(&stream);
    xd3_free_stream(&stream);
    CloseHandle(out);
    if (!ok) return fail(p.cancel ? L"" : err);
    return true;
}

} // namespace patch
