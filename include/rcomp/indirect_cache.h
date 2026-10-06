// Shared interface (owner: PRIME; implementation: cpu/runtime). Direct-mapped cache of the indirect-call target lookup.
//
// Every virtual call, function-pointer call and bctrl of the recompiled code looks its guest target up (rcomp_call_indirect ->
// lookup_function, a hash probe in a table of about 2 MB). In the instruction-level profile of Episodes from Liberty City's main
// thread (4 October 2026) that lookup was 4.8 % of all samples, about a tenth of the thread's running time. The generated code now
// (include/rcomp/ppc_prelude.h, PPC_CALL_INDIRECT_FUNC) tries this cache first.
//
// One word per entry: guest address in the high half, the host function pointer in the low half (the title image is mapped below
// 4 GiB; a pointer that does not fit is never cached). Entries are read and written with single relaxed 64-bit accesses, so a
// reader never sees half of two entries. An empty entry (host 0) never matches: an invalid target still reaches the fatal report.
// rcomp_call_indirect fills an entry after a successful lookup; publishing a function table (cpu/runtime/func_table.cpp) clears
// the whole cache, so a replaced or removed function is never called through a stale entry once the publication is over.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef RCOMP_INDIRECT_CACHE_BITS
#define RCOMP_INDIRECT_CACHE_BITS 14  // 16384 entries, 128 KiB
#endif

namespace rcomp {

constexpr size_t kIndirectCacheEntries = size_t(1) << RCOMP_INDIRECT_CACHE_BITS;
// An inline variable: every translation unit and library shares one table.
alignas(64) inline uint64_t g_indirect_cache[kIndirectCacheEntries];

inline size_t indirect_cache_index(uint32_t guest) { return (guest >> 2) & (kIndirectCacheEntries - 1); }

inline void indirect_cache_clear() {
    for (size_t i = 0; i < kIndirectCacheEntries; ++i) __atomic_store_n(&g_indirect_cache[i], uint64_t(0), __ATOMIC_RELAXED);
}

inline void indirect_cache_fill(uint32_t guest, const void* host) {
    if (!host || uintptr_t(host) > 0xFFFFFFFFull) return;
    __atomic_store_n(&g_indirect_cache[indirect_cache_index(guest)], (uint64_t(guest) << 32) | uint32_t(uintptr_t(host)),
                     __ATOMIC_RELAXED);
}

// The host function cached for `guest`, or nullptr.
inline void* indirect_cache_lookup(uint32_t guest) {
    const uint64_t entry = __atomic_load_n(&g_indirect_cache[indirect_cache_index(guest)], __ATOMIC_RELAXED);
    return (uint32_t(entry >> 32) == guest && uint32_t(entry) != 0) ? reinterpret_cast<void*>(uintptr_t(uint32_t(entry))) : nullptr;
}

}  // namespace rcomp
