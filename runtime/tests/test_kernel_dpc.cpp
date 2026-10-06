// DPC objects: KeInitializeDpc layout, KeInsertQueueDpc / KeRemoveQueueDpc
// results, real deferred execution on the runtime DPC worker at DISPATCH_LEVEL.
#include <atomic>
#include <chrono>
#include <thread>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__KeInitializeDpc);
PPC_EXTERN_FUNC(__imp__KeInsertQueueDpc);
PPC_EXTERN_FUNC(__imp__KeRemoveQueueDpc);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory mem;
uint32_t scratch;
constexpr uint32_t kBlocker = 0x00, kTarget = 0x40, kOther = 0x80, kRequeue = 0xC0;  // KDPCs (0x1C bytes)
constexpr uint32_t kBlockRoutine = 0x82000100, kRecordRoutine = 0x82000200, kRequeueRoutine = 0x82000300,
                   kMissingRoutine = 0x82000400;

std::atomic<bool> g_release{false};
std::atomic<int> g_block_runs{0}, g_record_runs{0}, g_requeue_runs{0};
std::atomic<uint32_t> g_seen[5];
std::atomic<uint32_t> g_irql{~0u}, g_host_thread_differs{0};
std::thread::id g_main_thread;

uint32_t read(uint32_t address) { uint32_t value = 0; CHECK(guest_read_be32(address, &value)); return value; }
uint32_t call(PPCFunc* fn, uint32_t a = 0, uint32_t b = 0, uint32_t c3 = 0) {
    PPCContext c{};
    c.r3.u64 = a; c.r4.u64 = b; c.r5.u64 = c3; c.lr = 0x82000010;
    fn(c, mem.base());
    return c.r3.u32;
}
bool eventually(const std::atomic<int>& value, int expected) {
    for (int i = 0; i < 500 && value.load() < expected; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return value.load() == expected;
}

void TESTDOUBLE_block(PPCContext&, uint8_t*) {
    g_block_runs.fetch_add(1);
    while (!g_release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
void TESTDOUBLE_record(PPCContext& c, uint8_t*) {
    g_seen[0] = c.r3.u32; g_seen[1] = c.r4.u32; g_seen[2] = c.r5.u32; g_seen[3] = c.r6.u32;
    GuestThread* t = current_guest_thread();
    g_irql = t ? t->irql : ~0u;
    g_host_thread_differs = std::this_thread::get_id() != g_main_thread;
    g_record_runs.fetch_add(1);
}
void TESTDOUBLE_requeue(PPCContext& c, uint8_t*) {
    // NT dequeues a DPC before running it: the routine may queue itself again.
    const int run = g_requeue_runs.fetch_add(1) + 1;
    if (run == 1) {
        PPCContext n{};
        n.r3.u64 = c.r3.u32; n.r4.u64 = 7; n.r5.u64 = 8;
        __imp__KeInsertQueueDpc(n, mem.base());
        g_seen[4] = n.r3.u32;
    }
}
}  // namespace

int main() {
    g_main_thread = std::this_thread::get_id();
    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK_ST(runtime_init(&mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &scratch), Status::Ok);
    const FuncEntry functions[] = {{kBlockRoutine, TESTDOUBLE_block, "TESTDOUBLE_block"},
                                   {kRecordRoutine, TESTDOUBLE_record, "TESTDOUBLE_record"},
                                   {kRequeueRoutine, TESTDOUBLE_requeue, "TESTDOUBLE_requeue"}};
    CHECK(register_functions(functions, 3));

    // KeInitializeDpc writes Type 19, the processor bytes, routine and context only.
    memset(mem.base() + scratch, 0xEE, 0x100);
    call(__imp__KeInitializeDpc, scratch + kTarget, kRecordRoutine, 0xC0C0);
    CHECK_EQ(mem.base()[scratch + kTarget], 0u); CHECK_EQ(mem.base()[scratch + kTarget + 1], 19u);
    CHECK_EQ(mem.base()[scratch + kTarget + 2], 0u); CHECK_EQ(mem.base()[scratch + kTarget + 3], 0u);
    CHECK_EQ(read(scratch + kTarget + 4), 0xEEEEEEEEu);  // list entry untouched
    CHECK_EQ(read(scratch + kTarget + 0x0C), kRecordRoutine);
    CHECK_EQ(read(scratch + kTarget + 0x10), 0xC0C0u);
    CHECK_EQ(read(scratch + kTarget + 0x14), 0xEEEEEEEEu);  // arguments untouched until insertion

    // Deferred execution on the DPC worker, with the routine's four arguments.
    CHECK_EQ(call(__imp__KeInsertQueueDpc, scratch + kTarget, 0xA1, 0xA2), 1u);
    CHECK(eventually(g_record_runs, 1));
    CHECK_EQ(g_seen[0].load(), scratch + kTarget);
    CHECK_EQ(g_seen[1].load(), 0xC0C0u);
    CHECK_EQ(g_seen[2].load(), 0xA1u);
    CHECK_EQ(g_seen[3].load(), 0xA2u);
    CHECK_EQ(g_irql.load(), 2u);  // DISPATCH_LEVEL
    CHECK_EQ(g_host_thread_differs.load(), 1u);
    CHECK_EQ(read(scratch + kTarget + 0x14), 0xA1u);

    // Queued state: TRUE once, FALSE while queued, removal cancels it.
    call(__imp__KeInitializeDpc, scratch + kBlocker, kBlockRoutine, 0);
    call(__imp__KeInitializeDpc, scratch + kOther, kRecordRoutine, 0xD0D0);
    CHECK_EQ(call(__imp__KeInsertQueueDpc, scratch + kBlocker, 0, 0), 1u);
    CHECK(eventually(g_block_runs, 1));  // the worker is now busy
    CHECK_EQ(call(__imp__KeInsertQueueDpc, scratch + kTarget, 0xB1, 0xB2), 1u);
    CHECK_EQ(call(__imp__KeInsertQueueDpc, scratch + kTarget, 0xC1, 0xC2), 0u);
    CHECK_EQ(read(scratch + kTarget + 0x14), 0xB1u);  // a refused insertion keeps the arguments
    CHECK_EQ(call(__imp__KeRemoveQueueDpc, scratch + kTarget), 1u);
    CHECK_EQ(call(__imp__KeRemoveQueueDpc, scratch + kTarget), 0u);
    CHECK_EQ(call(__imp__KeInsertQueueDpc, scratch + kOther, 0xE1, 0xE2), 1u);
    g_release = true;
    CHECK(eventually(g_record_runs, 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(g_record_runs.load(), 2);   // the removed DPC never ran
    CHECK_EQ(g_seen[1].load(), 0xD0D0u);
    CHECK_EQ(g_seen[2].load(), 0xE1u);

    // A routine may queue its own DPC again.
    call(__imp__KeInitializeDpc, scratch + kRequeue, kRequeueRoutine, 0);
    CHECK_EQ(call(__imp__KeInsertQueueDpc, scratch + kRequeue, 0, 0), 1u);
    CHECK(eventually(g_requeue_runs, 2));
    CHECK_EQ(g_seen[4].load(), 1u);
    CHECK_EQ(read(scratch + kRequeue + 0x14), 7u);

    // Misuse: an unwritable KDPC, a routine without an AOT function.
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__KeInitializeDpc, 0x50000000u, kRecordRoutine, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    call(__imp__KeInitializeDpc, scratch + kOther, kMissingRoutine, 0);
    CAPTURE_FATAL(call(__imp__KeInsertQueueDpc, scratch + kOther, 0, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_INDIRECT_TARGET);

    CHECK_ST(runtime()->heap.free(scratch), Status::Ok);
    runtime_shutdown();  // joins the DPC worker and frees its guest stack
    clear_imports();
    clear_functions();
    mem.release();
    return test_result("rt_kernel_dpc");
}
