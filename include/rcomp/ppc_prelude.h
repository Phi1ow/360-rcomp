// Shared interface (owner: PRIME; implementation: cpu/, Agent 1).
//
// Force-included (-include rcomp/ppc_prelude.h) in front of every
// XenonRecomp-generated translation unit, *before* ppc_context.h. It only
// overrides macros that ppc_context.h guards with #ifndef, so the generated
// C++ is never edited by hand.
//
// What it changes versus upstream ppc_context.h:
//   * PPC_CALL_INDIRECT_FUNC: upstream loads a host pointer from the lookup
//     table and calls it unchecked (null or garbage -> jump to an arbitrary
//     address). Here the target is validated against the registered code
//     range and table; an invalid target ends in rcomp_fatal() with the
//     target address and the guest link register.
//
// RCOMP_VIRTUAL_GUEST_ACCESS enables bounded scalar fields in the reserved
// kernel/object arena. Ordinary guest RAM retains an inline volatile access.
// RCOMP_CHECKED_GUEST_ACCESS additionally validates guest page permissions.
// RCOMP_PHYSICAL_4K_WINDOW_OFFSET (0 or 1, default 0) shifts the 0xE0000000
// window by one 4 KiB page as the console does (see PPC_HOST_PTR below).
#pragma once

#include <stdint.h>
#include <time.h>

#include "rcomp/guest_write_tracking.h"
#include "rcomp/indirect_cache.h"

struct PPCContext;

namespace rcomp {
[[noreturn]] void bad_guest_access(uint32_t addr, uint32_t size, int is_store);
bool guest_access_ok(uint32_t addr, uint32_t size);
bool guest_access_permitted(uint32_t addr, uint32_t size, int is_store);
}  // namespace rcomp

// Defined in cpu/runtime/indirect.cpp.
void rcomp_call_indirect(PPCContext& ctx, uint8_t* base, uint32_t target);
// Returns only for the documented debug-print service (twi 31,r0,20); any other
// taken trap stops the process.
void rcomp_guest_trap(PPCContext& ctx, uint32_t guest_pc);
[[noreturn]] void rcomp_unresolved_import(PPCContext& ctx, const char* module, uint32_t ordinal);
[[noreturn]] void rcomp_call_unknown(PPCContext& ctx, uint32_t target);
[[noreturn]] void rcomp_switch_out_of_range(PPCContext& ctx, uint32_t guest_pc, uint32_t index);

// Physical 4 KiB-page window offset (title build option, 0 or 1, default 0).
// On the console the CPU window 0xE0000000 (4 KiB pages) is one page off the
// other two: guest address 0xE0000000 + X is physical X + 0x1000 (Xenia:
// Memory::TranslateVirtual and MmGetPhysicalAddress add 0x1000 there; XDK D3D
// in title code converts CPU to GPU addresses as
// (addr & 0x1FFFFFFF) + (addr >= 0xE0000000 ? 0x1000 : 0)). GuestMemory maps
// 0xC0000000 and 0xE0000000 as zero-offset aliases of 0xA0000000
// (rcomp/guest_memory.h; a 4 KiB host shift is impossible with 16 KiB host
// pages), so with the option at 1 the generated code applies the shift itself:
//   host = base + ea + (ea >= 0xE0000000 ? 0x1000 : 0)
// The last page of the window reaches [base + 4 GiB, base + 4 GiB + 0x1000):
// the host must map it as physical 0 (the page after physical 512 MiB - 4 KiB,
// modulo 512 MiB, which is also what the write tracking below marks).
// Requires code from a generator with cpu/patches/xenonrecomp/0020 (every raw
// guest data pointer it emits goes through PPC_HOST_PTR; checked at compile
// time through PPC_HOST_PTR_EMITTED, which that generator writes in
// ppc_config.h). Guest addresses given to the runtime stay unshifted guest
// addresses: fatal reports, virtual providers, and the permission checks of
// RCOMP_CHECKED_GUEST_ACCESS (guest_access_permitted -> GuestMemory::
// is_accessible, which applies the same shift itself when the runtime runs
// with GuestMemory::set_physical_4k_offset(true); the title must set it from
// this same option, see app/m6). Write tracking marks the physical page the
// shifted access reaches.
// At 0 every macro below expands to what it was before the option existed.
#ifndef RCOMP_PHYSICAL_4K_WINDOW_OFFSET
#define RCOMP_PHYSICAL_4K_WINDOW_OFFSET 0
#endif
#if RCOMP_PHYSICAL_4K_WINDOW_OFFSET != 0 && RCOMP_PHYSICAL_4K_WINDOW_OFFSET != 1
#error "RCOMP_PHYSICAL_4K_WINDOW_OFFSET must be 0 or 1"
#endif

namespace rcomp {
// 0x1000 for a guest address in the 0xE0000000 window, 0 elsewhere (branch free).
inline uint32_t physical_4k_window_shift(uint32_t ea) {
    return uint32_t(ea >= kPhysical4KWindowBase) << 12;
}
// Host offset from `base` of guest address `ea`, at most 4 GiB + 0xFFF.
inline uint64_t physical_4k_host_offset(uint32_t ea) {
    return uint64_t(ea) + physical_4k_window_shift(ea);
}
// GPU write tracking of a shifted store of `size` bytes (1..4096) at guest
// address `address`: a store to 0xE0000000 + X writes physical X + 0x1000, so
// that page is marked (guest_write_page_index reduces modulo 512 MiB).
inline void note_shifted_guest_write(uint32_t address, uint32_t size) {
    const uint32_t offset = address - kGuestWriteWindowBase;
    if (__builtin_expect(offset < kGuestWriteWindowSpan, 0)) {
        const uint32_t physical = offset + physical_4k_window_shift(address);
        note_guest_physical_page(physical);
        note_guest_physical_page(physical + size - 1);
    }
}
}  // namespace rcomp

#if RCOMP_PHYSICAL_4K_WINDOW_OFFSET
#define RCOMP_HOST_OFFSET(ea) rcomp::physical_4k_host_offset(uint32_t(ea))
#define RCOMP_NOTE_GUEST_WRITE(ea, size) rcomp::note_shifted_guest_write((ea), (size))
// Window offset (guest address - kGuestWriteWindowBase) of a compared store, as
// note_guest_window_store expects it: shifted by the page in the 0xE0000000 window.
#define RCOMP_WINDOW_STORE_OFFSET(offset, ea) ((offset) + rcomp::physical_4k_window_shift(ea))
// Seen as `false` unless ppc_config.h (generator patch 0020) defines the macro
// of the same name to 1 before the function bodies that test it.
#ifndef PPC_HOST_PTR_EMITTED
constexpr bool PPC_HOST_PTR_EMITTED = false;
#endif
#define RCOMP_REQUIRE_HOST_PTR_GENERATOR() \
    static_assert(PPC_HOST_PTR_EMITTED, "RCOMP_PHYSICAL_4K_WINDOW_OFFSET=1 needs code generated with XenonRecomp patch 0020");
#else
#define RCOMP_HOST_OFFSET(ea) (ea)
#define RCOMP_NOTE_GUEST_WRITE(ea, size) rcomp::note_guest_write((ea), (size))
#define RCOMP_WINDOW_STORE_OFFSET(offset, ea) (offset)
#define RCOMP_REQUIRE_HOST_PTR_GENERATOR()
#endif
// Emitted by cpu/patches/xenonrecomp/0020 wherever the generated code forms a
// host pointer to guest data itself (vector loads, dcbz/dcbzl, lwarx/ldarx,
// stwcx./stdcx., setjmp/longjmp); the macros below use it too.
#define PPC_HOST_PTR(x) (base + RCOMP_HOST_OFFSET(x))

// The indirect-call lookup behind rcomp_call_indirect took 4.8 % of the main thread's samples of Episodes from Liberty City
// (instruction-level profile, 4 October 2026): the generated code tries the direct-mapped cache of rcomp/indirect_cache.h first.
namespace rcomp {
static inline void indirect_call_cached(PPCContext& ctx, uint8_t* base, uint32_t target) {
    void* const host = indirect_cache_lookup(target);
    if (__builtin_expect(host != nullptr, 1)) {
        reinterpret_cast<void (*)(PPCContext&, uint8_t*)>(host)(ctx, base);
        return;
    }
    rcomp_call_indirect(ctx, base, target);
}
}  // namespace rcomp
#define PPC_CALL_INDIRECT_FUNC(x) rcomp::indirect_call_cached(ctx, base, (uint32_t)(x))
// Emitted by cpu/patches/xenonrecomp/0001 for a taken tw/twi/td/tdi.
#define PPC_TRAP(addr) rcomp_guest_trap(ctx, (uint32_t)(addr))
// Emitted by cpu/patches/xenonrecomp/0005: an import no export table names
// (module is the XEX library name with non-alphanumerics as '_'), and a direct
// call to an address with no recompiled function.
#define PPC_UNRESOLVED_IMPORT(module, ordinal) rcomp_unresolved_import(ctx, module, (uint32_t)(ordinal))
#define PPC_CALL_UNKNOWN(address) rcomp_call_unknown(ctx, (uint32_t)(address))
// Emitted by cpu/patches/xenonrecomp/0013 as the `default` of a recognised
// jump table: an index outside the analysed table stops with its guest pc
// instead of reaching __builtin_unreachable().
#define PPC_SWITCH_OUT_OF_RANGE(guest_pc, index) rcomp_switch_out_of_range(ctx, (uint32_t)(guest_pc), (uint32_t)(index))
// Emitted by cpu/patches/xenonrecomp/0016 after guest writes that bypass
// PPC_STORE_* (vector stores, dcbz/dcbzl, successful stwcx./stdcx., setjmp):
// records written physical pages for the GPU caches (rcomp/guest_write_tracking.h).
#define PPC_WRITE_NOTIFY(x, size) RCOMP_NOTE_GUEST_WRITE((uint32_t)(x), (uint32_t)(size))
#if defined(RCOMP_STORE_COMPARE)
#if !defined(RCOMP_VIRTUAL_GUEST_ACCESS)
#error "RCOMP_STORE_COMPARE needs RCOMP_VIRTUAL_GUEST_ACCESS (the scalar stores below)"
#endif
// Emitted by cpu/patches/xenonrecomp/0017 for stvx/stvx128: an aligned 16-byte store inside the windows
// loads the old bytes first and records a change only if they differ (rcomp/guest_write_tracking.h). The
// other writes that bypass PPC_STORE_* (PPC_WRITE_NOTIFY above) do not know the old bytes and count as changes.
#define PPC_VSTORE128(ea, value) do { \
    const uint32_t rcomp_vea_ = (ea); \
    simde__m128i* const rcomp_vp_ = (simde__m128i*)PPC_HOST_PTR(rcomp_vea_); \
    const simde__m128i rcomp_vnew_ = (value); \
    const uint32_t rcomp_voff_ = rcomp_vea_ - rcomp::kGuestWriteWindowBase; \
    if (__builtin_expect(rcomp_voff_ < rcomp::kGuestWriteWindowSpan, 0)) { \
        const simde__m128i rcomp_vold_ = simde_mm_load_si128(rcomp_vp_); \
        simde_mm_store_si128(rcomp_vp_, rcomp_vnew_); \
        rcomp::note_guest_window_store(RCOMP_WINDOW_STORE_OFFSET(rcomp_voff_, rcomp_vea_), 16, simde_mm_movemask_epi8(simde_mm_cmpeq_epi8(rcomp_vold_, rcomp_vnew_)) != 0xFFFF); \
    } else { \
        simde_mm_store_si128(rcomp_vp_, rcomp_vnew_); \
    } } while (0)
#elif RCOMP_PHYSICAL_4K_WINDOW_OFFSET
// The ppc_context.h default of patch 0017 with the shifted pointer and tracking, whichever ppc_context.h the
// generated code carries.
#define PPC_VSTORE128(ea, value) do { const uint32_t rcomp_vea_ = (ea); \
    simde_mm_store_si128((simde__m128i*)PPC_HOST_PTR(rcomp_vea_), (value)); \
    PPC_WRITE_NOTIFY(rcomp_vea_, 16); } while (0)
#endif

// Diagnostic titles only (RCOMP_DIAGNOSTIC_FUNCTION_RING): every recompiled
// function records its name in a per-thread ring read by the sampling
// profiler and crash reports (cpu/runtime/indirect.cpp). Overrides the
// prologue that cpu/patches/xenonrecomp/0014 makes overridable.
// With RCOMP_PHYSICAL_4K_WINDOW_OFFSET=1 the prologue also checks at compile
// time that the generator emitted PPC_HOST_PTR (patch 0020).
#if defined(RCOMP_DIAGNOSTIC_FUNCTION_RING)
extern "C" thread_local const char* rcomp_t_fn;
extern "C" thread_local const char* rcomp_t_ring[256];
extern "C" thread_local unsigned rcomp_t_idx;
#define PPC_FUNC_PROLOGUE()     RCOMP_REQUIRE_HOST_PTR_GENERATOR() __builtin_assume(((size_t)base & 0x1F) == 0); rcomp_t_fn = __func__; rcomp_t_ring[rcomp_t_idx++ & 255] = __func__
#elif RCOMP_PHYSICAL_4K_WINDOW_OFFSET
#define PPC_FUNC_PROLOGUE() RCOMP_REQUIRE_HOST_PTR_GENERATOR() __builtin_assume(((size_t)base & 0x1F) == 0)
#endif

// Emitted by cpu/patches/xenonrecomp/0007 for mftb: the Xenon timebase runs
// at 50 MHz (what KeQueryPerformanceFrequency reports); derived from the host
// monotonic clock instead of the host TSC.
#define RCOMP_GUEST_TIMEBASE_HZ 50000000ull
// On x86-64 the invariant TSC, scaled to 50 MHz by cpu/runtime/timebase.cpp,
// replaces clock_gettime: on the PS5 that is a system call, and titles poll
// mftb in tight timing loops. Until calibration has run the clock is used.
extern "C" uint64_t rcomp_tsc_origin, rcomp_tb_origin, rcomp_tsc_to_tb;  // 32.32 fixed point
static inline uint64_t rcomp_guest_timebase() {
#if defined(__x86_64__)
    if (__builtin_expect(rcomp_tsc_to_tb != 0, 1)) {
        const uint64_t delta = __builtin_ia32_rdtsc() - rcomp_tsc_origin;
        return rcomp_tb_origin + uint64_t((unsigned __int128)delta * rcomp_tsc_to_tb >> 32);
    }
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * RCOMP_GUEST_TIMEBASE_HZ + (uint64_t)ts.tv_nsec / (1000000000ull / RCOMP_GUEST_TIMEBASE_HZ);
}
#define PPC_MFTB() rcomp_guest_timebase()

// Emitted by cpu/patches/xenonrecomp/0023 for every integer division: divw, divwu,
// divd, divdu and their record (.) and overflow-enable (o) forms. The quotient goes
// through PPC_DIVW / PPC_DIVWU / PPC_DIVD / PPC_DIVDU; the OE forms set XER[OV] from
// PPC_*_OVERFLOW (and OR it into XER[SO]) before rD is written.
// On Xenon a zero divisor, or INT_MIN / -1 (INT64_MIN / -1) for the signed forms,
// does not trap: the PowerPC architecture leaves the quotient undefined (and sets OV
// for the OE forms). Titles compute such quotients speculatively and discard them
// (Halo 3 sub_824E2AF8 divides before testing the divisor). A raw C++ division of
// those operands raises SIGFPE on x86-64 (the PS5). These helpers never trap:
//   divisor 0          -> 0
//   INT_MIN / -1       -> INT_MIN (two's complement wrap of the true quotient)
// These values are R-comp's deterministic choice, not a claim about what the Xenon
// hardware returns; a title that consumes an undefined quotient is not expected to
// get the console's value. Any other operands give the C++ (truncating) quotient,
// identical to the PowerPC result. The defaults in ppc_context.h (patch 0023) give
// the same values; this definition takes precedence (all eight macros together).
namespace rcomp {
inline int32_t ppc_divw(int32_t a, int32_t b) {
    if (__builtin_expect(b == 0, 0)) return 0;
    if (__builtin_expect(b == -1, 0)) return int32_t(0u - uint32_t(a));  // INT32_MIN / -1 wraps
    return a / b;
}
inline uint32_t ppc_divwu(uint32_t a, uint32_t b) {
    return __builtin_expect(b == 0, 0) ? 0u : a / b;
}
inline int64_t ppc_divd(int64_t a, int64_t b) {
    if (__builtin_expect(b == 0, 0)) return 0;
    if (__builtin_expect(b == -1, 0)) return int64_t(0ull - uint64_t(a));  // INT64_MIN / -1 wraps
    return a / b;
}
inline uint64_t ppc_divdu(uint64_t a, uint64_t b) {
    return __builtin_expect(b == 0, 0) ? 0ull : a / b;
}
// XER[OV] of divwo / divdo: the operands whose quotient the architecture leaves undefined.
inline uint8_t ppc_divw_overflow(int32_t a, int32_t b) { return b == 0 || (b == -1 && a == INT32_MIN); }
inline uint8_t ppc_divd_overflow(int64_t a, int64_t b) { return b == 0 || (b == -1 && a == INT64_MIN); }
}  // namespace rcomp
#define PPC_DIVW(a, b) rcomp::ppc_divw((a), (b))
#define PPC_DIVWU(a, b) rcomp::ppc_divwu((a), (b))
#define PPC_DIVD(a, b) rcomp::ppc_divd((a), (b))
#define PPC_DIVDU(a, b) rcomp::ppc_divdu((a), (b))
#define PPC_DIVW_OVERFLOW(a, b) rcomp::ppc_divw_overflow((a), (b))
#define PPC_DIVWU_OVERFLOW(a, b) uint8_t(uint32_t(b) == 0)
#define PPC_DIVD_OVERFLOW(a, b) rcomp::ppc_divd_overflow((a), (b))
#define PPC_DIVDU_OVERFLOW(a, b) uint8_t(uint64_t(b) == 0)

#if defined(RCOMP_VIRTUAL_GUEST_ACCESS)
namespace rcomp {
// These are address-space identities, not host addresses or committed pages.
// cpu/runtime/guest_access.cpp checks them against the runtime provider ABI.
constexpr uint32_t kVirtualGuestArenaBase = 0x70000000u;
constexpr uint64_t kVirtualGuestArenaEnd = 0x71000000ull;
uint64_t virtual_guest_load(uint32_t address, uint8_t width, uint32_t lr);
void virtual_guest_store(uint32_t address, uint8_t width, uint64_t value, uint32_t lr);

// Memory-mapped device windows served by runtime providers, besides the opaque
// object arena: the XMA decoder register file (runtime/src/hle_xboxkrnl_xma.cpp).
constexpr uint64_t kVirtualXmaRegistersBase = 0x7FEA0000u;
constexpr uint64_t kVirtualXmaRegistersEnd = 0x7FEA4000u;
// The exact test: does an access of `width` bytes touch one of the two windows? Applied out of
// line (cpu/runtime/guest_access.cpp), not by the generated code.
inline bool touches_virtual_guest_arena(uint32_t address, uint32_t width) {
    return (uint64_t(address) < kVirtualGuestArenaEnd &&
            uint64_t(address) + width > kVirtualGuestArenaBase) ||
           (uint64_t(address) < kVirtualXmaRegistersEnd &&
            uint64_t(address) + width > kVirtualXmaRegistersBase);
}

// The test the generated code makes on every load and store is one range that contains both
// windows, [0x6FFFFFF0, 0x80000000): a subtraction and a compare instead of two range tests (about
// a sixth of the generated .text, docs/OPTIMIZATION_AUDIT_20260930.md C1). The margin of 16 bytes
// below the arena is the reach of the widest access that can start before it. An access inside the
// range goes through the out-of-line functions, which apply the exact test and the provider
// dispatch above, and perform the ordinary access for an address no window claims (the upper part
// of the guest heap lies inside the range).
constexpr uint32_t kVirtualCoarseBase = 0x6FFFFFF0u;
constexpr uint32_t kVirtualCoarseSize = 0x10000010u;
static_assert(uint64_t(kVirtualCoarseBase) + kVirtualCoarseSize == 0x80000000ull, "the coarse range ends at 2 GiB");
static_assert(kVirtualCoarseBase + 16 == kVirtualGuestArenaBase && kVirtualXmaRegistersEnd <= 0x80000000ull,
              "the coarse range contains both exact windows with the margin of the widest access");
inline bool in_virtual_coarse_range(uint32_t address) {
    return __builtin_expect(uint32_t(address - kVirtualCoarseBase) < kVirtualCoarseSize, 0);
}
uint64_t virtual_window_load(uint8_t* base, uint32_t address, uint8_t width, uint32_t lr);
void virtual_window_store(uint8_t* base, uint32_t address, uint8_t width, uint64_t value, uint32_t lr);

// Packed/may_alias avoids alignment and strict-aliasing UB for legal unaligned
// guest scalar accesses, while retaining one volatile host scalar access.
template <typename T> struct __attribute__((packed, may_alias)) GuestScalar {
    T value;
};
template <typename T> inline T guest_scalar_endian(T value) {
    if constexpr (sizeof(T) == 1) return value;
    else if constexpr (sizeof(T) == 2) return __builtin_bswap16(value);
    else if constexpr (sizeof(T) == 4) return __builtin_bswap32(value);
    else return __builtin_bswap64(value);
}
template <typename T>
inline T generated_guest_load(uint8_t* base, uint32_t address, uint32_t lr) {
    if (in_virtual_coarse_range(address))
        return T(virtual_window_load(base, address, sizeof(T), lr));
#if defined(RCOMP_CHECKED_GUEST_ACCESS)
    if (!guest_access_permitted(address, sizeof(T), 0))
        bad_guest_access(address, sizeof(T), 0);
#endif
    return guest_scalar_endian(reinterpret_cast<volatile GuestScalar<T>*>(PPC_HOST_PTR(address))->value);
}
template <typename T>
inline void generated_guest_store(uint8_t* base, uint32_t address, T value, uint32_t lr) {
    if (in_virtual_coarse_range(address)) {
        virtual_window_store(base, address, sizeof(T), value, lr);
        return;
    }
#if defined(RCOMP_CHECKED_GUEST_ACCESS)
    if (!guest_access_permitted(address, sizeof(T), 1))
        bad_guest_access(address, sizeof(T), 1);
#endif
#if defined(RCOMP_STORE_COMPARE)
    // Inside the physical windows the old bytes are loaded first (the line is about to be written anyway):
    // the page is recorded as changed only if the store writes different bytes.
    volatile GuestScalar<T>* const target = reinterpret_cast<volatile GuestScalar<T>*>(PPC_HOST_PTR(address));
    const T swapped = guest_scalar_endian(value);
    const uint32_t offset = address - kGuestWriteWindowBase;
    if (__builtin_expect(offset < kGuestWriteWindowSpan, 0)) {
        const T old = target->value;
        target->value = swapped;
        note_guest_window_store(RCOMP_WINDOW_STORE_OFFSET(offset, address), sizeof(T), old != swapped);
    } else {
        target->value = swapped;
    }
#else
    reinterpret_cast<volatile GuestScalar<T>*>(PPC_HOST_PTR(address))->value = guest_scalar_endian(value);
    RCOMP_NOTE_GUEST_WRITE(address, sizeof(T));
#endif
}
} // namespace rcomp

// The arguments of each load/store are evaluated exactly once. Values returned
// by a virtual provider already have architectural guest endian semantics.
#define PPC_LOAD_U8(x) rcomp::generated_guest_load<uint8_t>(base, uint32_t(x), uint32_t(ctx.lr))
#define PPC_LOAD_U16(x) rcomp::generated_guest_load<uint16_t>(base, uint32_t(x), uint32_t(ctx.lr))
#define PPC_LOAD_U32(x) rcomp::generated_guest_load<uint32_t>(base, uint32_t(x), uint32_t(ctx.lr))
#define PPC_LOAD_U64(x) rcomp::generated_guest_load<uint64_t>(base, uint32_t(x), uint32_t(ctx.lr))
#define PPC_STORE_U8(x, y) rcomp::generated_guest_store<uint8_t>(base, uint32_t(x), uint8_t(y), uint32_t(ctx.lr))
#define PPC_STORE_U16(x, y) rcomp::generated_guest_store<uint16_t>(base, uint32_t(x), uint16_t(y), uint32_t(ctx.lr))
#define PPC_STORE_U32(x, y) rcomp::generated_guest_store<uint32_t>(base, uint32_t(x), uint32_t(y), uint32_t(ctx.lr))
#define PPC_STORE_U64(x, y) rcomp::generated_guest_store<uint64_t>(base, uint32_t(x), uint64_t(y), uint32_t(ctx.lr))
#elif defined(RCOMP_CHECKED_GUEST_ACCESS)
#define RCOMP_CHK(a, n, st) \
    (rcomp::guest_access_permitted((uint32_t)(a), (n), (st)) ? (void)0 : rcomp::bad_guest_access((uint32_t)(a), (n), (st)))
#define PPC_LOAD_U8(x) (RCOMP_CHK(x, 1, 0), *(volatile uint8_t*)PPC_HOST_PTR(x))
#define PPC_LOAD_U16(x) (RCOMP_CHK(x, 2, 0), __builtin_bswap16(*(volatile uint16_t*)PPC_HOST_PTR(x)))
#define PPC_LOAD_U32(x) (RCOMP_CHK(x, 4, 0), __builtin_bswap32(*(volatile uint32_t*)PPC_HOST_PTR(x)))
#define PPC_LOAD_U64(x) (RCOMP_CHK(x, 8, 0), __builtin_bswap64(*(volatile uint64_t*)PPC_HOST_PTR(x)))
#define PPC_STORE_U8(x, y) (RCOMP_CHK(x, 1, 1), *(volatile uint8_t*)PPC_HOST_PTR(x) = (y), RCOMP_NOTE_GUEST_WRITE((uint32_t)(x), 1))
#define PPC_STORE_U16(x, y) (RCOMP_CHK(x, 2, 1), *(volatile uint16_t*)PPC_HOST_PTR(x) = __builtin_bswap16(y), RCOMP_NOTE_GUEST_WRITE((uint32_t)(x), 2))
#define PPC_STORE_U32(x, y) (RCOMP_CHK(x, 4, 1), *(volatile uint32_t*)PPC_HOST_PTR(x) = __builtin_bswap32(y), RCOMP_NOTE_GUEST_WRITE((uint32_t)(x), 4))
#define PPC_STORE_U64(x, y) (RCOMP_CHK(x, 8, 1), *(volatile uint64_t*)PPC_HOST_PTR(x) = __builtin_bswap64(y), RCOMP_NOTE_GUEST_WRITE((uint32_t)(x), 8))
#else
// Unchecked stores, still recorded for the GPU caches (address evaluated once).
#define RCOMP_TRACKED_STORE(T, swap, x, y) \
    do { const uint32_t rcomp_ea_ = (uint32_t)(x); \
         *(volatile T*)PPC_HOST_PTR(rcomp_ea_) = swap(y); \
         RCOMP_NOTE_GUEST_WRITE(rcomp_ea_, sizeof(T)); } while (0)
#define RCOMP_NO_SWAP(v) (v)
#define PPC_STORE_U8(x, y) RCOMP_TRACKED_STORE(uint8_t, RCOMP_NO_SWAP, x, y)
#define PPC_STORE_U16(x, y) RCOMP_TRACKED_STORE(uint16_t, __builtin_bswap16, x, y)
#define PPC_STORE_U32(x, y) RCOMP_TRACKED_STORE(uint32_t, __builtin_bswap32, x, y)
#define PPC_STORE_U64(x, y) RCOMP_TRACKED_STORE(uint64_t, __builtin_bswap64, x, y)
#if RCOMP_PHYSICAL_4K_WINDOW_OFFSET
// Unchecked loads: the ppc_context.h defaults with the shifted pointer, whichever ppc_context.h the
// generated code carries (with patch 0020 its defaults use PPC_HOST_PTR as well).
#define PPC_LOAD_U8(x) *(volatile uint8_t*)PPC_HOST_PTR(x)
#define PPC_LOAD_U16(x) __builtin_bswap16(*(volatile uint16_t*)PPC_HOST_PTR(x))
#define PPC_LOAD_U32(x) __builtin_bswap32(*(volatile uint32_t*)PPC_HOST_PTR(x))
#define PPC_LOAD_U64(x) __builtin_bswap64(*(volatile uint64_t*)PPC_HOST_PTR(x))
#endif
#endif
