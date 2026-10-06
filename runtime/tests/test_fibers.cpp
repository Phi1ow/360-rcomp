// Guest fibers (runtime/docs/THREAD_OBJECTS.md ("Guest fibers")). xapi's fiber routines are statically
// linked into titles; here they are test doubles that follow the generated
// code of the Halo 3 inventory instruction by instruction (KTHREAD and fiber
// block offsets, the SwitchToFiber save/load sequence and its tail call to
// KeSetCurrentStackPointers), using the generated-code memory macros, so
// every KTHREAD access goes through the real virtual-field provider.
//
// Proves: KTHREAD +0x164/+0x5C/+0x60/+0xD0/+0x84 and KPROCESS +0x1C values,
// ConvertThreadToFiber / CreateFiber / SwitchToFiber round trips with the
// non-volatile registers, guest stack and host frames preserved, several
// fibers, fiber-to-fiber switches, switching to the running fiber, a fiber
// deleted while suspended (its host context released), a fiber resumed by
// another guest thread, a thread that exits on a fiber, and the explicit
// diagnostics (unknown continuation, fiber entry returning).
#include <stddef.h>

#include <thread>
#include <vector>

#include "rcomp/ppc_prelude.h"
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_heap.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/kernel_variables.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/virtual_fields.h"
#include "rcomp/runtime_state.h"
#include "host_fiber.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__KeSetCurrentStackPointers);
PPC_EXTERN_FUNC(__imp__MmCreateKernelStack);
PPC_EXTERN_FUNC(__imp__MmDeleteKernelStack);
PPC_EXTERN_FUNC(__imp__ExTerminateThread);

using namespace rcomp;
using namespace rcomp::rt;

namespace {
GuestMemory mem;
uint32_t g_vars = 0;  // guest block: test variables the fibers read

// Guest variable offsets inside g_vars.
constexpr uint32_t kNextOfA = 0x00, kNextOfB = 0x04;
// Guest addresses of the test-double functions (function table).
constexpr uint32_t kTrampoline = 0x82010000, kFiberA = 0x82010100, kFiberB = 0x82010200, kFiberExit = 0x82010300,
                   kReturns = 0x82010400;
constexpr uint32_t kFiberBlockSize = 2640;  // xapi's allocation
constexpr uint32_t kErrorAlreadyFiber = 0;  // ConvertThreadToFiber's return on failure

// ---- register helpers ------------------------------------------------------------
static_assert(offsetof(PPCContext, r31) - offsetof(PPCContext, r14) == 17 * sizeof(PPCRegister));
static_assert(offsetof(PPCContext, f31) - offsetof(PPCContext, f14) == 17 * sizeof(PPCRegister));
static_assert(offsetof(PPCContext, v127) - offsetof(PPCContext, v64) == 63 * sizeof(PPCVRegister));
PPCRegister& gpr(PPCContext& c, int n) { return (&c.r14)[n - 14]; }
PPCRegister& fpr(PPCContext& c, int n) { return (&c.f14)[n - 14]; }
PPCVRegister& vr(PPCContext& c, int n) { return (&c.v64)[n - 64]; }
PPCCRRegister& crf(PPCContext& c, int n) { return (&c.cr0)[n]; }

uint32_t pack_cr(PPCContext& c) {  // mfcr
    uint32_t value = 0;
    for (int i = 0; i < 8; ++i) {
        const PPCCRRegister& f = crf(c, i);
        value |= uint32_t((f.lt ? 8 : 0) | (f.gt ? 4 : 0) | (f.eq ? 2 : 0) | (f.so ? 1 : 0)) << (28 - 4 * i);
    }
    return value;
}
void unpack_cr(PPCContext& c, uint32_t value) {  // mtcr
    for (int i = 0; i < 8; ++i) {
        const uint32_t bits = value >> (28 - 4 * i);
        PPCCRRegister& f = crf(c, i);
        f.lt = (bits >> 3) & 1; f.gt = (bits >> 2) & 1; f.eq = (bits >> 1) & 1; f.so = bits & 1;
    }
}

// Non-volatile state of one fiber: r14-r31, f14-f31, v64-v127, CR fields 2-4.
void set_nonvolatiles(PPCContext& c, uint32_t seed) {
    for (int i = 14; i <= 31; ++i) gpr(c, i).u64 = (uint64_t(seed) << 32) | uint32_t(i * 0x01010101u);
    for (int i = 14; i <= 31; ++i) fpr(c, i).f64 = double(seed) + i * 0.25;
    for (int i = 64; i <= 127; ++i)
        for (int w = 0; w < 4; ++w) vr(c, i).u32[w] = seed ^ uint32_t(i << 8 | w);
    unpack_cr(c, (pack_cr(c) & 0xFF000FFFu) | ((seed * 0x111u) & 0x00FFF000u));
}
bool check_nonvolatiles(PPCContext& c, uint32_t seed) {
    bool ok = true;
    for (int i = 14; i <= 31; ++i) ok &= gpr(c, i).u64 == ((uint64_t(seed) << 32) | uint32_t(i * 0x01010101u));
    for (int i = 14; i <= 31; ++i) ok &= fpr(c, i).f64 == double(seed) + i * 0.25;
    for (int i = 64; i <= 127; ++i)
        for (int w = 0; w < 4; ++w) ok &= vr(c, i).u32[w] == (seed ^ uint32_t(i << 8 | w));
    ok &= (pack_cr(c) & 0x00FFF000u) == ((seed * 0x111u) & 0x00FFF000u);
    return ok;
}
// Clobbers what a call may clobber (volatile registers), as callee code would.
void clobber_volatiles(PPCContext& c) {
    c.r0.u64 = c.r3.u64 = c.r4.u64 = c.r5.u64 = c.r6.u64 = c.r7.u64 = c.r8.u64 = 0xDEADBEEFDEADBEEFull;
    c.r9.u64 = c.r10.u64 = c.r11.u64 = c.r12.u64 = 0xDEADBEEFDEADBEEFull;
    c.f1.f64 = c.f2.f64 = -1.0;
}

void push_frame(PPCContext& ctx, uint8_t* base, uint32_t size) {  // stwu r1,-size(r1)
    const uint32_t ea = ctx.r1.u32 - size;
    PPC_STORE_U32(ea, ctx.r1.u32);
    ctx.r1.u64 = ea;
}
void pop_frame(PPCContext& ctx, uint32_t size) { ctx.r1.u64 = ctx.r1.u32 + size; }

uint32_t alloc_block() {
    uint32_t block = 0;
    CHECK_ST(runtime()->heap.alloc(kFiberBlockSize, 16, true, &block), Status::Ok);
    memset(mem.base() + block, 0, kFiberBlockSize);
    return block;
}
uint32_t thread_body(PPCContext& ctx, uint8_t* base) { return PPC_LOAD_U32(ctx.r13.u32 + 256); }

// ---- xapi (test doubles of the Halo 3 inventory routines) -----------------------

// sub_8259FC10 GetCurrentFiber
void TESTDOUBLE_GetCurrentFiber(PPCContext& ctx, uint8_t* base) {
    ctx.r11.u64 = PPC_LOAD_U32(ctx.r13.u32 + 256);
    ctx.r3.u64 = PPC_LOAD_U32(ctx.r11.u32 + 356);
}
// sub_8259FC20 ConvertThreadToFiber(param)
void TESTDOUBLE_ConvertThreadToFiber(PPCContext& ctx, uint8_t* base) {
    const uint32_t thread = PPC_LOAD_U32(ctx.r13.u32 + 256);
    if (PPC_LOAD_U32(thread + 356)) { ctx.r3.u64 = kErrorAlreadyFiber; return; }
    const uint32_t param = ctx.r3.u32, fiber = alloc_block();
    PPC_STORE_U32(fiber + 0, param);
    PPC_STORE_U32(fiber + 4, PPC_LOAD_U32(thread + 208));
    PPC_STORE_U32(fiber + 8, PPC_LOAD_U32(thread + 92));
    PPC_STORE_U32(fiber + 12, PPC_LOAD_U32(thread + 96));
    PPC_STORE_U32(thread + 356, fiber);
    ctx.r3.u64 = fiber;
}
// sub_8259FD00 CreateFiber(stack size, start, param)
void TESTDOUBLE_CreateFiber(PPCContext& ctx, uint8_t* base) {
    const uint32_t start = ctx.r4.u32, param = ctx.r5.u32;
    uint32_t size = ctx.r3.u32;
    if (!size) size = PPC_LOAD_U32(PPC_LOAD_U32(PPC_LOAD_U32(ctx.r13.u32 + 256) + 132) + 28);
    else size = (size + 4095) & 0xFFFFF000u;
    if (size < 16384) size = 16384;
    const uint32_t fiber = alloc_block();
    ctx.r3.u64 = size;
    ctx.r4.u64 = 0;
    ctx.lr = 0x8259FD74;
    __imp__MmCreateKernelStack(ctx, base);
    const uint32_t top = ctx.r3.u32;
    CHECK(top != 0);
    PPC_STORE_U32(fiber + 4, top);
    PPC_STORE_U32(fiber + 8, top);
    PPC_STORE_U32(fiber + 0, param);
    PPC_STORE_U32(fiber + 12, top - size);
    memset(base + top - 80, 0, 80);
    PPC_STORE_U64(fiber + 48, uint64_t(top - 80));
    PPC_STORE_U64(fiber + 288, uint64_t(start));  // r31 slot
    PPC_STORE_U32(fiber + 28, kTrampoline);       // LR slot
    ctx.r3.u64 = fiber;
}
// sub_825A2180 SwitchToFiber(target): saves the running fiber, loads the
// target, publishes it at KTHREAD+0x164 and tail-calls KeSetCurrentStackPointers.
void TESTDOUBLE_SwitchToFiber(PPCContext& ctx, uint8_t* base) {
    ctx.r4.u64 = PPC_LOAD_U32(ctx.r13.u32 + 256);
    ctx.r5.u64 = PPC_LOAD_U32(ctx.r4.u32 + 356);
    const uint32_t from = ctx.r5.u32, to = ctx.r3.u32;
    ctx.r6.u64 = pack_cr(ctx);
    ctx.r7.u64 = ctx.lr;
    PPC_STORE_U64(from + 48, ctx.r1.u64);
    for (int i = 14; i <= 31; ++i) PPC_STORE_U64(from + 152 + 8 * (i - 14), gpr(ctx, i).u64);
    PPC_STORE_U32(from + 296, ctx.r6.u32);
    PPC_STORE_U32(from + 28, ctx.r7.u32);
    for (int i = 14; i <= 31; ++i) PPC_STORE_U64(from + 424 + 8 * (i - 14), fpr(ctx, i).u64);
    for (int i = 64; i <= 127; ++i) memcpy(base + from + 1616 + 16 * (i - 64), &vr(ctx, i), 16);
    ctx.r6.u64 = PPC_LOAD_U32(to + 296);
    ctx.r7.u64 = PPC_LOAD_U32(to + 28);
    for (int i = 14; i <= 31; ++i) gpr(ctx, i).u64 = PPC_LOAD_U64(to + 152 + 8 * (i - 14));
    unpack_cr(ctx, ctx.r6.u32);
    ctx.lr = ctx.r7.u64;
    for (int i = 14; i <= 31; ++i) fpr(ctx, i).u64 = PPC_LOAD_U64(to + 424 + 8 * (i - 14));
    for (int i = 64; i <= 127; ++i) memcpy(&vr(ctx, i), base + to + 1616 + 16 * (i - 64), 16);
    PPC_STORE_U32(ctx.r4.u32 + 356, to);
    ctx.r5.u64 = PPC_LOAD_U32(to + 4);
    ctx.r6.u64 = PPC_LOAD_U32(to + 8);
    ctx.r7.u64 = PPC_LOAD_U32(to + 12);
    ctx.r3.u64 = PPC_LOAD_U64(to + 48);
    __imp__KeSetCurrentStackPointers(ctx, base);
}
// sub_8259FDE0 DeleteFiber for a fiber that is not running.
void TESTDOUBLE_DeleteFiber(PPCContext& ctx, uint8_t* base) {
    const uint32_t fiber = ctx.r3.u32;
    CHECK(PPC_LOAD_U32(thread_body(ctx, base) + 356) != fiber);
    ctx.r4.u64 = PPC_LOAD_U32(fiber + 12);
    ctx.r3.u64 = PPC_LOAD_U32(fiber + 4);
    ctx.lr = 0x8259FE1C;
    __imp__MmDeleteKernelStack(ctx, base);
    CHECK_ST(runtime()->heap.free(fiber), Status::Ok);
}
bool g_startup_returned = false;
// sub_8259FE50 fiber start-up: start(GetFiberData()), then KeBugCheck.
void TESTDOUBLE_FiberStartup(PPCContext& ctx, uint8_t* base) {
    push_frame(ctx, base, 96);
    const uint32_t start = ctx.r3.u32;
    ctx.r3.u64 = PPC_LOAD_U32(PPC_LOAD_U32(thread_body(ctx, base) + 356) + 0);
    ctx.lr = 0x8259FE84;
    PPCFunc* fn = lookup_function(start);
    CHECK(fn != nullptr);
    if (fn) fn(ctx, base);
    g_startup_returned = true;  // KeBugCheck(0) on the console
}
// sub_825A2170: mr r3,r31 ; bl 0x8259FE50
void TESTDOUBLE_Trampoline(PPCContext& ctx, uint8_t* base) {
    ctx.r3.u64 = ctx.r31.u64;
    ctx.lr = kTrampoline + 8;
    TESTDOUBLE_FiberStartup(ctx, base);
}

uint32_t call(PPCFunc* fn, PPCContext& ctx, uint8_t* base, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0,
              uint32_t lr = 0x82020000) {
    ctx.r3.u64 = a; ctx.r4.u64 = b; ctx.r5.u64 = c;
    ctx.lr = lr;
    fn(ctx, base);
    return ctx.r3.u32;
}
void switch_to(PPCContext& ctx, uint8_t* base, uint32_t fiber, uint32_t return_address) {
    clobber_volatiles(ctx);
    call(TESTDOUBLE_SwitchToFiber, ctx, base, fiber, 0, 0, return_address);
}

// ---- fiber bodies -----------------------------------------------------------------
struct FiberLog {
    uint32_t param = 0, rounds = 0, own_fiber = 0, body = 0, kthread_stack_base = 0, kthread_stack_limit = 0,
             pcr_stack_base = 0, pcr_stack_end = 0, resumed_body = 0;
    bool ok = true;
};
FiberLog g_a, g_b, g_exit;

// Loops forever: records, switches to the fiber named by the guest variable
// `next`, and checks its whole state each time it is resumed.
void fiber_loop(PPCContext& ctx, uint8_t* base, FiberLog& log, uint32_t seed, uint32_t next, uint32_t lr) {
    log.param = ctx.r3.u32;
    push_frame(ctx, base, 0x80);
    const uint32_t sp = ctx.r1.u32;
    PPC_STORE_U32(sp + 0x20, seed * 3);
    set_nonvolatiles(ctx, seed);
    const uint32_t body = thread_body(ctx, base);
    log.body = body;
    log.own_fiber = PPC_LOAD_U32(body + 0x164);
    log.kthread_stack_base = PPC_LOAD_U32(body + 0x5C);
    log.kthread_stack_limit = PPC_LOAD_U32(body + 0x60);
    log.pcr_stack_base = PPC_LOAD_U32(ctx.r13.u32 + kPcrStackBase);
    log.pcr_stack_end = PPC_LOAD_U32(ctx.r13.u32 + kPcrStackEnd);
    log.ok &= PPC_LOAD_U32(body + 0xD0) == PPC_LOAD_U32(log.own_fiber + 4);
    log.ok &= sp < log.kthread_stack_base && sp >= log.kthread_stack_limit;
    for (;;) {
        switch_to(ctx, base, PPC_LOAD_U32(g_vars + next), lr);
        ++log.rounds;
        const uint32_t now_body = thread_body(ctx, base);
        log.resumed_body = now_body;
        log.ok &= check_nonvolatiles(ctx, seed);
        log.ok &= ctx.r1.u32 == sp;
        log.ok &= PPC_LOAD_U32(sp + 0x20) == seed * 3;
        log.ok &= PPC_LOAD_U32(now_body + 0x164) == log.own_fiber;
        log.ok &= PPC_LOAD_U32(now_body + 0x5C) == log.kthread_stack_base;
        log.ok &= PPC_LOAD_U32(ctx.r13.u32 + kPcrStackEnd) == log.pcr_stack_end;
    }
}
void TESTDOUBLE_FiberA(PPCContext& ctx, uint8_t* base) { fiber_loop(ctx, base, g_a, 0xA00, kNextOfA, kFiberA + 0x40); }
void TESTDOUBLE_FiberB(PPCContext& ctx, uint8_t* base) { fiber_loop(ctx, base, g_b, 0xB00, kNextOfB, kFiberB + 0x40); }
void TESTDOUBLE_FiberExit(PPCContext& ctx, uint8_t* base) {
    g_exit.param = ctx.r3.u32;
    push_frame(ctx, base, 0x60);
    call(__imp__ExTerminateThread, ctx, base, 0x55, 0, 0, kFiberExit + 0x10);
    g_exit.ok = false;  // never reached
}
void TESTDOUBLE_Returns(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0x99; }

// ---- guest threads ----------------------------------------------------------------
struct MainLog {
    bool ok = true;
    uint32_t main_fiber = 0, fiber_a = 0, fiber_b = 0;
    uint32_t a_top = 0, b_top = 0, b_limit = 0;
    size_t live_after_round_trips = 0, suspended_after_round_trips = 0, live_after_delete = 0;
    bool unknown_continuation_fatal = false;
} g_main;

void TESTDOUBLE_main(PPCContext& ctx, uint8_t* base) {
    GuestThread* self = current_guest_thread();
    const uint32_t body = thread_body(ctx, base);
    // KTHREAD fields before any fiber.
    CHECK_EQ(PPC_LOAD_U32(body + 0x164), 0u);
    CHECK_EQ(PPC_LOAD_U32(body + 0x5C), self->stack_base);
    CHECK_EQ(PPC_LOAD_U32(body + 0x60), self->stack_limit);
    CHECK_EQ(PPC_LOAD_U32(body + 0xD0), self->stack_base);
    CHECK_EQ(PPC_LOAD_U32(body + 0x84), kTitleProcessVirtualAddress);
    CHECK_EQ(PPC_LOAD_U32(PPC_LOAD_U32(body + 0x84) + 0x1C), kDefaultGuestThreadStackSize);
    uint64_t value = 0;
    CHECK(runtime_virtual_write(body + 0x5C, 4, 0, 0) == VirtualAccessStatus::ReadOnly);
    CHECK(runtime_virtual_write(body + 0x84, 4, 0, 0) == VirtualAccessStatus::ReadOnly);
    CHECK(runtime_virtual_write(kTitleProcessVirtualAddress + 0x1C, 4, 0, 0) == VirtualAccessStatus::ReadOnly);
    CHECK(runtime_virtual_read(body + 0x168, 4, 0, &value) == VirtualAccessStatus::UnknownField);
    CHECK(runtime_virtual_read(body + 0x164, 2, 0, &value) == VirtualAccessStatus::InvalidWidth);
    CHECK(runtime_virtual_read(kTitleProcessVirtualAddress + 0x20, 4, 0, &value) == VirtualAccessStatus::UnknownField);

    // ConvertThreadToFiber, twice.
    CHECK_EQ(call(TESTDOUBLE_GetCurrentFiber, ctx, base), 0u);
    const uint32_t main_fiber = call(TESTDOUBLE_ConvertThreadToFiber, ctx, base, 0x1234);
    g_main.main_fiber = main_fiber;
    CHECK(main_fiber != 0);
    CHECK_EQ(call(TESTDOUBLE_GetCurrentFiber, ctx, base), main_fiber);
    CHECK_EQ(call(TESTDOUBLE_ConvertThreadToFiber, ctx, base, 0x5678), kErrorAlreadyFiber);
    CHECK_EQ(PPC_LOAD_U32(main_fiber + 0), 0x1234u);
    CHECK_EQ(PPC_LOAD_U32(main_fiber + 8), self->stack_base);
    CHECK_EQ(PPC_LOAD_U32(main_fiber + 12), self->stack_limit);

    // CreateFiber with an explicit stack size and with 0 (KPROCESS default).
    const uint32_t fiber_a = call(TESTDOUBLE_CreateFiber, ctx, base, 0x8000, kFiberA, 0xAAAA);
    const uint32_t fiber_b = call(TESTDOUBLE_CreateFiber, ctx, base, 0, kFiberB, 0xBBBB);
    g_main.fiber_a = fiber_a;
    g_main.fiber_b = fiber_b;
    g_main.a_top = PPC_LOAD_U32(fiber_a + 8);
    g_main.b_top = PPC_LOAD_U32(fiber_b + 8);
    g_main.b_limit = PPC_LOAD_U32(fiber_b + 12);
    CHECK_EQ(g_main.a_top - PPC_LOAD_U32(fiber_a + 12), 0x8000u);
    CHECK_EQ(g_main.b_top - g_main.b_limit, kDefaultGuestThreadStackSize);

    push_frame(ctx, base, 0x70);
    const uint32_t sp = ctx.r1.u32;
    PPC_STORE_U32(sp + 0x10, 0x51515151);
    set_nonvolatiles(ctx, 0x100);
    auto main_state_ok = [&] {
        bool ok = check_nonvolatiles(ctx, 0x100) && ctx.r1.u32 == sp && PPC_LOAD_U32(sp + 0x10) == 0x51515151u;
        ok &= PPC_LOAD_U32(body + 0x164) == main_fiber;
        ok &= PPC_LOAD_U32(body + 0x5C) == self->stack_base && PPC_LOAD_U32(body + 0x60) == self->stack_limit;
        ok &= PPC_LOAD_U32(ctx.r13.u32 + kPcrStackBase) == self->stack_base;
        ok &= PPC_LOAD_U32(ctx.r13.u32 + kPcrStackEnd) == self->stack_limit;
        return ok;
    };

    // main -> A (new) -> main.
    PPC_STORE_U32(g_vars + kNextOfA, main_fiber);
    switch_to(ctx, base, fiber_a, 0x82030010);
    g_main.ok &= main_state_ok();
    CHECK_EQ(g_a.param, 0xAAAAu);
    CHECK_EQ(g_a.rounds, 0u);
    CHECK_EQ(host_fiber_live_secondary_count(), 1u);
    CHECK_EQ(host_fiber_suspended_count(), 1u);  // A
    // main -> A (resumed) -> main.
    switch_to(ctx, base, fiber_a, 0x82030020);
    g_main.ok &= main_state_ok();
    CHECK_EQ(g_a.rounds, 1u);
    // main -> B (new) -> A (resumed) -> main: a fiber-to-fiber switch.
    PPC_STORE_U32(g_vars + kNextOfB, fiber_a);
    switch_to(ctx, base, fiber_b, 0x82030030);
    g_main.ok &= main_state_ok();
    CHECK_EQ(g_b.param, 0xBBBBu);
    CHECK_EQ(g_a.rounds, 2u);
    CHECK_EQ(g_b.rounds, 0u);
    // Repeated round trips through both fibers.
    PPC_STORE_U32(g_vars + kNextOfB, main_fiber);
    for (int i = 0; i < 200; ++i) {
        switch_to(ctx, base, (i & 1) ? fiber_a : fiber_b, 0x82030040);
        g_main.ok &= main_state_ok();
    }
    CHECK_EQ(g_a.rounds, 102u);
    CHECK_EQ(g_b.rounds, 100u);
    g_main.live_after_round_trips = host_fiber_live_secondary_count();
    g_main.suspended_after_round_trips = host_fiber_suspended_count();

    // Switching to the running fiber returns to its caller.
    switch_to(ctx, base, main_fiber, 0x82030050);
    g_main.ok &= main_state_ok();
    CHECK_EQ(host_fiber_suspended_count(), 2u);

    // A deleted while suspended: its host context is released with its stack.
    call(TESTDOUBLE_DeleteFiber, ctx, base, fiber_a);
    g_main.live_after_delete = host_fiber_live_secondary_count();
    CHECK_EQ(host_fiber_suspended_count(), 1u);  // B

    // A continuation that is neither a suspended fiber nor a function entry.
    bool fatal = false;
    PPCContext probe = ctx;  // the fatal leaves r1..r7 of the call behind
    ctx.r6.u64 = g_main.b_top;
    ctx.r7.u64 = g_main.b_limit;
    CAPTURE_FATAL(call(__imp__KeSetCurrentStackPointers, ctx, base, g_main.b_top - 0x200, body, g_main.b_top,
                       0x82000010),
                  fatal);
    g_main.unknown_continuation_fatal = fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED &&
                                        g_fatal_msg.find("neither a suspended guest fiber") != std::string::npos;
    // Restore the thread's own stack fields (StackPointer == r1: no switch).
    ctx = probe;
    ctx.r3.u64 = ctx.r1.u64; ctx.r4.u64 = body; ctx.r5.u64 = self->stack_base; ctx.r6.u64 = self->stack_base;
    ctx.r7.u64 = self->stack_limit;
    __imp__KeSetCurrentStackPointers(ctx, base);
    g_main.ok &= main_state_ok();

    pop_frame(ctx, 0x70);
    ctx.r3.u64 = 0;  // B stays suspended: it outlives this thread
}

// A second guest thread resumes B (suspended by the first thread), then deletes it.
struct MigrationLog { uint32_t body = 0, own_fiber = 0; bool ok = true; size_t live_after_delete = ~size_t(0); } g_migration;
void TESTDOUBLE_migration_main(PPCContext& ctx, uint8_t* base) {
    const uint32_t body = thread_body(ctx, base);
    g_migration.body = body;
    const uint32_t own = call(TESTDOUBLE_ConvertThreadToFiber, ctx, base, 0x2222);
    g_migration.own_fiber = own;
    set_nonvolatiles(ctx, 0x200);
    PPC_STORE_U32(g_vars + kNextOfB, own);
    const uint32_t rounds = g_b.rounds;
    switch_to(ctx, base, g_main.fiber_b, 0x82040010);
    g_migration.ok &= check_nonvolatiles(ctx, 0x200);
    g_migration.ok &= g_b.rounds == rounds + 1 && g_b.resumed_body == body;  // B ran on this thread
    g_migration.ok &= PPC_LOAD_U32(body + 0x164) == own;
    call(TESTDOUBLE_DeleteFiber, ctx, base, g_main.fiber_b);
    g_migration.live_after_delete = host_fiber_live_secondary_count();
    ctx.r3.u64 = 7;
}

// A thread that ends (ExTerminateThread) while a created fiber runs.
uint32_t g_exit_fiber = 0;
void TESTDOUBLE_exit_main(PPCContext& ctx, uint8_t* base) {
    call(TESTDOUBLE_ConvertThreadToFiber, ctx, base, 0x3333);
    g_exit_fiber = call(TESTDOUBLE_CreateFiber, ctx, base, 0x4000, kFiberExit, 0xE0E0);
    switch_to(ctx, base, g_exit_fiber, 0x82050010);
    g_exit.ok = false;  // never resumed
    ctx.r3.u64 = 1;
}

// A fiber whose first function returns: explicit fatal on the fiber's stack.
void TESTDOUBLE_returning_main(PPCContext& ctx, uint8_t* base) {
    call(TESTDOUBLE_ConvertThreadToFiber, ctx, base, 0x4444);
    const uint32_t fiber = call(TESTDOUBLE_CreateFiber, ctx, base, 0x4000, 0, 0);
    PPC_STORE_U32(fiber + 28, kReturns);  // entry LR: a function that returns
    switch_to(ctx, base, fiber, 0x82060010);
}

uint32_t run_thread(PPCFunc* entry) {
    GuestThread t;
    alignas(64) PPCContext ctx{};
    uint32_t code = ~0u;
    CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0x82000000, 0}, &ctx, &t), Status::Ok);
    CHECK_ST(run_guest_thread(t, ctx, mem.base(), entry, &code), Status::Ok);
    CHECK_ST(destroy_guest_thread(runtime()->heap, &t), Status::Ok);
    return code;
}

// Minimal XEX headers declaring xboxkrnl/xam 2.0.6683.0: registers the
// kernel-token page provider (KPROCESS lives there), as the title bootstrap does.
std::vector<uint8_t> profile_xex() {
    std::vector<uint8_t> x(0x600, 0);
    auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) x[o + i] = uint8_t(v >> (24 - 8 * i)); };
    put32(0, 0x58455832u); put32(8, 0x600); put32(16, 0x400); put32(20, 1); put32(24, 0x000103FFu); put32(28, 0x100);
    memcpy(x.data() + 0x100 + 12, "xboxkrnl.exe", 13);
    memcpy(x.data() + 0x100 + 28, "xam.xex", 8);
    size_t o = 0x100 + 36;
    for (uint16_t name = 0; name < 2; ++name, o += 0x28) {
        put32(o, 0x28); put32(o + 0x1C, 0x201A1B00u); put32(o + 0x20, 0x201A1B00u);
        x[o + 0x24] = 0; x[o + 0x25] = uint8_t(name);
    }
    put32(0x100, uint32_t(o - 0x100)); put32(0x104, 24); put32(0x108, 2);
    return x;
}
}  // namespace

int main() {
    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK_ST(runtime_init(&mem), Status::Ok);
    set_active_guest_memory(&mem);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    const auto xex = profile_xex();
    CHECK_ST(register_xboxkrnl_kernel_variables_from_xex(xex.data(), xex.size(), nullptr), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000, 0x1000, true, &g_vars), Status::Ok);
    const FuncEntry functions[] = {
        {kTrampoline, TESTDOUBLE_Trampoline, "TESTDOUBLE_Trampoline"},
        {kFiberA, TESTDOUBLE_FiberA, "TESTDOUBLE_FiberA"},
        {kFiberB, TESTDOUBLE_FiberB, "TESTDOUBLE_FiberB"},
        {kFiberExit, TESTDOUBLE_FiberExit, "TESTDOUBLE_FiberExit"},
        {kReturns, TESTDOUBLE_Returns, "TESTDOUBLE_Returns"},
    };
    CHECK(register_functions(functions, sizeof functions / sizeof functions[0]));

    // 1. Main thread: fields, round trips, deletion, diagnostics.
    CHECK_EQ(run_thread(TESTDOUBLE_main), 0u);
    CHECK(g_main.ok);
    CHECK(g_a.ok);
    CHECK(g_b.ok);
    CHECK(!g_startup_returned);
    CHECK_EQ(g_main.live_after_round_trips, 2u);
    CHECK_EQ(g_main.suspended_after_round_trips, 2u);
    CHECK_EQ(g_main.live_after_delete, 1u);
    CHECK(g_main.unknown_continuation_fatal);
    CHECK_EQ(g_a.own_fiber, g_main.fiber_a);
    CHECK_EQ(g_a.kthread_stack_base, g_main.a_top);
    CHECK_EQ(g_a.pcr_stack_base, g_main.a_top);
    CHECK_EQ(g_a.kthread_stack_limit, g_main.a_top - 0x8000u);
    CHECK_EQ(g_a.pcr_stack_end, g_main.a_top - 0x8000u);
    CHECK_EQ(g_b.kthread_stack_limit, g_main.b_limit);
    CHECK_EQ(host_fiber_suspended_count(), 1u);  // B outlives its creating thread

    // 2. Another guest thread on another host thread resumes B, then deletes it.
    uint32_t code = 0;
    std::thread other([&] { code = run_thread(TESTDOUBLE_migration_main); });
    other.join();
    CHECK_EQ(code, 7u);
    CHECK(g_migration.ok);
    CHECK(g_b.ok);
    CHECK(g_b.resumed_body == g_migration.body && g_migration.body != g_b.body);
    CHECK_EQ(g_migration.live_after_delete, 0u);
    CHECK_EQ(host_fiber_suspended_count(), 0u);

    // 3. ExTerminateThread on a created fiber ends the thread; the fiber's
    // host context is released. Its guest stack stays allocated (console
    // behaviour) and is deleted afterwards.
    CHECK_EQ(run_thread(TESTDOUBLE_exit_main), 0x55u);
    CHECK(g_exit.ok);
    CHECK_EQ(g_exit.param, 0xE0E0u);
    CHECK_EQ(host_fiber_live_secondary_count(), 0u);
    CHECK_EQ(host_fiber_suspended_count(), 0u);
    {
        uint8_t* base = mem.base();
        alignas(64) PPCContext ctx{};
        ctx.r1.u64 = 0x10;
        ctx.r3.u64 = PPC_LOAD_U32(g_exit_fiber + 4);
        ctx.r4.u64 = PPC_LOAD_U32(g_exit_fiber + 12);
        __imp__MmDeleteKernelStack(ctx, base);
        CHECK_ST(runtime()->heap.free(g_exit_fiber), Status::Ok);
    }

    // 4. A fiber whose first function returns: explicit fatal.
    {
        GuestThread t;
        alignas(64) PPCContext ctx{};
        uint32_t ignored = 0;
        bool fatal = false;
        CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0x82000000, 0}, &ctx, &t), Status::Ok);
        CAPTURE_FATAL(run_guest_thread(t, ctx, mem.base(), TESTDOUBLE_returning_main, &ignored), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
        CHECK(g_fatal_msg.find("guest fiber entry 0x82010400 returned") != std::string::npos);
        abandon_current_guest_thread_after_fatal();
        CHECK_EQ(host_fiber_live_secondary_count(), 0u);
        CHECK_ST(destroy_guest_thread(runtime()->heap, &t), Status::Ok);
    }

    runtime_shutdown();
    clear_imports();
    clear_functions();
    mem.release();
    return test_result("rt_fibers");
}
