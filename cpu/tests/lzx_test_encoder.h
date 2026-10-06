// Minimal LZX *encoder* for tests only: produces streams in the format that
// libmspack's lzxd (non-delta, as XenonUtils' lzxDecompress configures it)
// decodes, so that the XEX "normal" (LZX) compression path can be exercised
// without any retail file or proprietary compressor.
//
// Format (libmspack lzxd.c): bits are read MSB-first from 16-bit little-endian
// words; stream header = 1 bit (Intel E8 translation, always 0 here); each
// block = 3-bit type + 24-bit length; output frames of 32 KiB after which the
// bitstream is realigned to 16 bits. Huffman trees are canonical, complete,
// and their code lengths are sent as deltas (mod 17) through a 20-symbol
// pretree; lengths persist from block to block.
//
// Covered: VERBATIM blocks (literals, matches with verbatim offset bits, long
// matches through the LENGTH tree) and UNCOMPRESSED blocks (odd length pad).
// Not produced: ALIGNED blocks, repeated-offset slots R0-R2, pretree run
// codes 17-19, matches across frames, Intel E8 translation.
#pragma once

#include <stdint.h>

#include <algorithm>
#include <functional>
#include <queue>
#include <vector>

namespace lzxtest {

constexpr uint32_t kFrame = 32768;
constexpr int kNumChars = 256, kNumSecondaryLengths = 249, kPretree = 20;

inline int position_slots(uint32_t window_size) {
    static const int slots[11] = {30, 32, 34, 36, 38, 42, 50, 66, 98, 162, 290};
    int bits = 0;
    while ((1u << bits) < window_size) ++bits;
    return slots[bits - 15];
}
inline int extra_bits(int slot) {
    if (slot < 4) return 0;
    if (slot < 36) return slot / 2 - 1;
    return 17;
}
inline uint32_t position_base(int slot) {
    uint32_t base = 0;
    for (int i = 0; i < slot; ++i) base += 1u << extra_bits(i);
    return base;
}

class BitWriter {
public:
    std::vector<uint8_t> out;
    void put(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            word_ = uint16_t((word_ << 1) | ((value >> i) & 1));
            if (++n_ == 16) flush_word();
        }
    }
    int pending_bits() const { return n_; }
    void align16() {
        if (n_) put(0, 16 - n_);
    }
    void raw(const uint8_t* p, size_t n) {
        out.insert(out.end(), p, p + n);
    }

private:
    void flush_word() {
        out.push_back(uint8_t(word_));
        out.push_back(uint8_t(word_ >> 8));
        word_ = 0;
        n_ = 0;
    }
    uint16_t word_ = 0;
    int n_ = 0;
};

// Code lengths (<= max_len) from symbol frequencies; complete code guaranteed
// (a single used symbol gets a partner). Zero frequency -> length 0.
inline std::vector<int> huffman_lengths(std::vector<uint32_t> freq, int max_len) {
    const size_t n = freq.size();
    int used = 0;
    for (auto f : freq) used += f != 0;
    if (used == 0) return std::vector<int>(n, 0);
    if (used == 1) {
        for (size_t i = 0; i < n; ++i)
            if (!freq[i]) {
                freq[i] = 1;
                break;
            }
    }
    for (;;) {
        struct Node {
            uint64_t w;
            int left, right;
        };
        std::vector<Node> nodes;
        using Item = std::pair<uint64_t, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
        for (size_t i = 0; i < n; ++i)
            if (freq[i]) {
                nodes.push_back({freq[i], -1, int(i)});
                pq.push({freq[i], int(nodes.size() - 1)});
            }
        while (pq.size() > 1) {
            auto a = pq.top();
            pq.pop();
            auto b = pq.top();
            pq.pop();
            nodes.push_back({a.first + b.first, a.second, b.second});
            pq.push({a.first + b.first, int(nodes.size() - 1)});
        }
        std::vector<int> len(n, 0);
        std::function<void(int, int)> walk = [&](int idx, int depth) {
            const Node& nd = nodes[idx];
            if (nd.left < 0) {
                len[nd.right] = depth;
                return;
            }
            walk(nd.left, depth + 1);
            walk(nd.right, depth + 1);
        };
        walk(pq.top().second, 0);
        if (*std::max_element(len.begin(), len.end()) <= max_len) return len;
        for (auto& f : freq)
            if (f) f = (f >> 1) | 1;  // flatten and retry
    }
}

// Canonical codes (by length, then symbol), as libmspack's make_decode_table.
inline std::vector<uint32_t> canonical_codes(const std::vector<int>& len) {
    std::vector<uint32_t> code(len.size(), 0);
    uint32_t next = 0;
    for (int l = 1; l <= 16; ++l) {
        for (size_t s = 0; s < len.size(); ++s)
            if (len[s] == l) code[s] = next++;
        next <<= 1;
    }
    return code;
}

struct Token {
    bool match;
    uint8_t literal;
    uint32_t length, offset;
};

class Encoder {
public:
    explicit Encoder(uint32_t window_size)
        : num_main_(kNumChars + position_slots(window_size) * 8),
          prev_main_(num_main_, 0),
          prev_length_(kNumSecondaryLengths, 0) {
        bw_.put(0, 1);  // no Intel E8 translation
    }

    // One frame (<= 32 KiB, all but the last exactly 32 KiB).
    void frame(const uint8_t* data, uint32_t size, bool uncompressed) {
        if (uncompressed) {
            uncompressed_block(data, size);
        } else {
            verbatim_block(tokenize(data, size), size);
        }
        bw_.align16();  // the decoder realigns at every frame end
    }
    std::vector<uint8_t> finish() {
        bw_.align16();
        return bw_.out;
    }

private:
    static std::vector<Token> tokenize(const uint8_t* d, uint32_t n) {
        std::vector<Token> t;
        std::vector<int32_t> head(1 << 15, -1), prev(n, -1);
        auto hash = [&](uint32_t i) { return ((d[i] << 10) ^ (d[i + 1] << 5) ^ d[i + 2]) & 0x7FFF; };
        uint32_t i = 0;
        while (i < n) {
            uint32_t best_len = 0, best_off = 0;
            if (i + 3 <= n) {
                int chain = 64;
                for (int32_t c = head[hash(i)]; c >= 0 && chain--; c = prev[c]) {
                    uint32_t l = 0, maxl = std::min<uint32_t>(257, n - i);
                    while (l < maxl && d[c + l] == d[i + l]) ++l;
                    if (l > best_len) {
                        best_len = l;
                        best_off = i - uint32_t(c);
                    }
                }
            }
            uint32_t step = best_len >= 3 ? best_len : 1;
            if (best_len >= 3) t.push_back({true, 0, best_len, best_off});
            else t.push_back({false, d[i], 0, 0});
            for (uint32_t k = 0; k < step; ++k, ++i)
                if (i + 3 <= n) {
                    uint32_t h = hash(i);
                    prev[i] = head[h];
                    head[h] = int32_t(i);
                }
        }
        return t;
    }

    static int slot_for(uint32_t formatted) {
        int s = 3;
        while (position_base(s + 1) <= formatted) ++s;
        return s;
    }

    void send_lengths(const std::vector<int>& lens, std::vector<int>& prev, size_t first, size_t last) {
        std::vector<int> syms;
        std::vector<uint32_t> pf(kPretree, 0);
        for (size_t x = first; x < last; ++x) {
            int z = (prev[x] - lens[x] + 17) % 17;
            syms.push_back(z);
            ++pf[z];
        }
        auto plen = huffman_lengths(pf, 15);
        auto pcode = canonical_codes(plen);
        for (int x = 0; x < kPretree; ++x) bw_.put(plen[x], 4);
        for (int z : syms) bw_.put(pcode[z], plen[z]);
        for (size_t x = first; x < last; ++x) prev[x] = lens[x];
    }

    void verbatim_block(const std::vector<Token>& toks, uint32_t size) {
        std::vector<uint32_t> mf(num_main_, 0), lf(kNumSecondaryLengths, 0);
        struct Enc {
            int main;
            int length_sym;  // -1 if none
            int slot;
            uint32_t verbatim;
        };
        std::vector<Enc> enc;
        for (const Token& t : toks) {
            if (!t.match) {
                enc.push_back({t.literal, -1, 0, 0});
                ++mf[t.literal];
                continue;
            }
            const uint32_t formatted = t.offset + 2;
            const int slot = slot_for(formatted);
            const uint32_t lh = std::min<uint32_t>(t.length - 2, 7);
            const int main = kNumChars + ((slot << 3) | int(lh));
            const int ls = lh == 7 ? int(t.length - 2 - 7) : -1;
            enc.push_back({main, ls, slot, formatted - position_base(slot)});
            ++mf[main];
            if (ls >= 0) ++lf[ls];
        }
        auto mlen = huffman_lengths(mf, 16);
        auto llen = huffman_lengths(lf, 16);
        auto mcode = canonical_codes(mlen), lcode = canonical_codes(llen);
        bw_.put(1, 3);  // VERBATIM
        bw_.put(size >> 8, 16);
        bw_.put(size & 0xFF, 8);
        send_lengths(mlen, prev_main_, 0, kNumChars);
        send_lengths(mlen, prev_main_, kNumChars, num_main_);
        send_lengths(llen, prev_length_, 0, kNumSecondaryLengths);
        for (const Enc& e : enc) {
            bw_.put(mcode[e.main], mlen[e.main]);
            if (e.main < kNumChars) continue;
            if (e.length_sym >= 0) bw_.put(lcode[e.length_sym], llen[e.length_sym]);
            if (int eb = extra_bits(e.slot)) bw_.put(e.verbatim, eb);
        }
    }

    void uncompressed_block(const uint8_t* data, uint32_t size) {
        bw_.put(3, 3);  // UNCOMPRESSED
        bw_.put(size >> 8, 16);
        bw_.put(size & 0xFF, 8);
        // The decoder drops the rest of the current word, or a whole word if
        // it is exactly aligned.
        if (bw_.pending_bits() == 0) bw_.put(0, 16);
        else bw_.align16();
        const uint8_t r[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};  // R0, R1, R2
        bw_.raw(r, sizeof r);
        bw_.raw(data, size);
        if (size & 1) {
            const uint8_t pad = 0;
            bw_.raw(&pad, 1);
        }
    }

    BitWriter bw_;
    int num_main_;
    std::vector<int> prev_main_, prev_length_;
};

// Whole buffer; frames whose index is in `uncompressed_frames` use
// UNCOMPRESSED blocks, the others VERBATIM.
inline std::vector<uint8_t> compress(const std::vector<uint8_t>& in, uint32_t window_size,
                                     const std::vector<int>& uncompressed_frames = {}) {
    Encoder e(window_size);
    int idx = 0;
    for (size_t o = 0; o < in.size(); o += kFrame, ++idx) {
        const uint32_t n = uint32_t(std::min<size_t>(kFrame, in.size() - o));
        const bool raw = std::find(uncompressed_frames.begin(), uncompressed_frames.end(), idx) !=
                         uncompressed_frames.end();
        e.frame(in.data() + o, n, raw);
    }
    return e.finish();
}

}  // namespace lzxtest
