#include <atomic>
#include <thread>
#include <time.h>

#include "rcomp/runtime/clock_sync.h"
#include "test_util.h"

using namespace rcomp::rt;

int main() {
    // --- clock ---------------------------------------------------------------
    uint64_t prev = monotonic_ns();
    bool monotonic = true;
    for (int i = 0; i < 200000; ++i) {
        uint64_t now = monotonic_ns();
        monotonic &= now >= prev;
        prev = now;
    }
    CHECK(monotonic);
    const auto before_start = std::chrono::steady_clock::now();
    uint64_t t0 = monotonic_ns();
    const auto after_start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto before_end = std::chrono::steady_clock::now();
    uint64_t dt = monotonic_ns() - t0;
    const auto after_end = std::chrono::steady_clock::now();
    // Cygwin's sleep_for(20ms) was independently observed to return after
    // 19.6692ms with no R-comp code linked. Test our clock's elapsed units
    // against bracketing standard-clock measurements, not scheduler accuracy.
    timespec resolution{};
    CHECK(clock_getres(CLOCK_MONOTONIC, &resolution) == 0);
    const uint64_t quantum = uint64_t(resolution.tv_sec) * 1000000000ull + resolution.tv_nsec;
    const auto lower = std::chrono::duration_cast<std::chrono::nanoseconds>(before_end - after_start).count();
    const auto upper = std::chrono::duration_cast<std::chrono::nanoseconds>(after_end - before_start).count();
    CHECK(dt > 0);
    CHECK(dt + 2 * quantum >= uint64_t(lower));
    CHECK(dt <= uint64_t(upper) + 2 * quantum);
    CHECK(dt < 5000000000ull);
    // FILETIME: after 2024-01-01 (133485408000000000) and before 2100.
    uint64_t ft = system_filetime();
    CHECK(ft > 133485408000000000ull);
    CHECK(ft < 157766112000000000ull);

    // --- event: timeout, auto/manual reset -----------------------------------
    Event ev_auto(false);
    CHECK_ST(ev_auto.wait(1000000), Status::Timeout);  // 1 ms
    ev_auto.set();
    CHECK_ST(ev_auto.wait(0), Status::Ok);
    CHECK_ST(ev_auto.wait(0), Status::Timeout);  // consumed
    Event ev_man(true, true);
    CHECK_ST(ev_man.wait(0), Status::Ok);
    CHECK_ST(ev_man.wait(0), Status::Ok);  // stays signalled
    ev_man.reset();
    CHECK_ST(ev_man.wait(0), Status::Timeout);

    // --- 2 threads: handshake through events ---------------------------------
    Event go(false), done(false);
    int shared_value = 0;  // plain int: ordering comes from the events
    std::thread worker([&] {
        if (go.wait(5000000000ull) != Status::Ok) return;
        shared_value = shared_value * 10 + 7;
        done.set();
    });
    shared_value = 4;
    go.set();
    CHECK_ST(done.wait(5000000000ull), Status::Ok);
    worker.join();
    CHECK_EQ(shared_value, 47);

    // --- 2 threads: mutex protects a non-atomic counter ----------------------
    Mutex m;
    long counter = 0;
    std::atomic<int> ready{0};
    auto body = [&] {
        ready.fetch_add(1);
        while (ready.load() < 2) {
        }
        for (int i = 0; i < 200000; ++i) {
            m.lock();
            ++counter;
            m.unlock();
        }
    };
    std::thread a(body), b(body);
    a.join();
    b.join();
    CHECK_EQ(counter, 400000L);
    CHECK(m.try_lock());
    m.unlock();

    return test_result("rt_test_clock_sync");
}
