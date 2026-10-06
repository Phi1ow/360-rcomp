#pragma once

#include <cstdint>
#include <new>
#include <pthread.h>
#include <signal.h>

// A pthread owns at most one sampling target. Guest context metadata may be
// attached temporarily to a GPU target, but never creates a second target.
// The sampler keeps the registry lock through pthread_kill: releasing it after
// copying pthread_t would permit exit, reuse, then signalling an unrelated
// thread. The SIGURG handler must never acquire this lock.
class RcompProfilerRegistry {
    struct ThreadState;
public:
    struct GuestRecord {
        ThreadState* owner = nullptr;
        pthread_t thread{};
        int group = -1;
        const char* const* fn = nullptr;
        const char* const* ring = nullptr;
        const unsigned* ring_idx = nullptr;
        void* ctx = nullptr;
    };
    struct SampleResult { unsigned sent = 0; unsigned failed = 0; };

    RcompProfilerRegistry() {
        key_error_ = pthread_key_create(&key_, thread_exit);
    }
    // All registered threads must be joined before a non-static test registry
    // is destroyed. The title registry has process lifetime.
    ~RcompProfilerRegistry() { if (!key_error_) pthread_key_delete(key_); }
    RcompProfilerRegistry(const RcompProfilerRegistry&) = delete;
    RcompProfilerRegistry& operator=(const RcompProfilerRegistry&) = delete;

    bool register_slot(const char* const* fn, const char* const* ring,
                       const unsigned* ring_idx) {
        ThreadState* state = current(true);
        if (!state) return false;
        Guard lock(mutex_);
        state->fn = fn; state->ring = ring; state->ring_idx = ring_idx;
        if (state->tid && state->tid < kGuests) {
            auto& guest = guests_[state->tid];
            if (guest.owner == state) {
                guest.fn = fn; guest.ring = ring; guest.ring_idx = ring_idx;
            }
        }
        return true;
    }

    bool register_guest(uint32_t tid, void* ctx) {
        ThreadState* state = current(true);
        if (!state || !tid || !ctx) return false;
        Guard lock(mutex_);
        if (tid < kGuests && guests_[tid].owner && guests_[tid].owner != state)
            return false;
        // Guest ids are not recycled by the runtime. Keep a larger id in the
        // owner state for correct unregister even when it cannot be sampled.
        clear_guest(state, false);
        state->tid = tid; state->ctx = ctx;
        if (tid < kGuests) {
            guests_[tid] = {state, pthread_self(), state->gpu ? 2 : tid == 1 ? 0 : 1,
                           state->fn, state->ring, state->ring_idx, ctx};
        }
        return true;
    }

    bool register_host() {
        ThreadState* state = current(true);
        if (!state) return false;
        Guard lock(mutex_);
        state->gpu = true;
        if (state->tid && state->tid < kGuests && guests_[state->tid].owner == state)
            guests_[state->tid].group = 2;
        if (state->host_slot >= 0) return true;  // idempotent
        for (unsigned i = 0; i < kHosts; ++i) {
            if (hosts_[i].owner) continue;
            hosts_[i] = {state, pthread_self()};
            state->host_slot = int(i);
            return true;
        }
        return false;  // capacity exhausted; never reserve an occupied slot
    }

    bool unregister_guest(uint32_t tid, void* ctx) {
        ThreadState* state = current(false);
        if (!state) return false;
        Guard lock(mutex_);
        if (state->tid != tid || state->ctx != ctx) return false;
        clear_guest(state, true);
        return true;  // the GPU target, if any, survives callback return
    }

    bool unregister_current() {
        ThreadState* state = current(false);
        if (!state) return key_error_ == 0;
        if (pthread_setspecific(key_, nullptr) != 0) return false;
        thread_exit(state);
        return true;
    }

    SampleResult sample_tick(uint32_t& next_other, int signal = SIGURG) {
        Guard lock(mutex_);
        SampleResult result;
        auto send = [&](pthread_t thread) {
            ++result.sent;
            if (pthread_kill(thread, signal) != 0) ++result.failed;
        };
        if (guests_[1].owner && guests_[1].group == 0) send(guests_[1].thread);
        for (const auto& host : hosts_) if (host.owner) send(host.thread);
        for (unsigned tries = 0; tries < 62; ++tries) {
            next_other = next_other >= 63 ? 2 : next_other + 1;
            const auto& guest = guests_[next_other];
            if (guest.owner && guest.group == 1) { send(guest.thread); break; }
        }
        return result;
    }

    template <class Visitor> void with_guest_contexts(Visitor&& visitor) {
        Guard lock(mutex_);
        visitor(guests_);
    }
    unsigned host_count() {
        Guard lock(mutex_);
        unsigned count = 0;
        for (const auto& host : hosts_) if (host.owner) ++count;
        return count;
    }

private:
    static constexpr unsigned kGuests = 256, kHosts = 4;
    struct ThreadState {
        RcompProfilerRegistry* registry = nullptr;
        uint32_t tid = 0;
        void* ctx = nullptr;
        bool gpu = false;
        int host_slot = -1;
        const char* const* fn = nullptr;
        const char* const* ring = nullptr;
        const unsigned* ring_idx = nullptr;
    };
    struct HostRecord { ThreadState* owner = nullptr; pthread_t thread{}; };
    struct Guard {
        explicit Guard(pthread_mutex_t& mutex) : mutex_(mutex) { pthread_mutex_lock(&mutex_); }
        ~Guard() { pthread_mutex_unlock(&mutex_); }
        pthread_mutex_t& mutex_;
    };
    ThreadState* current(bool create) {
        if (key_error_) return nullptr;
        auto* state = static_cast<ThreadState*>(pthread_getspecific(key_));
        if (!state && create) {
            state = new (std::nothrow) ThreadState;
            if (!state) return nullptr;
            state->registry = this;
            if (pthread_setspecific(key_, state) != 0) { delete state; return nullptr; }
        }
        return state;
    }
    void clear_guest(ThreadState* state, bool clear_ring) {
        if (state->tid && state->tid < kGuests && guests_[state->tid].owner == state)
            guests_[state->tid] = GuestRecord{};
        state->tid = 0; state->ctx = nullptr;
        if (clear_ring) { state->fn = nullptr; state->ring = nullptr; state->ring_idx = nullptr; }
    }
    static void thread_exit(void* opaque) {
        auto* state = static_cast<ThreadState*>(opaque);
        auto& registry = *state->registry;
        {
            Guard lock(registry.mutex_);
            registry.clear_guest(state, true);
            if (state->host_slot >= 0 && registry.hosts_[state->host_slot].owner == state)
                registry.hosts_[state->host_slot] = HostRecord{};
        }
        delete state;
    }
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_key_t key_{};
    int key_error_ = 0;
    GuestRecord guests_[kGuests]{};
    HostRecord hosts_[kHosts]{};
};
