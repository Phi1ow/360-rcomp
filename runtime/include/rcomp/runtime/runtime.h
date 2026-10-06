// Process-wide guest runtime services used by the HLE exports
// (owner: Agent 3, runtime/).
#pragma once

#include <stdint.h>
#include <vector>
#include <atomic>
#include <list>
#include <mutex>
#include <set>
#include <memory>

#include "rcomp/runtime/guest_heap.h"
#include "rcomp/runtime/handle_table.h"
#include "rcomp/runtime/status.h"
#include "rcomp/runtime/vfs.h"
#include "rcomp/runtime/xex_loader.h"
#include "rcomp/xenos_gpu.h"

namespace rcomp {
class GuestMemory;
}

namespace rcomp::rt {
struct ModuleState;
struct XamState;

struct RuntimeConfig {
    uint32_t heap_lo = GuestHeap::kSmallPageLo;
    uint32_t heap_hi = GuestHeap::kDefaultHi;
    uint32_t max_handles = 4096;
    // Physical memory (MmAllocatePhysicalMemoryEx): the GPU-visible window,
    // physical address P = guest address phys_lo + P (include/rcomp/xenos_gpu.h).
    uint32_t phys_lo = xenos::kXenosPhysicalWindow;
    uint32_t phys_hi = xenos::kXenosPhysicalWindow + xenos::kXenosPhysicalSize;
    // Console layout of the physical windows (title build option RCOMP_PHYSICAL_4K_WINDOW_OFFSET, which
    // must match the generated code): 4 KiB-page blocks in 0xE0000000 with the one-page shift, 16 MiB
    // ones in 0xC0000000 (src/physical_window.h). Off: every block at its 0xA0000000 address.
    bool physical_4k_window_offset = false;
};

struct Runtime {
    explicit Runtime(uint32_t max_handles);
    ~Runtime();
    GuestMemory* mem = nullptr;
    uint64_t generation = 0;  // host lifetime identity, not an Xbox ABI field
    GuestHeap heap;
    GuestHeap physical;  // physical allocations, in the GPU window
    // Owned by this lifetime, never a process-static list that can admit an
    // allocation from the previous title. The heap also holds stacks/VA, so
    // ExFreePool must consult this separate ownership list.
    struct PoolAllocation { uint32_t address, requested_size, tag; };
    std::mutex pool_mutex;
    std::list<PoolAllocation> pool_allocations;
    // Strings allocated by RtlUnicodeStringToAnsiString are distinct from
    // pool, stack and virtual-memory allocations, including at shutdown.
    struct AnsiStringAllocation { uint32_t address, requested_size; };
    std::mutex ansi_string_mutex;
    std::list<AnsiStringAllocation> ansi_string_allocations;
    std::mutex virtual_allocation_mutex;
    std::set<uint32_t> virtual_allocations;
    std::mutex physical_allocation_mutex;
    std::set<uint32_t> physical_allocations;
    HandleTable handles;
    Vfs vfs;
    XexTlsInfo static_tls;
    std::vector<uint8_t> static_tls_template;
    std::atomic<bool> static_tls_frozen{false};
    std::unique_ptr<ModuleState> modules;
    std::unique_ptr<XamState> xam;
    // Last FscSetCacheElementCount(0, Count) of this title (4 KiB elements).
    // Recorded only: the VFS has no file-system cache to resize.
    std::atomic<uint32_t> fsc_cache_elements{0};
};

// `mem` must be reserved (GuestMemory::reserve) and outlive the runtime.
// Runtime ranges (function table) must be reserved *before* this call so the
// heap excludes them.
Status runtime_init(GuestMemory* mem, const RuntimeConfig& cfg = RuntimeConfig());
// Bootstrap-only: configure before creating threads. Copy the loaded template
// so later guest writes cannot change initial values of future threads.
Status runtime_set_static_tls(const XexTlsInfo& tls);
// Non-blocking dispatcher cancellation, valid from an active guest thread.
void runtime_request_thread_quiesce();
// True once that cancellation was requested (dispatcher waits then end at once).
bool runtime_thread_quiesce_requested();
// Host/owner phase after the initial guest entry has unwound.
void runtime_finish_thread_quiesce();
// Request cancellation of all title work, including blocking network I/O.
// This is the lifecycle callback used by guest title-termination exports.
void runtime_request_title_quiesce();
// Stop admission, wake suspended workers/waits, and await worker exit.
// The caller must have joined the initial thread first. AOT code that never
// returns or waits may delay quiescence; memory stays alive until it exits.
void runtime_quiesce_threads();
void runtime_shutdown();
Runtime* runtime();  // nullptr before runtime_init

// Registers the real xboxkrnl implementations of src/hle_xboxkrnl.cpp and
// src/hle_xboxkrnl_threads.cpp in the import registry. Everything else stays
// unresolved and traps.
Status register_xboxkrnl_hle();
Status register_xboxkrnl_threading_hle();  // called by register_xboxkrnl_hle
Status register_xboxkrnl_memory_hle();     // called by register_xboxkrnl_hle
Status register_xboxkrnl_pool_rtl_hle();   // called by register_xboxkrnl_hle
Status register_xboxkrnl_time_hle();       // called by register_xboxkrnl_hle
Status register_xboxkrnl_sched_hle();      // called by register_xboxkrnl_hle
Status register_xboxkrnl_strings_hle();    // called by register_xboxkrnl_hle
// xam.xex implementations of src/hle_xam.cpp (controller input). The title
// must then link an input backend (include/rcomp/input.h).
Status register_xam_hle();

// Guest pointer helpers for HLE code (checked through GuestMemory::translate).
bool guest_read_be32(uint32_t addr, uint32_t* v);
bool guest_write_be32(uint32_t addr, uint32_t v);
bool guest_read_be16(uint32_t addr, uint16_t* v);
bool guest_read_be64(uint32_t addr, uint64_t* v);
bool guest_write_be64(uint32_t addr, uint64_t v);

}  // namespace rcomp::rt
