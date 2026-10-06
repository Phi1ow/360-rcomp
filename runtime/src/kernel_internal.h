// Runtime-internal hooks shared by the xboxkrnl HLE translation units
// (owner: Agent 3, runtime/). Not a guest ABI and not part of include/.
#pragma once

#include <stdint.h>

#include "rcomp/runtime/status.h"

struct PPCContext;

namespace rcomp::rt {

// Registration of the export groups of src/hle_xboxkrnl_seh.cpp,
// src/hle_xboxkrnl_more.cpp and src/hle_xboxkrnl_dpc.cpp (called by
// register_xboxkrnl_hle).
Status register_xboxkrnl_seh_hle();
Status register_xboxkrnl_more_hle();
Status register_xboxkrnl_dpc_hle();

// ObOpenObjectByPointer (src/hle_xboxkrnl_objects.cpp): a new handle in the
// runtime handle table to the worker thread whose Body is `body`, exactly as
// NtDuplicateObject would create one. NotFound when no worker HandleObject owns
// that Body (bootstrap thread, interrupt/DPC callback contexts, exited and
// released workers); TableFull when the table is full.
Status open_thread_handle_for_body(uint32_t body, uint32_t* handle);

// DPC worker (src/hle_xboxkrnl_dpc.cpp). Stops and joins the worker and
// releases its guest stack; called by runtime_shutdown after the managed guest
// threads have quiesced and before the heap is destroyed. Idempotent.
void shutdown_dpc_worker();

// Calls a guest routine synchronously on the calling guest thread's own
// context (APC normal routines): r3..r5 = a0..a2, r1/lr/r13 preserved. The
// routine must exist in the AOT function table (RCOMP_FATAL_INDIRECT_TARGET).
void call_guest_routine_on_current_context(PPCContext& ctx, uint8_t* base, uint32_t routine, uint32_t a0,
                                           uint32_t a1, uint32_t a2, const char* what);

}  // namespace rcomp::rt
