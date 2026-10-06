// R-comp replacement for rexglue's rex/system/xmemory.h (owner: gpu/xenos).
// It takes precedence through the include order (gpu/xenos/rexglue/shim/
// first). rexglue's Memory owns its own guest address space; in R-comp the
// guest space is GuestMemory (include/rcomp/guest_memory.h), one flat 4 GiB
// block, and the Xbox 360's 512 MiB of physical memory is the guest window
// [kPhysicalWindow, kPhysicalWindow + 512 MiB) where the runtime places
// physical allocations (MmAllocatePhysicalMemoryEx). A GPU (physical)
// address P is therefore guest address kPhysicalWindow + P.
//
// CPU write detection. rexglue (like Xenia) learns that the guest CPU wrote
// memory the GPU cached (textures, vertex data) by write-protecting the
// pages and catching the fault. Signal handlers do not run in a PS5 title,
// so R-comp does it without faults: every guest CPU/HLE write to a physical
// window records its 4 KiB page (include/rcomp/guest_write_tracking.h). The
// GPU caches still register their invalidation callbacks and mark the ranges
// they cached as watched (EnablePhysicalMemoryAccessCallbacks); at each
// command-stream kick (InvalidateWatchedPhysicalMemory, called by the command
// processor before it executes newly submitted packets; patch 0009) exactly
// the watched pages written since the previous kick are invalidated. Data the
// CPU wrote before it moved the write pointer is therefore always seen, and
// unchanged data is not uploaded again.
#pragma once

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/memory/utils.h>
#include <rex/thread/mutex.h>

#include "rcomp/guest_write_tracking.h"

namespace rex::memory {

class Memory {
 public:
  static constexpr uint32_t kPhysicalWindow = 0xA0000000u;
  static constexpr uint32_t kPhysicalSize = 0x20000000u;

  explicit Memory(uint8_t* guest_base) : base_(guest_base) {}

  uint8_t* virtual_membase() const { return base_; }
  uint8_t* physical_membase() const { return base_ + kPhysicalWindow; }

  template <typename T = uint8_t*>
  T TranslateVirtual(uint32_t guest_address) const {
    return reinterpret_cast<T>(base_ + guest_address);
  }
  template <typename T = uint8_t*>
  T TranslatePhysical(uint32_t guest_physical_address) const {
    return reinterpret_cast<T>(physical_membase() + (guest_physical_address & (kPhysicalSize - 1)));
  }
  typedef std::pair<uint32_t, uint32_t> (*PhysicalMemoryInvalidationCallback)(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);

  void* RegisterPhysicalMemoryInvalidationCallback(PhysicalMemoryInvalidationCallback callback,
                                                   void* callback_context) {
    auto* entry = new std::pair<PhysicalMemoryInvalidationCallback, void*>(callback, callback_context);
    std::lock_guard<std::mutex> lk(watch_mu_);
    callbacks_.push_back(entry);
    return entry;
  }
  void UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle) {
    auto* entry = static_cast<std::pair<PhysicalMemoryInvalidationCallback, void*>*>(callback_handle);
    {
      std::lock_guard<std::mutex> lk(watch_mu_);
      callbacks_.erase(std::remove(callbacks_.begin(), callbacks_.end(), entry), callbacks_.end());
    }
    delete entry;
  }
  // Marks [physical_address, +length) (snapped to 4 KiB pages) as cached by
  // the GPU. Data providers are not used (no fault-based reads either).
  void EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                           bool enable_invalidation_notifications,
                                           bool /*enable_data_providers*/) {
    if (!enable_invalidation_notifications || !length) return;
    physical_address &= kPhysicalSize - 1;
    const uint64_t end = std::min<uint64_t>(uint64_t(physical_address) + length, kPhysicalSize);
    std::lock_guard<std::mutex> lk(watch_mu_);
    for (uint64_t p = physical_address >> kWatchPageShift; p < (end + kWatchPage - 1) >> kWatchPageShift; ++p)
      watched_[p >> 6] |= uint64_t(1) << (p & 63);
  }
  // Invalidates (and unwatches) every watched page the guest wrote since the
  // previous kick, as recorded by include/rcomp/guest_write_tracking.h. Marks
  // of pages nothing watches are consumed too: whatever the GPU caches later
  // reads their current contents.
  void InvalidateWatchedPhysicalMemory() {
#if RCOMP_XENOS_DIAG_SKIP_CPU_INVALIDATION
    // DIAGNOSTIC UPPER BOUND ONLY (never a deliverable): measures the cost of
    // blanket per-kick invalidation. CPU writes after first use are missed.
    return;
#endif
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    std::vector<std::pair<PhysicalMemoryInvalidationCallback, void*>> cbs;
    uint64_t marked_pages = 0, invalidated_pages = 0;
    {
      std::lock_guard<std::mutex> lk(watch_mu_);
      uint8_t* marks = rcomp::g_guest_physical_written;
      uint32_t run_start = 0, run_len = 0;
      auto flush = [&] {
        if (run_len) runs.emplace_back(run_start << kWatchPageShift, run_len << kWatchPageShift);
        run_len = 0;
      };
      static_assert(rcomp::kGuestWritePages == (kPhysicalSize >> kWatchPageShift));
      static_assert(rcomp::kGuestWritePageShift == kWatchPageShift);
      for (uint32_t group = 0; group < rcomp::kGuestWritePages; group += 8) {
        uint64_t peek;
        __builtin_memcpy(&peek, marks + group, sizeof(peek));
        if (!peek) {
          flush();
          continue;
        }
        for (uint32_t page = group; page < group + 8; ++page) {
          const bool written = __atomic_load_n(&marks[page], __ATOMIC_RELAXED) &&
                               __atomic_exchange_n(&marks[page], uint8_t(0), __ATOMIC_ACQ_REL);
          uint64_t& word = watched_[page >> 6];
          const uint64_t bit = uint64_t(1) << (page & 63);
          marked_pages += written;
          if (written && (word & bit)) {
            word &= ~bit;
            ++invalidated_pages;
            if (!run_len) run_start = page;
            ++run_len;
          } else {
            flush();
          }
        }
      }
      flush();
      if (!runs.empty())
        for (auto* c : callbacks_) cbs.push_back(*c);
    }
    // How many pages the kicks mark (written since the previous kick) and invalidate (marked and cached by the GPU), cumulative, every 4096 kicks.
    kick_marked_pages_ += marked_pages;
    kick_invalidated_pages_ += invalidated_pages;
    if ((++kicks_ & 0xFFF) == 0) {
      std::fprintf(stderr, "RCOMP-INVALIDATE kicks=%llu marked_pages=%llu invalidated_pages=%llu\n", (unsigned long long)kicks_,
                   (unsigned long long)kick_marked_pages_, (unsigned long long)kick_invalidated_pages_);
    }
    for (const auto& r : runs)
      for (const auto& c : cbs) {
        // Runs contain every written watched 4 KiB page. Fault-amortization
        // widening in SharedMemory is unnecessary here; its returned range is
        // unused.
        c.first(c.second, r.first, r.second, true);
      }
  }

  // R-comp: the command processor writes guest memory too (fence and counter values, the read pointer, scratch registers, occlusion query results, memory export and resolve
  // read-backs). The GPU caches that hold such a page must see the write like the guest's own (include/rcomp/guest_write_tracking.h): without the per-frame revalidation of
  // every page (clear_memory_page_state) nothing else would tell them.
  void NoteCpWrite(uint32_t physical_address, uint32_t size) const {
    rcomp::note_guest_write(rcomp::kGuestWriteWindowBase + (physical_address & (kPhysicalSize - 1)), size);
  }

  // Guest virtual -> physical for addresses inside the physical window,
  // UINT32_MAX otherwise (the same contract as rexglue's).
  uint32_t GetPhysicalAddress(uint32_t address) const {
    return (address >= kPhysicalWindow && address - kPhysicalWindow < kPhysicalSize) ? address - kPhysicalWindow
                                                                                     : UINT32_MAX;
  }

  static constexpr uint32_t kWatchPageShift = 12;
  static constexpr uint32_t kWatchPage = 1u << kWatchPageShift;

 private:
  static constexpr uint32_t kWatchWords = (kPhysicalSize >> kWatchPageShift) / 64;
  uint8_t* base_;
  std::mutex watch_mu_;
  std::vector<std::pair<PhysicalMemoryInvalidationCallback, void*>*> callbacks_;
  uint64_t watched_[kWatchWords] = {};
  uint64_t kicks_ = 0, kick_marked_pages_ = 0, kick_invalidated_pages_ = 0;
};

}  // namespace rex::memory
