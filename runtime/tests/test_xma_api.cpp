// XMA* kernel context helpers (src/hle_xboxkrnl_xma_api.cpp) over the production XMA device model:
// the 64-byte records they write are the ones the device and decoder read (bit layout of
// src/xma_decoder.cpp), and Enable / Disable / Initialize reach the Kick / Lock / Clear registers of the
// production register provider exactly as a title store would.
#include <initializer_list>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/virtual_fields.h"
#include "rcomp/runtime/xma.h"
#include "rcomp/xenos_gpu.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__XMACreateContext);
PPC_EXTERN_FUNC(__imp__XMAReleaseContext);
PPC_EXTERN_FUNC(__imp__XMAInitializeContext);
PPC_EXTERN_FUNC(__imp__XMAEnableContext);
PPC_EXTERN_FUNC(__imp__XMADisableContext);
PPC_EXTERN_FUNC(__imp__XMASetInputBuffer0);
PPC_EXTERN_FUNC(__imp__XMASetInputBuffer1);
PPC_EXTERN_FUNC(__imp__XMASetInputBuffer0Valid);
PPC_EXTERN_FUNC(__imp__XMASetInputBuffer1Valid);
PPC_EXTERN_FUNC(__imp__XMAIsInputBuffer0Valid);
PPC_EXTERN_FUNC(__imp__XMAIsInputBuffer1Valid);
PPC_EXTERN_FUNC(__imp__XMASetInputBufferReadOffset);
PPC_EXTERN_FUNC(__imp__XMAIsOutputBufferValid);
PPC_EXTERN_FUNC(__imp__XMASetOutputBufferValid);
PPC_EXTERN_FUNC(__imp__XMASetOutputBufferReadOffset);
PPC_EXTERN_FUNC(__imp__XMAGetOutputBufferReadOffset);
PPC_EXTERN_FUNC(__imp__XMAGetOutputBufferWriteOffset);

using namespace rcomp;
using namespace rcomp::rt;

namespace {
constexpr uint32_t kRegisterBase = 0x7FEA0000u;
constexpr uint32_t kKick = 0x650, kLock = 0x690, kClear = 0x6A0;
constexpr uint32_t kScratch = 0x30000000u;  // init blocks and outputs
GuestMemory g_mem;

uint32_t call(PPCFunc* f, std::initializer_list<uint64_t> args) {
    alignas(64) PPCContext ctx{};
    PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    size_t i = 0;
    for (uint64_t v : args) regs[i++]->u64 = v;
    f(ctx, g_mem.base());
    return ctx.r3.u32;
}

uint32_t read_register(uint32_t index) {
    uint64_t value = 0;
    CHECK_EQ(runtime_virtual_read(kRegisterBase + 4 * index, 4, 0, &value), VirtualAccessStatus::Handled);
    return __builtin_bswap32(uint32_t(value));
}
uint32_t word(uint32_t context, unsigned w) {
    uint32_t v = 0;
    guest_read_be32(context + 4 * w, &v);
    return v;
}
void set_word(uint32_t context, unsigned w, uint32_t v) { guest_write_be32(context + 4 * w, v); }

uint32_t create() {
    CHECK_EQ(call(__imp__XMACreateContext, {kScratch}), 0u);
    uint32_t context = 0;
    guest_read_be32(kScratch, &context);
    return context;
}

// XMA_CONTEXT_INIT at kScratch + 0x100.
void put_init(uint32_t in0, uint32_t in0_packets, uint32_t in1, uint32_t in1_packets, uint32_t read_offset,
              uint32_t out, uint32_t blocks, uint32_t subframes, uint32_t channels, uint32_t rate,
              uint32_t loop_start, uint32_t loop_end, uint8_t loop_count, uint8_t subframe_end, uint8_t subframe_skip) {
    const uint32_t a = kScratch + 0x100;
    const uint32_t v[11] = {in0, in0_packets, in1, in1_packets, read_offset, out, blocks, 0xDEAD0000u, subframes,
                            channels, rate};
    for (unsigned i = 0; i < 11; ++i) guest_write_be32(a + 4 * i, v[i]);
    guest_write_be32(a + 44, loop_start);
    guest_write_be32(a + 48, loop_end);
    uint8_t* bytes = g_mem.host(a + 52);
    bytes[0] = loop_count;
    bytes[1] = subframe_end;
    bytes[2] = subframe_skip;
    bytes[3] = 0;
}
}  // namespace

int main() {
    CHECK(g_mem.reserve() == MemStatus::Ok);
    CHECK(g_mem.commit(kScratch, 0x10000, Protect::ReadWrite) == MemStatus::Ok);
    CHECK_ST(runtime_init(&g_mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_xma_hle(), Status::Ok);

    // Contexts 0 and 33 (second register group).
    const uint32_t c0 = create();
    uint32_t c33 = 0;
    for (int i = 1; i <= 33; ++i) c33 = create();
    CHECK_EQ(c33, c0 + 33 * 64);

    // Initialize: every field lands at its decoder bit position; buffers become physical addresses
    // (0xA... and 0xE... windows; no 4 KiB shift without the title option); the work buffer is not kept.
    for (unsigned w = 0; w < 16; ++w) set_word(c33, w, 0xFFFFFFFFu);  // stale record
    put_init(0xA0123000u, 0x7FF, 0, 0, 0x20, 0xE0456000u, 0x1F, 3, 1, 2, 0x12345, 0x3FFFFFF, 0xFF, 2, 4);
    CHECK_EQ(call(__imp__XMAInitializeContext, {c33, kScratch + 0x100}), 0u);
    CHECK_EQ(word(c33, 0), 0x7FFu | (0xFFu << 12) | (0x1Fu << 22));
    CHECK_EQ(word(c33, 1), (2u << 12) | (4u << 17) | (3u << 20) | (2u << 27) | (1u << 29));
    CHECK_EQ(word(c33, 2), 0x20u);
    CHECK_EQ(word(c33, 3), 0x12345u);
    CHECK_EQ(word(c33, 4), 0x3FFFFFFu);
    CHECK_EQ(word(c33, 5), 0x00123000u);
    CHECK_EQ(word(c33, 6), 0u);  // NULL input buffer 1 stays 0
    CHECK_EQ(word(c33, 7), 0x00456000u);
    for (unsigned w = 8; w < 16; ++w) CHECK_EQ(word(c33, w), 0u);
    CHECK_EQ(read_register(kClear + 1), 1u << 1);  // context 33 = group 1, bit 1

    // Valid flags, buffers, cursors.
    CHECK_EQ(call(__imp__XMAIsInputBuffer0Valid, {c33}), 0u);
    CHECK_EQ(call(__imp__XMAIsInputBuffer1Valid, {c33}), 0u);
    CHECK_EQ(call(__imp__XMASetInputBuffer0Valid, {c33}), 0u);
    CHECK_EQ(call(__imp__XMAIsInputBuffer0Valid, {c33}), 1u);
    CHECK_EQ(call(__imp__XMAIsInputBuffer1Valid, {c33}), 0u);
    CHECK_EQ(call(__imp__XMASetInputBuffer1Valid, {c33}), 0u);
    CHECK_EQ(call(__imp__XMAIsInputBuffer1Valid, {c33}), 1u);
    CHECK_EQ(word(c33, 0) & 0x00300000u, 0x00300000u);
    CHECK_EQ(call(__imp__XMASetInputBuffer1, {c33, 0xA0200000u, 16}), 0u);
    CHECK_EQ(word(c33, 6), 0x00200000u);
    CHECK_EQ(word(c33, 1) & 0xFFFu, 16u);
    CHECK_EQ(call(__imp__XMASetInputBuffer0, {c33, 0xA0300000u, 4}), 0u);
    CHECK_EQ(word(c33, 5), 0x00300000u);
    CHECK_EQ(word(c33, 0) & 0xFFFu, 4u);
    CHECK_EQ(word(c33, 0) & 0x00300000u, 0x00300000u);  // flags kept
    CHECK_EQ(call(__imp__XMASetInputBufferReadOffset, {c33, 0x1234}), 0u);
    CHECK_EQ(word(c33, 2), 0x1234u);
    CHECK_EQ(call(__imp__XMAIsOutputBufferValid, {c33}), 0u);
    CHECK_EQ(call(__imp__XMASetOutputBufferValid, {c33}), 0u);
    CHECK_EQ(call(__imp__XMAIsOutputBufferValid, {c33}), 1u);
    CHECK_EQ(word(c33, 1) >> 31, 1u);
    CHECK_EQ(call(__imp__XMASetOutputBufferReadOffset, {c33, 17}), 0u);
    CHECK_EQ(call(__imp__XMAGetOutputBufferReadOffset, {c33}), 17u);
    CHECK_EQ(word(c33, 9), 17u);
    // The write offset is hardware-owned (the decoder publishes it): read back from the record.
    set_word(c33, 0, (word(c33, 0) & ~0xF8000000u) | (9u << 27));
    CHECK_EQ(call(__imp__XMAGetOutputBufferWriteOffset, {c33}), 9u);
    // Other fields untouched by the setters.
    CHECK_EQ(word(c33, 3), 0x12345u);
    CHECK_EQ(word(c33, 7), 0x00456000u);

    // Enable kicks, Disable locks the context's bit in its group (group 0 for context 0).
    CHECK_EQ(call(__imp__XMAEnableContext, {c0}), 0u);
    CHECK_EQ(read_register(kKick + 0), 1u);
    CHECK_EQ(call(__imp__XMAEnableContext, {c33}), 0u);
    CHECK_EQ(read_register(kKick + 1), 2u);
    CHECK_EQ(call(__imp__XMADisableContext, {c33, 1}), 0u);
    CHECK_EQ(read_register(kLock + 1), 2u);
    // Without a decoder a kick consumes nothing: the record is unchanged by Enable/Disable.
    CHECK_EQ(word(c33, 2), 0x1234u);
    CHECK_EQ(word(c33, 0) & 0x00300000u, 0x00300000u);

    // Rejections: not a context, out-of-width values, buffers outside the physical windows.
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__XMAIsInputBuffer0Valid, {c0 + 4}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    CAPTURE_FATAL(call(__imp__XMASetInputBuffer0, {c33, 0xA0300000u, 0x1000}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(call(__imp__XMASetOutputBufferReadOffset, {c33, 32}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(call(__imp__XMASetInputBuffer1, {c33, kScratch, 1}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    put_init(0xA0123000u, 1, 0, 0, 32, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);  // NULL output buffer
    CAPTURE_FATAL(call(__imp__XMAInitializeContext, {c0, kScratch + 0x100}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    put_init(0xA0123000u, 1, 0, 0, 32, 0xA0400000u, 1, 0, 2, 0, 0, 0, 0, 0, 0);  // channel_count 2
    CAPTURE_FATAL(call(__imp__XMAInitializeContext, {c0, kScratch + 0x100}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    // A released context is no longer one.
    call(__imp__XMAReleaseContext, {c0});
    CAPTURE_FATAL(call(__imp__XMAEnableContext, {c0}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);

    runtime_shutdown();
    clear_imports();
    return test_result("rt_xma_api");
}
