// The TSC-derived clocks (include/rcomp/fast_clock.h behind rt::monotonic_ns, rt::steady_now and
// rt::system_filetime): they agree with the operating system's clocks, never run backwards on a
// thread and keep FILETIME moving with the monotonic clock. cpu/runtime/timebase.cpp (the
// calibration) is linked into this test through rcomp_runtime_platform_deps.
#include <stdint.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "rcomp/fast_clock.h"
#include "rcomp/runtime/clock_sync.h"
#include "test_util.h"

using namespace rcomp::rt;

namespace {
uint64_t os_monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}
uint64_t os_filetime() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t(ts.tv_sec) + 11644473600ull) * 10000000ull + uint64_t(ts.tv_nsec) / 100;
}
int64_t distance(uint64_t a, uint64_t b) { return a > b ? int64_t(a - b) : -int64_t(b - a); }
}  // namespace

int main() {
    if (!rcomp::fast_clock_ready()) {
        printf("rt_test_fast_clock NOT TESTED: no calibrated TSC on this host\n");
        return 0;
    }
    // Agreement with the operating system: the best of several back-to-back pairs (a preemption
    // between the two reads only makes one pair worse).
    int64_t best = INT64_MAX;
    for (int i = 0; i < 20; ++i) {
        const uint64_t a = os_monotonic_ns();
        const uint64_t f = monotonic_ns();
        const uint64_t b = os_monotonic_ns();
        const int64_t off = distance(f, a + (b - a) / 2);
        best = std::min(best, off < 0 ? -off : off);
    }
    CHECK(best < 200000);  // 200 us: calibration error plus the time since the start-up calibration
    CHECK(rcomp::fast_monotonic_ns_ready() % 20 == 0);  // one 50 MHz timebase tick

    // Never backwards on one thread, including from several threads at once.
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            uint64_t prev = monotonic_ns();
            for (int i = 0; i < 500000; ++i) {
                const uint64_t now = monotonic_ns();
                if (now < prev) ++failures;
                prev = now;
            }
        });
    for (auto& t : threads) t.join();
    CHECK_EQ(failures.load(), 0);

    // Elapsed time over a sleep agrees with the operating system's clock to 0.1 % + 200 us.
    const uint64_t os0 = os_monotonic_ns(), f0 = monotonic_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint64_t os1 = os_monotonic_ns(), f1 = monotonic_ns();
    const int64_t error = distance(f1 - f0, os1 - os0);
    CHECK((error < 0 ? -error : error) < int64_t((os1 - os0) / 1000) + 200000);

    // steady_now() is the same timeline as std::chrono::steady_clock.
    int64_t steady_best = INT64_MAX;
    for (int i = 0; i < 20; ++i) {
        const auto a = std::chrono::steady_clock::now();
        const auto s = steady_now();
        const auto b = std::chrono::steady_clock::now();
        const int64_t off = std::chrono::duration_cast<std::chrono::nanoseconds>(s - (a + (b - a) / 2)).count();
        steady_best = std::min(steady_best, off < 0 ? -off : off);
    }
    CHECK(steady_best < 200000);

    // FILETIME starts at the wall clock and then advances with the monotonic clock.
    const uint64_t ft0 = system_filetime(), m0 = monotonic_ns();
    CHECK(distance(ft0, os_filetime()) < 100000000 && distance(ft0, os_filetime()) > -100000000);  // 10 ms
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t ft1 = system_filetime(), m1 = monotonic_ns();
    CHECK(ft1 >= ft0);
    const int64_t drift = distance((ft1 - ft0) * 100, m1 - m0);
    CHECK((drift < 0 ? -drift : drift) < 2000000);  // 2 ms
    return test_result("rt_test_fast_clock");
}
