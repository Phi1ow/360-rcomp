// Xbox LIVE system dialogs answered for the local offline profile (owner: Agent 3, runtime/).
// See src/hle_xam_live_dialogs.cpp.
#pragma once

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Registers XamShowFriendsUI and XamShowMessageComposeUI (called by register_xam_hle).
Status register_xam_live_dialogs_hle();

}  // namespace rcomp::rt
