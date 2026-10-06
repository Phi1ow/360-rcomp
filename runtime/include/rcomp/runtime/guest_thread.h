// Guest thread / stack initialisation (owner: Agent 3, runtime/).
//
// Conventions (Xbox 360 PPC ABI as used by XenonRecomp output):
//
//  guest memory (heap allocation, 64 KiB aligned)
//   alloc_base                    +64 KiB                       stack_base
//   | guard page (PROT_NONE) | ....... stack grows down ........ |
//                            ^ stack_limit        r1 = stack_base - 0x50 ^
//
//  * The guest stack lives in guest memory (GuestHeap), never on the native
//    stack. A 64 KiB guard page below stack_limit is made inaccessible, so an
//    overflow faults (SIGSEGV) instead of silently corrupting the heap.
//  * r1 = stack_base - kStackAbiReserve (0x50): 16-byte aligned, the 0x50
//    bytes above r1 are the caller-frame area the ABI lets a callee write
//    (saved LR at -8 is *below* r1 in callee prologues, register save
//    helpers __savegprlr_* use negative offsets too). The back-chain word at
//    0(r1) is 0, terminating stack walks.
//  * r13 = guest KPCR-like block (kPcrSize bytes, zeroed) with the fields
//    guest code is known to read inline:
//       +0x000 tls_ptr        -> static template + slots (default kTlsBytes, zeroed)
//       +0x030 pcr_ptr (u64)  -> self
//       +0x070 stack_base_ptr -> stack_base (high)
//       +0x074 stack_end_ptr  -> stack_limit (low)
//       +0x100 current_thread -> the real thread's virtual Body token (u32)
//    Other PCR/IRQL/DPC fields remain outside this supported subset.
//       +0x10C processor_number (u8) -> hardware thread the thread runs on (0 unless ExCreateThread/interrupt set it)
//    Configured XEX TLS initial data is copied per thread.
//  * lr = kGuestLrSentinel. XenonRecomp turns `blr` into a C++ `return`, so lr
//    is only used for diagnostics and by code that branches to it via ctr; the
//    sentinel is not a function address, so such a branch ends in
//    RCOMP_FATAL_INDIRECT_TARGET instead of running something arbitrary.
//  * Other GPRs/FPRs/VRs are 0; r3 = GuestThreadInit::r3. fpscr/msr keep the
//    PPCContext defaults.
#pragma once

#include <stdint.h>
#include <memory>

#include "rcomp/func_table.h"
#include "rcomp/hle.h"
#include "rcomp/runtime/status.h"

namespace rcomp::rt {

class GuestHeap;
struct ThreadObjectIdentity;

constexpr uint32_t kGuestLrSentinel = 0xFEEDFACCu;
constexpr uint32_t kStackAbiReserve = 0x50;
constexpr uint32_t kStackGuardSize = 0x10000;
constexpr uint32_t kPcrSize = 0x2E0;  // >= 0x2D8 (Xbox KPCR size), 16-byte multiple
constexpr uint32_t kTlsBytes = 1024 * 4;  // default when no static XEX TLS is declared
constexpr uint32_t kPcrTlsPtr = 0x000;
constexpr uint32_t kPcrSelfPtr = 0x030;
constexpr uint32_t kPcrStackBase = 0x070;
constexpr uint32_t kPcrStackEnd = 0x074;
constexpr uint32_t kPcrCurrentThread = 0x100;
// Byte: the hardware thread (0..5) the thread runs on. Read inline by the title's D3D
// interrupt handler to pick "its" bit of a CPU-synchronisation mask.
constexpr uint32_t kPcrProcessorNumber = 0x10C;
// Dynamic TLS (KeTlsAlloc/KeTlsGetValue): 64 slots, kept host-side per
// thread (only reachable through the kernel calls, so it cannot collide with
// the XEX static TLS the compiler addresses through the PCR).
constexpr uint32_t kTlsDynamicSlots = 64;
// Guest stack of ExCreateThread when the title passes a stack size of 0. The
// title process object publishes the same value as KPROCESS+0x1C (the default
// stack size xapi's CreateFiber(0, ...) reads); runtime/docs/THREAD_OBJECTS.md ("Guest fibers").
constexpr uint32_t kDefaultGuestThreadStackSize = 0x40000;

struct GuestThread {
    uint32_t alloc_base = 0;   // start of guard page (heap allocation)
    uint32_t stack_limit = 0;  // lowest usable stack byte
    uint32_t stack_base = 0;   // one past the highest stack byte
    uint32_t pcr = 0;          // r13
    uint32_t tls = 0;
    uint32_t tls_size = 0;
    uint32_t initial_r1 = 0;
    uint32_t entry = 0;        // guest address (informational)
    uint32_t thread_id = 0;    // unique, non-zero (ExCreateThread's ThreadId)
    uint32_t irql = 0;         // current IRQL (0 = PASSIVE_LEVEL); owned by the thread itself
    // KeEnableFpuExceptions state (owned by the thread itself). Recorded only:
    // generated code never raises guest FP exceptions (runtime/docs/SUSPEND_PULSE.md).
    bool fpu_exceptions_enabled = false;
    // Present for threads created from the process Runtime heap. This is the
    // common main/worker object identity used by Ob*; standalone test heaps
    // intentionally have no kernel object identity.
    std::shared_ptr<ThreadObjectIdentity> identity;
    uint32_t tls_values[kTlsDynamicSlots] = {};
};

// Allocates stack + guard + PCR + TLS from `heap` and initialises `ctx`.
// init.stack_size must be a non-zero multiple of 64 KiB.
Status create_guest_thread(GuestHeap& heap, const GuestThreadInit& init, PPCContext* ctx,
                           GuestThread* out);
// On cleanup failure, only successfully freed pointers are cleared. The caller
// must retain t and retry, or report a terminal failure before discarding it.
Status destroy_guest_thread(GuestHeap& heap, GuestThread* t);

// Runs `entry` on the calling host thread with `ctx`. Returns Ok when the
// entry returns (*exit_code = r3) or when guest code calls ExTerminateThread
// (*exit_code = its argument). Not reentrant on one host thread.
Status run_guest_thread(GuestThread& t, PPCContext& ctx, uint8_t* base, PPCFunc* entry,
                        uint32_t* exit_code);

// Synchronously invokes a callback in a reusable guest context. A normal
// return does not signal thread exit, abandon handle mutants, or broadcast
// dispatcher wakeups. Explicit ExTerminateThread still retires the context;
// the owner must replace a retired context before invoking another callback.
// Shares run_guest_thread's arguments and non-reentrancy/longjmp constraints.
Status run_guest_callback(GuestThread& t, PPCContext& ctx, uint8_t* base, PPCFunc* entry,
                          uint32_t* exit_code);

// Publish real end-of-context effects before destroying a callback's guest
// stack. Its owner must stop/join all callback producers first. Idempotent
// for an already exited identity; refuses the currently active guest thread.
Status retire_guest_thread(GuestThread& t, uint32_t exit_code);

// Unwinds to the active thread or callback invocation of the calling host thread. rcomp_fatal if no
// guest thread is running on it. Only POD frames (generated code, HLE
// wrappers without live C++ objects) may be crossed.
[[noreturn]] void exit_current_guest_thread(uint32_t exit_code);
GuestThread* current_guest_thread();

// First statement of every host service thread the runtime creates (DPC worker, XMA decoder, XAudio
// frame clock, network worker): its own CPU mask (RCOMP_CPU_MASK_SERVICE, else the common guest mask)
// instead of the creating guest thread's, which may be a single CPU (RCOMP_CPU_MASK_GUEST_MAIN).
// Logs RCOMP-PLACEMENT role=SERVICE_<role>. No effect on hosts other than the console.
void apply_host_service_affinity(const char* role);

// For test fatal hooks only (a production rcomp_fatal exits the process):
// after a hook has longjmp'ed out of guest code, marks the calling host
// thread's run as ended so the GuestThread can be destroyed and another run
// can start. rcomp_fatal if no guest thread was running.
void abandon_current_guest_thread_after_fatal();

}  // namespace rcomp::rt
