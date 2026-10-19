// Kernel exports Ninja Gaiden Black imports and BLACK does not (docs/02 §3 of the port project). Only the ones its
// own code (.text) reaches are real; what XNET / XONLINE / XPP's memory-unit driver call stays a logging stub.
#include "kernel/kexports.h"
#include "kernel/nt.h"

#include <map>
#include <mutex>

namespace xk {

// XAPI fibers (CreateFiber / SwitchToFiber, used by the game) get their stacks from the kernel. Nothing on the host
// needs to know about them: natively executed code just switches esp into the block. The game asks for 16 KB
// (enough for its own code on the console), but host code called from a fiber (the Direct3D 9 driver under the D3D
// HLE) needs far more, and an overflow of a 16 KB block killed the process before any handler could log it
// (docs/04). So the block is at least MIN_FIBER_STACK bytes: the game sees StackBase and StackLimit = StackBase -
// NumberOfBytes as asked, and the memory below its limit is ours, committed, with a guard page at the bottom.
constexpr uint32_t MIN_FIBER_STACK = 1u << 20;
static std::mutex g_stack_mutex;
static std::map<void *, void *> g_stack_alloc; // StackBase -> allocation

void *__stdcall MmCreateKernelStack(uint32_t NumberOfBytes, BOOLEAN DebuggerThread)
{
    KTRACE("0x%x, %u", NumberOfBytes, DebuggerThread);
    uint32_t size = (NumberOfBytes + 0xFFF) & ~0xFFFu;
    uint32_t total = size < MIN_FIBER_STACK ? MIN_FIBER_STACK : size;
    uint8_t *alloc = (uint8_t *)VirtualAlloc(nullptr, total + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!alloc) return nullptr;
    DWORD old;
    VirtualProtect(alloc, 0x1000, PAGE_NOACCESS, &old); // guard page
    void *base = alloc + 0x1000 + total;
    std::lock_guard<std::mutex> g(g_stack_mutex);
    g_stack_alloc[base] = alloc;
    return base;
}

// The kernel stack esp lies in: [lo, hi) is its committed range (above the guard page). fiber.cpp keeps the Windows
// thread's stack bounds on it while a fiber runs there.
bool kernel_stack_bounds(uint32_t esp, uint32_t *lo, uint32_t *hi)
{
    std::lock_guard<std::mutex> g(g_stack_mutex);
    for (const auto &kv : g_stack_alloc) {
        uint32_t base = (uint32_t)(uintptr_t)kv.first, alloc = (uint32_t)(uintptr_t)kv.second;
        if (esp > alloc + 0x1000 && esp <= base) {
            *lo = alloc + 0x1000;
            *hi = base;
            return true;
        }
    }
    return false;
}

void __stdcall MmDeleteKernelStack(void *StackBase, void *StackLimit)
{
    KTRACE("%p, %p", StackBase, StackLimit);
    std::lock_guard<std::mutex> g(g_stack_mutex);
    auto it = g_stack_alloc.find(StackBase);
    if (it == g_stack_alloc.end()) {
        logf("[k] MmDeleteKernelStack(%p, %p): not a stack this kernel made", StackBase, StackLimit);
        return;
    }
    VirtualFree(it->second, 0, MEM_RELEASE);
    g_stack_alloc.erase(it);
}

void __stdcall DbgBreakPoint() { KTRACE(""); } // retail kernel: no debugger, nothing happens

uint32_t __stdcall KeSetDisableBoostThread(void *Thread, BOOLEAN Disable)
{
    KTRACE("%p, %u", Thread, Disable);
    return 0; // previous value: boosts were never disabled
}

// Only XNET reaches these two (docs/02 §3); the game's own timer is XAPI's rdtsc-based QueryPerformanceCounter.
LARGE_INTEGER __stdcall KeQueryPerformanceCounter()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t;
}

LARGE_INTEGER __stdcall KeQueryPerformanceFrequency()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
}

BOOLEAN __stdcall HalIsResetOrShutdownPending()
{
    return FALSE;
}

// Ethernet: no link. The game's online module (Ranking) asks before it starts XNET (docs/02 §9); an absent cable is
// a state the game handles on the console.
uint32_t __stdcall PhyGetLinkState(BOOLEAN Verify)
{
    KTRACE("%u", Verify);
    return 0;
}

NTSTATUS __stdcall PhyInitialize(BOOLEAN ForceReset, void *Param2)
{
    KTRACE("%u, %p", ForceReset, Param2);
    return XSTATUS_SUCCESS;
}

// KeTickCount: milliseconds since boot, read by XAPI timeouts and XNET. It must move (xboxrecomp's note on Halo's
// DHCP loop); the DPC thread refreshes it whenever it wakes (at most 4 ms apart), and readers that spin see it advance.
// Readers in Ninja Gaiden Black: XAPI GetTickCount (0x329B27; its game callers time 1 s and 10 s intervals, play
// time and seeds) and XNET (10 sites); docs/07 #6. The Xbox adds 1 every 1 ms: counted from the performance counter.
volatile uint32_t KeTickCount;
static uint64_t g_tick_base;

void ktick_update()
{
    uint64_t now = kclock_ms();
    if (!g_tick_base) g_tick_base = now;
    KeTickCount = (uint32_t)(now - g_tick_base);
}

uint8_t XboxLANKey[6];          // MAC address: zero (no network)
uint8_t XePublicKeyData[284];   // Xbox Live public key: zero (nothing signed by it is verified here)
static char g_disk_model[] = "ST310211A";
static char g_disk_serial[] = "0000000000000000";
XANSI_STRING HalDiskModelNumber = {(uint16_t)(sizeof g_disk_model - 1), (uint16_t)sizeof g_disk_model, g_disk_model};
XANSI_STRING HalDiskSerialNumber = {(uint16_t)(sizeof g_disk_serial - 1), (uint16_t)sizeof g_disk_serial, g_disk_serial};

} // namespace xk
