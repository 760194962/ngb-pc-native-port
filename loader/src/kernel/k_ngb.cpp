// Kernel exports Ninja Gaiden Black imports and BLACK does not (docs/02 §3 of the port project). Only the ones its
// own code (.text) reaches are real; what XNET / XONLINE / XPP's memory-unit driver call stays a logging stub.
#include "kernel/kexports.h"
#include "kernel/nt.h"

#include <mutex>

namespace xk {

// XAPI fibers (CreateFiber / SwitchToFiber, used by the game) get their stacks from the kernel. Nothing on the host
// needs to know about them: natively executed code just switches esp into the block. Guard page below the stack as
// on the console; the returned pointer is the stack base (top), as MmCreateKernelStack returns.
void *__stdcall MmCreateKernelStack(uint32_t NumberOfBytes, BOOLEAN DebuggerThread)
{
    KTRACE("0x%x, %u", NumberOfBytes, DebuggerThread);
    uint32_t size = (NumberOfBytes + 0xFFF) & ~0xFFFu;
    uint8_t *limit = (uint8_t *)VirtualAlloc(nullptr, size + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!limit) return nullptr;
    DWORD old;
    VirtualProtect(limit, 0x1000, PAGE_NOACCESS, &old); // guard page
    return limit + 0x1000 + size;
}

void __stdcall MmDeleteKernelStack(void *StackBase, void *StackLimit)
{
    KTRACE("%p, %p", StackBase, StackLimit);
    if (StackLimit) VirtualFree((uint8_t *)StackLimit - 0x1000, 0, MEM_RELEASE);
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
// DHCP loop); the DPC thread refreshes it whenever it wakes, and readers that spin see it advance.
volatile uint32_t KeTickCount;
static ULONGLONG g_tick_base;

void ktick_update()
{
    ULONGLONG now = GetTickCount64();
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
