#include "d3d/shader_cache.h"

#include "common.h"
#include "perf.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xd3d {

namespace {

std::mutex g_lock;

// Entry file: "NGBS", version, flags, the profile and the HLSL (each length-prefixed), then the bytecode; an empty
// bytecode records a source this profile does not compile (callers try ps_2_0 before ps_2_a, and so on).
constexpr uint32_t kMagic = 0x5342474E, kVersion = 1;

struct Entry {
    uint32_t flags = 0;
    std::string profile, hlsl;
    std::vector<uint8_t> code; // empty: does not compile
};

uint64_t fnv1a(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

std::wstring cache_dir() { return g_cfg.port_dir + L"\\cache\\shaders"; }

std::wstring entry_path(const std::string &hlsl, const char *profile, UINT flags)
{
    uint64_t h = 0xCBF29CE484222325ull;
    h = fnv1a(h, profile, strlen(profile));
    h = fnv1a(h, &flags, sizeof flags);
    h = fnv1a(h, hlsl.data(), hlsl.size());
    wchar_t name[40];
    swprintf(name, 40, L"\\%016llx.bin", (unsigned long long)h);
    return cache_dir() + name;
}

// A cache entry's generator epoch, in a sidecar file: "*.gen" holds one uint32. Entries whose sidecar is older were
// made by a different HLSL generator; they stay on disk (never deleted by age) but are not prewarmed. An entry made
// before this sidecar existed has no file: it is migrated to the current epoch once, so the first run of this build
// does not turn the whole existing cache into runtime work. A hit read from the file in play (not prewarmed) marks a
// sidecar-less entry current; a prewarmed hit touches no file.
constexpr uint32_t kShaderEpoch = 1;

std::wstring epoch_path(const std::wstring &entry) { return entry + L".gen"; }

uint32_t read_epoch(const std::wstring &entry)
{
    uint32_t e = 0;
    if (FILE *f = _wfopen(epoch_path(entry).c_str(), L"rb")) {
        if (fread(&e, sizeof e, 1, f) != 1) e = 0;
        fclose(f);
    }
    return e;
}

void write_epoch(const std::wstring &entry, uint32_t epoch)
{
    std::wstring tmp = epoch_path(entry) + L".tmp";
    if (FILE *f = _wfopen(tmp.c_str(), L"wb")) {
        bool ok = fwrite(&epoch, sizeof epoch, 1, f) == 1 && !ferror(f);
        fclose(f);
        if (!ok || !MoveFileExW(tmp.c_str(), epoch_path(entry).c_str(), MOVEFILE_REPLACE_EXISTING))
            DeleteFileW(tmp.c_str());
    }
}

// A .bin is usable only if it belongs to this generator and was not overwritten after its sidecar was written. The
// second check catches a downgrade in the same folder: an older build writes .bin without knowing about .gen, so a
// current-looking sidecar next to a newer .bin would otherwise prewarm (or read) that older bytecode.
bool cache_entry_current(const std::wstring &path)
{
    uint32_t epoch = read_epoch(path);
    if (epoch && epoch != kShaderEpoch) return false;
    if (!epoch) return true; // before the sidecar existed: prewarm migrates it once
    WIN32_FILE_ATTRIBUTE_DATA bin = {}, gen = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &bin) ||
        !GetFileAttributesExW(epoch_path(path).c_str(), GetFileExInfoStandard, &gen))
        return true; // a missing timestamp is not proof; parse_file still validates the file
    ULARGE_INTEGER bt, gt;
    bt.LowPart = bin.ftLastWriteTime.dwLowDateTime;
    bt.HighPart = bin.ftLastWriteTime.dwHighDateTime;
    gt.LowPart = gen.ftLastWriteTime.dwLowDateTime;
    gt.HighPart = gen.ftLastWriteTime.dwHighDateTime;
    return bt.QuadPart <= gt.QuadPart; // .gen is written after .bin; a newer .bin came from another build
}

std::vector<uint8_t> read_all(const std::wstring &path, bool *ok)
{
    std::vector<uint8_t> d;
    FILE *f = _wfopen(path.c_str(), L"rb");
    *ok = f != nullptr;
    if (!f) return d;
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    fclose(f);
    return d;
}

// One entry's bytes (a .bin file, or a record of the shipped seed)
bool parse_bytes(const uint8_t *d, size_t size, Entry *e)
{
    size_t at = 0;
    auto u32 = [&](uint32_t *v) {
        if (at + 4 > size) return false;
        memcpy(v, d + at, 4);
        at += 4;
        return true;
    };
    auto field = [&](std::string *s) {
        uint32_t n;
        if (!u32(&n) || n > size - at) return false;
        s->assign((const char *)d + at, n);
        at += n;
        return true;
    };
    uint32_t magic, version, len;
    if (!u32(&magic) || magic != kMagic || !u32(&version) || version != kVersion || !u32(&e->flags)) return false;
    if (!field(&e->profile) || !field(&e->hlsl) || !u32(&len) || at + len != size) return false;
    e->code.assign(d + at, d + size);
    return true;
}

bool parse_file(const std::wstring &path, Entry *e)
{
    bool ok;
    std::vector<uint8_t> d = read_all(path, &ok);
    return ok && parse_bytes(d.data(), d.size(), e);
}

// The entries read at start (prewarm_shaders), by file path: a lookup in play then reads no file. Opening one took about
// 0.9 ms (19 entries, 16 ms in the opening cutscene's frame on the RTX 3060, perf 2026-10-22_07-24-25).
// Every readable entry is made into its shader object at start: a cache entry first used in play may not create a
// shader on the render thread. The idle-based "cold" entries and the run-count pruning were removed: they could
// compile or create a shader during a scene (a hitch) just because the player had not visited it for a few runs, and
// the last 10 s of a short run's use index could be lost. Unreadable files are still deleted; valid entries are kept
// (a few hundred files, a few MB) because being unused is not proof that no source will ask for it again.
std::unordered_map<std::wstring, Entry> g_mem;

// 1: the bytecode is in *code; -1: known not to compile; 0: not in the cache
int read_entry(const std::wstring &path, const std::string &hlsl, const char *profile, UINT flags, ID3DBlob **code)
{
    Entry file;
    const Entry *e = nullptr;
    auto it = g_mem.find(path);
    if (it != g_mem.end()) {
        e = &it->second; // prewarmed and validated at start
    } else {
        if (!cache_entry_current(path)) {
            static int noted;
            if (noted++ < 4) logf("[shader cache] old .bin ignored (recompiling): %ls", path.c_str());
            return 0; // another generator, or .bin overwritten after its .gen
        }
        if (parse_file(path, &file)) {
            e = &file;
            if (!read_epoch(path)) write_epoch(path, kShaderEpoch); // a legacy entry used in play: current next start
        }
    }
    if (!e || e->flags != flags || e->profile != profile || e->hlsl != hlsl) return 0; // not cached, or another source
    if (e->code.empty()) return -1;
    if (FAILED(D3DCreateBlob(e->code.size(), code))) return 0;
    memcpy((*code)->GetBufferPointer(), e->code.data(), e->code.size());
    return 1;
}

void write_entry(const std::wstring &path, const std::string &hlsl, const char *profile, UINT flags, ID3DBlob *code)
{
    std::wstring dir = g_cfg.port_dir + L"\\cache";
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW(cache_dir().c_str(), nullptr);
    std::wstring tmp = path + L".tmp";
    FILE *f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    u32(kMagic);
    u32(kVersion);
    u32(flags);
    u32((uint32_t)strlen(profile));
    fwrite(profile, 1, strlen(profile), f);
    u32((uint32_t)hlsl.size());
    fwrite(hlsl.data(), 1, hlsl.size(), f);
    u32(code ? (uint32_t)code->GetBufferSize() : 0);
    if (code) fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), f);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return;
    }
    write_epoch(path, kShaderEpoch);
}

// Shader objects made at start from the cache (prewarm_shaders), keyed by their bytecode, until a draw asks for them.
struct Warm {
    IDirect3DDevice9 *dev;
    bool vertex;
    std::vector<uint8_t> code;
    IUnknown *obj;
};
std::unordered_multimap<uint64_t, Warm> g_warm;

uint64_t code_key(const void *p, size_t n) { return fnv1a(0xCBF29CE484222325ull, p, n); }

// A prewarmed object for this bytecode (its reference passes to the caller), or null.
IUnknown *take_warm(IDirect3DDevice9 *d, bool vertex, ID3DBlob *code)
{
    auto range = g_warm.equal_range(code_key(code->GetBufferPointer(), code->GetBufferSize()));
    for (auto it = range.first; it != range.second; ++it) {
        const Warm &w = it->second;
        if (w.dev == d && w.vertex == vertex && w.code.size() == code->GetBufferSize() &&
            !memcmp(w.code.data(), code->GetBufferPointer(), w.code.size())) {
            IUnknown *obj = w.obj;
            g_warm.erase(it);
            return obj;
        }
    }
    return nullptr;
}

} // namespace

HRESULT cached_compile(const std::string &hlsl, const char *name, const char *profile, UINT flags, ID3DBlob **code,
                       ID3DBlob **errors)
{
    *code = nullptr;
    if (errors) *errors = nullptr;
    std::lock_guard<std::mutex> g(g_lock);
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    std::wstring path = entry_path(hlsl, profile, flags);
    int cached = read_entry(path, hlsl, profile, flags, code);
    HRESULT hr = cached > 0 ? S_OK : E_FAIL;
    if (!cached) {
        perf::DetScope ds(perf::kDetD3DCompile);
        hr = D3DCompile(hlsl.data(), hlsl.size(), name, nullptr, nullptr, "main", profile, flags, 0, code, errors);
        write_entry(path, hlsl, profile, flags, SUCCEEDED(hr) ? *code : nullptr);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    perf::add_shader_build((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, cached != 0);
    return hr;
}

void prewarm_shaders(IDirect3DDevice9 *d)
{
    std::lock_guard<std::mutex> g(g_lock);
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((cache_dir() + L"\\*.bin").c_str(), &fd);
    unsigned nv = 0, np = 0, bad = 0, failed = 0, old = 0, migrated = 0, seeded = 0;
    std::unordered_set<std::wstring> scanned; // the player's entries made this time (the seed skips those)
    // An entry kept in g_mem made into its shader object, handed out later by take_warm
    auto warm = [&](const Entry &kept) {
        if (kept.code.empty()) return; // a source this profile does not compile
        bool vertex = kept.profile.compare(0, 3, "vs_") == 0;
        IUnknown *obj = nullptr;
        HRESULT hr = vertex ? d->CreateVertexShader((const DWORD *)kept.code.data(), (IDirect3DVertexShader9 **)&obj)
                            : d->CreatePixelShader((const DWORD *)kept.code.data(), (IDirect3DPixelShader9 **)&obj);
        if (FAILED(hr)) {
            failed++;
            return;
        }
        (vertex ? nv : np)++;
        g_warm.emplace(code_key(kept.code.data(), kept.code.size()), Warm{d, vertex, kept.code, obj});
    };
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring path = cache_dir() + L"\\" + fd.cFileName;
            const uint32_t epoch = read_epoch(path);
            if (epoch && epoch != kShaderEpoch) { // another generator: keep it, but do not make it now
                old++;
                continue;
            }
            if (epoch && !cache_entry_current(path)) { // .bin rewritten after its .gen (a downgrade): not this build's
                old++;
                continue;
            }
            if (!epoch) { // before the sidecar existed: migrate it once, so this build's first start stays warm
                write_epoch(path, kShaderEpoch);
                migrated++;
            }
            Entry e;
            if (!parse_file(path, &e)) { // another format, or a truncated file: nothing can use it
                DeleteFileW(path.c_str());
                DeleteFileW(epoch_path(path).c_str());
                g_mem.erase(path);
                bad++;
                continue;
            }
            warm(g_mem[path] = e);
            scanned.insert(path);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // The seed shipped beside the program (shader_seed.bin, tools/make_shader_seed.py): entries recorded in test runs
    // over the chapters, so a first start has them too -- a new player otherwise compiled every shader the first time
    // an area showed it, on the render thread (2-40 ms each, D3DCompile; ngbrecomp ships one for the same reason,
    // packaging/d3d-seed.bin). Read-only: it stays out of cache\shaders, and an entry the player's cache holds (the
    // same source, so the same bytecode) is not made twice. Records: "NGBSEED1", count, then each entry's file bytes
    // with their length. The game's own vertex programs are not in it (their HLSL is the game's code translated).
    {
        bool ok;
        std::vector<uint8_t> s = read_all(g_cfg.port_dir + L"\\shader_seed.bin", &ok);
        unsigned in_seed = 0, seed_bad = 0;
        if (ok && s.size() >= 12 && !memcmp(s.data(), "NGBSEED1", 8)) {
            uint32_t count;
            memcpy(&count, &s[8], 4);
            size_t at = 12;
            for (uint32_t i = 0; i < count && at + 4 <= s.size(); i++) {
                uint32_t len;
                memcpy(&len, &s[at], 4);
                at += 4;
                if (len > s.size() - at) break;
                Entry e;
                bool good = parse_bytes(&s[at], len, &e);
                at += len;
                in_seed++;
                if (!good) {
                    seed_bad++;
                    continue;
                }
                std::wstring path = entry_path(e.hlsl, e.profile.c_str(), e.flags);
                if (scanned.count(path)) continue; // the player's cache has it (made above)
                warm(g_mem[path] = std::move(e));
                seeded++;
            }
        } else if (ok) {
            seed_bad = 1;
        }
        if (ok) logf("[shader cache] seed: %u entries, %u not in the player's cache added%s", in_seed, seeded,
                     seed_bad ? " (part of shader_seed.bin unreadable)" : "");
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    logf("[shader cache] read %zu entries, made %u vertex and %u pixel shaders from %s and the seed in %.0f ms; %u old-generator kept, "
         "%u legacy marked, %u unreadable deleted%s",
         g_mem.size(), nv, np, narrow(cache_dir()).c_str(),
         (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, old, migrated, bad,
         failed ? " (the device refused some)" : "");
}

void warm_drop_all()
{
    std::lock_guard<std::mutex> g(g_lock);
    for (auto &kv : g_warm) kv.second.obj->Release();
    g_warm.clear();
}

HRESULT create_vertex_shader(IDirect3DDevice9 *d, ID3DBlob *code, IDirect3DVertexShader9 **out)
{
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (IUnknown *w = take_warm(d, true, code)) {
            *out = (IDirect3DVertexShader9 *)w;
            return S_OK;
        }
    }
    perf::DetScope ds(perf::kDetShaderCreate);
    return d->CreateVertexShader((const DWORD *)code->GetBufferPointer(), out);
}

HRESULT create_pixel_shader(IDirect3DDevice9 *d, ID3DBlob *code, IDirect3DPixelShader9 **out)
{
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (IUnknown *w = take_warm(d, false, code)) {
            *out = (IDirect3DPixelShader9 *)w;
            return S_OK;
        }
    }
    perf::DetScope ds(perf::kDetShaderCreate);
    return d->CreatePixelShader((const DWORD *)code->GetBufferPointer(), out);
}

} // namespace xd3d
