// Production decoder and production MMIO, using XMA silence encoded here from
// the documented FFmpeg frame syntax. No compressed title assets or doubles.
#include <array>
#include <cstring>
#include <time.h>
#include <unistd.h>
#include <string>
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/virtual_fields.h"
#include "rcomp/runtime/xma.h"
#include "rcomp/xenos_gpu.h"
#include "test_util.h"
using namespace rcomp;
using namespace rcomp::rt;
namespace {
void bits(uint8_t* p, unsigned& at, uint32_t value, unsigned count) {
    for (unsigned n = 0; n < count; ++n, ++at) {
        const uint8_t mask = uint8_t(1u << (7 - (at & 7)));
        p[at / 8] = uint8_t((p[at / 8] & ~mask) | (((value >> (count - n - 1)) & 1) ? mask : 0));
    }
}
void silent_frame(uint8_t* p, unsigned& at, bool stereo, bool more) {
    const unsigned start = at;
    bits(p, at, stereo ? 36 : 31, 15);
    bits(p, at, 1, 1); // fixed channel layout
    bits(p, at, 0, 1); // full 512-sample tile
    if (stereo) bits(p, at, 0, 1); // no postprocessing matrix
    bits(p, at, 0, 8); // DRC gain field
    bits(p, at, 0, 1); // no stream trim fields
    bits(p, at, 0, 1); // no extended subframe header
    bits(p, at, 0, 1); // reserved bit
    if (stereo) {
        bits(p, at, 0, 1); // no unusual channel transform
        bits(p, at, 1, 1); bits(p, at, 0, 1); // uncoupled stereo
    }
    bits(p, at, 0, stereo ? 2 : 1); // both channels have zero coefficients
    bits(p, at, 0, 1); // frame fill bit
    bits(p, at, more, 1); // another frame starts in this packet
    CHECK_EQ(at - start, stereo ? 36u : 31u);
}
uint32_t reg(uint32_t index) {
    uint64_t value = 0;
    CHECK_EQ(runtime_virtual_read(0x7FEA0000u + 4 * index, 4, 0, &value), VirtualAccessStatus::Handled);
    return __builtin_bswap32(uint32_t(value));
}
void write_reg(uint32_t index, uint32_t value) {
    CHECK_EQ(runtime_virtual_write(0x7FEA0000u + 4 * index, 4, __builtin_bswap32(value), 0),
             VirtualAccessStatus::Handled);
}
uint32_t word(uint32_t context, unsigned index) { uint32_t value = 0; CHECK(guest_read_be32(context + index * 4, &value)); return value; }
bool wait_word(uint32_t context, unsigned index, uint32_t mask, uint32_t expected) {
    for (unsigned n = 0; n < 100; ++n) {
        if ((word(context, index) & mask) == expected) return true;
        timespec delay{0, 1000000}; nanosleep(&delay, nullptr);
    }
    return false;
}
void setup(GuestMemory& m, uint32_t context, bool stereo, unsigned frames, unsigned blocks) {
    uint8_t* input = m.translate(0xA2000000, 2048);
    std::memset(input, 0, 2048);
    unsigned at = 0;
    bits(input, at, frames, 6); bits(input, at, 0, 15); bits(input, at, 1, 3); bits(input, at, 0, 8);
    for (unsigned n = 0; n < frames; ++n) silent_frame(input, at, stereo, n + 1 < frames);
    std::memset(m.translate(0xA2010000, 0x10000), 0xA5, 0x10000);
    for (unsigned n = 0; n < 16; ++n) guest_write_be32(context + n * 4, 0);
    guest_write_be32(context, 1 | (1u << 20) | (blocks << 22));
    guest_write_be32(context + 4, 0x80000000u | (3u << 27) | (unsigned(stereo) << 29) | (4u << 20));
    guest_write_be32(context + 8, 32);
    guest_write_be32(context + 20, 0x2000000);
    guest_write_be32(context + 28, 0x2010000);
}
}
int main() {
    GuestMemory m;
    CHECK_EQ(m.reserve(), MemStatus::Ok);
    CHECK_EQ(m.commit(0x30000000, 0x10000, Protect::ReadWrite), MemStatus::Ok);
    CHECK_EQ(m.commit(0xA2000000, 0x20000, Protect::ReadWrite), MemStatus::Ok);
    CHECK_ST(runtime_init(&m), Status::Ok);
    CHECK_ST(register_xboxkrnl_xma_hle(), Status::Ok);
    PPCFunc* create = find_import(kModuleXboxkrnl, 0x224);
    PPCFunc* release = find_import(kModuleXboxkrnl, 0x226);
    CHECK(create && release);
    if (!create || !release) return test_result("rt_xma_decode");
    PPCContext p{}; p.r3.u32 = 0x30000000; create(p, m.base());
    uint32_t context = 0; CHECK(guest_read_be32(0x30000000, &context));
    CHECK_EQ(context, reg(0x600) + xenos::kXenosPhysicalWindow);

    setup(m, context, false, 1, 8);
    write_reg(0x607, 0xFFFFFFFFu); // malformed guest cursor cannot index pending[]
    write_reg(0x650, 1);
    CHECK(wait_word(context, 0, 0xF8000000u, 4u << 27));
    write_reg(0x690, 1); // production lock fences all subsequent checks
    CHECK_EQ(word(context, 0) & 0x00300000u, 0u);
    const uint8_t* output = m.translate(0xA2010000, 2048);
    for (unsigned n = 0; n < 1024; ++n) CHECK_EQ(output[n], 0u);
    for (unsigned n = 1024; n < 2048; ++n) CHECK_EQ(output[n], 0xA5u);
    guest_write_be32(context + 36, 4);
    write_reg(0x650, 1);
    CHECK(wait_word(context, 1, 0x80000000u, 0)); // only after guest drained PCM

    write_reg(0x6A0, 1);
    CHECK_EQ(word(context, 2) & 0x03FFFFFFu, 32u);
    CHECK_EQ(word(context, 0) & 0xF8300000u, 0u);
    setup(m, context, true, 1, 16);
    write_reg(0x650, 1);
    CHECK(wait_word(context, 0, 0xF8000000u, 8u << 27));
    write_reg(0x690, 1);
    for (unsigned n = 0; n < 2048; ++n) CHECK_EQ(output[n], 0u);

    // A full ring must not consume input; no availability is invented.
    write_reg(0x6A0, 1); setup(m, context, false, 1, 8);
    guest_write_be32(context, word(context, 0) | (7u << 27));
    write_reg(0x650, 1);
    timespec delay{0, 10000000}; nanosleep(&delay, nullptr);
    write_reg(0x690, 1);
    CHECK_EQ(word(context, 0) & 0x00300000u, 1u << 20);
    for (unsigned n = 0; n < 2048; ++n) CHECK_EQ(output[n], 0xA5u);
    guest_write_be32(context + 36, 7); write_reg(0x650, 1);
    CHECK(wait_word(context, 0, 0x00300000u, 0));
    write_reg(0x690, 1);
    CHECK_EQ((word(context, 0) >> 27) & 31, 3u); // ring wraps 7 -> 3

    // Invalid compressed frame is retained, with no output or success ack.
    write_reg(0x6A0, 1); setup(m, context, false, 1, 8);
    unsigned at = 32; bits(m.translate(0xA2000000, 2048), at, 0x7FFF, 15);
    write_reg(0x650, 1); nanosleep(&delay, nullptr); write_reg(0x690, 1);
    CHECK_EQ(word(context, 0) & 0x00300000u, 1u << 20);
    CHECK_EQ((word(context, 0) >> 27) & 31, 0u);

    // A frame with a valid length but the reserved subframe bit set reaches
    // the actual FFmpeg decoder (not the packet validator). Capture its real
    // warning through R-comp's logger, and verify failure retains input/PCM.
    write_reg(0x6A0, 1); setup(m, context, false, 1, 8);
    at = 59; bits(m.translate(0xA2000000, 2048), at, 1, 1);
    FILE* capture = std::tmpfile();
    CHECK(capture != nullptr);
    const int previous_stderr = dup(STDERR_FILENO);
    CHECK(previous_stderr >= 0);
    if (capture && previous_stderr >= 0) {
        CHECK_EQ(dup2(fileno(capture), STDERR_FILENO), STDERR_FILENO);
        write_reg(0x650, 1); nanosleep(&delay, nullptr); write_reg(0x690, 1);
        std::fflush(stderr);
        CHECK_EQ(dup2(previous_stderr, STDERR_FILENO), STDERR_FILENO);
        close(previous_stderr);
        std::rewind(capture);
        std::string text;
        char buffer[512];
        size_t size = 0;
        while ((size = std::fread(buffer, 1, sizeof(buffer), capture))) text.append(buffer, size);
        std::fclose(capture);
        CHECK(text.find("RCOMP-XMA-FFMPEG level=24") != std::string::npos);
        CHECK(text.find("Reserved bit") != std::string::npos);
        CHECK(text.find("frame decode ret=") != std::string::npos);
    } else {
        if (capture) std::fclose(capture);
        if (previous_stderr >= 0) close(previous_stderr);
    }
    CHECK_EQ(word(context, 0) & 0x00300000u, 1u << 20);
    CHECK_EQ((word(context, 0) >> 27) & 31, 0u);
    for (unsigned n = 0; n < 2048; ++n) CHECK_EQ(output[n], 0xA5u);

    // Release while a decode is pending and shutdown while another is pending
    // both synchronously retire decoder ownership before guest memory ends.
    write_reg(0x6A0, 1); setup(m, context, false, 4, 8); write_reg(0x650, 1);
    p.r3.u32 = context; release(p, m.base());
    p.r3.u32 = 0x30000000; create(p, m.base());
    setup(m, context, false, 4, 8); write_reg(0x650, 1);
    runtime_shutdown(); clear_imports();
    CHECK_ST(runtime_init(&m), Status::Ok);
    CHECK_ST(register_xboxkrnl_xma_hle(), Status::Ok);
    runtime_shutdown(); clear_imports();
    return test_result("rt_xma_decode");
}
