#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_heap.h"
#include "rcomp/runtime/guest_thread.h"
#include "test_util.h"

using namespace rcomp;
using namespace rcomp::rt;

namespace {
#if defined(__CYGWIN__)
sigjmp_buf TESTDOUBLE_fault_jump;
volatile sig_atomic_t TESTDOUBLE_fault_signal = 0;
void TESTDOUBLE_fault_handler(int signo) {
    TESTDOUBLE_fault_signal = signo;
    siglongjmp(TESTDOUBLE_fault_jump, 1);
}
#endif
uint32_t be32(uint8_t* base, uint32_t a) {
    uint32_t v;
    memcpy(&v, base + a, 4);
    return __builtin_bswap32(v);
}

// Stands in for a recompiled guest function: uses the guest stack through
// base + r1 exactly like generated code (stwu r1,-0x60(r1); stw r31,..).
void fake_guest_entry(PPCContext& ctx, uint8_t* base) {
    uint32_t sp = ctx.r1.u32;
    uint32_t nsp = sp - 0x60;
    uint32_t tmp = __builtin_bswap32(sp);
    memcpy(base + nsp, &tmp, 4);  // back chain
    ctx.r1.u32 = nsp;
    uint32_t v = __builtin_bswap32(ctx.r3.u32 * 2);
    memcpy(base + nsp + 0x10, &v, 4);
    ctx.r3.u64 = be32(base, nsp + 0x10) + 1;
    ctx.r1.u32 = sp;
}
}  // namespace

int main() {
    GuestMemory mem;
    if (mem.reserve() != MemStatus::Ok) return 2;
    GuestHeap heap;
    CHECK_ST(heap.init(&mem), Status::Ok);
    uint8_t* base = mem.base();

    GuestThreadInit init{0x40000, 0x82000000, 0x1234};
    PPCContext ctx;
    GuestThread t;
    CHECK_ST(create_guest_thread(heap, init, &ctx, &t), Status::Ok);

    // Stack layout.
    CHECK_EQ(t.stack_limit, t.alloc_base + kStackGuardSize);
    CHECK_EQ(t.stack_base - t.stack_limit, 0x40000u);
    CHECK_EQ(ctx.r1.u32, t.stack_base - kStackAbiReserve);
    CHECK_EQ(ctx.r1.u32 % 16, 0u);
    CHECK(ctx.r1.u32 >= t.stack_limit && ctx.r1.u32 < t.stack_base);
    CHECK(t.alloc_base >= GuestHeap::kDefaultLo && t.stack_base <= GuestHeap::kDefaultHi);
    CHECK_EQ(be32(base, ctx.r1.u32), 0u);  // back chain terminator
    CHECK(mem.is_committed(t.stack_limit, t.stack_base - t.stack_limit));
    // Distinct from the native stack: the guest stack is inside the guest
    // reservation, the native one is not.
    int native_local = 0;
    uintptr_t native = (uintptr_t)&native_local;
    CHECK(native < (uintptr_t)base || native >= (uintptr_t)base + kGuestSpaceSize);

    // r13 / PCR, lr sentinel, r3.
    CHECK_EQ(ctx.r13.u32, t.pcr);
    CHECK_EQ(be32(base, t.pcr + kPcrTlsPtr), t.tls);
    CHECK_EQ(be32(base, t.pcr + kPcrSelfPtr + 4), t.pcr);  // low word of the u64
    CHECK_EQ(be32(base, t.pcr + kPcrStackBase), t.stack_base);
    CHECK_EQ(be32(base, t.pcr + kPcrStackEnd), t.stack_limit);
    CHECK_EQ((uint32_t)ctx.lr, kGuestLrSentinel);
    CHECK_EQ(ctx.r3.u32, 0x1234u);
    CHECK_EQ(ctx.r4.u64, 0ull);

    // Run an entry on it.
    uint32_t code = 0;
    CHECK_ST(run_guest_thread(t, ctx, base, fake_guest_entry, &code), Status::Ok);
    CHECK_EQ(code, 0x1234u * 2 + 1);
    CHECK_EQ(ctx.r1.u32, t.stack_base - kStackAbiReserve);

    // A fatal unwound out of guest code by a test hook leaves the run active
    // (destroy refused, no second run) until it is explicitly abandoned.
    {
        auto fatal_entry = [](PPCContext&, uint8_t*) { rcomp_fatal(RCOMP_FATAL_INTERNAL, "test fatal in guest code"); };
        bool fatal = false;
        CAPTURE_FATAL(run_guest_thread(t, ctx, base, fatal_entry, &code), fatal);
        CHECK(fatal);
        CHECK(current_guest_thread() == &t);
        CHECK_ST(destroy_guest_thread(heap, &t), Status::InvalidArgument);  // t left intact
        CHECK(t.alloc_base != 0);
        CHECK_ST(run_guest_thread(t, ctx, base, fake_guest_entry, &code), Status::InvalidArgument);
        abandon_current_guest_thread_after_fatal();
        CHECK(current_guest_thread() == nullptr);
        ctx.r3.u64 = 7;
        ctx.r1.u64 = t.initial_r1;
        CHECK_ST(run_guest_thread(t, ctx, base, fake_guest_entry, &code), Status::Ok);
        CHECK_EQ(code, 7u * 2 + 1);
        bool again = false;
        CAPTURE_FATAL(abandon_current_guest_thread_after_fatal(), again);
        CHECK(again);  // nothing to abandon
    }

    // Guard page below the stack faults (checked in a child process).
    fflush(stdout);
    fflush(stderr);
#if defined(__CYGWIN__)
    // Native guest reservations cannot be inherited by Cygwin fork.
    // Probe the real guard page in this process and restore both handlers.
    struct sigaction action{}, old_segv{}, old_bus{};
    action.sa_handler = TESTDOUBLE_fault_handler;
    sigemptyset(&action.sa_mask);
    CHECK_EQ(sigaction(SIGSEGV, &action, &old_segv), 0);
    CHECK_EQ(sigaction(SIGBUS, &action, &old_bus), 0);
    if (sigsetjmp(TESTDOUBLE_fault_jump, 1) == 0) {
        volatile uint8_t* guard = base + t.stack_limit - 8;
        *guard = 1;
    }
    CHECK_EQ(sigaction(SIGSEGV, &old_segv, nullptr), 0);
    CHECK_EQ(sigaction(SIGBUS, &old_bus, nullptr), 0);
    CHECK(TESTDOUBLE_fault_signal == SIGSEGV || TESTDOUBLE_fault_signal == SIGBUS);
#else
    pid_t pid = fork();
    if (pid == 0) {
        volatile uint8_t* g = base + t.stack_limit - 8;
        *g = 1;  // must SIGSEGV
        _exit(0);
    }
    int ws = 0;
    waitpid(pid, &ws, 0);
    CHECK(WIFSIGNALED(ws) && (WTERMSIG(ws) == SIGSEGV || WTERMSIG(ws) == SIGBUS));
#endif

    // Invalid sizes.
    GuestThread t2;
    PPCContext c2;
    GuestThreadInit bad{0x1234, 0, 0};
    CHECK_ST(create_guest_thread(heap, bad, &c2, &t2), Status::InvalidArgument);
    GuestThreadInit zero{0, 0, 0};
    CHECK_ST(create_guest_thread(heap, zero, &c2, &t2), Status::InvalidArgument);

    // Destroy releases everything and restores the guard page.
    uint32_t live_before = heap.stats().live_allocations;
    GuestThread copy = t;
    const uint32_t guard_addr = t.alloc_base;
    CHECK_ST(destroy_guest_thread(heap, &t), Status::Ok);
    CHECK_EQ(heap.stats().live_allocations, live_before - 3);
    CHECK_ST(destroy_guest_thread(heap, &t), Status::InvalidArgument);
    CHECK_ST(destroy_guest_thread(heap, &copy), Status::DoubleFree);
    base[guard_addr] = 7;  // guard page writable again after free
    CHECK_EQ(base[guard_addr], 7);

    // exit_current_guest_thread outside a guest thread is fatal.
    bool fatal = false;
    CAPTURE_FATAL(exit_current_guest_thread(5), fatal);
    CHECK(fatal);
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_INTERNAL);

    return test_result("rt_test_stack");
}
