// Shared interface (owner: PRIME). Process-wide runtime state accessors.
#pragma once

namespace rcomp {
class GuestMemory;
void set_active_guest_memory(GuestMemory* mem);
GuestMemory* active_guest_memory();
// Guest loads and stores inside the coarse range of include/rcomp/ppc_prelude.h that no provider window
// claimed, since start (ordinary heap accesses taking the out-of-line path); 0 without virtual access.
uint64_t virtual_window_fallback_count();
}  // namespace rcomp
