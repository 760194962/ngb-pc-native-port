# gpfx_check: the console's GP effects, word for word

Evidence for `loader/src/audio/gp_effects.cpp` (docs/05 §11). Runs the effects of the game's `dsstdfx.bin` on
xemu's DSP56300 interpreter and DMA engine and on the translation side by side, and compares the GP's X memory, its
mix buffer and the scratch memory after every 32-sample frame.

## Steps

1. Unscramble the image's code (DirectSound's own algorithm, `FUN_00382386` / `FUN_003823B3`):
   `python -I ../dsp_unscramble.py <game>/dsstdfx.bin dsstdfx_plain.bin`
   It prints each effect's code size and how many words have a non-zero top byte (0 for real DSP56300 code).
2. Get xemu's DSP sources (GPL, not in this repository). They are `hw/xbox/mcpx/apu/dsp/` of xemu-project/xemu
   (checked at 6095054b7d02, 2026-11-11). Put the folder's files in `xdsp/` and `interp/` in `xdsp/interp/`.
   Then add the shim headers this folder's `.c` files need:
   - `qemu/osdep.h`: stdint, stdio, stdlib, string, stdbool, assert, `ARRAY_SIZE`, `MIN`, `MAX`.
   - `qemu/bswap.h`: `ldl_le_p` / `stl_le_p` as memcpy.
   - `qemu/compiler.h`: empty.
   - `trace.h`: `trace_dsp56k_execute_instruction(...)`, `trace_event_get_state(...)` (0) and
     `trace_dsp56k_execute_instruction_disasm(...)`.

   For data words, copy `interp/dsp_cpu.c` to `interp/dsp_cpu_tool.c` and replace the
   `assert(!"Invalid op code in dsp_cpu")` with `{ static const OpcodeEntry none = {0}; return &none; }`.
3. Build with the toolchain's clang (llvm-mingw):
   - `clang -O2 -w -Ixdsp/shim -Ixdsp/interp -Ixdsp -c xemu_gp.c`
   - `clang++ -O2 -std=c++17 -static gp_effects_test.cpp <loader>/src/audio/gp_effects.cpp xemu_gp.o -I<loader>/src/audio`
4. Run `gp_effects_test dsstdfx_plain.bin <patch> 3000 <seed> <amplitude>`.
   A patch is the reverb words the library's I3DL2 conversion gives (`index value` lines, word 4 and 0x46-0x87).
   Get them from the game: the loader's log prints a summary, or call the conversion as `SetI3DL2Listener` does.
   The patch is applied at frame 200, as the library would.

`dis_tool.c` disassembles any range of an unscrambled image: `dis_tool <image> <byte offset> <byte size> <P address>`.

## What the comparison found (2026-11-11)

- Three parameter sets, noise at 0.5 and 1.0, 3000 frames each: every word identical. Two settings are needed:
  - `saturation_mode = false`: xemu has no SR bit 20 (SM, DSP56300 Family Manual); effects 0-3 and 5 set it.
  - `xemu_macr_x1_y0_b = true`: xemu's `emu_macr_p_x1_y0_b` rounds an uninitialised value before the add, so
    `macr +x1,y0,b` does not round. It is the only one of xemu's 64 `macr` / `mpyr` variants that does this. The
    reverb's output stage uses it.

  The game build keeps both as the manual has them.
- One reading error was caught: the fade's gain steps every sample (the loop at `p:00B2` ends at its `add`).
