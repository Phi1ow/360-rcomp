// Synthetic device proof: independently generated tones, no game data.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <time.h>
#include "rcomp/audio_output.h"

namespace {
uint64_t monotonic_ns() {
    timespec t{};
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}
}

extern "C" int rcomp_native_audio_probe() {
    rcomp_audio_stream stream = 0;
    const int opened = rcomp_audio_open(&stream);
    if (opened != RCOMP_AUDIO_OK) {
        fprintf(stderr, "RCOMP-AUDIO-PROBE FAIL open=%d\n", opened);
        return 1;
    }
    // Left 440 Hz then right 660 Hz, one second each at low amplitude.
    // The six-channel backend performs its normal channel mapping.
    constexpr uint32_t blocks = 375;
    constexpr double tau = 6.2831853071795864769;
    float frame[256 * 6];
    uint32_t accepted = 0, busy = 0;
    int failure = RCOMP_AUDIO_OK;
    const uint64_t begin = monotonic_ns();
    for (uint32_t block = 0; block < blocks; ++block) {
        for (uint32_t sample = 0; sample < 256; ++sample) {
            const uint32_t position = block * 256 + sample;
            const unsigned channel = position < 48000 ? 0 : 1;
            const double hz = channel ? 660.0 : 440.0;
            for (unsigned c = 0; c < 6; ++c) frame[sample * 6 + c] = 0;
            frame[sample * 6 + channel] = float(0.08 * std::sin(tau * hz * position / 48000.0));
        }
        for (;;) {
            const int result = rcomp_audio_submit(stream, frame);
            if (result == RCOMP_AUDIO_OK) { ++accepted; break; }
            if (result != RCOMP_AUDIO_BUSY) { failure = result; break; }
            ++busy;
            const uint64_t now = monotonic_ns();
            if (!now || !begin || now < begin || now - begin > 10000000000ull) {
                failure = RCOMP_AUDIO_DEVICE_ERROR;
                break;
            }
            timespec delay{0, 1000000};
            nanosleep(&delay, nullptr);
        }
        if (failure != RCOMP_AUDIO_OK) break;
    }
    // Allow the bounded device queue to drain; close cancels outstanding blocks.
    if (failure == RCOMP_AUDIO_OK) {
        timespec delay{0, 250000000};
        nanosleep(&delay, nullptr);
    }
    const int closed = rcomp_audio_close(stream);
    const uint64_t end = monotonic_ns();
    const double elapsed_ms = begin && end >= begin ? double(end - begin) / 1000000.0 : -1.0;
    const bool passed = failure == RCOMP_AUDIO_OK && closed == RCOMP_AUDIO_OK && accepted == blocks;
    fprintf(stderr, "RCOMP-AUDIO-PROBE %s accepted_blocks=%u busy_retries=%u submit_error=%d close=%d elapsed_ms=%.3f audible=NOT_TESTED\n",
            passed ? "PASS" : "FAIL", accepted, busy, failure, closed, elapsed_ms);
    fflush(stderr);
    return passed ? 0 : 1;
}
