// xboxkrnl deferred procedure calls: KeInitializeDpc, KeInsertQueueDpc,
// KeRemoveQueueDpc (owner: Agent 3, runtime/).
//
// KDPC layout (big-endian, 0x1C bytes; rexglue-sdk XDPC, BSD-3, layout only):
//   +0x00 u16 Type (19 = DpcObject)   +0x02 u8 SelectedCpuNumber
//   +0x03 u8 DesiredCpuNumber         +0x04 LIST_ENTRY DpcListEntry
//   +0x0C DeferredRoutine             +0x10 DeferredContext
//   +0x14 SystemArgument1             +0x18 SystemArgument2
// Routine: VOID (PKDPC Dpc, PVOID DeferredContext, PVOID SystemArgument1,
// PVOID SystemArgument2) -- the NT KDEFERRED_ROUTINE contract.
//
// Execution model: one runtime DPC worker (a host thread with its own guest
// stack, PCR and thread identity, started on the first insertion) drains a
// FIFO queue, NT's order for medium-importance DPCs, at DISPATCH_LEVEL (the
// worker's IRQL is 2 while a routine runs). All DPCs therefore run on one
// "processor": routines never run concurrently with each other, which NT
// guarantees per processor. The queued state is runtime-side: the guest
// LIST_ENTRY is not linked into a kernel list (no guest-visible DPC list
// exists in R-comp), exactly like Xenia's host-side DPC list. A DPC is dequeued
// before its routine runs, so the routine may queue it again (NT rule).
#include <pthread.h>
#include <string.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>

#include "kernel_internal.h"
#include "rcomp/diag.h"
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "physical_window.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"

namespace rcomp::rt {

Status register_xboxkrnl_dpc_hle();

namespace {

constexpr uint16_t kDpcObjectType = 19;
constexpr uint32_t kDpcBytes = 0x1C;
constexpr uint32_t kDispatchLevel = 2;
constexpr uint32_t kDpcStackSize = 0x40000;
constexpr size_t kHostStackSize = 8u << 20;

struct DpcQueue {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<uint32_t> order;  // KDPC addresses, FIFO
    std::set<uint32_t> queued;   // the same addresses, for the BOOLEAN results
    bool started = false, stopping = false;
    pthread_t thread{};
    uint64_t generation = 0;
};
DpcQueue g_dpc;

Runtime& rt_or_die(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s before runtime_init", fn);
    return *r;
}

uint8_t* dpc_bytes(Runtime& r, uint32_t dpc, const char* fn, PPCContext& ctx) {
    if (!dpc || (dpc & 3) || !r.mem->is_accessible(dpc, kDpcBytes, Protect::ReadWrite))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s KDPC 0x%08X unwritable or misaligned lr=0x%08X", fn, dpc,
                    (uint32_t)ctx.lr);
    return r.mem->host(dpc);
}

void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
uint32_t get32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void* dpc_worker_main(void*) {
    apply_host_service_affinity("DPC");
    Runtime* r = runtime();
    GuestThread thread;
    alignas(64) PPCContext ctx{};
    if (!r || create_guest_thread(r->heap, {kDpcStackSize, 0, 0}, &ctx, &thread) != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "DPC worker: no guest memory for the DPC stack");
    for (;;) {
        uint32_t dpc = 0;
        {
            std::unique_lock<std::mutex> lock(g_dpc.mu);
            g_dpc.cv.wait(lock, [] { return g_dpc.stopping || !g_dpc.order.empty(); });
            if (g_dpc.stopping) break;
            dpc = g_dpc.order.front();
            g_dpc.order.pop_front();
            g_dpc.queued.erase(dpc);
        }
        // The KDPC was validated when it was queued; read its fields now, at
        // execution, as the kernel does.
        if (!r->mem->is_accessible(dpc, kDpcBytes, Protect::Read))
            rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "DPC 0x%08X became unreadable before it ran", dpc);
        const uint8_t* p = r->mem->host(dpc);
        const uint32_t routine = get32(p + 0x0C), context = get32(p + 0x10);
        const uint32_t argument1 = get32(p + 0x14), argument2 = get32(p + 0x18);
        PPCFunc* fn = lookup_function(routine);
        if (!fn) rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET, "DPC 0x%08X routine 0x%08X has no recompiled function", dpc, routine);
        ctx = PPCContext{};
        ctx.r1.u64 = thread.initial_r1;
        ctx.r13.u64 = thread.pcr;
        ctx.r3.u64 = dpc;
        ctx.r4.u64 = context;
        ctx.r5.u64 = argument1;
        ctx.r6.u64 = argument2;
        ctx.lr = kGuestLrSentinel;
        ctx.fpscr.loadFromHost();
        thread.irql = kDispatchLevel;
        uint32_t code = 0;
        if (run_guest_callback(thread, ctx, r->mem->base(), fn, &code) != Status::Ok)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "DPC worker re-entered guest code");
        if (thread_object_exited(thread.identity))
            rcomp_fatal(RCOMP_FATAL_GUEST_TRAP, "DPC routine 0x%08X terminated the DPC context (ExTerminateThread at DISPATCH_LEVEL)",
                        routine);
        if (thread.irql != kDispatchLevel)
            rcomp_fatal(RCOMP_FATAL_GUEST_TRAP, "DPC routine 0x%08X returned at IRQL %u instead of DISPATCH_LEVEL", routine,
                        thread.irql);
    }
    if (retire_guest_thread(thread, 0) != Status::Ok || destroy_guest_thread(r->heap, &thread) != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "DPC worker: guest stack cleanup failed");
    return nullptr;
}

// Caller holds g_dpc.mu.
bool start_worker_locked(Runtime& r) {
    if (g_dpc.started) return true;
    g_dpc.stopping = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostStackSize);
    const int err = pthread_create(&g_dpc.thread, &attr, dpc_worker_main, nullptr);
    pthread_attr_destroy(&attr);
    if (err != 0) return false;
    g_dpc.started = true;
    g_dpc.generation = r.generation;
    return true;
}

// KeInitializeDpc (0x006F): VOID (PKDPC, PKDEFERRED_ROUTINE, PVOID Context).
// Writes Type, the processor bytes, routine and context (NT does not touch
// the list entry or the system arguments).
void KeInitializeDpc(PPCContext& ctx, uint8_t*) {
    const char* fn = "KeInitializeDpc";
    Runtime& r = rt_or_die(fn);
    uint8_t* p = dpc_bytes(r, ctx.r3.u32, fn, ctx);
    put16(p, kDpcObjectType);
    p[2] = 0;
    p[3] = 0;
    put32(p + 0x0C, ctx.r4.u32);
    put32(p + 0x10, ctx.r5.u32);
    note_title_write(ctx.r3.u32, 0x14);
}

// KeInsertQueueDpc (0x007B): BOOLEAN (PKDPC, PVOID SystemArgument1, PVOID
// SystemArgument2). FALSE when the DPC is already queued (arguments
// unchanged); otherwise stores the arguments, queues it and returns TRUE. The
// routine must be in the AOT function table (checked here, before queuing).
void KeInsertQueueDpc(PPCContext& ctx, uint8_t*) {
    const char* fn = "KeInsertQueueDpc";
    Runtime& r = rt_or_die(fn);
    const uint32_t dpc = ctx.r3.u32;
    uint8_t* p = dpc_bytes(r, dpc, fn, ctx);
    const uint32_t routine = get32(p + 0x0C);
    if (!lookup_function(routine))
        rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET, "xboxkrnl.exe!%s DPC 0x%08X routine 0x%08X has no recompiled function lr=0x%08X",
                    fn, dpc, routine, (uint32_t)ctx.lr);
    std::unique_lock<std::mutex> lock(g_dpc.mu);
    const char* failure = nullptr;
    if (g_dpc.started && g_dpc.generation != r.generation) failure = "a DPC worker of a previous runtime is still registered";
    else if (g_dpc.stopping) failure = "called during runtime shutdown";
    if (!failure && g_dpc.queued.count(dpc)) { ctx.r3.u64 = 0; return; }
    if (!failure && !start_worker_locked(r)) failure = "DPC worker thread creation failed";
    if (failure) {
        lock.unlock();  // no lock held across the fatal path
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xboxkrnl.exe!%s: %s", fn, failure);
    }
    put32(p + 0x14, ctx.r4.u32);
    put32(p + 0x18, ctx.r5.u32);
    note_title_write(dpc + 0x14, 8);
    g_dpc.queued.insert(dpc);
    g_dpc.order.push_back(dpc);
    g_dpc.cv.notify_one();
    ctx.r3.u64 = 1;
}

// KeRemoveQueueDpc (0x008E): BOOLEAN (PKDPC). TRUE when the DPC was queued and
// is now removed (its routine will not run for that insertion), else FALSE.
void KeRemoveQueueDpc(PPCContext& ctx, uint8_t*) {
    const char* fn = "KeRemoveQueueDpc";
    Runtime& r = rt_or_die(fn);
    const uint32_t dpc = ctx.r3.u32;
    (void)dpc_bytes(r, dpc, fn, ctx);
    std::lock_guard<std::mutex> lock(g_dpc.mu);
    if (!g_dpc.queued.erase(dpc)) { ctx.r3.u64 = 0; return; }
    for (auto it = g_dpc.order.begin(); it != g_dpc.order.end(); ++it)
        if (*it == dpc) { g_dpc.order.erase(it); break; }
    ctx.r3.u64 = 1;
}

struct Impl { uint32_t ordinal; const char* name; PPCFunc* function; };
constexpr Impl kImpls[] = {
    {0x006F, "KeInitializeDpc", &KeInitializeDpc},
    {0x007B, "KeInsertQueueDpc", &KeInsertQueueDpc},
    {0x008E, "KeRemoveQueueDpc", &KeRemoveQueueDpc},
};

}  // namespace

void shutdown_dpc_worker() {
    bool join = false;
    pthread_t thread{};
    {
        std::lock_guard<std::mutex> lock(g_dpc.mu);
        if (g_dpc.started) {
            g_dpc.stopping = true;
            join = true;
            thread = g_dpc.thread;
            g_dpc.cv.notify_all();
        }
    }
    if (join && pthread_join(thread, nullptr) != 0)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "DPC worker join failed");
    std::lock_guard<std::mutex> lock(g_dpc.mu);
    g_dpc.order.clear();
    g_dpc.queued.clear();
    g_dpc.started = false;
    g_dpc.stopping = false;
    g_dpc.generation = 0;
}

Status register_xboxkrnl_dpc_hle() {
    for (const auto& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            return Status::InvalidArgument;
        const Status status = register_import(kModuleXboxkrnl, impl.ordinal, impl.function, impl.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
