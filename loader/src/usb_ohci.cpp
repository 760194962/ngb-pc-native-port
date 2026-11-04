// The Xbox's first USB host controller (OHCI 1.0a at 0xFED00000) with an original Xbox controller (the "Duke", an XID
// device) on root-hub port 1, for programs that drive USB themselves (nxdk's usbh and xid drivers: the Insignia Setup
// Assistant). The games' XInput is HLE'd and never touches it; installed only for a program no build profile matches.
//
// Registers trap like the NIC's (mmio_decode.cpp). A 1 ms frame thread walks the HCCA's interrupt table and the control
// and bulk lists: endpoint descriptors (ED) with general transfer descriptors (TD), retired TDs go to the done queue,
// written back to HccaDoneHead with WDH. No isochronous transfers. The controller's interrupt (bus level 1) runs the
// driver's service routine at its IRQL from the frame thread. Behaviour per the OHCI 1.0a specification; the XID
// descriptors and report as xemu's hw/xbox/xid.c (LGPL 2+) and the console's controller.
#include "usb_ohci.h"

#include "common.h"
#include "kernel/xboxkrnl.h"
#include "mmio_decode.h"
#include "xthread.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace xinput {
void pad_report(uint8_t out[18]); // input/xinput.cpp
}
void __stdcall usb_thread(void *);

namespace {

constexpr uint32_t REG_BASE = 0xFED00000, REG_PAGE = 0x1000, PORTS = 2;
enum : uint32_t {
    HcRevision = 0x00, HcControl = 0x04, HcCommandStatus = 0x08, HcInterruptStatus = 0x0C, HcInterruptEnable = 0x10,
    HcInterruptDisable = 0x14, HcHCCA = 0x18, HcPeriodCurrentED = 0x1C, HcControlHeadED = 0x20, HcControlCurrentED = 0x24,
    HcBulkHeadED = 0x28, HcBulkCurrentED = 0x2C, HcDoneHead = 0x30, HcFmInterval = 0x34, HcFmRemaining = 0x38,
    HcFmNumber = 0x3C, HcPeriodicStart = 0x40, HcLSThreshold = 0x44, HcRhDescriptorA = 0x48, HcRhDescriptorB = 0x4C,
    HcRhStatus = 0x50, HcRhPortStatus = 0x54,
};
// HcControl
constexpr uint32_t CTL_PLE = 1 << 2, CTL_IE = 1 << 3, CTL_CLE = 1 << 4, CTL_BLE = 1 << 5, CTL_HCFS = 3 << 6, HCFS_OPER = 2 << 6;
// HcCommandStatus
constexpr uint32_t CMD_HCR = 1 << 0, CMD_CLF = 1 << 1, CMD_BLF = 1 << 2;
// interrupts
constexpr uint32_t INT_SO = 1 << 0, INT_WDH = 1 << 1, INT_SF = 1 << 2, INT_RD = 1 << 3, INT_FNO = 1 << 5, INT_RHSC = 1 << 6,
                   INT_MIE = 1u << 31;
// port status
constexpr uint32_t PS_CCS = 1 << 0, PS_PES = 1 << 1, PS_PSS = 1 << 2, PS_PRS = 1 << 4, PS_PPS = 1 << 8, PS_LSDA = 1 << 9,
                   PS_CSC = 1 << 16, PS_PESC = 1 << 17, PS_PSSC = 1 << 18, PS_PRSC = 1 << 20;
// TD condition codes
constexpr uint32_t CC_NOERROR = 0, CC_STALL = 4, CC_NOTRESPONDING = 5, CC_DATAUNDERRUN = 9;

// --- the controller (Duke): an XID device -------------------------------------------------------------------------

const uint8_t kDeviceDesc[18] = {18, 1, 0x10, 0x01, 0, 0, 0, 0x40, 0x5E, 0x04, 0x02, 0x02, 0x00, 0x01, 0, 0, 0, 1};
const uint8_t kConfigDesc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,              // configuration: 1 interface, bus powered, 100 mA
    9, 4, 0, 0, 2, 0x58, 0x42, 0, 0,             // interface: XID class 0x58, subclass 0x42, 2 endpoints
    7, 5, 0x82, 3, 0x20, 0, 4,                   // endpoint 2 IN, interrupt, 32 bytes, every 4 ms
    7, 5, 0x02, 3, 0x20, 0, 4,                   // endpoint 2 OUT, interrupt (rumble)
};
const uint8_t kXidDesc[16] = {16, 0x42, 0x00, 0x01, 1, 1, 20, 6, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct Device {
    uint8_t address = 0, pending_address = 0;
    bool configured = false, address_pending = false;
    std::vector<uint8_t> reply; // the data stage of the current control transfer (device to host)
    size_t reply_pos = 0;
    bool stalled = false;
    uint8_t last_report[20] = {};
} g_dev;

// A SETUP packet: prepare the data stage. false = STALL (a request the controller does not know)
bool setup(const uint8_t *s)
{
    uint8_t type = s[0], req = s[1];
    uint16_t value = (uint16_t)(s[2] | s[3] << 8), length = (uint16_t)(s[6] | s[7] << 8);
    g_dev.reply.clear();
    g_dev.reply_pos = 0;
    static int logged;
    if (logged++ < 40) logf("[usb] controller: setup type 0x%02x request %u value 0x%04x index 0x%04x length %u", type, req, value,
                            (uint16_t)(s[4] | s[5] << 8), length);
    auto give = [&](const uint8_t *d, size_t n) { g_dev.reply.assign(d, d + (n < length ? n : length)); };
    if (type == 0x80 && req == 6) { // GET_DESCRIPTOR
        uint8_t kind = value >> 8;
        if (kind == 1) give(kDeviceDesc, sizeof kDeviceDesc);
        else if (kind == 2) give(kConfigDesc, sizeof kConfigDesc);
        else return false; // no strings
        return true;
    }
    if (type == 0x00 && req == 5) { // SET_ADDRESS: takes effect after the status stage
        g_dev.pending_address = (uint8_t)(value & 0x7F);
        g_dev.address_pending = true;
        return true;
    }
    if (type == 0x00 && req == 9) { // SET_CONFIGURATION
        g_dev.configured = value != 0;
        return true;
    }
    if (type == 0x80 && req == 0) { // GET_STATUS
        static const uint8_t st[2] = {0, 0};
        give(st, 2);
        return true;
    }
    if (type == 0x80 && req == 8) { // GET_CONFIGURATION
        uint8_t c = g_dev.configured ? 1 : 0;
        give(&c, 1);
        return true;
    }
    if ((type == 0x01 && req == 11) || (type == 0x02 && req == 1)) return true; // SET_INTERFACE, CLEAR_FEATURE (endpoint)
    if (type == 0xC1 && req == 6 && value == 0x4200) { // XID descriptor
        give(kXidDesc, sizeof kXidDesc);
        return true;
    }
    if (type == 0xC1 && req == 1) { // XID GET_CAPABILITIES: input (0x0100) / output (0x0200)
        static const uint8_t in[20] = {0, 20, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        static const uint8_t out[6] = {0, 6, 0xFF, 0xFF, 0xFF, 0xFF};
        if (value == 0x0100) give(in, sizeof in);
        else if (value == 0x0200) give(out, sizeof out);
        else return false;
        return true;
    }
    if (type == 0xA1 && req == 1) { // HID-style GET_REPORT: the current input report
        uint8_t r[20] = {0, 20};
        xinput::pad_report(r + 2);
        give(r, sizeof r);
        return true;
    }
    if (type == 0x21 && req == 9) return true; // SET_REPORT (rumble): accepted, no motors here
    static int unknown;
    if (unknown++ < 16) logf("[usb] controller: request type 0x%02x %u value 0x%04x length %u not handled (STALL)", type, req, value, length);
    return false;
}

// --- the host controller ------------------------------------------------------------------------------------------

std::mutex g_m;
uint32_t g_regs[0x60 / 4];
uint32_t g_port[PORTS];
uint32_t g_done; // the done queue (TD physical addresses linked by NextTD), written back at a frame's end
XKINTERRUPT *g_irq;
bool g_installed, g_started;
uint32_t g_frame; // 16-bit frame number

uint32_t &reg(uint32_t r) { return g_regs[r / 4]; }

// A DMA address: contiguous memory (physical = virtual - 0x80000000) or other memory, handed out with the top bit set
// (kernel/k_memory.cpp MmGetPhysicalAddress for programs without a profile)
uint8_t *mem(uint32_t pa)
{
    if (!pa) return nullptr;
    return (uint8_t *)(uintptr_t)(pa & 0x80000000u ? pa & 0x7FFFFFFFu : 0x80000000u | pa);
}
uint32_t rd32(uint32_t pa) { uint32_t v; memcpy(&v, mem(pa), 4); return v; }
void wr32(uint32_t pa, uint32_t v) { memcpy(mem(pa), &v, 4); }

void reset_controller()
{
    memset(g_regs, 0, sizeof g_regs);
    reg(HcRevision) = 0x10;
    reg(HcFmInterval) = 0x2EDF | (0x2778u << 16);
    reg(HcLSThreshold) = 0x628;
    reg(HcRhDescriptorA) = PORTS | (1 << 9) /* no power switching */ | (1 << 12) /* no overcurrent */ | (1u << 24);
    for (uint32_t p = 0; p < PORTS; p++) g_port[p] = PS_PPS;
    g_port[0] |= PS_CCS | PS_CSC; // the controller is plugged into port 1
    g_dev = Device();
    g_done = 0;
}

// Move a data stage through a TD's buffer (CBP..BE, which may cross one page boundary)
uint32_t td_copy(uint32_t cbp, uint32_t be, uint8_t *data, uint32_t n, bool to_memory)
{
    uint32_t total = cbp ? be - cbp + 1 : 0;
    if ((cbp ^ be) & ~0xFFFu) total = (0x1000 - (cbp & 0xFFF)) + (be & 0xFFF) + 1;
    if (n > total) n = total;
    uint32_t first = (cbp ^ be) & ~0xFFFu ? 0x1000 - (cbp & 0xFFF) : n;
    if (first > n) first = n;
    if (to_memory) {
        memcpy(mem(cbp), data, first);
        if (n > first) memcpy(mem(be & ~0xFFFu), data + first, n - first);
    } else {
        memcpy(data, mem(cbp), first);
        if (n > first) memcpy(data + first, mem(be & ~0xFFFu), n - first);
    }
    return n;
}

// One TD on an ED. Returns false when the ED must not go on this frame (NAK, halted or empty).
bool process_td(uint32_t ed)
{
    uint32_t e0 = rd32(ed), tail = rd32(ed + 4) & ~0xFu, head = rd32(ed + 8);
    uint32_t td = head & ~0xFu;
    if ((e0 & (1 << 14)) || (head & 1) || td == tail) return false; // skip, halted, empty
    uint32_t t0 = rd32(td), cbp = rd32(td + 4), next = rd32(td + 8), be = rd32(td + 12);
    uint32_t fa = e0 & 0x7F, en = (e0 >> 7) & 0xF, ed_dir = (e0 >> 11) & 3, mps = (e0 >> 16) & 0x7FF;
    uint32_t dp = ed_dir == 1 ? 1 : ed_dir == 2 ? 2 : (t0 >> 19) & 3; // 0 SETUP, 1 OUT, 2 IN
    uint32_t len = cbp ? ((cbp ^ be) & ~0xFFFu ? (0x1000 - (cbp & 0xFFF)) + (be & 0xFFF) + 1 : be - cbp + 1) : 0;
    bool rounding = (t0 >> 18) & 1;
    uint32_t cc = CC_NOERROR, done_bytes = len;
    bool present = (g_port[0] & PS_PES) && fa == g_dev.address;
    static int td_logged;
    if (en != 0 && td_logged++ < 10) logf("[usb] TD on device %u endpoint %u direction %u, %u bytes, rounding %u, td0 0x%08x ed head 0x%08x", fa, en, dp, len, rounding ? 1 : 0, t0, head);
    if (!present) {
        cc = CC_NOTRESPONDING;
    } else if (en == 0) { // control endpoint
        if (dp == 0) {
            uint8_t s[8] = {};
            td_copy(cbp, be, s, 8, false);
            g_dev.stalled = !setup(s);
            if (g_dev.stalled) cc = CC_STALL;
        } else if (g_dev.stalled) {
            cc = CC_STALL;
        } else if (dp == 2) { // data IN (or the status stage of a host-to-device request)
            uint32_t left = (uint32_t)(g_dev.reply.size() - g_dev.reply_pos);
            uint32_t n = left < len ? left : len;
            if (n) td_copy(cbp, be, g_dev.reply.data() + g_dev.reply_pos, n, true);
            g_dev.reply_pos += n;
            done_bytes = n;
            if (n < len && !rounding) cc = CC_DATAUNDERRUN;
            if (len == 0 && g_dev.address_pending) g_dev.address = g_dev.pending_address, g_dev.address_pending = false;
        } else { // OUT: a data stage we ignore, or the status stage of a device-to-host request
            if (len == 0 && g_dev.address_pending) g_dev.address = g_dev.pending_address, g_dev.address_pending = false;
        }
    } else if (en == 2 && dp == 2) { // the input report
        uint8_t r[20] = {0, 20};
        xinput::pad_report(r + 2);
        if (!memcmp(r, g_dev.last_report, 20) && g_dev.configured && false) return false; // (reports always; NAK unused)
        memcpy(g_dev.last_report, r, 20);
        uint32_t n = len < 20 ? len : 20;
        td_copy(cbp, be, r, n, true);
        done_bytes = n;
        if (n < len && !rounding) cc = CC_DATAUNDERRUN;
    } else if (en == 2 && dp == 1) { // rumble: accepted
    } else {
        cc = CC_STALL;
    }
    (void)mps;
    // retire the TD: condition code, the buffer pointer (0 when all moved), toggle, to the done queue
    uint32_t toggle = (t0 >> 24) & 2 ? (t0 >> 24) & 1 : (head >> 1) & 1;
    toggle ^= 1;
    t0 = (t0 & ~(0xFu << 28) & ~(3u << 26) & ~(3u << 24)) | (cc << 28) | (2u << 24) | (toggle << 24);
    wr32(td, t0);
    wr32(td + 4, cc == CC_NOERROR && done_bytes == len ? 0 : cc == CC_NOERROR || cc == CC_DATAUNDERRUN ? cbp + done_bytes : cbp);
    wr32(td + 8, g_done);
    g_done = td;
    bool halt = cc != CC_NOERROR;
    wr32(ed + 8, (next & ~0xFu) | (toggle << 1) | (halt ? 1 : 0));
    return !halt;
}

// The EDs of a list: each gets its TDs processed (a few per frame)
void process_list(uint32_t first, int max_per_ed)
{
    for (uint32_t ed = first, guard = 0; ed && guard < 64; ed = rd32(ed + 12) & ~0xFu, guard++)
        for (int i = 0; i < max_per_ed && process_td(ed); i++) {
        }
}

// One 1 ms frame. Returns true when the interrupt line is asserted.
bool frame()
{
    if ((reg(HcControl) & CTL_HCFS) != HCFS_OPER) return false;
    uint32_t hcca = reg(HcHCCA);
    g_frame = (g_frame + 1) & 0xFFFF;
    reg(HcFmNumber) = g_frame;
    if (hcca) {
        uint32_t fn = g_frame;
        memcpy(mem(hcca + 0x80), &fn, 2);
        memset(mem(hcca + 0x82), 0, 2);
    }
    reg(HcInterruptStatus) |= INT_SF;
    if (g_frame == 0 || g_frame == 0x8000) reg(HcInterruptStatus) |= INT_FNO;
    if (hcca && (reg(HcControl) & CTL_PLE)) process_list(rd32(hcca + 4 * (g_frame & 31)) & ~0xFu, 1);
    if ((reg(HcControl) & CTL_CLE) && (reg(HcCommandStatus) & CMD_CLF)) {
        reg(HcCommandStatus) &= ~CMD_CLF;
        process_list(reg(HcControlHeadED), 8);
    }
    if ((reg(HcControl) & CTL_BLE) && (reg(HcCommandStatus) & CMD_BLF)) {
        reg(HcCommandStatus) &= ~CMD_BLF;
        process_list(reg(HcBulkHeadED), 8);
    }
    if (g_done && hcca && !(reg(HcInterruptStatus) & INT_WDH)) { // the done queue goes back when the driver took the last
        uint32_t dh = g_done | ((reg(HcInterruptStatus) & reg(HcInterruptEnable) & ~INT_WDH) ? 1 : 0);
        wr32(hcca + 0x84, dh);
        static int dh_logged;
        if (dh_logged++ < 40) logf("[usb] frame %u: done head 0x%08x written, status 0x%08x enable 0x%08x", g_frame, dh,
                                   reg(HcInterruptStatus), reg(HcInterruptEnable));
        g_done = 0;
        reg(HcInterruptStatus) |= INT_WDH;
    }
    uint32_t en = reg(HcInterruptEnable);
    return (en & INT_MIE) && (reg(HcInterruptStatus) & en & ~INT_MIE);
}

uint32_t rd(uint32_t off, int size)
{
    std::lock_guard<std::mutex> g(g_m);
    uint32_t a = off & ~3u, v = 0;
    static int reads;
    if (reads++ < 60) logf("[usb] read 0x%02x", off);
    if (a >= HcRhPortStatus && a < HcRhPortStatus + 4 * PORTS) v = g_port[(a - HcRhPortStatus) / 4];
    else if (a < sizeof g_regs) v = g_regs[a / 4];
    if (a == HcInterruptDisable) v = reg(HcInterruptEnable);
    if (a == HcFmRemaining) v = 0x2000;
    v >>= (off & 3) * 8;
    return size == 1 ? v & 0xFF : size == 2 ? v & 0xFFFF : v;
}

void wr(uint32_t off, uint32_t v, int size)
{
    if (size != 4) v <<= (off & 3) * 8;
    std::lock_guard<std::mutex> g(g_m);
    uint32_t a = off & ~3u;
    static int writes;
    if (writes++ < 60 || (a == HcControl && writes < 400)) logf("[usb] write 0x%02x = 0x%08x", off, v);
    if (a >= HcRhPortStatus && a < HcRhPortStatus + 4 * PORTS) {
        uint32_t &p = g_port[(a - HcRhPortStatus) / 4];
        p &= ~(v & (PS_CSC | PS_PESC | PS_PSSC | PS_PRSC | (1 << 19))); // write 1 to clear the change bits
        if (v & 0x1) p &= ~PS_PES;                                          // ClearPortEnable
        if ((v & 0x2) && (p & PS_CCS)) p |= PS_PES;                         // SetPortEnable
        static int port_writes;
        if (port_writes++ < 20) logf("[usb] port %u write 0x%08x (status 0x%08x)", (a - HcRhPortStatus) / 4 + 1, v, p);
        if ((v & 0x10) && (p & PS_CCS)) {                                   // SetPortReset: done at once
            p = (p & ~PS_PRS) | PS_PES | PS_PRSC;
            g_dev.address = 0, g_dev.configured = false;
            reg(HcInterruptStatus) |= INT_RHSC;
        }
        if (v & 0x100) p |= PS_PPS;
        return;
    }
    switch (a) {
    case HcControl: {
        uint32_t old = reg(HcControl);
        reg(HcControl) = v;
        if ((old & CTL_HCFS) != (v & CTL_HCFS))
            logf("[usb] host controller state %u -> %u", (old >> 6) & 3, (v >> 6) & 3);
        return;
    }
    case HcCommandStatus:
        if (v & CMD_HCR) {
            reset_controller();
            logf("[usb] host controller reset");
            return;
        }
        reg(HcCommandStatus) |= v;
        return;
    case HcInterruptStatus: reg(a) &= ~v; return;
    case HcInterruptEnable: reg(a) |= v; return;
    case HcInterruptDisable: reg(HcInterruptEnable) &= ~v; return;
    case HcRhStatus:
        if (v & (1u << 17)) reg(a) &= ~(1u << 17); // OCIC
        return;
    case HcRevision: case HcFmRemaining: case HcFmNumber: case HcRhDescriptorA: return;
    default:
        if (a < sizeof g_regs) g_regs[a / 4] = v;
    }
}

LONG CALLBACK trap(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t addr = (uint32_t)er->ExceptionInformation[1];
    if (addr - REG_BASE >= REG_PAGE) return EXCEPTION_CONTINUE_SEARCH;
    if (!g_started) {
        g_started = true;
        kthread_create(nullptr, nullptr, 0, 0, (uint32_t)(uintptr_t)&usb_thread, 0, 0, false, "usb-ohci");
    }
    static const MmioOps ops = {rd, wr};
    if (mmio_decode(ep->ContextRecord, addr - REG_BASE, ops)) return EXCEPTION_CONTINUE_EXECUTION;
    const uint8_t *ip = (const uint8_t *)(uintptr_t)ep->ContextRecord->Eip;
    logf("[usb] register access at %08lx (address %08x) not decoded: %02x %02x %02x %02x %02x %02x", ep->ContextRecord->Eip, addr,
         ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

// The frame thread: 1 ms frames (caught up after a late wake-up, at most 8 at once), the interrupt when asserted
void __stdcall usb_thread(void *)
{
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -10000;
    SetWaitableTimer(timer, &due, 1, nullptr, nullptr, FALSE);
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    uint64_t frames = 0;
    for (;;) {
        WaitForSingleObject(timer, 2);
        QueryPerformanceCounter(&t);
        uint64_t due_frames = (uint64_t)((t.QuadPart - t0.QuadPart) * 1000 / f.QuadPart);
        if (due_frames - frames > 8) frames = due_frames - 8;
        bool fire = false;
        {
            std::lock_guard<std::mutex> g(g_m);
            for (; frames < due_frames; frames++) fire |= frame();
            uint32_t en = reg(HcInterruptEnable);
            fire = (en & INT_MIE) && (reg(HcInterruptStatus) & en & ~INT_MIE) && g_irq;
        }
        // Level-triggered: while the line is asserted the service routine runs again (it deasserts it, clearing the
        // status or masking MIE). Only on a rising edge, an interrupt asserted again by a transfer the routine itself
        // queued was lost and enumeration stopped at random (r934, r935).
        if (fire) {
            uint8_t old = xthread_raise_irql((uint8_t)g_irq->Irql);
            ((BOOLEAN(__stdcall *)(void *, void *))g_irq->ServiceRoutine)(g_irq, g_irq->ServiceContext);
            xthread_lower_irql(old);
        }
    }
}

void usb_ohci_install()
{
    if (g_cfg.profile) return;
    void *p = VirtualAlloc((void *)(uintptr_t)REG_BASE, REG_PAGE, MEM_RESERVE, PAGE_NOACCESS);
    if (p != (void *)(uintptr_t)REG_BASE) {
        logc("[usb] cannot reserve the USB controller's registers at 0x%08x (error %lu)", REG_BASE, GetLastError());
        return;
    }
    reset_controller();
    AddVectoredExceptionHandler(1, trap);
    g_installed = true;
    logf("[usb] USB host controller at 0x%08x traps to its model; an Xbox controller is on port 1", REG_BASE);
}

bool usb_ohci_connect_interrupt(void *kinterrupt)
{
    XKINTERRUPT *i = (XKINTERRUPT *)kinterrupt;
    if (!g_installed || i->BusInterruptLevel != 1) return false;
    std::lock_guard<std::mutex> g(g_m);
    g_irq = i;
    logf("[usb] controller interrupt connected (service routine 0x%08x, IRQL %u)", (uint32_t)(uintptr_t)i->ServiceRoutine, i->Irql);
    return true;
}
