// cpu/tools/xex_decode.cpp against synthetic encrypted/compressed variants of
// the plain synthetic XEX (fixtures/xex via cpu/tools/mkxex.py). No game
// content. The variants are made here with the retail key constant of the
// pinned upstream header XenonUtils/xex.h (not copied into the repository).
//
//   test_xex_decode <plain.xex> [<variant-out.xex>]
// Prints one line per case (PASS/FAIL/NOT TESTED); exit 0 iff no FAIL.
// With a second argument, also writes the encrypted + LZX-compressed
// variant there (end-to-end check in tools/cpu_pipeline.sh).
#define RCOMP_XEX_DECODE_NO_MAIN
#include "../tools/xex_decode.cpp"

#include <aes.hpp>
#include <TinySHA1.hpp>

#include <random>

#include "lzx_test_encoder.h"
#include "xex_patcher.h"

namespace {

int g_fail = 0;
void verdict(const char* id, bool ok, const std::string& why = "") {
    printf("%-44s %s%s%s\n", id, ok ? "PASS" : "FAIL", why.empty() ? "" : " ", why.c_str());
    g_fail += !ok;
}

void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off] = uint8_t(x >> 24);
    v[off + 1] = uint8_t(x >> 16);
    v[off + 2] = uint8_t(x >> 8);
    v[off + 3] = uint8_t(x);
}

struct Plain {
    std::vector<uint8_t> xex, image;
    uint32_t header_size, ff_off, security_off;
};

// Rewrites the file format header at a free spot of the header area (the
// mkxex header is padded to 4 KiB) and repoints the optional header to it.
size_t move_file_format(std::vector<uint8_t>& x, uint32_t header_size, size_t info_size) {
    const size_t at = header_size - 0x200;
    const uint32_t count = rd32(x.data() + 20);
    for (uint32_t i = 0; i < count; ++i)
        if (rd32(x.data() + 24 + 8 * i) == XEX_HEADER_FILE_FORMAT_INFO) put32(x, 28 + 8 * i, uint32_t(at));
    for (size_t i = 0; i < info_size; ++i) x[at + i] = 0;
    put32(x, at, uint32_t(info_size));
    return at;
}

// BASIC compression: the image as runs of (data, zeros).
std::vector<uint8_t> make_basic(const Plain& p) {
    std::vector<uint8_t> x(p.xex.begin(), p.xex.begin() + p.header_size);
    std::vector<std::pair<uint32_t, uint32_t>> blocks;
    std::vector<uint8_t> payload;
    const size_t page = 0x1000;
    for (size_t o = 0; o < p.image.size(); o += page) {
        size_t n = std::min(page, p.image.size() - o), data = n;
        while (data > 0 && p.image[o + data - 1] == 0) --data;
        blocks.push_back({uint32_t(data), uint32_t(n - data)});
        payload.insert(payload.end(), p.image.begin() + o, p.image.begin() + o + data);
    }
    const size_t info = 8 + 8 * blocks.size();
    size_t at = move_file_format(x, p.header_size, info);
    x[at + 7] = XEX_COMPRESSION_BASIC;
    for (size_t i = 0; i < blocks.size(); ++i) {
        put32(x, at + 8 + 8 * i, blocks[i].first);
        put32(x, at + 12 + 8 * i, blocks[i].second);
    }
    x.insert(x.end(), payload.begin(), payload.end());
    return x;
}

// NORMAL compression: LZX stream (lzx_test_encoder.h) cut into 2-byte-sized
// chunks, grouped in blocks; each block starts with the size and SHA-1 of the
// next block, the first block's are in the file format header.
std::vector<uint8_t> make_normal(const Plain& p, uint32_t window, const std::vector<int>& raw_frames,
                                 size_t chunk = 0x1800, size_t chunks_per_block = 2) {
    const std::vector<uint8_t> lzx = lzxtest::compress(p.image, window, raw_frames);
    std::vector<std::vector<uint8_t>> bodies;  // block contents after the 24-byte header
    for (size_t o = 0; o < lzx.size();) {
        std::vector<uint8_t> b;
        for (size_t c = 0; c < chunks_per_block && o < lzx.size(); ++c) {
            const size_t n = std::min(chunk, lzx.size() - o);
            b.push_back(uint8_t(n >> 8));
            b.push_back(uint8_t(n));
            b.insert(b.end(), lzx.begin() + o, lzx.begin() + o + n);
            o += n;
        }
        b.push_back(0);
        b.push_back(0);
        bodies.push_back(b);
    }
    std::vector<std::vector<uint8_t>> blocks(bodies.size());
    uint32_t next_size = 0;
    uint8_t next_hash[20] = {};
    for (size_t i = bodies.size(); i-- > 0;) {
        std::vector<uint8_t> b(24, 0);
        put32(b, 0, next_size);
        memcpy(b.data() + 4, next_hash, 20);
        b.insert(b.end(), bodies[i].begin(), bodies[i].end());
        sha1::SHA1 h;
        h.processBytes(b.data(), b.size());
        h.finalize(next_hash);
        next_size = uint32_t(b.size());
        blocks[i] = b;
    }
    std::vector<uint8_t> x(p.xex.begin(), p.xex.begin() + p.header_size);
    size_t at = move_file_format(x, p.header_size, 8 + 4 + 24);
    x[at + 7] = XEX_COMPRESSION_NORMAL;
    put32(x, at + 8, window);
    put32(x, at + 12, next_size);
    memcpy(x.data() + at + 16, next_hash, 20);
    for (auto& b : blocks) x.insert(x.end(), b.begin(), b.end());
    return x;
}

// libmspack's LZX decoder (as configured by XenonUtils) against the test
// encoder on synthetic buffers: multi-frame, both block types, two windows.
bool lzx_unit_roundtrips(std::string* why) {
    std::mt19937 rng(1);
    for (uint32_t win : {0x8000u, 0x20000u})
        for (size_t size : {size_t(100), size_t(32768), size_t(32769), size_t(100001), size_t(0x30000)}) {
            std::vector<uint8_t> in(size);
            for (size_t i = 0; i < size; ++i)
                in[i] = (i % 5000 < 2500) ? uint8_t(rng()) : uint8_t("rcomp text "[i % 11]);
            for (int mode = 0; mode < 3; ++mode) {
                std::vector<int> raw;
                if (mode == 1) raw = {0, 2};
                if (mode == 2)
                    for (int k = 0; k < 16; ++k) raw.push_back(k);
                auto c = lzxtest::compress(in, win, raw);
                std::vector<uint8_t> out(size, 0xCC);
                int rc = lzxDecompress(c.data(), c.size(), out.data(), out.size(), win, nullptr, 0);
                if (rc != 0 || out != in) {
                    char b[128];
                    snprintf(b, sizeof b, "window %#x size %zu mode %d: rc=%d", win, size, mode, rc);
                    *why = b;
                    return false;
                }
                if (mode == 0 && size >= 32768 && c.size() * 10 > size * 7) {
                    *why = "verbatim blocks did not compress: matches not exercised";
                    return false;
                }
            }
        }
    return true;
}

const uint8_t kSessionKey[16] = {'r', 'c', 'o', 'm', 'p', '-', 't', 'e', 's', 't', '-', 'k', 'e', 'y', '0', '1'};

// NORMAL encryption: payload AES-128-CBC (IV 0) with a session key stored in
// the security info encrypted by the retail key (single block, IV 0).
std::vector<uint8_t> encrypt(std::vector<uint8_t> x, uint32_t header_size, uint32_t security_off) {
    const uint32_t count = rd32(x.data() + 20);
    for (uint32_t i = 0; i < count; ++i)
        if (rd32(x.data() + 24 + 8 * i) == XEX_HEADER_FILE_FORMAT_INFO) x[rd32(x.data() + 28 + 8 * i) + 5] = 1;
    x.resize(header_size + ((x.size() - header_size + 15) & ~size_t(15)), 0);
    uint8_t wrapped[16];
    memcpy(wrapped, kSessionKey, 16);
    AES_ctx ctx;
    AES_init_ctx_iv(&ctx, Xex2RetailKey, AESBlankIV);
    AES_CBC_encrypt_buffer(&ctx, wrapped, 16);
    memcpy(x.data() + security_off + offsetof(Xex2SecurityInfo, aesKey), wrapped, 16);
    AES_init_ctx_iv(&ctx, kSessionKey, AESBlankIV);
    AES_CBC_encrypt_buffer(&ctx, x.data() + header_size, x.size() - header_size);
    return x;
}

void check_roundtrip(const char* id, const Plain& p, const std::vector<uint8_t>& variant, unsigned want_enc,
                     unsigned want_comp) {
    std::vector<uint8_t> out;
    unsigned enc = 99, comp = 99;
    std::string err = rcomp_write_plain_xex(variant, out, &enc, &comp);
    if (!err.empty()) return verdict(id, false, err);
    bool ok = enc == want_enc && comp == want_comp && out.size() == p.header_size + p.image.size() &&
              memcmp(out.data() + p.header_size, p.image.data(), p.image.size()) == 0;
    // The output is itself plain: decoding it again is the identity.
    std::vector<uint8_t> again;
    unsigned e2, c2;
    ok = ok && rcomp_write_plain_xex(out, again, &e2, &c2).empty() && e2 == 0 && c2 == 0 && again == out;
    verdict(id, ok, ok ? "" : "decoded image differs from the plain image");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s <plain.xex> [<variant-out.xex>]\n", argv[0]);
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 2;
    Plain p;
    uint8_t buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) p.xex.insert(p.xex.end(), buf, buf + n);
    fclose(f);
    p.header_size = rd32(p.xex.data() + 8);
    p.security_off = rd32(p.xex.data() + 16);
    const uint32_t image_size = rd32(p.xex.data() + p.security_off + 4);
    p.image.assign(p.xex.begin() + p.header_size, p.xex.begin() + p.header_size + image_size);

    check_roundtrip("xex_decode/plain", p, p.xex, 0, 0);
    check_roundtrip("xex_decode/basic_compression", p, make_basic(p), 0, 1);
    check_roundtrip("xex_decode/encrypted", p, encrypt(p.xex, p.header_size, p.security_off), 1, 0);
    check_roundtrip("xex_decode/encrypted_basic", p, encrypt(make_basic(p), p.header_size, p.security_off), 1, 1);
    {
        std::string why;
        verdict("xex_decode/lzx_unit_roundtrips", lzx_unit_roundtrips(&why), why);
    }
    check_roundtrip("xex_decode/normal_lzx_verbatim", p, make_normal(p, 0x8000, {}), 0, 2);
    check_roundtrip("xex_decode/normal_lzx_uncompressed_block", p, make_normal(p, 0x20000, {0}), 0, 2);
    check_roundtrip("xex_decode/normal_lzx_one_chunk_per_block", p, make_normal(p, 0x8000, {}, 0x400, 1), 0, 2);
    check_roundtrip("xex_decode/encrypted_normal_lzx", p,
                    encrypt(make_normal(p, 0x8000, {}), p.header_size, p.security_off), 1, 2);

    // Negative controls.
    {
        auto x = encrypt(p.xex, p.header_size, p.security_off);
        x[p.security_off + offsetof(Xex2SecurityInfo, aesKey)] ^= 1;  // wrong session key
        std::vector<uint8_t> out;
        unsigned e, c;
        bool decoded = rcomp_write_plain_xex(x, out, &e, &c).empty();
        bool differs = !decoded || memcmp(out.data() + p.header_size, p.image.data(), p.image.size()) != 0;
        verdict("xex_decode/negative_wrong_key", differs);
    }
    {
        // A modified byte in the last block breaks the SHA-1 chain (the
        // stream is cut into 64-byte chunks so that there are many blocks).
        auto x = make_normal(p, 0x8000, {}, 0x40, 1);
        const size_t data = x.size() - p.header_size;
        bool many = data > 4 * (24 + 2 + 0x40 + 2);
        x[x.size() - 8] ^= 0x01;
        std::vector<uint8_t> out;
        unsigned e, c;
        verdict("xex_decode/negative_normal_block_hash", many && !rcomp_write_plain_xex(x, out, &e, &c).empty(),
                many ? "" : "too few blocks");
    }
    {
        auto x = make_basic(p);
        const uint32_t count = rd32(x.data() + 20);
        for (uint32_t i = 0; i < count; ++i)
            if (rd32(x.data() + 24 + 8 * i) == XEX_HEADER_FILE_FORMAT_INFO) x[rd32(x.data() + 28 + 8 * i) + 7] = 3;
        std::vector<uint8_t> out;
        unsigned e, c;
        verdict("xex_decode/negative_delta_compression_refused", !rcomp_write_plain_xex(x, out, &e, &c).empty());
    }
    {
        std::vector<uint8_t> out;
        unsigned e, c;
        std::vector<uint8_t> x(p.xex.begin(), p.xex.begin() + 100);
        verdict("xex_decode/negative_truncated", !rcomp_write_plain_xex(x, out, &e, &c).empty());
    }
    {
        std::string why;
        bool refused = !rcomp_output_path_allowed(std::filesystem::path(RCOMP_REPO_ROOT) / "fixtures" / "x.xex", &why);
        bool allowed = rcomp_output_path_allowed(std::filesystem::path(RCOMP_REPO_ROOT) / "build" / "x.xex", &why);
        verdict("xex_decode/output_path_guard", refused && allowed);
    }
    if (argc == 3) {
        auto v = encrypt(make_normal(p, 0x8000, {}), p.header_size, p.security_off);
        FILE* o = fopen(argv[2], "wb");
        if (!o || fwrite(v.data(), 1, v.size(), o) != v.size() || fclose(o) != 0) return 2;
    }
    printf("%-44s NOT TESTED %s\n", "xex_decode/lzx_features_not_produced",
           "ALIGNED blocks, repeated offsets, cross-frame matches, Intel E8 (test encoder limits)");
    return g_fail ? 1 : 0;
}
