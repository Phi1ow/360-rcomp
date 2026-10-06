// xboxkrnl.exe HLE: time, dynamic TLS, critical sections, events,
// semaphores, handle mutants, waits, thread creation (owner: Agent 3).
//
// Same rules as hle_xboxkrnl.cpp: each export states the Xbox 360 semantics
// and the implemented subset; anything outside it ends in
// RCOMP_FATAL_UNIMPLEMENTED, never in a fake success.
//
// Guest object layouts (big-endian, as the title and the real kernel use
// them; checked against the pinned rexglue-sdk/Xenia definitions, BSD-3):
//   DISPATCHER_HEADER (16 bytes)  +0 u8 type, +1 u8 absolute, +2 u8 size,
//                                 +3 u8 inserted, +4 s32 signal_state,
//                                 +8 LIST_ENTRY wait_list {flink, blink}
//   KEVENT (16)                   header; type 0 = notification (manual reset),
//                                 1 = synchronization (auto reset)
//   RTL_CRITICAL_SECTION (28)     header (type 1, absolute = spin count / 256),
//                                 +0x10 s32 lock_count (-1 = free),
//                                 +0x14 s32 recursion_count,
//                                 +0x18 u32 owning_thread
//
// Waiting model: one runtime-wide dispatcher lock and condition variable.
// Every signal change happens under the lock and wakes all waiters, which
// re-check their own object (correct, simple; not tuned for many threads).
// Guest events/semaphores/critical sections keep their state in guest memory;
// handle objects (events, semaphores, mutants, threads) keep it host-side.
// User APCs (NtQueueApcThread) are queued per guest thread id and delivered by
// a user-mode alertable wait of that thread, NT's rule: an object already
// signalled wins; otherwise a pending (or newly queued) APC ends the wait
// with STATUS_USER_APC after every queued APC ran on the waiting thread's own
// guest context. Kernel-mode or non-alertable waits never deliver them.
//
// A pulse (NtPulseEvent) cannot use that re-check model: the event is set and
// reset under one lock hold, so a woken waiter would only see it reset. Every
// blocked waiter therefore also publishes its own satisfaction test; the
// pulsing thread runs it, under g_disp, for the threads blocked at that moment
// (oldest first) and marks them satisfied, as the NT kernel's wait test does
// before it resets the event. See runtime/docs/SUSPEND_PULSE.md.
//
// Thread suspension (NtSuspendThread) is cooperative: a suspended worker stops
// at its next kernel call (import dispatch) or stays blocked in its current
// wait without consuming any signal until its suspend count returns to zero.
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include "physical_window.h"
#include "rcomp/fast_clock.h"
#include "diagnostics.h"
#include "thread_suspend.h"
#include <sched.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <map>
#include <string>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "rcomp/diag.h"
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/guest_write_tracking.h"
#include "rcomp/runtime/clock_sync.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/io_event.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/notifications.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/wait_stats.h"
#include "kernel_internal.h"

#include <deque>

namespace rcomp::rt {

namespace {

constexpr uint32_t kStatusSuccess = 0x00000000;
constexpr uint32_t kStatusTimeout = 0x00000102;
constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
constexpr uint32_t kStatusObjectTypeMismatch = 0xC0000024;
constexpr uint32_t kStatusNoMemory = 0xC0000017;
constexpr uint32_t kStatusThreadTerminating = 0xC000004B;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kStatusAccessViolation = 0xC0000005;
constexpr uint32_t kStatusSemaphoreLimit = 0xC0000047;
constexpr uint32_t kStatusMutantNotOwned = 0xC0000046;
constexpr uint32_t kStatusAbandoned = 0x00000080;
constexpr uint32_t kStatusUserApc = 0x000000C0;
constexpr uint32_t kStatusUnsuccessful = 0xC0000001;
constexpr uint32_t kUserMode = 1;
constexpr uint32_t kMaximumWaitObjects = 64;
constexpr uint32_t kTlsOutOfIndexes = 0xFFFFFFFF;
constexpr uint32_t kCreateSuspended = 0x00000001;
constexpr uint32_t kDefaultStackSize = kDefaultGuestThreadStackSize;  // 256 KiB when the title passes 0 (guest_thread.h)
constexpr size_t kHostStackSize = 8u << 20;      // native stack of a guest thread
// ---- helpers ---------------------------------------------------------------

Runtime& rt_or_die(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s called before rcomp::rt::runtime_init", fn);
    return *r;
}

[[noreturn]] void unimplemented(const char* fn, PPCContext& ctx, const char* what, uint32_t v) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xboxkrnl.exe!%s %s=0x%08X lr=0x%08X (not implemented)", fn, what, v,
                (uint32_t)ctx.lr);
}

GuestThread& current_or_die(const char* fn) {
    GuestThread* t = current_guest_thread();
    if (!t) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s called outside a guest thread", fn);
    return *t;
}
bool writable_word(uint32_t address);
uint32_t unnamed_attributes(uint32_t address,const char* fn,PPCContext& ctx);
void destroy_thread_or_die(GuestHeap& heap,GuestThread* thread,const char* where) {
    const Status status=destroy_guest_thread(heap,thread);
    if(status!=Status::Ok)rcomp_fatal(RCOMP_FATAL_PLATFORM,"%s guest thread cleanup: %s",where,status_name(status));
}
template<class T,class... Args>
std::shared_ptr<T> make_object(Args&&... args) {
#if defined(__cpp_exceptions)
    try { return std::make_shared<T>(std::forward<Args>(args)...); }
    catch(const std::bad_alloc&){ return {}; }
#else
    return std::make_shared<T>(std::forward<Args>(args)...);
#endif
}

// Host pointer to a 4-byte guest word, or fatal.
uint32_t* guest_word(uint32_t addr, const char* fn, PPCContext& ctx) {
    Runtime& r = rt_or_die(fn);
    if ((addr & 3) || !r.mem->is_accessible(addr, 4, Protect::ReadWrite))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s object 0x%08X not writable or misaligned lr=0x%08X",
                    fn, addr, (uint32_t)ctx.lr);
    return reinterpret_cast<uint32_t*>(r.mem->host(addr));
}
uint8_t* guest_bytes(uint32_t addr, uint32_t n, const char* fn, PPCContext& ctx,
                     Protect access = Protect::ReadWrite) {
    Runtime& r = rt_or_die(fn);
    if (!r.mem->is_accessible(addr, n, access))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s object 0x%08X+%u not accessible (mode=%u) lr=0x%08X", fn, addr, n,
                    unsigned(access),
                    (uint32_t)ctx.lr);
    return r.mem->host(addr);
}

// The guest's synchronization objects live in guest memory, in pages a GPU cache may hold: what the HLE changes there is a guest memory write like any other
// (include/rcomp/guest_write_tracking.h), recorded after the data store.
void note_word_write(const uint32_t* w) {
    const Runtime* r = runtime();
    if (!r) return;
    const uint64_t address = uint64_t(reinterpret_cast<const uint8_t*>(w) - r->mem->base());
    if (address < (uint64_t(1) << 32)) note_title_write(uint32_t(address), 4);
}
void note_bytes_write(const uint8_t* p, uint32_t n) {
    const Runtime* r = runtime();
    if (!r) return;
    const uint64_t address = uint64_t(p - r->mem->base());
    if (address < (uint64_t(1) << 32)) note_title_write(address, n);
}

int32_t load_s32(uint32_t* w) { return (int32_t)__builtin_bswap32(__atomic_load_n(w, __ATOMIC_SEQ_CST)); }
void store_s32(uint32_t* w, int32_t v) {
    __atomic_store_n(w, __builtin_bswap32((uint32_t)v), __ATOMIC_SEQ_CST);
    note_word_write(w);
}
// Atomic add on a big-endian guest word; returns the new value.
int32_t add_s32(uint32_t* w, int32_t delta) {
    uint32_t old = __atomic_load_n(w, __ATOMIC_SEQ_CST);
    for (;;) {
        int32_t nv = (int32_t)__builtin_bswap32(old) + delta;
        if (__atomic_compare_exchange_n(w, &old, __builtin_bswap32((uint32_t)nv), false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {
            note_word_write(w);
            return nv;
        }
    }
}
bool cas_s32(uint32_t* w, int32_t expected, int32_t desired) {
    uint32_t e = __builtin_bswap32((uint32_t)expected);
    const bool swapped = __atomic_compare_exchange_n(w, &e, __builtin_bswap32((uint32_t)desired), false, __ATOMIC_SEQ_CST,
                                                     __ATOMIC_SEQ_CST);
    if (swapped) note_word_write(w);
    return swapped;
}

// ---- dispatcher lock and timeouts ------------------------------------------

std::mutex g_disp;
bool g_stopping = false;  // guarded by g_disp
uint32_t g_workers = 0;   // includes suspended workers

// Every blocked guest thread owns a Waiter (on its stack, linked under g_disp)
// with the identities of the objects it waits for. A state change of one
// object wakes only the waiters of that object; signal_all() (lifecycle,
// thread exit, rarely used objects) still wakes every waiter, which re-checks
// its own condition. A single shared condition variable woke all ~30 guest
// threads on every semaphore release, a thundering herd that dominated
// GTA IV's frame time.
// The host condition variable a guest thread blocks on, one per native thread (a thread has at most one
// Waiter at a time), created on first use and kept for the thread's life. It sleeps on CLOCK_MONOTONIC
// (pthread_condattr_setclock) so a deadline on the runtime's TSC clock (rt::steady_now, the same epoch as
// CLOCK_MONOTONIC, a few ppm of drift) is passed to pthread_cond_timedwait as it is: no system call per
// wait. The std::condition_variable it replaces read the operating system's clock three times per
// blocking wait (once here, twice inside libc++'s wait_until), about 1 us each on the console, on
// every guest thread that blocks (wave 1 profile: 8.9 % of the main thread's samples under
// steady_clock::now; the city scene blocks about 100,000 times a second across the guest threads).
// When the platform does not honour the clock attribute (checked once with a 2 ms test wait), the
// timed wait falls back to one CLOCK_REALTIME read per wait; RCOMP-WAIT-COND reports which.
struct WaitSlot {
    pthread_cond_t cond;
    bool monotonic = false;
};
pthread_key_t g_wait_slot_key;
pthread_once_t g_wait_slot_once = PTHREAD_ONCE_INIT;
std::atomic<int> g_wait_cond_mode{-1};  // -1 untested, 1 monotonic deadlines honoured, 0 fallback

void wait_slot_key_create() {
    if (pthread_key_create(&g_wait_slot_key, [](void* p) {
            auto* slot = static_cast<WaitSlot*>(p);
            pthread_cond_destroy(&slot->cond);
            delete slot;
        }) != 0)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "pthread_key_create failed for the wait slots");
}

// Does pthread_cond_timedwait honour a CLOCK_MONOTONIC condition variable on this platform? A 2 ms
// wait on an unsignalled variable must take about 2 ms (an ignored attribute reads the small
// monotonic value as a CLOCK_REALTIME deadline in 1970 and returns at once). Decided once.
bool monotonic_cond_supported() {
    int mode = g_wait_cond_mode.load(std::memory_order_acquire);
    if (mode >= 0) return mode == 1;
    bool ok = false;
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) == 0) {
        if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0) {
            pthread_cond_t cond;
            pthread_mutex_t mutex;
            if (pthread_cond_init(&cond, &attr) == 0 && pthread_mutex_init(&mutex, nullptr) == 0) {
                const uint64_t start = monotonic_ns();
                timespec ts;
                const uint64_t abs_ns = start + 2000000ull;
                ts.tv_sec = time_t(abs_ns / 1000000000ull);
                ts.tv_nsec = long(abs_ns % 1000000000ull);
                pthread_mutex_lock(&mutex);
                const int rc = pthread_cond_timedwait(&cond, &mutex, &ts);
                pthread_mutex_unlock(&mutex);
                const uint64_t elapsed = monotonic_ns() - start;
                ok = rc == ETIMEDOUT && elapsed >= 1000000ull && elapsed < 1000000000ull;
                std::fprintf(stderr, "RCOMP-WAIT-COND monotonic=%d rc=%d elapsed_us=%llu\n", ok ? 1 : 0, rc,
                             (unsigned long long)(elapsed / 1000));
                pthread_cond_destroy(&cond);
                pthread_mutex_destroy(&mutex);
            }
        } else {
            std::fprintf(stderr, "RCOMP-WAIT-COND monotonic=0 setclock_unsupported\n");
        }
        pthread_condattr_destroy(&attr);
    }
    g_wait_cond_mode.store(ok ? 1 : 0, std::memory_order_release);
    return ok;
}

WaitSlot* wait_slot() {
    pthread_once(&g_wait_slot_once, wait_slot_key_create);
    auto* slot = static_cast<WaitSlot*>(pthread_getspecific(g_wait_slot_key));
    if (slot) return slot;
    slot = new (std::nothrow) WaitSlot();
    if (!slot) rcomp_fatal(RCOMP_FATAL_PLATFORM, "wait slot allocation failed");
    slot->monotonic = monotonic_cond_supported();
    pthread_condattr_t attr;
    int rc = pthread_condattr_init(&attr);
    if (rc == 0 && slot->monotonic) rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) rc = pthread_cond_init(&slot->cond, &attr);
    pthread_condattr_destroy(&attr);
    if (rc != 0 || pthread_setspecific(g_wait_slot_key, slot) != 0)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "wait slot condition variable creation failed rc=%d", rc);
    return slot;
}

struct Waiter {
    WaitSlot* slot = wait_slot();
    // Wakes the blocked thread (the caller holds g_disp, as std::condition_variable::notify_one did).
    void wake() { pthread_cond_signal(&slot->cond); }
    // Blocks until woken; g_disp is released while asleep and held again on return.
    void sleep(std::unique_lock<std::mutex>& lk) { pthread_cond_wait(&slot->cond, lk.mutex()->native_handle()); }
    // Blocks until woken or until `until` on the runtime's clock (rt::steady_now).
    void sleep_until(std::unique_lock<std::mutex>& lk, std::chrono::steady_clock::time_point until) {
        uint64_t abs_ns = uint64_t(std::max<int64_t>(0, until.time_since_epoch().count()));
        if (!slot->monotonic) {
            // Fallback: one CLOCK_REALTIME read, the time left counted from now on that clock.
            const int64_t left = std::max<int64_t>(0, (until - steady_now()).count());
            timespec real;
            clock_gettime(CLOCK_REALTIME, &real);
            abs_ns = uint64_t(real.tv_sec) * 1000000000ull + uint64_t(real.tv_nsec) + uint64_t(left);
        }
        timespec ts;
        ts.tv_sec = time_t(abs_ns / 1000000000ull);
        ts.tv_nsec = long(abs_ns % 1000000000ull);
        pthread_cond_timedwait(&slot->cond, lk.mutex()->native_handle(), &ts);
    }
    const void* const* ids = nullptr;  // null/0: woken only by signal_all()
    uint32_t count = 0;
    Waiter* next = nullptr;
    // Object waits only (null for lifecycle waits). Run by a pulsing thread
    // under g_disp: tests and consumes exactly like the waiter's own loop.
    bool (*try_ready)(void*) = nullptr;
    void* ready_arg = nullptr;
    bool satisfied = false;  // a pulse satisfied (and consumed) this wait
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    const int* suspend_count = nullptr;  // the waiting worker's, guarded by g_disp; null: never suspended
};
Waiter* g_waiters = nullptr;  // guarded by g_disp
// The calling worker's suspend count (GuestThreadObject::suspend_count),
// published by guest_thread_main; null on threads without a worker object.
thread_local const int* t_suspend_count = nullptr;

// User-mode APCs queued by NtQueueApcThread, per target guest thread id (ids
// are unique and never reused). Guarded by g_disp; g_pending_apcs mirrors the
// total so waits without APCs pay one relaxed load.
struct UserApc { uint32_t routine, context, argument1, argument2; };
std::map<uint32_t, std::deque<UserApc>> g_user_apcs;
std::atomic<uint32_t> g_pending_apcs{0};
// Caller holds g_disp.
bool user_apc_pending_locked(uint32_t thread_id) {
    if (!thread_id || g_pending_apcs.load(std::memory_order_relaxed) == 0) return false;
    const auto it = g_user_apcs.find(thread_id);
    return it != g_user_apcs.end() && !it->second.empty();
}
// Caller holds g_disp: drops the queue of an exited thread (NT runs no rundown
// for user APCs that were never delivered).
void discard_user_apcs_locked(uint32_t thread_id) {
    const auto it = g_user_apcs.find(thread_id);
    if (it == g_user_apcs.end()) return;
    g_pending_apcs.fetch_sub(uint32_t(it->second.size()), std::memory_order_relaxed);
    g_user_apcs.erase(it);
}

void signal_all() {
    for (Waiter* w = g_waiters; w; w = w->next) w->wake();
}
// Lifecycle waits (no object identity): woken by signal_all() only.
template <class Pred>
void wait_broadcast(std::unique_lock<std::mutex>& lk, Pred pred) {
    Waiter self;
    self.next = g_waiters;
    g_waiters = &self;
    while (!pred()) self.sleep(lk);
    for (Waiter** at = &g_waiters; *at; at = &(*at)->next)
        if (*at == &self) { *at = self.next; break; }
}

// Caller holds g_disp.
void signal_object(const void* id) {
    for (Waiter* w = g_waiters; w; w = w->next)
        for (uint32_t i = 0; i < w->count; ++i)
            if (w->ids[i] == id) { w->wake(); break; }
}

bool waits_for(const Waiter& w, const void* id) {
    for (uint32_t i = 0; i < w.count; ++i)
        if (w.ids[i] == id) return true;
    return false;
}

// Caller holds g_disp and has just made `id` signalled. Runs the satisfaction
// test of every thread blocked on `id` at this moment, oldest waiter first (the
// list is newest-first: it is reversed in place for the walk and restored), and
// marks the satisfied ones. A waiter whose own test consumes the signal (an
// auto-reset event) stops the walk for later waiters through their own test.
// Suspended waiters and waits whose timeout has already expired are skipped.
void satisfy_current_waiters(const void* id, bool (*still_signalled)(const void*)) {
    if (g_stopping) return;
    Waiter* reversed = nullptr;
    for (Waiter* w = g_waiters; w;) {
        Waiter* next = w->next;
        w->next = reversed;
        reversed = w;
        w = next;
    }
    const auto now = steady_now();
    bool open = true;
    for (Waiter* w = reversed; w && open; w = w->next) {
        if (w->satisfied || !w->try_ready || !waits_for(*w, id)) continue;
        if (w->suspend_count && *w->suspend_count > 0) continue;
        if (w->deadline <= now) continue;
        if (w->try_ready(w->ready_arg)) {
            w->satisfied = true;
            w->wake();
        }
        open = still_signalled(id);
    }
    Waiter* restored = nullptr;
    for (Waiter* w = reversed; w;) {
        Waiter* next = w->next;
        w->next = restored;
        restored = w;
        w = next;
    }
    g_waiters = restored;
}

// LARGE_INTEGER* timeout, 100 ns units: NULL = infinite, negative = relative,
// positive = absolute system time (FILETIME), 0 = poll.
bool deadline_from(uint32_t timeout_ptr, std::chrono::steady_clock::time_point* out) {
    if (!timeout_ptr) return false;
    uint64_t raw = 0;
    if (!rt_or_die("wait timeout").mem->is_accessible(timeout_ptr, 8, Protect::Read) ||
        !guest_read_be64(timeout_ptr, &raw))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "wait timeout pointer 0x%08X not readable", timeout_ptr);
    const uint64_t system_now = system_filetime();
    const uint64_t ticks = (raw & (uint64_t(1) << 63)) ? uint64_t(0) - raw
        : (raw > system_now ? raw - system_now : 0);
    using Clock = std::chrono::steady_clock;
    using Ticks = std::chrono::duration<uint64_t, std::ratio<1, 10000000>>;
    // Deadlines live on the runtime's own clock (rt::steady_now, no system call): a wait that
    // polls with a zero timeout, which the guest does in loops, costs no system call at all.
    const auto now = steady_now();
    const uint64_t maximum = std::chrono::duration_cast<Ticks>(Clock::time_point::max() - now).count();
    *out = ticks >= maximum ? Clock::time_point::max()
        : now + std::chrono::duration_cast<Clock::duration>(Ticks(ticks));
    return true;
}

std::chrono::steady_clock::time_point wait_deadline(uint32_t pointer) {
    auto result = std::chrono::steady_clock::time_point::max();
    deadline_from(pointer, &result);
    return result;
}

// Waits under g_disp until ready() (which may consume the signal). Returns
// STATUS_SUCCESS or STATUS_TIMEOUT.
#if RCOMP_RUNTIME_DIAGNOSTICS
thread_local PPCContext* t_wait_ctx = nullptr;  // bring-up tracing: the guest call that is blocking
std::mutex g_sem_stat_mutex;
std::map<uint32_t, uint64_t> g_sem_releases;  // bring-up tracing: releases per semaphore handle

void report_long_wait(const char* what, std::chrono::steady_clock::time_point deadline) {
    PPCContext* c = t_wait_ctx;
    GuestThread* t = current_guest_thread();
    char line[768];
    int n = std::snprintf(line, sizeof line, "RCOMP-WAIT %s blocked >5s thread=0x%X timeout=%s lr=0x%08X", what,
                          t ? t->thread_id : 0,
                          deadline == std::chrono::steady_clock::time_point::max() ? "infinite" : "finite",
                          c ? (uint32_t)c->lr : 0u);
    if (c) {
        n += std::snprintf(line + n, sizeof line - n, " r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X", c->r3.u32, c->r4.u32, c->r5.u32, c->r6.u32);
        if (Runtime* r = runtime()) {
            std::shared_ptr<HandleObject> object;
            if (r->handles.lookup_any(c->r3.u32, &object) == Status::Ok && object)
                n += std::snprintf(line + n, sizeof line - n, " (r3 is a handle, kind=%u)", (unsigned)object->kind());
            else if (r->mem && r->mem->is_accessible(c->r3.u32, 16, Protect::Read)) {
                uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
                guest_read_be32(c->r3.u32, &w0); guest_read_be32(c->r3.u32 + 4, &w1);
                guest_read_be32(c->r3.u32 + 8, &w2); guest_read_be32(c->r3.u32 + 12, &w3);
                n += std::snprintf(line + n, sizeof line - n, " (r3 object %08X %08X %08X %08X)", w0, w1, w2, w3);
            }
        }
        n += std::snprintf(line + n, sizeof line - n, " callers:");
        uint32_t sp = c->r1.u32;
        for (int frame = 0; frame < 12 && sp && n < 700; ++frame) {
            uint32_t back = 0, saved = 0;
            if (!guest_read_be32(sp, &back) || !back || !guest_read_be32(back - 8, &saved)) break;
            n += std::snprintf(line + n, sizeof line - n, " 0x%08X", saved);
            sp = back;
        }
    }
    if (c) {
        std::lock_guard<std::mutex> stat_lock(g_sem_stat_mutex);
        const auto it = g_sem_releases.find(c->r3.u32);
        n += std::snprintf(line + n, sizeof line - n, " releases_of_this_handle=%llu", (unsigned long long)(it == g_sem_releases.end() ? 0 : it->second));
    }
    std::snprintf(line + n, sizeof line - n, "\n");
    std::fputs(line, stderr);  // one write: threads report concurrently
}

// bring-up profiling: time the main guest thread spends blocked, per guest caller
std::map<uint64_t, uint64_t> g_main_wait_us;  // (lr << 32 | r3) -> us, guarded by g_disp
std::chrono::steady_clock::time_point g_main_wait_dump;
void account_main_wait(std::chrono::steady_clock::time_point started) {
    GuestThread* t = current_guest_thread();
    if (!t || t->thread_id != 1 || !t_wait_ctx) return;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t key = (uint64_t(uint32_t(t_wait_ctx->lr)) << 32) | t_wait_ctx->r3.u32;
    g_main_wait_us[key] += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - started).count());
    if (now - g_main_wait_dump > std::chrono::seconds(5)) {
        g_main_wait_dump = now;
        std::vector<std::pair<uint64_t, uint64_t>> rows;
        for (const auto& e : g_main_wait_us) rows.emplace_back(e.second, e.first);
        std::sort(rows.rbegin(), rows.rend());
        std::string line;
        for (size_t i = 0; i < rows.size() && i < 10; ++i) {
            char b[64]; std::snprintf(b, sizeof b, " lr=%08X,r3=%08X:%llums", uint32_t(rows[i].second >> 32), uint32_t(rows[i].second), (unsigned long long)(rows[i].first / 1000)); line += b;
        }
        std::fprintf(stderr, "RCOMP-MAINWAIT%s\n", line.c_str());
        g_main_wait_us.clear();
    }
}
#endif

#ifndef RCOMP_RUNTIME_WAIT_STATS
#define RCOMP_RUNTIME_WAIT_STATS 0
#endif
#if RCOMP_RUNTIME_WAIT_STATS
std::atomic<uint64_t> g_wait_stats[3];  // ready at once, immediate timeouts, blocked
#define RCOMP_WAIT_STAT(slot) g_wait_stats[slot].fetch_add(1, std::memory_order_relaxed)
#else
#define RCOMP_WAIT_STAT(slot) ((void)0)
#endif

// Earliest time at which an object of a wait may become ready without anyone signalling it (an
// armed timer); time_point::max() when none.
using WakeHint = std::chrono::steady_clock::time_point (*)(const void* arg);

#if RCOMP_RUNTIME_WAIT_STATS
// Adds `ns` of blocked wall time to the calling guest thread (defined with the thread report below).
void account_wait_ns(uint64_t ns);
#endif

template <class Ready>
uint32_t wait_until(std::unique_lock<std::mutex>& lk, std::chrono::steady_clock::time_point deadline, Ready ready,
                    const void* const* ids = nullptr, uint32_t id_count = 0, WakeHint wake = nullptr,
                    const void* wake_arg = nullptr) {
#if RCOMP_RUNTIME_DIAGNOSTICS
    const auto wait_started = std::chrono::steady_clock::now();
    bool reported = false;
#endif
    Waiter self;
    self.ids = ids;
    self.count = id_count;
    self.try_ready = [](void* arg) { return (*static_cast<Ready*>(arg))(); };
    self.ready_arg = &ready;
    self.deadline = deadline;
    self.suspend_count = t_suspend_count;
    self.next = g_waiters;
    g_waiters = &self;
    struct Unlink {
        Waiter* w;
        ~Unlink() {  // g_disp is held again whenever wait_until returns
            for (Waiter** at = &g_waiters; *at; at = &(*at)->next)
                if (*at == w) { *at = w->next; break; }
        }
    } unlink{&self};
#if RCOMP_RUNTIME_DIAGNOSTICS
    struct Account {
        std::chrono::steady_clock::time_point started;
        ~Account() { account_main_wait(started); }
    } account{wait_started};
#endif
    bool first_pass = true;
    for (;;) {
        if (self.satisfied) return kStatusSuccess;  // a pulse already consumed the signal for us
        if (g_stopping) return kStatusThreadTerminating;
        if (self.suspend_count && *self.suspend_count > 0) {
            // Suspended while blocked: consume nothing until resumed (resume
            // broadcasts). The deadline is checked again after the resume.
            self.sleep(lk);
            continue;
        }
        if (ready()) {
            if (first_pass) RCOMP_WAIT_STAT(0);
            return kStatusSuccess;
        }
        const auto now = steady_now();
        if (now >= deadline) {
            if (first_pass) RCOMP_WAIT_STAT(1);
            return kStatusTimeout;
        }
        first_pass = false;
        RCOMP_WAIT_STAT(2);
        // Native condition variables may not represent a centuries-long
        // timeout. Bounded slices preserve the deadline and remain wakeable.
#if RCOMP_RUNTIME_DIAGNOSTICS
        if (!reported && now - wait_started > std::chrono::seconds(5)) { reported = true; report_long_wait("wait", deadline); }
#endif
        const auto slice = now + std::chrono::seconds(1);
        auto until = deadline < slice ? deadline : slice;
        if (wake) until = std::max(now, std::min(until, wake(wake_arg)));
        // The slot's condition variable sleeps on CLOCK_MONOTONIC, the clock the runtime's TSC clock is
        // calibrated against: the deadline goes to it as it is, without a system call (WaitSlot).
#if RCOMP_RUNTIME_WAIT_STATS
        const auto blocked_from = steady_now();
        self.sleep_until(lk, until);
        account_wait_ns(uint64_t((steady_now() - blocked_from).count()));
#else
        self.sleep_until(lk, until);
#endif
    }
}

// ---- handle objects --------------------------------------------------------

class HostEvent : public HandleObject {
public:
    static constexpr HandleKind kKind = HandleKind::Event;
    HostEvent(bool manual, bool state) : manual_reset(manual), signalled(state) {}
    HandleKind kind() const override { return kKind; }
    const bool manual_reset;
    bool signalled;  // guarded by g_disp
};

class GuestThreadObject : public HandleObject {
public:
    static constexpr HandleKind kKind = HandleKind::Thread;
    HandleKind kind() const override { return kKind; }
    uint32_t guest_object_body() const override { return thread_object_body(identity); }
    void handle_opened() override { thread_object_handle_opened(identity); }
    void handle_closed() override { thread_object_handle_closed(identity); }
    GuestThread thread;
    std::shared_ptr<ThreadObjectIdentity> identity;
    alignas(64) PPCContext ctx;
    PPCFunc* entry = nullptr;
    int suspend_count = 0;  // guarded by g_disp; change it only through set_suspend_count()
    // Lock-free mirror of suspend_count > 0, polled by the worker at every
    // import dispatch (src/thread_suspend.h). Authoritative state stays above.
    std::atomic<uint32_t> suspend_request{0};
    bool exited = false;    // guarded by g_disp
    uint32_t exit_code = 0;
    Runtime* owner = nullptr;  // alive until runtime_quiesce_threads
    std::shared_ptr<GuestThreadObject> self;  // keeps the object alive while running
    // CPU time of the worker's host thread, sampled by the worker itself (thread_cpu_sample_tick: the
    // console's pthread_getcpuclockid clocks read the calling thread, so a reader cannot ask for it);
    // guest_thread_cpu_report keeps the previous reading in cpu_report_last_ns under g_disp.
    std::atomic<uint64_t> cpu_ns{0};
    uint64_t cpu_report_last_ns = 0;
    // Wall time the worker spent blocked in wait_until (object waits and non-zero sleeps), summed by the worker
    // itself; guest_thread_wait_report keeps the previous reading in wait_report_last_ns under g_disp.
    std::atomic<uint64_t> wait_ns{0};
    uint64_t wait_report_last_ns = 0;
};

class HostSemaphore : public HandleObject {
public:
    static constexpr HandleKind kKind = HandleKind::Semaphore;
    HostSemaphore(int32_t initial, int32_t maximum) : count(initial), limit(maximum) {}
    HandleKind kind() const override { return kKind; }
    int32_t count;  // guarded by g_disp
    const int32_t limit;
};

class HostMutant : public HandleObject {
public:
    static constexpr HandleKind kKind = HandleKind::Mutex;
    HandleKind kind() const override { return kKind; }
    uint32_t owner = 0;  // GuestThread::thread_id; guarded by g_disp
    uint32_t depth = 0;
    bool abandoned = false;
};

// NT waitable timer (NtCreateTimer). No host thread runs per timer: whoever looks at it under
// g_disp (a wait, NtSetTimerEx, NtCancelTimer) first applies the expiry that is due, and a wait
// sleeps no later than the earliest due time of its armed timers (WakeHint).
class HostTimer : public HandleObject {
public:
    static constexpr HandleKind kKind = HandleKind::Timer;
    explicit HostTimer(bool synchronization_timer) : synchronization(synchronization_timer) {}
    HandleKind kind() const override { return kKind; }
    // Signals a due expiry; a periodic timer is rearmed past `now`, so expiries missed in
    // between coalesce into this one signal (NT keeps one signal state, not a count).
    void expire(std::chrono::steady_clock::time_point now) {
        if (!armed || now < due) return;
        signalled = true;
        if (period.count() == 0) {
            armed = false;
            return;
        }
        due += period * ((now - due) / period + 1);
    }
    const bool synchronization;  // SynchronizationTimer: a satisfied wait resets it
    bool signalled = false;      // guarded by g_disp, like the rest
    bool armed = false;
    std::chrono::steady_clock::time_point due{};
    std::chrono::steady_clock::duration period{};
};
// I/O completion port (NtCreateIoCompletion): a FIFO of completion packets,
// NT's KQUEUE. Waitable like NT's queue object: signalled while it holds a
// packet, without consuming one (only NtRemoveIoCompletion dequeues).
class HostIoCompletion : public HandleObject {
public:
    static constexpr HandleKind kKind = HandleKind::IoCompletion;
    explicit HostIoCompletion(uint32_t concurrency) : concurrent_threads(concurrency) {}
    HandleKind kind() const override { return kKind; }
    bool dispatcher_ready() const override { return !packets.empty(); }  // under g_disp
    struct Packet { uint32_t key, apc_context, status, information; };
    std::deque<Packet> packets;  // guarded by g_disp
    // NumberOfConcurrentThreads: recorded only; the host scheduler does not
    // limit how many removers run (runtime/docs/KERNEL_IMPORTS_20261003.md).
    const uint32_t concurrent_threads;
};

// Caller holds g_disp.
void set_suspend_count(GuestThreadObject& thread, int count) {
    thread.suspend_count = count;
    thread.suspend_request.store(count > 0 ? 1u : 0u, std::memory_order_relaxed);
}
// The worker object running on this host thread (null: main thread, tests'
// standalone guest threads, callbacks). Alive while it runs (obj->self).
thread_local GuestThreadObject* t_self_object = nullptr;

#if RCOMP_RUNTIME_WAIT_STATS
std::atomic<uint64_t> g_main_guest_wait_ns{0};  // blocked wall time of the main guest thread
uint64_t g_main_guest_wait_last_ns = 0;         // guarded by g_disp
void account_wait_ns(uint64_t ns) {
    if (GuestThreadObject* self = t_self_object) self->wait_ns.fetch_add(ns, std::memory_order_relaxed);
    else if (is_main_guest_host_thread()) g_main_guest_wait_ns.fetch_add(ns, std::memory_order_relaxed);
}
#endif

// Every worker, so a thread Body (KeResumeThread) can reach its suspension
// state. Weak: the registry never extends a thread object's life.
std::vector<std::weak_ptr<GuestThreadObject>> g_thread_objects;  // guarded by g_disp
// Weak entries neither keep closed objects alive nor prevent handle reuse.
// A blocked wait itself retains its objects through WaitTarget.
std::vector<std::weak_ptr<HostMutant>> g_mutants;  // guarded by g_disp

struct WaitTarget {
    uint32_t* guest_signal = nullptr;
    uint8_t guest_type = 0;
    std::shared_ptr<HostEvent> event;
    std::shared_ptr<HostSemaphore> semaphore;
    std::shared_ptr<HostMutant> mutant;
    std::shared_ptr<GuestThreadObject> thread;
    std::shared_ptr<HandleObject> notification;
    std::shared_ptr<HostTimer> timer;

    const void* identity() const {
        if (guest_signal) return guest_signal;
        if (event) return event.get();
        if (semaphore) return semaphore.get();
        if (mutant) return mutant.get();
        if (notification) return notification.get();
        if (timer) return timer.get();
        return thread.get();
    }
    bool ready(uint32_t caller) const {
        if (guest_signal) return load_s32(guest_signal) > 0;
        if (event) return event->signalled;
        if (semaphore) return semaphore->count > 0;
        if (mutant) return !mutant->owner || mutant->owner == caller;
        if (notification) return notification->dispatcher_ready();
        if (timer) {
            timer->expire(steady_now());
            return timer->signalled;
        }
        return thread->exited;
    }
    // Called only after every required object passed ready(), under g_disp.
    // Returns whether ownership was obtained from an abandoned mutant.
    bool consume(uint32_t caller) {
        if (guest_signal) {
            if (guest_type == 1) store_s32(guest_signal, 0);
            else if (guest_type == 5) store_s32(guest_signal, load_s32(guest_signal) - 1);
        } else if (event) {
            if (!event->manual_reset) event->signalled = false;
        } else if (semaphore) {
            --semaphore->count;
        } else if (timer) {
            if (timer->synchronization) timer->signalled = false;
        } else if (mutant) {
            const bool abandoned = mutant->abandoned;
            mutant->owner = caller;
            ++mutant->depth;
            mutant->abandoned = false;
            return abandoned;
        }
        return false;
    }
};

void guest_wait_target(uint32_t address, WaitTarget* out, const char* fn, PPCContext& ctx) {
    if (address & 3) rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s dispatcher 0x%08X misaligned", fn, address);
    uint8_t* p = guest_bytes(address, 16, fn, ctx, Protect::Read);
    const uint8_t type = p[0];
    if (type != 0 && type != 1 && type != 5) unimplemented(fn, ctx, "dispatcher_type", type);
    // A manual-reset event wait only reads its state. Synchronization events
    // and semaphores consume state, so their objects must also be writable.
    if (type != 0) guest_bytes(address, type == 5 ? 20 : 16, fn, ctx, Protect::ReadWrite);
    out->guest_signal = reinterpret_cast<uint32_t*>(p + 4);
    out->guest_type = type;
}

uint32_t handle_wait_target(uint32_t handle, WaitTarget* out, const char* fn) {
    auto& handles = rt_or_die(fn).handles;
    Status s = handles.lookup_as<HostEvent>(handle, &out->event);
    if (s == Status::WrongHandleKind) s = handles.lookup_as<HostSemaphore>(handle, &out->semaphore);
    if (s == Status::WrongHandleKind) s = handles.lookup_as<HostMutant>(handle, &out->mutant);
    if (s == Status::WrongHandleKind) s = handles.lookup_as<GuestThreadObject>(handle, &out->thread);
    if (s == Status::WrongHandleKind) s = handles.lookup(handle, HandleKind::Notification, &out->notification);
    if (s == Status::WrongHandleKind) s = handles.lookup_as<HostTimer>(handle, &out->timer);
    if (s == Status::WrongHandleKind) s = handles.lookup(handle, HandleKind::IoCompletion, &out->notification);
    if (s == Status::WrongHandleKind) return kStatusObjectTypeMismatch;
    return s == Status::Ok ? kStatusSuccess : kStatusInvalidHandle;
}

struct TimerWake {
    const WaitTarget* objects;
    uint32_t count;
};
// WakeHint of a wait with timers; called under g_disp.
std::chrono::steady_clock::time_point earliest_timer(const void* arg) {
    const auto& w = *static_cast<const TimerWake*>(arg);
    auto earliest = std::chrono::steady_clock::time_point::max();
    for (uint32_t i = 0; i < w.count; ++i)
        if (w.objects[i].timer && w.objects[i].timer->armed) earliest = std::min(earliest, w.objects[i].timer->due);
    return earliest;
}

// alert_thread: the caller's guest thread id for a user-mode alertable wait
// (0 otherwise); a pending user APC then ends the wait with STATUS_USER_APC
// unless an object is already signalled. signal_first (NtSignalAndWait) runs
// under g_disp immediately before the wait, atomically with it; a non-zero
// result is returned without waiting.
uint32_t wait_targets(WaitTarget* objects, uint32_t count, bool wait_all,
                      uint32_t timeout_ptr, const char* fn, uint32_t alert_thread = 0,
                      uint32_t (*signal_first)(void*) = nullptr, void* signal_arg = nullptr) {
    // All guest memory validation and handle retention precede the lock and
    // any state changes. A failed WaitAll cannot consume even one signal.
    const auto deadline = wait_deadline(timeout_ptr);
    uint32_t caller = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (objects[i].mutant) caller = current_or_die(fn).thread_id;
        for (uint32_t j = 0; j < i; ++j)
            if (objects[i].identity() == objects[j].identity()) return kStatusInvalidParameter;
    }
    uint32_t result = kStatusSuccess;
    const void* ids[64];
    const uint32_t id_count = count <= 64 ? count : 0;  // more than 64: broadcast wakes only
    for (uint32_t i = 0; i < id_count; ++i) ids[i] = objects[i].identity();
    const TimerWake timers{objects, count};
    const bool has_timer = std::any_of(objects, objects + count, [](const WaitTarget& t) { return bool(t.timer); });
    std::unique_lock<std::mutex> lk(g_disp);
    if (signal_first) {
        const uint32_t signalled = signal_first(signal_arg);
        if (signalled != kStatusSuccess) return signalled;
    }
    const uint32_t status = wait_until(lk, deadline, [&] {
        uint32_t first = count;
        for (uint32_t i = 0; i < count; ++i) {
            if (!objects[i].ready(caller)) {
                if (wait_all) {
                    first = count;
                    break;
                }
            } else if (first == count) {
                first = i;
            }
        }
        if (first == count) {
            if (alert_thread && user_apc_pending_locked(alert_thread)) {
                result = kStatusUserApc;
                return true;
            }
            return false;
        }
        const uint32_t begin = wait_all ? 0 : first, end = wait_all ? count : first + 1;
        for (uint32_t i = begin; i < end; ++i) {
            if (objects[i].mutant && objects[i].mutant->depth == 0x80000000u) {
                // Guest exception dispatch is not yet implemented. Do not
                // overflow ownership or partially consume a WaitAll set.
                lk.unlock();
                rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "%s mutant recursion limit (guest exception required)", fn);
            }
        }
        result = wait_all ? kStatusSuccess : first;
        for (uint32_t i = begin; i < end; ++i)
            if (objects[i].consume(caller) && result < kStatusAbandoned)
                result = kStatusAbandoned + (wait_all ? 0 : i);
        return true;
    }, ids, id_count, has_timer ? &earliest_timer : nullptr, &timers);
    return status == kStatusSuccess ? result : status;
}

// The calling guest thread's id when (WaitMode, Alertable) make a wait a
// user-mode alertable one, else 0 (KPROCESSOR_MODE and BOOLEAN are bytes).
uint32_t alertable_thread(uint32_t mode, uint32_t alertable) {
    if ((mode & 0xFF) != kUserMode || !(alertable & 0xFF)) return 0;
    GuestThread* t = current_guest_thread();
    return t ? t->thread_id : 0;
}

// After a wait ended with STATUS_USER_APC: runs every user APC queued to the
// calling thread, oldest first, on its own guest context (NT delivers them on
// the return to user mode, before the wait call returns). APCs queued by the
// routines themselves are delivered in the same pass.
uint32_t deliver_user_apcs(PPCContext& ctx, uint32_t status) {
    if (status != kStatusUserApc) return status;
    GuestThread& self = current_or_die("user APC delivery");
    uint8_t* base = rt_or_die("user APC delivery").mem->base();
    for (;;) {
        UserApc apc{};
        {
            std::lock_guard<std::mutex> lk(g_disp);
            const auto it = g_user_apcs.find(self.thread_id);
            if (it == g_user_apcs.end() || it->second.empty()) break;
            apc = it->second.front();
            it->second.pop_front();
            g_pending_apcs.fetch_sub(1, std::memory_order_relaxed);
            if (it->second.empty()) g_user_apcs.erase(it);
        }
        call_guest_routine_on_current_context(ctx, base, apc.routine, apc.context, apc.argument1, apc.argument2,
                                              "user APC routine");
    }
    return status;
}

uint32_t wait_guest_dispatcher(uint32_t address, uint32_t timeout_ptr, const char* fn, PPCContext& ctx,
                               uint32_t alert_thread = 0) {
    WaitTarget target;
    guest_wait_target(address, &target, fn, ctx);
    return wait_targets(&target, 1, false, timeout_ptr, fn, alert_thread);
}

uint32_t wait_handle(uint32_t handle, uint32_t timeout_ptr, const char* fn, PPCContext& ctx,
                     uint32_t alert_thread = 0) {
    if (timeout_ptr && !rt_or_die(fn).mem->is_accessible(timeout_ptr, 8, Protect::Read))
        return kStatusAccessViolation;
    WaitTarget target;
    const uint32_t status = handle_wait_target(handle, &target, fn);
    return status == kStatusSuccess ? wait_targets(&target, 1, false, timeout_ptr, fn, alert_thread) : status;
}

// ---- time ------------------------------------------------------------------

// KeQueryPerformanceFrequency (0x0083): LARGE_INTEGER frequency of the
// timebase read by mftb/KeQueryPerformanceCounter. Implemented: 50 MHz, the
// rate of PPC_MFTB (include/rcomp/ppc_prelude.h, patch 0007).
void KeQueryPerformanceFrequency(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 50000000ull; }

// Histogram of the intervals the title sleeps for, per calling thread (the first 16 guest threads by
// order of appearance), printed every 5 s by whichever thread crosses the period: RCOMP-DELAY
// tid=N zero=A le1ms=B le4ms=C gt4ms=D. Relaxed atomics; a diagnostic that costs one add per call.
// Why: the city profile of 4 October 2026 (arm 188) showed GTA IV's main thread inside
// KeDelayExecutionThread for 92 % of its import samples and asleep in the system call for 20 % of
// all its samples: which intervals it asks for decides what a wait quantum costs the frame.
struct DelayHistogram {
    std::atomic<uint32_t> tid{0};
    // Buckets of the interval asked for: zero, up to 1 / 2 / 5 / 10 / 20 ms, more; `absolute` counts the
    // positive (FILETIME deadline) requests among them, `sum_ns` the total asked for (the mean follows).
    std::atomic<uint64_t> b[7]{};
    std::atomic<uint64_t> absolute{0}, sum_ns{0};
    // The sleeps that were served (the wait ran to its end or was cut short): how long they really took
    // (`actual_ns`) and by how much each ended after its deadline (`over_ns`, 0 for an early end). The
    // timer granularity of the console decides what a 1 ms sleep costs the title's polling loops.
    std::atomic<uint64_t> served{0}, actual_ns{0}, over_ns{0};
};
DelayHistogram g_delay_hist[16];
std::atomic<uint64_t> g_delay_hist_period_ns{0};
DelayHistogram* delay_histogram_note(std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point now, bool absolute) {
    GuestThread* t = current_guest_thread();
    const uint32_t tid = t ? t->thread_id : 0;
    DelayHistogram* row = nullptr;
    for (auto& h : g_delay_hist) {
        uint32_t expected = 0;
        if (h.tid.load(std::memory_order_relaxed) == tid ||
            h.tid.compare_exchange_strong(expected, tid, std::memory_order_relaxed)) { row = &h; break; }
    }
    if (row) {
        const int64_t ns = (deadline - now).count();
        const int bucket = ns <= 0 ? 0 : ns <= 1000000 ? 1 : ns <= 2000000 ? 2 : ns <= 5000000 ? 3 : ns <= 10000000 ? 4 : ns <= 20000000 ? 5 : 6;
        row->b[bucket].fetch_add(1, std::memory_order_relaxed);
        if (ns > 0) row->sum_ns.fetch_add(uint64_t(ns), std::memory_order_relaxed);
        if (absolute) row->absolute.fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t now_ns = uint64_t(now.time_since_epoch().count());
    uint64_t period = g_delay_hist_period_ns.load(std::memory_order_relaxed);
    if (period == 0) { g_delay_hist_period_ns.compare_exchange_strong(period, now_ns, std::memory_order_relaxed); return row; }
    if (now_ns - period < 5000000000ull) return row;
    if (!g_delay_hist_period_ns.compare_exchange_strong(period, now_ns, std::memory_order_relaxed)) return row;
    for (auto& h : g_delay_hist) {
        const uint32_t id = h.tid.load(std::memory_order_relaxed);
        if (!id) continue;
        unsigned long long b[7], total = 0, nonzero = 0;
        for (int i = 0; i < 7; ++i) { b[i] = h.b[i].exchange(0, std::memory_order_relaxed); total += b[i]; if (i) nonzero += b[i]; }
        const unsigned long long absolute = h.absolute.exchange(0, std::memory_order_relaxed);
        const unsigned long long sum_ns = h.sum_ns.exchange(0, std::memory_order_relaxed);
        const unsigned long long served = h.served.exchange(0, std::memory_order_relaxed);
        const unsigned long long actual_ns = h.actual_ns.exchange(0, std::memory_order_relaxed);
        const unsigned long long over_ns = h.over_ns.exchange(0, std::memory_order_relaxed);
        if (total)
            std::fprintf(stderr, "RCOMP-DELAY tid=%u zero=%llu le1ms=%llu le2ms=%llu le5ms=%llu le10ms=%llu le20ms=%llu gt20ms=%llu absolute=%llu mean_ms=%.2f per_s=%.1f served=%llu actual_ms=%.1f over_us=%.1f\n",
                         id, b[0], b[1], b[2], b[3], b[4], b[5], b[6], absolute, nonzero ? double(sum_ns) / 1e6 / double(nonzero) : 0.0,
                         double(now_ns - period) / 1e9, served, double(actual_ns) / 1e6, served ? double(over_ns) / 1e3 / double(served) : 0.0);
    }
    return row;
}

// KeDelayExecutionThread (0x005A): (KPROCESSOR_MODE, BOOLEAN Alertable,
// PLARGE_INTEGER Interval) sleeps for a relative (negative, 100 ns) or until
// an absolute (positive, FILETIME) time. Returns STATUS_SUCCESS; a user-mode
// alertable sleep ends early with STATUS_USER_APC when a user APC is (or
// becomes) pending, after the APCs ran. A zero interval yields (below).
void KeDelayExecutionThread(PPCContext& ctx, uint8_t*) {
#if RCOMP_RUNTIME_DIAGNOSTICS
    t_wait_ctx = &ctx;
#endif
    const char* fn = "KeDelayExecutionThread";
    if (!ctx.r5.u32) unimplemented(fn, ctx, "interval_ptr", 0);
    std::chrono::steady_clock::time_point deadline;
    deadline_from(ctx.r5.u32, &deadline);
    const uint32_t alert = alertable_thread(ctx.r3.u32, ctx.r4.u32);
    const auto now = steady_now();
    uint64_t raw_interval = 0;
    guest_read_be64(ctx.r5.u32, &raw_interval);  // validated by deadline_from above
    DelayHistogram* const delay_row = delay_histogram_note(deadline, now, raw_interval != 0 && !(raw_interval & (uint64_t(1) << 63)));
    if (deadline <= now && !(alert && g_pending_apcs.load(std::memory_order_relaxed))) {
        // A zero (or already elapsed) interval is Sleep(0): the title's spin loops call it about
        // 190,000 times a second in the city (4 October 2026, arm 188, the main thread). The kernel
        // gives the processor to another ready thread of the same priority; a wait on the dispatcher
        // (lock, link, unlink, unlock) that returns at once does not, and the spinning thread then
        // takes the core, and the SMT sibling's share of it, from the thread it waits for.
        RCOMP_WAIT_STAT(1);
        sched_yield();
        // RCOMP_SLEEP0_PAUSE_NS (tuning environment, default 0): after the yield, rest in `pause`
        // instructions for this long before returning to the title's spin loop. On the console's SMT
        // pairs a thread spinning at full speed takes about half of the physical core from the thread
        // it is waiting for; `pause` hands the core to the sibling. A few microseconds against waits of
        // milliseconds cost the waiting thread nothing measurable. Measured as an A/B (arms 189/190).
        static const uint32_t pause_ns = [] {
            const char* v = getenv("RCOMP_SLEEP0_PAUSE_NS");
            const long n = v ? strtol(v, nullptr, 10) : 0;
            return uint32_t(n < 0 ? 0 : n > 1000000 ? 1000000 : n);
        }();
        if (pause_ns) {
            const uint64_t start = rcomp::fast_monotonic_ns();
            while (rcomp::fast_monotonic_ns() - start < pause_ns) __builtin_ia32_pause();
        }
        ctx.r3.u64 = kStatusSuccess;
        return;
    }
    // RCOMP_SLEEP_CAP_NS (tuning environment, default 0 = off; at most 1 ms): the title's main guest thread sleeps a
    // few hundred times a second for about a millisecond (relative Sleep(1), 4 October 2026: 160-450 a second, 20-30 % of
    // its wall time) and each such sleep ends 0.15 ms late on top of its length. When the awaited work is done in the
    // middle of a sleep the thread notices it at the end of the sleep, on the critical path of the frame. This knob wakes
    // the main thread after at most this long, so its polling loops poll more often. A Sleep(n) that returns early is
    // outside the documented contract (an experiment: titles that count Sleep(1) iterations to time a wait run out of
    // iterations sooner); it applies to the main guest thread and to relative sleeps of 1.1 ms or less only.
    static const int64_t sleep_cap_ns = [] {
        const char* v = getenv("RCOMP_SLEEP_CAP_NS");
        const long n = v ? strtol(v, nullptr, 10) : 0;
        return int64_t(n < 0 ? 0 : n > 1000000 ? 1000000 : n);
    }();
    if (sleep_cap_ns && (raw_interval & (uint64_t(1) << 63)) && is_main_guest_host_thread()) {
        const int64_t asked = (deadline - now).count();
        if (asked > sleep_cap_ns && asked <= 1100000) deadline = now + std::chrono::nanoseconds(sleep_cap_ns);
    }
    bool apc = false;
    std::unique_lock<std::mutex> lk(g_disp);
    const auto status = wait_until(lk, deadline, [&] { return apc = alert && user_apc_pending_locked(alert); });
    lk.unlock();
    if (delay_row) {
        const auto done = steady_now();
        delay_row->served.fetch_add(1, std::memory_order_relaxed);
        delay_row->actual_ns.fetch_add(uint64_t((done - now).count()), std::memory_order_relaxed);
        if (done > deadline) delay_row->over_ns.fetch_add(uint64_t((done - deadline).count()), std::memory_order_relaxed);
    }
    if (status == kStatusThreadTerminating) { ctx.r3.u64 = status; return; }
    ctx.r3.u64 = deliver_user_apcs(ctx, apc ? kStatusUserApc : kStatusSuccess);
}

// NtYieldExecution (0x0101): gives up the processor. Implemented with
// sched_yield(); always returns STATUS_SUCCESS (the real kernel may return
// STATUS_NO_YIELD_PERFORMED, which titles treat the same way).
void NtYieldExecution(PPCContext& ctx, uint8_t*) {
    sched_yield();
    ctx.r3.u64 = kStatusSuccess;
}

// ---- dynamic TLS -----------------------------------------------------------

std::mutex g_tls_mu;
uint64_t g_tls_used = 0;  // bit n = slot n allocated

// KeTlsAlloc (0x0152): returns a free slot index, or TLS_OUT_OF_INDEXES
// (0xFFFFFFFF) when all 64 are taken. The slot reads 0 in the calling thread.
void KeTlsAlloc(PPCContext& ctx, uint8_t*) {
    GuestThread& t = current_or_die("KeTlsAlloc");
    std::lock_guard<std::mutex> lk(g_tls_mu);
    for (uint32_t i = 0; i < kTlsDynamicSlots; ++i)
        if (!(g_tls_used & (1ull << i))) {
            g_tls_used |= 1ull << i;
            t.tls_values[i] = 0;
            ctx.r3.u64 = i;
            return;
        }
    ctx.r3.u64 = kTlsOutOfIndexes;
}
// KeTlsFree (0x0153): releases a slot; returns TRUE, FALSE for an index out
// of range or not allocated.
void KeTlsFree(PPCContext& ctx, uint8_t*) {
    const uint32_t i = ctx.r3.u32;
    std::lock_guard<std::mutex> lk(g_tls_mu);
    const bool ok = i < kTlsDynamicSlots && (g_tls_used & (1ull << i));
    if (ok) g_tls_used &= ~(1ull << i);
    ctx.r3.u64 = ok ? 1 : 0;
}
// KeTlsGetValue (0x0154): value of the slot in the calling thread (the real
// kernel has no error path; an out-of-range index is a title bug and traps).
void KeTlsGetValue(PPCContext& ctx, uint8_t*) {
    GuestThread& t = current_or_die("KeTlsGetValue");
    if (ctx.r3.u32 >= kTlsDynamicSlots) unimplemented("KeTlsGetValue", ctx, "index", ctx.r3.u32);
    ctx.r3.u64 = t.tls_values[ctx.r3.u32];
}
// KeTlsSetValue (0x0155): stores the value; returns TRUE (FALSE out of range).
void KeTlsSetValue(PPCContext& ctx, uint8_t*) {
    GuestThread& t = current_or_die("KeTlsSetValue");
    const uint32_t i = ctx.r3.u32;
    if (i >= kTlsDynamicSlots) {
        ctx.r3.u64 = 0;
        return;
    }
    t.tls_values[i] = ctx.r4.u32;
    ctx.r3.u64 = 1;
}

// ---- critical sections -----------------------------------------------------

void init_critical_section(uint32_t cs, uint32_t spin_div_256, const char* fn, PPCContext& ctx) {
    uint8_t* p = guest_bytes(cs, 28, fn, ctx);
    memset(p, 0, 28);
    p[0] = 1;  // synchronization event
    p[1] = uint8_t(spin_div_256);
    p[2] = 7;  // size in dwords
    const uint32_t list = __builtin_bswap32(cs + 8);
    memcpy(p + 8, &list, 4);   // empty wait list: flink = blink = &wait_list
    memcpy(p + 12, &list, 4);
    note_bytes_write(p, 28);
    store_s32(reinterpret_cast<uint32_t*>(p + 0x10), -1);
}

// RtlInitializeCriticalSection (0x012E): VOID (PRTL_CRITICAL_SECTION).
void RtlInitializeCriticalSection(PPCContext& ctx, uint8_t*) {
    init_critical_section(ctx.r3.u32, 0, "RtlInitializeCriticalSection", ctx);
}
// RtlInitializeCriticalSectionAndSpinCount (0x012F): spin count rounded up to
// a multiple of 256 (max 255*256) in header.absolute; returns STATUS_SUCCESS.
// The spin count only tunes the real kernel's busy-wait; waits here block.
void RtlInitializeCriticalSectionAndSpinCount(PPCContext& ctx, uint8_t*) {
    uint32_t div = (ctx.r4.u32 + 255) >> 8;
    if (div > 255) div = 255;
    init_critical_section(ctx.r3.u32, div, "RtlInitializeCriticalSectionAndSpinCount", ctx);
    ctx.r3.u64 = kStatusSuccess;
}

struct CsView {
    uint32_t* signal;
    uint32_t* lock_count;
    uint32_t* recursion;
    uint32_t* owner;
};
CsView cs_view(uint32_t cs, const char* fn, PPCContext& ctx) {
    uint8_t* p = guest_bytes(cs, 28, fn, ctx);
    if (cs & 3) rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s: critical section 0x%08X misaligned", fn, cs);
    return {reinterpret_cast<uint32_t*>(p + 4), reinterpret_cast<uint32_t*>(p + 0x10),
            reinterpret_cast<uint32_t*>(p + 0x14), reinterpret_cast<uint32_t*>(p + 0x18)};
}

// RtlEnterCriticalSection (0x0125): recursive lock owned by the calling
// thread (owner token = its PCR address). lock_count counts the owner's
// recursion and the waiters; a contended enter waits on the embedded
// synchronization event, signalled by the leave that hands the lock over.
void RtlEnterCriticalSection(PPCContext& ctx, uint8_t*) {
    const char* fn = "RtlEnterCriticalSection";
    const uint32_t me = current_or_die(fn).pcr;
    CsView v = cs_view(ctx.r3.u32, fn, ctx);
    if ((uint32_t)load_s32(v.owner) == me) {
        add_s32(v.lock_count, 1);
        store_s32(v.recursion, load_s32(v.recursion) + 1);
        return;
    }
    if (add_s32(v.lock_count, 1) != 0) {  // contended: wait for the hand-over
        uint32_t status;
        {
            std::unique_lock<std::mutex> lk(g_disp);
            const void* id = v.signal;
            status = wait_until(lk, std::chrono::steady_clock::time_point::max(), [&] {
                if (load_s32(v.signal) <= 0) return false;
                store_s32(v.signal, 0);
                return true;
            }, &id, 1);
        }
        // No live C++ lock when unwinding POD-only generated guest frames.
        if (status == kStatusThreadTerminating) exit_current_guest_thread(status);
    }
    store_s32(v.owner, (int32_t)me);
    store_s32(v.recursion, 1);
}

// RtlTryEnterCriticalSection (0x0141): TRUE if the lock was free or already
// owned by the caller, FALSE otherwise (never waits).
void RtlTryEnterCriticalSection(PPCContext& ctx, uint8_t*) {
    const char* fn = "RtlTryEnterCriticalSection";
    const uint32_t me = current_or_die(fn).pcr;
    CsView v = cs_view(ctx.r3.u32, fn, ctx);
    if (cas_s32(v.lock_count, -1, 0)) {
        store_s32(v.owner, (int32_t)me);
        store_s32(v.recursion, 1);
        ctx.r3.u64 = 1;
    } else if ((uint32_t)load_s32(v.owner) == me) {
        add_s32(v.lock_count, 1);
        store_s32(v.recursion, load_s32(v.recursion) + 1);
        ctx.r3.u64 = 1;
    } else {
        ctx.r3.u64 = 0;
    }
}

// RtlLeaveCriticalSection (0x0130): the retail kernel does not check the
// caller: it decrements RecursionCount; while that stays nonzero it only
// decrements LockCount, otherwise it clears OwningThread and, when LockCount
// shows waiters, wakes one. A title that leaves from another thread than the
// one that entered (GTA IV does, in play) gets exactly that behavior here.
void RtlLeaveCriticalSection(PPCContext& ctx, uint8_t*) {
    const char* fn = "RtlLeaveCriticalSection";
    const uint32_t me = current_or_die(fn).pcr;
    CsView v = cs_view(ctx.r3.u32, fn, ctx);
    if ((uint32_t)load_s32(v.owner) != me || load_s32(v.recursion) <= 0) {
        static std::atomic<uint32_t> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 4)
            std::fprintf(stderr, "RCOMP-CS leave by non-owner cs=0x%08X owner=0x%08X caller_pcr=0x%08X recursion=%d lr=0x%08X\n",
                         ctx.r3.u32, (uint32_t)load_s32(v.owner), me, load_s32(v.recursion), uint32_t(ctx.lr));
    }
    const int32_t rec = load_s32(v.recursion) - 1;
    store_s32(v.recursion, rec);
    if (rec != 0) {
        add_s32(v.lock_count, -1);
        return;
    }
    store_s32(v.owner, 0);
    if (add_s32(v.lock_count, -1) != -1) {  // waiters: hand the lock to one of them
        std::lock_guard<std::mutex> lk(g_disp);
        store_s32(v.signal, 1);
        signal_object(v.signal);
    }
}

// ---- guest events (KEVENT) -------------------------------------------------

// KeInitializeEvent (0x0070): (PRKEVENT, EVENT_TYPE Type, BOOLEAN State);
// type 0 = notification, 1 = synchronization.
void KeInitializeEvent(PPCContext& ctx, uint8_t*) {
    const char* fn = "KeInitializeEvent";
    if (ctx.r4.u32 > 1) unimplemented(fn, ctx, "event_type", ctx.r4.u32);
    uint8_t* p = guest_bytes(ctx.r3.u32, 16, fn, ctx);
    std::lock_guard<std::mutex> lk(g_disp);
    memset(p, 0, 16);
    p[0] = uint8_t(ctx.r4.u32);
    p[2] = 4;  // size in dwords
    store_s32(reinterpret_cast<uint32_t*>(p + 4), (ctx.r5.u32 & 0xFF) ? 1 : 0);
    const uint32_t list = __builtin_bswap32(ctx.r3.u32 + 8);
    memcpy(p + 8, &list, 4);
    memcpy(p + 12, &list, 4);
    note_bytes_write(p, 16);
    signal_all();
}
// KeSetEvent (0x009D): (PRKEVENT, KPRIORITY Increment, BOOLEAN Wait) signals
// the event, returns the previous signal state. Increment/Wait only tune the
// real scheduler.
void KeSetEvent(PPCContext& ctx, uint8_t*) {
    uint32_t* s = guest_word(ctx.r3.u32 + 4, "KeSetEvent", ctx);
    std::lock_guard<std::mutex> lk(g_disp);
    ctx.r3.u64 = (uint32_t)load_s32(s);
    store_s32(s, 1);
    signal_object(s);
}
// KeResetEvent (0x008F): clears the event, returns the previous signal state.
void KeResetEvent(PPCContext& ctx, uint8_t*) {
    uint32_t* s = guest_word(ctx.r3.u32 + 4, "KeResetEvent", ctx);
    std::lock_guard<std::mutex> lk(g_disp);
    ctx.r3.u64 = (uint32_t)load_s32(s);
    store_s32(s, 0);
}

// KeWaitForSingleObject (0x00B0): (PVOID Object, KWAIT_REASON, KPROCESSOR_MODE,
// BOOLEAN Alertable, PLARGE_INTEGER Timeout). Supports guest events and
// semaphores. Other guest layouts (mutant, thread, timer, queue) still trap.
void KeWaitForSingleObject(PPCContext& ctx, uint8_t*) {
#if RCOMP_RUNTIME_DIAGNOSTICS
    t_wait_ctx = &ctx;
#endif
    const uint32_t alert = alertable_thread(ctx.r5.u32, ctx.r6.u32);
    ctx.r3.u64 = deliver_user_apcs(ctx, wait_guest_dispatcher(ctx.r3.u32, ctx.r7.u32, "KeWaitForSingleObject", ctx, alert));
}

// ---- handle events ---------------------------------------------------------

// NtCreateEvent (0x00D1): (PHANDLE, POBJECT_ATTRIBUTES, EVENT_TYPE, BOOLEAN
// InitialState). Implemented: unnamed events (ObjectAttributes NULL or with
// ObjectName NULL). Named events trap.
void NtCreateEvent(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtCreateEvent";
    Runtime& r = rt_or_die(fn);
    const uint32_t output=ctx.r3.u32, attrs = ctx.r4.u32, type = ctx.r5.u32;
    if(!writable_word(output)){ctx.r3.u64=kStatusAccessViolation;return;}
    if (type > 1) unimplemented(fn, ctx, "event_type", type);
    const uint32_t attributes_status=unnamed_attributes(attrs,fn,ctx);
    if(attributes_status){ctx.r3.u64=attributes_status;return;}
    auto ev = make_object<HostEvent>(type == 0, (ctx.r6.u32 & 0xFF) != 0);
    if(!ev){ctx.r3.u64=kStatusNoMemory;return;}
    uint32_t h = 0;
    if (r.handles.insert(ev, &h) != Status::Ok) {
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }
    guest_write_be32(output,h);
    ctx.r3.u64 = kStatusSuccess;
}

uint32_t with_host_event(uint32_t handle, const char* fn, PPCContext& ctx, std::shared_ptr<HostEvent>* out) {
    Status s = rt_or_die(fn).handles.lookup_as<HostEvent>(handle, out);
    if (s == Status::WrongHandleKind) return kStatusObjectTypeMismatch;
    return s == Status::Ok ? kStatusSuccess : kStatusInvalidHandle;
}

// NtSetEvent (0x00F6): (HANDLE, PLONG PreviousState optional).
void NtSetEvent(PPCContext& ctx, uint8_t*) {
    std::shared_ptr<HostEvent> ev;
    uint32_t st = with_host_event(ctx.r3.u32, "NtSetEvent", ctx, &ev);
    if (st == kStatusSuccess) {
        if(ctx.r4.u32 && !writable_word(ctx.r4.u32)){ctx.r3.u64=kStatusAccessViolation;return;}
        std::lock_guard<std::mutex> lk(g_disp);
        if (ctx.r4.u32) guest_write_be32(ctx.r4.u32, ev->signalled ? 1 : 0);
        ev->signalled = true;
        signal_object(ev.get());
    }
    ctx.r3.u64 = st;
}
// NtClearEvent (0x00CE): (HANDLE).
void NtClearEvent(PPCContext& ctx, uint8_t*) {
    std::shared_ptr<HostEvent> ev;
    uint32_t st = with_host_event(ctx.r3.u32, "NtClearEvent", ctx, &ev);
    if (st == kStatusSuccess) {
        std::lock_guard<std::mutex> lk(g_disp);
        ev->signalled = false;
    }
    ctx.r3.u64 = st;
}
// NtPulseEvent (0x00E2): (HANDLE, PLONG PreviousState optional) -> NTSTATUS.
// NT semantics (KePulseEvent): set the event, satisfy the waits that this
// makes satisfiable at that instant, then reset it. A notification event
// releases every thread then waiting on it, a synchronization event at most
// one; a thread that starts waiting after the pulse is not released, and the
// event ends non-signalled whatever its previous state. PreviousState receives
// the signal state before the pulse (Xenia 95a5c3e always writes 1 here; this
// follows NT, whose KePulseEvent returns the previous state).
void NtPulseEvent(PPCContext& ctx, uint8_t*) {
    std::shared_ptr<HostEvent> ev;
    const uint32_t st = with_host_event(ctx.r3.u32, "NtPulseEvent", ctx, &ev);
    if (st != kStatusSuccess) { ctx.r3.u64 = st; return; }
    const uint32_t previous = ctx.r4.u32;
    if (previous && !writable_word(previous)) { ctx.r3.u64 = kStatusAccessViolation; return; }
    std::lock_guard<std::mutex> lk(g_disp);
    const bool was_signalled = ev->signalled;
    ev->signalled = true;
    satisfy_current_waiters(ev.get(), [](const void* id) { return static_cast<const HostEvent*>(id)->signalled; });
    ev->signalled = false;
    if (previous) guest_write_be32(previous, was_signalled ? 1u : 0u);
    ctx.r3.u64 = kStatusSuccess;
}
// NtWaitForSingleObjectEx (0x00FD): (HANDLE, KPROCESSOR_MODE, BOOLEAN
// Alertable, PLARGE_INTEGER Timeout). Implemented for event and thread
// handles (a thread is signalled when it has exited).
void NtWaitForSingleObjectEx(PPCContext& ctx, uint8_t*) {
#if RCOMP_RUNTIME_DIAGNOSTICS
    t_wait_ctx = &ctx;
#endif
    const uint32_t alert = alertable_thread(ctx.r4.u32, ctx.r5.u32);
    ctx.r3.u64 = deliver_user_apcs(ctx, wait_handle(ctx.r3.u32, ctx.r6.u32, "NtWaitForSingleObjectEx", ctx, alert));
}

// ---- semaphores, mutants, multiple waits -----------------------------------
// Xbox signatures/ordinals and semaphore layout checked against Xenia
// 95a5c3ee250f80c3b9d139658649d9ffb6db3eec. This is an independent dispatcher;
// no Xenia kernel, native wait implementation or object table is imported.

bool writable_word(uint32_t address) {
    return address && !(address & 3) && runtime()->mem->is_accessible(address, 4, Protect::ReadWrite);
}

uint32_t unnamed_attributes(uint32_t address, const char* fn, PPCContext& ctx) {
    if (!address) return kStatusSuccess;
    if ((address & 3) || !runtime()->mem->is_accessible(address, 12, Protect::Read))
        return kStatusAccessViolation;
    uint8_t* p = runtime()->mem->host(address);
    uint32_t name;
    memcpy(&name, p + 4, 4);
    name = __builtin_bswap32(name);
    if (name) unimplemented(fn, ctx, "object_name", name);
    return kStatusSuccess;
}

// KeInitializeSemaphore(PKSEMAPHORE, LONG Count, LONG Limit). The 20-byte
// guest object contains a dispatcher header and a signed limit at +16.
void KeInitializeSemaphore(PPCContext& ctx, uint8_t*) {
    const char* fn = "KeInitializeSemaphore";
    const int32_t count = ctx.r4.s32, limit = ctx.r5.s32;
    if (count < 0 || limit <= 0 || count > limit) unimplemented(fn, ctx, "invalid_count_or_limit", ctx.r4.u32);
    if (ctx.r3.u32 & 3) rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s misaligned object", fn);
    uint8_t* p = guest_bytes(ctx.r3.u32, 20, fn, ctx);
    std::lock_guard<std::mutex> lk(g_disp);
    memset(p, 0, 20);
    p[0] = 5; p[2] = 5;
    store_s32(reinterpret_cast<uint32_t*>(p + 4), count);
    store_s32(reinterpret_cast<uint32_t*>(p + 8), (int32_t)(ctx.r3.u32 + 8));
    store_s32(reinterpret_cast<uint32_t*>(p + 12), (int32_t)(ctx.r3.u32 + 8));
    store_s32(reinterpret_cast<uint32_t*>(p + 16), limit);
    signal_all();
}

// KeReleaseSemaphore(PKSEMAPHORE, KPRIORITY, LONG Adjustment, BOOLEAN Wait).
// Returns previous count. Scheduler priority/Wait hints do not alter signal
// accounting. An invalid/overflowing release needs a guest exception: fatal.
void KeReleaseSemaphore(PPCContext& ctx, uint8_t*) {
    const char* fn = "KeReleaseSemaphore";
    if (ctx.r3.u32 & 3) rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s misaligned object", fn);
    uint8_t* p = guest_bytes(ctx.r3.u32, 20, fn, ctx);
    if (p[0] != 5) unimplemented(fn, ctx, "dispatcher_type", p[0]);
    const int32_t adjustment = ctx.r5.s32;
    std::unique_lock<std::mutex> lk(g_disp);
    auto* signal = reinterpret_cast<uint32_t*>(p + 4);
    const int32_t old = load_s32(signal), limit = load_s32(reinterpret_cast<uint32_t*>(p + 16));
    if (adjustment <= 0 || old < 0 || limit <= 0 || int64_t(old) + adjustment > limit) {
        lk.unlock();
        unimplemented(fn, ctx, "release_requires_guest_exception", ctx.r5.u32);
    }
    store_s32(signal, old + adjustment);
    ctx.r3.u64 = uint32_t(old);
    signal_object(signal);
}

// NtCreateSemaphore(PHANDLE, POBJECT_ATTRIBUTES, LONG Initial, LONG Maximum).
void NtCreateSemaphore(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtCreateSemaphore";
    Runtime& r = rt_or_die(fn);
    const uint32_t output = ctx.r3.u32;
    if (!writable_word(output)) { ctx.r3.u64 = kStatusAccessViolation; return; }
    const uint32_t attrs = unnamed_attributes(ctx.r4.u32, fn, ctx);
    if (attrs) { ctx.r3.u64 = attrs; return; }
    const int32_t initial = ctx.r5.s32, limit = ctx.r6.s32;
    if (initial < 0 || limit <= 0 || initial > limit) { ctx.r3.u64 = kStatusInvalidParameter; return; }
    uint32_t h;
    auto semaphore=make_object<HostSemaphore>(initial,limit);
    if(!semaphore){ctx.r3.u64=kStatusNoMemory;return;}
    const Status s = r.handles.insert(std::move(semaphore), &h);
    if (s != Status::Ok) { ctx.r3.u64 = kStatusNoMemory; return; }
    guest_write_be32(output, h);
#if RCOMP_RUNTIME_DIAGNOSTICS
    {   // bring-up tracing
        static std::atomic<int> shown{0};
        if (shown.fetch_add(1) < 60)
            std::fprintf(stderr, "RCOMP-SEM create handle=0x%08X initial=%d limit=%d thread=%u lr=0x%08X\n", h, initial, limit,
                         current_guest_thread() ? current_guest_thread()->thread_id : 0u, (uint32_t)ctx.lr);
    }
#endif
    ctx.r3.u64 = kStatusSuccess;
}

// NtReleaseSemaphore(HANDLE, LONG ReleaseCount, PLONG PreviousCount optional).
void NtReleaseSemaphore(PPCContext& ctx, uint8_t*) {
    auto& r = rt_or_die("NtReleaseSemaphore");
    const uint32_t output = ctx.r5.u32;
    if (output && !writable_word(output)) { ctx.r3.u64 = kStatusAccessViolation; return; }
    std::shared_ptr<HostSemaphore> semaphore;
    const Status s = r.handles.lookup_as<HostSemaphore>(ctx.r3.u32, &semaphore);
    if (s != Status::Ok) {
        ctx.r3.u64 = s == Status::WrongHandleKind ? kStatusObjectTypeMismatch : kStatusInvalidHandle; return;
    }
    const int32_t adjustment = ctx.r4.s32;
    if (adjustment <= 0) { ctx.r3.u64 = kStatusInvalidParameter; return; }
    std::lock_guard<std::mutex> lk(g_disp);
    if (int64_t(semaphore->count) + adjustment > semaphore->limit) { ctx.r3.u64 = kStatusSemaphoreLimit; return; }
#if RCOMP_RUNTIME_DIAGNOSTICS
    {   // bring-up tracing: first releases of each semaphore
        static std::atomic<int> shown{0};
        if (shown.fetch_add(1) < 120)
            std::fprintf(stderr, "RCOMP-SEM release handle=0x%08X count=%d adjust=%d thread=%u lr=0x%08X\n", ctx.r3.u32, semaphore->count,
                         adjustment, current_guest_thread() ? current_guest_thread()->thread_id : 0u, (uint32_t)ctx.lr);
    }
    {
        uint64_t releases_now;
        { std::lock_guard<std::mutex> stat_lock(g_sem_stat_mutex); releases_now = ++g_sem_releases[ctx.r3.u32]; }
        if (releases_now % 256 == 1) {   // bring-up tracing: who kicks this semaphore
            char chain[400]; int n = 0; uint32_t sp = ctx.r1.u32;
            for (int frame = 0; frame < 10 && sp; ++frame) {
                uint32_t back = 0, saved = 0;
                if (!guest_read_be32(sp, &back) || !back || !guest_read_be32(back - 8, &saved)) break;
                n += std::snprintf(chain + n, sizeof chain - n, " %08X", saved); sp = back;
            }
            chain[n] = 0;
            std::fprintf(stderr, "RCOMP-SEMKICK t=%llums handle=0x%08X release#%llu thread=%u chain:%s\n", (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % 100000000ull, ctx.r3.u32, (unsigned long long)releases_now, current_guest_thread() ? current_guest_thread()->thread_id : 0u, chain);
        }
    }
#endif
    if (output) guest_write_be32(output, uint32_t(semaphore->count));
    semaphore->count += adjustment;
    signal_object(semaphore.get());
    ctx.r3.u64 = kStatusSuccess;
}

// NtCreateMutant(PHANDLE, POBJECT_ATTRIBUTES, BOOLEAN InitialOwner).
void NtCreateMutant(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtCreateMutant";
    Runtime& r = rt_or_die(fn);
    const uint32_t output = ctx.r3.u32;
    if (!writable_word(output)) { ctx.r3.u64 = kStatusAccessViolation; return; }
    const uint32_t attrs = unnamed_attributes(ctx.r4.u32, fn, ctx);
    if (attrs) { ctx.r3.u64 = attrs; return; }
    const uint32_t owner = (ctx.r5.u32 & 0xFF) ? current_or_die(fn).thread_id : 0;
    auto mutant = make_object<HostMutant>();
    if(!mutant){ctx.r3.u64=kStatusNoMemory;return;}
    mutant->owner = owner; mutant->depth = owner ? 1 : 0;
    std::lock_guard<std::mutex> lk(g_disp);
    // Bound registry storage to live objects even if a title continuously
    // creates and closes mutants without ending its main thread.
    for (auto it = g_mutants.begin(); it != g_mutants.end(); )
        if (it->expired()) it = g_mutants.erase(it); else ++it;
#if defined(__cpp_exceptions)
    try { g_mutants.reserve(g_mutants.size()+1); }
    catch(const std::bad_alloc&){ctx.r3.u64=kStatusNoMemory;return;}
#else
    g_mutants.reserve(g_mutants.size()+1);
#endif
    uint32_t h;
    if (r.handles.insert(mutant, &h) != Status::Ok) { ctx.r3.u64 = kStatusNoMemory; return; }
    g_mutants.emplace_back(mutant);
    guest_write_be32(output, h);
    ctx.r3.u64 = kStatusSuccess;
}

// Xbox NtReleaseMutant(HANDLE, DWORD ReservedZero), unlike NT's output
// pointer variant. The second argument's nonzero meaning remains unknown.
void NtReleaseMutant(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtReleaseMutant";
    if (ctx.r4.u32) unimplemented(fn, ctx, "reserved", ctx.r4.u32);
    auto& r = rt_or_die(fn);
    std::shared_ptr<HostMutant> mutant;
    const Status s = r.handles.lookup_as<HostMutant>(ctx.r3.u32, &mutant);
    if (s != Status::Ok) {
        ctx.r3.u64 = s == Status::WrongHandleKind ? kStatusObjectTypeMismatch : kStatusInvalidHandle; return;
    }
    const uint32_t caller = current_or_die(fn).thread_id;
    std::lock_guard<std::mutex> lk(g_disp);
    if (mutant->owner != caller) { ctx.r3.u64 = kStatusMutantNotOwned; return; }
    if (--mutant->depth == 0) { mutant->owner = 0; signal_all(); }
    ctx.r3.u64 = kStatusSuccess;
}

uint32_t wait_multiple(uint32_t count, uint32_t array, uint32_t type, uint32_t timeout,
                       bool handles, const char* fn, PPCContext& ctx, uint32_t alert_thread) {
    if (!count || count > kMaximumWaitObjects || type > 1) return kStatusInvalidParameter;
    auto* memory = rt_or_die(fn).mem;
    if ((array & 3) || !memory->is_accessible(array, count * 4, Protect::Read)) {
        if (handles) return kStatusAccessViolation;
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s object array 0x%08X not readable or misaligned", fn, array);
    }
    if (timeout && !memory->is_accessible(timeout, 8, Protect::Read)) {
        if (handles) return kStatusAccessViolation;
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s timeout 0x%08X not readable", fn, timeout);
    }
    uint8_t* p = memory->host(array);
    std::array<WaitTarget, kMaximumWaitObjects> targets;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t value;
        memcpy(&value, p + i * 4, 4);
        value = __builtin_bswap32(value);
        if (handles) {
            const uint32_t status = handle_wait_target(value, &targets[i], fn);
            if (status) return status;
        } else {
            guest_wait_target(value, &targets[i], fn, ctx);
        }
    }
    return wait_targets(targets.data(), count, type == 0, timeout, fn, alert_thread);
}

// Ke signature: Count, Objects, WaitType, Reason, Mode, Alertable, Timeout,
// WaitBlockArray. Bookkeeping lives on the host stack; the opaque guest wait
// blocks are not linked into guest lists. Nt omits Reason and WaitBlockArray.
// A user-mode alertable wait delivers user APCs (NtQueueApcThread); no alert producer exists.
void KeWaitForMultipleObjects(PPCContext& ctx, uint8_t*) {
#if RCOMP_RUNTIME_DIAGNOSTICS
    t_wait_ctx = &ctx;
#endif
    const uint32_t alert = alertable_thread(ctx.r7.u32, ctx.r8.u32);
    ctx.r3.u64 = deliver_user_apcs(ctx, wait_multiple(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r9.u32,
                                                      false, "KeWaitForMultipleObjects", ctx, alert));
}
void NtWaitForMultipleObjectsEx(PPCContext& ctx, uint8_t*) {
#if RCOMP_RUNTIME_DIAGNOSTICS
    t_wait_ctx = &ctx;
#endif
    const uint32_t alert = alertable_thread(ctx.r6.u32, ctx.r7.u32);
    ctx.r3.u64 = deliver_user_apcs(ctx, wait_multiple(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r8.u32,
                                                      true, "NtWaitForMultipleObjectsEx", ctx, alert));
}

// ---- threads ---------------------------------------------------------------

// ---- interlocked singly linked lists ---------------------------------------
// SLIST_HEADER (8 bytes, big-endian, 8-aligned): +0 u32 first entry, +4 u16
// depth, +6 u16 sequence; an entry's first word is the next entry (Xenia
// 95a5c3e xbox.h, xboxkrnl_threading.cc). Titles usually inline the push with
// ldarx/stdcx. on the whole header, so the HLE operates on the same 8 bytes
// with a 64-bit compare-and-swap, never under the dispatcher lock.

uint64_t* slist_header(uint32_t address, const char* fn, PPCContext& ctx) {
    if (address & 7) rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s header 0x%08X misaligned lr=0x%08X",
                                 fn, address, (uint32_t)ctx.lr);
    return reinterpret_cast<uint64_t*>(guest_bytes(address, 8, fn, ctx));
}

// InterlockedPopEntrySList (0x002C): (PSLIST_HEADER). Removes and returns the
// first entry (NULL when empty): depth - 1, sequence kept, as the reference.
void InterlockedPopEntrySList(PPCContext& ctx, uint8_t*) {
    const char* fn = "InterlockedPopEntrySList";
    uint64_t* header = slist_header(ctx.r3.u32, fn, ctx);
    uint64_t old = __atomic_load_n(header, __ATOMIC_SEQ_CST);
    for (;;) {
        const uint64_t value = __builtin_bswap64(old);
        const uint32_t first = uint32_t(value >> 32);
        if (!first) {
            ctx.r3.u64 = 0;
            return;
        }
        uint32_t next = 0;
        if ((first & 3) || !rt_or_die(fn).mem->is_accessible(first, 4, Protect::Read) || !guest_read_be32(first, &next))
            rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s entry 0x%08X not readable lr=0x%08X", fn, first,
                        (uint32_t)ctx.lr);
        const uint64_t replaced = uint64_t(next) << 32 | uint64_t(uint16_t((value >> 16) - 1)) << 16 | (value & 0xFFFF);
        if (__atomic_compare_exchange_n(header, &old, __builtin_bswap64(replaced), false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {
            note_bytes_write(reinterpret_cast<uint8_t*>(header), 8);
            ctx.r3.u64 = first;
            return;
        }
    }
}

// InterlockedFlushSList (0x002B): (PSLIST_HEADER). Detaches the whole list and
// returns its first entry (NULL when empty); the header becomes all zero, as
// the reference.
void InterlockedFlushSList(PPCContext& ctx, uint8_t*) {
    uint64_t* header = slist_header(ctx.r3.u32, "InterlockedFlushSList", ctx);
    const uint64_t old = __atomic_exchange_n(header, uint64_t(0), __ATOMIC_SEQ_CST);
    note_bytes_write(reinterpret_cast<uint8_t*>(header), 8);
    ctx.r3.u64 = uint32_t(__builtin_bswap64(old) >> 32);
}

// ---- timers ----------------------------------------------------------------
// NT waitable timers. Xbox signatures and ordinals as Xenia 95a5c3e names them
// (xboxkrnl_threading.cc); the state rules are NT's, observed on Windows ntdll
// (runtime/tests/timer_windows_oracle.py, recorded in timer_windows_oracle.json):
// a new timer is not signalled; setting one resets it to not signalled and
// arms it; an expiry signals it; a satisfied wait resets a SynchronizationTimer
// only; cancelling disarms without changing the signal state. The cases NT
// rejects with a parameter-indexed status (STATUS_INVALID_PARAMETER_n, whose n
// the Xbox signatures shift) are outside the subset and trap rather than guess.

uint32_t with_timer(uint32_t handle, const char* fn, std::shared_ptr<HostTimer>* out) {
    Status s = rt_or_die(fn).handles.lookup_as<HostTimer>(handle, out);
    if (s == Status::WrongHandleKind) return kStatusObjectTypeMismatch;
    return s == Status::Ok ? kStatusSuccess : kStatusInvalidHandle;
}

// Optional PBOOLEAN output (one byte).
bool writable_byte(uint32_t address) {
    return address && runtime()->mem->is_accessible(address, 1, Protect::ReadWrite);
}
void store_boolean(uint32_t address, bool value, const char* fn, PPCContext& ctx) {
    uint8_t* p = guest_bytes(address, 1, fn, ctx);
    *p = value ? 1 : 0;
    note_bytes_write(p, 1);
}

// NtCreateTimer (0x00D7): (PHANDLE, POBJECT_ATTRIBUTES, TIMER_TYPE).
// Implemented: unnamed NotificationTimer (0) and SynchronizationTimer (1),
// created not signalled and not armed. Named timers and other types trap.
void NtCreateTimer(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtCreateTimer";
    Runtime& r = rt_or_die(fn);
    const uint32_t output = ctx.r3.u32, attrs = ctx.r4.u32, type = ctx.r5.u32;
    if (!writable_word(output)) { ctx.r3.u64 = kStatusAccessViolation; return; }
    if (type > 1) unimplemented(fn, ctx, "timer_type", type);
    const uint32_t attributes_status = unnamed_attributes(attrs, fn, ctx);
    if (attributes_status) { ctx.r3.u64 = attributes_status; return; }
    auto timer = make_object<HostTimer>(type == 1);
    if (!timer) { ctx.r3.u64 = kStatusNoMemory; return; }
    uint32_t h = 0;
    if (r.handles.insert(timer, &h) != Status::Ok) { ctx.r3.u64 = kStatusNoMemory; return; }
    guest_write_be32(output, h);
    ctx.r3.u64 = kStatusSuccess;
}

// NtSetTimerEx (0x00FA): (HANDLE, PLARGE_INTEGER DueTime, PTIMER_APC_ROUTINE,
// KPROCESSOR_MODE ApcMode, PVOID ApcContext, BOOLEAN ResumeTimer, LONG Period
// in ms, PBOOLEAN PreviousState optional). DueTime counts 100 ns: negative is
// relative, otherwise an absolute system time (a past one expires at once).
// Implemented: no APC routine (no APC delivery exists), no resume, Period >= 0.
void NtSetTimerEx(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtSetTimerEx";
    std::shared_ptr<HostTimer> timer;
    const uint32_t due_ptr = ctx.r4.u32, previous = ctx.r10.u32;
    const uint32_t st = with_timer(ctx.r3.u32, fn, &timer);
    if (st != kStatusSuccess) { ctx.r3.u64 = st; return; }
    if (ctx.r5.u32) unimplemented(fn, ctx, "apc_routine", ctx.r5.u32);
    if (ctx.r8.u32 & 0xFF) unimplemented(fn, ctx, "resume", ctx.r8.u32);
    if (ctx.r9.s32 < 0) unimplemented(fn, ctx, "period", ctx.r9.u32);
    if (!due_ptr || !rt_or_die(fn).mem->is_accessible(due_ptr, 8, Protect::Read) ||
        (previous && !writable_byte(previous))) {
        ctx.r3.u64 = kStatusAccessViolation;
        return;
    }
    auto due = std::chrono::steady_clock::time_point::max();
    deadline_from(due_ptr, &due);
    std::lock_guard<std::mutex> lk(g_disp);
    timer->expire(steady_now());
    if (previous) store_boolean(previous, timer->signalled, fn, ctx);
    timer->signalled = false;
    timer->armed = true;
    timer->due = due;
    timer->period = std::chrono::milliseconds(ctx.r9.u32);
    signal_object(timer.get());  // waiters recompute their wake time
    ctx.r3.u64 = kStatusSuccess;
}

// NtCancelTimer (0x00CD): (HANDLE, PBOOLEAN CurrentState optional). Applies an
// expiry that is already due, disarms the timer without changing its signal
// state and reports that state.
void NtCancelTimer(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtCancelTimer";
    std::shared_ptr<HostTimer> timer;
    const uint32_t current = ctx.r4.u32;
    const uint32_t st = with_timer(ctx.r3.u32, fn, &timer);
    if (st != kStatusSuccess) { ctx.r3.u64 = st; return; }
    if (current && !writable_byte(current)) { ctx.r3.u64 = kStatusAccessViolation; return; }
    std::lock_guard<std::mutex> lk(g_disp);
    timer->expire(steady_now());
    timer->armed = false;
    if (current) store_boolean(current, timer->signalled, fn, ctx);
    signal_object(timer.get());
    ctx.r3.u64 = kStatusSuccess;
}

void* guest_thread_main(void* arg) {
    auto* obj = static_cast<GuestThreadObject*>(arg);
    bool cancelled = false;
    {
        std::unique_lock<std::mutex> lk(g_disp);
        wait_broadcast(lk, [&] { return g_stopping || obj->suspend_count == 0; });
        cancelled = g_stopping;
    }
    uint32_t code = kStatusThreadTerminating;
    if (!cancelled) {
        // NtSuspendThread reaches this worker through these (waits and the
        // import-dispatch checkpoint of src/thread_suspend.h).
        t_self_object = obj;
        t_suspend_count = &obj->suspend_count;
        set_current_thread_suspend_request(&obj->suspend_request);
        run_guest_thread(obj->thread, obj->ctx, obj->owner->mem->base(), obj->entry, &code);
        set_current_thread_suspend_request(nullptr);
        t_suspend_count = nullptr;
        t_self_object = nullptr;
    } else
        thread_object_mark_exited(obj->identity, code);
    destroy_thread_or_die(obj->owner->heap,&obj->thread,"worker completion");
    std::shared_ptr<GuestThreadObject> keep;
    {
        std::lock_guard<std::mutex> lk(g_disp);
        obj->exit_code = code;
        obj->exited = true;
        keep = std::move(obj->self);  // released after the lock
        --g_workers;  // no guest-memory/runtime access after this point
        signal_all();
    }
    return nullptr;
}

// ExCreateThread (0x000D): (PHANDLE, DWORD StackSize, LPDWORD ThreadId,
// PVOID XapiThreadStartup, PVOID StartAddress, PVOID StartContext, DWORD
// CreationFlags). With XapiThreadStartup the thread runs
// XapiThreadStartup(StartAddress, StartContext), otherwise
// StartAddress(StartContext). Implemented: guest stack from the runtime heap
// (StackSize rounded up to 64 KiB, 256 KiB when 0), a host thread with an
// 8 MiB native stack, CREATE_SUSPENDED (resumed by NtResumeThread). Other
// creation flags (processor affinity, priority, system threads) trap.
void ExCreateThread(PPCContext& ctx, uint8_t*) {
    const char* fn = "ExCreateThread";
    Runtime& r = rt_or_die(fn);
    const uint32_t phandle = ctx.r3.u32, stack = ctx.r4.u32, pid = ctx.r5.u32, startup = ctx.r6.u32,
                   start = ctx.r7.u32, context = ctx.r8.u32, flags = ctx.r9.u32;
    // Accepted: CREATE_SUSPENDED (bit 0), the priority hint bits 0x60 and the
    // hardware-thread mask in the top byte (bits 0..5 = the six Xbox 360 hardware
    // threads; 0 = unrestricted). Everything else (system threads, 0x2/0x10/0x80
    // ...) stays unimplemented. Hints are recorded per thread like KeSet*Thread.
    constexpr uint32_t kPriorityHintBits = 0x60u;
    const uint32_t hardware_mask = flags >> 24;
    if ((flags & 0x00FFFFFFu & ~(kCreateSuspended | kPriorityHintBits)) || (hardware_mask & ~kDefaultThreadAffinity))
        unimplemented(fn, ctx, "creation_flags", flags);
    if (phandle) guest_word(phandle, fn, ctx);
    if (pid) guest_word(pid, fn, ctx);
    const uint32_t entry_addr = startup ? startup : start;
    PPCFunc* entry = lookup_function(entry_addr);
    if (!entry)
        rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET, "xboxkrnl.exe!%s entry 0x%08X has no recompiled function lr=0x%08X",
                    fn, entry_addr, (uint32_t)ctx.lr);
    // Admit creation under the dispatcher lock; a new worker cannot enter
    // guest code until its ownership has been counted below.
    std::unique_lock<std::mutex> lifecycle(g_disp);
    if (g_stopping) { ctx.r3.u64 = kStatusThreadTerminating; return; }
    auto obj = make_object<GuestThreadObject>();
    if(!obj){ctx.r3.u64=kStatusNoMemory;return;}
    obj->owner = &r;
    uint32_t size = stack ? (stack + 0xFFFF) & ~0xFFFFu : kDefaultStackSize;
    Status s = create_guest_thread(r.heap, {size, entry_addr, startup ? start : context}, &obj->ctx, &obj->thread);
    if (s != Status::Ok) {
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }
    obj->identity = obj->thread.identity;
    if (hardware_mask) {
        (void)thread_object_exchange_affinity(obj->identity, hardware_mask);
        // Fake processor number: the highest hardware thread in the mask (public Xenia/rexglue behaviour).
        uint32_t cpu = 0;
        for (uint32_t bit = 0; bit < 6; ++bit)
            if (hardware_mask & (1u << bit)) cpu = bit;
        r.mem->base()[obj->thread.pcr + kPcrProcessorNumber] = uint8_t(cpu);
    }
    if (flags & kPriorityHintBits) (void)thread_object_exchange_base_priority(obj->identity, (flags & 0x20) ? 1 : 0);
    g_thread_objects.erase(std::remove_if(g_thread_objects.begin(), g_thread_objects.end(),
                                          [](const auto& weak) { return weak.expired(); }),
                           g_thread_objects.end());
    g_thread_objects.push_back(obj);
    if (startup) obj->ctx.r4.u64 = context;
    obj->ctx.fpscr.loadFromHost();
    obj->entry = entry;
    set_suspend_count(*obj, (flags & kCreateSuspended) ? 1 : 0);
    uint32_t h = 0;
    if (r.handles.insert(obj, &h) != Status::Ok) {
        destroy_thread_or_die(r.heap,&obj->thread,"thread handle insertion");
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }
    obj->self = obj;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostStackSize);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    const int err = pthread_create(&tid, &attr, guest_thread_main, obj.get());
    pthread_attr_destroy(&attr);
    if (err != 0) {
        obj->self.reset();
        const Status closed=r.handles.close(h);
        if(closed!=Status::Ok)rcomp_fatal(RCOMP_FATAL_PLATFORM,"pthread_create rollback handle: %s",status_name(closed));
        destroy_thread_or_die(r.heap,&obj->thread,"pthread_create rollback");
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }
    ++g_workers;
    if (phandle) guest_write_be32(phandle, h);
    if (pid) guest_write_be32(pid, obj->thread.thread_id);
    ctx.r3.u64 = kStatusSuccess;
}

// NtResumeThread (0x00F5): (HANDLE, PULONG PreviousSuspendCount optional);
// decrements the suspend count, the thread runs when it reaches 0.
void NtResumeThread(PPCContext& ctx, uint8_t*) {
    std::shared_ptr<GuestThreadObject> th;
    Status s = rt_or_die("NtResumeThread").handles.lookup_as<GuestThreadObject>(ctx.r3.u32, &th);
    if (s != Status::Ok) {
        ctx.r3.u64 = s == Status::WrongHandleKind ? kStatusObjectTypeMismatch : kStatusInvalidHandle;
        return;
    }
    if(ctx.r4.u32 && !writable_word(ctx.r4.u32)){ctx.r3.u64=kStatusAccessViolation;return;}
    std::lock_guard<std::mutex> lk(g_disp);
    if (ctx.r4.u32) guest_write_be32(ctx.r4.u32, (uint32_t)th->suspend_count);
    if (th->suspend_count > 0) set_suspend_count(*th, th->suspend_count - 1);
    signal_all();
    ctx.r3.u64 = kStatusSuccess;
}

// KeResumeThread (0x0092): (PKTHREAD Body) -> previous suspend count. The main
// thread and any thread without a worker object are never suspended: 0.
void KeResumeThread(PPCContext& ctx, uint8_t*) {
    std::shared_ptr<ThreadObjectIdentity> identity;
    if (find_thread_object(ctx.r3.u32, &identity) != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!KeResumeThread unknown thread body 0x%08X lr=0x%08X",
                    ctx.r3.u32, (uint32_t)ctx.lr);
    std::lock_guard<std::mutex> lk(g_disp);
    uint32_t previous = 0;
    for (const auto& weak : g_thread_objects) {
        auto obj = weak.lock();
        if (!obj || obj->identity != identity) continue;
        previous = (uint32_t)obj->suspend_count;
        if (obj->suspend_count > 0) set_suspend_count(*obj, obj->suspend_count - 1);
        break;
    }
    signal_all();
    ctx.r3.u64 = previous;
}

// NtSuspendThread (0x00FC): (HANDLE, PULONG PreviousSuspendCount optional) ->
// NTSTATUS. Increments the suspend count shared with NtResumeThread,
// KeResumeThread and CREATE_SUSPENDED, and writes the count before the
// increment. NT semantics (ReactOS PsSuspendThread/KeSuspendThread): an exited
// thread gives STATUS_THREAD_IS_TERMINATING, a count already at
// MAXIMUM_SUSPEND_COUNT (127) gives STATUS_SUSPEND_COUNT_EXCEEDED; neither
// changes the count nor writes the output.
//
// When it takes effect. NT delivers suspension asynchronously (a kernel APC);
// SuspendThread returning does not mean the target has stopped. R-comp cannot
// stop a host thread at an arbitrary guest instruction, so a suspended worker
// stops at its next kernel call (the import-dispatch checkpoint), and a worker
// already blocked in a wait stays blocked and consumes no signal (pulses
// included) until resumed; its timeout is re-checked after the resume. A
// thread suspending itself blocks inside this call until resumed, as on NT.
// A worker in a pure guest-code loop that never calls the kernel keeps
// running until it does (documented divergence, runtime/docs/SUSPEND_PULSE.md).
void NtSuspendThread(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtSuspendThread";
    constexpr int kMaximumSuspendCount = 127;
    constexpr uint32_t kStatusSuspendCountExceeded = 0xC000004A;
    constexpr uint32_t kCurrentThreadPseudoHandle = 0xFFFFFFFEu;
    if (ctx.r3.u32 == kCurrentThreadPseudoHandle) unimplemented(fn, ctx, "current_thread_pseudo_handle", ctx.r3.u32);
    std::shared_ptr<GuestThreadObject> th;
    const Status s = rt_or_die(fn).handles.lookup_as<GuestThreadObject>(ctx.r3.u32, &th);
    if (s != Status::Ok) {
        ctx.r3.u64 = s == Status::WrongHandleKind ? kStatusObjectTypeMismatch : kStatusInvalidHandle;
        return;
    }
    const uint32_t output = ctx.r4.u32;
    if (output && !writable_word(output)) { ctx.r3.u64 = kStatusAccessViolation; return; }
    uint32_t status = kStatusSuccess;
    bool stopped = false;
    {
        std::unique_lock<std::mutex> lk(g_disp);
        if (th->exited) {
            status = kStatusThreadTerminating;
        } else if (th->suspend_count >= kMaximumSuspendCount) {
            status = kStatusSuspendCountExceeded;
        } else {
            if (output) guest_write_be32(output, uint32_t(th->suspend_count));
            set_suspend_count(*th, th->suspend_count + 1);
            if (th.get() == t_self_object) {
                GuestThreadObject* self = th.get();
                wait_broadcast(lk, [self] { return g_stopping || self->suspend_count == 0; });
                stopped = g_stopping;
            }
        }
    }
    th.reset();
    // No live C++ lock or object when unwinding POD-only generated guest frames.
    if (stopped) exit_current_guest_thread(kStatusThreadTerminating);
    ctx.r3.u64 = status;
}

// KeEnableFpuExceptions (0x005D): VOID (BOOLEAN Enable). Selects whether the
// calling thread's enabled FPSCR exceptions trap (MSR FE0/FE1 on the PPC). The
// signature is Xenia 95a5c3e's (xboxkrnl_misc.cc, a no-op there); GoW2's CRT
// control-word helper calls it with Enable = "some exception is unmasked" and
// discards r3. R-comp records the state per guest thread and leaves the
// previous state in r3 (volatile, legal for a VOID export). Generated code
// never raises guest floating-point exceptions, so enabling them cannot be
// honoured: the first enable is reported once on stderr (the title only
// diverges if an unmasked FP exception condition actually occurs).
void KeEnableFpuExceptions(PPCContext& ctx, uint8_t*) {
    GuestThread& t = current_or_die("KeEnableFpuExceptions");
    const bool enable = (ctx.r3.u32 & 0xFF) != 0;
    const bool previous = t.fpu_exceptions_enabled;
    t.fpu_exceptions_enabled = enable;
    if (enable) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
            std::fprintf(stderr,
                         "RCOMP-FPU exceptions_enabled thread=%u lr=0x%08X: guest FP exceptions are never raised "
                         "by R-comp\n",
                         t.thread_id, uint32_t(ctx.lr));
    }
    ctx.r3.u64 = previous ? 1 : 0;
}

// ---- user APCs ---------------------------------------------------------------

// NtQueueApcThread (0x00E3): (HANDLE Thread, PPS_APC_ROUTINE ApcRoutine, PVOID
// ApcRoutineContext, PVOID Argument1, PVOID Argument2) -> NTSTATUS. Queues a
// user-mode APC: ApcRoutine(ApcRoutineContext, Argument1, Argument2) runs on the
// target thread the next time it waits alertably in user mode (Xenia/rexglue
// signature: KeInitializeApc with NormalRoutine/NormalContext, then
// KeInsertQueueApc with the two system arguments). The thread may be the
// current-thread pseudo-handle. A thread that has exited cannot be queued to:
// STATUS_UNSUCCESSFUL, NT's result when KeInsertQueueApc refuses. The routine
// must exist in the AOT function table (checked when queued).
void NtQueueApcThread(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtQueueApcThread";
    Runtime& r = rt_or_die(fn);
    const uint32_t handle = ctx.r3.u32, routine = ctx.r4.u32;
    if (!routine || (routine & 3))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s apc_routine=0x%08X lr=0x%08X", fn, routine,
                    (uint32_t)ctx.lr);
    if (!lookup_function(routine))
        rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET, "xboxkrnl.exe!%s APC routine 0x%08X has no recompiled function lr=0x%08X",
                    fn, routine, (uint32_t)ctx.lr);
    std::shared_ptr<GuestThreadObject> thread;
    uint32_t target = 0;
    if (handle == 0xFFFFFFFEu) {
        target = current_or_die(fn).thread_id;
    } else {
        const Status s = r.handles.lookup_as<GuestThreadObject>(handle, &thread);
        if (s != Status::Ok) {
            ctx.r3.u64 = s == Status::WrongHandleKind ? kStatusObjectTypeMismatch : kStatusInvalidHandle;
            return;
        }
        target = thread->thread.thread_id ? thread->thread.thread_id : thread_object_thread_id(thread->identity);
    }
    std::lock_guard<std::mutex> lk(g_disp);
    if (!target || (thread && (thread->exited || thread_object_exited(thread->identity)))) {
        ctx.r3.u64 = kStatusUnsuccessful;
        return;
    }
#if defined(__cpp_exceptions)
    try {
#endif
        g_user_apcs[target].push_back({routine, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32});
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }
#endif
    g_pending_apcs.fetch_add(1, std::memory_order_relaxed);
    signal_all();  // an alertable waiter of any object re-checks its APC queue
    ctx.r3.u64 = kStatusSuccess;
}

// ---- signal and wait -----------------------------------------------------------

// NtSignalAndWaitForSingleObjectEx (0x00FB): (HANDLE SignalHandle, HANDLE
// WaitHandle, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER
// Timeout). NT NtSignalAndWaitForSingleObject plus the WaitMode of every Xbox
// "Ex" wait (NtWaitForSingleObjectEx / NtWaitForMultipleObjectsEx order); the
// timeout in r7 agrees with Xenia. Signals an event (set), a semaphore
// (release 1) or an owned mutant (release 1) and waits on the second object as
// NtWaitForSingleObjectEx, both under one dispatcher hold, so no other thread
// can observe the signal before the caller waits. Signal-object errors are
// NT's: STATUS_OBJECT_TYPE_MISMATCH for an object that cannot be signalled,
// STATUS_SEMAPHORE_LIMIT_EXCEEDED, STATUS_MUTANT_NOT_OWNED; then nothing waits.
struct SignalOrder {
    std::shared_ptr<HostEvent> event;
    std::shared_ptr<HostSemaphore> semaphore;
    std::shared_ptr<HostMutant> mutant;
    uint32_t caller = 0;
};
uint32_t signal_before_wait(void* arg) {  // under g_disp
    auto& order = *static_cast<SignalOrder*>(arg);
    if (order.event) {
        order.event->signalled = true;
        signal_object(order.event.get());
    } else if (order.semaphore) {
        if (int64_t(order.semaphore->count) + 1 > order.semaphore->limit) return kStatusSemaphoreLimit;
        ++order.semaphore->count;
        signal_object(order.semaphore.get());
    } else {
        if (order.mutant->owner != order.caller) return kStatusMutantNotOwned;
        if (--order.mutant->depth == 0) { order.mutant->owner = 0; signal_all(); }
    }
    return kStatusSuccess;
}

void NtSignalAndWaitForSingleObjectEx(PPCContext& ctx, uint8_t*) {
#if RCOMP_RUNTIME_DIAGNOSTICS
    t_wait_ctx = &ctx;
#endif
    const char* fn = "NtSignalAndWaitForSingleObjectEx";
    Runtime& r = rt_or_die(fn);
    const uint32_t signal_handle = ctx.r3.u32, wait_handle_value = ctx.r4.u32, timeout = ctx.r7.u32;
    if (timeout && !r.mem->is_accessible(timeout, 8, Protect::Read)) { ctx.r3.u64 = kStatusAccessViolation; return; }
    SignalOrder order;
    Status s = r.handles.lookup_as<HostEvent>(signal_handle, &order.event);
    if (s == Status::WrongHandleKind) s = r.handles.lookup_as<HostSemaphore>(signal_handle, &order.semaphore);
    if (s == Status::WrongHandleKind) s = r.handles.lookup_as<HostMutant>(signal_handle, &order.mutant);
    if (s != Status::Ok) {
        ctx.r3.u64 = s == Status::WrongHandleKind ? kStatusObjectTypeMismatch : kStatusInvalidHandle;
        return;
    }
    if (order.mutant) order.caller = current_or_die(fn).thread_id;
    WaitTarget target;
    const uint32_t status = handle_wait_target(wait_handle_value, &target, fn);
    if (status != kStatusSuccess) { ctx.r3.u64 = status; return; }
    const uint32_t alert = alertable_thread(ctx.r5.u32, ctx.r6.u32);
    ctx.r3.u64 = deliver_user_apcs(ctx, wait_targets(&target, 1, false, timeout, fn, alert, &signal_before_wait, &order));
}

// ---- I/O completion ports ------------------------------------------------------

// NtCreateIoCompletion (0x00D3): (PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
// ULONG NumberOfConcurrentThreads) -> NTSTATUS. Unnamed ports only (a name
// traps like every named object here).
void NtCreateIoCompletion(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtCreateIoCompletion";
    Runtime& r = rt_or_die(fn);
    const uint32_t output = ctx.r3.u32;
    if (!writable_word(output)) { ctx.r3.u64 = kStatusAccessViolation; return; }
    const uint32_t attributes = unnamed_attributes(ctx.r5.u32, fn, ctx);
    if (attributes) { ctx.r3.u64 = attributes; return; }
    auto port = make_object<HostIoCompletion>(ctx.r6.u32);
    if (!port) { ctx.r3.u64 = kStatusNoMemory; return; }
    uint32_t handle = 0;
    if (r.handles.insert(port, &handle) != Status::Ok) { ctx.r3.u64 = kStatusNoMemory; return; }
    guest_write_be32(output, handle);
    ctx.r3.u64 = kStatusSuccess;
}

uint32_t with_port(uint32_t handle, const char* fn, std::shared_ptr<HostIoCompletion>* out) {
    const Status s = rt_or_die(fn).handles.lookup_as<HostIoCompletion>(handle, out);
    if (s == Status::WrongHandleKind) return kStatusObjectTypeMismatch;
    return s == Status::Ok ? kStatusSuccess : kStatusInvalidHandle;
}

// NtSetIoCompletion (0x00F8): (HANDLE, PVOID KeyContext, PVOID ApcContext,
// NTSTATUS IoStatus, ULONG_PTR IoStatusInformation) -> NTSTATUS. Appends one
// packet and wakes the port's waiters (Xenia/rexglue argument order).
void NtSetIoCompletion(PPCContext& ctx, uint8_t*) {
    std::shared_ptr<HostIoCompletion> port;
    const uint32_t st = with_port(ctx.r3.u32, "NtSetIoCompletion", &port);
    if (st != kStatusSuccess) { ctx.r3.u64 = st; return; }
    std::lock_guard<std::mutex> lk(g_disp);
#if defined(__cpp_exceptions)
    try {
#endif
        port->packets.push_back({ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32});
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }
#endif
    signal_object(port.get());
    ctx.r3.u64 = kStatusSuccess;
}

// NtRemoveIoCompletion (0x00F4): (HANDLE, PVOID* KeyContext, PVOID* ApcContext,
// PIO_STATUS_BLOCK, PLARGE_INTEGER Timeout) -> NTSTATUS. Waits (not
// alertable, as NT's KeRemoveQueue for this call) for the oldest packet and
// stores its key, APC context and {Status, Information}; STATUS_TIMEOUT when
// the timeout elapses first (outputs untouched). Outputs are validated before
// waiting; a NULL output is skipped (Xenia does the same; NT would fault).
void NtRemoveIoCompletion(PPCContext& ctx, uint8_t*) {
#if RCOMP_RUNTIME_DIAGNOSTICS
    t_wait_ctx = &ctx;
#endif
    const char* fn = "NtRemoveIoCompletion";
    Runtime& r = rt_or_die(fn);
    const uint32_t key = ctx.r4.u32, apc_context = ctx.r5.u32, iosb = ctx.r6.u32, timeout = ctx.r7.u32;
    if ((key && !writable_word(key)) || (apc_context && !writable_word(apc_context)) ||
        (iosb && ((iosb & 3) || !r.mem->is_accessible(iosb, 8, Protect::ReadWrite))) ||
        (timeout && !r.mem->is_accessible(timeout, 8, Protect::Read))) {
        ctx.r3.u64 = kStatusAccessViolation;
        return;
    }
    std::shared_ptr<HostIoCompletion> port;
    const uint32_t st = with_port(ctx.r3.u32, fn, &port);
    if (st != kStatusSuccess) { ctx.r3.u64 = st; return; }
    const auto deadline = wait_deadline(timeout);
    const void* id = port.get();
    HostIoCompletion::Packet packet{};
    std::unique_lock<std::mutex> lk(g_disp);
    const uint32_t status = wait_until(lk, deadline, [&] {
        if (port->packets.empty()) return false;
        packet = port->packets.front();
        port->packets.pop_front();
        return true;
    }, &id, 1);
    lk.unlock();
    if (status != kStatusSuccess) { ctx.r3.u64 = status; return; }
    if (key) guest_write_be32(key, packet.key);
    if (apc_context) guest_write_be32(apc_context, packet.apc_context);
    if (iosb) {
        guest_write_be32(iosb, packet.status);
        guest_write_be32(iosb + 4, packet.information);
    }
    ctx.r3.u64 = kStatusSuccess;
}

struct Impl {
    uint32_t ordinal;
    const char* name;
    PPCFunc* fn;
};

const Impl kImpls[] = {
    {0x000D, "ExCreateThread", &ExCreateThread},
    {0x002B, "InterlockedFlushSList", &InterlockedFlushSList},
    {0x002C, "InterlockedPopEntrySList", &InterlockedPopEntrySList},
    {0x0092, "KeResumeThread", &KeResumeThread},
    {0x005A, "KeDelayExecutionThread", &KeDelayExecutionThread},
    {0x005D, "KeEnableFpuExceptions", &KeEnableFpuExceptions},
    {0x00E2, "NtPulseEvent", &NtPulseEvent},
    {0x00FC, "NtSuspendThread", &NtSuspendThread},
    {0x0070, "KeInitializeEvent", &KeInitializeEvent},
    {0x0074, "KeInitializeSemaphore", &KeInitializeSemaphore},
    {0x0083, "KeQueryPerformanceFrequency", &KeQueryPerformanceFrequency},
    {0x008F, "KeResetEvent", &KeResetEvent},
    {0x0088, "KeReleaseSemaphore", &KeReleaseSemaphore},
    {0x009D, "KeSetEvent", &KeSetEvent},
    {0x00B0, "KeWaitForSingleObject", &KeWaitForSingleObject},
    {0x00AF, "KeWaitForMultipleObjects", &KeWaitForMultipleObjects},
    {0x00CD, "NtCancelTimer", &NtCancelTimer},
    {0x00CE, "NtClearEvent", &NtClearEvent},
    {0x00D1, "NtCreateEvent", &NtCreateEvent},
    {0x00D7, "NtCreateTimer", &NtCreateTimer},
    {0x00D3, "NtCreateIoCompletion", &NtCreateIoCompletion},
    {0x00E3, "NtQueueApcThread", &NtQueueApcThread},
    {0x00F4, "NtRemoveIoCompletion", &NtRemoveIoCompletion},
    {0x00F8, "NtSetIoCompletion", &NtSetIoCompletion},
    {0x00FB, "NtSignalAndWaitForSingleObjectEx", &NtSignalAndWaitForSingleObjectEx},
    {0x00D4, "NtCreateMutant", &NtCreateMutant},
    {0x00D5, "NtCreateSemaphore", &NtCreateSemaphore},
    {0x00F2, "NtReleaseMutant", &NtReleaseMutant},
    {0x00F3, "NtReleaseSemaphore", &NtReleaseSemaphore},
    {0x00F5, "NtResumeThread", &NtResumeThread},
    {0x00F6, "NtSetEvent", &NtSetEvent},
    {0x00FA, "NtSetTimerEx", &NtSetTimerEx},
    {0x00FD, "NtWaitForSingleObjectEx", &NtWaitForSingleObjectEx},
    {0x00FE, "NtWaitForMultipleObjectsEx", &NtWaitForMultipleObjectsEx},
    {0x0101, "NtYieldExecution", &NtYieldExecution},
    {0x0125, "RtlEnterCriticalSection", &RtlEnterCriticalSection},
    {0x012E, "RtlInitializeCriticalSection", &RtlInitializeCriticalSection},
    {0x012F, "RtlInitializeCriticalSectionAndSpinCount", &RtlInitializeCriticalSectionAndSpinCount},
    {0x0130, "RtlLeaveCriticalSection", &RtlLeaveCriticalSection},
    {0x0141, "RtlTryEnterCriticalSection", &RtlTryEnterCriticalSection},
    {0x0152, "KeTlsAlloc", &KeTlsAlloc},
    {0x0153, "KeTlsFree", &KeTlsFree},
    {0x0154, "KeTlsGetValue", &KeTlsGetValue},
    {0x0155, "KeTlsSetValue", &KeTlsSetValue},
};

}  // namespace

WaitStats wait_stats() {
    WaitStats stats;
#if RCOMP_RUNTIME_WAIT_STATS
    stats.ready_at_once = g_wait_stats[0].load(std::memory_order_relaxed);
    stats.immediate_timeouts = g_wait_stats[1].load(std::memory_order_relaxed);
    stats.blocked = g_wait_stats[2].load(std::memory_order_relaxed);
#endif
    return stats;
}

namespace {
std::atomic<uint64_t> g_main_guest_cpu_ns{0};  // sampled by the main guest thread itself
std::atomic<bool> g_main_guest_known{false};
pthread_t g_main_guest_pthread{};  // written once by note_main_guest_thread before g_main_guest_known
uint64_t g_main_guest_cpu_last_ns = 0;  // guarded by g_disp
thread_local uint64_t t_cpu_sample_ns = 0;  // runtime clock of the thread's last sample
}  // namespace

void note_main_guest_thread() {
    g_main_guest_pthread = pthread_self();
    g_main_guest_known.store(true, std::memory_order_release);
}

bool is_main_guest_host_thread() {
    return g_main_guest_known.load(std::memory_order_acquire) && pthread_equal(pthread_self(), g_main_guest_pthread);
}

void thread_cpu_sample_tick() {
    // Once per millisecond of the runtime's clock: the thread's own CPU time (one system call) into
    // its object (a worker) or the main-thread slot. Called at every import dispatch of the wait-
    // statistics builds, so a thread that calls no import for a while reports a stale, lower value.
    const uint64_t now = monotonic_ns();
    if (now - t_cpu_sample_ns < 1000000ull) return;
    t_cpu_sample_ns = now;
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return;
    const uint64_t cpu = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
    if (GuestThreadObject* self = t_self_object) self->cpu_ns.store(cpu, std::memory_order_relaxed);
    else if (is_main_guest_host_thread()) g_main_guest_cpu_ns.store(cpu, std::memory_order_relaxed);
    // (callback contexts on other host threads, e.g. the DPC worker, report nowhere)
}

size_t guest_thread_cpu_report(char* out, size_t capacity) {
    if (!out || capacity < 2) return 0;
    size_t used = 0;
    auto append = [&](const char* format, auto... args) {
        if (used + 1 >= capacity) return;
        const int n = std::snprintf(out + used, capacity - used, format, args...);
        if (n > 0) used += size_t(n) < capacity - used ? size_t(n) : capacity - used - 1;
    };
    std::lock_guard<std::mutex> lk(g_disp);
    if (g_main_guest_known.load(std::memory_order_relaxed)) {
        const uint64_t ns = g_main_guest_cpu_ns.load(std::memory_order_relaxed);
        if (g_main_guest_cpu_last_ns && ns >= g_main_guest_cpu_last_ns)
            append(" main:%llu", (unsigned long long)((ns - g_main_guest_cpu_last_ns) / 1000000ull));
        g_main_guest_cpu_last_ns = ns;
    }
    for (const auto& weak : g_thread_objects) {
        const std::shared_ptr<GuestThreadObject> obj = weak.lock();
        if (!obj || obj->exited) continue;
        const uint64_t ns = obj->cpu_ns.load(std::memory_order_relaxed);
        if (!ns) continue;
        if (obj->cpu_report_last_ns && ns >= obj->cpu_report_last_ns)
            append(" %u:%08X:%llu", obj->thread.thread_id, obj->thread.entry,
                   (unsigned long long)((ns - obj->cpu_report_last_ns) / 1000000ull));
        obj->cpu_report_last_ns = ns;
    }
    out[used] = 0;
    return used;
}

size_t guest_thread_wait_report(char* out, size_t capacity) {
#if RCOMP_RUNTIME_WAIT_STATS
    if (!out || capacity < 2) return 0;
    size_t used = 0;
    auto append = [&](const char* format, auto... args) {
        if (used + 1 >= capacity) return;
        const int n = std::snprintf(out + used, capacity - used, format, args...);
        if (n > 0) used += size_t(n) < capacity - used ? size_t(n) : capacity - used - 1;
    };
    std::lock_guard<std::mutex> lk(g_disp);
    if (g_main_guest_known.load(std::memory_order_relaxed)) {
        const uint64_t ns = g_main_guest_wait_ns.load(std::memory_order_relaxed);
        if (g_main_guest_wait_last_ns && ns >= g_main_guest_wait_last_ns)
            append(" main:%llu", (unsigned long long)((ns - g_main_guest_wait_last_ns) / 1000000ull));
        g_main_guest_wait_last_ns = ns;
    }
    for (const auto& weak : g_thread_objects) {
        const std::shared_ptr<GuestThreadObject> obj = weak.lock();
        if (!obj || obj->exited) continue;
        const uint64_t ns = obj->wait_ns.load(std::memory_order_relaxed);
        if (!ns && !obj->wait_report_last_ns) continue;
        if (obj->wait_report_last_ns <= ns)
            append(" %u:%llu", obj->thread.thread_id, (unsigned long long)((ns - obj->wait_report_last_ns) / 1000000ull));
        obj->wait_report_last_ns = ns;
    }
    out[used] = 0;
    return used;
#else
    (void)out;
    (void)capacity;
    return 0;
#endif
}

void notify_runtime_waiters() {
    std::lock_guard<std::mutex> lock(g_disp);
    signal_all();
}

void runtime_suspend_checkpoint() {
    GuestThreadObject* self = t_self_object;
    if (!self) return;
    bool stopped = false;
    {
        std::unique_lock<std::mutex> lk(g_disp);
        if (self->suspend_count == 0) return;
        wait_broadcast(lk, [self] { return g_stopping || self->suspend_count == 0; });
        stopped = g_stopping;
    }
    // Called from import dispatch, before the import runs: only POD frames
    // (generated code, the dispatcher) lie between here and the thread entry.
    if (stopped) exit_current_guest_thread(kStatusThreadTerminating);
}

Status reference_io_event(uint32_t handle, std::shared_ptr<HandleObject>* out) {
    if (!out) return Status::InvalidArgument;
    std::shared_ptr<HostEvent> event;
    Runtime* r = runtime();
    if (!r) return Status::NotInitialized;
    const Status status = r->handles.lookup_as<HostEvent>(handle, &event);
    if (status == Status::Ok) *out = std::move(event);
    return status;
}

Status reference_io_completion(uint32_t handle, std::shared_ptr<HandleObject>* out) {
    if (!out) return Status::InvalidArgument;
    Runtime* r = runtime();
    if (!r) return Status::NotInitialized;
    std::shared_ptr<HostIoCompletion> port;
    const Status status = r->handles.lookup_as<HostIoCompletion>(handle, &port);
    if (status == Status::Ok) *out = std::move(port);
    return status;
}

Status post_io_completion(const std::shared_ptr<HandleObject>& port, uint32_t key, uint32_t apc_context,
                          uint32_t status, uint32_t information) {
    if (!port || port->kind() != HandleKind::IoCompletion) return Status::InvalidArgument;
    auto typed = std::static_pointer_cast<HostIoCompletion>(port);
    std::lock_guard<std::mutex> lk(g_disp);
#if defined(__cpp_exceptions)
    try {
#endif
        typed->packets.push_back({key, apc_context, status, information});
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return Status::OutOfMemory;
    }
#endif
    signal_object(typed.get());
    return Status::Ok;
}

uint32_t file_apc_routine(const char* fn, uint32_t apc, uint32_t lr) {
    const uint32_t routine = apc & ~1u;
    if (!routine || (routine & 3))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s apc_routine=0x%08X lr=0x%08X", fn, apc, lr);
    if (!lookup_function(routine))
        rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET, "xboxkrnl.exe!%s APC routine 0x%08X has no recompiled function lr=0x%08X",
                    fn, routine, lr);
    return routine;
}

void queue_file_apc(const char* fn, uint32_t apc, uint32_t apc_context, uint32_t piosb, uint32_t status, uint32_t lr) {
    if (!apc || (status >> 30) == 3) return;  // NT_ERROR: the request failed, nothing is queued
    const uint32_t routine = file_apc_routine(fn, apc, lr);
    const uint32_t target = current_or_die(fn).thread_id;
    std::lock_guard<std::mutex> lk(g_disp);
#if defined(__cpp_exceptions)
    try {
#endif
        g_user_apcs[target].push_back({routine, apc_context, piosb, 0});
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xboxkrnl.exe!%s: no memory to queue the I/O completion APC", fn);
    }
#endif
    g_pending_apcs.fetch_add(1, std::memory_order_relaxed);
    signal_all();
}

void set_io_event(const std::shared_ptr<HandleObject>& event, bool signalled) {
    if (!event || event->kind() != HandleKind::Event)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "set_io_event requires a referenced Event object");
    auto typed = std::static_pointer_cast<HostEvent>(event);
    std::lock_guard<std::mutex> lock(g_disp);
    typed->signalled = signalled;
    if (signalled) signal_all();
}

// Called for every guest-thread return/ExTerminateThread, including the main
// thread. The guest thread id, not a reusable PCR address or host pthread id,
// is the ownership token. A later waiter obtains ownership and ABANDONED.
void runtime_thread_exited(uint32_t thread_id) {
    std::lock_guard<std::mutex> lk(g_disp);
    discard_user_apcs_locked(thread_id);
    for (auto it = g_mutants.begin(); it != g_mutants.end(); ) {
        auto mutant = it->lock();
        if (!mutant) { it = g_mutants.erase(it); continue; }
        if (mutant->owner == thread_id) {
            mutant->owner = 0; mutant->depth = 0; mutant->abandoned = true;
        }
        ++it;
    }
    signal_all();
}

void runtime_request_thread_quiesce() {
    if (!runtime()) return;
    std::lock_guard<std::mutex> lk(g_disp);
    g_stopping = true;
    signal_all();
}

bool runtime_thread_quiesce_requested() {
    std::lock_guard<std::mutex> lk(g_disp);
    return g_stopping;
}

void runtime_finish_thread_quiesce() {
    if (!runtime()) return;
    if (current_guest_thread())
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "runtime quiescence requested by a running guest thread");
    std::unique_lock<std::mutex> lk(g_disp);
    wait_broadcast(lk, [] { return g_workers == 0; });
}

Status open_thread_handle_for_body(uint32_t body, uint32_t* handle) {
    Runtime* r = runtime();
    if (!r || !handle || !body) return Status::InvalidArgument;
    std::shared_ptr<GuestThreadObject> found;
    {
        std::lock_guard<std::mutex> lk(g_disp);
        for (const auto& weak : g_thread_objects) {
            auto object = weak.lock();
            if (object && object->identity && thread_object_body(object->identity) == body) {
                found = std::move(object);
                break;
            }
        }
    }
    if (!found) return Status::NotFound;
    return r->handles.insert(found, handle);
}

void call_guest_routine_on_current_context(PPCContext& ctx, uint8_t* base, uint32_t routine, uint32_t a0,
                                           uint32_t a1, uint32_t a2, const char* what) {
    PPCFunc* fn = lookup_function(routine);
    if (!fn) rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET, "%s 0x%08X has no recompiled function", what, routine);
    // A call made from the import's call site: the callee builds its frame
    // below the caller's r1, as for any call. r1/r13/lr are restored as a
    // returning callee would leave them; volatile registers are clobbered.
    const uint64_t r1 = ctx.r1.u64, r13 = ctx.r13.u64, lr = ctx.lr;
    ctx.r3.u64 = a0;
    ctx.r4.u64 = a1;
    ctx.r5.u64 = a2;
    ctx.lr = kGuestLrSentinel;
    fn(ctx, base);
    ctx.r1.u64 = r1;
    ctx.r13.u64 = r13;
    ctx.lr = lr;
}

void runtime_quiesce_threads() {
    runtime_request_thread_quiesce();
    runtime_finish_thread_quiesce();
}

void runtime_threading_reset() {
    {
        std::lock_guard<std::mutex> lk(g_disp);
        g_stopping = false; g_mutants.clear(); g_thread_objects.clear();
        g_user_apcs.clear(); g_pending_apcs.store(0, std::memory_order_relaxed);
    }
    { std::lock_guard<std::mutex> lk(g_tls_mu); g_tls_used = 0; }
    thread_objects_reset();
}

Status register_xboxkrnl_threading_hle() {
    for (const Impl& i : kImpls) {
        uint32_t ord = 0;
        if (!export_ordinal(kModuleXboxkrnl, i.name, &ord) || ord != i.ordinal)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "xboxkrnl HLE %s: ordinal 0x%04X not in export table", i.name,
                        i.ordinal);
        Status s = register_import(kModuleXboxkrnl, i.ordinal, i.fn, i.name);
        if (s != Status::Ok) return s;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
