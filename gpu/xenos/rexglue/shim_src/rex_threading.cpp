// R-comp implementation of the part of rexglue's threading API
// (include/rex/thread.h) the GPU layers use: events, waits with timeout,
// yield, sleep, memory barrier. Standard C++ only: rexglue's POSIX version
// relies on signals (thread suspension, alerts) that do not run in PS5
// titles, and on Linux-only calls. Alertable waits never report an APC (no
// user callbacks are ever queued to host threads here).
//
// Waitable objects (events, threads) all come from this file; Wait() reaches
// them through native_handle() -> Waitable (no RTTI in titles).
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <rex/thread.h>
#include <rex/thread/mutex.h>

#include "rcomp/diag.h"

namespace rex::thread {

namespace {

struct Waitable {
    // true if signalled before the timeout (milliseconds::max() = forever).
    virtual bool WaitFor(std::chrono::milliseconds timeout) = 0;

protected:
    ~Waitable() = default;
};

class CvEvent final : public Event, public Waitable {
public:
    CvEvent(bool manual, bool initial) : manual_(manual), signalled_(initial) {}
    void* native_handle() const override { return static_cast<Waitable*>(const_cast<CvEvent*>(this)); }
    void Set() override {
        std::lock_guard<std::mutex> lk(mu_);
        signalled_ = true;
        if (manual_) cv_.notify_all();
        else cv_.notify_one();
    }
    void Reset() override {
        std::lock_guard<std::mutex> lk(mu_);
        signalled_ = false;
    }
    void Pulse() override {
        std::lock_guard<std::mutex> lk(mu_);
        ++pulse_;
        cv_.notify_all();
    }
    bool WaitFor(std::chrono::milliseconds timeout) override {
        std::unique_lock<std::mutex> lk(mu_);
        const uint64_t pulse = pulse_;
        auto ready = [&] { return signalled_ || pulse_ != pulse; };
        bool ok;
        if (timeout == std::chrono::milliseconds::max()) {
            cv_.wait(lk, ready);
            ok = true;
        } else {
            ok = cv_.wait_for(lk, timeout, ready);
        }
        if (ok && signalled_ && !manual_) signalled_ = false;
        return ok;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    const bool manual_;
    bool signalled_;
    uint64_t pulse_ = 0;
};

}  // namespace

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
    return std::make_unique<CvEvent>(true, initial_state);
}
std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
    return std::make_unique<CvEvent>(false, initial_state);
}

WaitResult Wait(WaitHandle* wait_handle, bool, std::chrono::milliseconds timeout) {
    if (!wait_handle) return WaitResult::kFailed;
    auto* w = static_cast<Waitable*>(wait_handle->native_handle());
    return w->WaitFor(timeout) ? WaitResult::kSuccess : WaitResult::kTimeout;
}

// ---- Thread: a joinable pthread with the requested stack size -------------
// The GPU backends use it for worker threads (pipeline creation). Waiting on
// it waits for the thread to end. Priority and affinity are recorded only
// (scheduling hints the backends set for performance). Suspension, APCs and
// Exit are never used by the GPU layers: fatal if they are.
namespace {

pthread_key_t g_current_key;
pthread_once_t g_current_once = PTHREAD_ONCE_INIT;
void make_current_key() { pthread_key_create(&g_current_key, nullptr); }

[[noreturn]] void thread_unsupported(const char* what) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xenos: rex::thread::Thread::%s is not supported", what);
}

class HostThreadImpl final : public Thread, public Waitable {
public:
    explicit HostThreadImpl(std::function<void()> fn) : fn_(std::move(fn)) {
        static std::atomic<uint32_t> next_id{1};
        id_ = next_id.fetch_add(1);
    }
    ~HostThreadImpl() override {
        if (started_) pthread_join(thread_, nullptr);
    }
    bool Start(size_t stack) {
        pthread_once(&g_current_once, make_current_key);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, stack < 0x10000 ? 0x10000 : stack);
        const int rc = pthread_create(&thread_, &attr, &Trampoline, this);
        pthread_attr_destroy(&attr);
        started_ = rc == 0;
        return started_;
    }
    void* native_handle() const override { return static_cast<Waitable*>(const_cast<HostThreadImpl*>(this)); }
    bool WaitFor(std::chrono::milliseconds timeout) override { return done_.WaitFor(timeout); }
    uint32_t system_id() const override { return id_; }
    int32_t priority() override { return priority_; }
    void set_priority(int32_t p) override { priority_ = p; }
    uint64_t affinity_mask() override { return affinity_; }
    void set_affinity_mask(uint64_t m) override { affinity_ = m; }
    void QueueUserCallback(std::function<void()>) override { thread_unsupported("QueueUserCallback"); }
    bool Resume(uint32_t*) override { thread_unsupported("Resume"); }
    bool Suspend(uint32_t*) override { thread_unsupported("Suspend"); }
    void Terminate(int) override { thread_unsupported("Terminate"); }

private:
    static void* Trampoline(void* p) {
        auto* self = static_cast<HostThreadImpl*>(p);
        pthread_setspecific(g_current_key, static_cast<Thread*>(self));
        self->fn_();
        self->done_.Set();
        return nullptr;
    }
    std::function<void()> fn_;
    pthread_t thread_{};
    bool started_ = false;
    uint32_t id_ = 0;
    int32_t priority_ = 0;
    uint64_t affinity_ = 0;
    CvEvent done_{true, false};
};

}  // namespace

std::unique_ptr<Thread> Thread::Create(CreationParameters params, std::function<void()> start_routine) {
    if (params.create_suspended) thread_unsupported("Create(create_suspended)");
    auto t = std::make_unique<HostThreadImpl>(std::move(start_routine));
    if (!t->Start(params.stack_size)) return nullptr;
    return t;
}

Thread* Thread::GetCurrentThread() {
    pthread_once(&g_current_once, make_current_key);
    return static_cast<Thread*>(pthread_getspecific(g_current_key));
}

void Thread::Exit(int) { thread_unsupported("Exit"); }

uint32_t logical_processor_count() {
    const unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
}

std::recursive_mutex& global_critical_region::mutex() {
    static std::recursive_mutex m;
    return m;
}

void MaybeYield() { std::this_thread::yield(); }
void SyncMemory() { std::atomic_thread_fence(std::memory_order_seq_cst); }
void Sleep(std::chrono::microseconds duration) { std::this_thread::sleep_for(duration); }

}  // namespace rex::thread
