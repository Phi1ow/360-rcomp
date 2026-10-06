// MmQueryStatistics over the runtime's real guest heaps, and
// FscSetCacheElementCount. Plain production thunks, no test doubles.
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/xenos_gpu.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__MmQueryStatistics);
PPC_EXTERN_FUNC(__imp__MmAllocatePhysicalMemoryEx);
PPC_EXTERN_FUNC(__imp__MmFreePhysicalMemory);
PPC_EXTERN_FUNC(__imp__MmGetPhysicalAddress);
PPC_EXTERN_FUNC(__imp__NtAllocateVirtualMemory);
PPC_EXTERN_FUNC(__imp__NtFreeVirtualMemory);
PPC_EXTERN_FUNC(__imp__FscSetCacheElementCount);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory mem;
uint32_t scratch;
constexpr uint32_t kStats = 0x000, kBase = 0x200, kSize = 0x204;
constexpr uint32_t kPage64K = 0x10000, kMiB = 0x100000;

uint32_t read(uint32_t address) { uint32_t value = 0; CHECK(guest_read_be32(address, &value)); return value; }
void write(uint32_t address, uint32_t value) { CHECK(guest_write_be32(address, value)); }
uint32_t call(PPCFunc* fn, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0, uint32_t e = 0,
              uint32_t f = 0) {
    alignas(64) PPCContext ctx{};
    ctx.r3.u64 = a; ctx.r4.u64 = b; ctx.r5.u64 = c; ctx.r6.u64 = d; ctx.r7.u64 = e; ctx.r8.u64 = f;
    fn(ctx, mem.base());
    return ctx.r3.u32;
}
struct Stats { uint32_t w[26]; };
Stats query() {
    write(scratch + kStats, 104);
    CHECK_EQ(call(__imp__MmQueryStatistics, scratch + kStats), 0u);
    Stats s{};
    for (uint32_t i = 0; i < 26; ++i) s.w[i] = read(scratch + kStats + 4 * i);
    return s;
}
uint64_t pages(uint64_t bytes) { return (bytes + 4095) / 4096; }
// What the title has consumed according to the heaps themselves.
uint64_t expected_used() {
    const auto heap = runtime()->heap.stats(), physical = runtime()->physical.stats();
    return pages(heap.allocated_bytes - heap.reserved_unbacked_bytes) + pages(physical.allocated_bytes);
}

void statistics() {
    constexpr uint32_t total = xenos::kXenosPhysicalSize / 4096;
    Stats s = query();
    CHECK_EQ(s.w[0], 104u);
    CHECK_EQ(s.w[1], total);                                  // 512 MiB
    CHECK_EQ(s.w[2], 0u);                                     // no guest pages for the kernel
    CHECK_EQ(s.w[3], uint32_t(total - expected_used()));      // title.AvailablePages
    const auto heap = runtime()->heap.stats();
    CHECK_EQ(s.w[4], uint32_t(heap.region_bytes - uint64_t(heap.excluded_pages) * kPage64K));
    CHECK_EQ(s.w[5], uint32_t(heap.allocated_bytes));
    // The only physical allocation so far: the XMA hardware context array (64 KiB = 16 pages) that
    // register_xboxkrnl_hle() reserves in the physical window (src/hle_xboxkrnl_xma.cpp).
    constexpr uint32_t xma_pages = 16;
    CHECK_EQ(uint32_t(pages(runtime()->physical.stats().allocated_bytes)), xma_pages);
    CHECK_EQ(s.w[6], xma_pages);
    CHECK_EQ(s.w[9], 0u);                                     // no main module loaded
    for (uint32_t i = 14; i < 25; ++i) CHECK_EQ(s.w[i], 0u);  // system section
    CHECK_EQ(s.w[25], total - 1);
    const uint32_t available = s.w[3], reserved = s.w[5];

    // A physical allocation is backed: 1 MiB = 256 pages less available.
    const uint32_t physical = call(__imp__MmAllocatePhysicalMemoryEx, 0, kMiB, 4, 0, 0xFFFFFFFFu, 0);
    CHECK(physical != 0);
    s = query();
    CHECK_EQ(s.w[3], available - 256);
    CHECK_EQ(s.w[6], xma_pages + 256u);
    CHECK_EQ(s.w[5], reserved);  // physical memory is not title virtual address space here
    call(__imp__MmFreePhysicalMemory, 0, physical);
    s = query();
    CHECK_EQ(s.w[3], available); CHECK_EQ(s.w[6], xma_pages);

    // Physical windows, default build: every block at its 0xA0000000 address whatever its page size
    // (the layout GTA IV / EFLC run with); MmGetPhysicalAddress masks it.
    {
        const uint32_t small = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x3000, 4, 0, 0xFFFFFFFFu, 0x1000);
        const uint32_t large = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x10000, 0x20000004u, 0, 0xFFFFFFFFu, 0);
        CHECK(small >= 0xA0000000u && small < 0xC0000000u);
        CHECK(large >= 0xA0000000u && large < 0xC0000000u);
        CHECK_EQ(call(__imp__MmGetPhysicalAddress, small), small & 0x1FFFFFFFu);
        CHECK_EQ(call(__imp__MmGetPhysicalAddress, large), large & 0x1FFFFFFFu);
        s = query();
        CHECK_EQ(s.w[6], xma_pages + 3u + 16u);  // 3 pages of 4 KiB + 16 pages for the 64 KiB block
        call(__imp__MmFreePhysicalMemory, 0, small);
        call(__imp__MmFreePhysicalMemory, 0, large);
        s = query();
        CHECK_EQ(s.w[3], available); CHECK_EQ(s.w[6], xma_pages);
    }

    // Console window layout (RCOMP_PHYSICAL_4K_WINDOW_OFFSET): a 4 KiB-page block at physical P comes
    // back as 0xE0000000 + P - 0x1000 (virtual 0xE0000000 + X is physical X + 0x1000), where the host
    // aliases the windows; MmGetPhysicalAddress gives P, translate() reaches the same bytes as the
    // 0xA0000000 address, and the block frees through the address the title got.
    mem.set_physical_4k_offset(true);
    {
        const uint32_t small = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x3000, 4, 0, 0xFFFFFFFFu, 0x1000);
        CHECK(small != 0);
        const uint32_t physical = call(__imp__MmGetPhysicalAddress, small);
        if (small >= 0xE0000000u) {
            CHECK_EQ(small, 0xE0000000u + physical - 0x1000u);
            uint8_t* through_window = mem.translate(small, 0x3000);
            uint8_t* canonical = mem.translate(0xA0000000u + physical, 0x3000);
            CHECK(through_window && canonical);
            if (through_window && canonical) {
                through_window[0x2FFF] = 0x5A;
                CHECK_EQ(canonical[0x2FFF], 0x5Au);
            }
        } else {
            CHECK_EQ(small, 0xA0000000u + physical);  // host without aliasing: 0xA0000000 fallback
        }
        call(__imp__MmFreePhysicalMemory, 0, small);
        s = query();
        CHECK_EQ(s.w[3], available); CHECK_EQ(s.w[6], xma_pages);
    }
    mem.set_physical_4k_offset(false);

    // A reserve-only VM range costs address space, not pages, until committed.
    write(scratch + kBase, 0); write(scratch + kSize, 16 * kPage64K);
    CHECK_EQ(call(__imp__NtAllocateVirtualMemory, scratch + kBase, scratch + kSize, 0x60002000u, 4), 0u);
    const uint32_t region = read(scratch + kBase);
    s = query();
    CHECK_EQ(s.w[3], available);
    CHECK_EQ(s.w[5], reserved + 16 * kPage64K);
    write(scratch + kBase, region + kPage64K); write(scratch + kSize, 2 * kPage64K);
    CHECK_EQ(call(__imp__NtAllocateVirtualMemory, scratch + kBase, scratch + kSize, 0x1000u, 4), 0u);
    s = query();
    CHECK_EQ(s.w[3], available - 32);  // 128 KiB committed = 32 pages
    CHECK_EQ(s.w[3], uint32_t(total - expected_used()));
    write(scratch + kBase, region); write(scratch + kSize, 0);
    CHECK_EQ(call(__imp__NtFreeVirtualMemory, scratch + kBase, scratch + kSize, 0x8000u), 0u);
    s = query();
    CHECK_EQ(s.w[3], available); CHECK_EQ(s.w[5], reserved);

    // A committed allocation counts at once.
    write(scratch + kBase, 0); write(scratch + kSize, 4 * kPage64K);
    CHECK_EQ(call(__imp__NtAllocateVirtualMemory, scratch + kBase, scratch + kSize, 0x3000u, 4), 0u);
    s = query();
    CHECK_EQ(s.w[3], available - 64);
    write(scratch + kSize, 0);
    CHECK_EQ(call(__imp__NtFreeVirtualMemory, scratch + kBase, scratch + kSize, 0x8000u), 0u);

    // Errors: nothing written.
    CHECK_EQ(call(__imp__MmQueryStatistics, 0), 0xC000000Du);
    write(scratch + kStats, 100); write(scratch + kStats + 4, 0x12345678);
    CHECK_EQ(call(__imp__MmQueryStatistics, scratch + kStats), 0xC0000023u);
    CHECK_EQ(read(scratch + kStats + 4), 0x12345678u);
    CHECK_EQ(call(__imp__MmQueryStatistics, scratch + kStats + 2), 0xC0000005u);
}

void file_cache() {
    // GoW2: XSetFileCacheSize(128 KiB) -> FscSetCacheElementCount(0, 32).
    CHECK_EQ(runtime()->fsc_cache_elements.load(), 0u);
    CHECK_EQ(call(__imp__FscSetCacheElementCount, 0, 32), 0u);
    CHECK_EQ(runtime()->fsc_cache_elements.load(), 32u);
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__FscSetCacheElementCount, 1, 32), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_EQ(runtime()->fsc_cache_elements.load(), 32u);
}
}  // namespace

int main() {
    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK_ST(runtime_init(&mem), Status::Ok);
    clear_imports();
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &scratch), Status::Ok);
    statistics();
    file_cache();
    runtime_shutdown(); clear_imports(); mem.release();
    return test_result("rt_memory_statistics");
}
