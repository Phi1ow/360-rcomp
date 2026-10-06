#include "rcomp/runtime/clock_sync.h"

#include <time.h>

#include <chrono>

#include "rcomp/diag.h"
#include "rcomp/fast_clock.h"

namespace rcomp::rt {

uint64_t monotonic_ns() {
    if (__builtin_expect(fast_clock_ready(), 1)) return fast_monotonic_ns_ready();
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "clock_gettime(CLOCK_MONOTONIC) failed");
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

namespace {
uint64_t realtime_filetime() {
    // Seconds between 1601-01-01 and 1970-01-01.
    constexpr uint64_t kEpochDelta = 11644473600ull;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "clock_gettime(CLOCK_REALTIME) failed");
    return ((uint64_t)ts.tv_sec + kEpochDelta) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
}
}  // namespace

uint64_t system_filetime() {
    if (__builtin_expect(fast_clock_ready(), 1)) {
        // The wall clock is read once; from then on it advances with the monotonic clock.
        static const struct Origin {
            uint64_t monotonic_ns, filetime;
        } origin = {fast_monotonic_ns_ready(), realtime_filetime()};
        return origin.filetime + (fast_monotonic_ns_ready() - origin.monotonic_ns) / 100;
    }
    return realtime_filetime();
}

void Event::set() {
    {
        std::lock_guard<std::mutex> lock(m_);
        signalled_ = true;
    }
    if (manual_) cv_.notify_all();
    else cv_.notify_one();
}

void Event::reset() {
    std::lock_guard<std::mutex> lock(m_);
    signalled_ = false;
}

bool Event::is_set() const {
    std::lock_guard<std::mutex> lock(m_);
    return signalled_;
}

Status Event::wait(uint64_t timeout_ns) {
    std::unique_lock<std::mutex> lock(m_);
    auto ready = [this] { return signalled_; };
    // Treat absurdly large timeouts as infinite (avoids time_point overflow).
    if (timeout_ns >= (1ull << 62)) {
        cv_.wait(lock, ready);
    } else if (!cv_.wait_for(lock, std::chrono::nanoseconds(timeout_ns), ready)) {
        return Status::Timeout;
    }
    if (!manual_) signalled_ = false;
    return Status::Ok;
}

}  // namespace rcomp::rt
