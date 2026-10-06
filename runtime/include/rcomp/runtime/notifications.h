#pragma once
#include <cstdint>
#include "rcomp/runtime/status.h"

namespace rcomp::rt {
Status register_xam_notifications_hle();
// Publish an actual backend transition. Category=bits25..30; version=bits16..24.
// Does not manufacture sign-in, UI or connectivity events at listener creation.
Status publish_xam_notification(uint32_t id, uint32_t parameter);
void reset_xam_notifications();
// Call after releasing the producer's queue/state locks.
void notify_runtime_waiters();
}
