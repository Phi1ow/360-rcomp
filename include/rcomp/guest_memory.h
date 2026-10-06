// Shared interface (owner: PRIME; implementation: platform/, Agent 2).
//
// Guest memory contract used by XenonRecomp-generated code.
//
// The generated code addresses guest memory as `base + (uint32_t)ea`
// (ppc_context.h: PPC_LOAD_*/PPC_STORE_*). Effective addresses are computed in
// 32-bit unsigned arithmetic, so they wrap modulo 2^32, but a multi-byte access
// that starts near the top of the space may touch up to 15 bytes past 4 GiB
// (16-byte vector access at 0xFFFFFFF0 is the worst in-range case, a 64-bit
// load at 0xFFFFFFFC overruns by 4).
//
// Layout of the host reservation (all offsets relative to base()):
//
//   [0, 4 GiB)                 guest address space. Reserved PROT_NONE, pages
//                              committed on demand via commit().
//   [4 GiB, 4 GiB + guard)     guard, never committed. Catches overruns.
//
// The XenonRecomp indirect-call table (PPC_LOOKUP_FUNC) lives *inside* the
// guest range at PPC_IMAGE_BASE + PPC_IMAGE_SIZE, sized PPC_CODE_SIZE * 2
// bytes. The runtime must commit it and keep the guest allocator away from it
// (see reserve_runtime_range()). Reserving exactly 4 GiB is therefore not the
// whole contract: the table is runtime metadata stored in guest space.
//
// Physical memory: reserve() only takes virtual address space. Memory is only
// backed once committed, and on PS5 the backing (flexible vs direct memory) is
// a platform decision documented in platform/ps5/README.md. Nothing here may
// allocate multiple GiB of physical memory to mirror the 32-bit space.
//
// No mapping created through this interface is ever executable.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rcomp {

constexpr uint64_t kGuestSpaceSize = 0x100000000ull;  // 4 GiB
constexpr uint64_t kGuestGuardSize = 0x10000ull;      // 64 KiB guard after 4 GiB
constexpr uint64_t kGuestPageSize = 0x10000ull;       // commit granularity (64 KiB)
constexpr uint64_t kBaseAlignment = 0x10000ull;       // PPC_FUNC_PROLOGUE needs >= 32

// Xbox 360 physical memory (512 MiB) is visible through three CPU windows that
// alias the same bytes: 0xA0000000 (64 KiB pages), 0xC0000000 (16 MiB pages)
// and 0xE0000000 (4 KiB pages). The runtime allocates in the first; commit()
// there also maps the second and third views where the platform can alias
// (PS5), so a title that reaches physical memory through any window sees the
// same data. Alias windows cannot be committed/decommitted/protected directly.
constexpr uint64_t kPhysicalWindowBase = 0xA0000000ull;
constexpr uint64_t kPhysicalWindowSize = 0x20000000ull;
constexpr uint64_t kPhysicalAliasBases[2] = {0xC0000000ull, 0xE0000000ull};

enum class Protect : uint32_t {
    None = 0,
    Read = 1,
    ReadWrite = 3,
    // Deliberately no Execute: AOT code is linked into the title.
};

enum class MemStatus : int32_t {
    Ok = 0,
    InvalidArgument = -1,  // unaligned, zero size, or range outside the guest space
    OutOfMemory = -2,      // platform refused to reserve/commit
    NotReserved = -3,      // object not initialised or already released
    Conflict = -4,         // overlaps a runtime-reserved range
    PlatformError = -5,    // unexpected platform failure (errno logged)
};

const char* mem_status_name(MemStatus s);

struct GuestMemoryStats {
    uint64_t reserved_bytes;   // virtual, including guard
    uint64_t committed_bytes;  // currently committed guest bytes
    uint64_t peak_committed_bytes;
};

class GuestMemory {
public:
    GuestMemory() = default;
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // Reserve 4 GiB + guard of virtual space, aligned to kBaseAlignment.
    MemStatus reserve();
    // Release everything (idempotent).
    void release();

    bool reserved() const { return base_ != nullptr; }
    uint8_t* base() const { return base_; }

    // Xbox 360 4 KiB-page physical window shift (option of the title build, off by default): the
    // console maps virtual 0xE0000000 + X to physical X + 0x1000. When on, guest addresses at or above
    // kPhysical4KWindow reach host base + address + 0x1000 (the generated code does the same with
    // RCOMP_PHYSICAL_4K_WINDOW_OFFSET), so they land on the zero-offset 0xE0000000 alias of
    // physical X + 0x1000. translate(), is_accessible() and host() apply it; is_committed() and the
    // mapping calls (commit/decommit/protect) take host-side addresses and do not.
    static constexpr uint64_t kPhysical4KWindow = 0xE0000000ull;
    void set_physical_4k_offset(bool on) { physical_4k_offset_ = on; }
    bool physical_4k_offset() const { return physical_4k_offset_; }
    uint64_t host_address(uint64_t guest) const {
        return guest + ((physical_4k_offset_ && guest >= kPhysical4KWindow) ? 0x1000u : 0u);
    }
    // Host pointer of a guest address (no access check; see translate() for a checked one).
    uint8_t* host(uint32_t guest) const { return base_ + host_address(guest); }

    // Guest range [addr, addr + size). addr and size must be multiples of
    // kGuestPageSize and the range must lie within [0, 4 GiB).
    MemStatus commit(uint64_t addr, uint64_t size, Protect prot);
    // Returns pages to the platform; contents are lost, range stays reserved.
    MemStatus decommit(uint64_t addr, uint64_t size);
    MemStatus protect(uint64_t addr, uint64_t size, Protect prot);
    // True if every page of [addr, addr+size) is committed (any size/alignment).
    bool is_committed(uint64_t addr, uint64_t size) const;
    // True if the entire committed range allows the requested Read/ReadWrite
    // access. None and invalid protection values are rejected. As with the
    // other mapping operations, callers serialize changes to page mappings;
    // this query does not pin a mapping against a concurrent protect/decommit.
    bool is_accessible(uint64_t addr, uint64_t size, Protect required) const;

    // Marks a range as owned by the runtime (e.g. the function table) and
    // commits it read-write if needed. Later commit()/decommit()/protect()
    // calls overlapping it fail with Conflict; runtime ranges live until
    // release(). Not thread-safe: callers serialise (see runtime heap).
    MemStatus reserve_runtime_range(uint64_t addr, uint64_t size, const char* tag);
    // Reserve addresses for opaque HLE identities without committing any page.
    // Guest loads/stores fault; commit/decommit/protect cannot claim the range.
    // Repeating the exact opaque range and tag is idempotent while this memory
    // reservation lives. A committed page or any other ownership conflicts.
    MemStatus reserve_opaque_runtime_range(uint64_t addr, uint64_t size, const char* tag);
    bool overlaps_runtime_range(uint64_t addr, uint64_t size) const;

    // Checked accessors for runtime/HLE code (never used by generated code):
    // return nullptr if [addr, addr+size) is not fully committed.
    uint8_t* translate(uint32_t addr, uint32_t size) const;

    GuestMemoryStats stats() const;

private:
    MemStatus commit_impl(uint64_t addr, uint64_t size, Protect prot);
    // Physical-window aliasing (see kPhysicalAliasBases).
    MemStatus alias_range(uint64_t addr, uint64_t size, Protect prot);
    void unalias_range(uint64_t addr, uint64_t size);
    MemStatus protect_alias_range(uint64_t addr, uint64_t size, Protect prot);
    bool in_alias_window(uint64_t addr, uint64_t size) const;
    bool aliasing_supported_ = true;
    bool physical_4k_offset_ = false;
    uint8_t* base_ = nullptr;
    uint64_t reserve_size_ = 0;
    // One bit per 64 KiB page: 65536 pages -> 8 KiB bitmap.
    uint64_t committed_bits_[65536 / 64] = {};
    uint64_t readable_bits_[65536 / 64] = {};
    uint64_t writable_bits_[65536 / 64] = {};
    struct RuntimeRange { uint64_t addr, size; const char* tag; bool opaque = false; };
    RuntimeRange runtime_ranges_[16] = {};
    int runtime_range_count_ = 0;
    uint64_t committed_bytes_ = 0;
    uint64_t peak_committed_bytes_ = 0;
};

}  // namespace rcomp
