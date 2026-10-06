// Cooperative NtSuspendThread delivery (internal to runtime/src).
//
// A suspended ExCreateThread worker stops at its next import dispatch: every
// dispatch reads the calling thread's suspend-request flag (one thread-local
// load and one relaxed atomic load) and, when it is set, blocks in
// runtime_suspend_checkpoint() until the suspend count is back to zero. The
// authoritative count and the blocking live under the dispatcher lock in
// hle_xboxkrnl_threads.cpp. See runtime/docs/SUSPEND_PULSE.md.
#pragma once

#include <atomic>
#include <stdint.h>

namespace rcomp::rt {

// Publishes the flag the calling host thread's dispatches poll (nullptr: none).
// Set by the worker itself around its guest code.
void set_current_thread_suspend_request(const std::atomic<uint32_t>* flag);
// Blocks the calling worker while its suspend count is nonzero. When runtime
// quiescence is requested meanwhile, the guest thread exits with
// STATUS_THREAD_IS_TERMINATING (it unwinds only POD guest frames).
void runtime_suspend_checkpoint();

}  // namespace rcomp::rt
