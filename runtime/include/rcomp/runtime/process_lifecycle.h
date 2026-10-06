// Xbox title process/lifecycle services implemented by the isolated process
// tranche. The aggregate xboxkrnl/xam registrars wire these in after the
// services candidate freeze is released.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rcomp/func_table.h"
#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Public Xbox process type values used by KeGetCurrentProcessType.
constexpr uint32_t kProcessTypeIdle = 0;
constexpr uint32_t kProcessTypeUser = 1;
constexpr uint32_t kProcessTypeSystem = 2;

enum class TitleTerminationReason : uint32_t {
    XamLoader = 1,
    HalRebootToDashboard = 2,
    XamLoaderLaunchDashboard = 3,  // XamLoaderLaunchTitle(NULL)
};

enum class TitleLifecyclePhase : uint32_t {
    Running = 0,
    TerminationRequested,
    CallbacksRunning,
    CallbacksComplete,
};

struct TitleLifecycleSnapshot {
    uint64_t generation = 0;
    TitleLifecyclePhase phase = TitleLifecyclePhase::Running;
    TitleTerminationReason reason = TitleTerminationReason::XamLoader;
    uint32_t terminal_code = 0;
    size_t registered_callbacks = 0;
};

// Termination has a non-blocking request phase while a guest thread is still
// executing. The runtime owner later performs the existing blocking finish
// phase after that guest entry has unwound. PRIME wires this to the threading
// dispatcher once its frozen sources are released.
using TitleQuiesceRequestFn = void (*)();
Status configure_title_lifecycle_quiesce_request(TitleQuiesceRequestFn request);

// Performs the request phase and all registered guest callbacks. It never
// exits the current GuestThread itself; callers may safely invoke
// exit_current_guest_thread only after this function has returned and all of
// its C++ automatic objects have been destroyed. AlreadyExists means a nested
// termination request observed the first request and should return to that
// outer request rather than starting another unwind.
Status request_title_termination(PPCContext& ctx, uint8_t* base,
                                 TitleTerminationReason reason, uint32_t terminal_code,
                                 uint32_t* effective_terminal_code);

Status title_lifecycle_snapshot(TitleLifecycleSnapshot* out);
// Owner-only cleanup after all guest producers have stopped.
void reset_title_lifecycle();

// Test/diagnostic query of the host-side APC disable count for the active
// GuestThread. The Xbox critical-region calls use negative nesting with zero as
// the enabled state, matching the public KTHREAD behavior without fabricating
// a guest KTHREAD layout.
Status current_guest_apc_disable_count(int32_t* out);
void title_lifecycle_thread_exited(uint32_t thread_id);

Status register_xboxkrnl_process_hle();
Status register_xam_loader_hle();

}  // namespace rcomp::rt
