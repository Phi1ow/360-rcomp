// Original synthetic probe: triangle tones, no title or game data.
// SDK compile/link is not PS5 audibility proof. Only PRIME may run this payload.
#include "rcomp/audio_output.h"
#include <array>
#include <cstdio>
#include <time.h>

int main() {
  rcomp_audio_stream stream = 0;
  const int opened = rcomp_audio_open(&stream);
  std::printf("RCOMP-AUDIO open status=%d stream=%llu\n", opened,
              static_cast<unsigned long long>(stream));
  if (opened != RCOMP_AUDIO_OK) return 1;
  std::array<float, 256 * 6> block{};
  unsigned phase = 0, accepted = 0, busy = 0;
  int failed = 0;
  for (unsigned channel = 0; channel < 6 && !failed; ++channel) {
    std::printf("RCOMP-AUDIO tone channel=%u hz=%u\n", channel, 440 + channel * 110);
    for (unsigned n = 0; n < 200 && !failed; ++n) {
      block.fill(0);
      for (unsigned frame = 0; frame < 256; ++frame) {
        phase = (phase + 440 + channel * 110) % 48000;
        const unsigned folded = phase < 24000 ? phase : 48000 - phase;
        block[frame * 6 + channel] = (float(folded) / 12000.0f - 1.0f) * 0.25f;
      }
      for (;;) {
        const int result = rcomp_audio_submit(stream, block.data());
        if (result == RCOMP_AUDIO_OK) { ++accepted; break; }
        if (result != RCOMP_AUDIO_BUSY) { failed = result; break; }
        ++busy;
        timespec wait{0, 1000000};
        nanosleep(&wait, nullptr);
      }
    }
  }
  timespec drain_window{0, 60000000};
  nanosleep(&drain_window, nullptr);
  const int closed = rcomp_audio_close(stream);
  std::printf("RCOMP-AUDIO accepted=%u busy=%u submit_error=%d close_status=%d; "
              "device playback and audibility require observation\n", accepted, busy, failed, closed);
  return failed || closed != RCOMP_AUDIO_OK ? 1 : 0;
}
