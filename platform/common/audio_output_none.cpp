#include "rcomp/audio_output.h"

extern "C" int rcomp_audio_open(rcomp_audio_stream* out) {
  return out ? RCOMP_AUDIO_UNSUPPORTED : RCOMP_AUDIO_INVALID_ARGUMENT;
}
extern "C" int rcomp_audio_submit(rcomp_audio_stream stream, const float* input) {
  return stream && input ? RCOMP_AUDIO_UNSUPPORTED : RCOMP_AUDIO_INVALID_ARGUMENT;
}
extern "C" int rcomp_audio_close(rcomp_audio_stream stream) {
  return stream ? RCOMP_AUDIO_UNSUPPORTED : RCOMP_AUDIO_INVALID_ARGUMENT;
}
