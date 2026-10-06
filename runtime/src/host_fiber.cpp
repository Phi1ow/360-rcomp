// Host contexts for guest fibers (owner: Agent 3, runtime/). Contract:
// src/host_fiber.h and runtime/docs/THREAD_OBJECTS.md ("Guest fibers").
//
// The context switch is a minimal x86-64 routine: it saves the callee-saved
// state of the running host context on its own stack (System V: rbx, rbp,
// r12-r15, MXCSR, x87 control word; Microsoft x64, used by the Cygwin host
// tests: also rdi, rsi and xmm6-xmm15), stores the stack pointer, loads the
// other context's stack pointer and pops its state. A new context's stack is
// prepared so that the first switch "returns" into an entry thunk which calls
// fiber_entry(). The Windows TEB stack fields are deliberately not switched:
// Cygwin locates its per-thread data from TEB StackBase.
//
// Host stacks are anonymous mappings with a 64 KiB PROT_NONE guard at the low
// end (an overflow faults instead of corrupting memory). No lock or
// C++ object with a destructor is live across a switch in this file; guest
// frames are plain data.
#include "host_fiber.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

#include <atomic>
#include <map>
#include <mutex>
#include <new>
#include <vector>

#include "rcomp/diag.h"
#include "rcomp/func_table.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"

#if !defined(__x86_64__)
#error "host fibers: only the x86-64 System V (PS5, Linux) and Microsoft x64 (Cygwin) ABIs are implemented"
#endif
// The switch routine and the new-context entry carry an explicit calling
// convention. RCOMP_HOST_FIBER_FORCE_SYSV (host test builds only) selects the
// System V routine -- the one the PS5 runs -- on a Microsoft-ABI host, where
// the compiler then saves rdi/rsi/xmm6-xmm15 around the call itself.
#if (defined(_WIN64) || defined(__CYGWIN__)) && !defined(RCOMP_HOST_FIBER_FORCE_SYSV)
#define RCOMP_HOST_FIBER_MS_ABI 1
#define RCOMP_HOST_FIBER_CC __attribute__((ms_abi))
#else
#define RCOMP_HOST_FIBER_MS_ABI 0
#define RCOMP_HOST_FIBER_CC __attribute__((sysv_abi))
#endif

namespace rcomp::rt {
namespace {

// ---- raw context switch -----------------------------------------------------------

// Bytes below the saved general registers: [0] x87 control word, [8] MXCSR,
// and with the Microsoft ABI [16..176) xmm6-xmm15.
#if RCOMP_HOST_FIBER_MS_ABI
constexpr size_t kSwitchScratch = 176;
constexpr size_t kSwitchGprs = 8;  // popped as r15 r14 r13 r12 rsi rdi rbx rbp
#else
constexpr size_t kSwitchScratch = 16;
constexpr size_t kSwitchGprs = 6;  // popped as r15 r14 r13 r12 rbx rbp
#endif
constexpr size_t kSlotR13 = 2, kSlotR12 = 3;

// void raw_switch(void** save_sp, void* load_sp)
__attribute__((naked, noinline)) RCOMP_HOST_FIBER_CC void rcomp_host_fiber_raw_switch(void**, void*) {
#if RCOMP_HOST_FIBER_MS_ABI
    __asm__ volatile(
        "pushq %rbp\n\t"
        "pushq %rbx\n\t"
        "pushq %rdi\n\t"
        "pushq %rsi\n\t"
        "pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "subq $176, %rsp\n\t"
        "movdqu %xmm6, 16(%rsp)\n\t"
        "movdqu %xmm7, 32(%rsp)\n\t"
        "movdqu %xmm8, 48(%rsp)\n\t"
        "movdqu %xmm9, 64(%rsp)\n\t"
        "movdqu %xmm10, 80(%rsp)\n\t"
        "movdqu %xmm11, 96(%rsp)\n\t"
        "movdqu %xmm12, 112(%rsp)\n\t"
        "movdqu %xmm13, 128(%rsp)\n\t"
        "movdqu %xmm14, 144(%rsp)\n\t"
        "movdqu %xmm15, 160(%rsp)\n\t"
        "stmxcsr 8(%rsp)\n\t"
        "fnstcw (%rsp)\n\t"
        "movq %rsp, (%rcx)\n\t"
        "movq %rdx, %rsp\n\t"
        "ldmxcsr 8(%rsp)\n\t"
        "fldcw (%rsp)\n\t"
        "movdqu 16(%rsp), %xmm6\n\t"
        "movdqu 32(%rsp), %xmm7\n\t"
        "movdqu 48(%rsp), %xmm8\n\t"
        "movdqu 64(%rsp), %xmm9\n\t"
        "movdqu 80(%rsp), %xmm10\n\t"
        "movdqu 96(%rsp), %xmm11\n\t"
        "movdqu 112(%rsp), %xmm12\n\t"
        "movdqu 128(%rsp), %xmm13\n\t"
        "movdqu 144(%rsp), %xmm14\n\t"
        "movdqu 160(%rsp), %xmm15\n\t"
        "addq $176, %rsp\n\t"
        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12\n\t"
        "popq %rsi\n\t"
        "popq %rdi\n\t"
        "popq %rbx\n\t"
        "popq %rbp\n\t"
        "ret\n\t");
#else
    __asm__ volatile(
        "pushq %rbp\n\t"
        "pushq %rbx\n\t"
        "pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "subq $16, %rsp\n\t"
        "stmxcsr 8(%rsp)\n\t"
        "fnstcw (%rsp)\n\t"
        "movq %rsp, (%rdi)\n\t"
        "movq %rsi, %rsp\n\t"
        "ldmxcsr 8(%rsp)\n\t"
        "fldcw (%rsp)\n\t"
        "addq $16, %rsp\n\t"
        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12\n\t"
        "popq %rbx\n\t"
        "popq %rbp\n\t"
        "ret\n\t");
#endif
}

// First "return" of a new context: r12 = HostFiber*, r13 = fiber_entry.
// rsp is 16-byte aligned here; fiber_entry never returns.
__attribute__((naked, noinline)) void rcomp_host_fiber_entry_thunk() {
#if RCOMP_HOST_FIBER_MS_ABI
    __asm__ volatile(
        "movq %r12, %rcx\n\t"
        "subq $32, %rsp\n\t"
        "callq *%r13\n\t"
        "ud2\n\t");
#else
    __asm__ volatile(
        "movq %r12, %rdi\n\t"
        "callq *%r13\n\t"
        "ud2\n\t");
#endif
}

// ---- registry ---------------------------------------------------------------------

constexpr size_t kGuardBytes = 0x10000;
constexpr size_t kMinHostStack = 512u << 10;
constexpr size_t kMaxHostStack = 8u << 20;  // a guest thread's own host stack (hle_xboxkrnl_threads.cpp)
constexpr size_t kHostStackPerGuestByte = 8;

std::mutex g_mu;
std::map<uint32_t, HostFiber*> g_suspended;  // guest r1 -> suspended context
std::atomic<size_t> g_live_secondary{0};

size_t round_up(size_t value, size_t align) { return (value + align - 1) & ~(align - 1); }

RCOMP_HOST_FIBER_CC void fiber_entry(HostFiber* self);

// Stack image popped by the first switch into a new context.
void* prepare_new_context(uint8_t* stack_top, HostFiber* fiber) {
    const uintptr_t after_ret = (reinterpret_cast<uintptr_t>(stack_top) & ~uintptr_t(15)) - 16;
    const uintptr_t sp = after_ret - 8 - 8 * kSwitchGprs - kSwitchScratch;
    uint8_t* p = reinterpret_cast<uint8_t*>(sp);
    memset(p, 0, after_ret - sp);
    // The new context starts with the creating thread's floating-point
    // control state (the guest FPSCR of the switch has already been applied).
    uint16_t x87_control = 0;
    __asm__ volatile("fnstcw %0" : "=m"(x87_control));
    const uint32_t mxcsr = __builtin_ia32_stmxcsr();
    memcpy(p + 0, &x87_control, sizeof x87_control);
    memcpy(p + 8, &mxcsr, sizeof mxcsr);
    uint64_t* gprs = reinterpret_cast<uint64_t*>(p + kSwitchScratch);
    gprs[kSlotR13] = reinterpret_cast<uint64_t>(&fiber_entry);
    gprs[kSlotR12] = reinterpret_cast<uint64_t>(fiber);
    gprs[kSwitchGprs] = reinterpret_cast<uint64_t>(&rcomp_host_fiber_entry_thunk);
    return p;
}

HostFiber* create_secondary(uint32_t guest_stack_bytes, PPCFunc* entry, uint32_t entry_address, uint8_t* base) {
    size_t stack = size_t(guest_stack_bytes) * kHostStackPerGuestByte;
    if (stack < kMinHostStack) stack = kMinHostStack;
    if (stack > kMaxHostStack) stack = kMaxHostStack;
    stack = round_up(stack, 0x10000);
    const size_t header = round_up(sizeof(HostFiber) + 64 + sizeof(PPCContext), 0x10000);
    const size_t total = kGuardBytes + stack + header;
    void* mapping = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "guest fiber host stack: mmap of %zu bytes failed (errno %d)", total, errno);
    if (mprotect(mapping, kGuardBytes, PROT_NONE) != 0) {
        const int error = errno;
        munmap(mapping, total);
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "guest fiber host stack: guard mprotect failed (errno %d)", error);
    }
    uint8_t* const stack_top = static_cast<uint8_t*>(mapping) + kGuardBytes + stack;
    HostFiber* fiber = new (stack_top) HostFiber();
    const uintptr_t context = round_up(reinterpret_cast<uintptr_t>(stack_top) + sizeof(HostFiber), 64);
    fiber->ctx = new (reinterpret_cast<void*>(context)) PPCContext();
    fiber->base = base;
    fiber->entry = entry;
    fiber->entry_address = entry_address;
    fiber->mapping = mapping;
    fiber->mapping_size = total;
    fiber->host_sp = prepare_new_context(stack_top, fiber);
    g_live_secondary.fetch_add(1, std::memory_order_relaxed);
    return fiber;
}

void release_secondary(HostFiber* fiber) {
    if (!fiber || fiber->primary) return;
    void* const mapping = fiber->mapping;
    const size_t size = fiber->mapping_size;
    if (munmap(mapping, size) != 0)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "guest fiber host stack: munmap failed (errno %d)", errno);
    g_live_secondary.fetch_sub(1, std::memory_order_relaxed);
}

HostFiberRun& run_or_die(const char* what) {
    HostFiberRun* run = current_host_fiber_run();
    if (!run) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s outside a guest thread run", what);
    return *run;
}

// Completes a switch on the context that now runs: publishes the context that
// was left (its host stack pointer is saved now) and releases a dead one.
void after_switch() {
    HostFiberRun& run = run_or_die("guest fiber switch");
    HostFiber* const previous = run.previous;
    HostFiber* const dead = run.dead;
    run.previous = nullptr;
    run.dead = nullptr;
    bool collision = false;
    if (previous) {
        std::lock_guard<std::mutex> lock(g_mu);
        collision = !g_suspended.emplace(previous->key, previous).second;
        if (!collision) previous->suspended = true;
    }
    if (collision)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "two suspended guest fibers share the stack pointer 0x%08X", previous->key);
    if (dead) release_secondary(dead);
}

RCOMP_HOST_FIBER_CC void fiber_entry(HostFiber* self) {
    after_switch();
    self->entry(*self->ctx, self->base);
    // The guest entered this function with `blr`: it has no caller to return
    // to (xapi's fiber start-up bugchecks before returning).
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                "guest fiber entry 0x%08X returned (r3=0x%08X lr=0x%08X): a fiber's first function has no caller",
                self->entry_address, self->ctx->r3.u32, uint32_t(self->ctx->lr));
}

}  // namespace

void host_fiber_transfer(PPCContext& ctx, uint8_t* base, uint32_t stack_pointer, uint32_t stack_base,
                         uint32_t stack_limit) {
    HostFiberRun& run = run_or_die("KeSetCurrentStackPointers");
    if (!run.current) {
        run.primary = HostFiber{};
        run.primary.primary = true;
        run.primary.run = &run;
        run.primary.ctx = &ctx;
        run.current = &run.primary;
    }
    HostFiber* const current = run.current;
    if (current->ctx != &ctx)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "KeSetCurrentStackPointers sp=0x%08X lr=0x%08X from a nested guest callback (its register file "
                    "is not the running fiber's)",
                    stack_pointer, uint32_t(ctx.lr));
    HostFiber* next = nullptr;
    bool foreign_primary = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        const auto it = g_suspended.find(stack_pointer);
        if (it != g_suspended.end()) {
            if (it->second->primary && it->second->run != &run) {
                foreign_primary = true;
            } else {
                next = it->second;
                next->suspended = false;
                g_suspended.erase(it);
            }
        }
    }
    if (foreign_primary)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "KeSetCurrentStackPointers sp=0x%08X lr=0x%08X: the target is another guest thread's own "
                    "suspended stack (fiber migration of a thread's original context)",
                    stack_pointer, uint32_t(ctx.lr));
    if (!next) {
        const uint32_t continuation = uint32_t(ctx.lr);
        PPCFunc* const entry = lookup_function(continuation);
        if (!entry)
            rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                        "xboxkrnl.exe!KeSetCurrentStackPointers sp=0x%08X lr=0x%08X r1=0x%08X: the continuation is "
                        "neither a suspended guest fiber nor a recompiled function entry",
                        stack_pointer, continuation, ctx.r1.u32);
        next = create_secondary(stack_base - stack_limit, entry, continuation, base);
    }
    current->key = ctx.r1.u32;
    run.previous = current;
    ctx.r1.u64 = stack_pointer;
    if (next->ctx != &ctx) *next->ctx = ctx;  // the full register file of the switch
    run.current = next;
    set_current_run_context(next->ctx);
    rcomp_host_fiber_raw_switch(&current->host_sp, next->host_sp);
    // Resumed, possibly on another host thread (a fiber resumed by another
    // guest thread): only the state of the run executing now is used.
    after_switch();
    HostFiberRun& now = run_or_die("guest fiber resume");
    if (now.exit_pending && now.current == &now.primary) {
        now.exit_pending = false;
        exit_current_guest_thread(now.exit_code);
    }
}

void host_fiber_stack_deleted(uint32_t stack_base, uint32_t stack_limit) {
    std::vector<HostFiber*> released;
    bool thread_context = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto it = g_suspended.lower_bound(stack_limit); it != g_suspended.end() && it->first <= stack_base;) {
            if (it->second->primary) {
                thread_context = true;
                ++it;
                continue;
            }
            it->second->suspended = false;
            released.push_back(it->second);
            it = g_suspended.erase(it);
        }
    }
    if (thread_context)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,
                    "MmDeleteKernelStack [0x%08X,0x%08X) holds the suspended stack of a guest thread", stack_limit,
                    stack_base);
    for (HostFiber* fiber : released) release_secondary(fiber);
}

bool host_fiber_exit_to_primary(uint32_t exit_code) {
    HostFiberRun* run = current_host_fiber_run();
    if (!run || !run->current || run->current->primary) return false;
    HostFiber* const current = run->current;
    HostFiber* const primary = &run->primary;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        const auto it = g_suspended.find(primary->key);
        if (it != g_suspended.end() && it->second == primary) {
            g_suspended.erase(it);
            primary->suspended = false;
            found = true;
        }
    }
    if (!found)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "guest thread %u exits on a fiber but its own context is not suspended",
                    run->thread_id);
    run->previous = nullptr;
    run->dead = current;
    run->exit_pending = true;
    run->exit_code = exit_code;
    run->current = primary;
    set_current_run_context(primary->ctx);
    rcomp_host_fiber_raw_switch(&current->host_sp, primary->host_sp);
    rcomp_fatal(RCOMP_FATAL_INTERNAL, "an exited guest fiber was resumed");
}

void host_fiber_run_ended(HostFiberRun& run) {
    HostFiber* const abandoned = run.current && !run.current->primary ? run.current : nullptr;
    HostFiber* const dead = run.dead;
    HostFiber* const previous = run.previous;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (run.primary.suspended) {
            const auto it = g_suspended.find(run.primary.key);
            if (it != g_suspended.end() && it->second == &run.primary) g_suspended.erase(it);
        }
        // A context left by a switch whose completion never ran (a fatal hook
        // ended the run in between) is a valid suspended fiber.
        if (previous && !previous->primary && g_suspended.emplace(previous->key, previous).second)
            previous->suspended = true;
    }
    if (abandoned && abandoned != dead) release_secondary(abandoned);  // frames ended by a fatal hook
    if (dead) release_secondary(dead);
    const uint32_t thread_id = run.thread_id;
    run = HostFiberRun{};
    run.thread_id = thread_id;
}

void host_fibers_shutdown() {
    std::vector<HostFiber*> released;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& item : g_suspended) {
            item.second->suspended = false;
            if (!item.second->primary) released.push_back(item.second);
        }
        g_suspended.clear();
    }
    for (HostFiber* fiber : released) release_secondary(fiber);
}

size_t host_fiber_suspended_count() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_suspended.size();
}

size_t host_fiber_live_secondary_count() { return g_live_secondary.load(std::memory_order_relaxed); }

}  // namespace rcomp::rt
