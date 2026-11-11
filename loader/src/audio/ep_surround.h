// The console's Dolby Surround encoder: module 0 of the EP program in the game's DOLBY section (identical in all eight
// supported builds), translated word for word (docs/05 §12; checked against xemu's DSP56300 core by
// project/tools/gpfx_check/ep_surround_test.cpp). The EP runs it for the analog output when the speakers are set to
// surround (DSSPEAKER_SURROUND) or Dolby Digital is on; stereo and mono use modules 5 and 4 (plain downmixes).
#pragma once
#include <cstdint>

namespace epds {

class Encoder {
public:
    void reset(); // the module's first frame (b = 1): its 0x4E state words cleared
    // n samples of the speaker bins L C R Ls Rs (24-bit, as the GP outputs them) to Lt Rt (24-bit). Any n: the module
    // works sample by sample; its 256-sample frames show only where the filters' carried remainder is cut to 48 bits.
    void process(const int32_t *const in[5], int32_t *lt, int32_t *rt, uint32_t n);

private:
    struct Section { int32_t y1, y2, x1, x2; int64_t e; }; // e: the remainder carried from one output to the next
    int32_t run(Section *s, const int32_t *c, int sections, int32_t x);
    Section lf_[4], rf_[4], sf_[5];
    uint32_t phase_ = 0; // samples into the module's 256-sample frame
};

} // namespace epds
