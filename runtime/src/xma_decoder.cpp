// Original R-comp hardware adapter. The separately pinned LGPL FFmpeg codec
// decodes frames; no rexglue/Xenia audio, kernel or system source is included.
#include "xma_decoder.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <cstdarg>
#ifndef RCOMP_XMA_DECODER
#define RCOMP_XMA_DECODER 0
#endif
#ifndef RCOMP_XMA_DECODER_DIAGNOSTICS
#define RCOMP_XMA_DECODER_DIAGNOSTICS 0
#endif
#if RCOMP_XMA_DECODER
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include <libavutil/log.h>
}
#endif
#include "rcomp/guest_write_tracking.h"
#include "rcomp/xenos_gpu.h"

namespace rcomp::rt::xma {
namespace {
#if RCOMP_XMA_DECODER
void decoder_log(void*, int level, const char* format, va_list arguments) {
    // FFmpeg's default callback probes isatty(), unavailable on the PS5 SDK.
    // Keep its configured level filtering and actual messages, using only
    // the title's already supported stderr stream.
    if (level > av_log_get_level()) return;
    // This FFmpeg build prints a thread-emulation warning on every avcodec_open2, and a codec
    // context is opened for each new stream (about 45 a second in GTA IV: 19,189 lines, 59 % of
    // the stderr log, in one 430 s run). The message is benign; it is counted, not repeated.
    {
        va_list probe;
        va_copy(probe, arguments);
        char head[160];
        std::vsnprintf(head, sizeof head, format, probe);
        va_end(probe);
        if (std::strstr(head, "not compiled with thread support")) {
            static std::atomic<uint32_t> suppressed{0};
            const uint32_t count = suppressed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count % 4096 == 0)
                std::fprintf(stderr,
                             "RCOMP-XMA-FFMPEG level=%d note: the thread-support warning was seen %u times, not repeated\n",
                             level, count);
            return;
        }
    }
    std::fprintf(stderr, "RCOMP-XMA-FFMPEG level=%d ", level);
    std::vfprintf(stderr, format, arguments);
}
#endif
uint32_t get(const Context& c, unsigned word, unsigned shift, uint32_t mask) {
    return (c[word] >> shift) & mask;
}
void set(Context& c, unsigned word, unsigned shift, uint32_t mask, uint32_t value) {
    c[word] = (c[word] & ~(mask << shift)) | ((value & mask) << shift);
}
bool valid(const Context& c, unsigned b) { return (c[0] & (1u << (20 + b))) != 0; }
const uint8_t* physical(const GuestMemory& m, uint32_t p, uint32_t size, Protect access) {
    if (uint64_t(p) + size > xenos::kXenosPhysicalSize) return nullptr;
    const uint32_t address = xenos::kXenosPhysicalWindow + p;
    if (!m.is_accessible(address, size, access)) return nullptr;
    return m.translate(address, size);
}
struct Cursor {
    Context state;
    const GuestMemory& memory;
    unsigned buffer;
    uint32_t offset;
    uint32_t packets() const { return state[buffer ? 1 : 0] & 0xFFF; }
    const uint8_t* packet() const {
        if (!valid(state, buffer) || offset / kPacketBits >= packets()) return nullptr;
        const uint64_t address = uint64_t(state[5 + buffer]) + (offset / kPacketBits) * 2048;
        if (address + 2048 > xenos::kXenosPhysicalSize) return nullptr;
        return physical(memory, uint32_t(address),
                        2048, Protect::Read);
    }
    bool next_packet() {
        const uint8_t* p = packet();
        if (!p) return false;
        uint32_t next = offset / kPacketBits + uint32_t(p[3]) + 1;
        // 0xFF denotes a packet containing continuation data only.
        if (p[3] == 0xFF) next = offset / kPacketBits + 1;
        if (next >= packets()) {
            next -= packets();
            state[0] &= ~(1u << (20 + buffer));
            buffer ^= 1;
        }
        offset = next * kPacketBits + 32;
        if (!valid(state, buffer) || next >= packets()) return false;
        return packet() != nullptr;
    }
    bool bit(uint32_t* out) {
        if (offset % kPacketBits == 0) {
            // Keep the previous packet selected while following its stream skip.
            --offset;
            if (!next_packet()) return false;
        }
        const uint8_t* p = packet();
        if (!p) return false;
        const uint32_t relative = offset % kPacketBits;
        *out = (p[relative / 8] >> (7 - (relative & 7))) & 1;
        ++offset;
        return true;
    }
    bool first_frame() {
        // At most all packets in both supplied buffers. Malformed headers do
        // not create an unbounded parser loop.
        for (unsigned n = 0; n < 8190; ++n) {
            const uint8_t* p = packet();
            if (!p) return false;
            const uint32_t first = 32 + ((uint32_t(p[0] & 3) << 13) |
                                        (uint32_t(p[1]) << 5) | (p[2] >> 3));
            if (first < kPacketBits && p[3] != 0xFF) {
                offset = (offset / kPacketBits) * kPacketBits + first;
                return true;
            }
            if (!next_packet()) return false;
        }
        return false;
    }
    void finish() {
        set(state, 4, 31, 1, buffer);
        set(state, 2, 0, kCursorMask, offset);
    }
};
}  // namespace

PacketResult assemble_packet(const GuestMemory& memory, const Context& c, Packet* out) {
    if (!out) return PacketResult::Invalid;
    const Context original = c;
    *out = Packet{};
    const unsigned selected = original[4] >> 31;
    Cursor cursor{original, memory, selected, original[2] & kCursorMask};
    if (!valid(original, selected)) { cursor.buffer ^= 1; cursor.offset = 32; }
    if (!valid(original, cursor.buffer)) return PacketResult::NeedInput;
    if (!cursor.packets() || !cursor.packet()) return PacketResult::Invalid;
    if (cursor.offset < 32 || cursor.offset % kPacketBits < 32) {
        cursor.offset = (cursor.offset / kPacketBits) * kPacketBits + 32;
    }
    if (cursor.offset % kPacketBits == 32 && !cursor.first_frame())
        return PacketResult::NeedInput;
    const uint32_t first_offset = cursor.offset;
    const unsigned frame_buffer = cursor.buffer;
    const uint32_t frame_packet = cursor.offset / kPacketBits;
    uint32_t bits = 0, bit = 0;
    for (unsigned i = 0; i < 15; ++i) {
        if (!cursor.bit(&bit)) return PacketResult::NeedInput;
        bits = (bits << 1) | bit;
        out->bytes[1 + i / 8] |= uint8_t(bit << (7 - (i & 7)));
    }
    if (bits < 16 || bits == 0x7FFF) return PacketResult::Invalid;
    for (uint32_t i = 15; i < bits; ++i) {
        if (!cursor.bit(&bit)) return PacketResult::NeedInput;
        out->bytes[1 + i / 8] |= uint8_t(bit << (7 - (i & 7)));
    }
    out->frame_bits = bits;
    out->size = 1 + (bits + 7) / 8;
    out->bytes[0] = uint8_t(((8 - (bits & 7)) & 7) << 2);
    out->loop_end = get(original, 0, 12, 0xFF) && first_offset == std::max(32u, original[4] & kCursorMask);
    if (out->loop_end) {
        cursor.buffer = frame_buffer;
        cursor.offset = std::max(32u, original[3] & kCursorMask);
        const uint32_t loops = get(original, 0, 12, 0xFF);
        if (loops != 255) set(cursor.state, 0, 12, 0xFF, loops - 1);
    } else if (!bit || cursor.offset % kPacketBits == 0) {
        // A frame continued into another packet may end immediately before
        // that packet's first *new* frame. Its own trailer belongs to the
        // originating packet; retain new frames described by the header.
        bool has_next_in_continuation = false;
        if (cursor.offset % kPacketBits &&
            (cursor.buffer != frame_buffer || cursor.offset / kPacketBits != frame_packet)) {
            const uint8_t* p = cursor.packet();
            if (p && p[3] != 0xFF) {
                const uint32_t first = 32 + ((uint32_t(p[0] & 3) << 13) |
                                            (uint32_t(p[1]) << 5) | (p[2] >> 3));
                if (first >= cursor.offset % kPacketBits && first < kPacketBits) {
                    cursor.offset = (cursor.offset / kPacketBits) * kPacketBits + first;
                    has_next_in_continuation = true;
                }
            }
        }
        if (!has_next_in_continuation) {
        if (cursor.offset % kPacketBits == 0) --cursor.offset;
        if (!cursor.next_packet() || !cursor.first_frame()) {
            // next_packet/first_frame already retire fully traversed buffers.
            // The selected buffer is ready for a future refill.
            cursor.offset = 32;
        }
        }
    }
    cursor.finish();
    out->next = cursor.state;
    return PacketResult::Frame;
}

void pcm_s16_be(const float* const* input, unsigned channels, unsigned samples, uint8_t* output) {
    for (unsigned i = 0; i < samples; ++i) for (unsigned ch = 0; ch < channels; ++ch) {
        float value = input[ch][i];
        // NaN is invalid audio, represented deterministically as zero; finite
        // overshoot from the MDCT is saturated before integer conversion.
        if (!std::isfinite(value)) value = 0;
        value = std::max(-1.0f, std::min(1.0f, value));
        const int16_t sample = int16_t(value * 32767.0f);
        *output++ = uint8_t(uint16_t(sample) >> 8);
        *output++ = uint8_t(sample);
    }
}

Decoder::~Decoder() { reset(); }
bool decoder_available() {
#if RCOMP_XMA_DECODER
    return avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES) != nullptr;
#else
    return false;
#endif
}
void install_decoder_logger() {
#if RCOMP_XMA_DECODER
    av_log_set_callback(&decoder_log);
#endif
}
void Decoder::reset() {
#if RCOMP_XMA_DECODER
    auto* codec = static_cast<AVCodecContext*>(codec_);
    auto* frame = static_cast<AVFrame*>(frame_);
    avcodec_free_context(&codec);
    av_frame_free(&frame);
#endif
    codec_ = frame_ = nullptr;
    rate_ = channels_ = pcm_offset_ = pcm_size_ = 0;
    loop_skip_pending_ = false;
    error_reported_ = false;
}

void Decoder::codec_trace(uint32_t id, unsigned stage, const char* label, bool after, int result, uint32_t size) {
#if RCOMP_XMA_DECODER_DIAGNOSTICS
    const uint32_t bit = 1u << (stage * 2 + unsigned(after));
    if (codec_trace_mask_ & bit) return;
    codec_trace_mask_ |= bit;
    std::fprintf(stderr, "RCOMP-XMA-CODEC context=%u stage=%s %s ret=%d codec=%p frame=%p input_bytes=%u\n",
                 id, label, after ? "after" : "before", result, codec_, frame_, size);
    std::fflush(stderr);
#else
    (void)id; (void)stage; (void)label; (void)after; (void)result; (void)size;
#endif
}

bool Decoder::decode(const Packet& packet, const Context& c, uint32_t context_id) {
#if RCOMP_XMA_DECODER
    constexpr uint32_t rates[4] = {24000, 32000, 44100, 48000};
    const uint32_t rate = rates[get(c, 1, 27, 3)];
    const uint32_t channels = 1 + get(c, 1, 29, 1);
    if (!codec_ || rate_ != rate || channels_ != channels) {
        reset();
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 0, "find_decoder", false, 0, packet.size);
#endif
        const AVCodec* implementation = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 0, "find_decoder", true, implementation ? 0 : AVERROR_DECODER_NOT_FOUND, packet.size);
#endif
        if (!implementation) { decode_result_ = AVERROR_DECODER_NOT_FOUND; return false; }
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 1, "alloc_context", false, 0, packet.size);
#endif
        auto* codec = avcodec_alloc_context3(implementation);
        codec_ = codec;
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 1, "alloc_context", true, codec ? 0 : AVERROR(ENOMEM), packet.size);
#endif
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 2, "alloc_frame", false, 0, packet.size);
#endif
        frame_ = av_frame_alloc();
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 2, "alloc_frame", true, frame_ ? 0 : AVERROR(ENOMEM), packet.size);
#endif
        if (!codec || !frame_) { decode_result_ = AVERROR(ENOMEM); reset(); return false; }
        codec->sample_rate = int(rate);
        codec->channels = int(channels);
        codec->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
        codec->thread_count = 1;
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 3, "open", false, 0, packet.size);
#endif
        decode_result_ = avcodec_open2(codec, implementation, nullptr);
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 3, "open", true, decode_result_, packet.size);
#endif
        if (decode_result_ < 0) { reset(); return false; }
        rate_ = rate; channels_ = channels;
    }
    auto* codec = static_cast<AVCodecContext*>(codec_);
    auto* frame = static_cast<AVFrame*>(frame_);
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 4, "alloc_packet", false, 0, packet.size);
#endif
    AVPacket* compressed = av_packet_alloc();
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 4, "alloc_packet", true, compressed ? 0 : AVERROR(ENOMEM), packet.size);
#endif
    if (!compressed) { decode_result_ = AVERROR(ENOMEM); return false; }
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 5, "new_packet", false, 0, packet.size);
#endif
    decode_result_ = av_new_packet(compressed, int(packet.size));
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 5, "new_packet", true, decode_result_, packet.size);
#endif
    if (decode_result_ < 0) { av_packet_free(&compressed); return false; }
    std::memcpy(compressed->data, packet.bytes.data(), packet.size);
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 6, "send_packet", false, 0, packet.size);
#endif
    const int send = avcodec_send_packet(codec, compressed);
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 6, "send_packet", true, send, packet.size);
#endif
    av_packet_free(&compressed);
    av_frame_unref(frame);
    decode_result_ = send;
    if (send < 0) return false;
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 7, "receive_frame", false, 0, packet.size);
#endif
    decode_result_ = avcodec_receive_frame(codec, frame);
#if RCOMP_XMA_DECODER_DIAGNOSTICS
        codec_trace(context_id, 7, "receive_frame", true, decode_result_, packet.size);
#endif
    if (decode_result_ < 0) return false;
    if (frame->format != AV_SAMPLE_FMT_FLTP || frame->nb_samples != 512 || frame->channels != int(channels)) {
        decode_result_ = AVERROR_INVALIDDATA;
        return false;
    }
    const float* planar[2] = {reinterpret_cast<const float*>(frame->extended_data[0]),
                             channels == 2 ? reinterpret_cast<const float*>(frame->extended_data[1]) : nullptr};
    pcm_s16_be(planar, channels, 512, pcm_.data());
    pcm_offset_ = loop_skip_pending_ ? std::min(4u, get(c, 1, 17, 7)) * 256 * channels : 0;
    pcm_size_ = packet.loop_end ? (get(c, 1, 12, 3) + 1) * 256 * channels : 1024 * channels;
    pcm_offset_ = std::min(pcm_offset_, pcm_size_);
    loop_skip_pending_ = packet.loop_end;
    return true;
#else
    (void)packet; (void)c;
    return false;
#endif
}

bool Decoder::work(GuestMemory& memory, Context& c, uint32_t context_id) {
    if (!(c[1] & 0x80000000u)) return false;
    const uint32_t capacity = get(c, 0, 22, 31) * 256;
    uint32_t write = get(c, 0, 27, 31) * 256;
    const uint32_t read = get(c, 9, 0, 31) * 256;
    if (!capacity || read >= capacity || write >= capacity) return false;
    auto* output = const_cast<uint8_t*>(physical(memory, c[7], capacity, Protect::ReadWrite));
    if (!output) return false;
    if (!(c[0] & kInputValid) && pcm_offset_ == pcm_size_ && read == write) {
        c[1] &= ~0x80000000u;
        return true;  // All real output has been drained by the guest.
    }
    uint32_t free = write >= read ? capacity - write + read : read - write;
    // Reserve a block to distinguish empty from full when the cursors coincide.
    if (free <= 256) return false;
    free -= 256;
    const uint32_t padding = get(c, 1, 24, 7) * 256;
    bool did_work = false;
    // Capacity is bounded to 31 blocks; also bound zero-output loop warmups.
    for (unsigned n = 0; n < 32; ++n) {
        if (pcm_offset_ == pcm_size_) {
            if (free <= padding || free - padding < 256) break;
            Packet packet;
            const PacketResult parsed = assemble_packet(memory, c, &packet);
            if (parsed != PacketResult::Frame) {
                if (parsed == PacketResult::Invalid && !error_reported_)
                    std::fprintf(stderr, "RCOMP-XMA FAIL context=%u malformed packet at bit=%u; input retained\n", context_id, c[2] & kCursorMask);
                if (parsed == PacketResult::Invalid) error_reported_ = true;
                break;
            }
            if (!decode(packet, c, context_id)) {
                const bool reported = error_reported_;
                reset();
                if (!reported)
                    std::fprintf(stderr, "RCOMP-XMA FAIL context=%u frame decode ret=%d at bit=%u; input retained\n", context_id, decode_result_, c[2] & kCursorMask);
                error_reported_ = true;
                break;
            }
            error_reported_ = false;
            if (!first_frame_reported_) {
                first_frame_reported_ = true;
                std::fprintf(stdout, "RCOMP-XMA-DECODE context=%u samples=512 channels=%u rate=%u\n",
                             context_id, channels_, rate_);
            }
            // Compressed bytes become consumed only after genuine decode has
            // produced a durable PCM block held by this context's decoder.
            c = packet.next;
            did_work = true;
        }
        const uint32_t count = std::min(free, pcm_size_ - pcm_offset_) & ~255u;
        if (!count) break;
        const uint32_t first = std::min(count, capacity - write);
        std::memcpy(output + write, pcm_.data() + pcm_offset_, first);
        std::memcpy(output, pcm_.data() + pcm_offset_ + first, count - first);
        // The decoded PCM is guest memory a GPU cache may hold (a vertex range that spans the buffer): record it like a guest write.
        note_guest_write_range(uint64_t(xenos::kXenosPhysicalWindow) + c[7] + write, first);
        if (count > first) note_guest_write_range(uint64_t(xenos::kXenosPhysicalWindow) + c[7], count - first);
        pcm_offset_ += count; write = (write + count) % capacity; free -= count;
        did_work = true;
        if (pcm_offset_ == pcm_size_) { if (free <= padding) break; free -= padding; }
    }
    set(c, 0, 27, 31, write / 256);
    return did_work;
}
}  // namespace rcomp::rt::xma
