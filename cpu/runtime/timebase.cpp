// Guest timebase (mftb) from the invariant TSC; see rcomp_guest_timebase in
// include/rcomp/ppc_prelude.h. Calibrated once at start-up against
// CLOCK_MONOTONIC over 20 ms, so both clocks agree at the origin and drift is
// bounded by the calibration error (well below 0.1%).
#include <stdint.h>
#include <time.h>

extern "C" {
uint64_t rcomp_tsc_origin = 0, rcomp_tb_origin = 0, rcomp_tsc_to_tb = 0;
}

namespace {
constexpr uint64_t kTimebaseHz = 50000000ull;  // RCOMP_GUEST_TIMEBASE_HZ

uint64_t monotonic_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

struct Calibrate {
    Calibrate() {
#if defined(__x86_64__)
        const uint64_t ns0 = monotonic_ns(), tsc0 = __builtin_ia32_rdtsc();
        uint64_t ns1 = ns0, tsc1 = tsc0;
        while (ns1 - ns0 < 20000000ull) { ns1 = monotonic_ns(); tsc1 = __builtin_ia32_rdtsc(); }
        const uint64_t tsc_hz = (tsc1 - tsc0) * 1000000000ull / (ns1 - ns0);
        if (tsc_hz < kTimebaseHz) return;  // implausible: keep the clock_gettime path
        rcomp_tsc_origin = tsc1;
        rcomp_tb_origin = ns1 / (1000000000ull / kTimebaseHz);
        rcomp_tsc_to_tb = (kTimebaseHz << 32) / tsc_hz;
#endif
    }
} calibrate;
}  // namespace
