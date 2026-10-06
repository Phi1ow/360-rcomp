// The guest timebase behind PPC_MFTB (include/rcomp/ppc_prelude.h, patch 0007)
// runs at 50 MHz, the rate KeQueryPerformanceFrequency reports.
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "rcomp/ppc_prelude.h"

int main() {
    int fails = 0;
    for (int round = 0; round < 3; ++round) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        const uint64_t t0 = rcomp_guest_timebase();
        usleep(200000);
        const uint64_t t1 = rcomp_guest_timebase();
        clock_gettime(CLOCK_MONOTONIC, &b);
        const double host_s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
        const double rate = (t1 - t0) / host_s;
        const bool ok = rate > 49.5e6 && rate < 50.5e6;
        printf("timebase round %d: %.0f Hz over %.3f s %s\n", round, rate, host_s, ok ? "PASS" : "FAIL");
        fails += !ok;
    }
    return fails ? 1 : 0;
}
