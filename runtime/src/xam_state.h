#pragma once

#include <mutex>
#include <set>

#include "rcomp/input.h"
#include "rcomp/runtime/xam.h"

namespace rcomp::rt {

// Lifetime-owned by Runtime. Keeping allocations, configuration, and input
// packet history here prevents a later title from inheriting stale XAM state.
struct XamState {
    std::mutex mutex;
    bool configured = false;
    XamRuntimeConfig config{};
    std::set<uint32_t> allocations;
    rcomp_pad last_pad[4]{};
    uint32_t packet[4]{};
};

}  // namespace rcomp::rt
