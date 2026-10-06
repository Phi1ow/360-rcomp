#include "rcomp/guest_write_tracking.h"
#include "rcomp/runtime/guest_heap.h"

#include <algorithm>
#include <iterator>
#include <string.h>
#include <new>

#include "rcomp/guest_memory.h"

namespace rcomp::rt {

namespace {
constexpr uint64_t kPage = kGuestPageSize;

Status from_mem(MemStatus s) {
    switch (s) {
    case MemStatus::Ok: return Status::Ok;
    case MemStatus::InvalidArgument: return Status::InvalidArgument;
    case MemStatus::Conflict: return Status::Conflict;
    case MemStatus::NotReserved: return Status::NotInitialized;
    case MemStatus::OutOfMemory:
    case MemStatus::PlatformError: return Status::OutOfMemory;
    }
    return Status::OutOfMemory;
}

bool is_pow2(uint32_t v) { return v && !(v & (v - 1)); }
}  // namespace

Status GuestHeap::init(GuestMemory* mem, uint32_t lo, uint32_t hi) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!mem || !mem->reserved()) return Status::NotInitialized;
    if (lo >= hi || lo % kPage || hi % kPage || lo == 0) return Status::InvalidArgument;
    mem_ = mem;
    lo_ = lo;
    hi_ = hi;
    alloc_lo_ = (lo < kLargePageLo && hi > kLargePageLo) ? kLargePageLo : lo;
    free_.clear();
    used_.clear();
    guest_allocation_protect_.clear();
    guest_page_size_.clear();
    guest_protection_.clear();
    guards_.clear();
    freed_starts_.clear();
    vm_reservations_.clear();
    allocated_bytes_ = 0;
    excluded_pages_ = 0;
    // Build free blocks from runs of pages that do not overlap runtime ranges.
    uint64_t run_start = lo;
    for (uint64_t p = lo; p < hi; p += kPage) {
        if (mem->overlaps_runtime_range(p, kPage)) {
            if (p > run_start) free_[(uint32_t)run_start] = (uint32_t)(p - run_start);
            run_start = p + kPage;
            ++excluded_pages_;
        }
    }
    if (hi > run_start) free_[(uint32_t)run_start] = (uint32_t)(hi - run_start);
    return Status::Ok;
}

void GuestHeap::reset() {
    std::lock_guard<std::mutex> lock(mu_);
    mem_ = nullptr;
    free_.clear();
    used_.clear();
    guest_allocation_protect_.clear();
    guest_page_size_.clear();
    guest_protection_.clear();
    guards_.clear();
    freed_starts_.clear();
    vm_reservations_.clear();
    allocated_bytes_ = 0;
}

Status GuestHeap::commit_range(uint64_t addr, uint64_t size) {
    uint64_t first = addr & ~(kPage - 1);
    uint64_t last = (addr + size + kPage - 1) & ~(kPage - 1);
    // A committed page may have become a stack guard or have been protected
    // by its owner. Do not zero or hand out an unwritable block, and do not
    // silently change protection on a page shared with a live allocation.
    for (uint64_t p = first; p < last; p += kPage) {
        if (mem_->is_committed(p, kPage) && !mem_->is_accessible(p, kPage, Protect::ReadWrite))
            return Status::Conflict;
    }
    uint64_t run = 0, run_len = 0;
    for (uint64_t p = first; p <= last; p += kPage) {
        bool need = p < last && !mem_->is_committed(p, kPage);
        if (need) {
            if (!run_len) run = p;
            run_len += kPage;
            continue;
        }
        if (run_len) {
            Status s = from_mem(mem_->commit(run, run_len, Protect::ReadWrite));
            if (s != Status::Ok) return s;
            run_len = 0;
        }
    }
    return Status::Ok;
}

Status GuestHeap::alloc(uint32_t size, uint32_t align, bool zero, uint32_t* out) {
    return alloc_in(size, align, alloc_lo_, 0x100000000ull, false, zero, out);
}

Status GuestHeap::alloc_in(uint32_t size, uint32_t align, uint64_t lo, uint64_t hi, bool top_down, bool zero,
                           uint32_t* out) {
    return allocate_range(size, align, lo, hi, top_down, zero, true, out);
}

Status GuestHeap::reserve_in(uint32_t size, uint32_t align, uint64_t lo, uint64_t hi,
                            bool top_down, uint32_t* out) {
    if (!size || size % kPage || align < kPage || lo % kPage || hi % kPage)
        return Status::InvalidArgument;
    return allocate_range(size, align, lo, hi, top_down, false, false, out);
}

Status GuestHeap::allocate_range(uint32_t size, uint32_t align, uint64_t lo, uint64_t hi,
                                bool top_down, bool zero, bool commit, uint32_t* out) {
    if (!out || size == 0) return Status::InvalidArgument;
    if (align == 0) align = kMinAlign;
    if (!is_pow2(align)) return Status::InvalidArgument;
    if (align < kMinAlign) align = kMinAlign;
    uint64_t size_r = ((uint64_t)size + kMinAlign - 1) & ~(uint64_t)(kMinAlign - 1);
    std::lock_guard<std::mutex> lock(mu_);
    if (!mem_) return Status::NotInitialized;
    lo = lo < lo_ ? lo_ : lo;
    hi = hi > hi_ ? hi_ : hi;
    if (lo >= hi) return Status::OutOfMemory;
    // Candidate address inside one free block, or UINT64_MAX.
    auto fit = [&](uint64_t start, uint64_t end) -> uint64_t {
        start = start < lo ? lo : start;
        end = end > hi ? hi : end;
        if (start >= end || end - start < size_r) return UINT64_MAX;
        uint64_t a = top_down ? ((end - size_r) & ~(uint64_t)(align - 1))
                              : ((start + align - 1) & ~(uint64_t)(align - 1));
        return (a >= start && a + size_r <= end) ? a : UINT64_MAX;
    };
    auto take = [&](std::map<uint32_t, uint32_t>::iterator it, uint64_t a) -> Status {
        uint64_t start = it->first, end = start + it->second;
        // Allocate all bookkeeping nodes before changing the free list or
        // touching guest pages. In exception-disabled PS5 builds host OOM is
        // terminal; guest exhaustion/commit failure still returns a status.
        std::map<uint32_t, uint32_t> pending_free, pending_used;
        std::set<uint32_t> pending_reservation;
#if defined(__cpp_exceptions)
        try {
#endif
            if (a > start) pending_free.emplace((uint32_t)start, (uint32_t)(a - start));
            if (a + size_r < end) pending_free.emplace((uint32_t)(a + size_r), (uint32_t)(end - a - size_r));
            pending_used.emplace((uint32_t)a, (uint32_t)size_r);
            if (!commit) pending_reservation.insert((uint32_t)a);
#if defined(__cpp_exceptions)
        } catch (const std::bad_alloc&) { return Status::OutOfMemory; }
#endif
        if (mem_->overlaps_runtime_range(a, size_r)) return Status::Conflict;
        Status s = commit ? commit_range(a, size_r)
                          : from_mem(mem_->decommit(a, size_r));
        if (s != Status::Ok) return s;
        free_.erase(it);
        free_.merge(pending_free);
        used_.merge(pending_used);
        vm_reservations_.merge(pending_reservation);
        freed_starts_.erase((uint32_t)a);
        allocated_bytes_ += size_r;
        if (zero) {
            memset(mem_->host(a), 0, size_r);
            note_guest_write_range(a, size_r);
        }
        *out = (uint32_t)a;
        return Status::Ok;
    };
    if (top_down) {
        for (auto it = free_.end(); it != free_.begin();) {
            --it;
            uint64_t a = fit(it->first, (uint64_t)it->first + it->second);
            if (a != UINT64_MAX) return take(it, a);
        }
    } else {
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            uint64_t a = fit(it->first, (uint64_t)it->first + it->second);
            if (a != UINT64_MAX) return take(it, a);
        }
    }
    return Status::OutOfMemory;
}

Status GuestHeap::free(uint32_t addr) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!mem_) return Status::NotInitialized;
    auto it = used_.find(addr);
    if (it == used_.end()) {
        return freed_starts_.count(addr) ? Status::DoubleFree : Status::NotAllocated;
    }
    uint32_t size = it->second;
    uint64_t start = addr, end = (uint64_t)addr + size;
    auto next = free_.lower_bound(addr), prev = free_.end();
    if (next != free_.begin()) {
        auto p = std::prev(next);
        if ((uint64_t)p->first + p->second == start) { prev = p; start = p->first; }
    }
    if (next != free_.end() && next->first == end) end += next->second;
    else next = free_.end();
    std::map<uint32_t, uint32_t> pending_free;
    std::set<uint32_t> pending_freed;
#if defined(__cpp_exceptions)
    try {
#endif
        pending_free.emplace((uint32_t)start, (uint32_t)(end - start));
        pending_freed.insert(addr);
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) { return Status::OutOfMemory; }
#endif
    // A real VM reservation relinquishes its committed backing when released.
    // All allocation nodes are staged before this mapping mutation.
    if (vm_reservations_.count(addr)) {
        const Status status = from_mem(mem_->decommit(addr, size));
        if (status != Status::Ok) return status;
    }
    // Restore guard pages inside the block before recycling it.
    auto guard_begin = guards_.lower_bound(addr), guard_end = guard_begin;
    for (; guard_end != guards_.end() && guard_end->first < addr + (uint64_t)size; ++guard_end) {
        Status s = from_mem(mem_->protect(guard_end->first, guard_end->second, Protect::ReadWrite));
        if (s != Status::Ok) return s;  // block and guard ownership remain live
    }
    // Guest protection changes must not leak into the next allocation reusing
    // this host-backed page. Modifier-only spans are already RW; RO/NOACCESS
    // spans can only have been created through page-aligned protect_guest_range.
    auto protection_begin = guest_protection_.lower_bound(addr), protection_end = protection_begin;
    for (; protection_end != guest_protection_.end() &&
           protection_end->first < addr + (uint64_t)size; ++protection_end) {
        const uint32_t p = protection_end->first;
        const uint32_t n = protection_end->second.size;
        if (mem_->is_committed(p, n) && !mem_->is_accessible(p, n, Protect::ReadWrite)) {
            if ((p % kPage) || (n % kPage)) return Status::Conflict;
            Status s = from_mem(mem_->protect(p, n, Protect::ReadWrite));
            if (s != Status::Ok) return s;  // ownership/provenance stay live
        }
    }
    guards_.erase(guard_begin, guard_end);
    guest_allocation_protect_.erase(addr);
    guest_page_size_.erase(addr);
    guest_protection_.erase(protection_begin, protection_end);
    if (prev != free_.end()) free_.erase(prev);
    if (next != free_.end()) free_.erase(next);
    used_.erase(it);
    vm_reservations_.erase(addr);
    allocated_bytes_ -= size;
    freed_starts_.merge(pending_freed);
    free_.merge(pending_free);
    return Status::Ok;
}

Status GuestHeap::allocation_size(uint32_t addr, uint32_t* out) const {
    if (!out) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = used_.find(addr);
    if (it == used_.end()) return freed_starts_.count(addr) ? Status::DoubleFree : Status::NotAllocated;
    *out = it->second;
    return Status::Ok;
}

Status GuestHeap::allocation_containing(uint32_t addr, uint32_t* base, uint32_t* size) const {
    if (!base || !size) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    if (!mem_) return Status::NotInitialized;
    auto it = used_.upper_bound(addr);
    if (it == used_.begin()) return Status::NotAllocated;
    --it;
    if (uint64_t(addr) >= uint64_t(it->first) + it->second) return Status::NotAllocated;
    *base = it->first;
    *size = it->second;
    return Status::Ok;
}

Status GuestHeap::region_containing(uint32_t addr, GuestHeapRegionInfo* out) const {
    if (!out) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    if (!mem_) return Status::NotInitialized;
    if (addr < lo_ || addr >= hi_) return Status::InvalidArgument;

    auto used = used_.upper_bound(addr);
    if (used != used_.begin()) {
        auto candidate = std::prev(used);
        if (uint64_t(addr) < uint64_t(candidate->first) + candidate->second) {
            *out = {true, candidate->first, candidate->second};
            return Status::Ok;
        }
    }
    auto free = free_.upper_bound(addr);
    if (free != free_.begin()) {
        auto candidate = std::prev(free);
        if (uint64_t(addr) < uint64_t(candidate->first) + candidate->second) {
            *out = {false, candidate->first, candidate->second};
            return Status::Ok;
        }
    }
    // Heap holes are runtime-reserved/excluded, not guest-free pages.
    return Status::Conflict;
}

Status GuestHeap::set_guest_protection(uint32_t allocation_base, uint32_t protect,
                                       uint32_t guest_page_size) {
    if (!guest_page_size || !is_pow2(guest_page_size) || guest_page_size < 0x1000)
        return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    if (!mem_) return Status::NotInitialized;
    auto used = used_.find(allocation_base);
    if (used == used_.end()) return Status::NotAllocated;
    if (guest_allocation_protect_.count(allocation_base)) return Status::AlreadyExists;
    if ((allocation_base % guest_page_size) || (used->second % guest_page_size))
        return Status::InvalidArgument;

    std::map<uint32_t, uint32_t> pending_allocation;
    std::map<uint32_t, uint32_t> pending_page_size;
    std::map<uint32_t, GuestProtectionSpan> pending_protection;
#if defined(__cpp_exceptions)
    try {
#endif
        pending_allocation.emplace(allocation_base, protect);
        pending_page_size.emplace(allocation_base, guest_page_size);
        pending_protection.emplace(allocation_base, GuestProtectionSpan{used->second, protect});
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return Status::OutOfMemory;
    }
#endif
    guest_allocation_protect_.merge(pending_allocation);
    guest_page_size_.merge(pending_page_size);
    guest_protection_.merge(pending_protection);
    if (!pending_allocation.empty() || !pending_page_size.empty() || !pending_protection.empty()) {
        guest_allocation_protect_.erase(allocation_base);
        guest_page_size_.erase(allocation_base);
        guest_protection_.erase(allocation_base);
        return Status::Conflict;
    }
    return Status::Ok;
}

Status GuestHeap::protect_guest_range(uint32_t addr, uint32_t size, uint32_t protect,
                                      Protect host_protect, uint32_t* old_protect) {
    return change_guest_range(addr, size, protect, host_protect, old_protect, false, false);
}

Status GuestHeap::commit_guest_range(uint32_t addr, uint32_t size, uint32_t protect,
                                    Protect host_protect, bool zero) {
    return change_guest_range(addr, size, protect, host_protect, nullptr, true, zero);
}

Status GuestHeap::decommit_guest_range(uint32_t addr, uint32_t size) {
    if (!size || addr % kPage || size % kPage) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    if (!mem_) return Status::NotInitialized;
    auto it = used_.upper_bound(addr);
    if (it == used_.begin()) return Status::NotAllocated;
    --it;
    if (!guest_allocation_protect_.count(it->first) ||
        uint64_t(addr) + size > uint64_t(it->first) + it->second)
        return Status::NotAllocated;
    return from_mem(mem_->decommit(addr, size));
}

Status GuestHeap::change_guest_range(uint32_t addr, uint32_t size, uint32_t protect,
                                    Protect host_protect, uint32_t* old_protect,
                                    bool commit, bool zero) {
    if (!size || uint64_t(addr) + size > 0x100000000ull)
        return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    if (!mem_) return Status::NotInitialized;

    auto used = used_.upper_bound(addr);
    if (used == used_.begin()) return Status::NotAllocated;
    --used;
    const uint32_t allocation_base = used->first;
    const uint64_t allocation_end = uint64_t(allocation_base) + used->second;
    const uint64_t end = uint64_t(addr) + size;
    auto page_size_it = guest_page_size_.find(allocation_base);
    if (end > allocation_end || !guest_allocation_protect_.count(allocation_base) ||
        page_size_it == guest_page_size_.end())
        return Status::NotAllocated;
    const uint32_t guest_page_size = page_size_it->second;
    if ((addr % guest_page_size) || (size % guest_page_size)) return Status::InvalidArgument;
    if (commit && ((addr % kPage) || (size % kPage))) return Status::InvalidArgument;
    if (!commit && !mem_->is_committed(addr, size)) return Status::NotAllocated;

    auto old = guest_protection_.upper_bound(addr);
    if (old == guest_protection_.begin()) return Status::Conflict;
    --old;
    if (uint64_t(addr) >= uint64_t(old->first) + old->second.size) return Status::Conflict;
    const uint32_t previous_protect = old->second.protect;

    std::map<uint32_t, GuestProtectionSpan> staged;
#if defined(__cpp_exceptions)
    try {
#endif
        staged = guest_protection_;
        auto it = staged.upper_bound(addr);
        if (it != staged.begin()) --it;
        uint64_t covered = addr;
        while (it != staged.end() && it->first < end) {
            const uint64_t span_begin = it->first;
            const uint64_t span_end = span_begin + it->second.size;
            if (span_end <= addr) { ++it; continue; }
            if (span_begin > covered) return Status::Conflict;
            const GuestProtectionSpan span = it->second;
            auto erase = it++;
            staged.erase(erase);
            if (span_begin < addr)
                staged.emplace((uint32_t)span_begin,
                               GuestProtectionSpan{(uint32_t)(addr - span_begin), span.protect});
            if (span_end > end)
                staged.emplace((uint32_t)end,
                               GuestProtectionSpan{(uint32_t)(span_end - end), span.protect});
            covered = std::min<uint64_t>(span_end, end);
        }
        if (covered != end) return Status::Conflict;
        staged.emplace(addr, GuestProtectionSpan{size, protect});

        // Coalesce only inside this allocation; adjacent allocations must keep
        // distinct protection-region boundaries for MEMORY_BASIC_INFORMATION.
        auto current = staged.find(addr);
        if (current != staged.begin()) {
            auto previous = std::prev(current);
            if (previous->first >= allocation_base &&
                uint64_t(previous->first) + previous->second.size == current->first &&
                previous->second.protect == current->second.protect) {
                const uint32_t new_base = previous->first;
                const uint32_t new_size = previous->second.size + current->second.size;
                staged.erase(current);
                staged.erase(previous);
                current = staged.emplace(new_base, GuestProtectionSpan{new_size, protect}).first;
            }
        }
        auto next = std::next(current);
        if (next != staged.end() && next->first < allocation_end &&
            uint64_t(current->first) + current->second.size == next->first &&
            next->second.protect == current->second.protect) {
            current->second.size += next->second.size;
            staged.erase(next);
        }
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return Status::OutOfMemory;
    }
#endif

    if (commit) {
        for (uint64_t page = addr; page < end; page += kPage) {
            if (mem_->is_committed(page, kPage)) continue;
            const Status status = from_mem(mem_->commit(page, kPage, Protect::ReadWrite));
            if (status != Status::Ok) return status;
            if (zero) {
                memset(mem_->host(page), 0, kPage);
                note_guest_write_range(page, kPage);
            }
        }
    }
    auto current_host = [&](uint64_t page) -> Protect {
        if (!mem_->is_committed(page, 1)) return Protect::None;
        if (mem_->is_accessible(page, 1, Protect::ReadWrite)) return Protect::ReadWrite;
        if (mem_->is_accessible(page, 1, Protect::Read)) return Protect::Read;
        return Protect::None;
    };
    bool host_change = false;
    for (uint64_t p = addr & ~(kPage - 1); p < end; p += kPage) {
        if (current_host(p) != host_protect) { host_change = true; break; }
    }
    if (host_change) {
        if ((addr % kPage) || (size % kPage)) return Status::InvalidArgument;
        Status status = from_mem(mem_->protect(addr, size, host_protect));
        if (status != Status::Ok) return status;
    }
    guest_protection_.swap(staged);
    if (old_protect) *old_protect = previous_protect;
    return Status::Ok;
}

Status GuestHeap::query_guest_protection(uint32_t addr, GuestHeapProtectionInfo* out) const {
    if (!out) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    if (!mem_) return Status::NotInitialized;
    auto used = used_.upper_bound(addr);
    if (used == used_.begin()) return Status::NotAllocated;
    --used;
    if (uint64_t(addr) >= uint64_t(used->first) + used->second) return Status::NotAllocated;
    auto allocation_protect = guest_allocation_protect_.find(used->first);
    auto page_size = guest_page_size_.find(used->first);
    if (allocation_protect == guest_allocation_protect_.end() || page_size == guest_page_size_.end())
        return Status::NotFound;
    auto span = guest_protection_.upper_bound(addr);
    if (span == guest_protection_.begin()) return Status::Conflict;
    --span;
    if (uint64_t(addr) >= uint64_t(span->first) + span->second.size ||
        span->first < used->first || uint64_t(span->first) + span->second.size > uint64_t(used->first) + used->second)
        return Status::Conflict;
    *out = {used->first, used->second, page_size->second, allocation_protect->second,
            span->first, span->second.size, span->second.protect};
    return Status::Ok;
}

Status GuestHeap::set_guard(uint32_t addr, uint32_t size, bool guard) {
    if (size == 0 || addr % kPage || size % kPage) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    if (!mem_) return Status::NotInitialized;
    // Must lie inside one live allocation.
    auto it = used_.upper_bound(addr);
    if (it == used_.begin()) return Status::NotAllocated;
    --it;
    if ((uint64_t)addr + size > (uint64_t)it->first + it->second) return Status::NotAllocated;
    auto existing = guards_.lower_bound(addr);
    const bool exact = existing != guards_.end() && existing->first == addr && existing->second == size;
    if (!exact) {
        if (existing != guards_.end() && uint64_t(existing->first) < uint64_t(addr)+size) return Status::Conflict;
        if (existing != guards_.begin()) {
            const auto previous=std::prev(existing);
            if (uint64_t(previous->first)+previous->second>addr) return Status::Conflict;
        }
    }
    std::map<uint32_t,uint32_t> pending;
    if (guard && !exact) {
#if defined(__cpp_exceptions)
        try { pending.emplace(addr,size); }
        catch (const std::bad_alloc&) { return Status::OutOfMemory; }
#else
        pending.emplace(addr,size);
#endif
    }
    // Keep tracking the range even if the platform protection call partially
    // fails: free() must still restore every possibly affected page.
    guards_.merge(pending);
    Status s=from_mem(mem_->protect(addr,size,guard ? Protect::None : Protect::ReadWrite));
    if (s==Status::Ok && !guard && exact) guards_.erase(existing);
    return s;
}

GuestHeapStats GuestHeap::stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    GuestHeapStats st{};
    st.region_bytes = (uint64_t)hi_ - lo_;
    st.allocated_bytes = allocated_bytes_;
    for (auto& f : free_) st.free_bytes += f.second;
    st.live_allocations = (uint32_t)used_.size();
    st.excluded_pages = excluded_pages_;
    for (uint32_t base : vm_reservations_) {
        const auto it = used_.find(base);
        if (it == used_.end()) continue;
        const uint64_t end = uint64_t(base) + it->second;
        for (uint64_t page = base; page < end; page += kPage) {
            const uint64_t bytes = end - page < kPage ? end - page : kPage;
            if (!mem_ || !mem_->is_committed(page, bytes)) st.reserved_unbacked_bytes += bytes;
        }
    }
    return st;
}

}  // namespace rcomp::rt
