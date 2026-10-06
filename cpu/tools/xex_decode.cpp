// rcomp_xex_decode: turns an encrypted and/or compressed XEX2 (a retail
// title) into a *plain* XEX2 (no encryption, no compression) that both
// XenonRecomp and the runtime loader (runtime/xex_loader.h) accept.
//
//   rcomp_xex_decode <in.xex> <out.xex>
//
// Decoding is XenonUtils' own (Xex2DecodeImage, cpu/patches/xenonrecomp/0006):
// the retail key is the constant of the pinned upstream header XenonUtils/xex.h,
// never copied into this repository. The output keeps the input headers
// (imports, entry point, image base, security info) with the file format
// header rewritten to NONE/NONE, followed by the decoded image; thunks and
// import records are left untouched for the runtime loader.
//
// The output is derived from game content: it is written only outside any git
// work tree or under a `build/` directory of one, and must never be committed
// or published.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "xex.h"

namespace {

uint32_t rd32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void wr16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

constexpr size_t kSecurityImageSize = 4;

}  // namespace

// Returns an empty string on success, otherwise the reason.
std::string rcomp_write_plain_xex(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                  unsigned* encryption, unsigned* compression) {
    if (in.size() < 24 || rd32(in.data()) != 0x58455832) return "not an XEX2 file";
    const uint32_t header_size = rd32(in.data() + 8), security_off = rd32(in.data() + 16);
    const uint32_t count = rd32(in.data() + 20);
    if (header_size > in.size() || 24ull + 8ull * count > header_size ||
        uint64_t(security_off) + kSecurityImageSize + 4 > header_size)
        return "truncated or inconsistent XEX2 header";
    const uint8_t* ff = static_cast<const uint8_t*>(getOptHeaderPtr(in.data(), XEX_HEADER_FILE_FORMAT_INFO));
    if (!ff || size_t(ff - in.data()) + 8 > header_size) return "no file format header";
    *encryption = (ff[4] << 8) | ff[5];
    *compression = (ff[6] << 8) | ff[7];
    if (*encryption > XEX_ENCRYPTION_NORMAL || *compression > XEX_COMPRESSION_NORMAL)
        return "unsupported encryption/compression type (delta patches are not handled)";
    const uint32_t image_size = rd32(in.data() + security_off + kSecurityImageSize);
    size_t decoded_size = 0;
    std::unique_ptr<uint8_t[]> image = Xex2DecodeImage(in.data(), in.size(), &decoded_size);
    if (!image) return "decoding failed (corrupt data or block hash mismatch)";
    if (decoded_size != image_size) {
        char b[128];
        snprintf(b, sizeof b, "decoded %zu bytes, security info says %u", decoded_size, image_size);
        return b;
    }
    out.assign(in.begin(), in.begin() + header_size);
    uint8_t* off = out.data() + (ff - in.data());
    wr16(off + 4, XEX_ENCRYPTION_NONE);
    wr16(off + 6, XEX_COMPRESSION_NONE);
    out.insert(out.end(), image.get(), image.get() + decoded_size);
    return "";
}

// True if `path` may receive derived game content: outside every git work
// tree, or inside a `build` directory of one.
bool rcomp_output_path_allowed(const std::filesystem::path& path, std::string* why) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(fs::absolute(path, ec), ec);
    if (ec) {
        *why = "cannot resolve output path: " + ec.message();
        return false;
    }
    fs::path dir = resolved.parent_path();
    bool under_build = false;
    for (fs::path d = dir; !d.empty(); d = d.parent_path()) {
        if (d.filename() == "build") under_build = true;
        if (fs::exists(d / ".git", ec)) {
            if (under_build) return true;
            *why = "output is inside the git work tree " + d.string() + " (use its build/ directory)";
            return false;
        }
        if (d == d.parent_path()) break;
    }
    return true;
}

#ifndef RCOMP_XEX_DECODE_NO_MAIN
// Paths are data: Windows backslashes and quotes must not corrupt stdout JSON.
static void print_json_string(const char* value) {
    putchar('"');
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 0x20) printf("\\u%04x", unsigned(*p));
        else putchar(*p);
    }
    putchar('"');
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <in.xex> <out.xex>\n", argv[0]);
        return 2;
    }
    std::string why;
    if (!rcomp_output_path_allowed(argv[2], &why)) {
        fprintf(stderr, "refusing to write %s: %s\n", argv[2], why.c_str());
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> in;
    uint8_t buf[1 << 16];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) in.insert(in.end(), buf, buf + n);
    fclose(f);
    std::vector<uint8_t> out;
    unsigned enc = 0, comp = 0;
    std::string err = rcomp_write_plain_xex(in, out, &enc, &comp);
    if (!err.empty()) {
        fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
        return 1;
    }
    f = fopen(argv[2], "wb");
    if (!f || fwrite(out.data(), 1, out.size(), f) != out.size() || fclose(f) != 0) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    printf("{\"in\":");
    print_json_string(argv[1]);
    printf(",\"out\":");
    print_json_string(argv[2]);
    printf(",\"encryption\":%u,\"compression\":%u,\"bytes\":%zu}\n", enc, comp, out.size());
    fprintf(stderr, "note: decoded image %s may contain title content; keep it local\n", argv[2]);
    return 0;
}
#endif
