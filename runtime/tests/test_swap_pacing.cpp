// SwapPacing (runtime/src/swap_pacing.h): the histogram of vblanks between consecutive swaps.
#include <atomic>
#include <thread>
#include <vector>

#include "swap_pacing.h"
#include "test_util.h"

using namespace rcomp::rt;

namespace {
void expect(const SwapPacing& pacing, std::vector<uint64_t> want) {
    uint64_t got[SwapPacing::kBuckets];
    pacing.histogram(got);
    want.resize(SwapPacing::kBuckets, 0);
    for (unsigned k = 0; k < SwapPacing::kBuckets; ++k) {
        if (got[k] != want[k]) fprintf(stderr, "bucket %u: got %llu, want %llu\n", k, (unsigned long long)got[k], (unsigned long long)want[k]);
        CHECK_EQ(got[k], want[k]);
    }
}
}  // namespace

int main() {
    SwapPacing pacing;
    expect(pacing, {});
    pacing.swap();  // the first swap has no predecessor
    expect(pacing, {});

    // Locked at 30 fps: two vblanks per swap.
    for (int i = 0; i < 10; ++i) {
        pacing.vblank();
        pacing.vblank();
        pacing.swap();
    }
    expect(pacing, {0, 0, 10});

    // The ~36 fps regime: one-vblank and two-vblank swaps (1, 2, 2, ...).
    for (int i = 0; i < 4; ++i) {
        pacing.vblank();
        pacing.swap();
        for (int j = 0; j < 2; ++j) {
            pacing.vblank();
            pacing.vblank();
            pacing.swap();
        }
    }
    expect(pacing, {0, 4, 18});

    // Two swaps inside one vblank, then a long gap: zero and the saturating bucket.
    pacing.swap();
    pacing.swap();
    for (int i = 0; i < 9; ++i) pacing.vblank();
    pacing.swap();
    expect(pacing, {2, 4, 18, 0, 0, 0, 0, 1});
    CHECK_EQ(pacing.vblanks(), 49u);  // 10 * 2 + 4 * 5 + 9

    // A reset starts a new generation: nothing carried over, the next swap has no predecessor.
    pacing.reset();
    expect(pacing, {});
    CHECK_EQ(pacing.vblanks(), 0u);
    pacing.vblank();
    pacing.swap();
    expect(pacing, {});
    pacing.vblank();
    pacing.swap();
    expect(pacing, {0, 1});

    // Concurrent producers (the bridge thread raises vblanks, the main thread swaps): no swap is lost.
    SwapPacing shared;
    std::atomic<bool> stop{false};
    std::thread bridge([&] {
        while (!stop.load()) shared.vblank();
    });
    const int kSwaps = 200000;
    for (int i = 0; i < kSwaps; ++i) shared.swap();
    stop = true;
    bridge.join();
    uint64_t total[SwapPacing::kBuckets];
    shared.histogram(total);
    uint64_t sum = 0;
    for (uint64_t count : total) sum += count;
    CHECK_EQ(sum, uint64_t(kSwaps - 1));
    return test_result("rt_test_swap_pacing");
}
