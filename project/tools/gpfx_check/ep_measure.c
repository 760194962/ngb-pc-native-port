// Transfer of EP module 0 per input channel: a sine at f on one channel, amplitude and phase of Lt and Rt after settling.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
int xep_init(const uint8_t *sec, size_t n);
void xep_frame(const int32_t *const in[5], int32_t *const out[2]);
uint32_t xep_cmds(const uint32_t **log);
uint32_t xep_x(uint32_t a);

static uint8_t sec[0x8000];
static size_t secn;

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    FILE *f = fopen(argv[1], "rb");
    secn = fread(sec, 1, sizeof sec, f);
    fclose(f);
    const char *names[5] = {"L", "C", "R", "Ls", "Rs"};
    double freqs[] = {50, 100, 200, 500, 1000, 2000, 4000, 7000, 10000, 15000, 20000};
    for (int ch = 0; ch < 5; ch++)
        for (int fi = 0; fi < (int)(sizeof freqs / sizeof *freqs); fi++) {
            if (xep_init(sec, secn)) return 1;
            static int32_t in[5][256], o[2][256];
            const int32_t *ip[5] = {in[0], in[1], in[2], in[3], in[4]};
            int32_t *op[2] = {o[0], o[1]};
            double w = 2 * M_PI * freqs[fi] / 48000.0, amp = 0.25 * 8388608.0;
            double cr[2] = {0, 0}, ci[2] = {0, 0};
            const int frames = 200, skip = 100; // 0.53 s settle, 0.53 s measured
            for (int fr = 0; fr < frames; fr++) {
                memset(in, 0, sizeof in);
                for (int i = 0; i < 256; i++) in[ch][i] = (int32_t)lrint(amp * sin(w * (fr * 256 + i)));
                xep_frame(ip, op);
                if (fr == 0 && ch == 0 && fi == 0) {
                    const uint32_t *log;
                    uint32_t n = xep_cmds(&log);
                    printf("commands:");
                    for (uint32_t k = 0; k < n; k++) printf(" %x/%x", log[k] >> 8, log[k] & 0xFF);
                    printf("\nx:604 %06x x:636 %06x x:637 %06x x:63b %06x x:68b %06x x:68c %06x\n", xep_x(0x604), xep_x(0x636),
                           xep_x(0x637), xep_x(0x63B), xep_x(0x68B), xep_x(0x68C));
                }
                if (fr >= skip)
                    for (int i = 0; i < 256; i++) {
                        double t = w * (fr * 256 + i);
                        for (int k = 0; k < 2; k++) cr[k] += o[k][i] * cos(t), ci[k] += o[k][i] * sin(t);
                    }
            }
            double n = (frames - skip) * 256.0;
            printf("%-2s %6.0f Hz:", names[ch], freqs[fi]);
            for (int k = 0; k < 2; k++) {
                double re = 2 * ci[k] / n / amp, im = 2 * cr[k] / n / amp; // relative to the input sine
                printf("  %s %7.2f dB %7.1f deg", k ? "Rt" : "Lt", 20 * log10(hypot(re, im) + 1e-12), atan2(im, re) * 180 / M_PI);
            }
            printf("\n");
        }
    return 0;
}
