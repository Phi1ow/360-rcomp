// xboxkrnl batch 1 (src/hle_xboxkrnl_threads.cpp): time, TLS, critical
// sections, events, waits, thread creation. Several host threads each run a
// guest thread; the exports are called through the same __imp__ symbols
// generated code uses.
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__KeQueryPerformanceFrequency);
PPC_EXTERN_FUNC(__imp__KeTlsAlloc);
PPC_EXTERN_FUNC(__imp__KeTlsFree);
PPC_EXTERN_FUNC(__imp__KeTlsGetValue);
PPC_EXTERN_FUNC(__imp__KeTlsSetValue);
PPC_EXTERN_FUNC(__imp__RtlInitializeCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount);
PPC_EXTERN_FUNC(__imp__RtlEnterCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlTryEnterCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlLeaveCriticalSection);
PPC_EXTERN_FUNC(__imp__KeInitializeEvent);
PPC_EXTERN_FUNC(__imp__KeSetEvent);
PPC_EXTERN_FUNC(__imp__KeResetEvent);
PPC_EXTERN_FUNC(__imp__KeWaitForSingleObject);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtSetEvent);
PPC_EXTERN_FUNC(__imp__NtClearEvent);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__ExCreateThread);
PPC_EXTERN_FUNC(__imp__NtResumeThread);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__KeDelayExecutionThread);
PPC_EXTERN_FUNC(__imp__NtYieldExecution);

using namespace rcomp;
using namespace rcomp::rt;
using Clock = std::chrono::steady_clock;

namespace {

GuestMemory g_mem;
uint8_t* g_base;
uint32_t g_scratch;  // shared guest block
enum : uint32_t {
    kCs = 0x00,        // RTL_CRITICAL_SECTION (28 bytes)
    kCounter = 0x40,
    kEvent = 0x80,     // KEVENT
    kEvent2 = 0xA0,
    kTimeout = 0xC0,   // LARGE_INTEGER
    kHandle = 0xD0,
    kPrev = 0xD4,
    kThreadId = 0xD8,
    kFlag = 0xE0,
    kSeenR3 = 0xE4,
    kSeenR4 = 0xE8,
};
constexpr uint32_t kThreadFn = 0x82000100, kStartupFn = 0x82000200;

uint32_t rd32(uint32_t a) {
    uint32_t v = 0;
    guest_read_be32(a, &v);
    return v;
}
void wr32(uint32_t a, uint32_t v) { guest_write_be32(a, v); }
void set_timeout(int64_t v100ns) { guest_write_be64(g_scratch + kTimeout, (uint64_t)v100ns); }

// Runs `fn` as a guest thread on the calling host thread.
void as_guest(PPCFunc* fn, uint32_t r3 = 0) {
    alignas(64) PPCContext ctx;
    GuestThread t;
    if (create_guest_thread(runtime()->heap, {0x10000, 0, r3}, &ctx, &t) != Status::Ok) {
        CHECK(false);
        return;
    }
    uint32_t code;
    run_guest_thread(t, ctx, g_base, fn, &code);
    destroy_guest_thread(runtime()->heap, &t);
}

uint32_t call(PPCFunc* f, PPCContext& ctx, uint32_t r3 = 0, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0,
              uint32_t r7 = 0, uint32_t r8 = 0, uint32_t r9 = 0) {
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    ctx.r7.u64 = r7;
    ctx.r8.u64 = r8;
    ctx.r9.u64 = r9;
    f(ctx, g_base);
    return ctx.r3.u32;
}

// ---- TLS ----
std::atomic<uint32_t> g_slot{0};
void tls_other_thread(PPCContext& ctx, uint8_t*) {
    CHECK_EQ(call(__imp__KeTlsGetValue, ctx, g_slot), 0u);  // not the other thread's value
    CHECK_EQ(call(__imp__KeTlsSetValue, ctx, g_slot, 222), 1u);
    CHECK_EQ(call(__imp__KeTlsGetValue, ctx, g_slot), 222u);
}
void tls_main(PPCContext& ctx, uint8_t*) {
    g_slot = call(__imp__KeTlsAlloc, ctx);
    CHECK(g_slot < kTlsDynamicSlots);
    CHECK_EQ(call(__imp__KeTlsSetValue, ctx, g_slot, 111), 1u);
    std::thread b([] { as_guest(tls_other_thread); });
    b.join();
    CHECK_EQ(call(__imp__KeTlsGetValue, ctx, g_slot), 111u);
    CHECK_EQ(call(__imp__KeTlsSetValue, ctx, kTlsDynamicSlots, 1), 0u);  // out of range
    // Exhaustion returns TLS_OUT_OF_INDEXES.
    std::vector<uint32_t> got;
    for (;;) {
        uint32_t s = call(__imp__KeTlsAlloc, ctx);
        if (s == 0xFFFFFFFFu) break;
        got.push_back(s);
        if (got.size() > kTlsDynamicSlots) break;
    }
    CHECK_EQ(got.size() + 1, (size_t)kTlsDynamicSlots);
    for (uint32_t s : got) CHECK_EQ(call(__imp__KeTlsFree, ctx, s), 1u);
    CHECK_EQ(call(__imp__KeTlsFree, ctx, g_slot), 1u);
    CHECK_EQ(call(__imp__KeTlsFree, ctx, g_slot), 0u);  // already free
}

// ---- critical sections ----
constexpr int kCsThreads = 4, kCsIters = 20000;
void cs_worker(PPCContext& ctx, uint8_t*) {
    for (int i = 0; i < kCsIters; ++i) {
        call(__imp__RtlEnterCriticalSection, ctx, g_scratch + kCs);
        const uint32_t v = rd32(g_scratch + kCounter);  // non-atomic read-modify-write
        if ((i & 63) == 0) sched_yield();
        wr32(g_scratch + kCounter, v + 1);
        call(__imp__RtlLeaveCriticalSection, ctx, g_scratch + kCs);
    }
}
std::atomic<int> g_try_result{-1};
void cs_try_other(PPCContext& ctx, uint8_t*) { g_try_result = (int)call(__imp__RtlTryEnterCriticalSection, ctx, g_scratch + kCs); }
void cs_recursion(PPCContext& ctx, uint8_t*) {
    const uint32_t cs = g_scratch + kCs;
    call(__imp__RtlEnterCriticalSection, ctx, cs);
    call(__imp__RtlEnterCriticalSection, ctx, cs);
    CHECK_EQ(call(__imp__RtlTryEnterCriticalSection, ctx, cs), 1u);
    CHECK_EQ(rd32(cs + 0x14), 3u);  // recursion_count
    CHECK_EQ(rd32(cs + 0x10), 2u);  // lock_count
    CHECK_EQ(rd32(cs + 0x18), ctx.r13.u32);
    std::thread o([] { as_guest(cs_try_other); });
    o.join();
    CHECK_EQ(g_try_result.load(), 0);
    for (int i = 0; i < 3; ++i) call(__imp__RtlLeaveCriticalSection, ctx, cs);
    CHECK_EQ(rd32(cs + 0x10), 0xFFFFFFFFu);
    CHECK_EQ(rd32(cs + 0x18), 0u);
    std::thread o2([] { as_guest(cs_try_other); });
    o2.join();
    CHECK_EQ(g_try_result.load(), 1);  // free now; o2 took it and never left
}

// ---- KEVENT ----
std::atomic<int> g_woken{0};
void kevent_waiter(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = g_scratch + kEvent2;
    ctx.r7.u64 = 0;  // no timeout
    __imp__KeWaitForSingleObject(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, 0u);
    ++g_woken;
}
void kevent_main(PPCContext& ctx, uint8_t*) {
    const uint32_t ev = g_scratch + kEvent;
    call(__imp__KeInitializeEvent, ctx, ev, 0, 0);  // notification, not signalled
    CHECK_EQ((uint32_t)g_base[ev], 0u);
    set_timeout(0);
    CHECK_EQ(call(__imp__KeWaitForSingleObject, ctx, ev, 0, 0, 0, g_scratch + kTimeout), 0x102u);
    set_timeout(-200000);  // 20 ms relative
    auto t0 = Clock::now();
    CHECK_EQ(call(__imp__KeWaitForSingleObject, ctx, ev, 0, 0, 0, g_scratch + kTimeout), 0x102u);
    CHECK(Clock::now() - t0 >= std::chrono::milliseconds(19));
    CHECK_EQ(call(__imp__KeSetEvent, ctx, ev, 1, 0), 0u);  // previous state
    CHECK_EQ(call(__imp__KeSetEvent, ctx, ev, 1, 0), 1u);
    CHECK_EQ(call(__imp__KeWaitForSingleObject, ctx, ev, 0, 0, 0, 0), 0u);
    CHECK_EQ(call(__imp__KeWaitForSingleObject, ctx, ev, 0, 0, 0, 0), 0u);  // manual: stays set
    CHECK_EQ(call(__imp__KeResetEvent, ctx, ev), 1u);
    // Auto-reset: one signal wakes exactly one of two waiters.
    const uint32_t ev2 = g_scratch + kEvent2;
    call(__imp__KeInitializeEvent, ctx, ev2, 1, 0);
    std::thread w1([] { as_guest(kevent_waiter); }), w2([] { as_guest(kevent_waiter); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(g_woken.load(), 0);
    call(__imp__KeSetEvent, ctx, ev2, 1, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(g_woken.load(), 1);
    CHECK_EQ(rd32(ev2 + 4), 0u);  // consumed
    call(__imp__KeSetEvent, ctx, ev2, 1, 0);
    w1.join();
    w2.join();
    CHECK_EQ(g_woken.load(), 2);
}

// ---- NtCreateEvent ----
void ntevent_main(PPCContext& ctx, uint8_t*) {
    const uint32_t ph = g_scratch + kHandle;
    CHECK_EQ(call(__imp__NtCreateEvent, ctx, ph, 0, 1, 0), 0u);  // synchronization, not set
    const uint32_t h = rd32(ph);
    set_timeout(0);
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx, ctx, h, 0, 0, g_scratch + kTimeout), 0x102u);
    CHECK_EQ(call(__imp__NtSetEvent, ctx, h, g_scratch + kPrev), 0u);
    CHECK_EQ(rd32(g_scratch + kPrev), 0u);
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx, ctx, h, 0, 0, 0), 0u);
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx, ctx, h, 0, 0, g_scratch + kTimeout), 0x102u);  // consumed
    CHECK_EQ(call(__imp__NtSetEvent, ctx, h, 0), 0u);
    CHECK_EQ(call(__imp__NtClearEvent, ctx, h), 0u);
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx, ctx, h, 0, 0, g_scratch + kTimeout), 0x102u);
    CHECK_EQ(call(__imp__NtClose, ctx, h), 0u);
    CHECK_EQ(call(__imp__NtSetEvent, ctx, h, 0), 0xC0000008u);  // closed
    // Named events are not implemented: they trap (checked in main, see below).
}

// ---- ExCreateThread ----
void thread_fn(PPCContext& ctx, uint8_t*) {
    wr32(g_scratch + kFlag, ctx.r3.u32 + 1);
    ctx.r3.u64 = 0;
}
void startup_fn(PPCContext& ctx, uint8_t*) {
    wr32(g_scratch + kSeenR3, ctx.r3.u32);
    wr32(g_scratch + kSeenR4, ctx.r4.u32);
}
void thread_main(PPCContext& ctx, uint8_t*) {
    Runtime& r = *runtime();
    const uint32_t live0 = r.heap.stats().live_allocations;
    const uint32_t ph = g_scratch + kHandle, pid = g_scratch + kThreadId;
    wr32(g_scratch + kFlag, 0);
    CHECK_EQ(call(__imp__ExCreateThread, ctx, ph, 0, pid, 0, kThreadFn, 41, 1 /*suspended*/), 0u);
    const uint32_t h = rd32(ph);
    CHECK(rd32(pid) != 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(rd32(g_scratch + kFlag), 0u);  // still suspended
    set_timeout(0);
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx, ctx, h, 0, 0, g_scratch + kTimeout), 0x102u);
    CHECK_EQ(call(__imp__NtResumeThread, ctx, h, g_scratch + kPrev), 0u);
    CHECK_EQ(rd32(g_scratch + kPrev), 1u);
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx, ctx, h, 0, 0, 0), 0u);
    CHECK_EQ(rd32(g_scratch + kFlag), 42u);
    CHECK_EQ(call(__imp__NtClose, ctx, h), 0u);
    // XapiThreadStartup(StartAddress, StartContext).
    CHECK_EQ(call(__imp__ExCreateThread, ctx, ph, 0x20000, 0, kStartupFn, kThreadFn, 7, 0), 0u);
    const uint32_t h2 = rd32(ph);
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx, ctx, h2, 0, 0, 0), 0u);
    CHECK_EQ(rd32(g_scratch + kSeenR3), kThreadFn);
    CHECK_EQ(rd32(g_scratch + kSeenR4), 7u);
    CHECK_EQ(call(__imp__NtClose, ctx, h2), 0u);
    CHECK_EQ(r.heap.stats().live_allocations, live0);  // guest stacks released at exit
}

void delay_main(PPCContext& ctx, uint8_t*) {
    CHECK_EQ(call(__imp__KeQueryPerformanceFrequency, ctx), 50000000u);
    set_timeout(-300000);  // 30 ms
    auto t0 = Clock::now();
    CHECK_EQ(call(__imp__KeDelayExecutionThread, ctx, 1, 0, g_scratch + kTimeout), 0u);
    CHECK(Clock::now() - t0 >= std::chrono::milliseconds(29));
    CHECK_EQ(call(__imp__NtYieldExecution, ctx), 0u);
    // A zero interval (Sleep(0)) returns success at once, however often it is called.
    set_timeout(0);
    t0 = Clock::now();
    for (int i = 0; i < 2000; ++i) CHECK_EQ(call(__imp__KeDelayExecutionThread, ctx, 1, 0, g_scratch + kTimeout), 0u);
    CHECK(Clock::now() - t0 < std::chrono::seconds(2));
}

void named_event(PPCContext& ctx, uint8_t*) {
    wr32(g_scratch + 0x100, 0);
    wr32(g_scratch + 0x104, g_scratch + 0x110);  // ObjectName != NULL
    call(__imp__NtCreateEvent, ctx, g_scratch + kHandle, g_scratch + 0x100, 0, 0);
}

}  // namespace

int main() {
    if (g_mem.reserve() != MemStatus::Ok) return 2;
    g_base = g_mem.base();
    CHECK_ST(runtime_init(&g_mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &g_scratch), Status::Ok);
    const FuncEntry funcs[] = {{kThreadFn, thread_fn, "thread_fn"}, {kStartupFn, startup_fn, "startup_fn"}};
    CHECK(register_functions(funcs, 2));

    as_guest(tls_main);
    as_guest(delay_main);

    // Critical section under contention.
    {
        alignas(64) PPCContext ctx;
        GuestThread t;
        CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0, 0}, &ctx, &t), Status::Ok);
        call(__imp__RtlInitializeCriticalSectionAndSpinCount, ctx, g_scratch + kCs, 1000);
        CHECK_EQ(ctx.r3.u32, 0u);
        CHECK_EQ((uint32_t)g_base[g_scratch + kCs + 1], 4u);  // (1000 + 255) / 256
        destroy_guest_thread(runtime()->heap, &t);
        wr32(g_scratch + kCounter, 0);
        std::vector<std::thread> ts;
        for (int i = 0; i < kCsThreads; ++i) ts.emplace_back([] { as_guest(cs_worker); });
        for (auto& th : ts) th.join();
        CHECK_EQ(rd32(g_scratch + kCounter), (uint32_t)(kCsThreads * kCsIters));
        CHECK_EQ(rd32(g_scratch + kCs + 0x10), 0xFFFFFFFFu);
        CHECK_EQ(rd32(g_scratch + kCs + 0x18), 0u);
    }
    {
        alignas(64) PPCContext ctx;
        GuestThread t;
        CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0, 0}, &ctx, &t), Status::Ok);
        call(__imp__RtlInitializeCriticalSection, ctx, g_scratch + kCs);
        destroy_guest_thread(runtime()->heap, &t);
        as_guest(cs_recursion);
    }
    as_guest(kevent_main);
    as_guest(ntevent_main);
    as_guest(thread_main);
    {
        bool fatal = false;
        alignas(64) PPCContext ctx;
        GuestThread t;
        CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0, 0}, &ctx, &t), Status::Ok);
        CAPTURE_FATAL(named_event(ctx, g_base), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
        CHECK(g_fatal_msg.find("NtCreateEvent object_name") != std::string::npos);
        destroy_guest_thread(runtime()->heap, &t);
    }
    CHECK_EQ(runtime()->handles.live_count(), 0u);
    runtime_shutdown();
    clear_imports();
    return test_result("rt_threads");
}
