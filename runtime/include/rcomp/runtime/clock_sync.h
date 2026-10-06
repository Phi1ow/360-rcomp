// Clock and host synchronization primitives (owner: Agent 3, runtime/).
//
// Clock
//   monotonic_ns()      CLOCK_MONOTONIC, never goes backwards.
//   system_filetime()   CLOCK_REALTIME as Windows FILETIME (100 ns units
//                       since 1601-01-01 UTC), what KeQuerySystemTime returns.
//   Both read the TSC calibrated at start-up when the title has one (rcomp/fast_clock.h): no
//   system call, 20 ns resolution, a few parts per million of drift against the operating
//   system's clocks. steady_now() is monotonic_ns() as a std::chrono::steady_clock time_point.
//
// Synchronization
//   rt::Mutex / rt::Event are host primitives (std::mutex +
//   std::condition_variable) used by HLE objects and runtime services. They
//   give real happens-before ordering.
//
//   NOTE: `volatile` is not atomics. XenonRecomp's PPC_LOAD_*/PPC_STORE_*
//   macros use volatile accesses: volatile prevents the compiler from caching
//   or eliding them but provides no atomicity for 64-bit/vector accesses and
//   no inter-thread ordering on its own. On x86-64 hosts (Linux dev box and
//   PS5 Zen 2) plain aligned loads/stores up to 8 bytes are atomic and
//   hardware ordering is TSO, which is what XenonRecomp implicitly relies on.
//
//   How XenonRecomp @ddd128b emits PPC atomics (recompiler.cpp):
//     lwarx/ldarx  -> plain load into `reserved`, byte-swapped into rD
//     stwcx./stdcx.-> __sync_bool_compare_and_swap(addr, reserved, new) -> cr0.eq
//     sync/lwsync/eieio -> no code (relies on the CAS full barrier + x86 TSO)
//     mftb         -> __rdtsc() (host TSC ticks, NOT the 50 MHz Xenon timebase)
//   This is a value-based CAS, not a reservation: an ABA sequence (A->B->A by
//   another thread between lwarx and stwcx.) succeeds where hardware would
//   fail. Correctness of guest spinlocks therefore depends on the generator,
//   not on this runtime (Agent 1 concern).
#pragma once

#include <stdint.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

uint64_t monotonic_ns();
uint64_t system_filetime();
// Same epoch and unit as std::chrono::steady_clock, read without a system call. Only comparable
// with itself: a deadline for an operating-system wait is rebuilt from std::chrono::steady_clock
// (see wait_until in src/hle_xboxkrnl_threads.cpp).
inline std::chrono::steady_clock::time_point steady_now() {
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(monotonic_ns()));
}

constexpr uint64_t kWaitInfinite = ~0ull;

class Mutex {
public:
    void lock() { m_.lock(); }
    bool try_lock() { return m_.try_lock(); }
    void unlock() { m_.unlock(); }

private:
    std::mutex m_;
};

class Event {
public:
    // manual_reset: stays signalled until reset(); otherwise (auto-reset /
    // "synchronization event") a successful wait consumes the signal.
    explicit Event(bool manual_reset, bool initial_state = false)
        : manual_(manual_reset), signalled_(initial_state) {}
    void set();
    void reset();
    // Returns Ok when signalled, Timeout after timeout_ns (kWaitInfinite = forever).
    Status wait(uint64_t timeout_ns = kWaitInfinite);
    bool is_set() const;

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    const bool manual_;
    bool signalled_;
};

}  // namespace rcomp::rt
