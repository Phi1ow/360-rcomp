// Host contexts for guest fibers (owner: Agent 3, runtime/). Internal header.
//
// Xbox 360 xapi fibers (ConvertThreadToFiber / CreateFiber / SwitchToFiber,
// statically linked into titles) save and restore the guest registers in the
// guest fiber block themselves and then tail-call the kernel export
// KeSetCurrentStackPointers(StackPointer, Thread, StackAllocBase, StackBase,
// StackLimit), which loads r1 and returns to LR: the target fiber's saved LR,
// or, for a fiber that never ran, the entry CreateFiber wrote. In recompiled
// C++ a guest `blr` is a host `return`, so that return must happen on the
// host stack that holds the target fiber's C++ frames. Each guest fiber
// therefore gets a host context of its own; KeSetCurrentStackPointers
// switches host contexts (runtime/docs/THREAD_OBJECTS.md ("Guest fibers")).
//
// Identification is generic and needs no title address:
//  * a suspended host context is keyed by the guest r1 it had when it
//    suspended (the value SwitchToFiber saved in its fiber block, which the
//    switch back passes as StackPointer). Guest stacks are disjoint, so a key
//    names one context;
//  * a StackPointer that names no suspended context starts a new host
//    context, which calls the recompiled function at LR (the CreateFiber
//    entry). Neither: explicit RCOMP_FATAL_UNIMPLEMENTED.
// Every host context has its own PPCContext; the full register file is copied
// into the target's on each switch, so a fiber may resume on another guest
// thread (r13 and the thread state are the resuming thread's).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rcomp/func_table.h"
#include "rcomp/hle.h"

namespace rcomp::rt {

struct HostFiberRun;

struct HostFiber {
    void* host_sp = nullptr;      // saved host stack pointer while suspended
    PPCContext* ctx = nullptr;    // the PPCContext the context's frames use
    uint8_t* base = nullptr;      // guest memory base passed to the entry
    PPCFunc* entry = nullptr;     // recompiled entry (new contexts only)
    uint32_t entry_address = 0;   // its guest address (diagnostics)
    uint32_t key = 0;             // guest r1 while suspended (registry key)
    bool suspended = false;       // present in the suspended-context registry
    bool primary = false;         // the host thread's own stack (run root)
    HostFiberRun* run = nullptr;  // primary only: its run
    void* mapping = nullptr;      // secondary: mmap block (guard + stack + this header)
    size_t mapping_size = 0;
};

// Per active guest run (one host thread inside run_guest_thread /
// run_guest_callback). Lives in guest_thread.cpp's RunState.
struct HostFiberRun {
    HostFiber primary;
    HostFiber* current = nullptr;   // nullptr: the run never switched (primary)
    HostFiber* previous = nullptr;  // context left by the last switch, published after it
    HostFiber* dead = nullptr;      // secondary context to release after the switch
    bool exit_pending = false;      // resume of the primary must end the thread
    uint32_t exit_code = 0;
    uint32_t thread_id = 0;
};

// Implemented in guest_thread.cpp: the calling host thread's active run, or
// nullptr outside run_guest_thread/run_guest_callback.
HostFiberRun* current_host_fiber_run();
// Implemented in guest_thread.cpp: makes `ctx` the run's registered
// register file (profiling registration and its final unregistration).
void set_current_run_context(PPCContext* ctx);

// KeSetCurrentStackPointers continuation when StackPointer != r1: switches
// to the host context of the guest continuation (see above). Returns when
// the calling context is resumed. `ctx` must be the calling context's own.
void host_fiber_transfer(PPCContext& ctx, uint8_t* base, uint32_t stack_pointer, uint32_t stack_base,
                         uint32_t stack_limit);
// MmDeleteKernelStack: releases the suspended host contexts whose guest r1
// lies in [stack_limit, stack_base] (a fiber deleted while suspended).
void host_fiber_stack_deleted(uint32_t stack_base, uint32_t stack_limit);
// exit_current_guest_thread while a secondary context runs: switches to the
// run's primary context, which completes the exit there. False when the
// calling context is the primary (the caller exits directly).
bool host_fiber_exit_to_primary(uint32_t exit_code);
// run_guest_* epilogue: releases a context abandoned by a fatal hook and
// resets the run's state.
void host_fiber_run_ended(HostFiberRun& run);
// runtime_shutdown: releases every suspended secondary context.
void host_fibers_shutdown();
// Diagnostics/tests.
size_t host_fiber_suspended_count();
size_t host_fiber_live_secondary_count();

}  // namespace rcomp::rt
