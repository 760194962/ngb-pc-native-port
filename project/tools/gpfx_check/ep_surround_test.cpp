// EP module 0 (Dolby Surround encode) on xemu's DSP56300 core (xemu_ep.c) and loader/src/audio/ep_surround.cpp side
// by side: every output word compared. xemu runs the module's 256-sample frames; the translation is given blocks of
// random length, as the mixer would.
//   ep_surround_test <DOLBY section> <frames> <seed> <amplitude> [tone]
// amplitude 1.0 is full scale; above it the inputs clip at the 24-bit range (the filters then saturate too).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "ep_surround.h"

extern "C" {
int xep_init(const uint8_t *sec, size_t n);
void xep_frame(const int32_t *const in[5], int32_t *const out[2]);
}

static uint64_t rng;
static double rnd01() { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL; return (double)(rng >> 11) / 9007199254740992.0; }

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: ep_surround_test <DOLBY section> <frames> <seed> <amplitude> [tone]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static uint8_t sec[0x8000];
    size_t secn = fread(sec, 1, sizeof sec, f);
    fclose(f);
    int frames = atoi(argv[2]);
    rng = strtoull(argv[3], nullptr, 0);
    double amp = atof(argv[4]);
    bool tone = argc > 5;
    if (xep_init(sec, secn)) { fprintf(stderr, "section too short\n"); return 2; }
    size_t total = (size_t)frames * 256;
    std::vector<int32_t> in[5], xo[2], to[2];
    for (int c = 0; c < 5; c++) in[c].resize(total);
    for (int k = 0; k < 2; k++) xo[k].resize(total), to[k].resize(total);
    for (size_t i = 0; i < total; i++)
        for (int c = 0; c < 5; c++) {
            double v = tone ? amp * sin(6.283185307179586 * (220.0 * (c + 1)) * i / 48000.0) : amp * (2 * rnd01() - 1);
            v *= 8388608.0;
            in[c][i] = v > 8388607 ? 8388607 : v < -8388608 ? -8388608 : (int32_t)lrint(v);
        }
    for (int fr = 0; fr < frames; fr++) {
        const int32_t *ip[5];
        for (int c = 0; c < 5; c++) ip[c] = &in[c][fr * 256];
        int32_t *op[2] = {&xo[0][fr * 256], &xo[1][fr * 256]};
        xep_frame(ip, op);
    }
    epds::Encoder e;
    e.reset();
    for (size_t at = 0; at < total;) {
        uint32_t n = 1 + (uint32_t)(rnd01() * 700);
        if (at + n > total) n = (uint32_t)(total - at);
        const int32_t *ip[5];
        for (int c = 0; c < 5; c++) ip[c] = &in[c][at];
        e.process(ip, &to[0][at], &to[1][at], n);
        at += n;
    }
    size_t diff = 0, first = total;
    int32_t peak = 0;
    for (size_t i = 0; i < total; i++)
        for (int k = 0; k < 2; k++) {
            peak = std::max(peak, std::abs(xo[k][i]));
            if (xo[k][i] != to[k][i]) { if (!diff) first = i; diff++; }
        }
    printf("%d frames, amplitude %.2f%s: %zu of %zu words differ", frames, amp, tone ? " (tones)" : "", diff, total * 2);
    if (diff) printf(", first at sample %zu (frame %zu): xemu %06x %06x, translation %06x %06x", first, first / 256,
                     xo[0][first] & 0xFFFFFF, xo[1][first] & 0xFFFFFF, to[0][first] & 0xFFFFFF, to[1][first] & 0xFFFFFF);
    printf("; output peak %06x\n", peak);
    return diff != 0;
}
