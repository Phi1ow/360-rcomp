// Exercise the actual generated-code memory macros against production runtime
// providers, including real main/worker identities and retained object lifetime.
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include "rcomp/ppc_prelude.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/kernel_variables.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/virtual_fields.h"
#include "rcomp/runtime_state.h"
#include "test_util.h"

using namespace rcomp;
using namespace rcomp::rt;

namespace {
// Guest entry points for the running-thread checks (test doubles): they read the KTHREAD fields
// the way the title's GPU-wait loop does, through the generated-code memory macros.
struct SelfProbe {
    uint32_t own = 0, other = 0, thread_id = 0;
    bool ok = true;
    bool kernel_time_advanced = false;
    bool other_refused = false;
};
SelfProbe g_probe_a, g_probe_b;

void TESTDOUBLE_self_fields(SelfProbe& p, PPCContext& ctx, uint8_t* base, bool refuse_other) {
    uint32_t previous = PPC_LOAD_U32(p.own + 0x58);
    for (uint32_t i = 0; i < 20000; ++i) {
        const uint32_t now = PPC_LOAD_U32(p.own + 0x58);
        p.ok &= now >= previous;
        previous = now;
        p.ok &= PPC_LOAD_U32(p.own + 0x14C) == p.thread_id;
        PPC_STORE_U32(p.own + 0x160, i);
        p.ok &= PPC_LOAD_U32(p.own + 0x160) == i;
    }
    // The counter is the thread's own CPU time in 20 ms units: burn CPU until it moves.
    const auto start = std::chrono::steady_clock::now();
    while (PPC_LOAD_U32(p.own + 0x58) == previous &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
    }
    p.kernel_time_advanced = PPC_LOAD_U32(p.own + 0x58) > previous;
    if (refuse_other) {
        bool fatal = false;
        CAPTURE_FATAL((void)PPC_LOAD_U32(p.other + 0x58), fatal);
        p.other_refused = fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED;
    }
}
void TESTDOUBLE_guest_a(PPCContext& ctx, uint8_t* base) { TESTDOUBLE_self_fields(g_probe_a, ctx, base, true); }
void TESTDOUBLE_guest_b(PPCContext& ctx, uint8_t* base) { TESTDOUBLE_self_fields(g_probe_b, ctx, base, false); }
}  // namespace

int main() {
    GuestMemory memory;
    CHECK(memory.reserve() == MemStatus::Ok);
    CHECK_ST(runtime_init(&memory), Status::Ok);
    set_active_guest_memory(&memory);
    CHECK_ST(register_thread_object_type_variable(), Status::Ok);
    const uint32_t type = thread_object_type_address();
    CHECK_EQ(type, kExThreadObjectTypeVirtualAddress);

    GuestThread first, second;
    alignas(64) PPCContext ctx{}, second_context{};
    CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0x82001000, 0}, &ctx, &first), Status::Ok);
    CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0x82002000, 0}, &second_context, &second), Status::Ok);
    auto* base = memory.base();
    const auto first_body = thread_object_body(first.identity);
    const auto second_body = thread_object_body(second.identity);
    CHECK(first_body >= kThreadVirtualTokenBase && second_body != first_body);
    CHECK_EQ(PPC_LOAD_U32(first.pcr + kPcrCurrentThread), first_body);
    CHECK_EQ(PPC_LOAD_U32(second.pcr + kPcrCurrentThread), second_body);
    CHECK_EQ(PPC_LOAD_U32(first_body + 0x14C), first.thread_id);
    CHECK_EQ(PPC_LOAD_U32(second_body + 0x14C), second.thread_id);
    CHECK_EQ(PPC_LOAD_U32(first_body + 0x160), 0u);
    PPC_STORE_U32(first_body + 0x160, 0x89ABCDEFu);
    CHECK_EQ(PPC_LOAD_U32(first_body + 0x160), 0x89ABCDEFu);
    CHECK_EQ(PPC_LOAD_U32(second_body + 0x160), 0u);
    CHECK(!memory.is_committed(first_body, kThreadVirtualTokenStride));

    // Ordinary guest accesses still implement endian conversion and legal
    // unaligned scalar loads. Macro operands must not be evaluated twice.
    const uint32_t ram = first.initial_r1 - 64;
    unsigned evaluations = 0;
    auto address = [&] { ++evaluations; return ram + 1; };
    PPC_STORE_U64(address(), 0x0123456789ABCDEFull);
    CHECK_EQ(evaluations, 1u);
    CHECK_EQ(PPC_LOAD_U64(address()), 0x0123456789ABCDEFull);
    CHECK_EQ(evaluations, 2u);
    CHECK_EQ(base[ram + 1], 1u);
    CHECK_EQ(base[ram + 8], 0xEFu);
    PPC_STORE_U16(ram + 11, 0xABCD);
    CHECK_EQ(PPC_LOAD_U16(ram + 11), 0xABCDu);
    PPC_STORE_U8(ram + 13, 0x54);
    CHECK_EQ(PPC_LOAD_U8(ram + 13), 0x54u);

    bool fatal = false;
    ctx.lr = 0x829A27C0;
    CAPTURE_FATAL((void)PPC_LOAD_U32(first_body + 0x58), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(g_fatal_msg.find("offset=0x58") != std::string::npos);
    CHECK(g_fatal_msg.find("lr=0x829A27C0") != std::string::npos);
    CAPTURE_FATAL(PPC_STORE_U32(first_body + 0x14C, 123), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    CHECK_EQ(PPC_LOAD_U32(first_body + 0x14C), first.thread_id);
    CAPTURE_FATAL((void)PPC_LOAD_U64(first_body + 0x160), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(g_fatal_msg.find("invalid_width") != std::string::npos);
    // A crossing access must never reach a real host dereference of the arena.
    CAPTURE_FATAL((void)PPC_LOAD_U64(kOpaqueRuntimeArenaBase - 4), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);

    // The coarse range of the generated code, [0x6FFFFFF0, 0x80000000): an address inside it that no
    // provider window claims (the heap's upper part, or the 16 bytes below the arena) is an ordinary
    // access with the same endian conversion, counted; an address nobody serves in an exact window
    // is still fatal, and so is an uncommitted one.
    {
        const uint32_t high = 0x72000000u, edge = kVirtualCoarseBase;
        CHECK(memory.commit(high, 0x10000, Protect::ReadWrite) == MemStatus::Ok);
        CHECK(memory.commit(edge & ~0xFFFFu, 0x10000, Protect::ReadWrite) == MemStatus::Ok);
        const uint64_t before = virtual_window_fallback_count();
        PPC_STORE_U32(high + 4, 0x01234567u);
        CHECK_EQ(base[high + 4], 0x01u);
        CHECK_EQ(base[high + 7], 0x67u);
        CHECK_EQ(PPC_LOAD_U32(high + 4), 0x01234567u);
        PPC_STORE_U64(high + 9, 0x89ABCDEF01234567ull);
        CHECK_EQ(PPC_LOAD_U64(high + 9), 0x89ABCDEF01234567ull);
        CHECK_EQ(base[high + 9], 0x89u);
        PPC_STORE_U16(high + 32, 0xBEEF);
        CHECK_EQ(PPC_LOAD_U16(high + 32), 0xBEEFu);
        CHECK_EQ(base[high + 32], 0xBEu);
        PPC_STORE_U8(high + 40, 0x5A);
        CHECK_EQ(PPC_LOAD_U8(high + 40), 0x5Au);
        // The first address of the range, a scalar access that ends before the arena.
        PPC_STORE_U64(edge, 0x1122334455667788ull);
        CHECK_EQ(PPC_LOAD_U64(edge), 0x1122334455667788ull);
        CHECK_EQ(virtual_window_fallback_count() - before, 10u);
        // Addresses outside the range never take the out-of-line path.
        PPC_STORE_U32(ram, 0xCAFEF00Du);
        CHECK_EQ(PPC_LOAD_U32(ram), 0xCAFEF00Du);
        CHECK_EQ(virtual_window_fallback_count() - before, 10u);
        // An address nobody serves in an exact window: fatal as before (the XMA provider is not registered here).
        CAPTURE_FATAL((void)PPC_LOAD_U32(0x7FEA0000u), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
        CAPTURE_FATAL(PPC_STORE_U32(kOpaqueRuntimeArenaBase - 2, 1), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
        // An uncommitted page in the range is a guest access fault with the address, not a host crash.
        CAPTURE_FATAL((void)PPC_LOAD_U32(0x76000000u), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
        CAPTURE_FATAL(PPC_STORE_U8(0x76000000u, 1), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    }

    // The last-error field belongs to the retained identity, not a freed PCR.
    CHECK_ST(reference_thread_object(first_body), Status::Ok);
    CHECK_ST(destroy_guest_thread(runtime()->heap, &first), Status::Ok);
    CHECK_EQ(PPC_LOAD_U32(first_body + 0x160), 0x89ABCDEFu);
    CHECK_ST(dereference_thread_object(first_body), Status::Ok);
    CAPTURE_FATAL((void)PPC_LOAD_U32(first_body + 0x14C), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    CHECK(g_fatal_msg.find("status=stale") != std::string::npos);

    // Concurrent field access is atomic, with each thread retaining its own ID.
    std::atomic<bool> started{false};
    std::thread writer([&] {
        started = true;
        for (uint32_t n = 0; n < 10000; ++n)
            virtual_guest_store(second_body + 0x160, 4, n, 0x82002000);
    });
    while (!started) std::this_thread::yield();
    for (unsigned n = 0; n < 10000; ++n) {
        CHECK(virtual_guest_load(second_body + 0x160, 4, 0) < 10000);
        CHECK_EQ(virtual_guest_load(second_body + 0x14C, 4, 0), second.thread_id);
    }
    writer.join();
    CHECK_EQ(PPC_LOAD_U32(second_body + 0x160), 9999u);
    CHECK_ST(destroy_guest_thread(runtime()->heap, &second), Status::Ok);

    // A running guest thread reads its own fields through the per-thread cache, as the title's
    // GPU-wait loop does thousands of times a frame. Two native threads keep separate caches, a
    // thread refuses another thread's kernel time, and a thread that started after another one
    // ended finds nothing of its predecessor.
    {
        GuestThread third, fourth, fifth;
        alignas(64) PPCContext third_context{}, fourth_context{}, fifth_context{};
        CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0x82003000, 0}, &third_context, &third), Status::Ok);
        CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0x82004000, 0}, &fourth_context, &fourth), Status::Ok);
        CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0x82005000, 0}, &fifth_context, &fifth), Status::Ok);
        g_probe_a = {thread_object_body(third.identity), thread_object_body(fourth.identity), third.thread_id};
        g_probe_b = {thread_object_body(fourth.identity), thread_object_body(third.identity), fourth.thread_id};
        uint32_t code_a = 0, code_b = 0;
        std::thread worker([&] {
            CHECK_ST(run_guest_callback(fourth, fourth_context, base, TESTDOUBLE_guest_b, &code_b), Status::Ok);
        });
        CHECK_ST(run_guest_callback(third, third_context, base, TESTDOUBLE_guest_a, &code_a), Status::Ok);
        worker.join();
        CHECK(g_probe_a.ok && g_probe_b.ok);
        CHECK(g_probe_a.kernel_time_advanced && g_probe_b.kernel_time_advanced);
        CHECK(g_probe_a.other_refused);
        g_probe_b = {thread_object_body(fifth.identity), thread_object_body(third.identity), fifth.thread_id};
        std::thread later([&] {
            CHECK_ST(run_guest_callback(fifth, fifth_context, base, TESTDOUBLE_guest_b, &code_b), Status::Ok);
        });
        later.join();
        CHECK(g_probe_b.ok && g_probe_b.kernel_time_advanced);
        CHECK_ST(destroy_guest_thread(runtime()->heap, &third), Status::Ok);
        CHECK_ST(destroy_guest_thread(runtime()->heap, &fourth), Status::Ok);
        CHECK_ST(destroy_guest_thread(runtime()->heap, &fifth), Status::Ok);
    }
    set_active_guest_memory(nullptr);
    runtime_shutdown();
    memory.release();
    return test_result("rt_virtual_guest_access");
}
