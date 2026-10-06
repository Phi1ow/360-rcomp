// How the title paces its swaps against the vertical blank (internal to the video HLE).
//
// The title's frame rate is not one number: seconds at exactly 30 fps (every swap two vblanks after
// the previous one) alternate with seconds near 36 fps, and the mix decides the average of a run. The
// histogram of "vblanks since the previous VdSwap" says which regime a second was in, without a trace;
// the frame-rate counter prints its per-second difference (app/src/title.cpp).
#pragma once

#include <stdint.h>

#include <atomic>

namespace rcomp::rt {

class SwapPacing {
public:
    static constexpr unsigned kBuckets = 8;  // 0..6 vblanks between two swaps, 7 = seven or more

    // A graphics interrupt with source 0 (vblank) reached the title.
    void vblank() { vblanks_.fetch_add(1, std::memory_order_relaxed); }

    // VdSwap: counts the vblanks since the previous swap (the first swap has no predecessor).
    void swap() {
        const uint64_t now = vblanks_.load(std::memory_order_relaxed);
        const uint64_t before = last_.exchange(now, std::memory_order_relaxed);
        if (before == kNoSwapYet) return;
        const uint64_t delta = now - before;
        hist_[delta < kBuckets ? delta : kBuckets - 1].fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t vblanks() const { return vblanks_.load(std::memory_order_relaxed); }
    void histogram(uint64_t out[kBuckets]) const {
        for (unsigned i = 0; i < kBuckets; ++i) out[i] = hist_[i].load(std::memory_order_relaxed);
    }
    void reset() {
        vblanks_.store(0, std::memory_order_relaxed);
        last_.store(kNoSwapYet, std::memory_order_relaxed);
        for (auto& bucket : hist_) bucket.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr uint64_t kNoSwapYet = ~uint64_t(0);
    std::atomic<uint64_t> vblanks_{0};
    std::atomic<uint64_t> last_{kNoSwapYet};
    std::atomic<uint64_t> hist_[kBuckets] = {};
};

}  // namespace rcomp::rt
