// InterlockedPopEntrySList / InterlockedFlushSList on real guest memory. The
// push side is the guest's own inlined ldarx/stdcx. sequence, reproduced here
// as a 64-bit compare-and-swap on the big-endian header (TESTDOUBLE_push).
#include <atomic>
#include <thread>
#include <vector>
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__InterlockedPopEntrySList);
PPC_EXTERN_FUNC(__imp__InterlockedFlushSList);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory mem;
uint32_t scratch;  // header at +0, entries from +0x100, 16 bytes apart
constexpr uint32_t kEntries = 0x100;

uint64_t header_value() {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | mem.base()[scratch + i];
    return v;
}
uint32_t entry(uint32_t i) { return scratch + kEntries + 16 * i; }
// What a title's inlined InterlockedPushEntrySList does: depth + 1, sequence + 1.
void TESTDOUBLE_push(uint32_t header, uint32_t item) {
    auto* h = reinterpret_cast<uint64_t*>(mem.base() + header);
    uint64_t old = __atomic_load_n(h, __ATOMIC_SEQ_CST);
    for (;;) {
        const uint64_t v = __builtin_bswap64(old);
        CHECK(guest_write_be32(item, uint32_t(v >> 32)));
        const uint64_t nv = uint64_t(item) << 32 | uint64_t(uint16_t((v >> 16) + 1)) << 16 | uint16_t(v + 1);
        if (__atomic_compare_exchange_n(h, &old, __builtin_bswap64(nv), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return;
    }
}
uint32_t call(PPCFunc* fn, uint32_t r3) {
    alignas(64) PPCContext c{};
    c.r3.u64 = r3;
    fn(c, mem.base());
    return c.r3.u32;
}
}  // namespace

int main() {
    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK_ST(runtime_init(&mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x10000, 64, true, &scratch), Status::Ok);

    CHECK_EQ(call(__imp__InterlockedPopEntrySList, scratch), 0u);  // empty
    CHECK_EQ(header_value(), 0u);
    for (uint32_t i = 0; i < 3; ++i) TESTDOUBLE_push(scratch, entry(i));
    CHECK_EQ(header_value(), uint64_t(entry(2)) << 32 | 3u << 16 | 3u);
    CHECK_EQ(call(__imp__InterlockedPopEntrySList, scratch), entry(2));  // LIFO
    CHECK_EQ(header_value(), uint64_t(entry(1)) << 32 | 2u << 16 | 3u);  // depth - 1, sequence kept
    CHECK_EQ(call(__imp__InterlockedFlushSList, scratch), entry(1));
    CHECK_EQ(header_value(), 0u);
    uint32_t next = 0;
    CHECK(guest_read_be32(entry(1), &next));
    CHECK_EQ(next, entry(0));  // the detached chain is intact
    CHECK_EQ(call(__imp__InterlockedFlushSList, scratch), 0u);

    // Concurrent pushes (guest-style CAS) and HLE pops: nothing lost, nothing twice.
    constexpr uint32_t kCount = 2000;
    std::atomic<uint32_t> popped{0};
    std::vector<std::atomic<uint8_t>> seen(kCount);
    std::thread pusher([] {
        for (uint32_t i = 0; i < kCount; ++i) TESTDOUBLE_push(scratch, entry(i));
    });
    std::vector<std::thread> poppers;
    for (int t = 0; t < 3; ++t)
        poppers.emplace_back([&] {
            while (popped.load() < kCount) {
                const uint32_t got = call(__imp__InterlockedPopEntrySList, scratch);
                if (!got) { std::this_thread::yield(); continue; }
                const uint32_t index = (got - entry(0)) / 16;
                CHECK(index < kCount);
                CHECK_EQ(seen[index].fetch_add(1), 0u);
                popped.fetch_add(1);
            }
        });
    pusher.join();
    for (auto& t : poppers) t.join();
    CHECK_EQ(popped.load(), kCount);
    CHECK_EQ(uint32_t(header_value() >> 32), 0u);
    CHECK_EQ(uint32_t(header_value() >> 16) & 0xFFFF, 0u);  // depth back to zero

    // Misaligned header and an unreadable entry end in guest_access diagnostics.
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__InterlockedPopEntrySList, scratch + 4), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    CHECK(guest_write_be32(scratch, 0x00000010));  // first entry in the unmapped page 0
    CAPTURE_FATAL(call(__imp__InterlockedPopEntrySList, scratch), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    CHECK_EQ(uint32_t(header_value() >> 32), 0x10u);  // unchanged
    runtime_shutdown();
    clear_imports();
    return test_result("rt_slist");
}
