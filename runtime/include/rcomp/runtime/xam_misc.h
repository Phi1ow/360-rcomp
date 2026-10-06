// XAM tasks, sessions, system UI, voice, QoS and XNetLogon queries for a console whose only profile is
// signed in locally, not to Xbox LIVE (owner: Agent 3, runtime/). See src/hle_xam_misc.cpp.
#pragma once

#include <stdint.h>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Registers the imports of src/hle_xam_misc.cpp (called by register_xam_hle).
Status register_xam_misc_hle();
// Registers the Guide dialogs of src/hle_xam_ui_more.cpp (called by register_xam_hle).
Status register_xam_ui_more_hle();

// ObDereferenceObject on a session object Body returned by XamSessionRefObjByHandle. Status::NotFound
// when `body` is not a live session Body of the current Runtime (the caller then tries the other object
// kinds), Status::Conflict when the Body holds no reference.
Status dereference_session_body(uint32_t body);

}  // namespace rcomp::rt
