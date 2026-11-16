// The game's CPU-made ripple textures, kept: computed once, then copied.
//
// Ninja Gaiden Black's FUN_000C64D0 (x2.1; the same code in every build here, found by its bytes) makes a texture of
// w x h two-byte texels (a V8U8 displacement map: the water's ripples) on the CPU: per texel three x87 square roots, a
// sine and two arctangents, then it swizzles the buffer into the texture it made. A cutscene's or an area's first
// frame calls it, 41-48% of that frame's samples (perf recorder's spike section, 33-44 ms frames, 2026-11-15); the
// console spends the same there.
//
// Its texels depend only on its arguments (width, height and three integers), the constants it reads (hashed into
// the key here, from the build's own copy) and the x87 control word (in the key too): no other state. So the loop is
// hooked at its start and end: a key seen before has its texels copied into the game's buffer and the loop skipped;
// a new one runs the game's own loop and its result is kept (cache\cpu_textures.bin, written off the game's thread).
// Texture creation, locking, swizzling and freeing stay the game's. Every texel is one the game's code computed.
// --cpu-tex-verify runs the loop anyway on a hit and compares it with the kept texels, byte for byte (the log says).
// --no-cpu-tex leaves the code alone.
#include "common.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// Hook A: the loop's start, "jbe <after the loop>" (0F 86 rel32), right after the setup; hook B: the loop's end,
// "mov edx, [esp+0x50]; mov edi, [edx]". Found by the bytes before each (the setup's last instructions; no address
// in them); their offsets from the function's start are the same in every build.
const uint8_t kSigA[] = {0x33, 0xD2, 0x85, 0xED, 0x89, 0x54, 0x24, 0x24, 0xD9, 0x5C, 0x24, 0x1C, 0x0F, 0x86};
constexpr uint32_t kHookAInSig = 12;      // the jbe
constexpr uint32_t kAToB = 0xC6830 - 0xC65B0;
constexpr uint32_t kAToLoop = 0xC65B6 - 0xC65B0; // the loop's first instruction (after the jbe)
constexpr uint32_t kFuncToA = 0xC65B0 - 0xC64D0;
const uint8_t kHookB[] = {0x8B, 0x54, 0x24, 0x50, 0x8B, 0x3A};
// The instructions that read the function's float constants (offset from the function's start, opcode bytes, where
// the 32-bit address is in it)
struct ConstRef {
    uint32_t off;
    uint8_t op0, op1;
    uint32_t addr_at;
};
const ConstRef kConsts[] = {{0xC8, 0xD8, 0x05, 2},  {0xCE, 0xD8, 0x3D, 2},  {0x1D5, 0xD8, 0x0D, 2}, {0x1E3, 0xD8, 0x0D, 2},
                            {0x29D, 0xD8, 0x0D, 2}, {0x2A3, 0xD8, 0x0D, 2}, {0x2BD, 0xD8, 0x0D, 2}, {0x2CD, 0xD9, 0x05, 2},
                            {0x2DD, 0xF3, 0x0F, 4}};

uint32_t g_ret_loop, g_ret_after, g_ret_b; // where the stubs go on
uint64_t g_consts;                         // the constants' hash (part of every key)

struct Key {
    uint64_t consts;
    uint32_t w, h, a4, a5, a6, fcw;
    bool operator<(const Key &o) const { return memcmp(this, &o, sizeof *this) < 0; }
};
static_assert(sizeof(Key) == 32, "compared as bytes");

std::mutex g_lock;
std::map<Key, std::vector<uint8_t>> g_kept;
bool g_loaded;
// the call in progress (one game thread makes these)
bool g_pending, g_verify_pending;
Key g_key;
LARGE_INTEGER g_t0;
uint32_t g_hits, g_made, g_verified, g_mismatched;

std::wstring file_path() { return g_cfg.port_dir + L"\\cache\\cpu_textures.bin"; }
// The package's seed beside the program (tools/make_cputex_seed.py from test runs' cache files): a new player's first
// start then makes none of them either (it was the one fixed spike left on a first start: 10 textures, ~40 ms in one
// frame, 4 of 4 runs from an empty cache, RTX 3060, 2026-11-16). Keys carry the constants' hash of the build that made
// them, so another build's entries are never used.
std::wstring seed_path() { return g_cfg.port_dir + L"\\cpu_textures.bin"; }
std::set<Key> g_seed_keys; // in the seed: not written to the player's file again

size_t read_file(const std::wstring &path, bool seed)
{
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return 0;
    char magic[8];
    uint32_t count = 0;
    size_t got = 0;
    if (fread(magic, 8, 1, f) == 1 && !memcmp(magic, "NGBCPUT1", 8) && fread(&count, 4, 1, f) == 1) {
        for (uint32_t i = 0; i < count && i < 4096; i++) {
            Key k;
            uint32_t n = 0;
            if (fread(&k, sizeof k, 1, f) != 1 || fread(&n, 4, 1, f) != 1 || n != k.w * k.h * 2 || n > (8u << 20)) break;
            std::vector<uint8_t> v(n);
            if (fread(v.data(), 1, n, f) != n) break;
            if (seed) g_seed_keys.insert(k);
            if (k.consts == g_consts) got++; // this build's
            g_kept[k] = std::move(v);
        }
    }
    fclose(f);
    return got;
}

void load()
{
    if (g_loaded) return;
    g_loaded = true;
    size_t seed = read_file(seed_path(), true), learned = read_file(file_path(), false);
    logf("[cputex] kept ripple textures for this build: %zu from the seed, %zu learned", seed, learned);
}

void save_async()
{
    std::map<Key, std::vector<uint8_t>> copy;
    for (auto &kv : g_kept)
        if (!g_seed_keys.count(kv.first)) copy.insert(kv);
    std::wstring path = file_path();
    std::thread([copy = std::move(copy), path] {
        CreateDirectoryW((g_cfg.port_dir + L"\\cache").c_str(), nullptr);
        std::wstring tmp = path + L".tmp";
        bool ok = false;
        if (FILE *f = _wfopen(tmp.c_str(), L"wb")) {
            uint32_t count = (uint32_t)copy.size();
            ok = fwrite("NGBCPUT1", 8, 1, f) == 1 && fwrite(&count, 4, 1, f) == 1;
            for (auto &kv : copy) {
                uint32_t n = (uint32_t)kv.second.size();
                ok = ok && fwrite(&kv.first, sizeof kv.first, 1, f) == 1 && fwrite(&n, 4, 1, f) == 1 &&
                     fwrite(kv.second.data(), 1, n, f) == n;
            }
            ok = fclose(f) == 0 && ok;
        }
        if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
    }).detach();
}

// gesp: the game's esp at the hook. Its frame: [0x44] the texel buffer, [0x54] width, [0x58] height, [0x5C], [0x60],
// [0x64] the other arguments.
const uint32_t &at(const uint32_t *gesp, uint32_t off) { return gesp[off / 4]; }

// 1: the buffer is filled, skip the loop
int __cdecl cputex_begin(const uint32_t *gesp)
{
    g_pending = g_verify_pending = false;
    uint32_t w = at(gesp, 0x54), h = at(gesp, 0x58);
    if (!w || !h || (uint64_t)w * h * 2 > (8u << 20)) return 0; // (the game's own way for an empty one)
    uint16_t fcw;
    __asm fnstcw fcw;
    Key k = {g_consts, w, h, at(gesp, 0x5C), at(gesp, 0x60), at(gesp, 0x64), fcw};
    std::lock_guard<std::mutex> g(g_lock);
    load();
    auto it = g_kept.find(k);
    if (it != g_kept.end()) {
        if (g_cfg.cpu_tex_verify) {
            g_key = k;
            g_verify_pending = true;
            return 0; // computed anyway, compared at the end
        }
        memcpy((void *)(uintptr_t)at(gesp, 0x44), it->second.data(), it->second.size());
        g_hits++;
        return 1;
    }
    g_key = k;
    g_pending = true;
    QueryPerformanceCounter(&g_t0);
    return 0;
}

void __cdecl cputex_end(const uint32_t *gesp)
{
    if (!g_pending && !g_verify_pending) return;
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const uint8_t *buf = (const uint8_t *)(uintptr_t)at(gesp, 0x44);
    size_t n = (size_t)g_key.w * g_key.h * 2;
    std::lock_guard<std::mutex> g(g_lock);
    if (g_verify_pending) {
        g_verify_pending = false;
        auto it = g_kept.find(g_key);
        bool same = it != g_kept.end() && it->second.size() == n && !memcmp(it->second.data(), buf, n);
        (same ? g_verified : g_mismatched)++;
        logf("[cputex] verify %ux%u (%u, %u, %u): the kept texels %s the game's (%u same, %u differing so far)", g_key.w, g_key.h, g_key.a4,
             g_key.a5, g_key.a6, same ? "ARE" : "are NOT", g_verified, g_mismatched);
        return;
    }
    g_pending = false;
    g_kept[g_key].assign(buf, buf + n);
    g_made++;
    if (g_made <= 40)
        logf("[cputex] ripple texture %ux%u (%u, %u, %u) made by the game in %.1f ms, kept (%u kept this run, %u copied)", g_key.w, g_key.h,
             g_key.a4, g_key.a5, g_key.a6, (t1.QuadPart - g_t0.QuadPart) * 1000.0 / f.QuadPart, g_made, g_hits);
    save_async();
}

// The stubs: every general register, the flags and xmm0-7 kept (the game holds values in xmm registers across calls)
__declspec(naked) void stub_a()
{
    __asm {
        pushad
        pushfd
        sub esp, 128
        movups [esp], xmm0
        movups [esp + 16], xmm1
        movups [esp + 32], xmm2
        movups [esp + 48], xmm3
        movups [esp + 64], xmm4
        movups [esp + 80], xmm5
        movups [esp + 96], xmm6
        movups [esp + 112], xmm7
        lea eax, [esp + 128 + 4 + 32]
        push eax
        call cputex_begin
        add esp, 4
        mov [esp + 128 + 4 + 28], eax // into the saved eax (the loop sets eax itself)
        movups xmm0, [esp]
        movups xmm1, [esp + 16]
        movups xmm2, [esp + 32]
        movups xmm3, [esp + 48]
        movups xmm4, [esp + 64]
        movups xmm5, [esp + 80]
        movups xmm6, [esp + 96]
        movups xmm7, [esp + 112]
        add esp, 128
        popfd
        popad
        test eax, eax
        jnz kept
        test ebp, ebp // the displaced "jbe <after the loop>" on the height (its test came before the hook)
        jbe empty
        jmp dword ptr [g_ret_loop]
    empty:
    kept:
        jmp dword ptr [g_ret_after]
    }
}

__declspec(naked) void stub_b()
{
    __asm {
        pushad
        pushfd
        sub esp, 128
        movups [esp], xmm0
        movups [esp + 16], xmm1
        movups [esp + 32], xmm2
        movups [esp + 48], xmm3
        movups [esp + 64], xmm4
        movups [esp + 80], xmm5
        movups [esp + 96], xmm6
        movups [esp + 112], xmm7
        lea eax, [esp + 128 + 4 + 32]
        push eax
        call cputex_end
        add esp, 4
        movups xmm0, [esp]
        movups xmm1, [esp + 16]
        movups xmm2, [esp + 32]
        movups xmm3, [esp + 48]
        movups xmm4, [esp + 64]
        movups xmm5, [esp + 80]
        movups xmm6, [esp + 96]
        movups xmm7, [esp + 112]
        add esp, 128
        popfd
        popad
        mov edx, [esp + 0x50] // the two displaced instructions
        mov edi, [edx]
        jmp dword ptr [g_ret_b]
    }
}

void write_jmp(uint32_t at, void *to, uint32_t len)
{
    auto *p = (uint8_t *)(uintptr_t)at;
    DWORD old;
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
    for (uint32_t i = 5; i < len; i++) p[i] = 0x90;
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
}

} // namespace

void install_cpu_textures(uint32_t text_va, uint32_t text_size)
{
    if (!g_cfg.cpu_tex || !text_va) return;
    const uint8_t *b = (const uint8_t *)(uintptr_t)text_va;
    uint32_t found = 0, hits = 0;
    for (uint32_t i = 0; i + sizeof kSigA <= text_size; i++)
        if (b[i] == kSigA[0] && !memcmp(b + i, kSigA, sizeof kSigA)) found = text_va + i, hits++;
    if (hits != 1) {
        if (hits) logf("[cputex] the ripple texture loop found %u times: left alone", hits);
        return;
    }
    uint32_t a = found + kHookAInSig, bsite = a + kAToB, func = a - kFuncToA;
    if (memcmp((const void *)(uintptr_t)bsite, kHookB, sizeof kHookB) != 0) {
        logf("[cputex] the ripple texture loop's end is not as expected: left alone");
        return;
    }
    // the constants' values (hashed into every key): from the addresses this build's code reads them at
    uint64_t h = 0xCBF29CE484222325ull;
    for (const ConstRef &c : kConsts) {
        const uint8_t *ins = (const uint8_t *)(uintptr_t)(func + c.off);
        if (ins[0] != c.op0 || ins[1] != c.op1) {
            logf("[cputex] the ripple texture code at 0x%08x is not as expected: left alone", func + c.off);
            return;
        }
        uint32_t addr;
        memcpy(&addr, ins + c.addr_at, 4);
        const uint8_t *v = (const uint8_t *)(uintptr_t)addr;
        for (int k = 0; k < 4; k++) h = (h ^ v[k]) * 0x100000001B3ull;
    }
    g_consts = h;
    int32_t rel;
    memcpy(&rel, (const void *)(uintptr_t)(a + 2), 4);
    g_ret_after = a + 6 + rel; // the jbe's target: after the loop (= hook B)
    g_ret_loop = a + kAToLoop;
    g_ret_b = bsite + sizeof kHookB;
    if (g_ret_after != bsite) {
        logf("[cputex] the ripple texture loop's exit is not hook B: left alone");
        return;
    }
    write_jmp(a, (void *)&stub_a, 6);
    write_jmp(bsite, (void *)&stub_b, 6);
    logf("[cputex] ripple texture loop at 0x%08x hooked: made once, then copied (constants %016llx)%s", func,
         (unsigned long long)g_consts, g_cfg.cpu_tex_verify ? "; verifying every copy" : "");
}
