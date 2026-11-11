// The console's DSP effects for the 3D mix bins, translated from the program DirectSound downloads to the MCPX's GP
// (dsstdfx.bin, the XDK's standard effects image, which Ninja Gaiden Black loads), run in the image's order:
//   effects 0-3  each 3D bin 6-9 doubled in place (two taps of 0.5 added to the bin itself),
//   effect 4     the I3DL2 reverb: bin 10 in, added to bins 6-9,
//   effect 5     the crosstalk network: bins 6-9 added to the speaker bins 0, 1, 4, 5.
// Fixed point as the DSP56300 does it, on a model of the GP's X memory (X:0C00-0FFF and X:1400-17FF are one mix
// buffer: the GP kernel sends the speaker bins from X:1400 while effect 5 writes them at X:0C00) and of the scratch
// memory its DMA reads and writes, so that every word matches the program run on an emulated DSP (docs/05 §11).
#pragma once

#include <cstdint>
#include <vector>

namespace gpfx {

constexpr uint32_t FRAME = 32; // samples a GP frame (48 kHz)

class Gp {
public:
    // The image's effect states as DirectSound loads them, each at the X address its code uses (the effect code's
    // first instruction, move #addr,r5). state[e] / words[e]: effects 0-5.
    bool load(const uint32_t *const state[6], const uint32_t words[6]);
    // The library's I3DL2 conversion (FUN_0037B4E5): words 0x46-0x87 of the reverb state; sets its "changed" flag.
    void set_reverb_params(const uint32_t words_46_87[0x42]);

    // One frame on the mix buffer (X:1400 + 32 * bin, 24-bit words): bins 6-10 in, bins 0, 1, 4, 5 added to.
    void frame();

    uint32_t *bin(uint32_t b) { return mix + FRAME * b; }

    bool saturation_mode = true; // effects 0-3 and 5 set SR bit 20 (SM); off only to compare with xemu, which lacks it
    bool xemu_macr_x1_y0_b = false; // to compare with xemu only: its macr +x1,y0,b does not round

    // the model's memory, for the comparison with an emulated DSP
    uint32_t x[0x1000] = {}; // X:0000-0FFF (X:0C00-0FFF reads and writes go to mix)
    uint32_t mix[0x400] = {}; // X:1400-17FF
    std::vector<uint8_t> scratch;

private:
    uint32_t xs[6] = {}; // each effect's state address (r5)
    uint32_t &m(uint32_t a);
    void doubler(int e);
    void reverb();
    void xtalk();
    // reverb parts
    void rv_init();
    void rv_tables();
    void rv_set_taps();
    void rv_update_taps();
    void rv_param_ramp();
    void rv_fade();
    void rv_process();
    void rv_tap(uint32_t wnode, uint32_t rnode, uint32_t dist);
    void dma(uint32_t node);
};

} // namespace gpfx
