// XAPI fibers on a Windows thread.
//
// Ninja Gaiden Black runs its game loop on XAPI fibers (CreateFiber / SwitchToFiber, 70 call sites). XAPI's
// SwitchToFiber (0x32A293 in x2.1, 63 bytes) saves fs:[0] and the callee-saved registers, stores esp in the current
// fiber object (+0xC), loads the new fiber's esp and makes it current; the stacks come from MmCreateKernelStack.
// Nothing tells Windows about the switch: the thread's TEB still describes its original stack, so while a fiber
// runs, esp is outside [StackLimit, StackBase). That is fatal the moment anything raises an exception on the fiber:
// RtlDispatchException rejects the SEH chain as off-stack and the process dies without running a handler (the
// silent exits of docs/04: nothing after the vectored handler's first-chance lines).
//
// The replacement below does exactly what XAPI's did and, before switching, moves StackBase / StackLimit to the
// kernel stack the new esp lies in (or back to the thread's own stack when the fiber switches back).
#include "fiber.h"

#include "common.h"
#include "kernel/kexports.h"
#include "xthread.h"

#include <windows.h>

namespace {

uint32_t g_fiber_tls_index_addr; // the dword XAPI's fiber code indexes the TLS array with (0x45E84C in x2.1)
uint32_t g_tls_array_fs_off;     // what fs:[4] was remapped to (xthread.cpp): the TLS array pointer

thread_local bool t_saved;
thread_local void *t_saved_base, *t_saved_limit;

void __cdecl fiber_switch_note(uint32_t new_esp)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    uint32_t lo, hi;
    if (xk::kernel_stack_bounds(new_esp, &lo, &hi)) {
        if (!t_saved) {
            t_saved_base = tib->StackBase;
            t_saved_limit = tib->StackLimit;
            t_saved = true;
        }
        tib->StackBase = (void *)(uintptr_t)hi;
        tib->StackLimit = (void *)(uintptr_t)lo;
    } else if (t_saved) {
        tib->StackBase = t_saved_base;
        tib->StackLimit = t_saved_limit;
    }
}

// XAPI 5849 SwitchToFiber, instruction for instruction, plus the note. fs:[4] is read through the TEB self pointer
// at the remapped offset, which is what the patched original does.
__declspec(naked) void __stdcall switch_to_fiber(void * /*pFiber*/)
{
    __asm {
        mov edx, g_fiber_tls_index_addr
        mov edx, [edx]
        mov eax, fs:[0x18]
        mov ecx, g_tls_array_fs_off
        mov ecx, [eax + ecx]
        mov eax, [esp + 4]
        push ebp
        push esi
        push edi
        push ebx
        push dword ptr fs:[0]
        mov edx, [ecx + edx * 4]
        mov ecx, [edx + 8]
        mov [ecx + 0xC], esp
        push eax
        push edx
        push dword ptr [eax + 0xC]
        call fiber_switch_note
        add esp, 4
        pop edx
        pop eax
        mov esp, [eax + 0xC]
        mov [edx + 8], eax
        pop dword ptr fs:[0]
        pop ebx
        pop edi
        pop esi
        pop ebp
        ret 4
    }
}

} // namespace

bool install_fiber_switch(const BuildProfile &profile)
{
    if (!profile.switch_to_fiber || !profile.fiber_tls_index) return true;
    g_fiber_tls_index_addr = profile.fiber_tls_index;
    g_tls_array_fs_off = xthread_slot_fs_offset(XSLOT_TLS_ARRAY);
    uint8_t *at = (uint8_t *)(uintptr_t)profile.switch_to_fiber;
    // XAPI's prologue: mov edx, [index]; mov ecx, fs:[remapped]; the fs displacement was rewritten by the fs patcher.
    if (at[0] != 0x8B || at[1] != 0x15 || at[6] != 0x64 || at[7] != 0x8B || at[8] != 0x0D) {
        logf("fiber: SwitchToFiber at 0x%08x does not look like XAPI's (%02x %02x %02x ...), not replaced", profile.switch_to_fiber,
             at[0], at[1], at[2]);
        return false;
    }
    DWORD old;
    VirtualProtect(at, 8, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)&switch_to_fiber - (uintptr_t)(at + 5));
    memcpy(at + 1, &rel, 4);
    VirtualProtect(at, 8, old, &old);
    logf("fiber: SwitchToFiber at 0x%08x replaced (TEB stack bounds follow the fiber stacks)", profile.switch_to_fiber);
    return true;
}
