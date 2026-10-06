// xboxkrnl.exe HLE: guest virtual/physical allocation and protection state.
//
// Same rules as hle_xboxkrnl.cpp: each export states the Xbox 360 semantics
// and the implemented subset; anything outside it ends in
// RCOMP_FATAL_UNIMPLEMENTED, never in a fake success.
//
// Virtual allocation PAGE_* provenance is owned by GuestHeap rather than the
// host mapping: GuestMemory records only commit/read/write access, while this
// layer must round-trip Xbox NOCACHE/WRITECOMBINE and AllocationProtect.
//
// Physical model. The Xbox 360 has 512 MiB of physical memory, visible to the
// CPU through three virtual windows that alias it (0xA0000000 with 64 KiB
// pages, 0xC0000000 with 16 MiB pages, 0xE0000000 with 4 KiB pages) and to
// the GPU by physical address. The physical heap and its bookkeeping use the
// 0xA0000000 address (physical P <-> kXenosPhysicalWindow + P); the title gets
// the block in the window of the page size it asked for (src/physical_window.h)
// where GuestMemory maps the alias windows (PS5), as titles test the window to
// tell a physical block from others (Unreal Engine 3's allocator frees an
// 0xE0000000 address as XPhysicalAlloc memory: Gears of War 2). Without
// aliasing (some hosts) the 0xA0000000 address is returned. Every service
// taking a physical address back accepts the three windows.
#include <algorithm>
#include <mutex>
#include "diagnostics.h"
#include "physical_window.h"
#include <new>
#include <set>
#include <cstdio>
#include <cstdlib>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/xenos_gpu.h"
#include "module_state.h"

namespace rcomp::rt {

namespace {

constexpr uint32_t PAGE_READWRITE = 0x04;
constexpr uint32_t PAGE_NOACCESS = 0x01;
constexpr uint32_t PAGE_READONLY = 0x02;
constexpr uint32_t PAGE_NOCACHE = 0x200;
constexpr uint32_t PAGE_WRITECOMBINE = 0x400;
constexpr uint32_t PAGE_BASE_MASK = PAGE_NOACCESS | PAGE_READONLY | PAGE_READWRITE;
constexpr uint32_t PAGE_MODIFIER_MASK = PAGE_NOCACHE | PAGE_WRITECOMBINE;
constexpr uint32_t MEM_COMMIT = 0x00001000;
constexpr uint32_t MEM_RESERVE = 0x00002000;
constexpr uint32_t MEM_DECOMMIT = 0x00004000;
constexpr uint32_t MEM_RELEASE = 0x00008000;
constexpr uint32_t MEM_TOP_DOWN = 0x00100000;
constexpr uint32_t MEM_NOZERO = 0x00800000;
constexpr uint32_t MEM_LARGE_PAGES = 0x20000000;  // 64 KiB pages
constexpr uint32_t MEM_HEAP = 0x40000000;  // title heap reservation in guest virtual memory
constexpr uint32_t MEM_16MB_PAGES = 0x80000000;

Runtime& rt_or_die(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s called before rcomp::rt::runtime_init", fn);
    return *r;
}

[[noreturn]] void unimplemented(const char* fn, PPCContext& ctx, const char* what, uint32_t v) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xboxkrnl.exe!%s %s=0x%08X lr=0x%08X (not implemented)", fn, what, v,
                (uint32_t)ctx.lr);
}

void ret(PPCContext& ctx, uint32_t status) { ctx.r3.u64 = status; }

bool guest_protect(uint32_t value, Protect* host) {
    const uint32_t base = value & PAGE_BASE_MASK;
    if (value & ~(PAGE_BASE_MASK | PAGE_MODIFIER_MASK)) return false;
    if (base != PAGE_NOACCESS && base != PAGE_READONLY && base != PAGE_READWRITE) return false;
    *host = base == PAGE_NOACCESS ? Protect::None :
            base == PAGE_READONLY ? Protect::Read : Protect::ReadWrite;
    return true;
}

uint32_t physical_page_protect(uint32_t value) {
    return value & (PAGE_READWRITE | PAGE_MODIFIER_MASK);
}

Status rollback_heap_allocation(GuestHeap& heap, uint32_t address) {
    return heap.free(address);
}

void report_allocation_failure(Runtime& r, const char* fn, uint32_t size, uint32_t protect, const char* detail,
                               Status status, PPCContext& ctx);

// ---- NtAllocateVirtualMemory (0x00CC) -------------------------------------
// Current verified subset: anonymous committed allocations, optionally with
// MEM_RESERVE/TOP_DOWN/NOZERO/LARGE_PAGES. PAGE_READWRITE plus NOCACHE and/or
// WRITECOMBINE is preserved exactly in GuestHeap provenance for future query.
void NtAllocateVirtualMemory(PPCContext& ctx, uint8_t*) {
    constexpr const char* fn = "NtAllocateVirtualMemory";
    Runtime& r = rt_or_die(fn);
    const uint32_t pbase = ctx.r3.u32, psize = ctx.r4.u32;
    const uint32_t type = ctx.r5.u32, protect = ctx.r6.u32;
    const uint32_t debug = ctx.r7.u32 & 0xFFu;
    uint32_t base_in = 0, size_in = 0;
    if (!r.mem->is_accessible(pbase, 4, Protect::ReadWrite) ||
        !r.mem->is_accessible(psize, 4, Protect::ReadWrite) ||
        !guest_read_be32(pbase, &base_in) || !guest_read_be32(psize, &size_in)) {
        ret(ctx, nt::kAccessViolation);
        return;
    }
#if RCOMP_RUNTIME_DIAGNOSTICS
    if (std::getenv("RCOMP_TRACE_MEMORY")) {
        std::fprintf(stderr, "RCOMP-MEM allocation base=0x%08X size=0x%08X type=0x%08X protect=0x%08X debug=%u lr=0x%08X\n",
                     base_in, size_in, type, protect, debug, uint32_t(ctx.lr));
        std::fflush(stderr);
    }
#endif
    if (!(type & (MEM_COMMIT | MEM_RESERVE))) { ret(ctx, nt::kInvalidParameter); return; }
    if (type & ~(MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_NOZERO | MEM_LARGE_PAGES | MEM_HEAP))
        unimplemented(fn, ctx, "allocation_type", type);
    Protect host_protect = Protect::None;
    if (!guest_protect(protect, &host_protect))
        unimplemented(fn, ctx, "protect", protect);
    if (debug) unimplemented(fn, ctx, "debug_memory", debug);
    if (!size_in) { ret(ctx, nt::kInvalidParameter); return; }
    const uint64_t requested_end = uint64_t(base_in) + size_in;
    if (requested_end > kGuestSpaceSize) { ret(ctx, nt::kInvalidParameter); return; }
    const uint64_t adjusted_base = uint64_t(base_in) & ~(kGuestPageSize - 1);
    const uint64_t size64 = (requested_end + kGuestPageSize - 1) & ~(kGuestPageSize - 1);
    const uint64_t region_size = size64 - adjusted_base;
    if (!region_size || region_size > 0xFFFF0000ull) { ret(ctx, nt::kNoMemory); return; }

    if (base_in && !(type & MEM_RESERVE)) {
        uint32_t allocation_base = 0, allocation_size = 0;
        Status status;
        {
            std::lock_guard<std::mutex> lock(r.virtual_allocation_mutex);
            status = r.heap.allocation_containing(uint32_t(adjusted_base), &allocation_base, &allocation_size);
            if (status == Status::Ok && r.virtual_allocations.count(allocation_base) &&
                size64 <= uint64_t(allocation_base) + allocation_size)
                status = r.heap.commit_guest_range(uint32_t(adjusted_base), uint32_t(region_size),
                                                    protect, host_protect, !(type & MEM_NOZERO));
            else
                status = Status::NotAllocated;
        }
        if (status != Status::Ok) { ret(ctx, to_ntstatus(status)); return; }
        guest_write_be32(pbase, uint32_t(adjusted_base));
        guest_write_be32(psize, uint32_t(region_size));
        ret(ctx, nt::kSuccess);
        return;
    }

    std::set<uint32_t> staged;
#if defined(__cpp_exceptions)
    try { staged.insert(0); }
    catch (const std::bad_alloc&) { ret(ctx, nt::kNoMemory); return; }
#else
    staged.insert(0);
#endif
    uint32_t address = 0;
    Status status = Status::Ok;
    Status rollback = Status::Ok;
    bool duplicate = false;
    {
        std::lock_guard<std::mutex> lock(r.virtual_allocation_mutex);
        // Without a base, the kernel places 4 KiB-page allocations below 0x40000000 and
        // MEM_LARGE_PAGES ones above (GuestHeap::kSmallPageLo / kLargePageLo).
        const bool large = (type & MEM_LARGE_PAGES) != 0;
        const uint64_t window_lo = large ? std::max<uint64_t>(r.heap.lo(), GuestHeap::kLargePageLo) : r.heap.lo();
        const uint64_t window_hi = large ? r.heap.hi() : std::min<uint64_t>(r.heap.hi(), GuestHeap::kLargePageLo);
        const uint64_t lo = base_in ? adjusted_base : window_lo;
        const uint64_t hi = base_in ? size64 : (window_lo < window_hi ? window_hi : r.heap.hi());
        if (lo < r.heap.lo() || hi > r.heap.hi()) {
            status = Status::Conflict;
        } else if ((type & MEM_RESERVE) && !(type & MEM_COMMIT)) {
            status = r.heap.reserve_in(uint32_t(region_size), uint32_t(kGuestPageSize),
                                       lo, hi, !!(type & MEM_TOP_DOWN), &address);
        } else {
            status = r.heap.alloc_in(uint32_t(region_size), uint32_t(kGuestPageSize),
                                     lo, hi, !!(type & MEM_TOP_DOWN), !(type & MEM_NOZERO), &address);
        }
        if (status == Status::Ok) {
            status = r.heap.set_guest_protection(address, protect, (uint32_t)kGuestPageSize);
            if (status == Status::Ok && (type & MEM_COMMIT) && host_protect != Protect::ReadWrite)
                status = r.heap.protect_guest_range(address, uint32_t(region_size), protect, host_protect);
            if (status != Status::Ok) {
                rollback = rollback_heap_allocation(r.heap, address);
            } else {
                auto ownership = staged.extract(staged.begin());
                ownership.value() = address;
                auto inserted = r.virtual_allocations.insert(std::move(ownership));
                duplicate = !inserted.inserted;
                if (duplicate) rollback = rollback_heap_allocation(r.heap, address);
            }
        }
    }
    if (rollback != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s rollback failed for 0x%08X: %s",
                    fn, address, status_name(rollback));
    if (duplicate)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s duplicate ownership 0x%08X", fn, address);
    if (status != Status::Ok) {
        char detail[64];
        std::snprintf(detail, sizeof detail, "base=0x%08X type=0x%08X", base_in, type);
        report_allocation_failure(r, fn, size_in, protect, detail, status, ctx);
        ret(ctx, to_ntstatus(status));
        return;
    }
    if (!guest_write_be32(pbase, address) || !guest_write_be32(psize, (uint32_t)region_size))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s validated output became unwritable", fn);
    ret(ctx, nt::kSuccess);
}

// ---- NtFreeVirtualMemory (0x00DC) -----------------------------------------
void NtFreeVirtualMemory(PPCContext& ctx, uint8_t*) {
    constexpr const char* fn = "NtFreeVirtualMemory";
    Runtime& r = rt_or_die(fn);
    const uint32_t pbase = ctx.r3.u32, psize = ctx.r4.u32, type = ctx.r5.u32;
    const uint32_t debug = ctx.r6.u32 & 0xFFu;
    if (type != MEM_RELEASE && type != MEM_DECOMMIT) unimplemented(fn, ctx, "free_type", type);
    if (debug) unimplemented(fn, ctx, "debug_memory", debug);
    uint32_t base = 0, requested_size = 0;
    if (!r.mem->is_accessible(pbase, 4, Protect::ReadWrite) ||
        !r.mem->is_accessible(psize, 4, Protect::ReadWrite) ||
        !guest_read_be32(pbase, &base) || !guest_read_be32(psize, &requested_size)) {
        ret(ctx, nt::kAccessViolation);
        return;
    }
    if (type == MEM_DECOMMIT) {
        uint32_t allocation_base = 0, allocation_size = 0;
        const uint64_t first = uint64_t(base) & ~(kGuestPageSize - 1);
        const uint64_t raw_end = uint64_t(base) + requested_size;
        const uint64_t end = (raw_end + kGuestPageSize - 1) & ~(kGuestPageSize - 1);
        if (!requested_size || end > kGuestSpaceSize || end <= first) {
            ret(ctx, nt::kInvalidParameter); return;
        }
        Status status;
        {
            std::lock_guard<std::mutex> lock(r.virtual_allocation_mutex);
            status = r.heap.allocation_containing(uint32_t(first), &allocation_base, &allocation_size);
            if (status == Status::Ok && r.virtual_allocations.count(allocation_base) &&
                end <= uint64_t(allocation_base) + allocation_size)
                status = r.heap.decommit_guest_range(uint32_t(first), uint32_t(end-first));
            else
                status = Status::NotAllocated;
        }
        if (status == Status::Ok) {
            guest_write_be32(pbase, uint32_t(first));
            guest_write_be32(psize, uint32_t(end-first));
        }
        ret(ctx, to_ntstatus(status));
        return;
    }
    if (requested_size) { ret(ctx, nt::kInvalidParameter); return; }
    uint32_t released_size = 0;
    Status status = Status::NotAllocated;
    {
        std::lock_guard<std::mutex> lock(r.virtual_allocation_mutex);
        if (r.virtual_allocations.count(base)) {
            status = r.heap.allocation_size(base, &released_size);
            if (status == Status::Ok) status = r.heap.free(base);
            if (status == Status::Ok) r.virtual_allocations.erase(base);
        }
    }
    if (status == Status::NotAllocated) { ret(ctx, nt::kMemoryNotAllocated); return; }
    if (status != Status::Ok) { ret(ctx, to_ntstatus(status)); return; }
    if (!guest_write_be32(psize, released_size))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s validated output became unwritable", fn);
    ret(ctx, nt::kSuccess);
}

// ---- NtProtectVirtualMemory (0x00E1) --------------------------------------
void NtProtectVirtualMemory(PPCContext& ctx, uint8_t*) {
    constexpr const char* fn = "NtProtectVirtualMemory";
    Runtime& r = rt_or_die(fn);
    const uint32_t pbase = ctx.r3.u32, psize = ctx.r4.u32;
    const uint32_t protect = ctx.r5.u32, pold = ctx.r6.u32;
    const uint32_t debug = ctx.r7.u32 & 0xFFu;
    if (debug) unimplemented(fn, ctx, "debug_memory", debug);
    Protect host = Protect::None;
    if (!guest_protect(protect, &host)) unimplemented(fn, ctx, "protect", protect);
    uint32_t base = 0, size = 0;
    if (!r.mem->is_accessible(pbase, 4, Protect::ReadWrite) ||
        !r.mem->is_accessible(psize, 4, Protect::ReadWrite) ||
        (pold && !r.mem->is_accessible(pold, 4, Protect::ReadWrite)) ||
        !guest_read_be32(pbase, &base) || !guest_read_be32(psize, &size)) {
        ret(ctx, nt::kAccessViolation);
        return;
    }
    if (!size) { ret(ctx, nt::kInvalidParameter); return; }
    const uint64_t begin = uint64_t(base) & ~(kGuestPageSize - 1);
    const uint64_t raw_end = uint64_t(base) + size;
    if (raw_end > kGuestSpaceSize) { ret(ctx, nt::kInvalidParameter); return; }
    const uint64_t end = (raw_end + kGuestPageSize - 1) & ~(kGuestPageSize - 1);
    if (end > kGuestSpaceSize || end <= begin) { ret(ctx, nt::kInvalidParameter); return; }
    const uint32_t adjusted_base = (uint32_t)begin;
    const uint32_t adjusted_size = (uint32_t)(end - begin);

    uint32_t allocation_base = 0, allocation_size = 0;
    uint32_t old = 0;
    Status status = Status::NotAllocated;
    {
        std::lock_guard<std::mutex> lock(r.virtual_allocation_mutex);
        status = r.heap.allocation_containing(adjusted_base, &allocation_base, &allocation_size);
        if (status == Status::Ok && r.virtual_allocations.count(allocation_base) &&
            end <= uint64_t(allocation_base) + allocation_size)
            status = r.heap.protect_guest_range(adjusted_base, adjusted_size, protect, host, &old);
        else
            status = Status::NotAllocated;
    }
    if (status == Status::NotAllocated) {
        ret(ctx, nt::kMemoryNotAllocated);
        return;
    }
    if (status != Status::Ok) { ret(ctx, to_ntstatus(status)); return; }
    if (!guest_write_be32(pbase, adjusted_base) || !guest_write_be32(psize, adjusted_size) ||
        (pold && !guest_write_be32(pold, old)))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s validated output became unwritable", fn);
    ret(ctx, nt::kSuccess);
}

// Bases returned by MmAllocatePhysicalMemory(Ex), so MmFreePhysicalMemory
// cannot release other blocks of the physical heap.
// Lifetime-scoped ownership is stored on Runtime.

bool in_window(uint32_t addr) { return in_physical_windows(addr); }

// A refused allocation is rare and decisive (titles usually stop with their own out-of-memory
// handler right after it): one RCOMP-MEM line with the request and the state of both heaps.
void report_allocation_failure(Runtime& r, const char* fn, uint32_t size, uint32_t protect, const char* detail,
                               Status status, PPCContext& ctx) {
    const GuestHeapStats v = r.heap.stats(), p = r.physical.stats();
    std::fprintf(stderr,
                 "RCOMP-MEM allocation_failed fn=%s size=0x%08X protect=0x%08X %s "
                 "status=%s lr=0x%08X virtual_used=0x%llX virtual_free=0x%llX virtual_live=%u physical_used=0x%llX "
                 "physical_free=0x%llX physical_live=%u\n",
                 fn, size, protect, detail, status_name(status), uint32_t(ctx.lr),
                 (unsigned long long)(v.allocated_bytes - v.reserved_unbacked_bytes),
                 (unsigned long long)v.free_bytes, v.live_allocations,
                 (unsigned long long)p.allocated_bytes, (unsigned long long)p.free_bytes, p.live_allocations);
    std::fflush(stderr);
}

// ---- MmAllocatePhysicalMemoryEx (0x00BA) ---------------------------------
// Xbox 360: PVOID MmAllocatePhysicalMemoryEx(ULONG Flags, SIZE_T Size,
//   ULONG Protect, ULONG_PTR LowestAcceptableAddress,
//   ULONG_PTR HighestAcceptableAddress, ULONG_PTR Alignment). Allocates
//   physically contiguous, committed memory whose physical range lies in
//   [Lowest, Highest]; returns its virtual address (in the window of the
//   page size: MEM_LARGE_PAGES 64 KiB, MEM_16MB_PAGES 16 MiB, else 4 KiB),
//   or NULL. This is what XPhysicalAlloc and Direct3D (ring buffer, render
//   targets, textures) use.
// Implemented: Flags == 0; Protect = PAGE_READWRITE optionally | NOCACHE |
//   WRITECOMBINE (no host effect) | a page-size bit. Size and alignment are
//   rounded up to the page size; the physical range limits are honoured;
//   placement is top-down (as the real allocator and Xenia do); contents are
//   zero. Returned in the window of the page size (see the file comment). NULL
//   when no block fits.
// Not implemented (trap): Flags != 0, read-only or other protections.
uint32_t alloc_physical(PPCContext& ctx, const char* fn, uint32_t flags, uint32_t size, uint32_t protect,
                        uint32_t lowest, uint32_t highest, uint32_t alignment) {
    Runtime& r = rt_or_die(fn);
    if (flags) unimplemented(fn, ctx, "flags", flags);
    const uint32_t page_bits = protect & (MEM_LARGE_PAGES | MEM_16MB_PAGES);
    if (page_bits == (MEM_LARGE_PAGES | MEM_16MB_PAGES)) unimplemented(fn, ctx, "protect", protect);
    if ((protect & ~(page_bits | PAGE_NOCACHE | PAGE_WRITECOMBINE)) != PAGE_READWRITE)
        unimplemented(fn, ctx, "protect", protect);
    if (size == 0) return 0;
    const uint64_t page = (protect & MEM_16MB_PAGES) ? 0x1000000 : (protect & MEM_LARGE_PAGES) ? 0x10000 : 0x1000;
    const uint64_t size_r = ((uint64_t)size + page - 1) & ~(page - 1);
    uint64_t align = alignment ? ((uint64_t)alignment + page - 1) & ~(page - 1) : page;
    if (align & (align - 1)) return 0;  // not a power of two: no block satisfies it
    if (size_r > xenos::kXenosPhysicalSize || align > xenos::kXenosPhysicalSize) return 0;
    // Physical [lowest, highest] -> guest [lo, hi).
    const uint64_t lo = (uint64_t)xenos::kXenosPhysicalWindow + lowest;
    const uint64_t hi = (uint64_t)xenos::kXenosPhysicalWindow +
                        ((uint64_t)highest >= xenos::kXenosPhysicalSize ? xenos::kXenosPhysicalSize
                                                                        : (uint64_t)highest + 1);
    uint32_t addr = 0;
    std::set<uint32_t> pending;
#if defined(__cpp_exceptions)
    try {
#endif
        pending.insert(0);
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) { return 0; }
#endif
    Status allocation_status = Status::Ok;
    Status protection_status = Status::Ok;
    Status rollback = Status::Ok;
    bool duplicate = false;
    {
        std::lock_guard<std::mutex> lk(r.physical_allocation_mutex);
        allocation_status = r.physical.alloc_in((uint32_t)size_r, (uint32_t)align, lo, hi, true, true, &addr);
        if (allocation_status == Status::Ok) {
            protection_status = r.physical.set_guest_protection(
                addr, physical_page_protect(protect), (uint32_t)page);
            if (protection_status != Status::Ok) {
                rollback = rollback_heap_allocation(r.physical, addr);
            } else {
                auto ownership=pending.extract(pending.begin()); ownership.value()=addr;
                auto inserted = r.physical_allocations.insert(std::move(ownership));
                duplicate = !inserted.inserted;
                if (duplicate) rollback = rollback_heap_allocation(r.physical, addr);
            }
        }
    }
    if (rollback != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s physical rollback failed for 0x%08X: %s",
                    fn, addr, status_name(rollback));
    if (duplicate)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s duplicate physical ownership 0x%08X", fn, addr);
    if (allocation_status != Status::Ok || protection_status != Status::Ok) {
        char detail[96];
        std::snprintf(detail, sizeof detail, "range=0x%08X-0x%08X align=0x%08X", lowest, highest, alignment);
        report_allocation_failure(r, fn, size, protect, detail, allocation_status, ctx);
        return 0;
    }
    const uint32_t windowed = physical_window_for_page(*r.mem, addr, page);
    return r.mem->is_accessible(windowed, uint32_t(size_r), Protect::ReadWrite) ? windowed : addr;
}

void MmAllocatePhysicalMemoryEx(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = alloc_physical(ctx, "MmAllocatePhysicalMemoryEx", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                ctx.r7.u32, ctx.r8.u32);
}

// ---- MmAllocatePhysicalMemory (0x00B9) -----------------------------------
// Xbox 360: MmAllocatePhysicalMemory(Flags, Size, Protect) ==
// MmAllocatePhysicalMemoryEx(Flags, Size, Protect, 0, MAXULONG_PTR, 0).
void MmAllocatePhysicalMemory(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 =
        alloc_physical(ctx, "MmAllocatePhysicalMemory", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 0, 0xFFFFFFFFu, 0);
}

// ---- MmFreePhysicalMemory (0x00BD) ---------------------------------------
// Xbox 360: VOID MmFreePhysicalMemory(ULONG Type, PVOID BaseAddress):
// releases a block returned by MmAllocatePhysicalMemory(Ex). Implemented for
// those blocks (Type is not interpreted: it only selects the kernel pool
// accounting). Any other address is a title bug the real kernel stops on:
// fatal here.
void MmFreePhysicalMemory(PPCContext& ctx, uint8_t*) {
    const char* fn = "MmFreePhysicalMemory";
    Runtime& r = rt_or_die(fn);
    const uint32_t addr = physical_canonical(*r.mem, ctx.r4.u32);
    Status result=Status::NotAllocated;
    {
        std::lock_guard<std::mutex> lk(r.physical_allocation_mutex);
        auto i=r.physical_allocations.find(addr);
        if(i!=r.physical_allocations.end()) {
            result=r.physical.free(addr);
            if(result==Status::Ok) r.physical_allocations.erase(i);
        }
    }
    if (result==Status::NotAllocated || result==Status::DoubleFree)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s 0x%08X is not a physical allocation lr=0x%08X", fn, addr,
                    (uint32_t)ctx.lr);
    if (result != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xboxkrnl.exe!%s 0x%08X: physical heap refused the free", fn, addr);
}

// ---- MmSetAddressProtect (0x00C7) -----------------------------------------
// Xbox API is void. We support tracked Nt allocations and physical allocations
// when the requested range can be represented at R-comp's 64-KiB host mapping
// granularity. Exact PAGE_NOCACHE/PAGE_WRITECOMBINE bits are retained even
// though they do not affect host page permissions.
void MmSetAddressProtect(PPCContext& ctx, uint8_t*) {
    constexpr const char* fn = "MmSetAddressProtect";
    Runtime& r = rt_or_die(fn);
    const uint32_t address = physical_canonical(*r.mem, ctx.r3.u32), size = ctx.r4.u32, protect = ctx.r5.u32;
    Protect host = Protect::None;
    if (!guest_protect(protect, &host)) unimplemented(fn, ctx, "protect", protect);
    if (!size) unimplemented(fn, ctx, "size", size);

    GuestHeap* heap = nullptr;
    std::mutex* family_mutex = nullptr;
    std::set<uint32_t>* family = nullptr;
    if (address >= r.physical.lo() && address < r.physical.hi()) {
        heap = &r.physical;
        family_mutex = &r.physical_allocation_mutex;
        family = &r.physical_allocations;
    } else if (address >= r.heap.lo() && address < r.heap.hi()) {
        heap = &r.heap;
        family_mutex = &r.virtual_allocation_mutex;
        family = &r.virtual_allocations;
    } else {
        unimplemented(fn, ctx, "address_outside_tracked_heaps", address);
    }
    const uint64_t raw_end = uint64_t(address) + size;
    if (raw_end > kGuestSpaceSize)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s range overflow address=0x%08X size=0x%08X",
                    fn, address, size);

    uint32_t allocation_base = 0, allocation_size = 0;
    GuestHeapProtectionInfo current{};
    Status status = Status::Ok;
    bool not_tracked = false, missing_provenance = false, bad_range = false;
    {
        std::lock_guard<std::mutex> lock(*family_mutex);
        status = heap->allocation_containing(address, &allocation_base, &allocation_size);
        if (status != Status::Ok || !family->count(allocation_base)) {
            not_tracked = true;
        } else {
            status = heap->query_guest_protection(address, &current);
            if (status != Status::Ok || !current.page_size) {
                missing_provenance = true;
            } else {
                const uint64_t begin = uint64_t(address) & ~(uint64_t(current.page_size) - 1);
                const uint64_t end = (raw_end + current.page_size - 1) & ~(uint64_t(current.page_size) - 1);
                if (end > kGuestSpaceSize || end <= begin || begin < allocation_base ||
                    end > uint64_t(allocation_base) + allocation_size) {
                    bad_range = true;
                } else {
                    status = heap->protect_guest_range((uint32_t)begin, (uint32_t)(end - begin),
                                                       protect, host, nullptr);
                }
            }
        }
    }
    if (not_tracked) unimplemented(fn, ctx, "range_not_one_tracked_allocation", address);
    if (missing_provenance)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s missing page provenance for 0x%08X", fn, address);
    if (bad_range) unimplemented(fn, ctx, "range_not_one_tracked_allocation", address);
    if (status == Status::InvalidArgument || status == Status::NotAllocated)
        unimplemented(fn, ctx, "range_granularity", address);
    if (status != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "%s protection failed: %s", fn, status_name(status));
}

// ---- MmGetPhysicalAddress (0x00BE) ---------------------------------------
// Xbox 360: ULONG_PTR MmGetPhysicalAddress(PVOID Address): physical address
// behind a virtual address. Implemented for the physical window (every
// physical allocation): Address - kXenosPhysicalWindow. Other memory (XEX
// image, NtAllocateVirtualMemory blocks) is not GPU-visible in this memory
// model: trap, since the GPU would read different bytes.
void MmGetPhysicalAddress(PPCContext& ctx, uint8_t*) {
    const uint32_t addr = ctx.r3.u32;
    if (!in_window(addr)) unimplemented("MmGetPhysicalAddress", ctx, "address_outside_physical_window", addr);
    ctx.r3.u64 = physical_of_window(*rt_or_die("MmGetPhysicalAddress").mem, addr);
}

// ---- MmQueryAllocationSize (0x00C5) --------------------------------------
// Xbox 360: SIZE_T MmQueryAllocationSize(PVOID BaseAddress): size of the
// allocation starting at BaseAddress. Implemented for physical allocations
// and runtime heap allocations (NtAllocateVirtualMemory): their rounded
// size. Any other address: 0 (what the kernel returns for no allocation).
void MmQueryAllocationSize(PPCContext& ctx, uint8_t*) {
    Runtime& r = rt_or_die("MmQueryAllocationSize");
    const uint32_t addr = physical_canonical(*r.mem, ctx.r3.u32);
    uint32_t size = 0;
    GuestHeap& h = in_window(addr) ? r.physical : r.heap;
    if (h.allocation_size(addr, &size) != Status::Ok) size = 0;
    ctx.r3.u64 = size;
}

// ---- MmQueryStatistics (0x00C6) ------------------------------------------
// NTSTATUS MmQueryStatistics(PMM_STATISTICS). Layout (104 bytes, BE32 words;
// Xenia 95a5c3e X_MM_QUERY_STATISTICS_RESULT, after the vdash kernel.h):
//   +0x00 Length (in: must be 104)   +0x04 TotalPhysicalPages
//   +0x08 KernelPages                +0x0C title section (11 words)
//   +0x38 system section (11 words)  +0x64 HighestPhysicalPage
// section: AvailablePages, TotalVirtualMemoryBytes, ReservedVirtualMemoryBytes,
// PhysicalPages, PoolPages, StackPages, ImagePages, HeapPages, VirtualPages,
// PageTablePages, CachePages. Pages are 4 KiB.
// Filled from R-comp's live guest memory accounting (runtime/docs/QUERY_MEMORY.md
// "MmQueryStatistics"): the title has the console's 512 MiB physical budget
// (the size of the physical window), consumed by every backed title
// allocation: virtual heap (reserve-only pages excluded), physical heap and the
// loaded image. The kernel and the system run as host code: no guest pages.
// Breakdown fields R-comp does not attribute (stacks, heaps, VM, page tables,
// file cache) are 0; their bytes are still subtracted from AvailablePages.
// Errors as Xenia: NULL -> STATUS_INVALID_PARAMETER, Length != 104 ->
// STATUS_BUFFER_TOO_SMALL; an unwritable structure -> STATUS_ACCESS_VIOLATION.
void MmQueryStatistics(PPCContext& ctx, uint8_t*) {
    constexpr uint32_t kSize = 104, kPage4K = 4096, kStatusBufferTooSmall = 0xC0000023u;
    Runtime& r = rt_or_die("MmQueryStatistics");
    const uint32_t out = ctx.r3.u32;
    if (!out) { ctx.r3.u64 = nt::kInvalidParameter; return; }
    if ((out & 3) || !r.mem->is_accessible(out, kSize, Protect::ReadWrite)) {
        ctx.r3.u64 = nt::kAccessViolation;
        return;
    }
    uint32_t length = 0;
    guest_read_be32(out, &length);
    if (length != kSize) { ctx.r3.u64 = kStatusBufferTooSmall; return; }
    auto pages = [](uint64_t bytes) { return uint64_t((bytes + kPage4K - 1) / kPage4K); };
    const GuestHeapStats heap = r.heap.stats(), physical = r.physical.stats();
    const uint64_t image = r.modules && r.modules->ready ? r.modules->image_size : 0;
    uint64_t pool = 0;
    {
        std::lock_guard<std::mutex> lock(r.pool_mutex);
        for (const auto& a : r.pool_allocations) pool += a.requested_size;
    }
    const uint64_t total = xenos::kXenosPhysicalSize / kPage4K;
    const uint64_t used = pages(heap.allocated_bytes - heap.reserved_unbacked_bytes) +
                          pages(physical.allocated_bytes) + pages(image);
    uint32_t words[kSize / 4] = {};
    words[0] = kSize;
    words[1] = uint32_t(total);
    words[2] = 0;                                                        // KernelPages
    words[3] = uint32_t(used < total ? total - used : 0);                // title.AvailablePages
    words[4] = uint32_t(heap.region_bytes - uint64_t(heap.excluded_pages) * kGuestPageSize);
    words[5] = uint32_t(heap.allocated_bytes);                           // title.ReservedVirtualMemoryBytes
    words[6] = uint32_t(pages(physical.allocated_bytes));               // title.PhysicalPages
    words[7] = uint32_t(pages(pool));                                    // title.PoolPages
    words[9] = uint32_t(pages(image));                                   // title.ImagePages
    words[25] = uint32_t(total - 1);                                     // HighestPhysicalPage
    for (uint32_t i = 0; i < kSize / 4; ++i) guest_write_be32(out + 4 * i, words[i]);
    ctx.r3.u64 = nt::kSuccess;
}

struct Impl {
    uint32_t ordinal;
    const char* name;
    PPCFunc* fn;
};

const Impl kImpls[] = {
    {0x00CC, "NtAllocateVirtualMemory", &NtAllocateVirtualMemory},
    {0x00B9, "MmAllocatePhysicalMemory", &MmAllocatePhysicalMemory},
    {0x00BA, "MmAllocatePhysicalMemoryEx", &MmAllocatePhysicalMemoryEx},
    {0x00BD, "MmFreePhysicalMemory", &MmFreePhysicalMemory},
    {0x00BE, "MmGetPhysicalAddress", &MmGetPhysicalAddress},
    {0x00C5, "MmQueryAllocationSize", &MmQueryAllocationSize},
    {0x00C6, "MmQueryStatistics", &MmQueryStatistics},
    {0x00C7, "MmSetAddressProtect", &MmSetAddressProtect},
    {0x00DC, "NtFreeVirtualMemory", &NtFreeVirtualMemory},
    {0x00E1, "NtProtectVirtualMemory", &NtProtectVirtualMemory},
};

}  // namespace

Status register_xboxkrnl_memory_hle() {
    for (const Impl& i : kImpls) {
        uint32_t ord = 0;
        if (!export_ordinal(kModuleXboxkrnl, i.name, &ord) || ord != i.ordinal)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "xboxkrnl HLE %s: ordinal 0x%04X not in export table", i.name,
                        i.ordinal);
        Status s = register_import(kModuleXboxkrnl, i.ordinal, i.fn, i.name);
        if (s != Status::Ok) return s;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
