#include "audio/hrtf.h"

#include "common.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace hrtf {
namespace {

constexpr int AZ_STEPS = 61, EL_STEPS = 31; // 0..180 in 3-degree steps, -90..90 in 6-degree steps

const uint16_t *g_index; // [azimuth step * 31 + elevation step] -> the far ear's 32-byte block; the near ear's follows
const uint8_t *g_blocks; // per block 31 taps and the delay in samples (byte 31)
uint32_t g_nblocks;
std::vector<float> g_taps; // the blocks' taps decoded

// A tap: sign-magnitude minifloat, 1 sign, 3 exponent, 4 mantissa bits, in 2048ths (0x40 = 1/16, 0x7F = 31/32): the
// format measured on retail hardware (hakuX-lr #166, from raddad772's probes; Ryzee119's xbox-apu-hrtf), applied as
// written. Read as plain signed bytes the front filter's tail would be a large negative lobe and its DC gain negative.
// (2.2.19 used 1024ths to make up for the 3D voices' dry centre feed, which it did not mix: docs/05 of the port project.)
float tap(uint8_t b)
{
    int e = (b >> 4) & 7, m = b & 15;
    float v = (e == 0 ? m : (16 + m) << (e - 1)) / 2048.0f;
    return (b & 0x80) ? -v : v;
}

// FUN_0037D88F: movzx ecx, word [esi*2 + index]; ...; shl ecx, 5; lea edx, [ecx + blocks]
const uint8_t kPattern[] = {0x0F, 0xB7, 0x0C, 0x75, 0, 0, 0, 0, 0xDF, 0xE0, 0xC1, 0xE1, 0x05, 0x8D, 0x91};

// CFullHRTFSource_GetCenterVolume (x2.1 0x37D774): shl edi, 2; fld [edi + centre]; shl esi, 2; fmul [esi + centre]; ...
// fld [edi + front]; fmul [esi + front]. Two tables of 45 floats, by whole degrees of azimuth and elevation.
const uint8_t kCentrePattern[] = {0xC1, 0xE7, 0x02, 0xD9, 0x87, 0, 0, 0, 0, 0xC1, 0xE6, 0x02, 0xD8, 0x8E};
const float *g_centre, *g_front_share;

// The library's linear-to-millibel conversions: amplitude (0x37A7AB, 2000 log10) and power (0x37D1F1, 1000 log10),
// <= 0 silent (-10000), >= 1 unity; cvttss2si truncates.
int32_t amp_mb(float x) { return x <= 0 ? -10000 : x >= 1 ? 0 : (int32_t)(2000.0f * log10f(x)); }

} // namespace

bool loaded() { return g_index != nullptr; }

bool load()
{
    if (g_index) return true;
    const uint8_t *img = (const uint8_t *)(uintptr_t)0x10000; // the XBE as mapped (its header first)
    uint32_t size = *(const uint32_t *)(img + 0x10C);           // SizeOfImage
    for (uint32_t i = 0; i + sizeof kPattern + 4 <= size; i++) {
        bool hit = true;
        for (uint32_t k = 0; k < sizeof kPattern && hit; k++) hit = (k >= 4 && k < 8) || img[i + k] == kPattern[k];
        if (!hit) continue;
        uint32_t index = *(const uint32_t *)(img + i + 4), blocks = *(const uint32_t *)(img + i + sizeof kPattern);
        if (index < 0x10000 || index - 0x10000 + AZ_STEPS * EL_STEPS * 2 > size || blocks < 0x10000 || blocks - 0x10000 >= size) continue;
        const uint16_t *idx = (const uint16_t *)(uintptr_t)index;
        uint32_t top = 0;
        bool ok = true;
        for (int k = 0; k < AZ_STEPS * EL_STEPS && ok; k++) {
            ok = (idx[k] & 1) == 0; // pairs: the far ear's block, then the near ear's
            if (idx[k] > top) top = idx[k];
        }
        if (!ok || blocks - 0x10000 + (top + 2) * 32 > size) continue;
        g_index = idx;
        g_blocks = (const uint8_t *)(uintptr_t)blocks;
        g_nblocks = top + 2;
        g_taps.resize(g_nblocks * TAPS);
        for (uint32_t b = 0; b < g_nblocks; b++)
            for (int t = 0; t < TAPS; t++) g_taps[b * TAPS + t] = tap(g_blocks[b * 32 + t]);
        logf("[dsound] HRTF: the library's tables at 0x%08x (index) and 0x%08x (%u filters of 31 taps a ear), from its "
             "filter choice at 0x%08x", index, blocks, g_nblocks / 2, 0x10000 + i);
        for (uint32_t j = 0; j + 40 <= size; j++) { // the centre / front shares of GetCenterVolume
            bool c = true;
            for (uint32_t k = 0; k < sizeof kCentrePattern && c; k++) c = (k >= 5 && k < 9) || img[j + k] == kCentrePattern[k];
            if (!c || *(const uint32_t *)(img + j + 5) != *(const uint32_t *)(img + j + 14)) continue;
            for (uint32_t k = j + 18; k + 12 <= j + 60 && k + 12 <= size; k++) // fld [edi + front] (D9 87), fmul [esi + front] (D8 8E)
                if (img[k] == 0xD9 && img[k + 1] == 0x87 && img[k + 6] == 0xD8 && img[k + 7] == 0x8E &&
                    *(const uint32_t *)(img + k + 2) == *(const uint32_t *)(img + k + 8)) {
                    g_centre = (const float *)(uintptr_t) * (const uint32_t *)(img + j + 5);
                    g_front_share = (const float *)(uintptr_t) * (const uint32_t *)(img + k + 2);
                    logf("[dsound] HRTF: the centre shares at 0x%08x and the front shares at 0x%08x (GetCenterVolume at 0x%08x); "
                         "straight ahead: centre %.3f, HRTF front %.3f", (uint32_t)(uintptr_t)g_centre,
                         (uint32_t)(uintptr_t)g_front_share, 0x10000 + j, g_centre[0] * g_centre[0],
                         1 - g_front_share[0] * g_front_share[0]);
                    break;
                }
            if (g_centre) break;
        }
        if (!g_centre) logf("[dsound] HRTF: GetCenterVolume's tables were not found; 3D voices feed no dry centre");
        return true;
    }
    logf("[dsound] HRTF: the library's tables were not found; 3D voices are panned as before");
    return false;
}

void angles(const float rel[3], const float front[3], const float top[3], const float right[3], float &az, float &el)
{
    auto dot = [&](const float *a) { return a[0] * rel[0] + a[1] * rel[1] + a[2] * rel[2]; };
    float f = dot(front), r = dot(right), u = dot(top);
    float d = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
    az = el = 0;
    if (d == 0) return;
    // octant approximations of atan, as the library computes them: 45 t below 45 degrees, 90 - 45 / t above
    float h = sqrtf(f * f + r * r), au = fabsf(u);
    el = au < h ? 45.0f * au / h : 90.0f - 45.0f * h / au;
    if (u < 0) el = -el;
    float af = fabsf(f), ar = fabsf(r);
    az = af > ar ? 45.0f * ar / af : ar == 0 ? 0.0f : 90.0f - 45.0f * af / ar;
    if (f < 0) az = 180.0f - az;
    if (r < 0) az = -az;
}

void pick(float az, float el, bool fold, Filter &out)
{
    if (!g_index) return;
    int q = (int)(el >= 0 ? el + 3.0f : el - 3.0f) / 6 * 6; // truncations, as cvttss2si and idiv
    int aq = abs(q), a;
    float aa = fabsf(az);
    if (aq == 90) a = 0;
    else if (aq > 60) a = (int)(aa + 6.0f) / 12 * 12;
    else if (aq > 30) a = (int)(aa + 3.0f) / 6 * 6;
    else a = (int)(aa + 1.5f) / 3 * 3;
    if (fold && a > 90) a = 180 - a;
    uint32_t ai = (uint32_t)(a / 3), ei = (uint32_t)((q + 90) / 6);
    if (ai >= AZ_STEPS) ai = 0;
    if (ei >= EL_STEPS) ei = 0;
    uint32_t far_block = g_index[ai * EL_STEPS + ei];
    // the far ear: the left one for a source on the right (azimuth >= 0), delayed by the far block's last byte
    const float *far_taps = &g_taps[far_block * TAPS], *near_taps = &g_taps[(far_block + 1) * TAPS];
    bool right_side = az >= 0;
    memcpy(out.c[0], right_side ? far_taps : near_taps, sizeof out.c[0]);
    memcpy(out.c[1], right_side ? near_taps : far_taps, sizeof out.c[1]);
    float delay = g_blocks[far_block * 32 + 31];
    out.itd = right_side ? delay : -delay;
}

bool centre(float az, float el, int32_t &front_mb, int32_t &centre_mb)
{
    if (!g_centre) return false;
    int a = (int)fabsf(az), e = (int)fabsf(el); // cvttss2si of the magnitudes
    if (a >= 45 || e >= 45) {
        front_mb = 0, centre_mb = -10000;
        return true;
    }
    centre_mb = amp_mb(g_centre[a] * g_centre[e]);
    front_mb = amp_mb(1.0f - g_front_share[a] * g_front_share[e]);
    return true;
}

void process(State &s, const Filter &target, const float *in, float *left, float *right, uint32_t n)
{
    if (!s.valid) {
        s.cur = target;
        s.valid = true;
    }
    // to a new target over one 32-sample frame of the voice processor, as its taps and delay ramp (hakuX-lr #166)
    bool moving = memcmp(&s.cur, &target, sizeof target) != 0;
    for (uint32_t i = 0; i < n; i++) {
        s.hist[s.pos] = in[i];
        float t = moving ? (i < 32 ? (i + 1) / 32.0f : 1.0f) : 0.0f;
        float itd = s.cur.itd + (target.itd - s.cur.itd) * t;
        for (int ch = 0; ch < 2; ch++) {
            float delay = ch == 0 ? (itd > 0 ? itd : 0) : (itd < 0 ? -itd : 0);
            int di = (int)delay;
            float fr = delay - di;
            const float *c0 = s.cur.c[ch], *c1 = target.c[ch];
            float acc = 0;
            for (int k = 0; k < TAPS; k++) {
                uint32_t p = (s.pos - di - k) & (HISTORY - 1);
                float x = s.hist[p];
                if (fr > 0) x = x * (1 - fr) + s.hist[(p - 1) & (HISTORY - 1)] * fr;
                acc += (moving ? c0[k] + (c1[k] - c0[k]) * t : c0[k]) * x;
            }
            (ch == 0 ? left : right)[i] = acc;
        }
        s.pos = (s.pos + 1) & (HISTORY - 1);
    }
    if (moving) s.cur = target;
}

} // namespace hrtf
