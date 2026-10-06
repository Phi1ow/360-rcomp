// Exact snapshots of the bytes submitted to shared GPU memory. The command
// processor owns this cache; GPU writes and backing-buffer resets invalidate
// snapshots. CPU invalidations keep them so the next upload can compare bytes.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace rcomp::xenos {
class PageUploadShadow {
 public:
  static constexpr size_t kMaximumDataBytes = size_t(128) << 20;
  using PageRange = std::pair<uint32_t, uint32_t>;

  // Payload bytes are bounded by both budget and address-space size. Metadata
  // is bounded by page_count (mapping) and payload capacity (intrusive LRU).
  // Allocation failure disables this optional optimization, never an upload.
  bool Configure(uint32_t page_size, uint32_t page_count, size_t budget_bytes) {
    Release();
    if (!budget_bytes) return true;
    if (page_size < 4096 || (page_size & (page_size - 1)) || !page_count ||
        uint64_t(page_size) * page_count > (uint64_t(512) << 20)) return false;
    budget_bytes = std::min(budget_bytes, kMaximumDataBytes);
    const uint32_t capacity = uint32_t(std::min<size_t>(page_count, budget_bytes / page_size));
    if (!capacity) return true;
    mapping_.reset(new (std::nothrow) uint32_t[page_count]);
    slots_.reset(new (std::nothrow) Slot[capacity]);
    if (!mapping_ || !slots_) {
      Release();
      return false;
    }
    page_size_ = page_size;
    page_count_ = page_count;
    capacity_ = capacity;
    Reset();
    return true;
  }

  bool enabled() const { return capacity_ != 0; }
  size_t data_size_bytes() const { return allocated_slots_ * size_t(page_size_); }
  size_t metadata_size_bytes() const {
    return page_count_ * sizeof(uint32_t) + capacity_ * sizeof(Slot);
  }

  bool Matches(uint32_t page, const uint8_t* bytes) {
    if (page >= page_count_) return false;
    const uint32_t slot = mapping_[page];
    if (slot == kInvalid || std::memcmp(slots_[slot].bytes.get(), bytes, page_size_)) return false;
    Touch(slot);
    return true;
  }

  // Call with staging bytes, never with a second read of guest CPU memory.
  // A missing snapshot (including allocation failure) always forces upload.
  bool Remember(uint32_t page, const uint8_t* bytes) {
    if (page >= page_count_) return false;
    uint32_t slot = mapping_[page];
    if (slot == kInvalid) {
      if (used_slots_ < capacity_) {
        slot = used_slots_;
        if (!slots_[slot].bytes) {
          slots_[slot].bytes.reset(new (std::nothrow) uint8_t[page_size_]);
          if (!slots_[slot].bytes) return false;
          ++allocated_slots_;
        }
        ++used_slots_;
      } else {
        slot = tail_;
        if (slots_[slot].page != kInvalid) mapping_[slots_[slot].page] = kInvalid;
        Unlink(slot);
      }
      slots_[slot].page = page;
      mapping_[page] = slot;
      LinkHead(slot);
    } else {
      Touch(slot);
    }
    std::memcpy(slots_[slot].bytes.get(), bytes, page_size_);
    return true;
  }

  // Invalidated slots stay on the LRU and may be reused without allocating.
  void InvalidateRange(uint32_t first_page, uint32_t page_count) {
    if (first_page >= page_count_) return;
    page_count = std::min(page_count, page_count_ - first_page);
    for (uint32_t page = first_page; page < first_page + page_count; ++page) {
      const uint32_t slot = mapping_[page];
      if (slot != kInvalid) {
        mapping_[page] = kInvalid;
        slots_[slot].page = kInvalid;
      }
    }
  }

  void Reset() {
    if (mapping_) std::fill_n(mapping_.get(), page_count_, kInvalid);
    for (uint32_t slot = 0; slot < capacity_; ++slot) {
      slots_[slot].page = slots_[slot].previous = slots_[slot].next = kInvalid;
    }
    used_slots_ = 0;
    head_ = tail_ = kInvalid;
  }

  void Release() {
    slots_.reset();
    mapping_.reset();
    page_size_ = page_count_ = capacity_ = used_slots_ = allocated_slots_ = 0;
    head_ = tail_ = kInvalid;
  }

  // The input is a single staging allocation. Only consecutive changed pages
  // are grouped. This does not alter snapshots until copy regions are queued.
  void CollectChangedRuns(uint32_t first_page, uint32_t count,
                          const uint8_t* staging, std::vector<PageRange>& runs) {
    runs.clear();
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t page = first_page + i;
      if (Matches(page, staging + size_t(i) * page_size_)) continue;
      if (!runs.empty() && runs.back().first + runs.back().second == page) ++runs.back().second;
      else runs.emplace_back(page, 1);
    }
  }

 private:
  static constexpr uint32_t kInvalid = UINT32_MAX;
  struct Slot {
    std::unique_ptr<uint8_t[]> bytes;
    uint32_t page = kInvalid;
    uint32_t previous = kInvalid;
    uint32_t next = kInvalid;
  };

  void Unlink(uint32_t slot) {
    const uint32_t previous = slots_[slot].previous, next = slots_[slot].next;
    if (previous == kInvalid) head_ = next;
    else slots_[previous].next = next;
    if (next == kInvalid) tail_ = previous;
    else slots_[next].previous = previous;
    slots_[slot].previous = slots_[slot].next = kInvalid;
  }
  void LinkHead(uint32_t slot) {
    slots_[slot].next = head_;
    if (head_ == kInvalid) tail_ = slot;
    else slots_[head_].previous = slot;
    head_ = slot;
  }
  void Touch(uint32_t slot) {
    if (slot == head_) return;
    Unlink(slot);
    LinkHead(slot);
  }

  std::unique_ptr<uint32_t[]> mapping_;
  std::unique_ptr<Slot[]> slots_;
  uint32_t page_size_ = 0, page_count_ = 0, capacity_ = 0;
  uint32_t used_slots_ = 0, allocated_slots_ = 0;
  uint32_t head_ = kInvalid, tail_ = kInvalid;
};
}  // namespace rcomp::xenos
