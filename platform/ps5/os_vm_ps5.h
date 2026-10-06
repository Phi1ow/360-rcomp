// PS5-only extensions of the os_vm backend (owner: Agent 2). Not part of the
// shared interface; used by platform tests and the PS5 test title.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rcomp::os::ps5 {

// Largest direct-memory allocation vm_commit makes for one contiguous run of
// pages (default: unlimited). 64 KiB = one allocation per guest page, which
// avoids relying on sceKernelReleaseDirectMemory accepting sub-ranges.
void set_max_run_bytes(size_t bytes);

// Direct memory currently backing committed pages of all reservations.
uint64_t direct_bytes_held();

}  // namespace rcomp::os::ps5
