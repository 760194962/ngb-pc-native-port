#include "d3d/dispatch.h"

#include "dashboard.h"

#include "build_profile.h"
#include "d3d/draw.h"
#include "d3d/state.h"
#include "d3d/xd3d.h"
#include "game_load.h"
#include "hooks.h"
#include "image.h"
#include "net/online.h"
#include "xbe.h"
#include "xthread.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

bool g_trace_d3d = false;
xd3d::DataLayout xd3d::g_layout = xd3d::kLayoutBlack;

namespace {

struct LibFunc {
    uint32_t addr, size;
    const char *name;
};
// Every function in the XBE sections whose library is replaced (D3D, DSOUND, XPP): each one is either
// replaced by HLE or stops the run on first call, so library code never touches GPU/APU hardware.
// One set of tables per supported build (build_profile.h); the loader picks the set by the XBE's hash.
const LibFunc kD3D[] = {
#include "d3d_functions.inc"
};
const LibFunc kDSound[] = {
#include "dsound_functions.inc"
};
const LibFunc kXpp[] = {
#include "xpp_functions.inc"
};
const LibFunc kNgbD3D[] = {
#include "ngb/d3d_functions.inc"
};
const LibFunc kNgbDSound[] = {
#include "ngb/dsound_functions.inc"
};
const LibFunc kNgbXpp[] = {
#include "ngb/xpp_functions.inc"
};
struct AuxFuncs {
    const char *xbe;
    const LibFunc *funcs;
    size_t count;
};
#include "auxtab/functions.inc"
struct AuxLayout {
    const char *xbe;
    xd3d::DataLayout layout;
};
const AuxLayout kAuxLayouts[] = {
#include "auxtab/layout.inc"
};

std::vector<LibFunc> g_funcs; // the build's D3D + DSOUND + XPP tables, sorted by address
std::vector<bool> g_int3_hook;
std::atomic<bool> g_unimplemented_hit{false};

// Executable memory for the generated thunks. A full block is left in place (its thunks are in use) and a new one
// taken; the first version wrote past its single 512 KB block once that was used up.
constexpr size_t kCodeBlock = 0x80000;
uint8_t *g_code;
size_t g_code_used, g_code_size;

uint8_t *code_alloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (!g_code || g_code_used + n > g_code_size) {
        size_t size = n > kCodeBlock ? n : kCodeBlock;
        uint8_t *block = (uint8_t *)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!block) {
            logf("hooks: no executable memory for %zu bytes of thunks", n);
            ExitProcess(3);
        }
        if (g_code) logf("hooks: thunk block full (%zu bytes used), a new %zu-byte block", g_code_used, size);
        g_code = block;
        g_code_used = 0;
        g_code_size = size;
    }
    uint8_t *p = g_code + g_code_used;
    g_code_used += n;
    return p;
}

void write_jmp(uint8_t *from, const void *to)
{
    from[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)to - (uintptr_t)(from + 5));
    memcpy(from + 1, &rel, 4);
}

} // namespace

// Unimplemented library function reached: log it with the arguments and end the run (functions are
// implemented one at a time as the game reaches them).
extern "C" void __stdcall blackrt_lib_unimplemented(uint32_t index, const uint32_t *frame, uint32_t eax, uint32_t ecx,
                                                    uint32_t edx)
{
    if (g_unimplemented_hit.exchange(true)) Sleep(INFINITE);
    const LibFunc &f = g_funcs[index];
    XThread *t = xthread_current_or_null();
    const XbeSection *sec = image_xbe() ? image_xbe()->section_containing(f.addr) : nullptr;
    const char *lib = sec ? sec->name.c_str() : "?";
    logf("==== UNIMPLEMENTED %s: %s (0x%08x), thread '%s'", lib, f.name, f.addr, t ? t->name : "host");
    logf("    called from %08x %s", frame[0], sym_name(frame[0]).c_str());
    logf("    eax=%08x ecx=%08x edx=%08x  stack args: %08x %08x %08x %08x %08x %08x", eax, ecx, edx, frame[1], frame[2], frame[3],
         frame[4], frame[5], frame[6]);
    log_stack_trace(frame);
    xd3d::screenshot_now("stop");
    if (dashboard_back_to_game("a library function not done here")) Sleep(INFINITE); // the Xbox Live dashboard: the game again
    request_exit(0, "reached an unimplemented library function");
    Sleep(INFINITE);
}

namespace {

__declspec(naked) void unimplemented_common()
{
    __asm {
        push ebp
        mov ebp, esp
        push edx
        push ecx
        push eax
        lea eax, [ebp+8]
        push eax
        push dword ptr [ebp+4]
        call blackrt_lib_unimplemented
        int 3
    }
}

// Around a D3D library call in its thunk (game_load.cpp): the registers the thunk still needs are kept.
__declspec(naked) void span_enter_saved()
{
    __asm {
        push eax
        push ecx
        push edx
        call game_load_span_enter
        pop edx
        pop ecx
        pop eax
        ret
    }
}

__declspec(naked) void span_leave_saved()
{
    __asm {
        push eax
        push ecx
        push edx
        call game_load_span_leave
        pop edx
        pop ecx
        pop eax
        ret
    }
}

void *const g_span_enter = (void *)&span_enter_saved, *const g_span_leave = (void *)&span_leave_saved;

// call dword ptr [p]
int emit_call_ptr(uint8_t *buf, void *const *p)
{
    buf[0] = 0xFF;
    buf[1] = 0x15;
    uint32_t a = (uint32_t)(uintptr_t)p;
    memcpy(buf + 2, &a, 4);
    return 6;
}

int reg_code(const std::string &r)
{
    static const char *names[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    for (int i = 0; i < 8; i++)
        if (r == names[i]) return i;
    return -1;
}

// Generate the thunk described in dispatch.h. Returns nullptr on a bad spec. `span`: a D3D library call, timed for the
// engine's load meter (game_load.cpp).
void *make_thunk(const xd3d::Hle &h, bool span)
{
    std::vector<std::string> params;
    {
        std::string p = h.params, tok;
        for (char c : p + " ") {
            if (c == ' ' || c == ',') {
                if (!tok.empty()) params.push_back(tok);
                tok.clear();
            } else {
                tok += c;
            }
        }
    }
    std::vector<int> stack_index(params.size(), -1);
    int stack_args = 0;
    for (size_t i = 0; i < params.size(); i++) {
        if (params[i] == "s") stack_index[i] = stack_args++;
        else if (reg_code(params[i]) < 0) return nullptr;
    }
    uint8_t buf[384];
    int n = 0;
    int saved = 0;
    // XMM0-7 kept across the call: the game is built with the D3D library's link-time code generation, which knows
    // the library's own functions leave the SSE registers alone, so it keeps values in them across those calls; this
    // C++ does not (a 16-byte memcpy alone is a movups through xmm0). Ninja Gaiden Black's FUN_0019e150 builds a
    // vertex constant from xmm0/xmm2 after three SetVertexShaderConstant1 calls: clobbered, the cloth's light floor
    // c40 came out (2,1,2,2) and the streamers on the chapter-1 fences drew white instead of dark grey (the user,
    // 2026-11-09; run/play: [c40probe], watch on 0x3D8EF8).
    buf[n++] = 0x81; buf[n++] = 0xEC; // sub esp, 128
    { uint32_t k = 128; memcpy(buf + n, &k, 4); n += 4; }
    for (int x = 0; x < 8; x++) { // movups [esp + 16x], xmmX
        buf[n++] = 0x0F; buf[n++] = 0x11; buf[n++] = (uint8_t)(0x44 | (x << 3)); buf[n++] = 0x24; buf[n++] = (uint8_t)(16 * x);
    }
    const int xmm_area = 128;
    if (span) n += emit_call_ptr(buf + n, &g_span_enter);
    buf[n++] = 0x51; // push ecx
    buf[n++] = 0x52; // push edx
    saved = 2;
    if (!h.returns) {
        buf[n++] = 0x50; // push eax
        saved = 3;
    }
    int pushed = 0;
    for (int i = (int)params.size() - 1; i >= 0; i--) {
        if (stack_index[i] >= 0) {
            int off = 4 + 4 * stack_index[i] + 4 * (saved + pushed) + xmm_area;
            if (off < 128) {
                buf[n++] = 0xFF; buf[n++] = 0x74; buf[n++] = 0x24; buf[n++] = (uint8_t)off; // push [esp+off8]
            } else {
                buf[n++] = 0xFF; buf[n++] = 0xB4; buf[n++] = 0x24;
                memcpy(buf + n, &off, 4);
                n += 4;
            }
        } else {
            buf[n++] = (uint8_t)(0x50 + reg_code(params[i])); // push reg
        }
        pushed++;
    }
    buf[n++] = 0xB8; // mov eax, impl
    uint32_t impl = (uint32_t)(uintptr_t)h.impl;
    memcpy(buf + n, &impl, 4);
    n += 4;
    buf[n++] = 0xFF; buf[n++] = 0xD0; // call eax (impl is __stdcall: pops its parameters)
    if (span) n += emit_call_ptr(buf + n, &g_span_leave);
    if (!h.returns) buf[n++] = 0x58; // pop eax
    buf[n++] = 0x5A; // pop edx
    buf[n++] = 0x59; // pop ecx
    for (int x = 0; x < 8; x++) { // movups xmmX, [esp + 16x]
        buf[n++] = 0x0F; buf[n++] = 0x10; buf[n++] = (uint8_t)(0x44 | (x << 3)); buf[n++] = 0x24; buf[n++] = (uint8_t)(16 * x);
    }
    buf[n++] = 0x81; buf[n++] = 0xC4; // add esp, 128
    { uint32_t k = 128; memcpy(buf + n, &k, 4); n += 4; }
    if (stack_args) {
        buf[n++] = 0xC2; // ret imm16
        uint16_t r = (uint16_t)(stack_args * 4);
        memcpy(buf + n, &r, 2);
        n += 2;
    } else {
        buf[n++] = 0xC3;
    }
    uint8_t *p = code_alloc(n);
    memcpy(p, buf, n);
    return p;
}

LONG CALLBACK int3_handler(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t a = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    for (size_t i = 0; i < g_funcs.size(); i++) {
        if (g_int3_hook[i] && g_funcs[i].addr == a) {
            CONTEXT *c = ep->ContextRecord;
            blackrt_lib_unimplemented((uint32_t)i, (const uint32_t *)(uintptr_t)c->Esp, c->Eax, c->Ecx, c->Edx);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

namespace xd3d {

bool install_d3d_hooks(const BuildProfile &profile)
{
    g_layout = profile.black ? kLayoutBlack : kLayoutNgb;
    if (profile.aux)
        for (const AuxLayout &l : kAuxLayouts)
            if (!strcmp(l.xbe, profile.aux)) g_layout = l.layout;
    // Addresses a build does not fix (the bundled programs keep these in their device object): the loader's own memory.
    static float s_vs_constants[192][4];
    static uint32_t s_push_segment;
    if (!g_layout.vs_constants) g_layout.vs_constants = (uint32_t)(uintptr_t)s_vs_constants;
    if (!g_layout.push_segment) g_layout.push_segment = (uint32_t)(uintptr_t)&s_push_segment;
    g_st.vs_constants = (float (*)[4])(uintptr_t)g_layout.vs_constants;
    if (profile.aux) {
        g_funcs.clear();
        for (const AuxFuncs &a : kAuxFuncs)
            if (!strcmp(a.xbe, profile.aux)) g_funcs.assign(a.funcs, a.funcs + a.count);
    } else if (profile.black) {
        g_funcs.assign(std::begin(kD3D), std::end(kD3D));
        g_funcs.insert(g_funcs.end(), std::begin(kDSound), std::end(kDSound));
        g_funcs.insert(g_funcs.end(), std::begin(kXpp), std::end(kXpp));
    } else {
        g_funcs.assign(std::begin(kNgbD3D), std::end(kNgbD3D));
        g_funcs.insert(g_funcs.end(), std::begin(kNgbDSound), std::end(kNgbDSound));
        g_funcs.insert(g_funcs.end(), std::begin(kNgbXpp), std::end(kNgbXpp));
    }
    std::sort(g_funcs.begin(), g_funcs.end(), [](const LibFunc &a, const LibFunc &b) { return a.addr < b.addr; });
    g_int3_hook.assign(g_funcs.size(), false);
    // Ninja Gaiden Black's load meter: the calls into its D3D library's code that take the time are timed
    // (game_load.cpp): draws, push buffers, waits, presents, texture binds and shader loads. Not the state setters the
    // game makes ten thousand of a frame (timing every call cost 0.3-0.5 ms a frame, r652/r653), nor DSound's, XPP's
    // or XNet's (their replacements sit in the same tables).
    game_load::install(profile.load_meter);
    uint32_t d3d_lo = ~0u, d3d_hi = 0;
    for (const LibFunc &f : kNgbD3D) {
        d3d_lo = std::min(d3d_lo, f.addr);
        d3d_hi = std::max(d3d_hi, f.addr + f.size);
    }
    static const char *const kHeavy[] = {"Draw", "RunPushBuffer", "Clear", "BlockOn", "BlockUntil", "Swap", "Present", "Lock",
                                         "SetRenderTarget", "CopyRects", "KickOff", "SetTexture", "EndVisibilityTest",
                                         "CreatePixelShader", "CreateVertexShader", "LoadVertexShader", "PersistDisplay"};
    auto timed = [&](const Hle &h) {
        if (!game_load::active() || h.addr < d3d_lo || h.addr >= d3d_hi) return false;
        for (const char *k : kHeavy)
            if (strstr(h.name, k) && !strstr(h.name, "SetTextureState")) return true;
        return false;
    };

    // The HLE tables hold BLACK's addresses; for another build nothing matches and every library function stops on
    // its first call, which is the state the port is brought up from (one function at a time, from the log).
    std::vector<Hle> all;
    if (profile.aux) {
        for (auto table : {hle_device_table_aux, hle_resource_table_aux, hle_draw_table_aux, hle_dsound_table_aux, hle_input_table_aux,
                           hle_ngb_extra_table_aux}) {
            size_t n = 0;
            const Hle *t = table(profile.aux, &n);
            all.insert(all.end(), t, t + n);
        }
        logf("hooks: %s: %zu HLE entries; every other function in the hooked sections stops on first call", profile.name, all.size());
    } else if (profile.black) {
        for (auto table : {hle_device_table, hle_resource_table, hle_draw_table, hle_dsound_table, hle_input_table}) {
            size_t n = 0;
            const Hle *t = table(&n);
            all.insert(all.end(), t, t + n);
        }
    } else {
        for (auto table : {hle_device_table_ngb, hle_resource_table_ngb, hle_draw_table_ngb, hle_dsound_table_ngb, hle_input_table_ngb,
                           hle_ngb_extra_table}) {
            size_t n = 0;
            const Hle *t = table(&n);
            all.insert(all.end(), t, t + n);
        }
        logf("hooks: %s: %zu HLE entries; every other function in the hooked sections stops on first call", profile.name, all.size());
    }
    // An entry at address 0 is not in this build: it is there for the others to find by name (gen_aux_tables.py),
    // e.g. WSAStartup for Ninja Gaiden 2004; Ninja Gaiden Black takes its extra table as it is, and installing that
    // entry wrote a jump at 0 (r830).
    all.erase(std::remove_if(all.begin(), all.end(), [](const Hle &r) { return r.addr == 0; }), all.end());
    // C++ static initializers of the libraries (run by _cinit before main) only set up globals: keep them.
    // The initializer table ranges come from the build profile (read from XAPI's _rtinit / _cinit).
    std::vector<uint32_t> initializers;
    for (const InitTableRange &r : profile.init_tables) {
        for (uint32_t p = r.begin; p < r.end; p += 4) {
            uint32_t fn = *(uint32_t *)(uintptr_t)p;
            if (fn && fn != 0xFFFFFFFF) initializers.push_back(fn);
        }
    }
    int replaced = 0, stopped = 0, traps = 0, native = 0;
    for (size_t i = 0; i < g_funcs.size(); i++) {
        const LibFunc &f = g_funcs[i];
        uint8_t *fn = (uint8_t *)(uintptr_t)f.addr;
        const Hle *h = nullptr;
        for (const Hle &r : all)
            if (r.addr == f.addr) h = &r;
        if (!h && std::find(initializers.begin(), initializers.end(), f.addr) != initializers.end()) {
            native++;
            continue;
        }
        uint32_t room = f.size;
        if (i + 1 < g_funcs.size() && g_funcs[i + 1].addr - f.addr < room) room = g_funcs[i + 1].addr - f.addr;
        if (h && !h->impl) {
            native++;
            continue;
        }
        void *target;
        if (h) {
            target = make_thunk(*h, timed(*h));
            if (!target) {
                logf("hooks: bad parameter spec '%s' for %s", h->params, h->name);
                return false;
            }
            replaced++;
        } else {
            uint8_t *t = code_alloc(10);
            uint32_t idx = (uint32_t)i;
            t[0] = 0x68; // push index
            memcpy(t + 1, &idx, 4);
            write_jmp(t + 5, (const void *)&unimplemented_common);
            target = t;
            stopped++;
        }
        if (room >= 5) {
            write_jmp(fn, target);
        } else if (!h) {
            fn[0] = 0xCC;
            g_int3_hook[i] = true;
            traps++;
        } else {
            logf("hooks: %s is too small (%u bytes) for a jump", f.name, room);
            return false;
        }
    }
    for (const Hle &r : all) {
        bool found = false;
        for (const LibFunc &f : g_funcs) found |= f.addr == r.addr;
        if (!found && r.impl && online_enabled() && (!strcmp(r.name, "XNetStartup") || !strcmp(r.name, "WSAStartup"))) {
            logf("hooks: online play: %s runs as the game has it (net/nvnet.cpp)", r.name); // the game's own
            continue;
        }
        if (!found && r.impl) { // replacement outside the library sections (BLACK: InsertFence-style LTCG strays)
            write_jmp((uint8_t *)(uintptr_t)r.addr, make_thunk(r, timed(r)));
            replaced++;
        }
    }
    AddVectoredExceptionHandler(1, int3_handler);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    logf("hooks: D3D+DSOUND+XPP (%zu functions): %d replaced by HLE, %d run natively, %d stop on first call (%d via int3)",
         g_funcs.size(), replaced, native, stopped, traps);
    return true;
}

} // namespace xd3d
