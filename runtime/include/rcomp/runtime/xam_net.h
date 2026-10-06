// xam.xex NetDll networking HLE integration points (owner: network lot).
#pragma once

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Registers the concrete NetDll Winsock imports implemented by
// runtime/src/hle_xam_net.cpp. Safe to call once per process registry setup.
Status register_xam_net_hle();

// Phase 1 of title teardown. Invalidates guest socket handles, rejects new
// startups/operations and interrupts blocking native calls, but does not close
// native descriptors or terminate the platform network service. Call before
// waiting for guest workers to quiesce.
Status begin_shutdown_xam_net_hle();

// Phase 2. Call only after guest/external producers are quiesced. Never waits
// for guest work: returns Conflict if an in-flight lease remains, otherwise
// closes descriptors and terminates the platform backend.
Status shutdown_xam_net_hle();

}  // namespace rcomp::rt
