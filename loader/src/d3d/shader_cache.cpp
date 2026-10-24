#include "d3d/shader_cache.h"

#include "common.h"
#include "perf.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
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

bool parse_file(const std::wstring &path, Entry *e)
{
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    fclose(f);
    size_t at = 0;
    auto u32 = [&](uint32_t *v) {
        if (at + 4 > d.size()) return false;
        memcpy(v, &d[at], 4);
        at += 4;
        return true;
    };
    auto field = [&](std::string *s) {
        uint32_t n;
        if (!u32(&n) || at + n > d.size()) return false;
        s->assign((const char *)&d[at], n);
        at += n;
        return true;
    };
    uint32_t magic, version, len;
    if (!u32(&magic) || magic != kMagic || !u32(&version) || version != kVersion || !u32(&e->flags)) return false;
    if (!field(&e->profile) || !field(&e->hlsl) || !u32(&len) || at + len != d.size()) return false;
    e->code.assign(d.begin() + at, d.end());
    return true;
}

// The entries read at start (prewarm_shaders), by file path: a lookup in play then reads no file. Opening one took about
// 0.9 ms (19 entries, 16 ms in the opening cutscene's frame on the RTX 3060, perf 2026-10-22_07-24-25).
std::unordered_map<std::wstring, Entry> g_mem;

// 1: the bytecode is in *code; -1: known not to compile; 0: not in the cache
int read_entry(const std::wstring &path, const std::string &hlsl, const char *profile, UINT flags, ID3DBlob **code)
{
    Entry file;
    const Entry *e = nullptr;
    auto it = g_mem.find(path);
    if (it != g_mem.end()) e = &it->second;
    else if (parse_file(path, &file)) e = &file;
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
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
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
    unsigned nv = 0, np = 0, bad = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring path = cache_dir() + L"\\" + fd.cFileName;
            Entry e;
            if (!parse_file(path, &e)) {
                bad++;
                continue;
            }
            Entry &kept = g_mem[path] = e;
            if (e.code.empty()) continue; // a source that does not compile
            bool vertex = e.profile.compare(0, 3, "vs_") == 0;
            IUnknown *obj = nullptr;
            HRESULT hr = vertex ? d->CreateVertexShader((const DWORD *)e.code.data(), (IDirect3DVertexShader9 **)&obj)
                                : d->CreatePixelShader((const DWORD *)e.code.data(), (IDirect3DPixelShader9 **)&obj);
            if (FAILED(hr)) {
                bad++;
                continue;
            }
            (vertex ? nv : np)++;
            uint64_t key = code_key(e.code.data(), e.code.size());
            g_warm.emplace(key, Warm{d, vertex, kept.code, obj});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    logf("[shader cache] read %zu entries, made %u vertex and %u pixel shaders from %s in %.0f ms%s", g_mem.size(), nv, np,
         narrow(cache_dir()).c_str(),
         (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, bad ? " (some entries unreadable)" : "");
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
    return d->CreatePixelShader((const DWORD *)code->GetBufferPointer(), out);
}

} // namespace xd3d
