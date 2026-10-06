// NtPulseEvent, NtSuspendThread and KeEnableFpuExceptions over the real
// dispatcher, handles and ExCreateThread workers. TESTDOUBLE entries stand in
// only for AOT guest code and call the production thunks.
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

PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtSetEvent);
PPC_EXTERN_FUNC(__imp__NtClearEvent);
PPC_EXTERN_FUNC(__imp__NtPulseEvent);
PPC_EXTERN_FUNC(__imp__NtCreateSemaphore);
PPC_EXTERN_FUNC(__imp__NtReleaseSemaphore);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__NtWaitForMultipleObjectsEx);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__ExCreateThread);
PPC_EXTERN_FUNC(__imp__NtResumeThread);
PPC_EXTERN_FUNC(__imp__NtSuspendThread);
PPC_EXTERN_FUNC(__imp__NtYieldExecution);
PPC_EXTERN_FUNC(__imp__KeEnableFpuExceptions);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
using Clock = std::chrono::steady_clock;
GuestMemory mem;
uint32_t scratch;
// Guest scratch layout.
constexpr uint32_t poll_timeout = 0x100, waiter_timeout = 0x108, join_timeout = 0x118, array_off = 0x120,
                   prev_off = 0x130, self_prev_off = 0x138, out_off = 0x140;
constexpr uint32_t kTimedOut = 0x102, kInvalidHandle = 0xC0000008, kTypeMismatch = 0xC0000024,
                   kAccessViolation = 0xC0000005, kTerminating = 0xC000004B, kCountExceeded = 0xC000004A;
constexpr uint32_t kSpinner = 0x82000100, kWaiter = 0x82000200, kSelfSuspender = 0x82000300, kEmpty = 0x82000400;

std::atomic<uint32_t> entering{0}, completed{0}, timed{0}, failed{0};
std::atomic<uint64_t> spins{0};
std::atomic<bool> stop_spinning{false};
std::atomic<uint32_t> self_phase{0}, self_previous{~0u}, self_status{~0u};
std::atomic<uint32_t> wait_handle_value{0}, waitall{0};
uint32_t self_handle = 0;

uint32_t read(uint32_t address) { uint32_t value = 0; CHECK(guest_read_be32(address, &value)); return value; }
void write(uint32_t address, uint32_t value) { CHECK(guest_write_be32(address, value)); }
uint32_t call(PPCFunc* fn, PPCContext& c, uint32_t a = 0, uint32_t b = 0, uint32_t d = 0, uint32_t e = 0,
              uint32_t f = 0, uint32_t g = 0, uint32_t h = 0) {
    c.r3.u64 = a; c.r4.u64 = b; c.r5.u64 = d; c.r6.u64 = e; c.r7.u64 = f; c.r8.u64 = g; c.r9.u64 = h;
    fn(c, mem.base());
    return c.r3.u32;
}
void as_guest(PPCFunc* fn) {
    GuestThread t; PPCContext c; uint32_t code;
    CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0, 0}, &c, &t), Status::Ok);
    CHECK_ST(run_guest_thread(t, c, mem.base(), fn, &code), Status::Ok);
    CHECK_ST(destroy_guest_thread(runtime()->heap, &t), Status::Ok);
}
uint32_t poll(PPCContext& c, uint32_t handle) {
    return call(__imp__NtWaitForSingleObjectEx, c, handle, 0, 0, scratch + poll_timeout);
}
uint32_t join(PPCContext& c, uint32_t thread) {  // bounded: 2 s
    return call(__imp__NtWaitForSingleObjectEx, c, thread, 0, 0, scratch + join_timeout);
}
uint32_t create_event(PPCContext& c, bool notification, bool state) {
    CHECK_EQ(call(__imp__NtCreateEvent, c, scratch + out_off, 0, notification ? 0 : 1, state ? 1 : 0), 0u);
    return read(scratch + out_off);
}
uint32_t create_thread(PPCContext& c, uint32_t entry, bool suspended) {
    CHECK_EQ(call(__imp__ExCreateThread, c, scratch + out_off, 0x10000, 0, 0, entry, 0, suspended ? 1 : 0), 0u);
    return read(scratch + out_off);
}
void close(PPCContext& c, uint32_t handle) { CHECK_EQ(call(__imp__NtClose, c, handle), 0u); }
template <class Pred>
bool eventually(Pred pred, int ms = 2000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (!pred() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return pred();
}
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(40)); }
uint32_t suspend(PPCContext& c, uint32_t thread) {
    write(scratch + prev_off, 0xDEADBEEF);
    return call(__imp__NtSuspendThread, c, thread, scratch + prev_off);
}
uint32_t resume(PPCContext& c, uint32_t thread) {
    write(scratch + prev_off, 0xDEADBEEF);
    return call(__imp__NtResumeThread, c, thread, scratch + prev_off);
}
void set_waiter_timeout_ms(uint32_t ms) { guest_write_be64(scratch + waiter_timeout, uint64_t(-int64_t(ms) * 10000)); }
void reset_counters() { entering = 0; completed = 0; timed = 0; failed = 0; }

// ---- guest code stand-ins ---------------------------------------------------

// Waits (timeout at waiter_timeout) on wait_handle_value, or with WaitAll on
// the two handles at array_off when `waitall`, and counts the outcome.
void TESTDOUBLE_waiter(PPCContext& c, uint8_t*) {
    ++entering;
    const uint32_t status = waitall.load()
        ? call(__imp__NtWaitForMultipleObjectsEx, c, 2, scratch + array_off, 0, 0, 0, scratch + waiter_timeout)
        : call(__imp__NtWaitForSingleObjectEx, c, wait_handle_value.load(), 0, 0, scratch + waiter_timeout);
    if (status == 0) ++completed; else if (status == kTimedOut) ++timed; else ++failed;
    c.r3.u64 = 0;
}
// Calls the kernel in a loop (each call is a suspension checkpoint).
void TESTDOUBLE_spinner(PPCContext& c, uint8_t*) {
    while (!stop_spinning.load()) {
        call(__imp__NtYieldExecution, c);
        ++spins;
    }
    c.r3.u64 = 0;
}
void TESTDOUBLE_self_suspender(PPCContext& c, uint8_t*) {
    self_phase = 1;
    self_status = call(__imp__NtSuspendThread, c, self_handle, scratch + self_prev_off);
    self_previous = read(scratch + self_prev_off);
    self_phase = 2;
    c.r3.u64 = 0;
}
void TESTDOUBLE_empty(PPCContext& c, uint8_t*) { c.r3.u64 = 0; }

// Starts n standalone guest threads running TESTDOUBLE_waiter and returns once
// all of them have entered their wait.
std::vector<std::thread> start_waiters(int n) {
    std::vector<std::thread> workers;
    for (int i = 0; i < n; ++i) workers.emplace_back([] { as_guest(TESTDOUBLE_waiter); });
    CHECK(eventually([n] { return entering.load() == uint32_t(n); }));
    settle();
    return workers;
}

// ---- NtPulseEvent -----------------------------------------------------------

void TESTDOUBLE_pulse(PPCContext& c, uint8_t*) {
    CHECK_EQ(call(__imp__NtPulseEvent, c, 0x12345678, 0), kInvalidHandle);
    CHECK_EQ(call(__imp__NtCreateSemaphore, c, scratch + out_off, 0, 0, 1), 0u);
    const uint32_t semaphore = read(scratch + out_off);
    CHECK_EQ(call(__imp__NtPulseEvent, c, semaphore, 0), kTypeMismatch);

    // No waiter: the event ends non-signalled; a thread that starts waiting
    // after the pulse is not released by it (it times out).
    const uint32_t manual = create_event(c, true, false);
    CHECK_EQ(call(__imp__NtPulseEvent, c, manual, scratch + prev_off), 0u);
    CHECK_EQ(read(scratch + prev_off), 0u);
    CHECK_EQ(poll(c, manual), kTimedOut);
    reset_counters(); waitall = 0; wait_handle_value = manual; set_waiter_timeout_ms(100);
    { std::thread late([] { as_guest(TESTDOUBLE_waiter); }); late.join(); }
    CHECK_EQ(timed.load(), 1u); CHECK_EQ(completed.load(), 0u);

    // Signalled before the pulse: PreviousState 1, non-signalled afterwards.
    CHECK_EQ(call(__imp__NtSetEvent, c, manual, 0), 0u);
    CHECK_EQ(call(__imp__NtPulseEvent, c, manual, scratch + prev_off), 0u);
    CHECK_EQ(read(scratch + prev_off), 1u);
    CHECK_EQ(poll(c, manual), kTimedOut);
    // An unwritable PreviousState is refused before any change.
    CHECK_EQ(call(__imp__NtSetEvent, c, manual, 0), 0u);
    CHECK_EQ(call(__imp__NtPulseEvent, c, manual, scratch + prev_off + 2), kAccessViolation);
    CHECK_EQ(poll(c, manual), 0u);
    CHECK_EQ(call(__imp__NtClearEvent, c, manual), 0u);

    // Notification event: every thread waiting at the pulse is released.
    reset_counters(); set_waiter_timeout_ms(3000);
    {
        auto workers = start_waiters(3);
        CHECK_EQ(completed.load(), 0u);
        CHECK_EQ(call(__imp__NtPulseEvent, c, manual, 0), 0u);
        for (auto& t : workers) t.join();
    }
    CHECK_EQ(completed.load(), 3u); CHECK_EQ(timed.load(), 0u); CHECK_EQ(failed.load(), 0u);
    CHECK_EQ(poll(c, manual), kTimedOut);

    // Synchronization event: exactly one of the three waiting threads.
    const uint32_t automatic = create_event(c, false, false);
    reset_counters(); wait_handle_value = automatic;
    {
        auto workers = start_waiters(3);
        CHECK_EQ(call(__imp__NtPulseEvent, c, automatic, scratch + prev_off), 0u);
        CHECK_EQ(read(scratch + prev_off), 0u);
        CHECK(eventually([] { return completed.load() == 1u; }));
        settle();
        CHECK_EQ(completed.load(), 1u);
        CHECK_EQ(poll(c, automatic), kTimedOut);  // reset, not left signalled
        // The two others still wait: each set releases one of them.
        CHECK_EQ(call(__imp__NtSetEvent, c, automatic, 0), 0u);
        CHECK(eventually([] { return completed.load() == 2u; }));
        CHECK_EQ(call(__imp__NtSetEvent, c, automatic, 0), 0u);
        for (auto& t : workers) t.join();
    }
    CHECK_EQ(completed.load(), 3u); CHECK_EQ(timed.load(), 0u); CHECK_EQ(failed.load(), 0u);

    // WaitAll(event, empty semaphore): the pulse cannot satisfy it and the
    // event does not stay signalled for a later semaphore release.
    write(scratch + array_off, manual); write(scratch + array_off + 4, semaphore);
    reset_counters(); waitall = 1; set_waiter_timeout_ms(300);
    {
        auto workers = start_waiters(1);
        CHECK_EQ(call(__imp__NtPulseEvent, c, manual, 0), 0u);
        CHECK_EQ(call(__imp__NtReleaseSemaphore, c, semaphore, 1, 0), 0u);
        for (auto& t : workers) t.join();
    }
    waitall = 0;
    CHECK_EQ(timed.load(), 1u); CHECK_EQ(completed.load(), 0u);
    CHECK_EQ(poll(c, semaphore), 0u);  // the semaphore token was not consumed
    close(c, manual); close(c, automatic); close(c, semaphore);
}

// ---- NtSuspendThread --------------------------------------------------------

void TESTDOUBLE_suspend(PPCContext& c, uint8_t*) {
    CHECK_EQ(call(__imp__NtSuspendThread, c, 0x12345678, 0), kInvalidHandle);
    const uint32_t event = create_event(c, false, false);
    CHECK_EQ(call(__imp__NtSuspendThread, c, event, 0), kTypeMismatch);

    // A running worker stops at its next kernel call; counts nest with
    // NtResumeThread.
    stop_spinning = false; spins = 0;
    const uint32_t spinner = create_thread(c, kSpinner, false);
    CHECK(eventually([] { return spins.load() > 10; }));
    CHECK_EQ(suspend(c, spinner), 0u); CHECK_EQ(read(scratch + prev_off), 0u);
    settle();
    uint64_t frozen = spins.load();
    settle();
    CHECK_EQ(spins.load(), frozen);
    CHECK_EQ(suspend(c, spinner), 0u); CHECK_EQ(read(scratch + prev_off), 1u);
    CHECK_EQ(call(__imp__NtSuspendThread, c, spinner, scratch + prev_off + 2), kAccessViolation);
    CHECK_EQ(resume(c, spinner), 0u); CHECK_EQ(read(scratch + prev_off), 2u);  // AV changed nothing
    settle();
    CHECK_EQ(spins.load(), frozen);  // still suspended once
    CHECK_EQ(resume(c, spinner), 0u); CHECK_EQ(read(scratch + prev_off), 1u);
    CHECK(eventually([frozen] { return spins.load() > frozen + 10; }));
    CHECK_EQ(resume(c, spinner), 0u); CHECK_EQ(read(scratch + prev_off), 0u);
    stop_spinning = true;
    CHECK_EQ(join(c, spinner), 0u);
    // An exited thread cannot be suspended; the output is left alone.
    CHECK_EQ(suspend(c, spinner), kTerminating); CHECK_EQ(read(scratch + prev_off), 0xDEADBEEFu);
    close(c, spinner);

    // CREATE_SUSPENDED shares the count; MAXIMUM_SUSPEND_COUNT is 127.
    const uint32_t idle = create_thread(c, kEmpty, true);
    for (uint32_t i = 1; i < 127; ++i) {
        CHECK_EQ(suspend(c, idle), 0u); CHECK_EQ(read(scratch + prev_off), i);
    }
    CHECK_EQ(suspend(c, idle), kCountExceeded); CHECK_EQ(read(scratch + prev_off), 0xDEADBEEFu);
    for (uint32_t i = 127; i > 1; --i) {
        CHECK_EQ(resume(c, idle), 0u); CHECK_EQ(read(scratch + prev_off), i);
    }
    CHECK_EQ(poll(c, idle), kTimedOut);  // count 1: never started
    CHECK_EQ(resume(c, idle), 0u); CHECK_EQ(read(scratch + prev_off), 1u);
    CHECK_EQ(join(c, idle), 0u);
    close(c, idle);

    // A worker suspended while blocked consumes nothing (sets, pulses) until
    // resumed.
    reset_counters(); waitall = 0; wait_handle_value = event; set_waiter_timeout_ms(3000);
    const uint32_t blocked = create_thread(c, kWaiter, false);
    CHECK(eventually([] { return entering.load() == 1u; }));
    settle();
    CHECK_EQ(suspend(c, blocked), 0u);
    CHECK_EQ(call(__imp__NtSetEvent, c, event, 0), 0u);
    CHECK_EQ(poll(c, event), 0u);  // this thread got the signal, not the suspended waiter
    CHECK_EQ(call(__imp__NtSetEvent, c, event, 0), 0u);
    settle();
    CHECK_EQ(completed.load(), 0u);
    CHECK_EQ(call(__imp__NtPulseEvent, c, event, scratch + prev_off), 0u);
    CHECK_EQ(read(scratch + prev_off), 1u);
    CHECK_EQ(resume(c, blocked), 0u); CHECK_EQ(read(scratch + prev_off), 1u);
    settle();
    CHECK_EQ(completed.load(), 0u);  // the pulse did not release it, the event is reset
    CHECK_EQ(call(__imp__NtSetEvent, c, event, 0), 0u);
    CHECK(eventually([] { return completed.load() == 1u; }));
    CHECK_EQ(join(c, blocked), 0u);
    close(c, blocked);

    // A timeout that expires while suspended is reported after the resume.
    reset_counters(); set_waiter_timeout_ms(50);
    const uint32_t timing = create_thread(c, kWaiter, true);
    CHECK_EQ(resume(c, timing), 0u);
    CHECK(eventually([] { return entering.load() == 1u; }));
    CHECK_EQ(suspend(c, timing), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_EQ(timed.load(), 0u); CHECK_EQ(completed.load(), 0u);
    CHECK_EQ(resume(c, timing), 0u);
    CHECK(eventually([] { return timed.load() == 1u; }));
    CHECK_EQ(join(c, timing), 0u);
    close(c, timing);

    // A thread suspending itself blocks inside the call until resumed.
    self_phase = 0;
    self_handle = create_thread(c, kSelfSuspender, true);
    CHECK_EQ(resume(c, self_handle), 0u);
    CHECK(eventually([] { return self_phase.load() == 1u; }));
    settle();
    CHECK_EQ(self_phase.load(), 1u);
    CHECK_EQ(resume(c, self_handle), 0u); CHECK_EQ(read(scratch + prev_off), 1u);
    CHECK(eventually([] { return self_phase.load() == 2u; }));
    CHECK_EQ(self_status.load(), 0u); CHECK_EQ(self_previous.load(), 0u);
    CHECK_EQ(join(c, self_handle), 0u);
    close(c, self_handle);
    close(c, event);
}

// ---- KeEnableFpuExceptions --------------------------------------------------

void TESTDOUBLE_fpu(PPCContext& c, uint8_t*) {
    GuestThread* self = current_guest_thread();
    CHECK(self != nullptr && !self->fpu_exceptions_enabled);
    CHECK_EQ(call(__imp__KeEnableFpuExceptions, c, 1), 0u);
    CHECK(self->fpu_exceptions_enabled);
    CHECK_EQ(call(__imp__KeEnableFpuExceptions, c, 1), 1u);
    CHECK_EQ(call(__imp__KeEnableFpuExceptions, c, 0x100), 1u);  // BOOLEAN: low byte only
    CHECK(!self->fpu_exceptions_enabled);
    CHECK_EQ(call(__imp__KeEnableFpuExceptions, c, 0), 0u);
}
}  // namespace

int main() {
    CHECK(mem.reserve() == MemStatus::Ok); CHECK_ST(runtime_init(&mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &scratch), Status::Ok);
    guest_write_be64(scratch + poll_timeout, 0);
    guest_write_be64(scratch + join_timeout, uint64_t(-20000000));
    const FuncEntry entries[] = {{kSpinner, TESTDOUBLE_spinner, "TESTDOUBLE_spinner"},
                                 {kWaiter, TESTDOUBLE_waiter, "TESTDOUBLE_waiter"},
                                 {kSelfSuspender, TESTDOUBLE_self_suspender, "TESTDOUBLE_self_suspender"},
                                 {kEmpty, TESTDOUBLE_empty, "TESTDOUBLE_empty"}};
    CHECK(register_functions(entries, 4));
    as_guest(TESTDOUBLE_pulse);
    as_guest(TESTDOUBLE_suspend);
    as_guest(TESTDOUBLE_fpu);
    {
        PPCContext c{}; bool fatal = false;
        CAPTURE_FATAL(call(__imp__KeEnableFpuExceptions, c, 1), fatal);  // no guest thread
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_INTERNAL);
        CAPTURE_FATAL(call(__imp__NtSuspendThread, c, 0xFFFFFFFEu, 0), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    }
    CHECK_EQ(runtime()->handles.live_count(), 0u);

    // Quiescence ends a worker held at its suspension checkpoint.
    stop_spinning = false; spins = 0;
    struct Start {
        static void run(PPCContext& c, uint8_t*) {
            write(scratch + out_off + 8, create_thread(c, kSpinner, false));
        }
    };
    as_guest(Start::run);
    const uint32_t parked = read(scratch + out_off + 8);
    CHECK(eventually([] { return spins.load() > 10; }));
    {
        PPCContext c{};
        CHECK_EQ(suspend(c, parked), 0u);
    }
    settle();
    const uint64_t frozen = spins.load();
    runtime_quiesce_threads();
    CHECK_EQ(spins.load(), frozen);
    {
        PPCContext c{};  // the worker has exited (quiescence awaited it); its handle is still open
        close(c, parked);
    }
    CHECK_EQ(runtime()->handles.live_count(), 0u);
    runtime_shutdown(); clear_functions(); clear_imports();
    return test_result("rt_suspend_pulse");
}
