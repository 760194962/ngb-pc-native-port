// Hardware-facing and miscellaneous kernel exports: AV, HAL, interrupts, EEPROM settings, crypto, data exports.
// Most hardware calls only matter to the statically linked D3D/DSound code, which is being replaced; they
// log and return values that keep XAPI going.
#include "hooks.h"
#include "kernel/kexports.h"
#include "net/nvnet.h"
#include "net/online.h"
#include "sha1.h"

#include <intrin.h>
#include "xbe.h"
#include "xthread.h"

namespace {

void *g_av_saved_data;
char g_image_file_name[260] = "\\Device\\CdRom0\\default.xbe";

} // namespace

namespace xk {

// --- data exports ---
uint8_t ExEventObjectType[64];
uint8_t IoFileObjectType[64];
uint8_t PsThreadObjectType[64];
uint32_t HalDiskCachePartitionCount = 3;
uint32_t HalBootSMCVideoMode = 0;
XLAUNCH_DATA_PAGE *LaunchDataPage = nullptr; // cold boot: no launch data
XXBOX_HARDWARE_INFO XboxHardwareInfo = {0, 0xA2, 0xD3, 0, 0}; // retail, not arcade (flag 8), not devkit (flag 2)
uint8_t XboxHDKey[16];
XXBOX_KRNL_VERSION XboxKrnlVersion = {1, 0, 5838, 1};
uint8_t XboxSignatureKey[16];
uint8_t XboxAlternateSignatureKeys[16][16];
XANSI_STRING XeImageFileName = {26, (uint16_t)sizeof g_image_file_name, g_image_file_name};

// XeImageFileName is the running image's path on the Xbox (\Device\CdRom0\default.xbe, or a bundled program's
// \Device\CdRom0\<dir>\<file> after XLaunchNewImage); it was always default.xbe. From the XBE's place in the
// game folder (= \Device\CdRom0).
void set_image_file_name(const std::wstring &xbe_path, const std::wstring &game_dir)
{
    wchar_t full[MAX_PATH], dir[MAX_PATH];
    std::wstring rel;
    if (GetFullPathNameW(xbe_path.c_str(), MAX_PATH, full, nullptr) && GetFullPathNameW(game_dir.c_str(), MAX_PATH, dir, nullptr)) {
        std::wstring f = full, d = dir;
        for (wchar_t &c : f) if (c == L'/') c = L'\\';
        for (wchar_t &c : d) if (c == L'/') c = L'\\';
        while (!d.empty() && d.back() == L'\\') d.pop_back();
        std::wstring hdd = host_full_path(g_cfg.hdd_dir) + L"\\";
        if (f.size() > d.size() + 1 && _wcsnicmp(f.c_str(), d.c_str(), d.size()) == 0 && f[d.size()] == L'\\')
            rel = f.substr(d.size() + 1);
        else if (f.size() > hdd.size() && _wcsnicmp(f.c_str(), hdd.c_str(), hdd.size()) == 0) // a title update on the disk
            rel = L"*" + f.substr(hdd.size());
        else
            rel = f.substr(f.find_last_of(L'\\') + 1);
    }
    if (rel.empty()) return;
    std::string name = rel[0] == L'*' ? "\\Device\\Harddisk0\\" + narrow(rel.substr(1)) : "\\Device\\CdRom0\\" + narrow(rel);
    if (name.size() >= sizeof g_image_file_name) return;
    memcpy(g_image_file_name, name.c_str(), name.size() + 1);
    XeImageFileName.Length = (uint16_t)name.size();
    logf("[k] XeImageFileName = %s", g_image_file_name);
}
uint8_t IdexChannelObject[256];

// --- AV ---

void *__stdcall AvGetSavedDataAddress()
{
    KTRACE("");
    return g_av_saved_data;
}

void __stdcall AvSetSavedDataAddress(void *Address)
{
    KTRACE("%p", Address);
    g_av_saved_data = Address;
}

void __stdcall AvSendTVEncoderOption(void *RegisterBase, uint32_t Option, uint32_t Param, uint32_t *Result)
{
    KTRACE("%p, %u, 0x%x, %p", RegisterBase, Option, Param, Result);
    if (!Result) return;
    switch (Option) {
    case 6: // AV_QUERY_AV_CAPABILITIES: standard AV pack, NTSC-M
        *Result = 0x00000100 | 0x00000001;
        break;
    default:
        *Result = 0;
        break;
    }
}

uint32_t __stdcall AvSetDisplayMode(void *RegisterBase, uint32_t Step, uint32_t Mode, uint32_t Format, uint32_t Pitch,
                                    uint32_t FrameBuffer)
{
    KTRACE("%p, %u, 0x%x, 0x%x, %u, 0x%x", RegisterBase, Step, Mode, Format, Pitch, FrameBuffer);
    return 0;
}

uint32_t __cdecl DbgPrint(const char *Format, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, Format);
    vsnprintf(msg, sizeof msg, Format, ap);
    va_end(ap);
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) msg[--n] = 0;
    logf("[DbgPrint] %s", msg);
    return 0;
}

// --- EEPROM settings ---

NTSTATUS __stdcall ExQueryNonVolatileSetting(uint32_t ValueIndex, uint32_t *Type, void *Value, uint32_t ValueLength,
                                             uint32_t *ResultLength)
{
    uint32_t dword = 0, size = 4, type = 4 /* REG_DWORD */;
    bool known = true;
    switch (ValueIndex) {
    case 0x000: dword = 0; break;                  // XC_TIMEZONE_BIAS (UTC)
    case 0x001: case 0x004: type = 3; break;       // XC_TZ_STD_NAME / XC_TZ_DLT_NAME (4 bytes)
    case 0x002: case 0x005: type = 3; break;       // XC_TZ_STD_DATE / XC_TZ_DLT_DATE
    case 0x003: case 0x006: dword = 0; break;      // XC_TZ_STD_BIAS / XC_TZ_DLT_BIAS
    case 0x007: dword = (uint32_t)g_cfg.language; break; // XC_LANGUAGE: black.ini language (1 English by default)
    case 0x008: dword = g_cfg.widescreen ? 0x00010000 : 0; break; // XC_VIDEO: bit 16 = widescreen TV, no HD modes
    case 0x009: dword = ((uint32_t)g_cfg.audio_mode | (g_cfg.audio_ac3 ? 0x10000u : 0u) | (g_cfg.audio_dts ? 0x20000u : 0u)); break; // XC_AUDIO: 0 stereo, 1 mono,
                                                   // 2 surround; 0x10000 Dolby Digital, 0x20000 DTS (black.ini audio_*)
    case 0x00A: dword = 0; break;                  // XC_P_CONTROL_GAMES: no limit
    case 0x00B: dword = 0; break;                  // XC_P_CONTROL_PASSWORD
    case 0x00C: dword = 0; break;                  // XC_P_CONTROL_MOVIES
    case 0x011: dword = 0; break;                  // XC_MISC: auto power-off off. XAPI 0x32F752 keeps bit 0,
                                                   // its 6-hour timer DPC 0x32F740 shuts down only if set
    case 0x012: dword = 1; break;                  // XC_DVD_REGION
    case 0x103: dword = 0x00400100; break;         // XC_FACTORY_AV_REGION: NTSC-M, 60 Hz
    case 0x104: dword = 1; break;                  // XC_FACTORY_GAME_REGION: North America
    case 0x0FF: case 0xFFFF: type = 3; size = ValueLength < 256 ? ValueLength : 256; break; // whole sections: zeros
    default: known = false; break;
    }
    uint8_t factory[16];
    uint32_t ftype, fsize;
    if (online_factory_setting(ValueIndex, &ftype, factory, &fsize)) { // online: the registered console's (net/online.cpp)
        logf("[k] ExQueryNonVolatileSetting(0x%x, len %u) -> the registered console's factory value (%u bytes)", ValueIndex, ValueLength, fsize);
        if (ValueLength < fsize) return XSTATUS_BUFFER_TOO_SMALL;
        if (Type) *Type = ftype;
        memcpy(Value, factory, fsize);
        SecureZeroMemory(factory, sizeof factory);
        if (ResultLength) *ResultLength = fsize;
        return XSTATUS_SUCCESS;
    }
    logf("[k] ExQueryNonVolatileSetting(0x%x, len %u) -> %s 0x%x", ValueIndex, ValueLength, known ? "" : "UNKNOWN, ", dword);
    if (!known) return XSTATUS_OBJECT_NAME_NOT_FOUND;
    if (ValueLength < size) return XSTATUS_BUFFER_TOO_SMALL;
    if (Type) *Type = type;
    memset(Value, 0, size);
    if (type == 4) memcpy(Value, &dword, 4);
    if (ResultLength) *ResultLength = size;
    return XSTATUS_SUCCESS;
}

// --- HAL ---

uint32_t __stdcall HalGetInterruptVector(uint32_t BusInterruptLevel, uint8_t *Irql)
{
    KTRACE("%u, %p", BusInterruptLevel, Irql);
    if (Irql) *Irql = (uint8_t)(BusInterruptLevel < 27 ? 27 - BusInterruptLevel : 1);
    return BusInterruptLevel + 0x30;
}

void __stdcall HalReadWritePCISpace(uint32_t BusNumber, uint32_t SlotNumber, uint32_t RegisterNumber, void *Buffer,
                                    uint32_t Length, BOOLEAN WritePCISpace)
{
    KTRACE("bus %u, slot 0x%x, reg 0x%x, %p, %u, write %u", BusNumber, SlotNumber, RegisterNumber, Buffer, Length, WritePCISpace);
    if (!WritePCISpace && Buffer) memset(Buffer, 0, Length);
}

void __stdcall HalRegisterShutdownNotification(void *ShutdownRegistration, BOOLEAN Register)
{
    KTRACE("%p, %u", ShutdownRegistration, Register);
}

// --- XLaunchNewImage: Ninja Gaiden Black starts its bundled programs (NinjaTomoe / NinjaTrilogy / NinjaFCJ / NinjaFCU)
// and they start it again. XAPI fills the launch data page (type, title, szLaunchPath at +8 = "<directory>;<file>" with
// the directory a device path, flags at +0x210, the title's data at +0x400) and asks for a quick reboot. Here the
// loader starts itself again on the target XBE with the page (--launch-data) and this process ends.

void load_launch_data(const std::wstring &file)
{
    static XLAUNCH_DATA_PAGE page;
    FILE *f = _wfopen(file.c_str(), L"rb");
    size_t n = f ? fread(&page, 1, sizeof page, f) : 0;
    if (f) fclose(f);
    if (n != sizeof page) {
        logf("[k] launch data %s: could not read 4096 bytes (got %zu); starting without it", narrow(file).c_str(), n);
        return;
    }
    LaunchDataPage = &page;
    const uint32_t *ld = (const uint32_t *)&page;
    logf("[k] launch data from %s: type %u, title %08X, path '%.64s', flags 0x%x", narrow(file).c_str(), ld[0], ld[1],
         (const char *)page.Header + 8, ld[0x210 / 4]);
}

// The launch path as a host file: "\\Device\\CdRom0\\dir;file.xbe" or "D:\\dir;file.xbe" -> game folder\\dir\\file.xbe.
static std::wstring launch_target(const char *path)
{
    std::string p(path, strnlen(path, 520));
    if (p.empty()) return L"";
    for (char &c : p)
        if (c == ';') c = '\\';
    std::string lower = p;
    for (char &c : lower) c = (char)tolower((unsigned char)c);
    std::string rest;
    if (lower.rfind("\\device\\cdrom0", 0) == 0) rest = p.substr(14);
    else if (lower.rfind("d:", 0) == 0) rest = p.substr(2);
    else return L"";
    while (!rest.empty() && rest[0] == '\\') rest.erase(0, 1);
    return g_cfg.game_dir + L"\\" + widen(rest);
}

// with_data: pass the launch data page on (XLaunchNewImage); false = a fresh start (back from the dashboard)
static bool relaunch(const std::wstring &xbe, bool with_data = true)
{
    // the page beside the log, the log renamed after the target ("black.log" -> "black.NinjaFCJ.log")
    std::wstring log = g_cfg.log_path, dir = L".", stem = log;
    size_t slash = log.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        dir = log.substr(0, slash);
        stem = log.substr(slash + 1);
    }
    if (stem.size() > 4 && _wcsicmp(stem.c_str() + stem.size() - 4, L".log") == 0) stem.resize(stem.size() - 4);
    std::wstring cur = g_cfg.xbe_path.substr(g_cfg.xbe_path.find_last_of(L"\\/") + 1);
    cur = cur.substr(0, cur.find_last_of(L'.'));
    if (stem.size() > cur.size() + 1 && stem.compare(stem.size() - cur.size() - 1, std::wstring::npos, L"." + cur) == 0)
        stem.resize(stem.size() - cur.size() - 1);
    std::wstring target = xbe.substr(xbe.find_last_of(L"\\/") + 1);
    target = target.substr(0, target.find_last_of(L'.'));
    std::wstring page_file = dir + L"\\" + stem + L"." + target + L".launchdata";
    if (with_data) {
        FILE *f = _wfopen(page_file.c_str(), L"wb");
        if (!f) return false;
        fwrite(LaunchDataPage, 1, sizeof(XLAUNCH_DATA_PAGE), f);
        fclose(f);
    }
    std::wstring new_log =
        dir + L"\\" + stem + (target == L"default" ? (with_data ? L"" : L".restart") : L"." + target) + L".log";

    std::vector<std::wstring> args = g_cfg.args;
    for (size_t i = 0; i < args.size(); i++) {
        // the scripted presses belong to the program they were written for
        if ((args[i] == L"--log" || args[i] == L"--launch-data" || args[i] == L"--press") && i + 1 < args.size()) {
            args.erase(args.begin() + i, args.begin() + i + 2);
            if ((int)i < g_cfg.xbe_arg) g_cfg.xbe_arg -= 2;
            i--;
        }
    }
    if (g_cfg.xbe_arg >= 0 && g_cfg.xbe_arg < (int)args.size()) args[g_cfg.xbe_arg] = xbe;
    else args.push_back(xbe);
    args.insert(args.begin(), {L"--log", new_log});
    if (with_data) args.insert(args.begin() + 2, {L"--launch-data", page_file});
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    for (const std::wstring &a : args) cmd += L" \"" + a + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        logf("[k] relaunch failed (error %lu): %s", GetLastError(), narrow(cmd).c_str());
        return false;
    }
    AllowSetForegroundWindow(pi.dwProcessId); // this window is in front: the next program's may come to the front too
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logf("[k] relaunched as process %lu on %s (log %s)", pi.dwProcessId, narrow(xbe).c_str(), narrow(new_log).c_str());
    return true;
}

// The Xbox's own programs a game may launch to reach the dashboard: Ninja Gaiden 2004 starts the disc's dashupdate.xbe
// (the dashboard updater, then its network settings) when Xbox Live cannot be reached (r701). (Its update.xbe, the
// title updater, runs: build_profile.h kProfileNg04Updater.)
static bool system_program(const std::wstring &xbe)
{
    std::wstring f = xbe.substr(xbe.find_last_of(L"\\/") + 1);
    for (const wchar_t *n : {L"dashupdate.xbe", L"xonlinedash.xbe", L"xboxdash.xbe"})
        if (!_wcsicmp(f.c_str(), n)) return true;
    return false;
}

void __stdcall HalReturnToFirmware(uint32_t Routine)
{
    logf("[k] HalReturnToFirmware(%u): the game asked to leave (0 halt, 1 reboot, 2 quick reboot, 3 fatal error)", Routine);
    if (LaunchDataPage && (Routine == 1 || Routine == 2)) {
        const char *path = (const char *)LaunchDataPage->Header + 8;
        std::wstring xbe = launch_target(path);
        logf("    launch path '%.64s' -> %s", path, xbe.empty() ? "(no XBE)" : narrow(xbe).c_str());
        // No note on the PC: the boxes that lead there answer Ⓐ as Ⓑ (ngb_livedash.cpp), so this is only a way left
        // open; it goes back to the game as the dashboard would (the user, 2026-11-01: as the Gamertag boxes do)
        if (!xbe.empty() && system_program(xbe)) {
            logf("    %s is the Xbox's own (the dashboard's network settings): the game afresh instead", narrow(xbe).c_str());
            xbe.clear(); // the dashboard: handled below
        }
        if (!xbe.empty() && GetFileAttributesW(xbe.c_str()) != INVALID_FILE_ATTRIBUTES && relaunch(xbe)) {
            request_exit(0, "XLaunchNewImage");
            Sleep(INFINITE);
        }
    }
    if (Routine == 1 || Routine == 2) {
        // No title to launch: the console goes to the dashboard (Ninja Gaiden Black's Ranking > "new user" asks for
        // the Xbox Live dashboard: it looks for dashupdate.xbe and xonlinedash.xbe, then reboots with an empty path).
        // There is no dashboard here; coming back from it starts the game afresh, so start default.xbe without data.
        std::wstring main_xbe = g_cfg.game_dir + L"\\default.xbe";
        logf("    no title to launch (the dashboard): starting %s afresh", narrow(main_xbe).c_str());
        if (GetFileAttributesW(main_xbe.c_str()) != INVALID_FILE_ATTRIBUTES && relaunch(main_xbe, false)) {
            request_exit(0, "back from the dashboard");
            Sleep(INFINITE);
        }
    }
    if (LaunchDataPage) {
        const uint32_t *ld = (const uint32_t *)LaunchDataPage;
        logf("    launch data page %p: type %u, title %08X, flags 0x%x; data %08x %08x %08x", LaunchDataPage, ld[0], ld[1], ld[2],
             ld[0x100 / 4], ld[0x104 / 4], ld[0x108 / 4]);
    }
    log_stack_trace((const uint32_t *)_AddressOfReturnAddress());
    request_exit(Routine == 3 ? 1 : 0, "HalReturnToFirmware");
    Sleep(INFINITE);
}

void __stdcall HalInitiateShutdown()
{
    logf("[k] HalInitiateShutdown()");
    request_exit(0, "HalInitiateShutdown");
    Sleep(INFINITE);
}

void __fastcall IofCompleteRequest(void *Irp, char PriorityBoost) { KTRACE("%p, %d", Irp, PriorityBoost); }

void __stdcall KeBugCheck(uint32_t BugCheckCode)
{
    logf("[k] KeBugCheck(0x%08x)", BugCheckCode);
    log_stack_trace((const uint32_t *)_AddressOfReturnAddress());
    request_exit(4, "KeBugCheck");
    Sleep(INFINITE);
}

// --- interrupts: recorded, never delivered (GPU/APU interrupts belong to the libraries being replaced) ---

void __stdcall KeInitializeInterrupt(XKINTERRUPT *Interrupt, void *ServiceRoutine, void *ServiceContext, uint32_t Vector,
                                     uint8_t Irql, uint32_t InterruptMode, BOOLEAN ShareVector)
{
    KTRACE("%p, %s, %p, 0x%x, %u, %u, %u", Interrupt, sym_name((uint32_t)(uintptr_t)ServiceRoutine).c_str(), ServiceContext, Vector,
           Irql, InterruptMode, ShareVector);
    Interrupt->ServiceRoutine = ServiceRoutine;
    Interrupt->ServiceContext = ServiceContext;
    Interrupt->BusInterruptLevel = Vector - 0x30;
    Interrupt->Irql = Irql;
    Interrupt->Mode = (uint8_t)InterruptMode;
    Interrupt->ShareVector = ShareVector;
    Interrupt->Connected = 0;
    Interrupt->ServiceCount = 0;
}

BOOLEAN __stdcall KeConnectInterrupt(XKINTERRUPT *Interrupt)
{
    if (nvnet_connect_interrupt(Interrupt)) { // online play: the NIC's interrupt is raised by its model (net/nvnet.cpp)
        if (Interrupt->Connected) return FALSE;
        Interrupt->Connected = 1;
        return TRUE;
    }
    logf("[k] KeConnectInterrupt(%p, level %u, isr %s): recorded, will never fire", Interrupt, Interrupt->BusInterruptLevel,
         sym_name((uint32_t)(uintptr_t)Interrupt->ServiceRoutine).c_str());
    if (Interrupt->Connected) return FALSE;
    Interrupt->Connected = 1;
    return TRUE;
}

BOOLEAN __stdcall KeDisconnectInterrupt(XKINTERRUPT *Interrupt)
{
    KTRACE("%p", Interrupt);
    bool was = Interrupt->Connected != 0;
    Interrupt->Connected = 0;
    return was;
}

BOOLEAN __stdcall KeSynchronizeExecution(XKINTERRUPT *Interrupt, void *SynchronizeRoutine, void *SynchronizeContext)
{
    KTRACE("%p, %s, %p", Interrupt, sym_name((uint32_t)(uintptr_t)SynchronizeRoutine).c_str(), SynchronizeContext);
    uint8_t old = xthread_raise_irql(Interrupt ? (uint8_t)Interrupt->Irql : 2);
    BOOLEAN r = ((BOOLEAN(__stdcall *)(void *))SynchronizeRoutine)(SynchronizeContext);
    xthread_lower_irql(old);
    return r;
}

// --- time, FPU ---

void __stdcall KeQuerySystemTime(LARGE_INTEGER *CurrentTime)
{
    KTRACE("%p", CurrentTime);
    CurrentTime->QuadPart = ktime_now_100ns();
}

NTSTATUS __stdcall KeSaveFloatingPointState(void *FloatSave)
{
    KTRACE("%p", FloatSave);
    return XSTATUS_SUCCESS; // user-mode threads keep their own FPU state on Windows
}

NTSTATUS __stdcall KeRestoreFloatingPointState(void *FloatSave)
{
    KTRACE("%p", FloatSave);
    return XSTATUS_SUCCESS;
}

// --- XBE sections ---

NTSTATUS __stdcall XeLoadSection(void *Section) { return image_load_section((XbeSectionHeader *)Section); }

NTSTATUS __stdcall XeUnloadSection(void *Section) { return image_unload_section((XbeSectionHeader *)Section); }

// --- crypto ---

void __stdcall XcSHAInit(uint8_t *Context)
{
    KTRACE("%p", Context);
    sha1_init((Sha1Ctx *)Context);
}

void __stdcall XcSHAUpdate(uint8_t *Context, const uint8_t *Input, uint32_t InputLength)
{
    KTRACE("%p, %p, %u", Context, Input, InputLength);
    sha1_update((Sha1Ctx *)Context, Input, InputLength);
}

void __stdcall XcSHAFinal(uint8_t *Context, uint8_t *Digest)
{
    KTRACE("%p, %p", Context, Digest);
    sha1_final((Sha1Ctx *)Context, Digest);
}

void __stdcall XcHMAC(const uint8_t *Key, uint32_t KeyLength, const uint8_t *Data, uint32_t DataLength, const uint8_t *Data2,
                      uint32_t Data2Length, uint8_t *Digest)
{
    KTRACE("%p, %u, %p, %u, %p, %u, %p", Key, KeyLength, Data, DataLength, Data2, Data2Length, Digest);
    hmac_sha1(Key, KeyLength, Data, DataLength, Data2, Data2Length, Digest);
}

} // namespace xk
