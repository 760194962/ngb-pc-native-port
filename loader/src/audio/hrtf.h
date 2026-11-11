// The Xbox's head-related transfer function for 3D voices, as the console does it. Ninja Gaiden Black calls
// DirectSoundUseFullHRTF at start (FUN_0031feb0, right after DirectSoundCreate): every 3D voice then goes through a
// per-ear 31-tap filter chosen by the source's azimuth and elevation, the far ear delayed (the APU's HRIR). The
// library's own tables and rules are used (docs/04 of the port project, audio): the filters and their index are read
// from the game's DSOUND code, the angles and the choice of filter follow its CDirectSound3DCalculator.
#pragma once

#include <cstdint>

namespace hrtf {

constexpr int TAPS = 31;
constexpr int HISTORY = 128; // power of two above TAPS + the largest delay (42 samples) + 1

struct Filter {
    float c[2][TAPS] = {}; // left, right ear coefficients
    float itd = 0;         // samples: > 0 the left ear is delayed, < 0 the right
};

struct State {
    float hist[HISTORY] = {};
    uint32_t pos = 0;
    Filter cur;
    bool valid = false; // cur set (the first block takes its target at once)
};

// Finds the library's HRTF tables in the loaded game image (false: not this library; no HRTF then).
bool load();
bool loaded();

// The library's angles in degrees (0x37D2C0): azimuth -180..180, > 0 to the right; elevation -90..90, > 0 up.
// rel: the source from the listener; front, top, right: the listener's axes (right = top x front).
void angles(const float rel[3], const float front[3], const float top[3], const float right[3], float &az, float &el);

// The library's choice of filter for those angles (0x37D88F) and the delay it gives the far ear (0x37FCB3).
// fold: speakers set to surround or Dolby Digital (sources behind are folded to the front, the rear speakers carry them).
void pick(float az, float el, bool fold, Filter &out);

// GetCenterVolume (CFullHRTFSource, x2.1 0x37D774): within 45 degrees of straight ahead a 3D voice's dry copy goes to
// the centre bin and its HRTF front pair is turned down: millibels for the front pair (bins 6 / 7) and the centre (bin 2),
// from the library's own tables. false: the tables were not found.
bool centre(float az, float el, int32_t &front_mb, int32_t &centre_mb);

// Filters n mono samples into left / right, moving from the state's filter to the target over the first 32 samples.
void process(State &s, const Filter &target, const float *in, float *left, float *right, uint32_t n);

} // namespace hrtf
