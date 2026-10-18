#include "hooks.h"

#include "build_profile.h"
#include "d3d/draw.h"

#include "image.h"
#include "xbe.h"
#include "xthread.h"

namespace {

bool in_image(uint32_t addr)
{
    const Xbe *x = image_xbe();
    return x && addr >= x->base && addr < x->base + x->size_of_image;
}

// "module.dll+0x1234" for an address in a loaded module (blackrt.map from the build gives port functions).
std::string module_offset(uint32_t addr)
{
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)addr, &m) ||
        !GetModuleFileNameW(m, path, MAX_PATH))
        return "";
    std::wstring w = path;
    char b[160];
    snprintf(b, sizeof b, "%s+0x%x", narrow(w.substr(w.find_last_of(L"\\/") + 1)).c_str(), addr - (uint32_t)(uintptr_t)m);
    return b;
}

std::string where(uint32_t addr)
{
    if (!in_image(addr)) {
        std::string mo = module_offset(addr);
        if (!mo.empty()) return mo;
    }
    std::string s = sym_name(addr);
    if (const Xbe *x = image_xbe())
        if (const XbeSection *sec = x->section_containing(addr)) s += " [" + sec->name + "]";
    return s;
}


thread_local int t_probing; // > 0 while safe_read is reading: its faults are expected and handled by its __except

bool safe_read(const uint32_t *p, uint32_t *out)
{
    t_probing++;
    __try {
        *out = *p;
        t_probing--;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_probing--;
        return false;
    }
}

} // namespace

// Return addresses on the stack: values inside the image preceded by a call instruction.
void log_stack_trace(const uint32_t *esp)
{
    // Stay inside the current thread's stack (reading past its top faults). On a stack Windows does not know (an
    // XAPI fiber's, see fiber.cpp) walk 16 KB from esp; safe_read stops at the first unmapped page.
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    const uint32_t *top = (const uint32_t *)tib->StackBase;
    if (esp < (const uint32_t *)tib->StackLimit || esp >= top) top = esp + 0x1000;
    log_stack_trace_ex(esp, top);
}

void log_stack_trace_ex(const uint32_t *esp, const uint32_t *top)
{
    const Xbe *x = image_xbe();
    if (!x) return;
    static uint32_t rt_lo, rt_hi; // blackrt.dll: port frames are listed too
    if (!rt_lo) {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&log_stack_trace_ex, &m);
        auto *nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
        rt_lo = (uint32_t)(uintptr_t)m;
        rt_hi = rt_lo + nt->OptionalHeader.SizeOfImage;
    }
    // Return addresses into the game image, blackrt.dll and (after those two, so a host-library crash still shows
    // the port frame that called into it) any other loaded module. 16 KB of stack covers d3d9's deepest paths.
    int found = 0;
    for (int i = 0; i < 4096 && found < 24 && esp + i < top; i++) {
        uint32_t v;
        if (!safe_read(esp + i, &v)) break;
        bool ours = (in_image(v) && v >= x->base + 8) || (v >= rt_lo + 8 && v < rt_hi);
        if (!ours && (v < 0x10000 || module_offset(v).empty())) continue;
        uint32_t lo, hi; // the 8 bytes before the candidate return address
        if (!safe_read((const uint32_t *)(uintptr_t)(v - 8), &lo) || !safe_read((const uint32_t *)(uintptr_t)(v - 4), &hi)) continue;
        uint8_t p[8];
        memcpy(p, &lo, 4);
        memcpy(p + 4, &hi, 4);
        bool call = p[3] == 0xE8 || (p[6] == 0xFF && (p[7] & 0x38) == 0x10) || (p[5] == 0xFF && (p[6] & 0x38) == 0x10) ||
                    (p[2] == 0xFF && (p[3] & 0x38) == 0x10);
        if (!call) continue;
        logf("    ret [esp+0x%04x] %08x %s", i * 4, v, where(v).c_str());
        found++;
    }
}

namespace {

const char *exception_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIVILEGED_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
    case EXCEPTION_SINGLE_STEP: return "SINGLE_STEP";
    case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
    default: return "exception";
    }
}

void dump_exception(EXCEPTION_POINTERS *ep, const char *kind)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    XThread *t = xthread_current_or_null();
    logf("%s %s (0x%08lx) at %08lx %s, thread '%s'", kind, exception_name(er->ExceptionCode), er->ExceptionCode, c->Eip,
         where(c->Eip).c_str(), t ? t->name : "host");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        uint32_t op = (uint32_t)er->ExceptionInformation[0];
        const char *opname = op == 0 ? "read" : op == 1 ? "write" : op == 8 ? "execute" : "?";
        uint32_t addr = (uint32_t)er->ExceptionInformation[1];
        const char *note = "";
        if (addr >= 0xFD000000 && addr < 0xFE000000) note = "  (NV2A GPU registers)";
        else if (addr >= 0xFE800000 && addr < 0xFE880000) note = "  (MCPX APU registers)";
        else if (addr >= 0xFEC00000 && addr < 0xFEC01000) note = "  (ACI audio registers)";
        else if (addr < 0x10000) note = "  (null pointer)";
        logf("    %s of address %08x%s", opname, addr, note);
    }
    logf("    eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx", c->Eax, c->Ebx, c->Ecx, c->Edx,
         c->Esi, c->Edi, c->Ebp, c->Esp);
    if (in_image(c->Eip)) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)c->Eip;
        logf("    code: %02x %02x %02x %02x %02x %02x %02x %02x", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    }
    log_stack_trace((const uint32_t *)(uintptr_t)c->Esp);
    for (unsigned i = 0; i < 4 && i < xd3d::g_last_draw_next; i++) {
        const xd3d::LastDraw &d = xd3d::g_last_draws[(xd3d::g_last_draw_next - 1 - i) % 4];
        logf("    draw -%u: prim %u, %u indices %u..%u, stride %u: host reads %p+0x%zx (vertex-0 pointer %p); converted block %p+0x%zx", i,
             d.prim, d.nidx, d.mn, d.mx, d.stride, d.src, d.bytes, d.verts, d.conv, d.conv_bytes);
    }
}

uint8_t g_probe_orig[16];
thread_local int t_probe_rearm = -1;
std::atomic<uint32_t> g_probe_hits[16];

int probe_index(uint32_t addr)
{
    for (int i = 0; i < g_cfg.probe_count; i++)
        if (g_cfg.probe[i] == addr) return i;
    return -1;
}

// rdtsc in game code: the Xbox's time stamp counter runs at 733.33 MHz and the engine scales its deltas with that
// in mind (FUN_0030E140: delta x 0.113 = its frame time, compared with 94 / 103.5 to fade the character shadow
// out and to drop effect detail). A host TSC is 3-4x faster, so the game saw every frame as far too slow. The
// sites are patched to ud2 (0F 0B, same length) and emulated here from QueryPerformanceCounter at 733.33 MHz.
// The engine's timer measures its own frame work, which on the Xbox excludes the GPU; here every HLE draw runs
// synchronously on the game thread, so the measured time sat at 78-99 against thresholds of 94 (shadow fade) and
// 103.5 and the shadow flickered on and off (docs/04 §3.9). Until rendering is asynchronous, the engine's sites
// see the counter divided by --tsc-divisor (4: the game's CPU share of a frame looks like the Xbox's).
const uint32_t *g_rdtsc_sites;
int g_rdtsc_site_count;
uint32_t g_rdtsc_qpc_site;
LARGE_INTEGER g_tsc_base, g_tsc_freq;
std::atomic<uint32_t> g_rdtsc_hits{0};

bool rdtsc_site(uint32_t eip)
{
    for (int i = 0; i < g_rdtsc_site_count; i++)
        if (g_rdtsc_sites[i] == eip) return true;
    return false;
}

uint64_t xbox_tsc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    uint64_t d = (uint64_t)(t.QuadPart - g_tsc_base.QuadPart), f = (uint64_t)g_tsc_freq.QuadPart;
    return (d / f) * 733333333ull + ((d % f) * 733333333ull) / f;
}

// --coverage: which of a list of code addresses run (gate 4b: the HANZI entries of x2.1). Each address gets an int3
// once; the first hit logs the frame and thread, restores the byte and continues, so hot paths cost one exception.
struct CoverageSite {
    uint32_t addr;
    uint8_t orig;
    std::atomic<uint32_t> hit_frame; // 0 = not yet
};
CoverageSite g_cov[64];
int g_cov_count;

LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT && g_cov_count) {
        uint32_t a = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        for (int i = 0; i < g_cov_count; i++) {
            if (g_cov[i].addr != a) continue;
            DWORD old;
            VirtualProtect((void *)(uintptr_t)a, 1, PAGE_EXECUTE_READWRITE, &old);
            *(uint8_t *)(uintptr_t)a = g_cov[i].orig;
            VirtualProtect((void *)(uintptr_t)a, 1, old, &old);
            uint32_t f = g_frame_count.load() + 1;
            g_cov[i].hit_frame = f;
            XThread *t = xthread_current_or_null();
            const uint32_t *s = (const uint32_t *)(uintptr_t)ep->ContextRecord->Esp;
            logf("coverage: %08x %s first hit at frame %u, thread '%s', return %08x %s", a, where(a).c_str(), f, t ? t->name : "host", s[0],
                 where(s[0]).c_str());
            ep->ContextRecord->Eip = a;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION && g_rdtsc_site_count && rdtsc_site((uint32_t)ep->ContextRecord->Eip)) {
        CONTEXT *c = ep->ContextRecord;
        uint64_t v = xbox_tsc();
        if (c->Eip != g_rdtsc_qpc_site && g_cfg.tsc_divisor > 1) v /= g_cfg.tsc_divisor;
        c->Eax = (uint32_t)v;
        c->Edx = (uint32_t)(v >> 32);
        c->Eip += 2;
        uint32_t n = ++g_rdtsc_hits;
        if (n <= 8) logf("rdtsc: at %08lx %s -> %llu (#%u)", c->Eip - 2, where(c->Eip - 2).c_str(), (unsigned long long)v, n);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_BREAKPOINT && g_cfg.probe_count) {
        CONTEXT *c = ep->ContextRecord;
        int i = probe_index((uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress);
        if (i >= 0) {
            const uint32_t *s = (const uint32_t *)(uintptr_t)c->Esp;
            uint32_t n = ++g_probe_hits[i];
            if (n <= 50) {
                XThread *t = xthread_current_or_null();
                float xmm0 = 0.0f; // FXSAVE image: XMM0 at byte 160
                if ((c->ContextFlags & CONTEXT_EXTENDED_REGISTERS) == CONTEXT_EXTENDED_REGISTERS) memcpy(&xmm0, c->ExtendedRegisters + 160, 4);
                logf("probe %08x %s #%u, thread '%s': eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx xmm0=%g | ret %08x args "
                     "%08x %08x %08x %08x %08x",
                     g_cfg.probe[i], where(g_cfg.probe[i]).c_str(), n, t ? t->name : "host", c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi,
                     c->Edi, xmm0, s[0], s[1], s[2], s[3], s[4], s[5]);
            }
            *(uint8_t *)(uintptr_t)g_cfg.probe[i] = g_probe_orig[i]; // run the original instruction once
            c->Eip = g_cfg.probe[i];
            c->EFlags |= 0x100; // trap after it to re-arm
            t_probe_rearm = i;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_SINGLE_STEP && t_probe_rearm >= 0) {
        *(uint8_t *)(uintptr_t)g_cfg.probe[t_probe_rearm] = 0xCC;
        t_probe_rearm = -1;
        if (!(ep->ContextRecord->Dr6 & 0xF)) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && g_pad_watch && (ep->ContextRecord->Dr6 & 8)) {
        CONTEXT *c = ep->ContextRecord;
        c->Dr6 &= ~8u;
        uint32_t eip = c->Eip;
        static uint32_t seen[64], counts[64], total;
        if (eip >= 0x10000 && eip < 0x587a40 && total < 400) { // game code only (the HLE writes the buffer too)
            int k = 0;
            while (k < 64 && seen[k] && seen[k] != eip) k++;
            if (k < 64) {
                seen[k] = eip;
                if (counts[k]++ < 3) {
                    total++;
                    logf("padwatch: %08x = %d / %g accessed by instruction before %08x %s | eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx "
                         "esi=%08lx edi=%08lx ebp=%08lx",
                         g_pad_watch, *(int16_t *)(uintptr_t)g_pad_watch, *(float *)(uintptr_t)g_pad_watch, eip, where(eip).c_str(),
                         c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp);
                    log_stack_trace((const uint32_t *)(uintptr_t)c->Esp);
                }
            }
        }
        if (!(c->Dr6 & 0xF)) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && g_cfg.watch_count && (ep->ContextRecord->Dr6 & 0xF)) {
        CONTEXT *c = ep->ContextRecord;
        for (int i = 0; i < 4; i++) {
            if (!(c->Dr6 & (1u << i)) || i >= g_cfg.watch_count) continue;
            uint32_t a = g_cfg.watch[i];
            XThread *t = xthread_current_or_null();
            logf("watch: write to %08x = %08x by instruction before %08lx %s, thread '%s'", a, *(uint32_t *)(uintptr_t)a, c->Eip,
                 where(c->Eip).c_str(), t ? t->name : "host");
            log_stack_trace((const uint32_t *)(uintptr_t)c->Esp);
        }
        c->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Privileged instructions in game or XAPI code that are no-ops in a user-mode process: wbinvd / invd (cache
    // flushes, Ninja Gaiden Black's resource flush 0x31E0D0 after D3D_BlockOnResource), cli / sti (interrupt masking
    // around XAPI's kernel-version workarounds). Skip them and count; the first few are logged with the site.
    if (code == EXCEPTION_PRIV_INSTRUCTION && in_image(ep->ContextRecord->Eip)) {
        CONTEXT *c = ep->ContextRecord;
        const uint8_t *p = (const uint8_t *)(uintptr_t)c->Eip;
        int len = 0;
        const char *what = nullptr;
        if (p[0] == 0x0F && p[1] == 0x09) { len = 2; what = "wbinvd"; }
        else if (p[0] == 0x0F && p[1] == 0x08) { len = 2; what = "invd"; }
        else if (p[0] == 0xFA) { len = 1; what = "cli"; }
        else if (p[0] == 0xFB) { len = 1; what = "sti"; }
        if (len) {
            static std::atomic<uint32_t> skipped{0};
            uint32_t n = ++skipped;
            if (n <= 8 || g_cfg.trace) {
                XThread *t = xthread_current_or_null();
                logf("priv: %s at %08lx %s skipped (#%u), thread '%s'", what, c->Eip, where(c->Eip).c_str(), n, t ? t->name : "host");
            }
            c->Eip += len;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    switch (code) {
    case 0x40010006: // OutputDebugString
    case 0x4001000A:
    case 0x406D1388: // thread naming
    case 0xE06D7363: // C++ exception
    case EXCEPTION_BREAKPOINT: // int3 hooks are handled by their own handler
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code < 0x80000000) return EXCEPTION_CONTINUE_SEARCH;
    if (t_probing > 0) return EXCEPTION_CONTINUE_SEARCH; // safe_read's own probe
    static std::atomic<int> logged{0};
    if (logged++ < 20) dump_exception(ep, "first-chance");
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep)
{
    dump_exception(ep, "UNHANDLED");
    logc("exiting with code 1 (unhandled %s at %08lx %s; registers and stack in the log)",
         exception_name(ep->ExceptionRecord->ExceptionCode), ep->ContextRecord->Eip, where(ep->ContextRecord->Eip).c_str());
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 1); // the crashing thread may be the one that would do the exit
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void dump_threads()
{
    std::vector<XThread *> threads = xthread_all();
    for (XThread *t : threads) {
        if (!t->host_handle || t->host_tid == GetCurrentThreadId()) continue;
        if (SuspendThread(t->host_handle) == (DWORD)-1) continue;
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t->host_handle, &c)) {
            logf("thread '%s' (tid %lu): eip %08lx %s, esp %08lx", t->name, t->host_tid, c.Eip, where(c.Eip).c_str(), c.Esp);
            log_stack_trace_ex((const uint32_t *)(uintptr_t)c.Esp, (const uint32_t *)(uintptr_t)(c.Esp + 0x4000));
        }
        ResumeThread(t->host_handle);
    }
}

void install_coverage()
{
    if (g_cfg.coverage.empty()) return;
    FILE *f = _wfopen(g_cfg.coverage.c_str(), L"r");
    if (!f) {
        logf("coverage: cannot open %s", narrow(g_cfg.coverage).c_str());
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f) && g_cov_count < 64) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        uint32_t a = (uint32_t)strtoul(line, nullptr, 16);
        if (!a || !in_image(a)) continue;
        CoverageSite &c = g_cov[g_cov_count++];
        c.addr = a;
        DWORD old;
        VirtualProtect((void *)(uintptr_t)a, 1, PAGE_EXECUTE_READWRITE, &old);
        c.orig = *(uint8_t *)(uintptr_t)a;
        *(uint8_t *)(uintptr_t)a = 0xCC;
        VirtualProtect((void *)(uintptr_t)a, 1, old, &old);
    }
    fclose(f);
    logf("coverage: %d one-shot probes armed from %s", g_cov_count, narrow(g_cfg.coverage).c_str());
}

void log_coverage_summary()
{
    std::string hit, miss;
    int nh = 0;
    char b[16];
    for (int i = 0; i < g_cov_count; i++) {
        snprintf(b, sizeof b, " %08x", g_cov[i].addr);
        (g_cov[i].hit_frame ? hit : miss) += b;
        nh += g_cov[i].hit_frame != 0;
    }
    logf("coverage summary: %d of %d hit;%s | not hit:%s", nh, g_cov_count, hit.c_str(), miss.c_str());
}

uint32_t probe_hit_count(int i) { return i >= 0 && i < 16 ? g_probe_hits[i].load() : 0; }

void install_probes()
{
    for (int i = 0; i < g_cfg.probe_count; i++) {
        uint8_t *p = (uint8_t *)(uintptr_t)g_cfg.probe[i];
        g_probe_orig[i] = *p;
        *p = 0xCC;
        logf("probe armed at %08x %s", g_cfg.probe[i], where(g_cfg.probe[i]).c_str());
    }
}

bool install_rdtsc_emulation(const BuildProfile &profile)
{
    QueryPerformanceFrequency(&g_tsc_freq);
    QueryPerformanceCounter(&g_tsc_base);
    for (int i = 0; i < profile.rdtsc_site_count; i++) {
        uint8_t *p = (uint8_t *)(uintptr_t)profile.rdtsc_sites[i];
        if (p[0] != 0x0F || p[1] != 0x31) {
            logf("rdtsc: no rdtsc at %08x (found %02x %02x)", profile.rdtsc_sites[i], p[0], p[1]);
            return false;
        }
        DWORD old;
        if (!VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) return false;
        p[1] = 0x0B; // ud2
        VirtualProtect(p, 2, old, &old);
    }
    g_rdtsc_sites = profile.rdtsc_sites;
    g_rdtsc_site_count = profile.rdtsc_site_count;
    g_rdtsc_qpc_site = profile.rdtsc_qpc_site;
    if (g_rdtsc_site_count)
        logf("rdtsc: %d sites trap to a 733.33 MHz counter (host QPC %lld Hz); the engine's frame timer runs /%u", g_rdtsc_site_count,
             (long long)g_tsc_freq.QuadPart, g_cfg.tsc_divisor);
    return true;
}

void install_crash_handlers()
{
    AddVectoredExceptionHandler(1, vectored_handler);
    SetUnhandledExceptionFilter(unhandled_filter);
}
