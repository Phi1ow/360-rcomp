#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Private CPU sampler policy. A stack word in title text is only a candidate;
// this is not an unwind and does not validate a call/return instruction.
struct RcompPcRxBounds {
    uint64_t begin = 0;
    uint64_t end = 0;

    constexpr bool valid() const noexcept {
        // Missing weak symbols are zero. Reject empty/reversed/noncanonical
        // user ranges as well, without falling back to a fixed load address.
        return begin != 0 && begin < end && end <= (uint64_t{1} << 47);
    }
    constexpr bool contains(uint64_t pc) const noexcept {
        return valid() && pc >= begin && pc < end;
    }
};

inline std::size_t rcomp_pc_stack_word_limit(uint64_t rsp) noexcept {
    if (!rsp || rsp >= (uint64_t{1} << 47) || (rsp & 7)) return 0;
    // Do not cross even a 4 KiB subpage of the interrupted stack. This is
    // conservative on the PS5's larger pages and needs no allocation, API
    // call, new TLS or access to another pthread's stack in the handler.
    const std::size_t available = (4096 - (rsp & 4095)) / sizeof(uint64_t);
    return available < 128 ? available : 128;
}

inline uint64_t rcomp_pc_stack_candidate(uint64_t raw_rip, uint64_t rsp,
                                        RcompPcRxBounds rx) noexcept {
    if (!rx.valid() || rx.contains(raw_rip)) return 0;
    const auto count = rcomp_pc_stack_word_limit(rsp);
    const auto* words = reinterpret_cast<const volatile uint64_t*>(rsp);
    for (std::size_t i = 0; i < count; ++i) {
        const uint64_t value = words[i];
        if (rx.contains(value)) return value;
    }
    return 0;
}

template <unsigned Slots = 8192, unsigned Probes = 64>
class RcompPcHistogram {
    static_assert(Slots && !(Slots & (Slots - 1)), "power-of-two PC table");
    static_assert(Probes && Probes <= Slots, "bounded PC probes");
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
public:
    struct Snapshot { uint64_t total = 0, recorded = 0, dropped = 0; };

    void record(uint64_t pc) noexcept {
        total_.fetch_add(1, std::memory_order_relaxed);
        if (pc) {
            unsigned slot = unsigned((pc >> 2) * 2654435761u) & (Slots - 1);
            for (unsigned probe = 0; probe < Probes;
                 ++probe, slot = (slot + 1) & (Slots - 1)) {
                uint64_t key = key_[slot].load(std::memory_order_relaxed);
                if (key == 0) {
                    if (!key_[slot].compare_exchange_strong(
                            key, pc, std::memory_order_relaxed) && key != pc)
                        continue;
                } else if (key != pc) continue;
                count_[slot].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    // Sampler pthread only; the visitor can format/allocate there. As with
    // the previous sampler, a dump concurrent with a signal is not an atomic
    // time-window snapshot. total, recorded and dropped may straddle a dump.
    template <class Visitor> Snapshot drain(Visitor&& visitor) {
        Snapshot result;
        result.total = total_.exchange(0, std::memory_order_relaxed);
        result.dropped = dropped_.exchange(0, std::memory_order_relaxed);
        for (unsigned i = 0; i < Slots; ++i) {
            const uint32_t n = count_[i].exchange(0, std::memory_order_relaxed);
            const uint64_t pc = key_[i].load(std::memory_order_relaxed);
            if (n && pc) { result.recorded += n; visitor(pc, n); }
        }
        return result;
    }

private:
    std::atomic<uint64_t> key_[Slots]{};
    std::atomic<uint32_t> count_[Slots]{};
    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> dropped_{0};
};

template <unsigned Slots = 8192, unsigned Probes = 64>
struct RcompPcSamples {
    RcompPcHistogram<Slots, Probes> raw;
    RcompPcHistogram<Slots, Probes> candidates;

    void record(uint64_t raw_rip, uint64_t rsp, RcompPcRxBounds rx) noexcept {
        // Exactly one raw sample per interrupted thread. Candidate counts
        // form a separate heuristic channel and must never be added to raw.
        raw.record(raw_rip);
        const uint64_t candidate = rcomp_pc_stack_candidate(raw_rip, rsp, rx);
        if (candidate) candidates.record(candidate);
    }
};
