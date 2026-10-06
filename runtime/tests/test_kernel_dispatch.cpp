// User APCs (NtQueueApcThread + alertable waits), NtSignalAndWaitForSingleObjectEx
// and I/O completion ports over the real dispatcher. TESTDOUBLE entries stand in
// only for AOT guest routines.
#include <atomic>
#include <chrono>
#include <thread>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__NtQueueApcThread);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtSetEvent);
PPC_EXTERN_FUNC(__imp__NtCreateSemaphore);
PPC_EXTERN_FUNC(__imp__NtCreateMutant);
PPC_EXTERN_FUNC(__imp__NtReleaseMutant);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__NtWaitForMultipleObjectsEx);
PPC_EXTERN_FUNC(__imp__KeDelayExecutionThread);
PPC_EXTERN_FUNC(__imp__NtSignalAndWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__NtCreateIoCompletion);
PPC_EXTERN_FUNC(__imp__NtSetIoCompletion);
PPC_EXTERN_FUNC(__imp__NtRemoveIoCompletion);
PPC_EXTERN_FUNC(__imp__ExCreateThread);
PPC_EXTERN_FUNC(__imp__NtResumeThread);
PPC_EXTERN_FUNC(__imp__NtClose);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory mem;
uint32_t scratch;
constexpr uint32_t kOut = 0x00, kZero = 0x10, kDelay = 0x18, kKey = 0x20, kApc = 0x24, kIosb = 0x28, kArray = 0x40,
                   kAttrs = 0x60, kName = 0x70;
constexpr uint32_t kTimedOut = 0x102, kUserApc = 0xC0, kAccessViolation = 0xC0000005, kInvalidHandle = 0xC0000008,
                   kTypeMismatch = 0xC0000024, kUnsuccessful = 0xC0000001, kSemaphoreLimit = 0xC0000047,
                   kMutantNotOwned = 0xC0000046;
constexpr uint32_t kApcRoutine = 0x82000400, kWorkerEntry = 0x82000500, kMissingRoutine = 0x82000600;
constexpr uint32_t kCurrentThread = 0xFFFFFFFEu;

struct ApcCall { uint32_t context, argument1, argument2, thread; };
std::atomic<int> g_apc_calls{0};
ApcCall g_last{};
uint32_t g_worker_event = 0;

uint32_t read(uint32_t address) { uint32_t value = 0; CHECK(guest_read_be32(address, &value)); return value; }
uint32_t call(PPCFunc* fn, PPCContext& c, uint32_t a = 0, uint32_t b = 0, uint32_t d = 0, uint32_t e = 0,
              uint32_t f = 0, uint32_t g = 0, uint32_t h = 0) {
    c.r3.u64 = a; c.r4.u64 = b; c.r5.u64 = d; c.r6.u64 = e; c.r7.u64 = f; c.r8.u64 = g; c.r9.u64 = h;
    fn(c, mem.base());
    return c.r3.u32;
}

void TESTDOUBLE_apc(PPCContext& c, uint8_t*) {
    GuestThread* t = current_guest_thread();
    g_last = {c.r3.u32, c.r4.u32, c.r5.u32, t ? t->thread_id : 0u};
    g_apc_calls.fetch_add(1);
    c.r1.u64 = 0xBAD;  // a callee that leaves r1 changed must not leak it to the waiter
    c.r3.u64 = 0x5555;
}

// Worker: an infinite user-mode alertable wait on an event nobody sets; exits with its status.
void TESTDOUBLE_worker(PPCContext& c, uint8_t*) {
    PPCContext w = c;
    c.r3.u64 = call(__imp__NtWaitForSingleObjectEx, w, g_worker_event, 1, 1, 0);
}

uint32_t event(PPCContext& c, uint32_t type, bool state) {
    CHECK_EQ(call(__imp__NtCreateEvent, c, scratch + kOut, 0, type, state ? 1 : 0), 0u);
    return read(scratch + kOut);
}
uint32_t wait(PPCContext& c, uint32_t handle, uint32_t mode, uint32_t alertable) {
    return call(__imp__NtWaitForSingleObjectEx, c, handle, mode, alertable, scratch + kZero);
}

void TESTDOUBLE_main(PPCContext& guest, uint8_t*) {
    PPCContext c = guest;
    const uint32_t r1 = c.r1.u32;
    const uint32_t self = current_guest_thread()->thread_id;

    // ---- user APCs on the calling thread ---------------------------------
    const uint32_t idle = event(c, 1, false);
    CHECK_EQ(call(__imp__NtQueueApcThread, c, kCurrentThread, kApcRoutine, 1, 2, 3), 0u);
    CHECK_EQ(wait(c, idle, 1, 0), kTimedOut);  // not alertable
    CHECK_EQ(wait(c, idle, 0, 1), kTimedOut);  // alertable but kernel mode
    CHECK_EQ(g_apc_calls.load(), 0);
    CHECK_EQ(wait(c, idle, 1, 1), kUserApc);
    CHECK_EQ(g_apc_calls.load(), 1);
    CHECK_EQ(g_last.context, 1u); CHECK_EQ(g_last.argument1, 2u); CHECK_EQ(g_last.argument2, 3u);
    CHECK_EQ(g_last.thread, self);
    CHECK_EQ(c.r1.u32, r1);  // the waiter's stack pointer is preserved
    CHECK_EQ(wait(c, idle, 1, 1), kTimedOut);  // delivered once

    // Two queued APCs run in order in one delivery.
    CHECK_EQ(call(__imp__NtQueueApcThread, c, kCurrentThread, kApcRoutine, 10, 0, 0), 0u);
    CHECK_EQ(call(__imp__NtQueueApcThread, c, kCurrentThread, kApcRoutine, 11, 0, 0), 0u);
    CHECK_EQ(wait(c, idle, 1, 1), kUserApc);
    CHECK_EQ(g_apc_calls.load(), 3);
    CHECK_EQ(g_last.context, 11u);

    // A signalled object wins over a pending APC (NT order); the APC stays queued.
    const uint32_t ready = event(c, 0, true);
    CHECK_EQ(call(__imp__NtQueueApcThread, c, kCurrentThread, kApcRoutine, 20, 0, 0), 0u);
    CHECK_EQ(wait(c, ready, 1, 1), 0u);
    CHECK_EQ(g_apc_calls.load(), 3);
    // ...and an alertable sleep delivers it at once.
    CHECK(guest_write_be64(scratch + kDelay, uint64_t(-int64_t(10'000'000))));  // 1 s
    const auto before = std::chrono::steady_clock::now();
    CHECK_EQ(call(__imp__KeDelayExecutionThread, c, 1, 1, scratch + kDelay), kUserApc);
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500));
    CHECK_EQ(g_apc_calls.load(), 4);
    CHECK_EQ(g_last.context, 20u);
    // Multiple-object alertable wait.
    CHECK_EQ(call(__imp__NtQueueApcThread, c, kCurrentThread, kApcRoutine, 21, 0, 0), 0u);
    CHECK(guest_write_be32(scratch + kArray, idle));
    CHECK_EQ(call(__imp__NtWaitForMultipleObjectsEx, c, 1, scratch + kArray, 1, 1, 1, scratch + kZero), kUserApc);
    CHECK_EQ(g_apc_calls.load(), 5);

    // Errors: wrong handle kind, unknown handle, routine without AOT function.
    CHECK_EQ(call(__imp__NtQueueApcThread, c, idle, kApcRoutine, 0, 0, 0), kTypeMismatch);
    CHECK_EQ(call(__imp__NtQueueApcThread, c, 0xF0000F00u, kApcRoutine, 0, 0, 0), kInvalidHandle);
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__NtQueueApcThread, c, kCurrentThread, kMissingRoutine, 0, 0, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_INDIRECT_TARGET);

    // ---- NtSignalAndWaitForSingleObjectEx --------------------------------
    const uint32_t a = event(c, 1, false), b = event(c, 0, true);
    CHECK_EQ(call(__imp__NtSignalAndWaitForSingleObjectEx, c, a, b, 1, 0, scratch + kZero), 0u);
    CHECK_EQ(wait(c, a, 1, 0), 0u);          // A was set (and this wait consumed it)
    CHECK_EQ(wait(c, a, 1, 0), kTimedOut);
    // The wait half times out but the signal half happened.
    CHECK_EQ(call(__imp__NtSignalAndWaitForSingleObjectEx, c, a, idle, 1, 0, scratch + kZero), kTimedOut);
    CHECK_EQ(wait(c, a, 1, 0), 0u);
    // Semaphore at its limit / mutant not owned: nothing waits.
    CHECK_EQ(call(__imp__NtCreateSemaphore, c, scratch + kOut, 0, 1, 1), 0u);
    const uint32_t full = read(scratch + kOut);
    CHECK_EQ(call(__imp__NtSignalAndWaitForSingleObjectEx, c, full, idle, 1, 0, 0), kSemaphoreLimit);
    CHECK_EQ(call(__imp__NtCreateSemaphore, c, scratch + kOut, 0, 0, 1), 0u);
    const uint32_t empty = read(scratch + kOut);
    CHECK_EQ(call(__imp__NtSignalAndWaitForSingleObjectEx, c, empty, empty, 1, 0, scratch + kZero), 0u);  // release then take
    CHECK_EQ(wait(c, empty, 1, 0), kTimedOut);
    CHECK_EQ(call(__imp__NtCreateMutant, c, scratch + kOut, 0, 0), 0u);
    const uint32_t unowned = read(scratch + kOut);
    CHECK_EQ(call(__imp__NtSignalAndWaitForSingleObjectEx, c, unowned, idle, 1, 0, 0), kMutantNotOwned);
    CHECK_EQ(call(__imp__NtCreateMutant, c, scratch + kOut, 0, 1), 0u);
    const uint32_t owned = read(scratch + kOut);
    CHECK_EQ(call(__imp__NtSignalAndWaitForSingleObjectEx, c, owned, b, 1, 0, scratch + kZero), 0u);
    CHECK_EQ(call(__imp__NtReleaseMutant, c, owned, 0), kMutantNotOwned);  // the signal released it
    // Not a signalable object; unknown handles.
    CHECK_EQ(call(__imp__NtSignalAndWaitForSingleObjectEx, c, idle, 0xF0000F00u, 1, 0, 0), kInvalidHandle);
    CHECK_EQ(call(__imp__NtSignalAndWaitForSingleObjectEx, c, 0xF0000F00u, idle, 1, 0, 0), kInvalidHandle);
    // Alertable form delivers a pending APC.
    CHECK_EQ(call(__imp__NtQueueApcThread, c, kCurrentThread, kApcRoutine, 30, 0, 0), 0u);
    CHECK_EQ(call(__imp__NtSignalAndWaitForSingleObjectEx, c, a, idle, 1, 1, scratch + kZero), kUserApc);
    CHECK_EQ(g_last.context, 30u);

    // ---- I/O completion ports ----------------------------------------------
    CHECK_EQ(call(__imp__NtCreateIoCompletion, c, scratch + kOut, 0, 0, 2), 0u);
    const uint32_t port = read(scratch + kOut);
    CHECK(guest_write_be32(scratch + kKey, 0xAAAA));
    CHECK_EQ(call(__imp__NtRemoveIoCompletion, c, port, scratch + kKey, scratch + kApc, scratch + kIosb, scratch + kZero), kTimedOut);
    CHECK_EQ(read(scratch + kKey), 0xAAAAu);
    CHECK_EQ(wait(c, port, 1, 0), kTimedOut);  // empty port: not signalled
    CHECK_EQ(call(__imp__NtSetIoCompletion, c, port, 0x11, 0x22, 0x33, 0x44), 0u);
    CHECK_EQ(call(__imp__NtSetIoCompletion, c, port, 0x55, 0x66, 0x77, 0x88), 0u);
    CHECK_EQ(wait(c, port, 1, 0), 0u);  // signalled while it holds packets, nothing consumed
    CHECK_EQ(call(__imp__NtRemoveIoCompletion, c, port, scratch + kKey, scratch + kApc, scratch + kIosb, scratch + kZero), 0u);
    CHECK_EQ(read(scratch + kKey), 0x11u); CHECK_EQ(read(scratch + kApc), 0x22u);
    CHECK_EQ(read(scratch + kIosb), 0x33u); CHECK_EQ(read(scratch + kIosb + 4), 0x44u);
    CHECK_EQ(call(__imp__NtRemoveIoCompletion, c, port, scratch + kKey, scratch + kApc, scratch + kIosb, 0), 0u);
    CHECK_EQ(read(scratch + kKey), 0x55u); CHECK_EQ(read(scratch + kIosb + 4), 0x88u);
    // A blocked remover is woken by a packet posted from another thread.
    std::thread poster([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        PPCContext p{};
        CHECK_EQ(call(__imp__NtSetIoCompletion, p, port, 0x99, 0, 0, 7), 0u);
    });
    CHECK_EQ(call(__imp__NtRemoveIoCompletion, c, port, scratch + kKey, 0, 0, 0), 0u);
    CHECK_EQ(read(scratch + kKey), 0x99u);
    poster.join();
    // Errors.
    CHECK_EQ(call(__imp__NtSetIoCompletion, c, idle, 0, 0, 0, 0), kTypeMismatch);
    CHECK_EQ(call(__imp__NtSetIoCompletion, c, 0xF0000F00u, 0, 0, 0, 0), kInvalidHandle);
    CHECK_EQ(call(__imp__NtRemoveIoCompletion, c, port, scratch + 2, 0, 0, scratch + kZero), kAccessViolation);
    CHECK_EQ(call(__imp__NtCreateIoCompletion, c, 0, 0, 0, 0), kAccessViolation);
    CHECK(guest_write_be32(scratch + kAttrs, 0)); CHECK(guest_write_be32(scratch + kAttrs + 4, scratch + kName));
    CHECK(guest_write_be32(scratch + kAttrs + 8, 0));
    CAPTURE_FATAL(call(__imp__NtCreateIoCompletion, c, scratch + kOut, 0, scratch + kAttrs, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_EQ(call(__imp__NtClose, c, port), 0u);

    for (uint32_t h : {idle, ready, a, b, full, empty, unowned, owned}) CHECK_EQ(call(__imp__NtClose, c, h), 0u);
    guest.r3.u64 = 0;
}
}  // namespace

int main() {
    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK_ST(runtime_init(&mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &scratch), Status::Ok);
    CHECK(guest_write_be64(scratch + kZero, 0));
    const FuncEntry functions[] = {{kApcRoutine, TESTDOUBLE_apc, "TESTDOUBLE_apc"},
                                   {kWorkerEntry, TESTDOUBLE_worker, "TESTDOUBLE_worker"}};
    CHECK(register_functions(functions, 2));

    {
        GuestThread t; PPCContext c{}; uint32_t code = ~0u;
        CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0, 0}, &c, &t), Status::Ok);
        CHECK_ST(run_guest_thread(t, c, mem.base(), TESTDOUBLE_main, &code), Status::Ok);
        CHECK_EQ(code, 0u);
        CHECK_ST(destroy_guest_thread(runtime()->heap, &t), Status::Ok);
    }

    // APC queued from another thread to a worker blocked in an infinite alertable wait.
    {
        PPCContext c{};
        CHECK_EQ(call(__imp__NtCreateEvent, c, scratch + kOut, 0, 0, 0), 0u);
        g_worker_event = read(scratch + kOut);
        CHECK_EQ(call(__imp__ExCreateThread, c, scratch + kOut, 0x10000, 0, 0, kWorkerEntry, 0, 0), 0u);
        const uint32_t worker = read(scratch + kOut);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int before = g_apc_calls.load();
        CHECK_EQ(call(__imp__NtQueueApcThread, c, worker, kApcRoutine, 40, 41, 42), 0u);
        CHECK(guest_write_be64(scratch + kDelay, uint64_t(-int64_t(50'000'000))));  // 5 s bound
        CHECK_EQ(call(__imp__NtWaitForSingleObjectEx, c, worker, 0, 0, scratch + kDelay), 0u);
        CHECK_EQ(g_apc_calls.load(), before + 1);
        CHECK_EQ(g_last.context, 40u); CHECK_EQ(g_last.argument2, 42u);
        // The worker's wait ended with STATUS_USER_APC (its exit code); an exited thread takes no APC.
        CHECK_EQ(call(__imp__NtQueueApcThread, c, worker, kApcRoutine, 0, 0, 0), kUnsuccessful);
        CHECK_EQ(call(__imp__NtClose, c, worker), 0u);
        CHECK_EQ(call(__imp__NtClose, c, g_worker_event), 0u);
    }

    CHECK_ST(runtime()->heap.free(scratch), Status::Ok);
    runtime_shutdown();
    clear_imports();
    clear_functions();
    mem.release();
    return test_result("rt_kernel_dispatch");
}
