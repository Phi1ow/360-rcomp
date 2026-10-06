// Shared interface (owner: PRIME). Guest CPU write tracking for GPU caches.
//
// The Xbox 360 GPU reads the 512 MiB of physical memory by physical address.
// rexglue's GPU caches (textures, vertex/index data, shared memory) learn that
// the CPU changed what they cached through watched pages. Xenia catches the
// write faults of protected pages; PS5 titles cannot, so every guest write to
// the physical windows records its 4 KiB physical page here instead:
//   * AOT code through include/rcomp/ppc_prelude.h: PPC_STORE_*, PPC_VSTORE128
//     and the PPC_WRITE_NOTIFY that cpu/patches/xenonrecomp/0016 emits after
//     dcbz/dcbzl, successful stwcx./stdcx. and setjmp;
//   * runtime HLE code that writes guest memory through note_guest_write*().
// The GPU consumes the marks at each command-stream kick
// (gpu/xenos/rexglue/shim/rex/system/xmemory.h) and invalidates only the
// watched pages that were written.
//
// Ordering: a writer marks the page after its data store (release store).
// The consumer takes each mark with an atomic exchange before it invalidates,
// so data written before a consumed mark is visible to the upload that
// follows, and a mark set later remains for the next kick. A title publishes
// its data before it moves the ring write pointer, as it must on the console.
//
// Store comparison (title build option RCOMP_M6_STORE_COMPARE, RCOMP_STORE_COMPARE in the generated code).
// A store that writes the bytes the memory already holds changes nothing a GPU cache could have
// missed, so the compiled stores inside the windows load the old bytes first and call
// note_guest_window_store, which marks the page only if the store changed them. The one exception
// is a page the GPU has written (a resolve, a memory export): its bytes in the GPU's copy are not the
// bytes in guest memory, and on the console a CPU store to it wins whatever it writes, so every store
// to such a page marks it. The command processor keeps one byte per page for that
// (g_guest_physical_gpu_written, written by that thread only, read by the guest threads without a
// lock: SharedMemory::MakeRangeValid sets it with written_by_gpu and clears it for a page made valid
// from memory or invalidated). Writes whose old bytes are not known (dcbz, store-conditional,
// setjmp, runtime writes) always mark.
#pragma once

#include <stdint.h>

namespace rcomp {

constexpr uint32_t kGuestWritePageShift = 12;
constexpr uint32_t kGuestWritePhysicalSize = 0x20000000u;  // 512 MiB
constexpr uint32_t kGuestWritePages = kGuestWritePhysicalSize >> kGuestWritePageShift;
// Guest windows aliasing physical memory: 0xA0000000, 0xC0000000, 0xE0000000
// (include/rcomp/guest_memory.h). Physical P is visible at each base + P.
constexpr uint32_t kGuestWriteWindowBase = 0xA0000000u;
constexpr uint64_t kGuestWriteWindowEnd = 0x100000000ull;
constexpr uint32_t kGuestWriteWindowSpan = uint32_t(kGuestWriteWindowEnd - kGuestWriteWindowBase);
// Console layout of the 4 KiB-page window (title build option RCOMP_PHYSICAL_4K_WINDOW_OFFSET,
// GuestMemory::physical_4k_offset()): guest 0xE0000000 + X is physical X + 0x1000 (modulo 512 MiB).
constexpr uint32_t kPhysical4KWindowBase = 0xE0000000u;
constexpr uint32_t kPhysical4KWindowShift = 0x1000u;

// One byte per physical page: nonzero = written since the last consumption.
// An inline variable, so every translation unit and library shares one table.
alignas(64) inline uint8_t g_guest_physical_written[kGuestWritePages];
// One byte per physical page: nonzero = the GPU wrote the page (see above). Set and cleared by the
// command processor thread only.
alignas(64) inline uint8_t g_guest_physical_gpu_written[kGuestWritePages];

inline uint32_t guest_write_page_index(uint32_t physical) {
    return (physical & (kGuestWritePhysicalSize - 1)) >> kGuestWritePageShift;
}

inline void note_guest_physical_page(uint32_t physical) {
    __atomic_store_n(&g_guest_physical_written[guest_write_page_index(physical)], uint8_t(1), __ATOMIC_RELEASE);
}

// A guest store of `size` bytes (1..4096) at guest address `address`.
inline void note_guest_write(uint32_t address, uint32_t size) {
    const uint32_t offset = address - kGuestWriteWindowBase;
    if (__builtin_expect(offset < kGuestWriteWindowSpan, 0)) {
        note_guest_physical_page(offset);
        note_guest_physical_page(offset + size - 1);
    }
}

// A compared guest store of `size` bytes (1..4096) at window offset `offset` (guest address minus
// kGuestWriteWindowBase, already known to lie inside the windows): `changed` tells whether it wrote
// bytes different from the ones that were there. The data store has been done.
inline void note_guest_window_store(uint32_t offset, uint32_t size, bool changed) {
    const uint32_t first = guest_write_page_index(offset);
    const uint32_t last = guest_write_page_index(offset + size - 1);
    if (changed || (g_guest_physical_gpu_written[first] | g_guest_physical_gpu_written[last])) {
        __atomic_store_n(&g_guest_physical_written[first], uint8_t(1), __ATOMIC_RELEASE);
        __atomic_store_n(&g_guest_physical_written[last], uint8_t(1), __ATOMIC_RELEASE);
    }
}

// Command processor thread: the pages [first_page, first_page + count) (4 KiB physical pages) are now
// written by the GPU, or hold what guest memory holds.
inline void note_gpu_written_pages(uint32_t first_page, uint32_t count, bool gpu_written) {
    if (first_page >= kGuestWritePages) return;
    if (count > kGuestWritePages - first_page) count = kGuestWritePages - first_page;
    __builtin_memset(&g_guest_physical_gpu_written[first_page], gpu_written ? 1 : 0, count);
}

// Any guest range, e.g. a file read or an HLE fill (runtime code).
inline void note_guest_write_range(uint64_t address, uint64_t size) {
    if (!size) return;
    uint64_t begin = address, end = address + size;
    if (end <= kGuestWriteWindowBase || begin >= kGuestWriteWindowEnd) return;
    if (begin < kGuestWriteWindowBase) begin = kGuestWriteWindowBase;
    if (end > kGuestWriteWindowEnd) end = kGuestWriteWindowEnd;
    const uint64_t page = uint64_t(1) << kGuestWritePageShift;
    if (end - begin >= kGuestWritePhysicalSize) {
        for (uint32_t p = 0; p < kGuestWritePages; ++p) note_guest_physical_page(p << kGuestWritePageShift);
        return;
    }
    for (uint64_t a = begin & ~(page - 1); a < end; a += page)
        note_guest_physical_page(uint32_t(a - kGuestWriteWindowBase));
}

// The same for a title running with the console's 4 KiB-page window shift when `physical_4k_offset`
// is true (pass GuestMemory::physical_4k_offset()): the part of the range in the 0xE0000000 window
// marks physical X + 0x1000 for guest 0xE0000000 + X, the bytes GuestMemory::translate() reached.
// With `physical_4k_offset` false it is note_guest_write_range(address, size).
inline void note_guest_write_range(uint64_t address, uint64_t size, bool physical_4k_offset) {
    if (!size) return;
    if (!physical_4k_offset || address + size <= kPhysical4KWindowBase) {
        note_guest_write_range(address, size);
        return;
    }
    if (address < kPhysical4KWindowBase) {
        note_guest_write_range(address, kPhysical4KWindowBase - address);
        size -= kPhysical4KWindowBase - address;
        address = kPhysical4KWindowBase;
    }
    const uint64_t end = address + size < kGuestWriteWindowEnd ? address + size : kGuestWriteWindowEnd;
    if (address >= end) return;
    const uint64_t page = uint64_t(1) << kGuestWritePageShift;
    if (end - address >= kGuestWritePhysicalSize) {
        for (uint32_t p = 0; p < kGuestWritePages; ++p) note_guest_physical_page(p << kGuestWritePageShift);
        return;
    }
    // guest_write_page_index reduces modulo 512 MiB: the window's last page marks physical page 0.
    for (uint64_t a = address & ~(page - 1); a < end; a += page)
        note_guest_physical_page(uint32_t(a - kGuestWriteWindowBase + kPhysical4KWindowShift));
}

}  // namespace rcomp
