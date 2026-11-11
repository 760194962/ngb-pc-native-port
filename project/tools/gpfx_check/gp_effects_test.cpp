// gp_effects_test IMAGE PATCH FRAMES [SEED] [AMPLITUDE]
// Runs effects 0-5 of the unscrambled image on xemu's DSP core (xemu_gp.c) and on gp_effects.cpp side by side, the
// same input in bins 6-10 (noise bursts and silence) each frame, and compares X memory, the mix buffer and the
// scratch memory after every frame. PATCH: the converted I3DL2 words ("index value" lines, word 4 and 0x46-0x87),
// applied after frame 200 as the library would.
#include "gp_effects.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

extern "C" {
int xgp_init(const uint8_t *img, size_t n, size_t scratch_bytes);
uint32_t xgp_read(uint32_t a);
void xgp_write(uint32_t a, uint32_t v);
uint8_t *xgp_scratch(void);
void xgp_frame(void);
}

static uint32_t w32(const std::vector<uint8_t> &d, uint32_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; }

int main(int argc, char **argv)
{
    if (argc < 4) return 1;
    FILE *f = fopen(argv[1], "rb");
    std::vector<uint8_t> img(1 << 20);
    img.resize(fread(img.data(), 1, img.size(), f));
    fclose(f);
    std::vector<std::pair<uint32_t, uint32_t>> patch;
    if (FILE *p = fopen(argv[2], "r")) {
        unsigned a, v;
        while (fscanf(p, "%x %x", &a, &v) == 2) patch.push_back({a, v});
        fclose(p);
    }
    int frames = atoi(argv[3]);
    unsigned seed = argc > 4 ? atoi(argv[4]) : 1;
    double amp = argc > 5 ? atof(argv[5]) : 0.5;

    const size_t scratch_bytes = 0x80000;
    xgp_init(img.data(), img.size(), scratch_bytes);

    uint32_t code_words = w32(img, 0x804), other = w32(img, 0x80C);
    uint32_t desc = 0x818 + (other + code_words) * 4;
    std::vector<std::vector<uint32_t>> st(6);
    const uint32_t *sp[6];
    uint32_t sw[6];
    for (int e = 0; e < 6; e++) {
        uint32_t at = w32(img, desc + 8 + 32 * e + 8), n = w32(img, desc + 8 + 32 * e + 12) / 4;
        for (uint32_t i = 0; i < n; i++) st[e].push_back(w32(img, at + 4 * i));
        sp[e] = st[e].data();
        sw[e] = n;
    }
    gpfx::Gp g;
    g.saturation_mode = false; // xemu has no SM
    g.xemu_macr_x1_y0_b = true;
    g.load(sp, sw);
    g.scratch.assign(scratch_bytes, 0);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(-1.0, 1.0);
    uint32_t xs4 = 0xF0;
    for (int fr = 0; fr < frames; fr++) {
        if (fr == 200 && !patch.empty()) {
            for (auto &pv : patch) {
                uint32_t v = pv.first == 4 ? (xgp_read(xs4 + 4) | 4) : (pv.second & 0xFFFFFF);
                xgp_write(xs4 + pv.first, v);
            }
            uint32_t words[0x42];
            for (auto &pv : patch)
                if (pv.first >= 0x46 && pv.first < 0x88) words[pv.first - 0x46] = pv.second & 0xFFFFFF;
            g.set_reverb_params(words);
        }
        // bins 0-5 random (the speakers' own mix), 6-10 bursts of noise: on for 20 frames in every 120
        bool on = (fr % 120) < 20;
        for (uint32_t b = 0; b < 11; b++)
            for (uint32_t i = 0; i < 32; i++) {
                double v = (b < 6 || on) ? uni(rng) * amp : 0.0;
                uint32_t w = (uint32_t)(int32_t)lrint(v * 8388607.0) & 0xFFFFFF;
                xgp_write(0x1400 + 32 * b + i, w);
                g.mix[32 * b + i] = w;
            }
        xgp_frame();
        g.frame();
        // compare
        int bad = 0;
        for (uint32_t a = 0; a < 0xC00; a++) { // X:0C00-0FFF is the mix buffer, compared below
            uint32_t want = xgp_read(a), got = g.x[a];
            if (want != got && bad++ < 24) printf("frame %d: X:%04x (S+%03x) xemu %06x ours %06x\n", fr, a, a - 0xF0, want, got);
        }
        if (bad) {
            // the first reverb output stage's inputs for the first differing sample of W33 (xemu's memory)
            uint32_t S = 0xF0, W12 = xgp_read(S + 12), W36 = xgp_read(S + 36), W40 = xgp_read(S + 40), W33 = xgp_read(S + 33);
            for (uint32_t i = 0; i < 16; i++)
                if (xgp_read(W33 + i) != g.x[W33 + i])
                    printf("W33[%u]: r1+16 %06x r2+16 %06x r3 %06x p1 %06x y0 %06x y1 %06x -> xemu %06x ours %06x\n", i,
                           xgp_read(W12 + 16 + i), xgp_read(W36 + 16 + i), xgp_read(W40 + i), xgp_read(S + 0x1F9),
                           xgp_read(S + 0x1FC), xgp_read(S + 0x1FD), xgp_read(W33 + i), g.x[W33 + i]);
            return 2;
        }
        for (uint32_t i = 0; i < 0x400; i++)
            if (xgp_read(0x1400 + i) != g.mix[i]) {
                printf("frame %d: X:%04x (bin %u sample %u) xemu %06x ours %06x\n", fr, 0x1400 + i, i / 32, i % 32,
                       xgp_read(0x1400 + i), g.mix[i]);
                return 3;
            }
        if (memcmp(xgp_scratch(), g.scratch.data(), scratch_bytes)) {
            for (size_t i = 0; i < scratch_bytes; i++)
                if (xgp_scratch()[i] != g.scratch[i]) {
                    printf("frame %d: scratch byte %zx differs\n", fr, i);
                    break;
                }
            return 4;
        }
    }
    printf("%d frames: X memory, mix buffer and scratch identical\n", frames);
    return 0;
}
