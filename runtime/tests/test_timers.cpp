// NT waitable timers (NtCreateTimer, NtSetTimerEx, NtCancelTimer) over the real
// dispatcher, handles and guest memory. Expected states and statuses are the
// Windows ntdll observations of timer_windows_oracle.json (an NT reference, not
// Xbox 360 or PS5 proof). TESTDOUBLE entries stand in only for AOT callers.
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

PPC_EXTERN_FUNC(__imp__NtCreateTimer);
PPC_EXTERN_FUNC(__imp__NtSetTimerEx);
PPC_EXTERN_FUNC(__imp__NtCancelTimer);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtSetEvent);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__NtWaitForMultipleObjectsEx);
PPC_EXTERN_FUNC(__imp__NtClose);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory mem;
uint32_t scratch;
constexpr uint32_t kOut = 0x00, kDue = 0x10, kTimeout = 0x20, kPrevious = 0x30, kArray = 0x40, kAttrs = 0x60;
constexpr uint32_t kTimedOut = 0x102, kAccessViolation = 0xC0000005, kInvalidHandle = 0xC0000008,
                   kTypeMismatch = 0xC0000024;
constexpr int64_t kMs = 10000;  // 100 ns units per millisecond

uint32_t read(uint32_t address) { uint32_t value = 0; CHECK(guest_read_be32(address, &value)); return value; }
uint8_t byte(uint32_t address) { return mem.base()[address]; }
uint32_t call(PPCFunc* fn, PPCContext& c, uint32_t a = 0, uint32_t b = 0, uint32_t d = 0, uint32_t e = 0,
              uint32_t f = 0, uint32_t g = 0, uint32_t h = 0, uint32_t i = 0) {
    c.r3.u64 = a; c.r4.u64 = b; c.r5.u64 = d; c.r6.u64 = e;
    c.r7.u64 = f; c.r8.u64 = g; c.r9.u64 = h; c.r10.u64 = i;
    fn(c, mem.base());
    return c.r3.u32;
}
void as_guest(PPCFunc* fn) {
    GuestThread t; PPCContext c; uint32_t code;
    CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0, 0}, &c, &t), Status::Ok);
    CHECK_ST(run_guest_thread(t, c, mem.base(), fn, &code), Status::Ok);
    CHECK_ST(destroy_guest_thread(runtime()->heap, &t), Status::Ok);
}
uint32_t create(PPCContext& c, uint32_t type) {
    CHECK_EQ(call(__imp__NtCreateTimer, c, scratch + kOut, 0, type), 0u);
    return read(scratch + kOut);
}
// NtSetTimerEx with a relative (negative) or absolute due time; previous state into kPrevious.
uint32_t set(PPCContext& c, uint32_t timer, int64_t due, int32_t period_ms = 0, bool previous = true) {
    CHECK(guest_write_be64(scratch + kDue, uint64_t(due)));
    mem.base()[scratch + kPrevious] = 0xAA;
    return call(__imp__NtSetTimerEx, c, timer, scratch + kDue, 0, 1, 0, 0, uint32_t(period_ms),
                previous ? scratch + kPrevious : 0);
}
uint32_t wait(PPCContext& c, uint32_t handle, int64_t ms) {
    CHECK(guest_write_be64(scratch + kTimeout, uint64_t(-ms * kMs)));
    return call(__imp__NtWaitForSingleObjectEx, c, handle, 1, 0, scratch + kTimeout);
}
uint32_t cancel(PPCContext& c, uint32_t timer) {
    mem.base()[scratch + kPrevious] = 0xAA;
    return call(__imp__NtCancelTimer, c, timer, scratch + kPrevious);
}
void close(PPCContext& c, uint32_t handle) { CHECK_EQ(call(__imp__NtClose, c, handle), 0u); }

// The oracle's rows, in its order.
void TESTDOUBLE_notification(PPCContext& c, uint8_t*) {
    const uint32_t t = create(c, 0);
    CHECK_EQ(wait(c, t, 0), kTimedOut);                          // created not signalled
    CHECK_EQ(set(c, t, -10 * kMs), 0u);
    CHECK_EQ(byte(scratch + kPrevious), 0u);
    CHECK_EQ(wait(c, t, 0), kTimedOut);                          // not before its due time
    CHECK_EQ(wait(c, t, 500), 0u);                               // signalled at due
    CHECK_EQ(wait(c, t, 0), 0u);                                 // a notification timer stays signalled
    CHECK_EQ(set(c, t, -1000 * kMs), 0u);
    CHECK_EQ(byte(scratch + kPrevious), 1u);                     // previous state reported...
    CHECK_EQ(wait(c, t, 0), kTimedOut);                          // ...and reset by the set
    CHECK_EQ(cancel(c, t), 0u);
    CHECK_EQ(byte(scratch + kPrevious), 0u);
    CHECK_EQ(wait(c, t, 50), kTimedOut);                         // cancelled: never fires
    CHECK_EQ(set(c, t, -1 * kMs), 0u);
    CHECK_EQ(wait(c, t, 500), 0u);
    CHECK_EQ(cancel(c, t), 0u);
    CHECK_EQ(byte(scratch + kPrevious), 1u);                     // cancel keeps the signal state
    CHECK_EQ(wait(c, t, 0), 0u);
    CHECK_EQ(cancel(c, t), 0u);
    CHECK_EQ(byte(scratch + kPrevious), 1u);                     // cancelling an idle timer
    CHECK_EQ(set(c, t, 1), 0u);                                  // absolute 1601: already past
    CHECK_EQ(wait(c, t, 50), 0u);
    CHECK_EQ(set(c, t, -10 * kMs, 0, false), 0u);                // PreviousState is optional
    CHECK_EQ(call(__imp__NtCancelTimer, c, t, 0), 0u);           // so is CurrentState
    close(c, t);
}

void TESTDOUBLE_synchronization(PPCContext& c, uint8_t*) {
    const uint32_t t = create(c, 1);
    CHECK_EQ(set(c, t, -10 * kMs), 0u);
    CHECK_EQ(wait(c, t, 500), 0u);
    CHECK_EQ(wait(c, t, 0), kTimedOut);                          // a satisfied wait resets it
    CHECK_EQ(set(c, t, -10 * kMs, 20), 0u);
    CHECK_EQ(wait(c, t, 500), 0u);                               // periodic: fires again
    CHECK_EQ(wait(c, t, 500), 0u);
    CHECK_EQ(cancel(c, t), 0u);
    CHECK_EQ(wait(c, t, 100), kTimedOut);
    // Expiries missed while nobody waits coalesce into one signal state.
    CHECK_EQ(set(c, t, -1 * kMs, 5), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_EQ(wait(c, t, 0), 0u);
    CHECK_EQ(wait(c, t, 0), kTimedOut);
    CHECK_EQ(call(__imp__NtCancelTimer, c, t, 0), 0u);
    close(c, t);
}

// A blocked wait wakes at the due time with nobody signalling: well before the
// dispatcher's one-second sleep slice, and in a multiple wait with its index.
void TESTDOUBLE_wake(PPCContext& c, uint8_t*) {
    const uint32_t t = create(c, 1);
    CHECK_EQ(call(__imp__NtCreateEvent, c, scratch + kOut, 0, 0, 0), 0u);
    const uint32_t ev = read(scratch + kOut);
    CHECK_EQ(set(c, t, -30 * kMs), 0u);
    auto start = std::chrono::steady_clock::now();
    CHECK_EQ(wait(c, t, 3000), 0u);
    auto took = std::chrono::steady_clock::now() - start;
    CHECK(took >= std::chrono::milliseconds(20));
    CHECK(took < std::chrono::milliseconds(700));
    CHECK(guest_write_be32(scratch + kArray, ev));
    CHECK(guest_write_be32(scratch + kArray + 4, t));
    CHECK_EQ(set(c, t, -30 * kMs), 0u);
    CHECK(guest_write_be64(scratch + kTimeout, uint64_t(-3000 * kMs)));
    start = std::chrono::steady_clock::now();
    CHECK_EQ(call(__imp__NtWaitForMultipleObjectsEx, c, 2, scratch + kArray, 1, 1, 0, scratch + kTimeout), 1u);
    took = std::chrono::steady_clock::now() - start;
    CHECK(took < std::chrono::milliseconds(700));
    // A timer set again while a thread waits moves that thread's wake time.
    CHECK_EQ(set(c, t, -2000 * kMs), 0u);
    std::atomic<uint32_t> status{~0u};
    std::thread waiter([&] {
        GuestThread g; PPCContext w; uint32_t code;
        CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0, 0}, &w, &g), Status::Ok);
        static std::atomic<uint32_t>* out;
        static uint32_t timer;
        out = &status;
        timer = t;
        CHECK_ST(run_guest_thread(g, w, mem.base(), [](PPCContext& x, uint8_t*) {
            CHECK(guest_write_be64(scratch + kTimeout + 8, uint64_t(-3000 * kMs)));
            x.r3.u64 = timer; x.r4.u64 = 1; x.r5.u64 = 0; x.r6.u64 = scratch + kTimeout + 8;
            __imp__NtWaitForSingleObjectEx(x, mem.base());
            *out = x.r3.u32;
        }, &code), Status::Ok);
        CHECK_ST(destroy_guest_thread(runtime()->heap, &g), Status::Ok);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    start = std::chrono::steady_clock::now();
    CHECK_EQ(set(c, t, -20 * kMs), 0u);
    waiter.join();
    CHECK_EQ(status.load(), 0u);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(700));
    close(c, ev);
    close(c, t);
}

void TESTDOUBLE_errors(PPCContext& c, uint8_t*) {
    const uint32_t t = create(c, 0);
    CHECK_EQ(call(__imp__NtCreateEvent, c, scratch + kOut, 0, 0, 0), 0u);
    const uint32_t ev = read(scratch + kOut);
    CHECK_EQ(call(__imp__NtCreateTimer, c, 0, 0, 0), kAccessViolation);
    CHECK_EQ(set(c, ev, -10 * kMs), kTypeMismatch);
    CHECK_EQ(cancel(c, ev), kTypeMismatch);
    CHECK_EQ(byte(scratch + kPrevious), 0xAAu);
    CHECK_EQ(set(c, 0x12345678, -10 * kMs), kInvalidHandle);
    CHECK_EQ(call(__imp__NtSetTimerEx, c, t, 0, 0, 1, 0, 0, 0, 0), kAccessViolation);  // DueTime required
    // An unwritable PreviousState fails before any change: the timer stays as it was.
    CHECK_EQ(set(c, t, -1 * kMs), 0u);
    CHECK_EQ(wait(c, t, 500), 0u);
    CHECK_EQ(call(__imp__NtSetTimerEx, c, t, scratch + kDue, 0, 1, 0, 0, 0, 1), kAccessViolation);
    CHECK_EQ(call(__imp__NtCancelTimer, c, t, 1), kAccessViolation);
    CHECK_EQ(wait(c, t, 0), 0u);
    close(c, ev);
    close(c, t);
    CHECK_EQ(call(__imp__NtCancelTimer, c, t, 0), kInvalidHandle);
}
}  // namespace

int main() {
    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK_ST(runtime_init(&mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &scratch), Status::Ok);
    as_guest(TESTDOUBLE_notification);
    as_guest(TESTDOUBLE_synchronization);
    as_guest(TESTDOUBLE_wake);
    as_guest(TESTDOUBLE_errors);
    CHECK_EQ(runtime()->handles.live_count(), 0u);

    // Outside the subset: trap, never a fake success, and no handle leaks.
    PPCContext c{};
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__NtCreateTimer, c, scratch + kOut, 0, 2), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(guest_write_be32(scratch + kAttrs + 4, scratch + kAttrs + 16));  // a named timer
    CAPTURE_FATAL(call(__imp__NtCreateTimer, c, scratch + kOut, scratch + kAttrs, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_EQ(runtime()->handles.live_count(), 0u);
    CHECK_EQ(call(__imp__NtCreateTimer, c, scratch + kOut, 0, 0), 0u);
    const uint32_t t = read(scratch + kOut);
    CHECK(guest_write_be64(scratch + kDue, uint64_t(-10 * kMs)));
    CAPTURE_FATAL(call(__imp__NtSetTimerEx, c, t, scratch + kDue, 0x82000000, 1, 0, 0, 0, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED && g_fatal_msg.find("apc_routine") != std::string::npos);
    CAPTURE_FATAL(call(__imp__NtSetTimerEx, c, t, scratch + kDue, 0, 1, 0, 1, 0, 0), fatal);
    CHECK(fatal && g_fatal_msg.find("resume") != std::string::npos);
    CAPTURE_FATAL(call(__imp__NtSetTimerEx, c, t, scratch + kDue, 0, 1, 0, 0, 0xFFFFFFFF, 0), fatal);
    CHECK(fatal && g_fatal_msg.find("period") != std::string::npos);
    close(c, t);
    CHECK_EQ(runtime()->handles.live_count(), 0u);
    runtime_shutdown();
    clear_imports();
    return test_result("rt_timers");
}
