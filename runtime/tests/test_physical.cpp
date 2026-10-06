// Physical memory exports (src/hle_xboxkrnl_memory.cpp), called through the
// same symbols generated code uses, plus GuestHeap::alloc_in.
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/xenos_gpu.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__MmAllocatePhysicalMemory);
PPC_EXTERN_FUNC(__imp__MmAllocatePhysicalMemoryEx);
PPC_EXTERN_FUNC(__imp__MmFreePhysicalMemory);
PPC_EXTERN_FUNC(__imp__MmGetPhysicalAddress);
PPC_EXTERN_FUNC(__imp__MmQueryAllocationSize);

using namespace rcomp;
using namespace rcomp::rt;
using rcomp::xenos::kXenosPhysicalSize;
using rcomp::xenos::kXenosPhysicalWindow;

namespace {
constexpr uint32_t RW = 0x04, NOCACHE = 0x200, WC = 0x400, LARGE = 0x20000000, M16 = 0x80000000;

uint8_t* g_base;

uint32_t alloc_ex(uint32_t flags, uint32_t size, uint32_t prot, uint32_t lo, uint32_t hi, uint32_t align) {
    alignas(64) PPCContext ctx{};
    ctx.r3.u64 = flags;
    ctx.r4.u64 = size;
    ctx.r5.u64 = prot;
    ctx.r6.u64 = lo;
    ctx.r7.u64 = hi;
    ctx.r8.u64 = align;
    __imp__MmAllocatePhysicalMemoryEx(ctx, g_base);
    return ctx.r3.u32;
}
uint32_t call1(PPCFunc* f, uint32_t a) {
    alignas(64) PPCContext ctx{};
    ctx.r3.u64 = a;
    f(ctx, g_base);
    return ctx.r3.u32;
}
void free_phys(uint32_t a) {
    alignas(64) PPCContext ctx{};
    ctx.r3.u64 = 0;
    ctx.r4.u64 = a;
    __imp__MmFreePhysicalMemory(ctx, g_base);
}
bool in_window(uint32_t a) { return a >= kXenosPhysicalWindow && a - kXenosPhysicalWindow < kXenosPhysicalSize; }
}  // namespace

int main() {
    GuestMemory mem;
    if (mem.reserve() != MemStatus::Ok) return 2;
    g_base = mem.base();
    CHECK_ST(runtime_init(&mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    Runtime& r = *runtime();
    // Registered services own real physical allocations (the XMA context
    // array, for example). Title allocations must return to this baseline.
    const auto physical_initial = r.physical.stats();

    // Default (4 KiB pages): committed, zeroed, in the window, top-down.
    const uint32_t a = alloc_ex(0, 0x1234, RW | WC, 0, 0xFFFFFFFF, 0);
    CHECK(in_window(a));
    CHECK_EQ(a & 0xFFF, 0u);
    CHECK(mem.translate(a, 0x2000) != nullptr);
    CHECK_EQ(g_base[a], 0);
    CHECK_EQ(call1(__imp__MmGetPhysicalAddress, a), a - kXenosPhysicalWindow);
    CHECK_EQ(call1(__imp__MmGetPhysicalAddress, a + 0x10), a - kXenosPhysicalWindow + 0x10);
    CHECK_EQ(call1(__imp__MmQueryAllocationSize, a), 0x2000u);
    CHECK(a - kXenosPhysicalWindow >= kXenosPhysicalSize - 0x100000);  // top-down placement

    // 64 KiB pages + alignment; physical range limits honoured.
    const uint32_t b = alloc_ex(0, 0x10001, RW | LARGE | NOCACHE, 0x01000000, 0x01FFFFFF, 0x40000);
    CHECK(in_window(b));
    CHECK_EQ((b - kXenosPhysicalWindow) & 0x3FFFF, 0u);
    CHECK(b - kXenosPhysicalWindow >= 0x01000000 && b - kXenosPhysicalWindow + 0x20000 <= 0x02000000);
    CHECK_EQ(call1(__imp__MmQueryAllocationSize, b), 0x20000u);

    // 16 MiB pages.
    const uint32_t c = alloc_ex(0, 0x100, RW | M16, 0, 0xFFFFFFFF, 0);
    CHECK(in_window(c));
    CHECK_EQ((c - kXenosPhysicalWindow) & 0xFFFFFF, 0u);
    CHECK_EQ(call1(__imp__MmQueryAllocationSize, c), 0x1000000u);

    // MmAllocatePhysicalMemory == Ex(…, 0, MAX, 0); zero size -> NULL;
    // impossible range -> NULL; too big -> NULL.
    CHECK_EQ(alloc_ex(0, 0, RW, 0, 0xFFFFFFFF, 0), 0u);
    {
        alignas(64) PPCContext ctx{};
        ctx.r4.u64 = 0x3000;
        ctx.r5.u64 = RW;
        __imp__MmAllocatePhysicalMemory(ctx, g_base);
        CHECK(in_window(ctx.r3.u32));
        free_phys(ctx.r3.u32);
    }
    CHECK_EQ(alloc_ex(0, 0x20000, RW, 0x1000, 0x1FFFF, 0), 0u);  // range smaller than the block
    CHECK_EQ(alloc_ex(0, 0x20000001u, RW, 0, 0xFFFFFFFF, 0), 0u);

    // Free, then the same space is reusable; MmQueryAllocationSize -> 0.
    free_phys(b);
    CHECK_EQ(call1(__imp__MmQueryAllocationSize, b), 0u);
    const uint32_t b2 = alloc_ex(0, 0x10001, RW | LARGE, 0x01000000, 0x01FFFFFF, 0x40000);
    CHECK_EQ(b2, b);

    // Traps: read-only protection, non-zero flags, free of a non-allocation,
    // physical address of memory outside the window.
    bool fatal = false;
    CAPTURE_FATAL(alloc_ex(0, 0x1000, 0x02, 0, 0xFFFFFFFF, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(alloc_ex(1, 0x1000, RW, 0, 0xFFFFFFFF, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(free_phys(a + 0x1000), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    uint32_t v = 0;
    CHECK_ST(r.heap.alloc(0x100, 16, true, &v), Status::Ok);
    CAPTURE_FATAL(call1(__imp__MmGetPhysicalAddress, v), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_EQ(call1(__imp__MmQueryAllocationSize, v), 0x100u);

    // alloc_in bottom-up / top-down inside bounds.
    uint32_t x = 0, y = 0;
    CHECK_ST(r.heap.alloc_in(0x10000, 0x10000, 0x50000000, 0x50100000, false, false, &x), Status::Ok);
    CHECK_EQ(x, 0x50000000u);
    CHECK_ST(r.heap.alloc_in(0x10000, 0x10000, 0x50000000, 0x50100000, true, false, &y), Status::Ok);
    CHECK_EQ(y, 0x500F0000u);
    CHECK_ST(r.heap.alloc_in(0x200000, 0x10000, 0x50000000, 0x50100000, true, false, &y), Status::OutOfMemory);

    free_phys(a);
    free_phys(c);
    free_phys(b2);
    const auto physical_final = r.physical.stats();
    CHECK_EQ(physical_final.live_allocations, physical_initial.live_allocations);
    CHECK_EQ(physical_final.allocated_bytes, physical_initial.allocated_bytes);
    CHECK_EQ(physical_final.free_bytes, physical_initial.free_bytes);
    CHECK(r.physical_allocations.empty());
    runtime_shutdown();
    clear_imports();
    return test_result("rt_physical");
}
