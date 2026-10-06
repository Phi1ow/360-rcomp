#include "rcomp/audio_output.h"
#include <cstdio>

int main() {
  rcomp_audio_stream output = 123;
  float samples[256 * 6]{};
  if (rcomp_audio_open(&output) != RCOMP_AUDIO_UNSUPPORTED || output != 123 ||
      rcomp_audio_open(nullptr) != RCOMP_AUDIO_INVALID_ARGUMENT ||
      rcomp_audio_submit(123, samples) != RCOMP_AUDIO_UNSUPPORTED ||
      rcomp_audio_close(123) != RCOMP_AUDIO_UNSUPPORTED ||
      rcomp_audio_submit(0, samples) != RCOMP_AUDIO_INVALID_ARGUMENT ||
      rcomp_audio_close(0) != RCOMP_AUDIO_INVALID_ARGUMENT) return 1;
  std::puts("PASS host production backend explicitly UNSUPPORTED");
}
