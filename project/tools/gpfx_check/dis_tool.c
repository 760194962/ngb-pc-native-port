// usage: dis_tool IMAGE BYTE_OFFSET BYTE_SIZE P_ADDR  - disassemble DSP56300 words from an image
#include "interp/dsp_cpu_tool.c"
static dsp_core_t core;
int main(int argc, char **argv)
{
    if (argc < 5) return 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    uint32_t off = strtoul(argv[2], 0, 16), size = strtoul(argv[3], 0, 16), at = strtoul(argv[4], 0, 16);
    core.is_gp = true;
    dsp56k_reset_cpu(&core);
    for (uint32_t i = 0; i < size / 4 && off + 4 * i + 4 <= n; i++) {
        uint32_t w; memcpy(&w, buf + off + 4 * i, 4);
        core.pram[(at + i) % DSP_PRAM_SIZE] = w & 0xffffff;
    }
    uint32_t pc = at, end = at + size / 4;
    while (pc < end) {
        core.pc = pc;
        core.disasm_prev_inst_pc = 0xffffffff;
        uint16_t len = disasm_instruction(&core, DSP_DISASM_MODE);
        if (!len) len = 1;
        printf("%s\n", disasm_get_instruction_text(&core));
        pc += len;
    }
    return 0;
}
