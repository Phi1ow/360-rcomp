// Locally implemented pool and RTL HLE. Contracts and research references:
// runtime/docs/POOL_RTL.md. No Xenia/rexglue runtime is linked or imported.
#include <algorithm>
#include <cstring>
#include <new>

#include "physical_window.h"
#include "rcomp/guest_write_tracking.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"

namespace rcomp::rt {
namespace {

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s called without runtime", fn);
    return *r;
}

[[noreturn]] void access_fault(const char* fn, uint64_t address, uint64_t length) {
    rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s address=0x%llX length=0x%llX", fn,
                (unsigned long long)address, (unsigned long long)length);
}

uint8_t* checked(Runtime& r, const char* fn, uint64_t address, uint64_t size, Protect rights) {
    if (!address || !r.mem->is_accessible(address, size, rights)) access_fault(fn, address, size);
    return r.mem->host(address);
}

// Scan only readable pages, without rounding a guest pointer through uint32_t.
// The caller may clamp an initialized descriptor to its representable length.
uint32_t string_length(Runtime& r, const char* fn, uint32_t source, uint32_t limit) {
    uint64_t offset = 0;
    while (offset < limit) {
        const uint64_t address = (uint64_t)source + offset;
        const uint64_t chunk = std::min<uint64_t>(limit - offset, kGuestPageSize - (address % kGuestPageSize));
        uint8_t* p = checked(r, fn, address, chunk, Protect::Read);
        const void* zero = std::memchr(p, 0, (size_t)chunk);
        if (zero) return (uint32_t)(offset + ((const uint8_t*)zero - p));
        offset += chunk;
    }
    return limit;
}

void store_descriptor(uint8_t* p, uint16_t length, uint16_t maximum, uint32_t source) {
    p[0] = (uint8_t)(length >> 8); p[1] = (uint8_t)length;
    p[2] = (uint8_t)(maximum >> 8); p[3] = (uint8_t)maximum;
    p[4] = (uint8_t)(source >> 24); p[5] = (uint8_t)(source >> 16);
    p[6] = (uint8_t)(source >> 8); p[7] = (uint8_t)source;
}

// Pool selector zero is established by the public ExAllocatePool wrapper.
// The nonzero selector's meaning is still unknown, not silently discarded.
// Tag is accounting metadata. Contents are unspecified, memory is RW only.
void ExAllocatePoolTypeWithTag(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "ExAllocatePoolTypeWithTag";
    Runtime& r = current(fn);
    const uint32_t size = c.r3.u32, tag = c.r4.u32, selector = c.r5.u32;
    if (selector)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "%s pool_selector=0x%08X lr=0x%08X", fn, selector, (uint32_t)c.lr);
    uint32_t address = 0;
    if (size) {
        std::lock_guard<std::mutex> lock(r.pool_mutex);
        // Reserve ownership bookkeeping before allocating guest memory.
        // With -fno-exceptions host allocator exhaustion terminates; it is not
        // reported as a recoverable guest OOM (see the documented limitation).
#if defined(__cpp_exceptions)
        try {
#endif
            r.pool_allocations.push_front({0, size, tag});
#if defined(__cpp_exceptions)
        } catch (const std::bad_alloc&) { c.r3.u64 = 0; return; }
#endif
        const uint32_t alignment = size > 0xFD8u ? 0x1000u : GuestHeap::kMinAlign;
        if (r.heap.alloc(size, alignment, false, &address) == Status::Ok)
            r.pool_allocations.front().address = address;
        else
            r.pool_allocations.pop_front();
    }
    c.r3.u64 = address;
}

// ExAllocatePool(SIZE_T NumberOfBytes): the public wrapper, the default pool
// (selector 0) with the tag 'None' (Xenia 95a5c3e xboxkrnl_memory.cc).
void ExAllocatePool(PPCContext& c, uint8_t* base) {
    c.r4.u64 = 0x656E6F4Eu;  // 'None' as the little-endian tag word NT pools use
    c.r5.u64 = 0;
    ExAllocatePoolTypeWithTag(c, base);
}

void ExFreePool(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "ExFreePool";
    Runtime& r = current(fn);
    const uint32_t address = c.r3.u32;
    Status result = Status::NotAllocated;
    {
        std::lock_guard<std::mutex> lock(r.pool_mutex);
        auto i = std::find_if(r.pool_allocations.begin(), r.pool_allocations.end(),
                              [address](const Runtime::PoolAllocation& p) { return p.address == address; });
        if (i != r.pool_allocations.end()) {
            result = r.heap.free(address);
            if (result == Status::Ok) r.pool_allocations.erase(i);
        }
    }
    // Fatal hooks can unwind/longjmp in tests. Never invoke one holding a lock.
    if (result == Status::NotAllocated || result == Status::DoubleFree)
        access_fault(fn, address, 0);
    if (result != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s failed without releasing allocation: %s", fn, status_name(result));
}

void RtlCompareMemoryUlong(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "RtlCompareMemoryUlong";
    Runtime& r = current(fn);
    const uint32_t source = c.r3.u32, length = c.r4.u32, pattern = c.r5.u32;
    uint32_t equal = 0;
    if (!(source & 3u) && !(length & 3u)) {
        while (equal < length) {
            const uint8_t* p = checked(r, fn, (uint64_t)source + equal, 4, Protect::Read);
            const uint32_t word = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
            if (word != pattern) break;
            equal += 4;
        }
    }
    c.r3.u64 = equal;  // leading equal ULONGs, in bytes, not a count of matches
}

// RtlCompareMemory (0x011A): SIZE_T (const VOID* Source1, const VOID* Source2, SIZE_T Length): the number of
// leading bytes that are equal (Length when the ranges match). Needed by Halo 3's Waves DLLs (L360, Q10).
void RtlCompareMemory(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "RtlCompareMemory";
    Runtime& r = current(fn);
    const uint32_t a = c.r3.u32, b = c.r4.u32, length = c.r5.u32;
    uint32_t equal = 0;
    while (equal < length) {
        const uint8_t ca = *checked(r, fn, (uint64_t)a + equal, 1, Protect::Read);
        const uint8_t cb = *checked(r, fn, (uint64_t)b + equal, 1, Protect::Read);
        if (ca != cb) break;
        ++equal;
    }
    c.r3.u64 = equal;
}

void RtlFillMemoryUlong(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "RtlFillMemoryUlong";
    Runtime& r = current(fn);
    const uint32_t destination = c.r3.u32, length = c.r4.u32, pattern = c.r5.u32;
    if ((destination & 3u) || (length & 3u))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s requires ULONG-aligned address and length", fn);
    if (!length) return;
    uint8_t* p = checked(r, fn, destination, length, Protect::ReadWrite);
    const uint8_t bytes[4] = {(uint8_t)(pattern >> 24), (uint8_t)(pattern >> 16),
                              (uint8_t)(pattern >> 8), (uint8_t)pattern};
    for (uint64_t i = 0; i < length; i += 4) std::memcpy(p + i, bytes, 4);
    note_title_write(destination, length);
}

// Xbox's fixed single-byte uppercase mapping from the public Canary research
// is independent of host locale. Express its ranges, not a host toupper().
uint8_t upper(uint8_t ch) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 0xE0 && ch <= 0xF6) || (ch >= 0xF8 && ch <= 0xFE))
        return (uint8_t)(ch - 0x20);
    return ch == 0xFF ? 0x3F : ch;
}

void RtlCompareStringN(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "RtlCompareStringN";
    Runtime& r = current(fn);
    const uint32_t a = c.r3.u32, b = c.r5.u32;
    uint32_t na = c.r4.u32, nb = c.r6.u32;
    const bool ignore_case = c.r7.u32 != 0;
    if (na == UINT32_MAX) na = string_length(r, fn, a, UINT32_MAX);
    if (nb == UINT32_MAX) nb = string_length(r, fn, b, UINT32_MAX);
    for (uint64_t i = 0, count = std::min(na, nb); i < count; ++i) {
        uint8_t ca = *checked(r, fn, (uint64_t)a + i, 1, Protect::Read);
        uint8_t cb = *checked(r, fn, (uint64_t)b + i, 1, Protect::Read);
        if (ignore_case) { ca = upper(ca); cb = upper(cb); }
        if (ca != cb) { c.r3.u64 = (uint32_t)((int32_t)ca - cb); return; }
    }
    c.r3.u64 = na - nb;  // DWORD arithmetic preserves the signed LONG ABI
}

void RtlInitAnsiString(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "RtlInitAnsiString";
    Runtime& r = current(fn);
    const uint32_t source = c.r4.u32;
    uint8_t* destination = checked(r, fn, c.r3.u32, 8, Protect::ReadWrite);
    const uint16_t length = source ? (uint16_t)string_length(r, fn, source, 0xFFFE) : 0;
    store_descriptor(destination, length, source ? (uint16_t)(length + 1) : 0, source);
}

void RtlInitUnicodeString(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "RtlInitUnicodeString";
    Runtime& r = current(fn);
    const uint32_t source = c.r4.u32;
    uint8_t* destination = checked(r, fn, c.r3.u32, 8, Protect::ReadWrite);
    uint16_t length = 0;
    if (source) {
        for (; length < 0xFFFC; length = (uint16_t)(length + 2)) {
            const uint8_t* p = checked(r, fn, (uint64_t)source + length, 2, Protect::Read);
            if (!(p[0] | p[1])) break;
        }
    }
    store_descriptor(destination, length, source ? (uint16_t)(length + 2) : 0, source);
    // Canary's Xbox-specific signature returns the descriptor address.
    c.r3.u64 = c.r3.u32;
}

struct Impl { uint32_t ordinal; const char* name; PPCFunc* fn; };
const Impl kImpls[] = {
    {0x0009, "ExAllocatePool", &ExAllocatePool},
    {0x000B, "ExAllocatePoolTypeWithTag", &ExAllocatePoolTypeWithTag},
    {0x000F, "ExFreePool", &ExFreePool},
    {0x011A, "RtlCompareMemory", &RtlCompareMemory},
    {0x011B, "RtlCompareMemoryUlong", &RtlCompareMemoryUlong},
    {0x011D, "RtlCompareStringN", &RtlCompareStringN},
    {0x0126, "RtlFillMemoryUlong", &RtlFillMemoryUlong},
    {0x012C, "RtlInitAnsiString", &RtlInitAnsiString},
    {0x012D, "RtlInitUnicodeString", &RtlInitUnicodeString},
};
}  // namespace

Status register_xboxkrnl_pool_rtl_hle() {
    for (const Impl& i : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, i.name, &ordinal) || ordinal != i.ordinal)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "pool/RTL ordinal mismatch: %s", i.name);
        Status result = register_import(kModuleXboxkrnl, i.ordinal, i.fn, i.name);
        if (result != Status::Ok) return result;
    }
    return Status::Ok;
}
}  // namespace rcomp::rt
