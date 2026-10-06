// Guest heap allocator over rcomp::GuestMemory (owner: Agent 3, runtime/).
//
// Manages one guest address region [lo, hi) (64 KiB aligned). Allocation
// metadata lives on the host (std::map), never in guest memory, so a guest
// buffer overrun cannot corrupt the allocator.
//
//  * First-fit over a coalescing free list; sizes rounded to 16 bytes;
//    alignment is any power of two (minimum 16).
//  * Pages are committed on demand (64 KiB granularity, Protect::ReadWrite) the
//    first time an allocation touches them. Pages are never decommitted by
//    free() (simple, deterministic; GuestMemory::stats() has the peak).
//  * Pages that overlap a GuestMemory runtime range (reserve_runtime_range,
//    e.g. the XenonRecomp function table) at init() are removed from the free
//    list; a runtime range added later makes the commit fail with
//    Status::Conflict rather than handing the range out.
//  * free() of an address that was never returned -> NotAllocated; of an
//    address already freed (still inside a free block) -> DoubleFree.
//  * Exhaustion -> Status::OutOfMemory, *out untouched.
//  * Already committed pages must be ReadWrite; allocation refuses protected
//    pages with Conflict without changing their rights or allocation ownership.
//  * Bookkeeping nodes are prepared before guest mapping/free-list mutation.
//    Host builds with exceptions return OutOfMemory on metadata bad_alloc;
//    -fno-exceptions builds terminate on host OOM. A partial platform commit
//    failure may retain extra committed pages, but never grants an allocation.
//
// Thread-safe (one mutex). All commits to the region must go through the heap
// because GuestMemory's commit bookkeeping is not itself synchronized.
#pragma once

#include <stdint.h>

#include <map>
#include <mutex>
#include <set>

#include "rcomp/runtime/status.h"

namespace rcomp {
class GuestMemory;
enum class Protect : uint32_t;
}

namespace rcomp::rt {

struct GuestHeapStats {
    uint64_t region_bytes;
    uint64_t allocated_bytes;  // sum of live allocation sizes (rounded)
    uint64_t free_bytes;       // sum of free-list block sizes
    uint32_t live_allocations;
    uint32_t excluded_pages;   // pages removed because of runtime ranges
    // Bytes of reserve-only VM allocations (reserve_in) that currently have no
    // backing: allocated_bytes - reserved_unbacked_bytes is what is backed.
    uint64_t reserved_unbacked_bytes;
};

struct GuestHeapRegionInfo {
    bool allocated = false;
    uint32_t base = 0;
    uint32_t size = 0;
};

struct GuestHeapProtectionInfo {
    uint32_t allocation_base = 0;
    uint32_t allocation_size = 0;
    uint32_t page_size = 0;
    uint32_t allocation_protect = 0;
    uint32_t region_base = 0;
    uint32_t region_size = 0;
    uint32_t protect = 0;
};

class GuestHeap {
public:
    // Default region: the Xbox 360 64 KiB-page virtual range used by titles
    // for NtAllocateVirtualMemory (0x40000000..0x7F000000); away from the XEX
    // image (0x82000000+) and its function table.
    static constexpr uint32_t kDefaultLo = 0x40000000u;
    static constexpr uint32_t kDefaultHi = 0x7F000000u;
    // The whole title virtual range of the Xbox 360 kernel: 4 KiB-page allocations live in
    // 0x00000000..0x3FFFFFFF (the first 64 KiB stay unmapped), 64 KiB-page ones in 0x40000000.. .
    // Titles rely on it: Unreal Engine 3's allocator frees a block below 0x40000000 as a direct
    // VirtualAlloc and looks anything above up in its own 64 KiB page table (Gears of War 2).
    static constexpr uint32_t kSmallPageLo = 0x00010000u;
    static constexpr uint32_t kLargePageLo = 0x40000000u;
    static constexpr uint32_t kMinAlign = 16;

    Status init(GuestMemory* mem, uint32_t lo = kDefaultLo, uint32_t hi = kDefaultHi);
    void reset();  // forgets all allocations (does not decommit)

    // size > 0; align power of two (0 = kMinAlign). zero: fill with 0. Runtime-owned blocks: when
    // the heap spans kLargePageLo, they stay at or above it (the 64 KiB-page range), leaving the
    // 4 KiB-page range below to the title's NtAllocateVirtualMemory.
    Status alloc(uint32_t size, uint32_t align, bool zero, uint32_t* out);
    // Same, but the block must lie inside [lo, hi) (hi is exclusive, clipped
    // to the heap region). top_down: highest fitting address instead of the
    // lowest.
    Status alloc_in(uint32_t size, uint32_t align, uint64_t lo, uint64_t hi, bool top_down, bool zero,
                    uint32_t* out);
    // Guest VM reservation: owns whole pages without backing or access. Freed
    // cached pages are decommitted before this reservation is published.
    Status reserve_in(uint32_t size, uint32_t align, uint64_t lo, uint64_t hi,
                      bool top_down, uint32_t* out);
    // Commit a bounded part of an existing tracked VM allocation. Existing
    // committed bytes survive; only newly committed pages are initialized.
    Status commit_guest_range(uint32_t addr, uint32_t size, uint32_t protect,
                              rcomp::Protect host_protect, bool zero);
    Status decommit_guest_range(uint32_t addr, uint32_t size);
    Status free(uint32_t addr);
    // Size (rounded) of the live allocation starting at addr.
    Status allocation_size(uint32_t addr, uint32_t* out) const;
    // Finds the live allocation containing `addr`, not just an exact base.
    Status allocation_containing(uint32_t addr, uint32_t* base, uint32_t* size) const;
    // Returns the allocator-owned region containing addr. `allocated=false`
    // means an actual GuestHeap free block even if its host pages remain
    // committed from a previous allocation. Runtime-excluded holes return
    // Conflict rather than being misclassified as free.
    Status region_containing(uint32_t addr, GuestHeapRegionInfo* out) const;

    // Marks a live allocation as guest virtual/physical memory and preserves
    // its exact Xbox PAGE_* flags (including cache/write-combine modifiers).
    // The allocation must already have matching host access rights.
    Status set_guest_protection(uint32_t allocation_base, uint32_t protect,
                                uint32_t guest_page_size);
    // Changes a page-aligned subrange of one tracked guest allocation. Host
    // access rights are changed through GuestMemory at the same time; metadata
    // is published only when that succeeds. `old_protect` receives the exact
    // flags at the first byte of the range when non-null.
    Status protect_guest_range(uint32_t addr, uint32_t size, uint32_t protect,
                               rcomp::Protect host_protect, uint32_t* old_protect = nullptr);
    // Exact allocation + current protection run for one tracked guest address.
    Status query_guest_protection(uint32_t addr, GuestHeapProtectionInfo* out) const;
    // Makes [addr, addr+size) inside a live allocation inaccessible / accessible
    // again (used for stack guard pages). Page-aligned only.
    Status set_guard(uint32_t addr, uint32_t size, bool guard);

    GuestHeapStats stats() const;
    GuestMemory* memory() const { return mem_; }
    uint32_t lo() const { return lo_; }
    uint32_t runtime_lo() const { return alloc_lo_; }
    uint32_t hi() const { return hi_; }

private:
    Status commit_range(uint64_t addr, uint64_t size);
    Status allocate_range(uint32_t size, uint32_t align, uint64_t lo, uint64_t hi,
                          bool top_down, bool zero, bool commit, uint32_t* out);
    Status change_guest_range(uint32_t addr, uint32_t size, uint32_t protect,
                              rcomp::Protect host_protect, uint32_t* old_protect,
                              bool commit, bool zero);

    struct GuestProtectionSpan { uint32_t size = 0, protect = 0; };

    std::map<uint32_t, uint32_t> guards_;  // guard addr -> size (inside a live allocation)
    std::set<uint32_t> freed_starts_;      // starts of freed, not re-allocated blocks
    std::set<uint32_t> vm_reservations_;   // whole-page reservations decommitted on release

    mutable std::mutex mu_;
    GuestMemory* mem_ = nullptr;
    uint32_t lo_ = 0, hi_ = 0;
    uint32_t alloc_lo_ = 0;  // lower bound of alloc() (see alloc)
    std::map<uint32_t, uint32_t> free_;  // addr -> size
    std::map<uint32_t, uint32_t> used_;  // addr -> size
    std::map<uint32_t, uint32_t> guest_allocation_protect_;  // allocation base -> initial PAGE_*
    std::map<uint32_t, uint32_t> guest_page_size_;  // allocation base -> Xbox page granularity
    std::map<uint32_t, GuestProtectionSpan> guest_protection_;  // region base -> size/current PAGE_*
    uint64_t allocated_bytes_ = 0;
    uint32_t excluded_pages_ = 0;
};

}  // namespace rcomp::rt
