// nForce Ethernet controller model and the MMIO trap (net/nvnet.h). Register semantics follow xemu's
// hw/xbox/mcpx/nvnet/nvnet.c (Copyright (c) 2013 espes, 2015-2025 Matt Borgerson; LGPL 2 or later), which runs this
// game online: descriptor rings of {buffer, length, flags}, TX on a kick of TX_RX_CONTROL, RX into the first
// descriptor the driver marked available, a level interrupt while IRQ_STATUS & IRQ_MASK.
//
// The driver (XNET, Ninja Gaiden Black 0x3B74D3-0x3B8527, project/docs/09) touches the registers with plain mov / test /
// or / and on 0xFEF00xxx; the trap decodes those forms, runs the access on the model and steps past the instruction.
// Its service routine (KeConnectInterrupt, bus level 4) is called from the network thread at its IRQL, which holds
// the dispatch lock like any code above PASSIVE_LEVEL here (xthread.cpp), so it never runs beside the driver's DPC.
#include "net/nvnet.h"

#include "common.h"
#include "mmio_decode.h"
#include "kernel/kexports.h"
#include "kernel/xboxkrnl.h"
#include "net/online.h"
#include "net/vnet.h"
#include "xthread.h"

#include <deque>
#include <mutex>
#include <vector>

namespace {

constexpr uint32_t MMIO_BASE = 0xFEF00000, MMIO_SIZE = 0x400, MMIO_PAGE = 0x1000;

enum : uint32_t {
    IRQ_STATUS = 0x000, IRQ_MASK = 0x004,
    TRANSMITTER_CONTROL = 0x084, RECEIVER_CONTROL = 0x094,
    PACKET_FILTER = 0x08C, MAC_ADDR_A = 0x0A8, MAC_ADDR_B = 0x0AC,
    TX_RING_PHYS_ADDR = 0x100, RX_RING_PHYS_ADDR = 0x104, RING_SIZE = 0x108,
    TX_RING_CURRENT = 0x11C, RX_RING_CURRENT = 0x120, SETUP_REG5 = 0x130,
    TX_RING_NEXT = 0x134, RX_RING_NEXT = 0x138, TX_RX_CONTROL = 0x144,
    MII_STATUS = 0x180, ADAPTER_CONTROL = 0x188, MDIO_ADDR = 0x190, MDIO_DATA = 0x194,
};
constexpr uint32_t IRQ_RX = 0x02, IRQ_TX = 0x10;
constexpr uint32_t TXRX_KICK = 0x01, TXRX_BIT1 = 0x02, TXRX_BIT2 = 0x04, TXRX_IDLE = 0x08, TXRX_RESET = 0x10;
constexpr uint16_t TX_LASTPACKET = 1 << 0, TX_VALID = 1 << 15, TX_ERRORS = (1 << 3) | (1 << 10) | (1 << 11) | (1 << 12) | (1 << 13) | (1 << 14);
constexpr uint16_t RX_DESCRIPTORVALID = 1 << 0, RX_BIT4 = 1 << 4, RX_AVAIL = 1 << 15;
constexpr uint32_t FRAME_MAX = 1500 + 128;

struct RingDesc {
    uint32_t buffer;
    uint16_t length;
    uint16_t flags;
};

std::mutex g_nic;           // the registers and the rings' state
uint8_t g_regs[MMIO_SIZE];
uint16_t g_phy[6] = {0x3100, 0x782D, 0, 0, 0x01E1, 0x45E1}; // BMCR FD|AUTOEN, BMSR link + autoneg done, ANAR, ANLPAR
std::vector<uint8_t> g_tx_buf;
std::deque<std::vector<uint8_t>> g_rx_pending; // frames for the guest waiting for a free RX descriptor
std::deque<std::vector<uint8_t>> g_tx_queue;   // frames from the guest for the network thread
XKINTERRUPT *g_interrupt;
bool g_started;
HANDLE g_wake;               // the network thread: frames to send, an interrupt to raise
uint64_t g_tx_frames, g_rx_frames, g_rx_dropped, g_isr_calls;

uint32_t reg(uint32_t a) { return *(uint32_t *)&g_regs[a]; }
void set(uint32_t a, uint32_t v) { *(uint32_t *)&g_regs[a] = v; }
bool irq_asserted() { return (reg(IRQ_STATUS) & reg(IRQ_MASK)) != 0; }

void raise_status(uint32_t bits)
{
    set(IRQ_STATUS, reg(IRQ_STATUS) | bits);
    if (irq_asserted()) SetEvent(g_wake);
}

// A DMA address as the driver got it from MmGetPhysicalAddress (k_memory.cpp): contiguous memory is physical =
// virtual - 0x80000000; other memory (XNET sends from pool buffers) comes back as the virtual address & 0x7FFFFFFF,
// which here is that address itself (the process's memory lies below 0x80000000 apart from the contiguous block).
uint8_t *phys(uint32_t pa, uint32_t len)
{
    if (pa < CONTIGUOUS_SIZE) return len <= CONTIGUOUS_SIZE - pa ? (uint8_t *)(uintptr_t)(CONTIGUOUS_BASE + pa) : nullptr;
    if (pa & 0x80000000u) { // XNET's pool buffers: MmGetPhysicalAddress gave it the address with the top bit set
        uint32_t va = pa & 0x7FFFFFFF;
        return len <= CONTIGUOUS_BASE - va ? (uint8_t *)(uintptr_t)va : nullptr;
    }
    if (len > CONTIGUOUS_BASE - pa) return nullptr;
    return (uint8_t *)(uintptr_t)pa;
}

bool load_desc(uint32_t pa, RingDesc &d)
{
    uint8_t *p = phys(pa, sizeof d);
    if (!p) return false;
    memcpy(&d, p, sizeof d);
    return true;
}

void store_desc(uint32_t pa, const RingDesc &d)
{
    if (uint8_t *p = phys(pa, sizeof d)) memcpy(p, &d, sizeof d);
}

uint32_t ring_size(bool rx) { return (rx ? reg(RING_SIZE) >> 16 : reg(RING_SIZE) & 0xFFFF) + 1; }

uint32_t current_desc(bool rx)
{
    uint32_t base = reg(rx ? RX_RING_PHYS_ADDR : TX_RING_PHYS_ADDR), max = base + ring_size(rx) * 8;
    uint32_t cur = reg(rx ? RX_RING_NEXT : TX_RING_NEXT);
    if (cur < base || cur + 8 > max) cur = base;
    set(rx ? RX_RING_CURRENT : TX_RING_CURRENT, cur);
    return cur;
}

void advance_desc(bool rx)
{
    uint32_t base = reg(rx ? RX_RING_PHYS_ADDR : TX_RING_PHYS_ADDR), max = base + ring_size(rx) * 8;
    uint32_t next = reg(rx ? RX_RING_CURRENT : TX_RING_CURRENT) + 8;
    set(rx ? RX_RING_NEXT : TX_RING_NEXT, next >= max ? base : next);
}

bool dma_enabled() { return !(reg(TX_RX_CONTROL) & TXRX_BIT2); }
void set_idle(bool idle) { set(TX_RX_CONTROL, idle ? reg(TX_RX_CONTROL) | TXRX_IDLE : reg(TX_RX_CONTROL) & ~TXRX_IDLE); }

void transmit() // caller holds g_nic
{
    if (!(reg(TRANSMITTER_CONTROL) & 1) || !dma_enabled()) return;
    set_idle(false);
    bool sent = false;
    for (uint32_t i = 0, n = ring_size(false); i < n; i++) {
        uint32_t at = current_desc(false);
        RingDesc d;
        if (!load_desc(at, d) || !(d.flags & TX_VALID)) break;
        uint32_t len = d.length + 1u;
        uint8_t *src = phys(d.buffer, len);
        if (src && g_tx_buf.size() + len <= FRAME_MAX) g_tx_buf.insert(g_tx_buf.end(), src, src + len);
        bool last = (d.flags & TX_LASTPACKET) != 0;
        if (last && g_cfg.online_unplugged) g_tx_buf.clear(); // no cable: the frame goes nowhere
        else if (last) {
            g_tx_queue.push_back(std::move(g_tx_buf));
            g_tx_buf.clear();
            g_tx_frames++;
            sent = true;
        }
        d.flags &= ~(TX_VALID | TX_ERRORS);
        store_desc(at, d);
        advance_desc(false);
        if (last) break;
    }
    set_idle(true);
    if (sent) {
        raise_status(IRQ_TX);
        SetEvent(g_wake);
    }
}

bool receive(const std::vector<uint8_t> &f) // caller holds g_nic; false = no free buffer now
{
    if (!(reg(RECEIVER_CONTROL) & 1) || !dma_enabled()) return false;
    uint32_t at = current_desc(true);
    RingDesc d;
    if (!load_desc(at, d) || !(d.flags & RX_AVAIL)) return false;
    uint8_t *dst = phys(d.buffer, (uint32_t)f.size());
    if (!dst || d.length + 1u < f.size()) {
        g_rx_dropped++;
        return true; // cannot take it: dropped, as an oversized frame on the wire would be
    }
    set_idle(false);
    memcpy(dst, f.data(), f.size());
    d.length = (uint16_t)f.size();
    d.flags = RX_BIT4 | RX_DESCRIPTORVALID;
    store_desc(at, d);
    advance_desc(true);
    set_idle(true);
    g_rx_frames++;
    raise_status(IRQ_RX);
    return true;
}

void flush_rx() // caller holds g_nic
{
    while (!g_rx_pending.empty() && receive(g_rx_pending.front())) g_rx_pending.pop_front();
}

void reset_rings()
{
    set(TX_RING_CURRENT, reg(TX_RING_PHYS_ADDR));
    set(TX_RING_NEXT, reg(TX_RING_PHYS_ADDR));
    set(RX_RING_CURRENT, reg(RX_RING_PHYS_ADDR));
    set(RX_RING_NEXT, reg(RX_RING_PHYS_ADDR));
    g_tx_buf.clear();
}

void __stdcall net_thread(void *);
bool g_unplugged_model; // nvnet_install_unplugged: the model without online play (nothing leaves the machine)
bool model_on() { return online_enabled() || g_unplugged_model; }

uint32_t mmio_read(uint32_t a, int size)
{
    std::lock_guard<std::mutex> g(g_nic);
    if (a + size > MMIO_SIZE) return 0;
    uint32_t v = 0;
    memcpy(&v, &g_regs[a], size);
    return v;
}

void mmio_write(uint32_t a, uint32_t v, int size)
{
    std::lock_guard<std::mutex> g(g_nic);
    if (!g_started) { // the driver's first access: the network thread and the virtual network start now
        g_started = true;
        kthread_create(nullptr, nullptr, 0, 0, (uint32_t)(uintptr_t)&net_thread, 0, 0, false, "xnet-nic");
        logf("[net] NIC: first register write (0x%03x); network thread started", a);
    }
    if (a + size > MMIO_SIZE) return;
    switch (a) {
    case MDIO_ADDR: {
        set(a, v);
        uint32_t phy_addr = (v >> 5) & 0x1F, r = v & 0x1F;
        if (v & 0x400) {
            if (phy_addr == 1 && r < 6) g_phy[r] = (uint16_t)reg(MDIO_DATA);
        } else {
            set(MDIO_DATA, phy_addr == 1 && r < 6 ? g_phy[r] : 0xFFFFFFFF);
        }
        set(a, reg(a) & ~0x8000u);
        break;
    }
    case TX_RX_CONTROL:
        set(a, (reg(a) & (v | TXRX_IDLE)) | (v & ~TXRX_IDLE)); // IDLE is the controller's
        if (v & TXRX_KICK) transmit();
        if (v & TXRX_RESET) reset_rings();
        if (v & TXRX_BIT1) set(IRQ_STATUS, 0);
        else if (v == 0) set(SETUP_REG5, 0x80000000u);
        flush_rx();
        break;
    case IRQ_STATUS:
    case MII_STATUS: {
        uint32_t cur = 0;
        memcpy(&cur, &g_regs[a], size);
        cur &= ~v;
        memcpy(&g_regs[a], &cur, size);
        break;
    }
    case IRQ_MASK:
        memcpy(&g_regs[a], &v, size);
        if (irq_asserted()) SetEvent(g_wake);
        break;
    default:
        memcpy(&g_regs[a], &v, size);
        break;
    }
}

LONG CALLBACK mmio_trap(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t addr = (uint32_t)er->ExceptionInformation[1];
    if (addr - MMIO_BASE >= MMIO_PAGE) return EXCEPTION_CONTINUE_SEARCH;
    static const MmioOps ops = {mmio_read, mmio_write};
    if (mmio_decode(ep->ContextRecord, addr - MMIO_BASE, ops)) return EXCEPTION_CONTINUE_EXECUTION;
    const uint8_t *ip = (const uint8_t *)(uintptr_t)ep->ContextRecord->Eip;
    logf("[net] NIC register access at %08lx (address %08x) not decoded: %02x %02x %02x %02x %02x %02x", ep->ContextRecord->Eip, addr,
         ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
    return EXCEPTION_CONTINUE_SEARCH;
}

// The network thread: frames from the guest to the virtual network, frames back into the RX ring, and the interrupt
void __stdcall net_thread(void *)
{
    vnet_start();
    uint64_t last_report = kclock_ms();
    for (;;) {
        std::deque<std::vector<uint8_t>> tx;
        bool pending;
        {
            std::lock_guard<std::mutex> g(g_nic);
            tx.swap(g_tx_queue);
            flush_rx();
            pending = !g_rx_pending.empty();
        }
        for (auto &f : tx) vnet_from_guest(f.data(), f.size());
        bool fire;
        {
            std::lock_guard<std::mutex> g(g_nic);
            fire = g_interrupt && irq_asserted();
        }
        if (fire) { // the service routine masks the controller (IRQ_MASK = 0) and queues the driver's DPC
            uint8_t old = xthread_raise_irql((uint8_t)g_interrupt->Irql);
            ((BOOLEAN(__stdcall *)(void *, void *))g_interrupt->ServiceRoutine)(g_interrupt, g_interrupt->ServiceContext);
            xthread_lower_irql(old);
            g_isr_calls++;
        }
        vnet_wait(g_wake, pending ? 2 : 50);
        uint64_t now = kclock_ms();
        if (now - last_report >= 30000) {
            last_report = now;
            logf("[net] NIC: %llu frames sent, %llu received (%llu dropped, %zu waiting), %llu interrupts", g_tx_frames, g_rx_frames,
                 g_rx_dropped, g_rx_pending.size(), g_isr_calls);
        }
    }
}

} // namespace

// A program without a build profile (the Insignia Setup Assistant) with online play off: the controller is there, its
// cable out: the driver starts, no link, every frame it sends is dropped (no identity is read, nothing is sent)
bool nvnet_install_unplugged()
{
    g_cfg.online_unplugged = true;
    if (!nvnet_install()) return false;
    g_unplugged_model = true;
    return true;
}

bool nvnet_install()
{
    void *page = VirtualAlloc((void *)(uintptr_t)MMIO_BASE, MMIO_PAGE, MEM_RESERVE, PAGE_NOACCESS);
    if (page != (void *)(uintptr_t)MMIO_BASE) {
        logc("online: cannot reserve the NIC's register page at 0x%08x (error %lu); staying offline", MMIO_BASE, GetLastError());
        return false;
    }
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    set(TX_RX_CONTROL, TXRX_IDLE);
    set(ADAPTER_CONTROL, g_cfg.online_unplugged ? 0 : 0x4); // LINKUP, unless the cable is out (--online-unplugged)
    if (g_cfg.online_unplugged) logf("[net] tests: the network cable is out (--online-unplugged): no link, nothing sent");
    return true;
}

void nvnet_install_trap()
{
    if (!model_on()) return;
    AddVectoredExceptionHandler(1, mmio_trap); // after the crash handlers: this one sees the access first
    logf("[net] NIC registers at 0x%08x trap to the controller model", MMIO_BASE);
}

bool nvnet_connect_interrupt(void *kinterrupt)
{
    XKINTERRUPT *i = (XKINTERRUPT *)kinterrupt;
    if (!model_on() || i->BusInterruptLevel != 4) return false;
    g_interrupt = i;
    logf("[net] NIC interrupt connected (IRQL %u)", i->Irql);
    return true;
}

// active, 100 Mbit/s, full duplex; none with the cable out (--online-unplugged)
uint32_t nvnet_link_state() { return online_enabled() && !g_cfg.online_unplugged ? 0x1 | 0x2 | 0x8 : 0; }

void nvnet_to_guest(const uint8_t *frame, size_t len)
{
    std::lock_guard<std::mutex> g(g_nic);
    if (len < 14 || len > FRAME_MAX) return;
    std::vector<uint8_t> f(frame, frame + len);
    if (len < 60) f.resize(60, 0); // Ethernet minimum (no FCS)
    if (g_rx_pending.empty() && receive(f)) return;
    if (g_rx_pending.size() < 512) g_rx_pending.push_back(std::move(f));
    else g_rx_dropped++;
}
