// Physical memory windows (owner: Agent 3, runtime/). The Xbox 360 shows its 512 MiB of physical
// memory through three CPU windows: 0xA0000000 (64 KiB pages), 0xC0000000 (16 MiB pages) and
// 0xE0000000 (4 KiB pages), the last one shifted by a page: virtual 0xE0000000 + X is physical
// X + 0x1000 (XDK Direct3D converts CPU to GPU addresses with that shift).
//
// The physical heap and its bookkeeping use the 0xA0000000 ("canonical") address of a block. With the
// title build option RCOMP_PHYSICAL_4K_WINDOW_OFFSET (RuntimeConfig::physical_4k_window_offset,
// GuestMemory::physical_4k_offset()) MmAllocatePhysicalMemory(Ex) hands the title the address in the
// window of the page size it asked for, with the console's shift, and the generated code and
// GuestMemory::translate() apply the shift on access. Without it every block stays at its 0xA0000000
// address (the behaviour GTA IV / EFLC are tuned with). Services taking a physical address back
// convert it with physical_canonical(). Not a public interface.
#pragma once

#include <stdint.h>

#include "rcomp/guest_memory.h"
#include "rcomp/guest_write_tracking.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/xenos_gpu.h"

namespace rcomp::rt {

constexpr uint32_t kPhysicalWindow64K = 0xA0000000u;
constexpr uint32_t kPhysicalWindow16M = 0xC0000000u;
constexpr uint32_t kPhysicalWindow4K = 0xE0000000u;
constexpr uint32_t kPhysical4KShift = 0x1000u;

// True for an address in any of the three windows.
inline bool in_physical_windows(uint32_t address) { return address >= kPhysicalWindow64K; }

// The physical address behind a window address (what MmGetPhysicalAddress returns).
inline uint32_t physical_of_window(const GuestMemory& mem, uint32_t address) {
    const uint32_t shift = (mem.physical_4k_offset() && address >= kPhysicalWindow4K) ? kPhysical4KShift : 0u;
    return (address + shift) & (xenos::kXenosPhysicalSize - 1);
}

// The 0xA0000000-window address of the same physical byte; other addresses unchanged.
inline uint32_t physical_canonical(const GuestMemory& mem, uint32_t address) {
    return in_physical_windows(address) ? xenos::kXenosPhysicalWindow + physical_of_window(mem, address) : address;
}

// The address a title gets for a block at canonical address `canonical` allocated with `page`-byte pages.
inline uint32_t physical_window_for_page(const GuestMemory& mem, uint32_t canonical, uint64_t page) {
    if (!mem.physical_4k_offset()) return canonical;
    const uint32_t physical = canonical - xenos::kXenosPhysicalWindow;
    if (page == 0x1000 && physical >= kPhysical4KShift) return kPhysicalWindow4K + physical - kPhysical4KShift;
    if (page == 0x1000000) return kPhysicalWindow16M + physical;
    return canonical;
}

// A runtime (HLE) write of `size` bytes at title address `address`: marks the physical pages it reached,
// with the 0xE0000000 shift when the title runs with the console window layout.
inline void note_title_write(uint64_t address, uint64_t size) {
    const Runtime* r = runtime();
    note_guest_write_range(address, size, r && r->mem && r->mem->physical_4k_offset());
}

}  // namespace rcomp::rt
