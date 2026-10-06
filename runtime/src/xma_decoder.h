// R-comp XMA hardware packet adapter. Protocol fields are represented by
// explicit masks, independently of any emulator's C++ bitfield layout.
#pragma once
#include <array>
#include <cstdint>
#include "rcomp/guest_memory.h"

namespace rcomp::rt::xma {
using Context = std::array<uint32_t, 16>;
constexpr uint32_t kInputValid = 0x00300000u;
constexpr uint32_t kCursorMask = 0x03FFFFFFu;
constexpr uint32_t kPacketBits = 2048 * 8;
enum class PacketResult { Frame, NeedInput, Invalid };
struct Packet {
    // One codec framing byte, at most 32766 frame bits, FFmpeg padding.
    std::array<uint8_t, 1 + 4096 + 64> bytes{};
    uint32_t size = 0;
    uint32_t frame_bits = 0;
    Context next{};
    bool loop_end = false;
};
PacketResult assemble_packet(const GuestMemory&, const Context&, Packet*);
void pcm_s16_be(const float* const*, unsigned channels, unsigned samples, uint8_t*);
bool decoder_available();
// Install a real stderr logger before decoder creation, without terminal APIs.
void install_decoder_logger();

class Decoder {
public:
    Decoder() = default;
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    void reset();
    // Called with the device mutex held. Returns true only for real PCM or
    // successful compressed-frame decoding, never for fabricated consumption.
    bool work(GuestMemory&, Context&, uint32_t context_id = 0xFFFFFFFFu);
private:
    bool decode(const Packet&, const Context&, uint32_t context_id);
    void codec_trace(uint32_t, unsigned, const char*, bool, int, uint32_t);
    void* codec_ = nullptr;
    void* frame_ = nullptr;
    uint32_t rate_ = 0, channels_ = 0;
    std::array<uint8_t, 2048> pcm_{};
    uint32_t pcm_offset_ = 0, pcm_size_ = 0;
    bool loop_skip_pending_ = false;
    bool error_reported_ = false;
    bool first_frame_reported_ = false;
    int decode_result_ = 0;
    uint32_t codec_trace_mask_ = 0;
};
}  // namespace rcomp::rt::xma
