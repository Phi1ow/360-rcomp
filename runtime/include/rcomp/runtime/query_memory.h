// Read-only Xbox virtual-memory query helpers for the AOT runtime.
#pragma once

#include <stdint.h>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Xbox 360 / 32-bit NT MEMORY_BASIC_INFORMATION layout used by
// NtQueryVirtualMemory region type 0. All fields are emitted big-endian to
// guest memory by the HLE wrapper; this host-side structure is native-endian.
struct MemoryBasicInformation32 {
    uint32_t base_address = 0;
    uint32_t allocation_base = 0;
    uint32_t allocation_protect = 0;
    uint32_t region_size = 0;
    uint32_t state = 0;
    uint32_t protect = 0;
    uint32_t type = 0;
};
static_assert(sizeof(MemoryBasicInformation32) == 28,
              "Xbox MEMORY_BASIC_INFORMATION must remain seven 32-bit fields");

enum class MemoryQueryProvenance : uint8_t {
    Free = 0,
    VirtualAllocation,
    PhysicalAllocation,
    MainImage,
};

struct MemoryQueryResult {
    MemoryBasicInformation32 basic{};
    MemoryQueryProvenance provenance = MemoryQueryProvenance::Free;
    // True when basic.protect is the complete Xbox PAGE_* value, including
    // cache/write-combine modifiers. Nt/Mm allocations get this from
    // GuestHeap-owned provenance; image/free results are complete by contract.
    bool protection_modifiers_known = false;
};

// Queries only memory whose guest provenance is established by current R-comp
// state: NtAllocateVirtualMemory allocations, physical allocations (including
// 4-KiB allocation boundaries), the finalized main image, or allocator-owned /
// genuinely unbacked free guest pages. Live runtime heap allocations without
// VM provenance return Unsupported rather than being misreported as title VM.
Status query_memory_basic(uint32_t address, MemoryQueryResult* out);

// Returns the exact Xbox PAGE_* value when provenance is known. For an
// unsupported raw committed page it returns only GuestMemory's base access and
// sets *modifiers_known=false; production MmQueryAddressProtect never treats
// that partial observation as a successful complete answer.
uint32_t query_address_protect(uint32_t address, bool* modifiers_known = nullptr);

// Registers NtQueryVirtualMemory (0xEE) and MmQueryAddressProtect (0xC4).
Status register_xboxkrnl_query_memory_hle();

}  // namespace rcomp::rt
