// Platform audio contract. PRIME owns this interface; Agent 2 owns backends.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef uint64_t rcomp_audio_stream;
enum rcomp_audio_status {
    RCOMP_AUDIO_OK = 0,
    RCOMP_AUDIO_UNSUPPORTED = 1,
    RCOMP_AUDIO_INVALID_ARGUMENT = 2,
    RCOMP_AUDIO_BUSY = 3,
    RCOMP_AUDIO_DEVICE_ERROR = 4,
    RCOMP_AUDIO_STOPPED = 5
};
// Xbox render input: 48 kHz, 256 frames, six native-endian interleaved floats
// per frame, ordered FL FR FC LFE surround-left surround-right.
// Open OK requires a real active device path, never a discard sink.
int rcomp_audio_open(rcomp_audio_stream* out);
// Copies all 256*6 floats before OK; caller's buffer may then be reused.
// An active device and a bounded queue are required. BUSY/error accepts none.
// Implementations are thread-safe and never retain a guest-memory pointer.
int rcomp_audio_submit(rcomp_audio_stream stream, const float* interleaved_6x256);
// Synchronous: rejects new submissions, cancels pending output and joins all
// native producers/consumers before returning. Idempotent for this stream.
// On failure the backend retains its ownership for a retry; no fake cleanup.
int rcomp_audio_close(rcomp_audio_stream stream);
#ifdef __cplusplus
}
#endif
