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

## The EP's Dolby Surround encoder (`ep_surround_test.cpp`, docs/05 §12)

Evidence for `loader/src/audio/ep_surround.cpp`: module 0 of the EP program in the XBE's DOLBY section (plaintext,
0x716C bytes, identical in all eight supported builds) on xemu's core and the translation side by side.

1. Save the DOLBY section's raw data from the XBE (section header: raw address and size) as `dolby.bin`.
2. Build `xemu_ep.c` like `xemu_gp.c` (same `xdsp/` tree and shims), then
   `clang++ -O2 -std=c++17 -static ep_surround_test.cpp <loader>/src/audio/ep_surround.cpp xemu_ep.o -I<loader>/src/audio -I<loader>/src`.
3. `ep_surround_test dolby.bin <frames> <seed> <amplitude> [tone]`.

`xemu_ep.c` does the EP kernel's service routine (P:0x180) for the commands module 0 uses (7 input, 8 output, 0xF
levels, 0x11 state heap), and the one instruction xemu lacks: `extractu #CO,S,D` (DSP56300 Family Manual), which the
module uses to read its flag word. xemu also decodes `IFcc` (`0x2020xx-0x203Fxx`, conditional ALU) as a plain ALU
op with a stray move; at `p:03DA` / `p:03DE` (the gain index clamp) both give the same result for the index the game
has (0x1F).

Result (2026-11-11): noise at 0.1, 0.5, 1.0, 2.0 and 8.0 of full scale and five tones, 400 frames each, the
translation fed blocks of random length: every word identical. Two deliberate breaks show the comparison bites:
rounding without the tie-to-even rule differs in about 58% of the words; leaving out the 48-bit cut of the filters'
remainder at the frame end differs at 8.0 (clipping) only.

## What the comparison found (2026-11-11)

- Three parameter sets, noise at 0.5 and 1.0, 3000 frames each: every word identical. Two settings are needed:
  - `saturation_mode = false`: xemu has no SR bit 20 (SM, DSP56300 Family Manual); effects 0-3 and 5 set it.
  - `xemu_macr_x1_y0_b = true`: xemu's `emu_macr_p_x1_y0_b` rounds an uninitialised value before the add, so
    `macr +x1,y0,b` does not round. It is the only one of xemu's 64 `macr` / `mpyr` variants that does this. The
    reverb's output stage uses it.

  The game build keeps both as the manual has them.
- One reading error was caught: the fade's gain steps every sample (the loop at `p:00B2` ends at its `add`).
