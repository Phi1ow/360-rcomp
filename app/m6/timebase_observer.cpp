// Read-only observer for a diagnostic AOT title, sharing its calibrated MFTB.
// No guest state or clock is changed. Called by one observer thread at 1 Hz.
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "rcomp/ppc_prelude.h"


namespace {
struct ClockSnapshot {
    uint64_t monotonic_ns = 0, realtime_ns = 0, tsc = 0, guest_tb = 0;
    uint64_t swaps = 0, vblanks = 0;
};

uint64_t ordered_tsc() {
#if defined(__x86_64__)
    __builtin_ia32_lfence();
    const uint64_t tsc = __builtin_ia32_rdtsc();
    __builtin_ia32_lfence();
    return tsc;
#else
    return 0;
#endif
}

bool snapshot(ClockSnapshot* out, uint64_t swaps, uint64_t vblanks) {
    timespec monotonic{}, realtime{};
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic) != 0 ||
        clock_gettime(CLOCK_REALTIME, &realtime) != 0) return false;
    out->tsc = ordered_tsc();
    out->guest_tb = PPC_MFTB();
    out->monotonic_ns = uint64_t(monotonic.tv_sec) * 1000000000ull + uint64_t(monotonic.tv_nsec);
    out->realtime_ns = uint64_t(realtime.tv_sec) * 1000000000ull + uint64_t(realtime.tv_nsec);
    out->swaps = swaps; out->vblanks = vblanks;
    return true;
}

bool report(FILE* log, const ClockSnapshot& first, const ClockSnapshot& last, bool counters) {
    if (last.monotonic_ns <= first.monotonic_ns || last.realtime_ns <= first.realtime_ns ||
        last.tsc < first.tsc || last.guest_tb < first.guest_tb ||
        (counters && (last.swaps < first.swaps || last.vblanks < first.vblanks))) {
        fprintf(log, "RCOMP-TIMEBASE FAIL non-monotonic observed clock\n");
        return false;
    }
    const uint64_t mono_delta = last.monotonic_ns - first.monotonic_ns;
    const uint64_t real_delta = last.realtime_ns - first.realtime_ns;
    const uint64_t tsc_delta = last.tsc - first.tsc;
    const uint64_t tb_delta = last.guest_tb - first.guest_tb;
    const double seconds = double(mono_delta) / 1e9;
    const double ratio = double(tb_delta) / double(RCOMP_GUEST_TIMEBASE_HZ) / seconds;
    const bool ok = ratio > 0.99 && ratio < 1.01;
    char line[1024];
    const int prefix = snprintf(line, sizeof(line), "RCOMP-TIMEBASE %s mono_now_ns=%llu real_now_ns=%llu mono_ns=%llu real_ns=%llu tsc_delta=%llu guest_tb_delta=%llu tsc_hz=%.0f guest_hz=%.0f guest_seconds_per_mono=%.9f real_per_mono=%.9f",
            ok ? "PASS" : "FAIL", (unsigned long long)last.monotonic_ns,
            (unsigned long long)last.realtime_ns, (unsigned long long)mono_delta,
            (unsigned long long)real_delta, (unsigned long long)tsc_delta,
            (unsigned long long)tb_delta, double(tsc_delta) / seconds, double(tb_delta) / seconds,
            ratio, double(real_delta) / double(mono_delta));
    if (prefix < 0 || size_t(prefix) >= sizeof(line)) {
        fprintf(log, "RCOMP-TIMEBASE FAIL format overflow\n");
        return false;
    }
    if (counters) {
        const int suffix = snprintf(line + prefix, sizeof(line) - size_t(prefix),
                         " swaps_delta=%llu swaps_hz=%.6f vblanks_delta=%llu vblank_hz=%.6f",
                         (unsigned long long)(last.swaps - first.swaps), double(last.swaps - first.swaps) / seconds,
                         (unsigned long long)(last.vblanks - first.vblanks), double(last.vblanks - first.vblanks) / seconds);
        if (suffix < 0 || size_t(suffix) >= sizeof(line) - size_t(prefix)) {
            fprintf(log, "RCOMP-TIMEBASE FAIL format overflow\n");
            return false;
        }
    }
    // One stdio call keeps the window together amid concurrent diagnostics.
    fprintf(log, "%s\n", line);
    fflush(log);
    return ok;
}
}  // namespace

extern "C" void rcomp_timebase_probe_identity(FILE* log) {
    uint32_t max_extended = 0, invariant = 0;
#if defined(__x86_64__)
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000u), "c"(0u));
    max_extended = eax;
    if (max_extended >= 0x80000007u) {
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000007u), "c"(0u));
        invariant = (edx >> 8) & 1u;
    }
#endif
    const uint64_t derived_hz = rcomp_tsc_to_tb ? (RCOMP_GUEST_TIMEBASE_HZ << 32) / rcomp_tsc_to_tb : 0;
    fprintf(log, "RCOMP-TIMEBASE ID max_extended=0x%08X invariant_tsc_bit=%u guest_hz=%llu tsc_origin=%llu tb_origin=%llu scale_32_32=%llu calibrated_hz=%llu",
            max_extended, invariant, (unsigned long long)RCOMP_GUEST_TIMEBASE_HZ,
            (unsigned long long)rcomp_tsc_origin, (unsigned long long)rcomp_tb_origin,
            (unsigned long long)rcomp_tsc_to_tb, (unsigned long long)derived_hz);
    fputc('\n', log); fflush(log);
}

// Call from a single observer thread at roughly 1 Hz. Counters are supplied
// by PRIME from real guest swaps and the real Xenos bridge vblank count.
extern "C" void rcomp_timebase_probe_snapshot(FILE* log, uint64_t swaps, uint64_t vblanks) {
    static ClockSnapshot previous{};
    static bool previous_valid = false;
    ClockSnapshot current{};
    if (!snapshot(&current, swaps, vblanks)) {
        fprintf(log, "RCOMP-TIMEBASE FAIL clock_gettime error\n"); fflush(log); return;
    }
    if (previous_valid) report(log, previous, current, true);
    else rcomp_timebase_probe_identity(log);
    previous = current; previous_valid = true;
}

#if defined(RCOMP_TIMEBASE_PROBE_MAIN)
int main() {
    rcomp_timebase_probe_identity(stdout);
    int failed = 0;
    for (unsigned i = 0; i < 6; ++i) {
        ClockSnapshot first{}, last{};
        if (!snapshot(&first, 0, 0)) return 2;
        timespec sleep{1, 0};
        while (nanosleep(&sleep, &sleep) != 0) {
            if (errno != EINTR) { fprintf(stderr, "RCOMP-TIMEBASE FAIL nanosleep errno=%d\n", errno); return 2; }
        }
        if (!snapshot(&last, 0, 0)) return 2;
        failed += !report(stdout, first, last, false);
    }
    fprintf(stdout, "RCOMP-TIMEBASE %s standalone six windows failed=%d; PS5 proof requires console output\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
#endif
