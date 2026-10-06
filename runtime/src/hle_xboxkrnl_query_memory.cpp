// Read-only virtual-memory queries backed by R-comp guest state.
// Contracts and limitations: runtime/docs/QUERY_MEMORY.md.
#include <cstdio>
#include "physical_window.h"
#include "diagnostics.h"
#include <atomic>
#include <algorithm>

#include "module_state.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/query_memory.h"
#include "rcomp/runtime/runtime.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kMemCommit = 0x00001000u;
constexpr uint32_t kMemReserve = 0x00002000u;
constexpr uint32_t kMemFree = 0x00010000u;
constexpr uint32_t kMemPrivate = 0x00020000u;
constexpr uint32_t kMemImage = 0x01000000u;

constexpr uint32_t kPageNoAccess = 0x01u;
constexpr uint32_t kPageReadOnly = 0x02u;
constexpr uint32_t kPageReadWrite = 0x04u;

constexpr uint32_t kXex2Magic = 0x58455832u;
constexpr uint32_t kXexSecurityLoadAddress = 0x110u;

struct ImageRange {
    uint32_t base = 0;
    uint64_t end = 0;
};

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r || !r->mem)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s called without initialized runtime", fn);
    return *r;
}

uint32_t load_be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void store_be32(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

uint32_t current_page_protect(const GuestMemory& mem, uint32_t address) {
    const uint64_t page = (uint64_t)address & ~(kGuestPageSize - 1);
    if (!mem.is_committed(page, kGuestPageSize)) return 0;
    if (mem.is_accessible(page, kGuestPageSize, Protect::ReadWrite)) return kPageReadWrite;
    if (mem.is_accessible(page, kGuestPageSize, Protect::Read)) return kPageReadOnly;
    return kPageNoAccess;
}

uint32_t guest_base_protect(uint32_t protect) {
    const uint32_t base = protect & 0xFFu;
    if (base == kPageNoAccess || base == kPageReadOnly || base == kPageReadWrite) return base;
    return 0;
}

bool inside_heap(const GuestHeap& heap, uint32_t address) {
    return address >= heap.lo() && address < heap.hi();
}

Status main_image(Runtime& r, ImageRange* image, bool* present) {
    *present = false;
    const ModuleState* modules = r.modules.get();
    if (!modules || !modules->ready || modules->generation != r.generation) return Status::Ok;
    if (!modules->header_storage || modules->header_size < 24 ||
        !r.mem->is_accessible(modules->header_storage, modules->header_size, Protect::Read))
        return Status::Conflict;
    const uint8_t* header = r.mem->host(modules->header_storage);
    if (load_be32(header) != kXex2Magic ||
        validate_xex_header(header, modules->header_size) != Status::Ok)
        return Status::Conflict;
    const uint32_t security = load_be32(header + 16);
    if (uint64_t(security) + kXexSecurityLoadAddress + 4 > modules->header_size)
        return Status::Conflict;
    const uint32_t image_base = load_be32(header + security + kXexSecurityLoadAddress);
    const uint32_t image_size = load_be32(header + security + 4);
    if (!image_size || uint64_t(image_base) + image_size > kGuestSpaceSize)
        return Status::Conflict;
    const uint64_t lo = uint64_t(image_base) & ~(kGuestPageSize - 1);
    const uint64_t hi = (uint64_t(image_base) + image_size + kGuestPageSize - 1) &
                        ~(kGuestPageSize - 1);
    if (lo != image_base || hi > kGuestSpaceSize) return Status::Unsupported;
    *image = {image_base, hi};
    *present = true;
    return Status::Ok;
}

bool page_is_external_free(Runtime& r, const ImageRange& image, bool image_present, uint64_t page) {
    if (page >= kGuestSpaceSize) return false;
    if (page < r.heap.hi() && r.heap.lo() < page + kGuestPageSize) return false;
    if (page < r.physical.hi() && r.physical.lo() < page + kGuestPageSize) return false;
    if (image_present && page < image.end && image.base < page + kGuestPageSize) return false;
    if (r.mem->overlaps_runtime_range(page, kGuestPageSize)) return false;
    return !r.mem->is_committed(page, kGuestPageSize);
}

Status query_image(Runtime& r, const ImageRange& image, uint32_t address,
                   MemoryQueryResult* out) {
    if (address < image.base || uint64_t(address) >= image.end) return Status::NotFound;
    const uint64_t page = uint64_t(address) & ~(kGuestPageSize - 1);
    const bool committed = r.mem->is_committed(page, kGuestPageSize);
    const uint32_t protect = committed ? current_page_protect(*r.mem, (uint32_t)page) : 0;
    uint64_t begin = page, end = page + kGuestPageSize;
    while (begin > image.base) {
        const uint64_t previous = begin - kGuestPageSize;
        const bool previous_committed = r.mem->is_committed(previous, kGuestPageSize);
        const uint32_t previous_protect = previous_committed ? current_page_protect(*r.mem, (uint32_t)previous) : 0;
        if (previous_committed != committed || previous_protect != protect) break;
        begin = previous;
    }
    while (end < image.end) {
        const bool next_committed = r.mem->is_committed(end, kGuestPageSize);
        const uint32_t next_protect = next_committed ? current_page_protect(*r.mem, (uint32_t)end) : 0;
        if (next_committed != committed || next_protect != protect) break;
        end += kGuestPageSize;
    }
    out->basic = {(uint32_t)begin, image.base, kPageReadWrite, (uint32_t)(end - begin),
                  committed ? kMemCommit : kMemReserve, committed ? protect : 0, kMemImage};
    out->provenance = MemoryQueryProvenance::MainImage;
    out->protection_modifiers_known = true;
    return Status::Ok;
}

Status query_heap(Runtime& r, GuestHeap& heap, const std::set<uint32_t>& family,
                  MemoryQueryProvenance provenance, uint32_t address,
                  MemoryQueryResult* out) {
    GuestHeapRegionInfo region{};
    Status status = heap.region_containing(address, &region);
    if (status == Status::Conflict) return Status::Unsupported;
    if (status != Status::Ok) return status;
    if (!region.allocated) {
        // GuestHeap suballocates at 16-byte granularity. Only whole free guest
        // pages are VM-free; a partial free fragment sharing a live page cannot
        // be advertised as MEM_FREE.
        const uint64_t full_begin = (uint64_t(region.base) + kGuestPageSize - 1) & ~(kGuestPageSize - 1);
        const uint64_t full_end = (uint64_t(region.base) + region.size) & ~(kGuestPageSize - 1);
        const uint64_t page = uint64_t(address) & ~(kGuestPageSize - 1);
        if (page < full_begin || page >= full_end || full_end <= full_begin) return Status::Unsupported;
        out->basic = {(uint32_t)full_begin, 0, 0, (uint32_t)(full_end - full_begin), kMemFree, 0, 0};
        out->provenance = MemoryQueryProvenance::Free;
        out->protection_modifiers_known = true;
        return Status::Ok;
    }
    if (!family.count(region.base)) return Status::Unsupported;

    GuestHeapProtectionInfo protection{};
    status = heap.query_guest_protection(address, &protection);
    if (status != Status::Ok) return status == Status::NotFound ? Status::Unsupported : status;
    const bool committed = r.mem->is_committed(address, 1);
    if (committed) {
        const uint32_t expected = guest_base_protect(protection.protect);
        const uint32_t actual = current_page_protect(*r.mem, address);
        if (!expected || expected != actual) return Status::Conflict;
    }

    // A reserved run has no current Protect, so old protection spans cannot
    // split it. Committed runs must match both protection and backing state.
    const uint64_t lower = committed ? protection.region_base : protection.allocation_base;
    const uint64_t upper = committed ? uint64_t(protection.region_base) + protection.region_size
                                     : uint64_t(region.base) + region.size;
    const uint64_t page = uint64_t(address) & ~(kGuestPageSize - 1);
    uint64_t begin = std::max<uint64_t>(lower, page);
    uint64_t end = std::min<uint64_t>(upper, page + kGuestPageSize);
    auto matches = [&](uint64_t candidate) {
        if (r.mem->is_committed(candidate, 1) != committed) return false;
        return !committed || current_page_protect(*r.mem, uint32_t(candidate)) ==
                             guest_base_protect(protection.protect);
    };
    while (begin > lower) {
        const uint64_t previous = std::max<uint64_t>(lower, begin - kGuestPageSize);
        if (!matches(previous)) break;
        begin = previous;
    }
    while (end < upper && matches(end))
        end = std::min<uint64_t>(upper, end + kGuestPageSize);
    if (end <= begin || end - begin > UINT32_MAX) return Status::Conflict;
    out->basic = {(uint32_t)begin, protection.allocation_base, protection.allocation_protect,
                  (uint32_t)(end - begin), committed ? kMemCommit : kMemReserve,
                  committed ? protection.protect : 0, kMemPrivate};
    out->provenance = provenance;
    out->protection_modifiers_known = true;
    return Status::Ok;
}

Status query_external_free(Runtime& r, const ImageRange& image, bool image_present,
                           uint32_t address, MemoryQueryResult* out) {
    const uint64_t query_page = uint64_t(address) & ~(kGuestPageSize - 1);
    if (!page_is_external_free(r, image, image_present, query_page)) return Status::Unsupported;
    uint64_t begin = query_page;
    while (begin >= kGuestPageSize && page_is_external_free(r, image, image_present, begin - kGuestPageSize))
        begin -= kGuestPageSize;
    uint64_t end = query_page + kGuestPageSize;
    while (end < kGuestSpaceSize && page_is_external_free(r, image, image_present, end)) end += kGuestPageSize;
    if (end <= begin || end - begin > UINT32_MAX) return Status::Unsupported;

    out->basic = {};
    out->basic.base_address = (uint32_t)begin;
    out->basic.region_size = (uint32_t)(end - begin);
    out->basic.state = kMemFree;
    out->provenance = MemoryQueryProvenance::Free;
    out->protection_modifiers_known = true;
    return Status::Ok;
}

void NtQueryVirtualMemory(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "NtQueryVirtualMemory";
    Runtime& r = current(fn);
    const uint32_t address = c.r3.u32;
    const uint32_t output = c.r4.u32;
    const uint32_t region_type = c.r5.u32;
    if (region_type != 0) {
        c.r3.u64 = nt::kInvalidParameter;
        return;
    }
    if (!output || !r.mem->is_accessible(output, sizeof(MemoryBasicInformation32),
                                         Protect::ReadWrite)) {
        c.r3.u64 = nt::kAccessViolation;
        return;
    }

    MemoryQueryResult result{};
    const Status status = query_memory_basic(address, &result);
    if (status == Status::OutOfMemory) {
        c.r3.u64 = nt::kNoMemory;
        return;
    }
    if (status == Status::InvalidArgument) {
        c.r3.u64 = nt::kInvalidParameter;
        return;
    }
    if (status == Status::Unsupported) {
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "%s address=0x%08X has committed/reserved provenance that current shared state cannot report exactly",
                    fn, address);
    }
    if (status != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s query failed for 0x%08X: %s", fn, address,
                    status_name(status));

    uint8_t* p = r.mem->host(output);
    store_be32(p + 0, result.basic.base_address);
    store_be32(p + 4, result.basic.allocation_base);
    store_be32(p + 8, result.basic.allocation_protect);
    store_be32(p + 12, result.basic.region_size);
    store_be32(p + 16, result.basic.state);
    store_be32(p + 20, result.basic.protect);
    store_be32(p + 24, result.basic.type);
    c.r3.u64 = nt::kSuccess;
}

void MmQueryAddressProtect(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "MmQueryAddressProtect";
    const uint32_t address = c.r3.u32;
    MemoryQueryResult info{};
    const Status status = query_memory_basic(address, &info);
    if (status == Status::OutOfMemory) {
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s could not allocate query metadata", fn);
    }
    if (status == Status::Unsupported) {
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "%s address=0x%08X has no exact guest allocation provenance", fn,
                    address);
    }
    if (status != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s query failed for 0x%08X: %s", fn, address,
                    status_name(status));
#if RCOMP_RUNTIME_DIAGNOSTICS
    {   // bring-up tracing
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1) < 40 || (n.load() % 20000) == 0) fprintf(stderr, "RCOMP-MMQAP addr=0x%08X state=0x%X protect=0x%X known=%d lr=0x%08X\n", address, info.basic.state, info.basic.protect, int(info.protection_modifiers_known), (uint32_t)c.lr);
    }
#endif
    if (info.basic.state != kMemCommit) {
        c.r3.u64 = 0;
        return;
    }
    if (!info.protection_modifiers_known)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "%s address=0x%08X has no complete Xbox PAGE_* provenance",
                    fn, address);
    c.r3.u64 = info.basic.protect;
}

struct Impl { uint32_t ordinal; const char* name; PPCFunc* fn; };
constexpr Impl kImpls[] = {
    {0x00C4, "MmQueryAddressProtect", &MmQueryAddressProtect},
    {0x00EE, "NtQueryVirtualMemory", &NtQueryVirtualMemory},
};

}  // namespace

Status query_memory_basic_canonical(uint32_t address, MemoryQueryResult* out);

// A physical block is queried at its 0xA0000000 address; an address in the 0xC0000000 / 0xE0000000
// alias windows (src/physical_window.h) gets the same answer expressed in its own window.
Status query_memory_basic(uint32_t address, MemoryQueryResult* out) {
    Runtime* r = runtime();
    if (!r || !r->mem) return Status::NotInitialized;
    const uint32_t canonical = physical_canonical(*r->mem, address);
    const Status status = query_memory_basic_canonical(canonical, out);
    if (status == Status::Ok && canonical != address) {
        const uint32_t delta = address - canonical;
        out->basic.base_address += delta;
        if (out->basic.allocation_base) out->basic.allocation_base += delta;
    }
    return status;
}

Status query_memory_basic_canonical(uint32_t address, MemoryQueryResult* out) {
    if (!out) return Status::InvalidArgument;
    Runtime* r = runtime();
    if (!r || !r->mem) return Status::NotInitialized;
    std::scoped_lock provenance_lock(r->virtual_allocation_mutex,
                                     r->physical_allocation_mutex);
    if (inside_heap(r->heap, address))
        return query_heap(*r, r->heap, r->virtual_allocations,
                          MemoryQueryProvenance::VirtualAllocation, address, out);
    if (inside_heap(r->physical, address))
        return query_heap(*r, r->physical, r->physical_allocations,
                          MemoryQueryProvenance::PhysicalAllocation, address, out);

    ImageRange image{};
    bool image_present = false;
    Status status = main_image(*r, &image, &image_present);
    if (status != Status::Ok) return status;
    if (image_present && address >= image.base && uint64_t(address) < image.end)
        return query_image(*r, image, address, out);
    return query_external_free(*r, image, image_present, address, out);
}

uint32_t query_address_protect(uint32_t address, bool* modifiers_known) {
    Runtime* r = runtime();
    if (!r || !r->mem) {
        if (modifiers_known) *modifiers_known = false;
        return 0;
    }
    MemoryQueryResult info{};
    const Status status = query_memory_basic(address, &info);
    if (status == Status::Ok) {
        if (modifiers_known) *modifiers_known = true;
        return info.basic.state == kMemCommit ? info.basic.protect : 0;
    }
    if (modifiers_known) *modifiers_known = false;
    return current_page_protect(*r->mem, address);
}

Status register_xboxkrnl_query_memory_hle() {
    for (const Impl& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "query-memory ordinal mismatch: %s", impl.name);
        Status status = register_import(kModuleXboxkrnl, impl.ordinal, impl.fn, impl.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
