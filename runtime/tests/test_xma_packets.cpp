// Synthetic protocol and PCM oracles. No title data or decoder double.
#include "../src/xma_decoder.h"
#include "test_util.h"
#include "rcomp/xenos_gpu.h"
#include <limits>
#include <cstring>
using namespace rcomp;
using namespace rcomp::rt;
using namespace rcomp::rt::xma;
namespace {
void bits(uint8_t* p, unsigned offset, uint32_t value, unsigned count) {
    for (unsigned n = 0; n < count; ++n) {
        const unsigned at = offset + n;
        const uint8_t mask = uint8_t(1u << (7 - (at & 7)));
        p[at / 8] = uint8_t((p[at / 8] & ~mask) | (((value >> (count - n - 1)) & 1) ? mask : 0));
    }
}
void header(uint8_t* p, unsigned first, unsigned frames, unsigned skip = 0) {
    bits(p, 0, frames, 6); bits(p, 6, first - 32, 15);
    bits(p, 21, 1, 3); bits(p, 24, skip, 8);
}
}
int main() {
    GuestMemory m;
    CHECK_EQ(m.reserve(), MemStatus::Ok);
    const uint32_t address = xenos::kXenosPhysicalWindow + 0x2000000;
    CHECK_EQ(m.commit(address, 0x10000, Protect::ReadWrite), MemStatus::Ok);
    uint8_t* input = m.translate(address, 0x10000);
    Context c{}; c[0] = 2 | (1u << 20); c[2] = 32; c[5] = 0x2000000;
    header(input, 32, 2);
    bits(input, 32, 31, 15); bits(input, 47, 0xA55B, 16);  // trailer=1
    bits(input, 63, 31, 15); bits(input, 78, 0x1234, 16);  // trailer=0
    header(input + 2048, 32, 1);
    bits(input + 2048, 32, 31, 15); bits(input + 2048, 47, 0x1234, 16);
    Packet packet;
    CHECK_EQ(assemble_packet(m, c, &packet), PacketResult::Frame);
    CHECK_EQ(packet.frame_bits, 31u);
    CHECK_EQ(packet.size, 5u);
    CHECK_EQ(packet.bytes[0], 4u);
    CHECK_EQ(packet.next[2] & kCursorMask, 63u);
    CHECK_EQ(packet.next[0] & kInputValid, 1u << 20);

    CHECK_EQ(assemble_packet(m, packet.next, &packet), PacketResult::Frame);
    CHECK_EQ(packet.next[2] & kCursorMask, kPacketBits + 32u);
    CHECK_EQ(assemble_packet(m, packet.next, &packet), PacketResult::Frame);
    CHECK_EQ(packet.next[0] & kInputValid, 0u);
    CHECK_EQ(assemble_packet(m, packet.next, &packet), PacketResult::NeedInput);

    // Frame header and body cross a packet boundary; skip the unrelated
    // middle stream packet, and strip the continuation packet's header.
    std::memset(input, 0, 0x10000);
    c[0] = 3 | (1u << 20); c[2] = kPacketBits - 10;
    header(input, c[2], 1, 1);
    header(input + 4096, 63, 1);
    const uint32_t frame = (31u << 16) | 0xA55Au;
    bits(input, c[2], frame >> 21, 10);
    bits(input + 4096, 32, frame & 0x1FFFFF, 21);
    CHECK_EQ(assemble_packet(m, c, &packet), PacketResult::Frame);
    CHECK_EQ(packet.frame_bits, 31u);
    CHECK_EQ(packet.next[2] & kCursorMask, 2 * kPacketBits + 63u);
    CHECK_EQ(packet.next[0] & kInputValid, 1u << 20);

    // The same fragment spans the two input buffers instead of two packets
    // in one buffer. Only buffer 0 is retired, and buffer 1's first new frame
    // remains selected for the following decode.
    header(input, c[2], 1, 0); // no interleaved stream packet in this case
    c[0] = 1 | kInputValid; c[1] = 1; c[6] = 0x2001000;
    CHECK_EQ(assemble_packet(m, c, &packet), PacketResult::Frame);
    CHECK_EQ(packet.next[0] & kInputValid, 1u << 21);
    CHECK_EQ(packet.next[4] >> 31, 1u);
    CHECK_EQ(packet.next[2] & kCursorMask, 63u);

    // Incomplete input must preserve the source context exactly; the adapter
    // plans transitions but never publishes them on NeedInput or Invalid.
    c[0] = 1 | (1u << 20);
    const Context before = c;
    CHECK_EQ(assemble_packet(m, c, &packet), PacketResult::NeedInput);
    CHECK(c == before);
    c[2] = 32; header(input, 32, 1); bits(input, 32, 0x7FFF, 15);
    CHECK_EQ(assemble_packet(m, c, &packet), PacketResult::Invalid);
    c[5] = 0xFFFFFFFFu;
    CHECK_EQ(assemble_packet(m, c, &packet), PacketResult::Invalid);
    c[5] = 0x2000000;

    // Loop end jumps exactly to the requested bit offset and decrements a
    // finite loop count while infinite loops keep 255.
    bits(input, 32, 31, 15); bits(input, 47, 0x1234, 16);
    c[0] = 1 | (1u << 20) | (2u << 12); c[3] = 32; c[4] = 32;
    CHECK_EQ(assemble_packet(m, c, &packet), PacketResult::Frame);
    CHECK(packet.loop_end);
    CHECK_EQ((packet.next[0] >> 12) & 255, 1u);
    CHECK_EQ(packet.next[2] & kCursorMask, 32u);
    c[0] = (c[0] & ~0xFF000u) | 0xFF000u;
    CHECK_EQ(assemble_packet(m, c, &packet), PacketResult::Frame);
    CHECK_EQ((packet.next[0] >> 12) & 255, 255u);

    const float left[] = {-2, -1, 0, 1, 2, std::numeric_limits<float>::quiet_NaN()};
    const float right[] = {1, 0, -1, 0.5f, -0.5f, 0};
    const float* planar[2] = {left, right};
    uint8_t pcm[24]{}; pcm_s16_be(planar, 2, 6, pcm);
    const uint8_t expected[] = {0x80,1,0x7F,0xFF, 0x80,1,0,0, 0,0,0x80,1,
                               0x7F,0xFF,0x3F,0xFF, 0x7F,0xFF,0xC0,1, 0,0,0,0};
    CHECK(std::memcmp(pcm, expected, sizeof(pcm)) == 0);
    return test_result("rt_xma_packets");
}
