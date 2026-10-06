// xboxkrnl scheduler controls: per-thread scheduling hints, IRQL and spin locks.
//
// IRQL is genuine per-guest-thread state. Interrupts and DPCs run on their own
// guest contexts (src/hle_xboxkrnl_video.cpp, src/hle_xboxkrnl_dpc.cpp), so
// raising it on a title thread only records the level the guest asked for; lowering
// above the current level, releasing a lock the thread does not own, or a
// same-thread reacquire are guest misuse and stop with a diagnostic. Thread
// scheduling hints (base-priority increment, affinity mask, boost-disable) are
// recorded and returned as previous values; the host scheduler is not
// reconfigured (see runtime/docs/SCHEDULER.md).
#include <sched.h>
#include "physical_window.h"
#include "diagnostics.h"
#include <time.h>

#include <atomic>
#include <cstdio>

#include <memory>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/guest_write_tracking.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kDispatchLevel = 2;
constexpr uint32_t kHighestIrql = 31;

Runtime& rt_or_die(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s before runtime_init", fn);
    return *r;
}

GuestThread& current_or_die(const char* fn) {
    GuestThread* t = current_guest_thread();
    if (!t || !t->identity) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s outside a guest thread", fn);
    return *t;
}

[[noreturn]] void misuse(const char* fn, PPCContext& ctx, const char* what, uint32_t value) {
    rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s %s=0x%08X lr=0x%08X", fn, what, value,
                (uint32_t)ctx.lr);
}

// Thread Body resolved from the guest pointer; unknown/stale pointers fault.
std::shared_ptr<ThreadObjectIdentity> thread_from_body(const char* fn, PPCContext& ctx, uint32_t body) {
    std::shared_ptr<ThreadObjectIdentity> identity;
    if (find_thread_object(body, &identity) != Status::Ok) misuse(fn, ctx, "unknown_thread_body", body);
    return identity;
}

// KeSetBasePriorityThread(PKTHREAD, LONG Increment) -> previous increment.
void KeSetBasePriorityThread(PPCContext& ctx, uint8_t*) {
    const auto identity = thread_from_body("KeSetBasePriorityThread", ctx, ctx.r3.u32);
    const int32_t previous = thread_object_exchange_base_priority(identity, (int32_t)ctx.r4.u32);
    ctx.r3.u64 = (uint32_t)previous;
}

// KeSetAffinityThread(PKTHREAD, KAFFINITY) -> previous mask. A zero or
// out-of-range mask has no defined meaning and is rejected.
void KeSetAffinityThread(PPCContext& ctx, uint8_t*) {
    const uint32_t mask = ctx.r4.u32;
    if (!mask || (mask & ~kDefaultThreadAffinity)) misuse("KeSetAffinityThread", ctx, "affinity", mask);
    const uint32_t body = ctx.r3.u32;
    const auto identity = thread_from_body("KeSetAffinityThread", ctx, body);
    ctx.r3.u64 = thread_object_exchange_affinity(identity, mask);
    // The title's own placement (read by the host placement at thread start, RCOMP_HWTHREAD_MAP): a change
    // after the thread started is recorded here and is not applied to the running host thread.
    std::fprintf(stderr, "RCOMP-AFFINITY body=0x%08X hw=0x%02X previous=0x%02X lr=0x%08X\n", body, mask,
                 uint32_t(ctx.r3.u64), uint32_t(ctx.lr));
}

// KeSetDisableBoostThread(PKTHREAD, BOOLEAN) -> previous state.
void KeSetDisableBoostThread(PPCContext& ctx, uint8_t*) {
    const auto identity = thread_from_body("KeSetDisableBoostThread", ctx, ctx.r3.u32);
    ctx.r3.u64 = thread_object_exchange_disable_boost(identity, (ctx.r4.u32 & 0xFF) ? 1 : 0);
}

uint32_t raise_irql(GuestThread& t, const char* fn, PPCContext& ctx, uint32_t level) {
    if (level > kHighestIrql || level < t.irql) misuse(fn, ctx, "raise_to", level);
    const uint32_t old = t.irql;
    t.irql = level;
    return old;
}

void lower_irql(GuestThread& t, const char* fn, PPCContext& ctx, uint32_t level) {
    if (level > t.irql) misuse(fn, ctx, "lower_to_above_current", level);
    t.irql = level;
}

uint32_t* lock_word(const char* fn, PPCContext& ctx, uint32_t address) {
    Runtime& r = rt_or_die(fn);
    if ((address & 3) || !r.mem->is_accessible(address, 4, Protect::ReadWrite))
        misuse(fn, ctx, "spinlock_unwritable_or_misaligned", address);
    return reinterpret_cast<uint32_t*>(r.mem->host(address));
}

// A held lock stores its owner's thread Body (never zero); zero is free.
bool try_lock(uint32_t* word, uint32_t owner) {
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(word, &expected, __builtin_bswap32(owner), false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void acquire_lock(const char* fn, PPCContext& ctx, GuestThread& t, uint32_t address) {
    uint32_t* word = lock_word(fn, ctx, address);
    const uint32_t owner = thread_object_body(t.identity);
    if (__builtin_bswap32(__atomic_load_n(word, __ATOMIC_RELAXED)) == owner)
        misuse(fn, ctx, "spinlock_already_owned_by_caller", address);
#if RCOMP_RUNTIME_DIAGNOSTICS
    // Bring-up diagnostics: report (once per acquisition) a lock that stays contended for over a second.
    timespec started{};
    clock_gettime(CLOCK_MONOTONIC, &started);
    bool reported = false;
    uint32_t spins = 0;
#endif
    while (!try_lock(word, owner)) {
        sched_yield();
#if RCOMP_RUNTIME_DIAGNOSTICS
        if (!reported && ((++spins & 0xFFF) == 0)) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - started.tv_sec >= 1) {
                reported = true;
                const uint32_t holder = __builtin_bswap32(__atomic_load_n(word, __ATOMIC_RELAXED));
                std::shared_ptr<ThreadObjectIdentity> holder_identity;
                const uint32_t holder_id = find_thread_object(holder, &holder_identity) == Status::Ok
                                               ? thread_object_thread_id(holder_identity) : 0;
                std::fprintf(stderr,
                             "RCOMP-SPINLOCK %s lock=0x%08X waiter_thread=%u holder_body=0x%08X holder_thread=%u lr=0x%08X irql=%u\n",
                             fn, address, t.thread_id, holder, holder_id, (uint32_t)ctx.lr, t.irql);
            }
        }
#endif
    }
    note_title_write(address, 4);  // the lock word is guest memory
}

void release_lock(const char* fn, PPCContext& ctx, GuestThread& t, uint32_t address) {
    uint32_t* word = lock_word(fn, ctx, address);
    uint32_t owner = __builtin_bswap32(thread_object_body(t.identity));
    if (!__atomic_compare_exchange_n(word, &owner, 0, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        misuse(fn, ctx, "spinlock_not_owned_by_caller", address);
    note_title_write(address, 4);
}

// KfAcquireSpinLock(PKSPIN_LOCK) raises to DISPATCH_LEVEL and returns the old IRQL.
void KfAcquireSpinLock(PPCContext& ctx, uint8_t*) {
    GuestThread& t = current_or_die("KfAcquireSpinLock");
    const uint32_t target = kDispatchLevel > t.irql ? kDispatchLevel : t.irql;
    const uint32_t old = raise_irql(t, "KfAcquireSpinLock", ctx, target);
    acquire_lock("KfAcquireSpinLock", ctx, t, ctx.r3.u32);
    ctx.r3.u64 = old;
}

// KfReleaseSpinLock(PKSPIN_LOCK, KIRQL OldIrql)
void KfReleaseSpinLock(PPCContext& ctx, uint8_t*) {
    GuestThread& t = current_or_die("KfReleaseSpinLock");
    release_lock("KfReleaseSpinLock", ctx, t, ctx.r3.u32);
    lower_irql(t, "KfReleaseSpinLock", ctx, ctx.r4.u32 & 0xFF);
}

void KeAcquireSpinLockAtRaisedIrql(PPCContext& ctx, uint8_t*) {
    acquire_lock("KeAcquireSpinLockAtRaisedIrql", ctx, current_or_die("KeAcquireSpinLockAtRaisedIrql"),
                 ctx.r3.u32);
}

void KeReleaseSpinLockFromRaisedIrql(PPCContext& ctx, uint8_t*) {
    release_lock("KeReleaseSpinLockFromRaisedIrql", ctx, current_or_die("KeReleaseSpinLockFromRaisedIrql"),
                 ctx.r3.u32);
}

void KeTryToAcquireSpinLockAtRaisedIrql(PPCContext& ctx, uint8_t*) {
    const char* fn = "KeTryToAcquireSpinLockAtRaisedIrql";
    GuestThread& t = current_or_die(fn);
    uint32_t* word = lock_word(fn, ctx, ctx.r3.u32);
    const uint32_t owner = thread_object_body(t.identity);
    if (__builtin_bswap32(__atomic_load_n(word, __ATOMIC_RELAXED)) == owner)
        misuse(fn, ctx, "spinlock_already_owned_by_caller", ctx.r3.u32);
    ctx.r3.u64 = try_lock(word, owner) ? 1 : 0;
}

void KeRaiseIrqlToDpcLevel(PPCContext& ctx, uint8_t*) {
    GuestThread& t = current_or_die("KeRaiseIrqlToDpcLevel");
    ctx.r3.u64 = raise_irql(t, "KeRaiseIrqlToDpcLevel", ctx, kDispatchLevel);
}

void KfLowerIrql(PPCContext& ctx, uint8_t*) {
    lower_irql(current_or_die("KfLowerIrql"), "KfLowerIrql", ctx, ctx.r3.u32 & 0xFF);
}

// KiApcNormalRoutineNop (0x01DF): the kernel's own APC normal routine that does
// nothing by definition: (NormalContext, SystemArgument1, SystemArgument2) -> VOID.
void KiApcNormalRoutineNop(PPCContext&, uint8_t*) {}

// RtlRaiseException (0x136): (PEXCEPTION_RECORD). Structured exception
// dispatch (unwind tables, __C_specific_handler, RtlUnwind) is NOT implemented
// (recompiled code has no frame unwinder). The one exception with a defined
// meaning without a debugger is the Microsoft thread-naming convention,
// code 0x406D1388: {info[0]=0x1000, info[1]=ANSI name, info[2]=thread id or
// -1, info[3]=flags}; with no debugger attached the title's own guard handler
// swallows it, so the call returns after recording the name. Every other code
// stops with the complete record.
constexpr uint32_t kThreadNameException = 0x406D1388u;
void RtlRaiseException(PPCContext& ctx, uint8_t*) {
    Runtime& r = rt_or_die("RtlRaiseException");
    const uint32_t record = ctx.r3.u32;
    uint32_t code = 0, flags = 0, address = 0, count = 0, info[4] = {};
    if (!record || !r.mem->is_accessible(record, 20, Protect::Read) || !guest_read_be32(record, &code) ||
        !guest_read_be32(record + 4, &flags) || !guest_read_be32(record + 12, &address) ||
        !guest_read_be32(record + 16, &count))
        misuse("RtlRaiseException", ctx, "unreadable_record", record);
    for (uint32_t i = 0; i < 4 && i < count && r.mem->is_accessible(record + 20 + 4 * i, 4, Protect::Read); ++i)
        guest_read_be32(record + 20 + 4 * i, &info[i]);
    if (code == kThreadNameException && count >= 3) {
        char name[64] = {};
        for (uint32_t i = 0; i + 1 < sizeof(name) && info[1] && r.mem->is_accessible(info[1] + i, 1, Protect::Read); ++i) {
            const char ch = char(*r.mem->translate(info[1] + i, 1));
            if (!ch) break;
            name[i] = (ch >= 0x20 && ch < 0x7F) ? ch : '.';
        }
        std::fprintf(stdout, "RCOMP-THREADNAME id=0x%08X name=%s\n", info[2], name);
        return;
    }
    // Floating-point exceptions raised by the C runtime's math routines
    // (STATUS_FLOAT_*, 0xC000008D..0xC0000093). Handlers would run through structured
    // exception dispatch, which this runtime does not have; the raise is reported and
    // the raising routine continues with its own default result, as it does on the
    // public rexglue-sdk/Xenia kernels where RtlRaiseException has no effect.
    if (code >= 0xC000008Du && code <= 0xC0000093u) {
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1) < 16)
            std::fprintf(stderr, "RCOMP-EXCEPTION float code=0x%08X address=0x%08X ignored: no structured exception dispatch\n", code, address);
        return;
    }
    {
        // Guest call chain (PPC back-chain: [sp] = caller's sp, saved LR at [caller_sp - 8]).
        uint32_t sp = ctx.r1.u32;
        std::fprintf(stderr, "RCOMP-EXCEPTION fpscr=0x%08X\n", ctx.fpscr.csr);
        std::fprintf(stderr, "RCOMP-EXCEPTION callers:");
        for (int frame = 0; frame < 30 && sp; ++frame) {
            uint32_t back = 0, saved = 0;
            if (!guest_read_be32(sp, &back) || !back || !guest_read_be32(back - 8, &saved)) break;
            std::fprintf(stderr, " 0x%08X", saved);
            sp = back;
        }
        std::fprintf(stderr, "\n");
    }
    // The first parameter often points at the raiser's message/record: show it (bounded, printable).
    {
        char text[161] = {};
        uint32_t n = 0;
        for (uint32_t i = 0; i < 160 && info[0] && r.mem->is_accessible(info[0] + i, 1, Protect::Read); ++i) {
            const uint8_t ch = *r.mem->translate(info[0] + i, 1);
            if (!ch) break;
            text[n++] = (ch >= 0x20 && ch < 0x7F) ? char(ch) : '.';
        }
        uint32_t words[8] = {};
        for (uint32_t i = 0; i < 8; ++i) guest_read_be32(info[0] + 4 * i, &words[i]);
        std::fprintf(stderr, "RCOMP-EXCEPTION param0 text='%s' words=%08X %08X %08X %08X %08X %08X %08X %08X\n", text, words[0],
                     words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
    }
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                "xboxkrnl.exe!RtlRaiseException code=0x%08X flags=0x%08X address=0x%08X params=%u [0x%08X 0x%08X 0x%08X 0x%08X] "
                "lr=0x%08X (structured exception dispatch not implemented)",
                code, flags, address, count, info[0], info[1], info[2], info[3], (uint32_t)ctx.lr);
}

struct Impl { uint32_t ordinal; const char* name; PPCFunc* function; };
constexpr Impl kImpls[] = {
    {0x004D, "KeAcquireSpinLockAtRaisedIrql", &KeAcquireSpinLockAtRaisedIrql},
    {0x0085, "KeRaiseIrqlToDpcLevel", &KeRaiseIrqlToDpcLevel},
    {0x0089, "KeReleaseSpinLockFromRaisedIrql", &KeReleaseSpinLockFromRaisedIrql},
    {0x0097, "KeSetAffinityThread", &KeSetAffinityThread},
    {0x0099, "KeSetBasePriorityThread", &KeSetBasePriorityThread},
    {0x009C, "KeSetDisableBoostThread", &KeSetDisableBoostThread},
    {0x00AE, "KeTryToAcquireSpinLockAtRaisedIrql", &KeTryToAcquireSpinLockAtRaisedIrql},
    {0x00B1, "KfAcquireSpinLock", &KfAcquireSpinLock},
    {0x00B3, "KfLowerIrql", &KfLowerIrql},
    {0x00B4, "KfReleaseSpinLock", &KfReleaseSpinLock},
    {0x0136, "RtlRaiseException", &RtlRaiseException},
    {0x01DF, "KiApcNormalRoutineNop", &KiApcNormalRoutineNop},
};

}  // namespace

Status register_xboxkrnl_sched_hle() {
    for (const auto& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            return Status::InvalidArgument;
        const Status status = register_import(kModuleXboxkrnl, impl.ordinal, impl.function, impl.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
