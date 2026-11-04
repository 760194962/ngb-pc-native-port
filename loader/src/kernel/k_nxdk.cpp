// Kernel exports that the games do not import: nxdk programs' (the Insignia Setup Assistant, title FFFF0002: SDL2,
// lwIP, its own NIC and USB drivers, mbedtls) and the dashboard's Xbox Live part (xonlinedash.xbe). The synchronization
// ones are in k_sync.cpp.
#include "kernel/kexports.h"
#include "kernel/nt.h"

#include <cstring>

namespace xk {

// The EEPROM is not written: the registered console's stays as it is (net/online.cpp reads it); logged
NTSTATUS __stdcall ExSaveNonVolatileSetting(uint32_t ValueIndex, uint32_t Type, const void *Value, uint32_t ValueLength)
{
    logf("[k] ExSaveNonVolatileSetting(0x%x, type %u, len %u): not written (the EEPROM stays as it is)", ValueIndex, Type,
         ValueLength);
    return XSTATUS_SUCCESS;
}

int32_t __fastcall KInterlockedDecrement(volatile int32_t *Addend) { return _InterlockedDecrement((volatile long *)Addend); }
int32_t __fastcall KInterlockedIncrement(volatile int32_t *Addend) { return _InterlockedIncrement((volatile long *)Addend); }
int32_t __fastcall KInterlockedExchange(volatile int32_t *Target, int32_t Value)
{
    return _InterlockedExchange((volatile long *)Target, Value);
}

// The object directory (\??, \Device): nothing to list
NTSTATUS __stdcall NtOpenDirectoryObject(HANDLE *DirectoryHandle, XOBJECT_ATTRIBUTES *ObjectAttributes)
{
    logf("[k] NtOpenDirectoryObject('%s'): no object directories", ObjectAttributes && ObjectAttributes->ObjectName
                                                                      ? xstr(ObjectAttributes->ObjectName).c_str()
                                                                      : "");
    if (DirectoryHandle) *DirectoryHandle = nullptr;
    return XSTATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS __stdcall NtQueryDirectoryObject(HANDLE DirectoryHandle, void *Buffer, uint32_t Length, BOOLEAN ReturnSingleEntry,
                                          uint32_t *Context, uint32_t *ReturnLength)
{
    if (ReturnLength) *ReturnLength = 0;
    return 0x8000001A; // STATUS_NO_MORE_ENTRIES
}

NTSTATUS __stdcall NtPulseEvent(HANDLE EventHandle, int32_t *PreviousState)
{
    KTRACE("%p", EventHandle);
    if (PreviousState) *PreviousState = 0;
    return PulseEvent(EventHandle) ? XSTATUS_SUCCESS : XSTATUS_INVALID_HANDLE;
}

void __stdcall RtlRaiseException(void *ExceptionRecord)
{
    const uint32_t *r = (const uint32_t *)ExceptionRecord;
    logc("[k] RtlRaiseException: code 0x%08x at 0x%08x (not handled: the program stops here)", r ? r[0] : 0, r ? r[3] : 0);
    fatal("RtlRaiseException 0x%08x", r ? r[0] : 0);
}

void __stdcall RtlUnwind(void *TargetFrame, void *TargetIp, void *ExceptionRecord, void *ReturnValue)
{
    logc("[k] RtlUnwind(%p, %p): not implemented", TargetFrame, TargetIp);
    fatal("RtlUnwind");
}

void __stdcall KRtlZeroMemory(void *Destination, uint32_t Length) { memset(Destination, 0, Length); }

// The DVD tray: closed, with media (the SMC's 0x60), no change since boot
NTSTATUS __stdcall HalReadSMCTrayState(uint32_t *State, uint32_t *Count)
{
    if (State) *State = 0x60;
    if (Count) *Count = 0;
    return XSTATUS_SUCCESS;
}

NTSTATUS __stdcall IoDismountVolumeByName(XANSI_STRING *DeviceName)
{
    logf("[k] IoDismountVolumeByName('%s'): nothing to dismount", DeviceName ? xstr(DeviceName).c_str() : "");
    return XSTATUS_SUCCESS;
}

// Strings RtlUnicodeStringToAnsiString allocated (ExAllocatePoolWithTag, k_rtl.cpp)
void __stdcall RtlFreeAnsiString(XANSI_STRING *AnsiString)
{
    if (AnsiString && AnsiString->Buffer) {
        ExFreePool(AnsiString->Buffer);
        AnsiString->Buffer = nullptr;
        AnsiString->Length = AnsiString->MaximumLength = 0;
    }
}

void __stdcall RtlInitUnicodeString(XUNICODE_STRING *DestinationString, const wchar_t *SourceString)
{
    size_t n = SourceString ? wcslen(SourceString) : 0;
    DestinationString->Buffer = (wchar_t *)SourceString;
    DestinationString->Length = (uint16_t)(n * 2);
    DestinationString->MaximumLength = (uint16_t)(SourceString ? n * 2 + 2 : 0);
}

} // namespace xk
