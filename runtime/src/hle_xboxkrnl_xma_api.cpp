// XMA* kernel helpers over R-comp's XMA device model (owner: Agent 3, runtime/). Contract:
// runtime/docs/XMA.md, "Kernel context helpers".
//
// The helpers read and write the 64-byte hardware context record (XMA_CONTEXT_DATA) that the device
// model and the decoder (src/hle_xboxkrnl_xma.cpp, src/xma_decoder.cpp) consume, and the per-context
// Kick / Lock / Clear registers, exactly as a title that drives the device itself would. Every record
// access holds the device mutex (src/xma_internal.h), so the decoder worker never loads or publishes a
// record halfway through a helper.
//
// Record layout (big-endian words; bit 0 = least significant), the same masks as src/xma_decoder.cpp:
//   w0: [0,12) input buffer 0 packet count, [12,8) loop count, bit 20/21 input buffer 0/1 valid,
//       [22,5) output buffer block count (256-byte blocks), [27,5) output buffer write offset
//   w1: [0,12) input buffer 1 packet count, [12,2) loop subframe end, [17,3) loop subframe skip,
//       [20,4) subframe decode count, [24,3) output padding, [27,2) sample rate, bit 29 stereo,
//       bit 31 output buffer valid
//   w2: [0,26) input buffer read offset (bits);  w3: [0,26) loop start;  w4: [0,26) loop end, bit 31
//       current input buffer;  w5/w6: input buffer 0/1 physical address;  w7: output buffer physical
//       address;  w8: work buffer;  w9: [0,5) output buffer read offset
// Entry points and XMA_CONTEXT_INIT (56 bytes): rexglue-sdk c94f5eb
// src/kernel/xboxkrnl/xboxkrnl_audio_xma.cpp and include/rex/audio/xma/context.h (BSD-3, derived from
// Xenia; read only, no code copied). Kick/Lock/Clear are the register groups 0x650/0x690/0x6A0 that the
// reference writes at byte offsets 0x1940/0x1A40/0x1A80.
#include <stdio.h>

#include "hle_more.h"
#include "physical_window.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "xma_internal.h"

namespace rcomp::rt {
namespace {

using xma_device::Signal;

constexpr uint32_t kRecordWords = 16;
constexpr uint32_t kInitBytes = 56;
constexpr uint32_t kOffsetMask = 0x03FFFFFFu;  // 26-bit bit offsets

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r || !r->mem) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xboxkrnl.exe!%s before runtime_init", fn);
    return *r;
}

uint32_t field(uint32_t word, unsigned shift, uint32_t mask) { return (word >> shift) & mask; }
uint32_t with_field(uint32_t word, unsigned shift, uint32_t mask, uint32_t value) {
    return (word & ~(mask << shift)) | ((value & mask) << shift);
}

uint32_t record_word(uint32_t record, unsigned word) {
    uint32_t value = 0;
    if (!guest_read_be32(record + 4 * word, &value))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "XMA context record 0x%08X word %u unreadable", record, word);
    return value;
}
void store_word(uint32_t record, unsigned word, uint32_t value) {
    if (!guest_write_be32(record + 4 * word, value))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "XMA context record 0x%08X word %u unwritable", record, word);
}

[[noreturn]] void not_a_context(const char* fn, uint32_t context, PPCContext& ctx) {
    rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,
                "xboxkrnl.exe!%s context 0x%08X is not a live XMACreateContext context lr=0x%08X", fn, context,
                uint32_t(ctx.lr));
}

void check_width(const char* fn, const char* what, uint32_t value, uint32_t mask, PPCContext& ctx) {
    if (value & ~mask)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xboxkrnl.exe!%s %s=0x%X does not fit its context field (mask 0x%X; no truncation contract) "
                    "lr=0x%08X",
                    fn, what, value, mask, uint32_t(ctx.lr));
}

// The physical address the hardware reads (MmGetPhysicalAddress). Only the physical windows have one in
// R-comp; a NULL pointer is accepted where `allow_null` says the reference does (an unused buffer).
uint32_t physical_address(Runtime& r, const char* fn, const char* what, uint32_t address, bool allow_null,
                          PPCContext& ctx) {
    if (!address && allow_null) return 0;
    if (!in_physical_windows(address))
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xboxkrnl.exe!%s %s 0x%08X is not in a physical memory window (no physical address) lr=0x%08X",
                    fn, what, address, uint32_t(ctx.lr));
    return physical_of_window(*r.mem, address);
}

// Reads one record word under the device mutex.
uint32_t read_field(const char* fn, PPCContext& ctx, unsigned word, unsigned shift, uint32_t mask) {
    current(fn);
    const uint32_t context = ctx.r3.u32;
    uint32_t value = 0;
    if (!xma_device::with_context(context, nullptr,
                                  [&](uint32_t record) { value = field(record_word(record, word), shift, mask); }))
        not_a_context(fn, context, ctx);
    return value;
}

// Read-modify-write of one record field under the device mutex.
void write_field(const char* fn, PPCContext& ctx, unsigned word, unsigned shift, uint32_t mask, uint32_t value) {
    current(fn);
    const uint32_t context = ctx.r3.u32;
    check_width(fn, "value", value, mask, ctx);
    if (!xma_device::with_context(context, nullptr, [&](uint32_t record) {
            store_word(record, word, with_field(record_word(record, word), shift, mask, value));
        }))
        not_a_context(fn, context, ctx);
}

// XMAInitializeContext (0x0225): (PVOID Context, const XMA_CONTEXT_INIT*) -> 0. Writes the Clear
// register (fences the decoder and resets its state for this context), then the whole record from the
// init block, under one device-mutex hold. The record is written after the Clear so the decoder build's
// clear side effects (read offset 32, flags) do not overwrite the title's values. The work buffer is
// not stored (the decoder uses none; the reference leaves it unset as well).
void XMAInitializeContext(PPCContext& ctx, uint8_t*) {
    const char* fn = "XMAInitializeContext";
    Runtime& r = current(fn);
    const uint32_t context = ctx.r3.u32, init = ctx.r4.u32;
    if (!init || !r.mem->is_accessible(init, kInitBytes, Protect::Read))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s init block 0x%08X not readable lr=0x%08X", fn, init,
                    uint32_t(ctx.lr));
    uint32_t v[11] = {};
    for (unsigned i = 0; i < 11; ++i) guest_read_be32(init + 4 * i, &v[i]);
    uint32_t loop_start = 0, loop_end = 0;
    guest_read_be32(init + 44, &loop_start);
    guest_read_be32(init + 48, &loop_end);
    const uint8_t* loop_bytes = r.mem->host(init + 52);  // XMA_LOOP_DATA: count, subframe end, subframe skip
    const uint32_t loop_count = loop_bytes[0], subframe_end = loop_bytes[1], subframe_skip = loop_bytes[2];
    const uint32_t in0_packets = v[1], in1_packets = v[3], read_offset = v[4], blocks = v[6], subframes = v[8],
                   channels = v[9], rate = v[10];
    check_width(fn, "input_buffer_0_packet_count", in0_packets, 0xFFF, ctx);
    check_width(fn, "input_buffer_1_packet_count", in1_packets, 0xFFF, ctx);
    check_width(fn, "input_buffer_read_offset", read_offset, kOffsetMask, ctx);
    check_width(fn, "output_buffer_block_count", blocks, 31, ctx);
    check_width(fn, "subframe_decode_count", subframes, 15, ctx);
    check_width(fn, "channel_count", channels, 1, ctx);  // reference: stereo when >= 1; 2+ not established
    check_width(fn, "sample_rate", rate, 3, ctx);
    check_width(fn, "loop_start", loop_start, kOffsetMask, ctx);
    check_width(fn, "loop_end", loop_end, kOffsetMask, ctx);
    check_width(fn, "loop_subframe_end", subframe_end, 3, ctx);
    check_width(fn, "loop_subframe_skip", subframe_skip, 7, ctx);
    const uint32_t in0 = physical_address(r, fn, "input buffer 0", v[0], true, ctx);
    const uint32_t in1 = physical_address(r, fn, "input buffer 1", v[2], true, ctx);
    const uint32_t out = physical_address(r, fn, "output buffer", v[5], false, ctx);
    uint32_t w[kRecordWords] = {};
    w[0] = with_field(w[0], 0, 0xFFF, in0_packets);
    w[0] = with_field(w[0], 12, 0xFF, loop_count);
    w[0] = with_field(w[0], 22, 31, blocks);
    w[1] = with_field(w[1], 0, 0xFFF, in1_packets);
    w[1] = with_field(w[1], 12, 3, subframe_end);
    w[1] = with_field(w[1], 17, 7, subframe_skip);
    w[1] = with_field(w[1], 20, 15, subframes);
    w[1] = with_field(w[1], 27, 3, rate);
    w[1] = with_field(w[1], 29, 1, channels);
    w[2] = read_offset;
    w[3] = loop_start;
    w[4] = loop_end;
    w[5] = in0;
    w[6] = in1;
    w[7] = out;
    const Signal clear = Signal::Clear;
    if (!xma_device::with_context(context, &clear, [&](uint32_t record) {
            for (unsigned i = 0; i < kRecordWords; ++i) store_word(record, i, w[i]);
        }))
        not_a_context(fn, context, ctx);
    ctx.r3.u64 = 0;
}

// XMAEnableContext (0x0227): (PVOID Context) -> 0. Kicks the context (one decoder visit).
void XMAEnableContext(PPCContext& ctx, uint8_t*) {
    const char* fn = "XMAEnableContext";
    current(fn);
    const Signal kick = Signal::Kick;
    if (!xma_device::with_context(ctx.r3.u32, &kick, [](uint32_t) {})) not_a_context(fn, ctx.r3.u32, ctx);
    ctx.r3.u64 = 0;
}

// XMADisableContext (0x0228): (PVOID Context, BOOL Wait) -> 0. Writes Lock, which cancels pending work
// and is fenced synchronously by the device mutex: when it returns no decode of the context is running,
// so the wait the title may ask for is already complete.
void XMADisableContext(PPCContext& ctx, uint8_t*) {
    const char* fn = "XMADisableContext";
    current(fn);
    const Signal lock = Signal::Lock;
    if (!xma_device::with_context(ctx.r3.u32, &lock, [](uint32_t) {})) not_a_context(fn, ctx.r3.u32, ctx);
    ctx.r3.u64 = 0;
}

// Buffers: (PVOID Context, PVOID Buffer, DWORD PacketCount) -> 0.
void set_input_buffer(PPCContext& ctx, const char* fn, unsigned buffer) {
    Runtime& r = current(fn);
    const uint32_t context = ctx.r3.u32, packets = ctx.r5.u32;
    check_width(fn, "packet_count", packets, 0xFFF, ctx);
    const uint32_t physical = physical_address(r, fn, "buffer", ctx.r4.u32, false, ctx);
    if (!xma_device::with_context(context, nullptr, [&](uint32_t record) {
            store_word(record, 5 + buffer, physical);
            store_word(record, buffer, with_field(record_word(record, buffer), 0, 0xFFF, packets));
        }))
        not_a_context(fn, context, ctx);
    ctx.r3.u64 = 0;
}
void XMASetInputBuffer0(PPCContext& ctx, uint8_t*) { set_input_buffer(ctx, "XMASetInputBuffer0", 0); }
void XMASetInputBuffer1(PPCContext& ctx, uint8_t*) { set_input_buffer(ctx, "XMASetInputBuffer1", 1); }

void XMASetInputBuffer0Valid(PPCContext& ctx, uint8_t*) {
    write_field("XMASetInputBuffer0Valid", ctx, 0, 20, 1, 1);
    ctx.r3.u64 = 0;
}
void XMASetInputBuffer1Valid(PPCContext& ctx, uint8_t*) {
    write_field("XMASetInputBuffer1Valid", ctx, 0, 21, 1, 1);
    ctx.r3.u64 = 0;
}
void XMAIsInputBuffer0Valid(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = read_field("XMAIsInputBuffer0Valid", ctx, 0, 20, 1); }
void XMAIsInputBuffer1Valid(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = read_field("XMAIsInputBuffer1Valid", ctx, 0, 21, 1); }

// XMASetInputBufferReadOffset (0x0237): (Context, DWORD BitOffset) -> 0.
void XMASetInputBufferReadOffset(PPCContext& ctx, uint8_t*) {
    write_field("XMASetInputBufferReadOffset", ctx, 2, 0, kOffsetMask, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

void XMASetOutputBufferValid(PPCContext& ctx, uint8_t*) {
    write_field("XMASetOutputBufferValid", ctx, 1, 31, 1, 1);
    ctx.r3.u64 = 0;
}
void XMAIsOutputBufferValid(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = read_field("XMAIsOutputBufferValid", ctx, 1, 31, 1); }

// Output ring cursors, in 256-byte blocks.
void XMASetOutputBufferReadOffset(PPCContext& ctx, uint8_t*) {
    write_field("XMASetOutputBufferReadOffset", ctx, 9, 0, 31, ctx.r4.u32);
    ctx.r3.u64 = 0;
}
void XMAGetOutputBufferReadOffset(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = read_field("XMAGetOutputBufferReadOffset", ctx, 9, 0, 31);
}
void XMAGetOutputBufferWriteOffset(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = read_field("XMAGetOutputBufferWriteOffset", ctx, 0, 27, 31);
}

struct Impl {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Impl kImpls[] = {
    {0x0225, "XMAInitializeContext", &XMAInitializeContext},
    {0x0227, "XMAEnableContext", &XMAEnableContext},
    {0x0228, "XMADisableContext", &XMADisableContext},
    {0x0229, "XMAGetOutputBufferWriteOffset", &XMAGetOutputBufferWriteOffset},
    {0x022A, "XMASetOutputBufferReadOffset", &XMASetOutputBufferReadOffset},
    {0x022B, "XMAGetOutputBufferReadOffset", &XMAGetOutputBufferReadOffset},
    {0x022C, "XMASetOutputBufferValid", &XMASetOutputBufferValid},
    {0x022D, "XMAIsOutputBufferValid", &XMAIsOutputBufferValid},
    {0x022E, "XMASetInputBuffer0Valid", &XMASetInputBuffer0Valid},
    {0x022F, "XMAIsInputBuffer0Valid", &XMAIsInputBuffer0Valid},
    {0x0230, "XMASetInputBuffer1Valid", &XMASetInputBuffer1Valid},
    {0x0231, "XMAIsInputBuffer1Valid", &XMAIsInputBuffer1Valid},
    {0x0232, "XMASetInputBuffer0", &XMASetInputBuffer0},
    {0x0233, "XMASetInputBuffer1", &XMASetInputBuffer1},
    {0x0237, "XMASetInputBufferReadOffset", &XMASetInputBufferReadOffset},
};

}  // namespace

Status register_xboxkrnl_xma_api_hle() {
    for (const Impl& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            return Status::InvalidArgument;
        const Status status = register_import(kModuleXboxkrnl, impl.ordinal, impl.function, impl.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
