// Host audio output: one 48 kHz float stream, stereo or 5.1, fed by the DirectSound HLE mixer.
#pragma once

#include <cstdint>

namespace audio_out {

constexpr uint32_t RATE = 48000;
constexpr uint32_t BLOCK = 480; // frames per submitted block (10 ms)

bool open(uint32_t channels); // 2, or 6 (FL FR C LFE BL BR); false: no host audio (blocks_wanted then follows the clock)
uint32_t blocks_wanted();     // how many more blocks the device can queue now (by the clock while it has none)
void submit(const float *lr); // one BLOCK of interleaved frames of the channels open() was given (copied)
void simulate_lost();         // tests: act as if the device had gone (XAudio2's critical error)

} // namespace audio_out
