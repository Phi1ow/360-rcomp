#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_heap.h"
#include "test_util.h"

using namespace rcomp;
using namespace rcomp::rt;

int main() {
    GuestMemory mem;
    if (mem.reserve() != MemStatus::Ok) {
        fprintf(stderr, "reserve failed\n");
        return 2;
    }

    // --- runtime range exclusion + alignment ---------------------------------
    const uint32_t lo = 0x40000000, hi = 0x40400000;  // 4 MiB region
    const uint32_t rr = 0x40100000;                   // runtime-owned 128 KiB inside it
    CHECK(mem.reserve_runtime_range(rr, 0x20000, "test-table") == MemStatus::Ok);
    GuestHeap heap;
    CHECK_ST(heap.init(&mem, lo, hi), Status::Ok);
    CHECK_EQ(heap.stats().excluded_pages, 2u);

    uint32_t a = 0;
    const uint32_t aligns[] = {0, 16, 32, 256, 4096, 0x10000, 0x100000};
    for (uint32_t al : aligns) {
        CHECK_ST(heap.alloc(100, al, true, &a), Status::Ok);
        uint32_t eff = al ? al : 16;
        CHECK_EQ(a % eff, 0u);
        CHECK(a >= lo && a + 100 <= hi);
        CHECK(!mem.overlaps_runtime_range(a, 100));
        CHECK(mem.is_committed(a, 100));
        uint8_t* p = mem.translate(a, 100);
        CHECK(p != nullptr);
        bool zero = true;
        for (int i = 0; i < 100; ++i) zero &= p[i] == 0;
        CHECK(zero);
        memset(p, 0xAB, 100);  // writable
    }
    CHECK_ST(heap.alloc(16, 3, false, &a), Status::InvalidArgument);  // not a power of two
    CHECK_ST(heap.alloc(0, 16, false, &a), Status::InvalidArgument);

    // A big allocation must never span the runtime range.
    uint32_t big = 0;
    while (heap.alloc(0x10000, 0x10000, false, &big) == Status::Ok)
        CHECK(!mem.overlaps_runtime_range(big, 0x10000));

    // --- exhaustion -----------------------------------------------------------
    GuestHeap small;
    CHECK_ST(small.init(&mem, 0x50000000, 0x50020000), Status::Ok);  // 128 KiB
    uint32_t x = 0, y = 0, z = 0xDEADBEEF;
    CHECK_ST(small.alloc(0x10000, 16, false, &x), Status::Ok);
    CHECK_ST(small.alloc(0x10000, 16, false, &y), Status::Ok);
    CHECK_ST(small.alloc(16, 16, false, &z), Status::OutOfMemory);
    CHECK_EQ(z, 0xDEADBEEFu);  // untouched on failure
    CHECK_EQ(small.stats().free_bytes, 0ull);

    // --- free / double free / coalescing --------------------------------------
    CHECK_ST(small.free(x), Status::Ok);
    CHECK_ST(small.free(x), Status::DoubleFree);
    CHECK_ST(small.free(x + 16), Status::NotAllocated);
    CHECK_ST(small.free(0x12345670), Status::NotAllocated);
    CHECK_ST(small.free(y), Status::Ok);
    CHECK_ST(small.free(y), Status::DoubleFree);
    CHECK_EQ(small.stats().free_bytes, 0x20000ull);
    CHECK_EQ(small.stats().live_allocations, 0u);
    // Coalesced: the whole region is available again as one block.
    CHECK_ST(small.alloc(0x20000, 16, false, &z), Status::Ok);
    CHECK_EQ(z, 0x50000000u);
    uint32_t sz = 0;
    CHECK_ST(small.allocation_size(z, &sz), Status::Ok);
    CHECK_EQ(sz, 0x20000u);
    // Re-allocated address is live again, no longer a double free.
    CHECK_ST(small.free(z), Status::Ok);

    // Sizes are rounded to 16 bytes and blocks do not overlap.
    uint32_t p1 = 0, p2 = 0;
    CHECK_ST(small.alloc(1, 16, false, &p1), Status::Ok);
    CHECK_ST(small.alloc(1, 16, false, &p2), Status::Ok);
    CHECK_EQ(p2 - p1, 16u);

    // --- commit_guest_range: runs of uncommitted pages around committed ones -------------
    // (the heap commits a run of pages at once; committed pages survive, new ones are zero)
    {
        constexpr uint32_t kPage = 0x10000;
        GuestHeap vm;
        CHECK_ST(vm.init(&mem, 0x60000000, 0x60800000), Status::Ok);
        uint32_t base = 0;
        CHECK_ST(vm.reserve_in(7 * kPage, kPage, 0x60000000, 0x60800000, false, &base), Status::Ok);
        CHECK_ST(vm.set_guest_protection(base, 0x04, kPage), Status::Ok);
        CHECK_ST(vm.commit_guest_range(base + 1 * kPage, kPage, 0x04, Protect::ReadWrite, true), Status::Ok);
        CHECK_ST(vm.commit_guest_range(base + 4 * kPage, kPage, 0x04, Protect::ReadWrite, true), Status::Ok);
        memset(mem.translate(base + 1 * kPage, kPage), 0xA5, kPage);
        memset(mem.translate(base + 4 * kPage, kPage), 0x5A, kPage);
        CHECK(!mem.is_committed(base, kPage));
        // Pages 0, 2-3 and 5-6 are new: three runs, two of them with committed neighbours.
        CHECK_ST(vm.commit_guest_range(base, 7 * kPage, 0x04, Protect::ReadWrite, true), Status::Ok);
        CHECK(mem.is_committed(base, 7 * kPage));
        for (uint32_t page = 0; page < 7; ++page) {
            const uint8_t* p = mem.translate(base + page * kPage, kPage);
            CHECK(p != nullptr);
            const uint8_t want = page == 1 ? 0xA5 : page == 4 ? 0x5A : 0x00;
            bool same = true;
            for (uint32_t i = 0; i < kPage; ++i) same &= p[i] == want;
            CHECK(same);
        }
        // Commits are whole 64 KiB pages.
        CHECK_ST(vm.commit_guest_range(base, kPage + 0x1000, 0x04, Protect::ReadWrite, true), Status::InvalidArgument);
    }

    // Uninitialised heap.
    GuestHeap none;
    CHECK_ST(none.alloc(16, 16, false, &a), Status::NotInitialized);
    CHECK_ST(none.init(&mem, 0x40000001, 0x40010000), Status::InvalidArgument);

    return test_result("rt_test_heap");
}
