// xemu's DSP56300 core running module 0 of the game's EP program (the DOLBY section: Dolby Surround encode for the
// analog output), as a library. The kernel's service routine at P:0x180 is done here (the commands module 0 uses):
//   7  input:  channel n1, n0 words into X:r0 (DMA format 6, 24-bit in 32: unchanged)
//   8  output: X:r0, n0 words to output n1 (format 1, 16-bit: kept here as the 24-bit words; the 16-bit cut is v >> 8)
//   0xF levels (ignored), 0x11 heap: b=0 allocate, b=1 heap -> X:r0, b=2 X:r0 -> heap (n0 words)
// Module 0 is loaded at P:0x300 (its 4-word header first) and entered at P:0x304 with r0 = 0xBC0 (the parameter block,
// from the section) and b = 1 on the first frame, 0 after (boot code p:0053-0059).
#include "interp/dsp_cpu_tool.c"

static dsp_core_t ep;
static bool ep_halt;
static uint32_t heap[0x100];
static const int32_t *ep_in[5];
static int32_t *ep_out[2];
static int ep_first = 1;
static uint32_t ep_cmd_log[32], ep_ncmd;

static uint32_t rd_periph(dsp_core_t *c, uint32_t a) { return 0; }
static void wr_periph(dsp_core_t *c, uint32_t a, uint32_t v) { if (a == 0xFFFFC4 && (v & 1)) ep_halt = true; }
static uint32_t w32(const uint8_t *d, uint32_t o) { uint32_t v; memcpy(&v, d + o, 4); return v; }

int xep_init(const uint8_t *sec, size_t n)
{
    if (n < 0x7120 + 4) return 1;
    dsp56k_reset_cpu(&ep);
    ep.is_gp = false;
    ep.read_peripheral = rd_periph;
    ep.write_peripheral = wr_periph;
    for (uint32_t i = 0; i < 0x2F2; i++) ep.pram[0x300 + i] = w32(sec, 0x920 + 4 * i) & 0xFFFFFF;
    for (uint32_t i = 0; i < 24; i++) // the section's data ends inside the block (0x716C bytes): the rest is zero
        dsp56k_write_memory(&ep, DSP_SPACE_X, 0xBC0 + i, 0x7120 + 4 * i + 4 <= n ? w32(sec, 0x7120 + 4 * i) & 0xFFFFFF : 0);
    ep.pram[0x180] = 0x00000C; // rts: the commands are done before it runs
    ep.pram[0x2F0] = 0x0BF080, ep.pram[0x2F1] = 0x000304; // jsr p:0304
    ep.pram[0x2F2] = 0x08F484, ep.pram[0x2F3] = 0x000001; // movep #1,x:$ffffc4: stop
    return 0;
}

uint32_t xep_x(uint32_t a) { return dsp56k_read_memory(&ep, DSP_SPACE_X, a); }

static int32_t s24(uint32_t v) { return (int32_t)(v << 8) >> 8; }

static void service(void)
{
    uint32_t a = ep.registers[DSP_REG_A1], b = ep.registers[DSP_REG_B1], r0 = ep.registers[DSP_REG_R0];
    uint32_t n0 = ep.registers[DSP_REG_N0], n1 = ep.registers[DSP_REG_N1];
    if (ep_ncmd < 32) ep_cmd_log[ep_ncmd++] = a << 8 | b;
    if (a == 7) {
        if (n1 > 4) { fprintf(stderr, "input %u\n", n1); exit(5); }
        for (uint32_t i = 0; i < n0; i++) dsp56k_write_memory(&ep, DSP_SPACE_X, r0 + i, (uint32_t)ep_in[n1][i] & 0xFFFFFF);
    } else if (a == 8) {
        if (n1 > 1) { fprintf(stderr, "output %u\n", n1); exit(5); }
        for (uint32_t i = 0; i < n0; i++) ep_out[n1][i] = s24(dsp56k_read_memory(&ep, DSP_SPACE_X, r0 + i));
    } else if (a == 0x11) {
        if (b == 1) for (uint32_t i = 0; i < n0; i++) dsp56k_write_memory(&ep, DSP_SPACE_X, r0 + i, heap[i]);
        else if (b == 2) for (uint32_t i = 0; i < n0; i++) heap[i] = dsp56k_read_memory(&ep, DSP_SPACE_X, r0 + i);
        else if (b != 0) { fprintf(stderr, "heap %u\n", b); exit(5); }
    } else if (a != 0xF) { fprintf(stderr, "command %x\n", a); exit(5); }
    ep.registers[DSP_REG_A2] = ep.registers[DSP_REG_A1] = ep.registers[DSP_REG_A0] = 0;
}

// one frame of 256 samples: in[5] (L C R Ls Rs) -> out[2] (Lt Rt), 24-bit values
void xep_frame(const int32_t *const in[5], int32_t *const out[2])
{
    for (int c = 0; c < 5; c++) ep_in[c] = in[c];
    ep_out[0] = out[0], ep_out[1] = out[1];
    ep.registers[DSP_REG_R0] = 0xBC0;
    ep.registers[DSP_REG_B2] = 0, ep.registers[DSP_REG_B1] = ep_first ? 1 : 0, ep.registers[DSP_REG_B0] = 0;
    ep_first = 0;
    ep.pc = 0x2F0;
    ep_halt = false;
    ep_ncmd = 0;
    uint64_t steps = 0;
    while (!ep_halt) {
        if (ep.pc == 0x180) service();
        uint32_t op = ep.pram[ep.pc];
        if ((op & 0xFFFFEE) == 0x0C1880) { // extractu #CO,S,D (xemu lacks it): DSP56300 FM, 0000110000011000100s000D + CO
            uint32_t co = ep.pram[ep.pc + 1], width = (co >> 12) & 0x3F, off = co & 0x3F;
            int s = (op >> 4) & 1, d = op & 1;
            uint64_t v = (uint64_t)(ep.registers[s ? DSP_REG_B2 : DSP_REG_A2] & 0xFF) << 48 |
                         (uint64_t)(ep.registers[s ? DSP_REG_B1 : DSP_REG_A1] & 0xFFFFFF) << 24 | (ep.registers[s ? DSP_REG_B0 : DSP_REG_A0] & 0xFFFFFF);
            uint64_t f = (v >> off) & ((1ull << width) - 1);
            ep.registers[d ? DSP_REG_B2 : DSP_REG_A2] = 0, ep.registers[d ? DSP_REG_B1 : DSP_REG_A1] = (uint32_t)(f >> 24) & 0xFFFFFF;
            ep.registers[d ? DSP_REG_B0 : DSP_REG_A0] = (uint32_t)f & 0xFFFFFF;
            ep.pc += 2;
            continue;
        }
        { // leaving the module means a misread: print the last 16 addresses
            static uint32_t h[16];
            static int hi;
            h[hi++ & 15] = ep.pc;
            if (ep.pc >= 0x5F2) { // past the module (the stop at P:0x2F2 is below it)
                for (int k = 0; k < 16; k++) fprintf(stderr, "%04x ", h[(hi + k) & 15]);
                fprintf(stderr, "\n");
                exit(9);
            }
        }
        dsp56k_execute_instruction(&ep);
        if (++steps > 5000000) { fprintf(stderr, "no end, pc %04x\n", ep.pc); exit(6); }
    }
}

uint32_t xep_cmds(const uint32_t **log) { *log = ep_cmd_log; return ep_ncmd; }
