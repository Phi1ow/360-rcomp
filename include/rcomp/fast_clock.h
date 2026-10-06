// Shared interface (owner: PRIME; read by runtime/, gpu/xenos and app/).
//
// CLOCK_MONOTONIC without a system call. On the PS5 clock_gettime is a real system call (and
// more expensive while kstuff is active); the title already derives the guest timebase (mftb)
// from the invariant TSC, calibrated once at start-up against CLOCK_MONOTONIC
// (cpu/runtime/timebase.cpp). This header reads that same calibration, so the clocks the
// guest sees and the clocks the runtime waits on agree.
//
// Drift against the operating system's clock is the calibration error (measured: a few parts
// per million). Deadlines computed from this clock are therefore only comparable with this
// clock; a deadline handed to an operating-system wait must be rebuilt from the operating
// system's own clock (see wait_until in runtime/src/hle_xboxkrnl_threads.cpp).
#pragma once

#include <stdint.h>
#include <time.h>

extern "C" {
// Defined by cpu/runtime/timebase.cpp (32.32 fixed point). Weak: host tests and non-x86
// builds do not link it and keep the system call.
extern uint64_t rcomp_tsc_origin __attribute__((weak));
extern uint64_t rcomp_tb_origin __attribute__((weak));
extern uint64_t rcomp_tsc_to_tb __attribute__((weak));
}

namespace rcomp {

// True once the TSC is calibrated (x86-64 title builds). Calibration runs during static
// initialization, before main.
inline bool fast_clock_ready() {
#if defined(__x86_64__)
    return &rcomp_tsc_to_tb != nullptr && rcomp_tsc_to_tb != 0;
#else
    return false;
#endif
}

// Nanoseconds on the CLOCK_MONOTONIC timeline, resolution 20 ns (one tick of the 50 MHz guest
// timebase). Only valid while fast_clock_ready().
inline uint64_t fast_monotonic_ns_ready() {
#if defined(__x86_64__)
    const uint64_t delta = __builtin_ia32_rdtsc() - rcomp_tsc_origin;
    return (rcomp_tb_origin + uint64_t((unsigned __int128)delta * rcomp_tsc_to_tb >> 32)) * 20u;
#else
    return 0;
#endif
}

// The same, with the system call as the fallback when the TSC is not calibrated.
inline uint64_t fast_monotonic_ns() {
    if (__builtin_expect(fast_clock_ready(), 1)) return fast_monotonic_ns_ready();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

}  // namespace rcomp
