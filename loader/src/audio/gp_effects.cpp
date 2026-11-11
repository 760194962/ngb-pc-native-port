// See gp_effects.h. Addresses in the comments (p:xxxx) are each effect's code, relative to its start, as the
// unscrambled image disassembles; S is the effect's state address (r5).
#include "gp_effects.h"

#include <cstring>

namespace gpfx {
namespace {

// ---- DSP56300 data ALU, fixed point (DSP56300 Family Manual; xemu's interpreter for the details) ----
// An accumulator is 56 bits, A2:A1:A0 = 8:24:24, held sign-extended in an int64 in units of 2^-47.
using Acc = int64_t;
inline int32_t s24(uint32_t v) { return (int32_t)(v << 8) >> 8; }
inline Acc wrap(Acc a) { return (Acc)((uint64_t)a << 8) >> 8; } // 56-bit two's complement
inline Acc acc24(uint32_t v) { return (Acc)s24(v) << 24; }       // a 24-bit word into A1, A2 sign, A0 0
// a product of two fractions: 48 bits shifted left once (the DSP's fractional multiply)
inline Acc mul(uint32_t a, uint32_t b) { return (Acc)s24(a) * s24(b) * 2; }
// convergent rounding to A1 (SR RM 0, no scaling): half an A1 LSB added; on a tie (A0 0 after) A1's LSB cleared
inline Acc rnd(Acc a)
{
    a = wrap(a + 0x800000);
    if ((a & 0xFFFFFF) == 0) a &= ~(Acc)0x1000000;
    return a & ~(Acc)0xFFFFFF;
}
// an accumulator read as a 24-bit word (moves to memory or registers): A1, or saturated when A2 is not A1's sign
inline uint32_t lim(Acc a)
{
    if (a > 0x7FFFFFFFFFFFLL) return 0x7FFFFF;
    if (a < -0x800000000000LL) return 0x800000;
    return (uint32_t)(a >> 24) & 0xFFFFFF;
}
// SR bit 20 (SM): arithmetic results saturate to 48 bits
inline Acc sm(Acc a, bool on)
{
    if (!on) return a;
    if (a > 0x7FFFFFFFFFFFLL) return 0x7FFFFFFFFFFFLL;
    if (a < -0x800000000000LL) return -0x800000000000LL;
    return a;
}
inline uint32_t a1(Acc a) { return (uint32_t)(a >> 24) & 0xFFFFFF; }
inline uint32_t a0(Acc a) { return (uint32_t)a & 0xFFFFFF; }
inline Acc set_a1(Acc a, uint32_t v) { return wrap((a & 0xFFFFFF) | ((Acc)(v & 0xFFFFFF) << 24) | (a & ~(Acc)0xFFFFFFFFFFFF)); }

} // namespace

uint32_t &Gp::m(uint32_t a)
{
    a &= 0xFFFF;
    if (a >= 0x1400 && a < 0x1800) return mix[a - 0x1400];
    if (a >= 0x0C00 && a < 0x1000) return mix[a - 0x0C00];
    return x[a & 0xFFF];
}

bool Gp::load(const uint32_t *const state[6], const uint32_t words[6])
{
    static const uint32_t kAt[6] = {0x80, 0x9C, 0xB8, 0xD4, 0xF0, 0x31A}; // each effect's move #addr,r5
    for (int e = 0; e < 6; e++) {
        xs[e] = kAt[e];
        for (uint32_t i = 0; i < words[e]; i++) m(xs[e] + i) = state[e][i] & 0xFFFFFF;
    }
    // the reverb's scratch: its state's word 0 (base) and 1 (size), from the image's descriptor
    scratch.assign(m(xs[4]) + m(xs[4] + 1) + 0x1000, 0);
    return true;
}

void Gp::set_reverb_params(const uint32_t words_46_87[0x42])
{
    const uint32_t S = xs[4];
    for (uint32_t i = 0; i < 0x42; i++) m(S + 0x46 + i) = words_46_87[i] & 0xFFFFFF;
    m(S + 4) |= 4; // FUN_0037E5CC: word 4 |= 4, then both writes
}

void Gp::frame()
{
    for (int e = 0; e < 4; e++) doubler(e);
    reverb();
    xtalk();
}

// ---- effects 0-3 (P:0-0x29 each): out = (out if flag 1) + in0 * g0 + in1 * g1, SM on ----
void Gp::doubler(int e)
{
    const uint32_t S = xs[e];
    uint32_t flags = m(S + 4);
    if (!(flags & 1)) m(S + 4) = flags |= 1;
    // the buffers (32 words each, none across a region's edge): pointers once (they may be one buffer, as here)
    uint32_t *in0 = &m(m(S + 5)), *in1 = &m(m(S + 6)), *out = &m(m(S + 7)), y0 = m(S + 8), y1 = m(S + 9);
    const bool on = saturation_mode;
    for (uint32_t i = 0; i < 32; i++) {
        Acc b = 0; // clr b
        if (flags & 2) b = acc24(out[i]);
        uint32_t x0 = in0[i];
        Acc a = sm(mul(y0, x0), on);
        uint32_t x1 = in1[i];
        b = sm(wrap(b + a), on);
        a = sm(mul(y1, x1), on);
        b = sm(wrap(b + a), on);
        out[i] = lim(b);
    }
}

// ---- effect 5, the crosstalk network (P:0-0x73), SM on ----
void Gp::xtalk()
{
    const uint32_t S = xs[5];
    if (!(m(S + 4) & 1)) {
        m(S + 4) |= 1;
        m(S + 63) = 0x7645A1; // the low-pass pole
        m(S + 64) = 0xF88313; // the crosstalk gain
    }
    const bool on = saturation_mode;
    for (int pass = 0; pass < 2; pass++) {
        // pass 0: bins 6/7 -> FL/FR (history S+13, S+25, index S+37, filter S+24); pass 1: 8/9 -> BL/BR
        uint32_t *in1 = &m(m(S + (pass ? 7 : 5))), *in2 = &m(m(S + (pass ? 8 : 6)));
        uint32_t *o6 = &m(m(S + (pass ? 11 : 9))), *o7 = &m(m(S + (pass ? 12 : 10)));
        uint32_t *h3 = &m(S + (pass ? 0x26 : 0x0D)), *h4 = &m(S + (pass ? 0x32 : 0x19)), *f0 = &m(S + (pass ? 49 : 24));
        uint32_t idx = S + (pass ? 62 : 37);
        uint32_t n3 = m(idx);
        const uint32_t y0 = m(S + 63), y1 = m(S + 64);
        const bool add = m(S + 4) & 2;
        for (uint32_t i = 0; i < 32; i++) {
            Acc b = sm(wrap(mul(y0, f0[0]) + 0), on);
            b = sm(wrap(b + mul(y1, h3[n3])), on);
            f0[0] = lim(b);
            Acc a = sm(mul(y0, f0[12]), on);
            a = sm(wrap(a + mul(y1, h4[n3])), on);
            f0[12] = lim(a);
            uint32_t x0 = in1[i], x1 = in2[i];
            a = sm(wrap(a + acc24(x0)), on);
            b = sm(wrap(b + acc24(x1)), on);
            h3[n3] = lim(a);
            h4[n3] = lim(b);
            if (!add) a = acc24(x0), b = acc24(x1); // not adding: the inputs themselves
            a = sm(wrap(a + acc24(o6[i])), on);
            b = sm(wrap(b + acc24(o7[i])), on);
            o6[i] = lim(a);
            o7[i] = lim(b);
            n3 = n3 + 1 < 11 ? n3 + 1 : 0;
        }
        m(idx) = n3;
    }
}

// ---- the reverb's DMA (GP DMA as xemu's dsp_dma.c runs it): a chain of 7-word nodes from `node` ----
// node: +0 next (0x4000 = end), +1 control, +2 count, +3 DSP address, +4 scratch offset, +5 base, +6 size - 1
void Gp::dma(uint32_t node)
{
    uint32_t next = node & ~0x2000u;
    while (!(next & 0x4000)) {
        uint32_t at = next & 0x3FFF;
        next = m(at);
        uint32_t control = m(at + 1), count = m(at + 2), dsp = m(at + 3), off = m(at + 4), base = m(at + 5),
                 size = m(at + 6) + 1;
        bool to_scratch = control & 2; // format 2 (24 bits in 4 bytes), buffer 0xE (circular), offset written back
        uint32_t bytes = count * 4, done = 0;
        uint32_t *d = &m(dsp); // the node's DSP buffer (16 words, in one region)
        if (off >= size) off = 0;
        while (done < bytes) {
            uint32_t chunk = bytes - done < size - off ? bytes - done : size - off;
            uint8_t *p = &scratch[base + off];
            uint32_t *w = d + done / 4;
            if (to_scratch) memcpy(p, w, chunk);
            else
                for (uint32_t k = 0; k < chunk / 4; k++) {
                    uint32_t v;
                    memcpy(&v, p + 4 * k, 4);
                    w[k] = v & 0xFFFFFF;
                }
            off += chunk;
            if (off >= size) off = 0;
            done += chunk;
        }
        if (control & 0x10) m(at + 4) = off;
    }
}

// p:475 with r0 = write node, r1 = read node, x1 = distance: the read node's offset dist bytes behind the write
// node's, 16-byte aligned, wrapped into the line
void Gp::rv_tap(uint32_t wnode, uint32_t rnode, uint32_t dist)
{
    Acc b = wrap(acc24(m(wnode + 4)) - acc24(dist));
    b = set_a1(b, a1(b) & 0xFFFFF0);
    while (b < 0) b = wrap(b + acc24(m(wnode + 6)) + acc24(1));
    m(rnode + 4) = lim(b);
}

// ---- the reverb, effect 4 ----
void Gp::reverb()
{
    const uint32_t S = xs[4];
    uint32_t f = m(S + 4);
    if (!(f & 1)) { // p:0005 first frame
        m(S + 4) = f | 1;
        // p:0007 each line's base: line k+1 = line k + its size (word 2k+1) + 1
        m(S + 0x2A) = m(S + 0);
        for (uint32_t k = 0; k < 13; k++) {
            Acc a = wrap(acc24(m(S + 0x2A + 2 * k)) + acc24(m(S + 0x2B + 2 * k)));
            a = wrap(a + acc24(1));
            m(S + 0x2C + 2 * k) = lim(a);
        }
        // p:0019 two coefficient sets (used by p:035F, which nothing calls)
        static const uint32_t k1[5] = {0x424778, 0x4B40E6, 0x424778, 0x23EB20, 0xDCBAB6};
        static const uint32_t k2[5] = {0x424778, 0x045B75, 0x424778, 0x117DE1, 0x8F1750};
        for (int i = 0; i < 5; i++) m(S + 0xB3 + i) = k1[i], m(S + 0xB8 + i) = k2[i];
        // p:017B
        rv_tables();                                                  // p:0197
        for (uint32_t i = 0; i < 6; i++) m(S + 0x1F8 + i) = m(S + 0x63 + i); // p:018B
        m(S + 526) = 0xFFFFFF, m(S + 511) = 0, m(S + 510) = 0;      // p:007C -> p:0089
        rv_init();                                                    // p:0386
    }
    f = m(S + 4); // p:0043
    if (f & 4) {
        m(S + 4) = f & ~4u;
        rv_tables();                                                  // p:0184: p:0197
        m(S + 526) = 0, m(S + 511) = 0, m(S + 510) = 1;              // p:007F
        rv_update_taps();                                             // p:0443
    }
    rv_process(); // p:01A0
}

// p:0197: the 31 working buffers' addresses, 16 words apart from word 11's
void Gp::rv_tables()
{
    const uint32_t S = xs[4];
    uint32_t a = m(S + 0x0B);
    for (uint32_t i = 0; i < 31; i++) m(S + 0x0B + i) = a, a = (a + 0x10) & 0xFFFFFF;
}

// p:0386: the DMA chains, then the taps
void Gp::rv_init()
{
    const uint32_t S = xs[4];
    // p:03EB twice: 14 write nodes (control 0x4BD2) from S+0xBD, then 31 read nodes (0x4BD0); count 16, offset 0
    uint32_t r0 = S + 0xBD, r1 = r0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t n = pass ? 31 : 14, control = pass ? 0x4BD0 : 0x4BD2;
        r1 = r0;
        for (uint32_t i = 0; i < n; i++) {
            m(r1) = r0;
            m(r0 + 1) = control;
            m(r0 + 2) = 0x10;
            m(r0 + 4) = 0;
            r1 = r0;
            r0 += 7;
        }
        m(r1) = 0x4000;
    }
    // p:0399 the write nodes' DSP addresses
    r0 = S + 0xC0;
    m(r0) = m(S + 11), r0 += 7;
    uint32_t a = m(S + 25);
    for (int i = 0; i < 3; i++) m(r0) = a, r0 += 7, a += 0x20;
    a = m(S + 12);
    for (int i = 0; i < 8; i++) m(r0) = a, r0 += 7, a += 0x10;
    a = m(S + 20);
    for (int i = 0; i < 2; i++) m(r0) = a, r0 += 7, a += 0x10;
    // p:03B1 the read nodes' DSP addresses
    r0 = S + 0x122;
    a = m(S + 11);
    for (int i = 0; i < 31; i++) m(r0) = a, r0 += 7, a += 0x10;
    // p:03BB the read nodes' lines: 13 on line 0, two each on lines 0-3, one each on lines 4-13
    r0 = S + 0x11F;
    r1 = S + 0x2A;
    uint32_t x0 = m(r1), x1 = m(r1 + 1);
    for (int i = 0; i < 13; i++) m(r0 + 5) = x0, m(r0 + 6) = x1, r0 += 7;
    for (int k = 0; k < 4; k++) {
        x0 = m(r1++), x1 = m(r1++);
        for (int j = 0; j < 2; j++) m(r0 + 5) = x0, m(r0 + 6) = x1, r0 += 7;
    }
    for (int k = 0; k < 10; k++) {
        x0 = m(r1++), m(r0 + 5) = x0;
        x1 = m(r1++), m(r0 + 6) = x1;
        r0 += 7;
    }
    // p:03DA the write nodes' lines 0-13
    r0 = S + 0xBD;
    r1 = S + 0x2A;
    for (int k = 0; k < 14; k++) {
        x0 = m(r1++), m(r0 + 5) = x0;
        x1 = m(r1++), m(r0 + 6) = x1;
        r0 += 7;
    }
    rv_set_taps();    // p:03F9: p:03FE
    rv_update_taps(); // p:0443
}

// p:03FE: the taps set only at start
void Gp::rv_set_taps()
{
    const uint32_t S = xs[4];
    // read nodes 0-4 on write node 0: words 0x46-0x4A
    for (uint32_t i = 0; i < 5; i++) {
        Acc a = acc24(m(S + 0x46 + i)); // + y0 (0)
        rv_tap(S + 0xBD, S + 0x11F + 7 * i, lim(a));
    }
    // read nodes 5-12 on write node 0: words 0x93-0x9A + word 0x4B
    for (uint32_t i = 0; i < 8; i++) {
        Acc a = wrap(acc24(m(S + 0x93 + i)) + acc24(m(S + 0x4B)));
        rv_tap(S + 0xBD, S + 0x142 + 7 * i, lim(a));
    }
    // read nodes 21-24 on write nodes 4-7: words 0x54-0x57
    for (uint32_t i = 0; i < 4; i++) rv_tap(S + 0xD9 + 7 * i, S + 0x1B2 + 7 * i, m(S + 0x54 + i));
    // read nodes 29, 30 on write nodes 12, 13: word 0x58
    for (uint32_t i = 0; i < 2; i++) rv_tap(S + 0x111 + 7 * i, S + 0x1EA + 7 * i, m(S + 88));
}

// p:0443: the taps the parameters change
void Gp::rv_update_taps()
{
    const uint32_t S = xs[4];
    // read nodes 13-20, two on each of write nodes 0-3: words 0x4C-0x53
    uint32_t r2 = S + 0x4C, r1 = S + 0x17A;
    for (uint32_t w = 0; w < 4; w++)
        for (uint32_t j = 0; j < 2; j++) {
            Acc a = acc24(m(r2++)); // + y0 (0)
            rv_tap(S + 0xBD + 7 * w, r1, lim(a));
            r1 += 7;
        }
    // read nodes 25-28 on write nodes 8-11: words 0x9B-0x9E
    for (uint32_t i = 0; i < 4; i++) rv_tap(S + 0xF5 + 7 * i, S + 0x1CE + 7 * i, m(S + 0x9B + i));
}

// p:0154: six parameters (S+0x1F8..0x1FD) approach their targets (S+0x63..0x68) for 0x150 frames, then take them
void Gp::rv_param_ramp()
{
    const uint32_t S = xs[4];
    Acc a = acc24(m(S + 526));
    if (a < 0) return;
    if (a1(a) == 0x150) {
        for (uint32_t i = 0; i < 6; i++) m(S + 0x1F8 + i) = m(S + 0x63 + i);
        m(S + 526) = 0xFFFFFF;
        return;
    }
    m(S + 526) = lim(wrap(a + acc24(1)));
    for (uint32_t i = 0; i < 6; i++) {
        Acc cur = acc24(m(S + 0x1F8 + i)), b = acc24(m(S + 0x63 + i));
        Acc d = wrap(cur - b);
        uint32_t y0 = lim(d);
        Acc p = mul(0x7C49BA, y0);
        b = wrap(b + p);
        m(S + 0x1F8 + i) = lim(b);
    }
}

// p:0098: when the parameters change, each group of taps in turn is faded out over 8 frames, moved, faded in
void Gp::rv_fade()
{
    const uint32_t S = xs[4];
    if (m(S + 510) == 0) return;
    uint32_t phase = m(S + 511);
    bool start = false;
    if (phase == 0) {
        uint32_t x0 = m(S + 510);
        Acc a = 0;
        if (x0 & 1) { // p:00DB
            m(S + 517) = S + 0x46;
            m(S + 515) = S + 0xBD;
            m(S + 514) = S + 0x11F;
            m(S + 516) = 0;
        } else if (x0 & (1 << 5)) { // p:00EA
            m(S + 515) = S + 0xBD;
            m(S + 514) = S + 0x142;
            m(S + 517) = S + 0x206;
            uint32_t x = m(S + 75);
            for (uint32_t i = 0; i < 8; i++) m(S + 0x206 + i) = lim(wrap(acc24(m(S + 0x93 + i)) + acc24(x)));
            m(S + 516) = 0;
        } else if (x0 & (1 << 13)) { // p:0119
            m(S + 517) = S + 0x54;
            m(S + 515) = S + 0xD9;
            m(S + 514) = S + 0x1B2;
            m(S + 516) = 7;
        } else if (x0 & (1 << 17)) { // p:0104
            m(S + 515) = S + 0x111;
            m(S + 514) = S + 0x1EA;
            m(S + 517) = S + 0x206;
            m(S + 0x206) = m(S + 88);
            m(S + 0x207) = m(S + 88);
            m(S + 516) = 7;
        } else if (x0 & (1 << 19)) { // p:014A: done
            m(S + 526) = 0xFFFFFF, m(S + 511) = 0, m(S + 510) = 0; // p:0089
            m(S + 511) = 0;                                         // p:014B
            m(S + 510) = lim(wrap(acc24(m(S + 510)) << 1));
            return;
        } else { // p:00CD: the next node
            m(S + 514) = lim(wrap(acc24(m(S + 514)) + acc24(7)));
            m(S + 515) = lim(wrap(acc24(m(S + 515)) + acc24(m(S + 516))));
        }
        (void)a;
        m(S + 512) = 0xFF0000; // p:012D: fade out
        m(S + 513) = 0x7F0000;
        start = true;
    } else if (phase == 0x80) { // p:0135: faded out: move the tap, fade in
        uint32_t r0 = m(S + 517);
        m(S + 517) = r0 + 1;
        rv_tap(m(S + 515), m(S + 514), 0); // x1 = a after clr a
        m(S + 512) = 0x010000;
        start = true;
    } else if (phase == 0x100) { // p:014B: faded in: the next group
        m(S + 511) = 0;
        m(S + 510) = lim(wrap(acc24(m(S + 510)) << 1));
        return;
    }
    (void)start;
    // p:00AA: the read node's buffer scaled by the fade level, the level stepped every sample (the loop ends at
    // p:00B8, its add)
    uint32_t r0 = m(m(S + 514) + 3);
    Acc b = acc24(m(S + 513));
    uint32_t y0 = m(S + 512);
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t x1 = lim(b), x0 = m(r0);
        m(r0++) = lim(mul(x1, x0));
        b = wrap(b + acc24(y0));
    }
    m(S + 513) = lim(b);
    m(S + 511) = lim(wrap(acc24(m(S + 511)) + acc24(0x10)));
}


// p:01A0, one frame of the reverb
void Gp::rv_process()
{
    const uint32_t S = xs[4];
    auto W = [&](uint32_t k) { return m(S + k); };
    // p:0053: (r0)+ *= x0, truncated
    // (each block of 16 words lies in one memory region: a pointer per block, sample by sample in the DSP's order)
    auto scale = [&](uint32_t &r0, uint32_t x0) {
        uint32_t *p = &m(r0);
        for (int i = 0; i < 16; i++) p[i] = lim(mul(p[i], x0));
        r0 += 16;
    };
    // p:0059: (r1)+ = rnd((r1) + (r0)+ * x0)
    auto macr = [&](uint32_t &r0, uint32_t &r1, uint32_t x0) {
        uint32_t *s = &m(r0), *d = &m(r1);
        for (int i = 0; i < 16; i++) d[i] = lim(rnd(wrap(acc24(d[i]) + mul(s[i], x0))));
        r0 += 16, r1 += 16;
    };
    // p:0060: (r1)+ += (r0)+
    auto add = [&](uint32_t &r0, uint32_t &r1) {
        uint32_t *s = &m(r0), *d = &m(r1);
        for (int i = 0; i < 16; i++) d[i] = lim(wrap(acc24(d[i]) + acc24(s[i])));
        r0 += 16, r1 += 16;
    };
    // p:0067: (r2)+ = (r1)+ + (r0)+
    auto add3 = [&](uint32_t &r0, uint32_t &r1, uint32_t &r2) {
        uint32_t *s0 = &m(r0), *s1 = &m(r1), *d = &m(r2);
        for (int i = 0; i < 16; i++) d[i] = lim(wrap(acc24(s1[i]) + acc24(s0[i])));
        r0 += 16, r1 += 16, r2 += 16;
    };
    // p:004E: (r1)+ = (r0)+
    auto copy = [&](uint32_t &r0, uint32_t &r1) {
        uint32_t *s = &m(r0), *d = &m(r1);
        for (int i = 0; i < 16; i++) d[i] = s[i];
        r0 += 16, r1 += 16;
    };
    // p:0348: one pole, state (r1), coefficients (r1+1) on the state and (r1+2) on the input; (r0)+ in place
    auto pole = [&](uint32_t &r0, uint32_t r1) {
        uint32_t *st = &m(r1), *p = &m(r0), y0 = st[1], y1 = st[2];
        for (int i = 0; i < 16; i++) {
            Acc a = wrap(mul(y0, st[0]) + mul(y1, p[i]));
            st[0] = lim(a);
            p[i] = lim(a);
        }
        r0 += 16;
    };
    // p:0353: 16 samples to 32, each twice (p:035E, the filter after it, is an rts)
    auto hold = [&](uint32_t r0, uint32_t r1) {
        uint32_t *s = &m(r0), *d = &m(r1);
        for (int i = 0; i < 16; i++) d[2 * i] = d[2 * i + 1] = s[i];
    };

    dma(S + 0x11F); // p:033C: the read chain (the taps into the working buffers)
    // p:01A9 the input (bin 10) halved twice, low-passed twice, every other sample kept
    uint32_t r0 = W(5), r1, r2, r3;
    scale(r0, 0x400000);
    scale(r0, 0x400000);
    r0 = W(5);
    pole(r0, S + 0x6A);
    pole(r0, S + 0x6A);
    {
        uint32_t *p = &m(W(5));
        for (int i = 0; i < 16; i++) p[i] = p[2 * i];
    }
    rv_param_ramp(); // p:0093
    rv_fade();
    // p:01CE
    r1 = W(12);
    for (int k = 0; k < 4; k++) r0 = W(11), add(r0, r1);
    r0 = W(32), r1 = W(12), r2 = S + 0x8F;
    for (int k = 0; k < 4; k++) macr(r0, r1, m(r2++));
    // p:01E5 two signed sources into each of four buffers
    r3 = S + 0x59, r0 = W(16), r2 = W(36);
    for (int o = 0; o < 4; o++) {
        for (int j = 0; j < 2; j++) {
            r1 = r2;
            macr(r0, r1, m(r3++));
        }
        r2 += 16;
    }
    // p:01F7
    r0 = W(25), r1 = W(26), r2 = W(20);
    add3(r0, r1, r2);
    r0 = W(24);
    add3(r0, r1, r2);
    r0 = W(28), r1 = W(20);
    add(r0, r1), add(r0, r1);
    r0 = W(31), r1 = W(20);
    add(r0, r1);
    r0 = W(30);
    add(r0, r1);
    r0 = W(20), r1 = W(21), r2 = W(22);
    add3(r0, r1, r2);
    r0 = W(22);
    scale(r0, W(98));
    r0 = W(20);
    scale(r0, W(97)), scale(r0, W(97));
    r0 = W(40), r1 = W(20);
    add(r0, r1), add(r0, r1);
    r0 = W(20), r1 = W(40);
    copy(r0, r1), copy(r0, r1);
    r0 = W(20);
    pole(r0, S + 0x82);
    pole(r0, S + 0x85);
    // p:0249 blocks 0, 2, 4 from W25 through the poles at S+0x6D, 0x70, 0x73
    r0 = W(25), r1 = S + 0x6D;
    for (int k = 0; k < 3; k++) {
        pole(r0, r1);
        r0 += 16;
        r1 += 3;
    }
    // p:0257 the 4x4 matrix: W16 block o, sample i = sum over j of (W36 block j, sample i) * c[o][j], rounded each
    {
        // block o's coefficients start at S+0x8B-o (the DSP's r3, one lower each block)
        const uint32_t *src = &m(W(36)), *c = &m(S + 0x88);
        uint32_t *dst = &m(W(16));
        for (int o = 0; o < 4; o++)
            for (int i = 0; i < 16; i++) {
                Acc a = 0;
                for (int j = 0; j < 4; j++) a = rnd(wrap(a + mul(c[3 - o + j], src[16 * j + i])));
                dst[16 * o + i] = lim(a);
            }
    }
    // p:026F
    r1 = W(16);
    for (int k = 0; k < 4; k++) r0 = W(22), add(r0, r1);
    r0 = W(16), r3 = S + 0x76;
    for (int k = 0; k < 4; k++) {
        pole(r0, r3);
        r3 += 3;
    }
    r0 = W(5), r1 = W(11);
    copy(r0, r1);
    // p:028A the outputs
    const bool four = W(105) == 4;
    auto out = [&](uint32_t r0p, uint32_t r1p, uint32_t r2p, uint32_t r3p, uint32_t r4p, uint32_t y0, uint32_t y1, bool pair) {
        uint32_t p0 = m(r0p), p1 = m(r0p + 1);
        const uint32_t *s1 = &m(r1p), *s2 = &m(r2p), *s3 = &m(r3p);
        uint32_t *d = &m(r4p);
        if (pair) y1 = lim(acc24(y1) >> 1); // p:0321: asr
        for (int i = 0; i < 16; i++) {
            if (pair) {
                Acc b = mul(s1[i + 16], p1);
                Acc a = mul(s1[i], p0);
                b = wrap(b + mul(s2[i + 16], y0));
                if (!xemu_macr_x1_y0_b) b = rnd(b); // xemu skips this rounding (emu_macr_p_x1_y0_b rounds before adding)
                a = rnd(wrap(a + mul(s2[i], y0)));
                uint32_t x1 = s3[i];
                b = rnd(wrap(b + mul(y1, x1)));
                a = rnd(wrap(a + mul(y1, x1)));
                b = wrap(b << 7), a = wrap(a << 7);
                d[i + 16] = lim(b);
                d[i] = lim(a);
            } else { // p:030D
                Acc a = mul(s1[i + 16], p1);
                a = rnd(wrap(a + mul(s1[i], p0)));
                a = rnd(wrap(a + mul(s2[i + 16], y0)));
                a = rnd(wrap(a + mul(s2[i], y0)));
                a = rnd(wrap(a + mul(y1, s3[i])));
                a = wrap(a << 7);
                d[i] = lim(a);
            }
        }
    };
    out(S + 0x1F8, W(12), W(36), W(40), W(32), W(508), W(509), four);
    out(S + 0x1FA, W(14), W(38), W(41), four ? W(34) : W(33), W(508), W(509), four);
    // p:02C2 each output to 48 kHz and onto its bin: W34 -> word 9's, W35 -> 10's (four only), W32 -> 7's, W33 -> 8's
    auto to_bin = [&](uint32_t src, uint32_t bin) {
        hold(src, W(38));
        uint32_t s = W(38), d = m(S + bin);
        add(s, d), add(s, d);
    };
    if (four) to_bin(W(34), 9), to_bin(W(35), 10);
    to_bin(W(32), 7);
    to_bin(W(33), 8);
    dma(S + 0xBD); // p:0342: the write chain (the working buffers into the lines)
}

} // namespace gpfx
