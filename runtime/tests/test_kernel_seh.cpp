// Structured exception exports: RtlCaptureContext (full CONTEXT), RtlUnwind
// (the implemented no-frame subset and every diagnostic), __C_specific_handler.
#include <math.h>
#include <string.h>

#include "kernel_image_util.h"
#include "rcomp/runtime/status.h"

PPC_EXTERN_FUNC(__imp__RtlCaptureContext);
PPC_EXTERN_FUNC(__imp__RtlUnwind);
PPC_EXTERN_FUNC(__imp____C_specific_handler);

using namespace rcomp;
using namespace rcomp::rt;

namespace {
GuestMemory mem;
constexpr uint32_t scratch = 0x30000000;  // committed by the test, outside the heap
uint32_t be32(uint32_t a) { uint32_t v = 0; CHECK(guest_read_be32(a, &v)); return v; }
uint64_t be64(uint32_t a) { uint64_t v = 0; CHECK(guest_read_be64(a, &v)); return v; }

void capture_context() {
    alignas(64) PPCContext c{};
    PPCRegister* gpr[32] = {&c.r0,  &c.r1,  &c.r2,  &c.r3,  &c.r4,  &c.r5,  &c.r6,  &c.r7,  &c.r8,  &c.r9,  &c.r10,
                            &c.r11, &c.r12, &c.r13, &c.r14, &c.r15, &c.r16, &c.r17, &c.r18, &c.r19, &c.r20, &c.r21,
                            &c.r22, &c.r23, &c.r24, &c.r25, &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31};
    for (uint32_t i = 0; i < 32; ++i) gpr[i]->u64 = 0x1111111100000000ull + i * 0x01010101u;
    c.r1.u64 = 0x7000FF00u;
    c.r3.u64 = scratch;  // the CONTEXT argument
    PPCRegister* fpr[32] = {&c.f0,  &c.f1,  &c.f2,  &c.f3,  &c.f4,  &c.f5,  &c.f6,  &c.f7,  &c.f8,  &c.f9,  &c.f10,
                            &c.f11, &c.f12, &c.f13, &c.f14, &c.f15, &c.f16, &c.f17, &c.f18, &c.f19, &c.f20, &c.f21,
                            &c.f22, &c.f23, &c.f24, &c.f25, &c.f26, &c.f27, &c.f28, &c.f29, &c.f30, &c.f31};
    for (uint32_t i = 0; i < 32; ++i) fpr[i]->f64 = 1.5 + i;
    c.lr = 0x82001238;
    c.ctr.u64 = 0xC0DE0000AA55ull;
    c.msr = 0x9032;
    c.cr0 = {1, 0, 0, {0}};  // lt
    c.cr2 = {0, 1, 1, {1}};  // gt eq so (a non-volatile field)
    c.cr7 = {0, 0, 1, {0}};  // eq
    c.xer.so = 1; c.xer.ca = 1;
    for (uint32_t b = 0; b < 16; ++b) { c.v5.u8[b] = uint8_t(b); c.v127.u8[b] = uint8_t(0xF0 + b); }
    memset(mem.base() + scratch, 0xCC, 0xA40);
    __imp__RtlCaptureContext(c, mem.base());

    CHECK_EQ(be32(scratch + 0x000), 0x17u);           // CONTROL | FLOATING_POINT | INTEGER | VECTOR
    CHECK_EQ(be32(scratch + 0x004), 0x9032u);         // Msr
    CHECK_EQ(be32(scratch + 0x008), 0x82001238u);     // Iar = return address
    CHECK_EQ(be32(scratch + 0x00C), 0x82001238u);     // Lr
    CHECK_EQ(be64(scratch + 0x010), 0xC0DE0000AA55ull);
    CHECK_EQ(be64(scratch + 0x018 + 8 * 1), 0x7000FF00ull);   // caller's stack pointer
    CHECK_EQ(be64(scratch + 0x018 + 8 * 3), uint64_t(scratch));
    CHECK_EQ(be64(scratch + 0x018 + 8 * 31), 0x1111111100000000ull + 31 * 0x01010101ull);
    CHECK_EQ(be32(scratch + 0x118), 0x80000000u | 0x00700000u | 0x00000002u);  // cr0 lt, cr2 gt|eq|so, cr7 eq
    CHECK_EQ(be32(scratch + 0x11C), 0xA0000000u);     // SO | CA
    CHECK_EQ(be64(scratch + 0x120), uint64_t(c.fpscr.loadFromHost()));  // what mffs reads
    double f17 = 0;
    const uint64_t raw = be64(scratch + 0x128 + 8 * 17);
    memcpy(&f17, &raw, 8);
    CHECK(f17 == 18.5);
    CHECK_EQ(be32(scratch + 0x228), 0u);              // UserModeControl (not modelled)
    CHECK_EQ(be32(scratch + 0x23C), 0x00010000u);     // VSCR NJ
    for (uint32_t b = 0; b < 16; ++b) {
        CHECK_EQ(mem.base()[scratch + 0x240 + 16 * 5 + b], uint8_t(15 - b));      // as stvx stores v5
        CHECK_EQ(mem.base()[scratch + 0x240 + 16 * 127 + b], uint8_t(0xFF - b));
    }
    CHECK_EQ(c.r3.u32, scratch);  // VOID: registers unchanged
    CHECK_EQ(c.r1.u32, 0x7000FF00u);

    // A CONTEXT that cannot be written is a guest fault.
    bool fatal = false;
    c.r3.u64 = scratch + 0x20000 - 0x100;  // runs past the committed range
    CAPTURE_FATAL(__imp__RtlCaptureContext(c, mem.base()), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    c.r3.u64 = scratch + 2;
    CAPTURE_FATAL(__imp__RtlCaptureContext(c, mem.base()), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
}

uint32_t unwind(uint32_t frame, uint32_t ip, uint32_t sp, uint32_t lr, uint32_t value, bool* fatal) {
    alignas(64) PPCContext c{};
    c.r3.u64 = frame; c.r4.u64 = ip; c.r5.u64 = 0; c.r6.u64 = value;
    c.r1.u64 = sp; c.lr = lr;
    CAPTURE_FATAL(__imp__RtlUnwind(c, mem.base()), *fatal);
    return c.r3.u32;
}
}  // namespace

int main() {
    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK(mem.commit(scratch, 0x20000, Protect::ReadWrite) == MemStatus::Ok);
    RuntimeConfig cfg;
    cfg.heap_hi = 0x40100000;
    CHECK_ST(runtime_init(&mem, cfg), Status::Ok);
    clear_imports();
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);

    capture_context();

    bool fatal = false;
    const uint32_t sp = 0x7000FE00u;
    // Without a finalized main image no .pdata can prove the caller has no handler.
    unwind(sp, kimage::plain_function + 8, sp, kimage::plain_function + 8, 0x77, &fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(g_fatal_msg.find("no finalized main image") != std::string::npos);

    CHECK(kimage::load_main(mem));
    // The implemented subset: target = caller's frame at the return address, caller without handler.
    CHECK_EQ(unwind(sp, kimage::plain_function + 8, sp, kimage::plain_function + 8, 0x77, &fatal), 0x77u);
    CHECK(!fatal);
    // A caller whose .pdata entry has an exception handler: its termination handlers would run.
    unwind(sp, kimage::handler_function + 8, sp, kimage::handler_function + 8, 0x77, &fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(g_fatal_msg.find("has an exception handler") != std::string::npos);
    // A caller outside every .pdata entry.
    unwind(sp, kimage::base + 0x1F00, sp, kimage::base + 0x1F00, 0x77, &fatal);
    CHECK(fatal && g_fatal_msg.find("address not covered") != std::string::npos);
    // Non-local unwinds (another frame or another resume address) and exit unwinds.
    unwind(sp + 0x100, kimage::plain_function + 8, sp, kimage::plain_function + 8, 0, &fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(g_fatal_msg.find("longjmp_address") != std::string::npos);
    unwind(sp, kimage::plain_function + 0x20, sp, kimage::plain_function + 8, 0, &fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    unwind(0, 0, sp, kimage::plain_function + 8, 0, &fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(g_fatal_msg.find("exit unwind") != std::string::npos);

    // __C_specific_handler is reachable only from a dispatcher: always reported.
    {
        alignas(64) PPCContext c{};
        CHECK(guest_write_be32(scratch + 0x1000, 0xC0000005u));
        CHECK(guest_write_be32(scratch + 0x1004, 2u));
        c.r3.u64 = scratch + 0x1000; c.r4.u64 = sp; c.r5.u64 = scratch; c.r6.u64 = scratch + 0x1100;
        CAPTURE_FATAL(__imp____C_specific_handler(c, mem.base()), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
        CHECK(g_fatal_msg.find("code=0xC0000005 flags=0x00000002") != std::string::npos);
    }

    runtime_shutdown();
    clear_functions();
    clear_imports();
    mem.release();
    return test_result("rt_kernel_seh");
}
