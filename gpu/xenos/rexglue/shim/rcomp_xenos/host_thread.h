// R-comp: joinable host thread with an explicit stack size.
// std::thread cannot set a stack size, and the default
// pthread stack of a PS5 title is far smaller than a desktop one; the command
// processor worker (shader translation, logging) and the register bridge run
// on these threads.
#pragma once

#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <utility>

#include "rcomp/diag.h"

#if defined(__PROSPERO__)
extern "C" int scePthreadSetaffinity(pthread_t thread, uint64_t mask);
#endif

namespace rcomp::xenos {

class HostThread {
public:
    static constexpr size_t kDefaultStack = size_t(4) << 20;

    HostThread() = default;
    HostThread(const HostThread&) = delete;
    HostThread& operator=(const HostThread&) = delete;
    ~HostThread() {
        if (joinable()) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xenos: host thread destroyed while joinable");
    }

    // Starts fn on a new thread; fatal if the thread cannot be created. `role` (CP, BRIDGE, REPLAY)
    // names the thread for the placement experiment: if the environment variable RCOMP_CPU_MASK_<role>
    // holds a CPU mask (0x...), the thread restricts itself to those logical CPUs when it starts.
    void Start(std::function<void()> fn, size_t stack_bytes = kDefaultStack, const char* role = nullptr) {
        if (joinable()) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xenos: host thread started twice");
        auto* start = new StartData{std::move(fn), role};
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        int rc = pthread_attr_setstacksize(&attr, stack_bytes);
        if (rc == 0) rc = pthread_create(&thread_, &attr, &Trampoline, start);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            delete start;
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "xenos: pthread_create failed (%d)", rc);
        }
        started_ = true;
    }
    bool joinable() const { return started_; }
    void join() {
        if (!started_) return;
        pthread_join(thread_, nullptr);
        started_ = false;
    }

private:
    struct StartData {
        std::function<void()> fn;
        const char* role;
    };
    // Placement experiment (see Start): the mask comes from RCOMP_CPU_MASK_<role>, no variable, no change.
    static void ApplyPlacement(const char* role) {
        if (!role) return;
        char name[48];
        std::snprintf(name, sizeof name, "RCOMP_CPU_MASK_%s", role);
        const char* value = std::getenv(name);
        if (!value || !*value) return;
        const unsigned long long mask = std::strtoull(value, nullptr, 0);
#if defined(__PROSPERO__)
        const int rc = scePthreadSetaffinity(pthread_self(), uint64_t(mask));
        std::fprintf(stderr, "RCOMP-PLACEMENT role=%s mask=0x%llx rc=%d\n", role, mask, rc);
#else
        std::fprintf(stderr, "RCOMP-PLACEMENT role=%s mask=0x%llx ignored (not a PS5 build)\n", role, mask);
#endif
    }
    static void* Trampoline(void* p) {
        std::unique_ptr<StartData> start(static_cast<StartData*>(p));
        ApplyPlacement(start->role);
        start->fn();
        return nullptr;
    }
    pthread_t thread_{};
    bool started_ = false;
};

}  // namespace rcomp::xenos
