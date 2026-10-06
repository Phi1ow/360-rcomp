// Original R-comp six-channel downmix. No external audio implementation.
#pragma once
#include <bit>
#include <cstdint>

namespace rcomp::audio {
constexpr unsigned kFrames = 256;
constexpr unsigned kChannels = 6;
constexpr unsigned kInputSamples = kFrames * kChannels;
constexpr unsigned kStereoSamples = kFrames * 2;

inline bool finite_block(const float* input) noexcept {
  for (unsigned i = 0; i < kInputSamples; ++i) {
    if ((std::bit_cast<uint32_t>(input[i]) & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000)) return false;
  }
  return true;
}

inline int16_t pcm16(double value) noexcept {
  // Clamp before converting to integer, including finite oversized inputs.
  if (value >= 1.0) return INT16_MAX;
  if (value <= -1.0) return INT16_MIN;
  const double scaled = value * (value < 0.0 ? 32768.0 : 32767.0);
  return static_cast<int16_t>(scaled + (scaled < 0.0 ? -0.5 : 0.5));
}

inline void downmix(const float* input, int16_t* stereo) noexcept {
  // FL FR FC LFE SL SR -> stereo, shared centre/LFE and same-side surround.
  // Constant headroom keeps six unit-amplitude channels within native PCM.
  constexpr double centre = 0.70710678118654752440;
  constexpr double surround = centre;
  constexpr double lfe = 0.5;
  constexpr double gain = 1.0 / (1.0 + centre + surround + lfe);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    const float* x = input + frame * kChannels;
    const double shared = centre * double(x[2]) + lfe * double(x[3]);
    stereo[frame * 2] = pcm16((double(x[0]) + shared + surround * double(x[4])) * gain);
    stereo[frame * 2 + 1] = pcm16((double(x[1]) + shared + surround * double(x[5])) * gain);
  }
}
}  // namespace rcomp::audio
