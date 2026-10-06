#include "rcomp/runtime/guest_thread.h"

#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include <array>
#include <atomic>
#include <cstdio>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_heap.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/process_lifecycle.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/wait_stats.h"
#include "host_fiber.h"

namespace rcomp::rt {

void runtime_thread_exited(uint32_t thread_id);

namespace {

struct RunState {
    jmp_buf jb;
    bool active = false;
    bool exit_requested = false;
    void* profiling_context = nullptr;
    uint32_t exit_code = 0;
    GuestThread* thread = nullptr;
    HostFiberRun fibers;  // guest fiber host contexts of this run (src/host_fiber.h)
};
// Per host thread run state. pthread keys instead of thread_local: the PS5
// payload SDK compiles thread_local as emulated TLS (__emutls_get_address),
// which neither its stubs nor the title provide (platform/ps5/README.md).
pthread_key_t g_run_key;
pthread_once_t g_run_once = PTHREAD_ONCE_INIT;
std::atomic<bool> g_run_key_ready{false};
void run_key_create() {
    if (pthread_key_create(&g_run_key, [](void* p) { delete static_cast<RunState*>(p); }) != 0)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "pthread_key_create failed for guest thread state");
    g_run_key_ready.store(true, std::memory_order_release);
}
// pthread_once is a call; once the key exists a load is enough (current_guest_thread() sits on
// the critical-section imports, which the guest calls hundreds of thousands of times a second).
inline void ensure_run_key() {
    if (__builtin_expect(!g_run_key_ready.load(std::memory_order_acquire), 0))
        pthread_once(&g_run_once, run_key_create);
}
RunState& run_state() {
    ensure_run_key();
    auto* st = static_cast<RunState*>(pthread_getspecific(g_run_key));
    if (!st) {
        st = new RunState();
        if (pthread_setspecific(g_run_key, st) != 0)
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "pthread_setspecific failed for guest thread state");
    }
    return *st;
}
RunState* existing_run_state() {
    ensure_run_key();
    return static_cast<RunState*>(pthread_getspecific(g_run_key));
}
#define t_run (run_state())

void store_be32(uint8_t* base, uint32_t addr, uint32_t v) {
    v = __builtin_bswap32(v);
    memcpy(base + addr, &v, 4);
}
void store_be64(uint8_t* base, uint32_t addr, uint64_t v) {
    v = __builtin_bswap64(v);
    memcpy(base + addr, &v, 8);
}

}  // namespace

Status create_guest_thread(GuestHeap& heap, const GuestThreadInit& init, PPCContext* ctx,
                           GuestThread* out) {
    if (!ctx || !out) return Status::InvalidArgument;
    if (init.stack_size == 0 || init.stack_size % kStackGuardSize) return Status::InvalidArgument;
    if (init.stack_size > 0x10000000u) return Status::InvalidArgument;  // 256 MiB sanity cap
    GuestMemory* mem = heap.memory();
    if (!mem) return Status::NotInitialized;

    GuestThread t;
    Runtime* owner = runtime();
    if (owner && &owner->heap != &heap) owner = nullptr;
    uint32_t tls_size = kTlsBytes;
    if (owner) {
        owner->static_tls_frozen.store(true);
        const auto& tls = owner->static_tls;
        if (tls.present && tls.slot_count)
            tls_size = tls.data_size + tls.slot_count * 4;
    }
    t.tls_size = tls_size;
    uint32_t total = init.stack_size + kStackGuardSize;
    Status s = heap.alloc(total, kStackGuardSize, true, &t.alloc_base);
    if (s != Status::Ok) return s;
    s = heap.alloc(kPcrSize, 16, true, &t.pcr);
    if (s != Status::Ok) {
        const Status cleanup=destroy_guest_thread(heap,&t);
        if(cleanup!=Status::Ok) rcomp_fatal(RCOMP_FATAL_PLATFORM,"create_guest_thread rollback: %s",status_name(cleanup));
        return s;
    }
    s = heap.alloc(tls_size, 16, true, &t.tls);
    if (s != Status::Ok) {
        const Status cleanup=destroy_guest_thread(heap,&t);
        if(cleanup!=Status::Ok) rcomp_fatal(RCOMP_FATAL_PLATFORM,"create_guest_thread rollback: %s",status_name(cleanup));
        return s;
    }
    s = heap.set_guard(t.alloc_base, kStackGuardSize, true);
    if (s != Status::Ok) {
        const Status cleanup=destroy_guest_thread(heap,&t);
        if(cleanup!=Status::Ok) rcomp_fatal(RCOMP_FATAL_PLATFORM,"create_guest_thread rollback: %s",status_name(cleanup));
        return s;
    }
    t.stack_limit = t.alloc_base + kStackGuardSize;
    t.stack_base = t.alloc_base + total;
    t.initial_r1 = t.stack_base - kStackAbiReserve;
    t.entry = init.entry;
    static std::atomic<uint32_t> next_thread_id{1};
    t.thread_id = next_thread_id.fetch_add(1);
    if (!t.thread_id) t.thread_id = next_thread_id.fetch_add(1);  // reserve zero
    if (owner) {
        s = create_thread_object_identity(heap, t.thread_id, init.entry, &t.identity);
        if (s != Status::Ok) {
            const Status cleanup = destroy_guest_thread(heap, &t);
            if (cleanup != Status::Ok)
                rcomp_fatal(RCOMP_FATAL_PLATFORM, "create_guest_thread object rollback: %s",
                            status_name(cleanup));
            return s;
        }
        // KTHREAD stack fields. StackAllocBase follows xapi's own convention
        // for the stacks it creates (CreateFiber stores the high end returned
        // by MmCreateKernelStack as both its alloc base and its base): the
        // value is only copied by ConvertThreadToFiber and handed back to
        // KeSetCurrentStackPointers (runtime/docs/THREAD_OBJECTS.md ("Guest fibers")).
        thread_object_set_stack(t.identity, t.stack_base, t.stack_base, t.stack_limit);
    }

    uint8_t* base = mem->base();
    if (owner && !owner->static_tls_template.empty())
        memcpy(base + t.tls, owner->static_tls_template.data(), owner->static_tls_template.size());
    store_be32(base, t.initial_r1, 0);  // back chain terminator
    store_be32(base, t.pcr + kPcrTlsPtr, t.tls);
    store_be64(base, t.pcr + kPcrSelfPtr, t.pcr);
    store_be32(base, t.pcr + kPcrStackBase, t.stack_base);
    store_be32(base, t.pcr + kPcrStackEnd, t.stack_limit);
    store_be32(base, t.pcr + kPcrCurrentThread, thread_object_body(t.identity));

    *ctx = PPCContext{};
    ctx->r1.u64 = t.initial_r1;
    ctx->r13.u64 = t.pcr;
    ctx->r3.u64 = init.r3;
    ctx->lr = kGuestLrSentinel;
    *out = t;
    return Status::Ok;
}

Status destroy_guest_thread(GuestHeap& heap, GuestThread* t) {
    if (!t || (!t->alloc_base && !t->pcr && !t->tls)) return Status::InvalidArgument;
    const RunState* state=existing_run_state();
    if (state && state->active && state->thread == t) return Status::InvalidArgument;
    Status result=Status::Ok;
    auto release=[&](uint32_t& address) {
        if(!address)return;
        Status s=heap.free(address);
        if(s==Status::Ok)address=0;
        else if(result==Status::Ok)result=s;
    };
    release(t->tls);release(t->pcr);release(t->alloc_base);
    if(result==Status::Ok)*t=GuestThread{};
    // Failed blocks remain owned and retryable, even if other frees succeeded.
    return result;
}

extern "C" void rcomp_register_thread_slot(uint32_t thread_id) __attribute__((weak));
extern "C" void rcomp_register_thread_ctx(uint32_t thread_id, void* ctx) __attribute__((weak));
extern "C" void rcomp_unregister_thread_ctx(uint32_t thread_id, void* ctx) __attribute__((weak));
namespace {
void unregister_guest_profile(RunState& state) {
    if (state.profiling_context && rcomp_unregister_thread_ctx)
        rcomp_unregister_thread_ctx(state.thread->thread_id, state.profiling_context);
    state.profiling_context = nullptr;
}
void retire_thread_context(GuestThread& t, uint32_t exit_code) {
    thread_object_mark_exited(t.identity, exit_code);
    runtime_thread_exited(t.thread_id);
    title_lifecycle_thread_exited(t.thread_id);
}

Status run_guest_context(GuestThread& t, PPCContext& ctx, uint8_t* base, PPCFunc* entry,
                         uint32_t* exit_code, bool retire_on_return) {
    if (!entry || !base || !exit_code) return Status::InvalidArgument;
    RunState& state = run_state();
    if (state.active) return Status::InvalidArgument;  // not reentrant
    state.active = true;
    state.exit_requested = false;
    state.thread = &t;
    state.profiling_context = &ctx;
    state.exit_code = 0;
    state.fibers = HostFiberRun{};
    state.fibers.thread_id = t.thread_id;
    if (rcomp_register_thread_slot) rcomp_register_thread_slot(t.thread_id);
    if (rcomp_register_thread_ctx) rcomp_register_thread_ctx(t.thread_id, &ctx);
    if (setjmp(state.jb) == 0) {
        entry(ctx, base);
        state.exit_code = ctx.r3.u32;
    }
    // A callback return leaves its hosting context alive. Explicit guest
    // termination retains the same thread/object/lifecycle effects as entry
    // termination, including abandonment of any owned handle mutants.
    // Guest fibers: a run always ends on its own (primary) host context.
    host_fiber_run_ended(state.fibers);
    unregister_guest_profile(state);
    *exit_code = state.exit_code;
    if (retire_on_return || state.exit_requested)
        retire_thread_context(t, state.exit_code);
    state.active = false;
    state.thread = nullptr;
    return Status::Ok;
}
}  // namespace

#if defined(__PROSPERO__)
extern "C" int scePthreadSetaffinity(pthread_t thread, uint64_t mask);
#endif
// EXPERIMENT (phase 50): guest threads stay off the CPUs reserved for the
// Xenos command processor (CPU 10) and both of its candidate SMT siblings.
// The placement experiment overrides the mask with RCOMP_CPU_MASK_GUEST (0x...).
static uint64_t guest_host_cpu_mask() {
    static const uint64_t mask = [] {
        uint64_t m = 0x1FFFull & ~((1ull << 2) | (1ull << 10) | (1ull << 11));
        if (const char* value = getenv("RCOMP_CPU_MASK_GUEST")) {
            if (*value) m = strtoull(value, nullptr, 0);
        }
        return m;
    }();
    return mask;
}
// RCOMP_CPU_MASK_GUEST_MAIN (0x..., optional): the title's main guest thread alone gets this mask instead
// of RCOMP_CPU_MASK_GUEST. In GTA IV's city the main thread paces the frame at 80-92 % of a core (4 October
// 2026, arm 192) while about ten workers share the guest CPUs; a mask of its own keeps it off a core whose
// other hyperthread runs another busy worker. 0 (unset) = the common mask.
static uint64_t guest_main_host_cpu_mask() {
    static const uint64_t mask = [] {
        uint64_t m = 0;
        if (const char* value = getenv("RCOMP_CPU_MASK_GUEST_MAIN")) {
            if (*value) m = strtoull(value, nullptr, 0);
        }
        return m;
    }();
    return mask;
}

void apply_host_service_affinity(const char* role) {
#if defined(__PROSPERO__)
    // A thread inherits the affinity of the thread that created it. The runtime's service threads (DPC
    // worker, XMA decoder, XAudio frame clock, network worker) are created lazily by guest threads: with
    // RCOMP_CPU_MASK_GUEST_MAIN the main thread's single CPU would be theirs too, and a guest spin wait
    // on the main thread then starves the service that would end it (arm 196, 4 October 2026: fps 0,
    // one core busy, no waits). They take the common guest mask (what they inherited before that knob),
    // or RCOMP_CPU_MASK_SERVICE when set.
    static const uint64_t mask = [] {
        uint64_t m = guest_host_cpu_mask();
        if (const char* value = getenv("RCOMP_CPU_MASK_SERVICE")) {
            if (*value) m = strtoull(value, nullptr, 0);
        }
        return m;
    }();
    const int rc = scePthreadSetaffinity(pthread_self(), mask);
    std::fprintf(stderr, "RCOMP-PLACEMENT role=SERVICE_%s mask=0x%llX rc=%d\n", role ? role : "?", (unsigned long long)mask, rc);
#else
    (void)role;
#endif
}

// RCOMP_HWTHREAD_MAP (optional): "k:0xMASK,..." (or '+' separated) for the six Xbox 360 hardware threads k = 0..5. A guest
// thread whose creation flags or KeSetAffinityThread named hardware threads (not the default 0x3F) runs on
// the union of the host masks of those threads: the title's own placement of its heavy threads on the
// console's three cores, carried over to the PS5's cores. Unmapped threads keep the common guest mask.
static uint64_t hardware_thread_host_mask(uint32_t hardware_mask) {
    static const std::array<uint64_t, 6> map = [] {
        std::array<uint64_t, 6> m{};
        if (const char* value = getenv("RCOMP_HWTHREAD_MAP")) {
            const char* p = value;
            while (*p) {
                char* end = nullptr;
                const long k = strtol(p, &end, 10);
                if (end == p || *end != ':') break;
                const uint64_t host = strtoull(end + 1, &end, 0);
                if (k >= 0 && k < 6) m[size_t(k)] = host;
                if (*end != ',' && *end != '+') break;  // '+' inside RCOMP_M6_TUNING_ENV, where ',' separates variables
                p = end + 1;
            }
        }
        return m;
    }();
    if (hardware_mask == 0 || hardware_mask == kDefaultThreadAffinity) return 0;
    uint64_t host = 0;
    for (uint32_t k = 0; k < 6; ++k)
        if (hardware_mask & (1u << k)) host |= map[k];
    return host;
}

Status run_guest_thread(GuestThread& t, PPCContext& ctx, uint8_t* base, PPCFunc* entry,
                        uint32_t* exit_code) {
#if defined(__PROSPERO__)
    {
        const bool main = is_main_guest_host_thread();
        const uint32_t hardware = thread_object_affinity(t.identity);
        const uint64_t own = main ? guest_main_host_cpu_mask() : 0;
        const uint64_t mapped = own ? 0 : hardware_thread_host_mask(hardware);
        const uint64_t mask = own ? own : mapped ? mapped : guest_host_cpu_mask();
        const int rc = scePthreadSetaffinity(pthread_self(), mask);
        std::fprintf(stderr, "RCOMP-PLACEMENT role=%s tid=%u entry=0x%08X hw=0x%02X mask=0x%llX rc=%d\n",
                     main ? "GUEST_MAIN" : mapped ? "GUEST_MAPPED" : "GUEST", t.thread_id, t.entry, hardware,
                     (unsigned long long)mask, rc);
    }
#endif
    return run_guest_context(t, ctx, base, entry, exit_code, true);
}

Status run_guest_callback(GuestThread& t, PPCContext& ctx, uint8_t* base, PPCFunc* entry,
                          uint32_t* exit_code) {
    return run_guest_context(t, ctx, base, entry, exit_code, false);
}

Status retire_guest_thread(GuestThread& t, uint32_t exit_code) {
    if (!t.thread_id) return Status::InvalidArgument;
    const RunState* state = existing_run_state();
    if (state && state->active && state->thread == &t) return Status::InvalidArgument;
    if (!thread_object_exited(t.identity)) retire_thread_context(t, exit_code);
    return Status::Ok;
}

void exit_current_guest_thread(uint32_t exit_code) {
    if (!t_run.active)
        rcomp_fatal(RCOMP_FATAL_INTERNAL,
                    "guest thread exit (code=0x%08X) requested outside run_guest_thread", exit_code);
    // On a guest fiber's host stack, first switch back to the thread's own
    // context (host_fiber.cpp), which calls this function again from there.
    (void)host_fiber_exit_to_primary(exit_code);
    t_run.exit_requested = true;
    t_run.exit_code = exit_code;
    longjmp(t_run.jb, 1);
}

void abandon_current_guest_thread_after_fatal() {
    if (!t_run.active)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "abandon_current_guest_thread_after_fatal without a running guest thread");
    host_fiber_run_ended(run_state().fibers);
    unregister_guest_profile(run_state());
    thread_object_mark_exited(t_run.thread->identity, t_run.exit_code);
    runtime_thread_exited(t_run.thread->thread_id);
    title_lifecycle_thread_exited(t_run.thread->thread_id);
    t_run.active = false;
    t_run.thread = nullptr;
}

GuestThread* current_guest_thread() {
    const RunState* state=existing_run_state();
    return state && state->active ? state->thread : nullptr;
}

HostFiberRun* current_host_fiber_run() {
    RunState* state = existing_run_state();
    return state && state->active ? &state->fibers : nullptr;
}

void set_current_run_context(PPCContext* ctx) {
    RunState* state = existing_run_state();
    if (!state || !state->active || !ctx) return;
    // Re-registration replaces this host thread's sampler entry.
    if (rcomp_register_thread_ctx) rcomp_register_thread_ctx(state->thread->thread_id, ctx);
    state->profiling_context = ctx;
}

}  // namespace rcomp::rt
