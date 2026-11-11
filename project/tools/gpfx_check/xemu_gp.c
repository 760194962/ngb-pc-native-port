// xemu's DSP56300 core and GP DMA running effects 0-5 of an unscrambled effects image, as a library for
// gp_effects_test.cpp. The code is loaded from P:0x206 as DirectSound's image places it (position independent).
#include <math.h>
#include "interp/dsp_cpu_tool.c"
#include "dsp_dma.c"

typedef struct {
    dsp_core_t core;
    DSPDMAState dma;
    uint32_t interrupts;
    bool halt;
} Gp;
static Gp gp;
static uint8_t *scratch;
static size_t scratch_size;
static uint32_t pstart, pend;

static void scratch_rw(void *o, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    if (addr + len > scratch_size) { fprintf(stderr, "scratch out of range %x+%zx\n", addr, len); exit(3); }
    if (dir) memcpy(scratch + addr, ptr, len);
    else memcpy(ptr, scratch + addr, len);
}
static void fifo_rw(void *o, uint8_t *ptr, unsigned idx, size_t len, bool dir) { fprintf(stderr, "fifo\n"); exit(4); }
static uint32_t mem_read(void *o, int space, uint32_t a) { return dsp56k_read_memory(&gp.core, space, a); }
static void mem_write(void *o, int space, uint32_t a, uint32_t v) { dsp56k_write_memory(&gp.core, space, a, v); }
static uint32_t rd_periph(dsp_core_t *c, uint32_t a)
{
    switch (a) {
    case 0xFFFFC5: return gp.interrupts | (gp.dma.eol ? 0x80 : 0);
    case 0xFFFFD4: return dsp_dma_read(&gp.dma, DMA_NEXT_BLOCK);
    case 0xFFFFD5: return dsp_dma_read(&gp.dma, DMA_START_BLOCK);
    case 0xFFFFD6: return dsp_dma_read(&gp.dma, DMA_CONTROL);
    case 0xFFFFD7: return dsp_dma_read(&gp.dma, DMA_CONFIGURATION);
    }
    return 0;
}
static void wr_periph(dsp_core_t *c, uint32_t a, uint32_t v)
{
    switch (a) {
    case 0xFFFFC4: if (v & 1) gp.halt = true; return;
    case 0xFFFFC5: gp.interrupts &= ~v; if (v & 0x80) gp.dma.eol = false; return;
    case 0xFFFFD4: dsp_dma_write(&gp.dma, DMA_NEXT_BLOCK, v); return;
    case 0xFFFFD5: dsp_dma_write(&gp.dma, DMA_START_BLOCK, v); return;
    case 0xFFFFD6: dsp_dma_write(&gp.dma, DMA_CONTROL, v); return;
    case 0xFFFFD7: dsp_dma_write(&gp.dma, DMA_CONFIGURATION, v); return;
    }
}
static uint32_t w32(const uint8_t *d, uint32_t o) { uint32_t v; memcpy(&v, d + o, 4); return v; }

// img: the unscrambled image; returns 0 on success
int xgp_init(const uint8_t *img, size_t n, size_t scratch_bytes)
{
    uint32_t code_words = w32(img, 0x804), other = w32(img, 0x80C);
    uint32_t desc = 0x818 + (other + code_words) * 4, count = w32(img, desc);
    scratch_size = scratch_bytes;
    scratch = calloc(1, scratch_size);
    dsp56k_reset_cpu(&gp.core);
    gp.core.is_gp = true;
    gp.core.read_peripheral = rd_periph;
    gp.core.write_peripheral = wr_periph;
    gp.dma.mem_read = mem_read;
    gp.dma.mem_write = mem_write;
    gp.dma.rw_opaque = &gp;
    gp.dma.scratch_rw = scratch_rw;
    gp.dma.fifo_rw = fifo_rw;
    uint32_t cur = 0x818;
    pstart = cur / 4;
    for (uint32_t e = 0; e < count; e++) {
        uint32_t size = w32(img, desc + 8 + 32 * e + 4), st = w32(img, desc + 8 + 32 * e + 8), st_size = w32(img, desc + 8 + 32 * e + 12);
        for (uint32_t i = 0; i < size / 4; i++) gp.core.pram[cur / 4 + i] = w32(img, cur + 4 * i) & 0xFFFFFF;
        uint32_t xs = gp.core.pram[cur / 4 + 1];
        for (uint32_t i = 0; i < st_size / 4; i++) dsp56k_write_memory(&gp.core, DSP_SPACE_X, xs + i, w32(img, st + 4 * i) & 0xFFFFFF);
        cur += size;
    }
    pend = cur / 4;
    gp.core.pram[pend] = 0x08F484; // movep #1,x:$ffffc4: stop after the last effect
    gp.core.pram[pend + 1] = 0x000001;
    return 0;
}

uint32_t xgp_read(uint32_t a) { return dsp56k_read_memory(&gp.core, DSP_SPACE_X, a); }
void xgp_write(uint32_t a, uint32_t v) { dsp56k_write_memory(&gp.core, DSP_SPACE_X, a, v); }
uint8_t *xgp_scratch(void) { return scratch; }

void xgp_frame(void)
{
    gp.interrupts |= 2;
    gp.halt = false;
    gp.core.pc = pstart;
    uint64_t steps = 0;
    while (!gp.halt) {
        dsp56k_execute_instruction(&gp.core);
        if (++steps > 5000000) { fprintf(stderr, "no end, pc %04x\n", gp.core.pc); exit(6); }
    }
}
