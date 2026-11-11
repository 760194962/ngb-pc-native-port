// EP module 0, the Dolby Surround encoder (ep_surround.h). Section VA 0xDBA600 in the US builds; module 0 is the 0x2F2
// words at section offset 0x920, loaded at P:0x300 and entered at P:0x304. Per 256-sample frame it does:
//   p:03D4  all five inputs times the gain word p:(0x305 + p[7]); p[7] is 0x1F, clamped to 1..0x1F by IFlt / IFgt, and
//           the 31-entry table is p:0x305-0x323, so index 0x1F reads p:0x324, the module's first instruction word
//           0x607000 (0.7534, -2.46 dB). The library never writes p[7] (its one writer of the block, 0x3827ED, sets
//           p[1-3] for stereo and mono only).
//   p:0469  S = 0.7071 Ls + 0.7071 Rs (mpy, macr)
//   p:047F  L' = L + 0.7071 C, R' = R + 0.7071 C (macr)
//   p:04A7  L' and R' through four second-order sections (coefficients Y:0x2D), S through five (Y:0x41): p:05CA
//   p:04EC  Lt = L' + 0.7071 S', Rt = R' - 0.7071 S' (macr)
// The coefficient set at Y:0 / Y:0x14 is chosen by bits 12-13 of x:0x608, which the module itself sets to 0: unused.
// Arithmetic as the DSP56300 does it (SR after reset: no scaling, convergent rounding, no arithmetic saturation).
#include "audio/ep_surround.h"
#include <cstring>

namespace epds {
namespace {

constexpr int32_t kGain = 0x607000; // p:0x324
constexpr int32_t kHalf = 0x5A827A; // p:0475 / p:049C / p:0500
// p:0560-0574 (Y:0x2D) and p:0574-058C (Y:0x41): per section c0..c4, y[n] = -2 c0 y1 - c1 y2 + c2 x + 2 c3 x1 + c4 x2
constexpr int32_t kLR[4][5] = {
    {0xC08974, 0x000000, 0x7EED19, 0xC00000, 0x000000},
    {0x462AF8, 0x378620, 0x378620, 0x462AF8, 0x7FFFFF},
    {0x84EF9E, 0x763A35, 0x763A35, 0x84EF9E, 0x7FFFFF},
    {0xE648FE, 0xE4B78A, 0xE4B78A, 0xE648FE, 0x7FFFFF},
};
constexpr int32_t kS[5][5] = {
    {0x8112E7, 0x7DDC7F, 0x7EEDAC, 0x811254, 0x7EEDAC},
    {0x462AF8, 0x378620, 0x511DF3, 0x4EF590, 0x511DF3},
    {0x462AF8, 0x378620, 0x511DF3, 0x4EF590, 0x511DF3},
    {0x82101A, 0x7BE2ED, 0x7BE2ED, 0x82101A, 0x7FFFFF},
    {0xAF2EFC, 0x2C7800, 0x2C7800, 0xAF2EFC, 0x7FFFFF},
};

inline int32_t s24(int32_t v) { return (int32_t)((uint32_t)v << 8) >> 8; }
// accumulators as int64 holding a2:a1:a0 (56 bits; a1 is bits 24-47)
inline int64_t mul(int32_t a, int32_t b) { return (int64_t)s24(a) * s24(b) * 2; } // fractional: s24 x s24 x 2
inline int64_t up(int32_t v) { return (int64_t)s24(v) * (1LL << 24); }            // a word into a1
inline int64_t rnd(int64_t a) // convergent rounding at a1 (SR RM = 0, no scaling)
{
    bool tie = (a & 0xFFFFFF) == 0x800000;
    a = (a + 0x800000) & ~(int64_t)0xFFFFFF;
    return tie ? a & ~((int64_t)1 << 24) : a;
}
inline int32_t lim(int64_t a) // the move of an accumulator to a word: a1, limited when a2 is not its sign
{
    if (a > 0x7FFFFFFFFFFFLL) return 0x7FFFFF;
    if (a < -0x800000000000LL) return -0x800000;
    return (int32_t)(a >> 24);
}
inline int64_t cut48(int64_t e) { return (int64_t)((uint64_t)e << 16) >> 16; } // b1:b0 saved, b2 dropped

} // namespace

void Encoder::reset()
{
    memset(lf_, 0, sizeof lf_), memset(rf_, 0, sizeof rf_), memset(sf_, 0, sizeof sf_);
    phase_ = 0;
}

// p:05CA, one sample through the cascade (in place on the module's buffer, a section at a time per frame; the same
// arithmetic per sample). b carries a - x0 (the output's cut-off remainder) into the next output's sum.
int32_t Encoder::run(Section *s, const int32_t *c, int sections, int32_t x)
{
    for (int k = 0; k < sections; k++, c += 5) {
        Section &q = s[k];
        int64_t a = -mul(c[0], q.y1);
        a -= mul(c[0], q.y1);
        a -= mul(q.y2, c[1]);
        a += mul(c[2], x);
        a += mul(q.x1, c[3]);
        a += mul(q.x1, c[3]);
        a += mul(c[4], q.x2);
        a += q.e;
        int32_t y = lim(a);
        q.e = a - up(y);
        q.y2 = q.y1, q.y1 = y, q.x2 = q.x1, q.x1 = x;
        x = y;
    }
    return x;
}

void Encoder::process(const int32_t *const in[5], int32_t *lt, int32_t *rt, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        int32_t g[5];
        for (int c = 0; c < 5; c++) g[c] = lim(rnd(mul(kGain, in[c][i]))); // p:043A mpyr
        int32_t sv = lim(rnd(mul(kHalf, g[3]) + mul(kHalf, g[4])));          // p:047B
        int32_t lv = lim(rnd(up(g[0]) + mul(kHalf, g[1])));                  // p:04A3
        int32_t rv = lim(rnd(up(g[2]) + mul(kHalf, g[1])));
        lv = run(lf_, &kLR[0][0], 4, lv);
        rv = run(rf_, &kLR[0][0], 4, rv);
        sv = run(sf_, &kS[0][0], 5, sv);
        lt[i] = lim(rnd(up(lv) + mul(kHalf, sv))); // p:0507
        rt[i] = lim(rnd(up(rv) - mul(kHalf, sv)));
        if (++phase_ == 256) { // the frame's end: each section's remainder saved as b1:b0 (p:05EB) and reloaded
            phase_ = 0;
            for (int k = 0; k < 4; k++) lf_[k].e = cut48(lf_[k].e), rf_[k].e = cut48(rf_[k].e);
            for (Section &q : sf_) q.e = cut48(q.e);
        }
    }
}

} // namespace epds
