// Xbox/NT string helpers implemented locally for the R-comp AOT runtime.
// Behavioral contract and pinned primary references: runtime/docs/STRINGS.md.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "hle_more.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/strings.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kStatusBufferOverflow = 0x80000005u;
constexpr uint32_t kErrorMrMidNotFound = 317u;

#include "hle_xboxkrnl_strings_tables.inc"

struct StringDescriptor {
    uint16_t length = 0;
    uint16_t maximum_length = 0;
    uint32_t buffer = 0;
};

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s called without runtime", fn);
    return *r;
}

[[noreturn]] void guest_fault(const char* fn, const char* what, uint64_t address, uint64_t length) {
    rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s %s address=0x%llX length=0x%llX", fn, what,
                (unsigned long long)address, (unsigned long long)length);
}

uint8_t* checked(Runtime& r, const char* fn, const char* what, uint64_t address, uint64_t size,
                 Protect rights) {
    if (!size) return nullptr;
    if (!address || address >= 0x100000000ull || size > 0x100000000ull - address ||
        !r.mem->is_accessible(address, size, rights))
        guest_fault(fn, what, address, size);
    return r.mem->host(address);
}

bool overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    if (!as || !bs) return false;
    return a < b + bs && b < a + as;
}

uint16_t load_be16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
uint32_t load_be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
void store_be16(uint8_t* p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}
void store_be32(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

StringDescriptor load_descriptor(Runtime& r, const char* fn, const char* what, uint32_t address,
                                 Protect rights) {
    const uint8_t* p = checked(r, fn, what, address, 8, rights);
    return {load_be16(p), load_be16(p + 2), load_be32(p + 4)};
}

void store_descriptor(uint8_t* p, const StringDescriptor& value) {
    store_be16(p, value.length);
    store_be16(p + 2, value.maximum_length);
    store_be32(p + 4, value.buffer);
}

uint16_t cp1252_to_unicode(uint8_t value) {
    if (value < 0x80 || value >= 0xA0) return value;
    return kCp1252C1ToUnicode[value - 0x80];
}

uint8_t unicode_to_cp1252(uint16_t value) {
    size_t lo = 0, hi = sizeof(kCp1252BestFit) / sizeof(kCp1252BestFit[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (kCp1252BestFit[mid].unicode < value) lo = mid + 1;
        else hi = mid;
    }
    return lo < sizeof(kCp1252BestFit) / sizeof(kCp1252BestFit[0]) &&
                   kCp1252BestFit[lo].unicode == value
               ? kCp1252BestFit[lo].byte
               : (uint8_t)'?';
}

uint16_t nt_upcase(uint16_t value) {
    size_t lo = 0, hi = sizeof(kNtUpcasePairs) / sizeof(kNtUpcasePairs[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (kNtUpcasePairs[mid].source < value) lo = mid + 1;
        else hi = mid;
    }
    return lo < sizeof(kNtUpcasePairs) / sizeof(kNtUpcasePairs[0]) &&
                   kNtUpcasePairs[lo].source == value
               ? kNtUpcasePairs[lo].upper
               : value;
}

uint32_t ntstatus_to_dos(uint32_t status) {
    size_t lo = 0, hi = sizeof(kNtStatusDosPairs) / sizeof(kNtStatusDosPairs[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (kNtStatusDosPairs[mid].status < status) lo = mid + 1;
        else hi = mid;
    }
    return lo < sizeof(kNtStatusDosPairs) / sizeof(kNtStatusDosPairs[0]) &&
                   kNtStatusDosPairs[lo].status == status
               ? kNtStatusDosPairs[lo].dos
               : kErrorMrMidNotFound;
}

class Sha1 {
  public:
    void update(const uint8_t* data, size_t size) {
        total_bytes_ += size;
        if (buffer_size_) {
            const size_t take = std::min<size_t>(64 - buffer_size_, size);
            std::memcpy(buffer_.data() + buffer_size_, data, take);
            buffer_size_ += take;
            data += take;
            size -= take;
            if (buffer_size_ == 64) {
                compress(buffer_.data());
                buffer_size_ = 0;
            }
        }
        while (size >= 64) {
            compress(data);
            data += 64;
            size -= 64;
        }
        if (size) {
            std::memcpy(buffer_.data(), data, size);
            buffer_size_ = size;
        }
    }

    std::array<uint8_t, 20> finish() {
        const uint64_t bit_length = total_bytes_ * 8u;
        buffer_[buffer_size_++] = 0x80;
        if (buffer_size_ > 56) {
            std::memset(buffer_.data() + buffer_size_, 0, 64 - buffer_size_);
            compress(buffer_.data());
            buffer_size_ = 0;
        }
        std::memset(buffer_.data() + buffer_size_, 0, 56 - buffer_size_);
        for (unsigned i = 0; i < 8; ++i)
            buffer_[56 + i] = (uint8_t)(bit_length >> (56 - 8 * i));
        compress(buffer_.data());

        std::array<uint8_t, 20> digest{};
        for (unsigned i = 0; i < 5; ++i) {
            digest[i * 4 + 0] = (uint8_t)(state_[i] >> 24);
            digest[i * 4 + 1] = (uint8_t)(state_[i] >> 16);
            digest[i * 4 + 2] = (uint8_t)(state_[i] >> 8);
            digest[i * 4 + 3] = (uint8_t)state_[i];
        }
        return digest;
    }

    // The guest's XECRYPT_SHA_STATE (88 bytes): be32 count (bytes hashed so far), be32 state[5], u8 buffer[64] (the first count % 64 bytes are pending).
    static constexpr uint32_t kGuestStateBytes = 88;
    void init_guest_state(uint8_t* guest) const {
        std::memset(guest, 0, kGuestStateBytes);
        for (unsigned i = 0; i < 5; ++i) store_be32(guest + 4 + 4 * i, state_[i]);
    }
    void load(const uint8_t* guest) {
        total_bytes_ = load_be32(guest);
        for (unsigned i = 0; i < 5; ++i) state_[i] = load_be32(guest + 4 + 4 * i);
        buffer_size_ = (size_t)(total_bytes_ & 63u);
        std::memcpy(buffer_.data(), guest + 24, 64);
    }
    void store(uint8_t* guest) const {
        store_be32(guest, (uint32_t)total_bytes_);
        for (unsigned i = 0; i < 5; ++i) store_be32(guest + 4 + 4 * i, state_[i]);
        std::memcpy(guest + 24, buffer_.data(), 64);
    }

  private:
    static uint32_t rol(uint32_t value, unsigned shift) {
        return (value << shift) | (value >> (32 - shift));
    }

    void compress(const uint8_t* block) {
        uint32_t w[80];
        for (unsigned i = 0; i < 16; ++i)
            w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
                   (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
        for (unsigned i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
        for (unsigned i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }

    uint32_t state_[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t total_bytes_ = 0;
    std::array<uint8_t, 64> buffer_{};
    size_t buffer_size_ = 0;
};

void write_count(Runtime& r, const char* fn, uint32_t address, uint32_t value) {
    if (!address) return;
    uint8_t* p = checked(r, fn, "byte-count output", address, 4, Protect::ReadWrite);
    store_be32(p, value);
}

void RtlMultiByteToUnicodeN(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "RtlMultiByteToUnicodeN";
    Runtime& r = current(fn);
    const uint32_t dst = c.r3.u32, dst_bytes = c.r4.u32, written = c.r5.u32;
    const uint32_t src = c.r6.u32, src_bytes = c.r7.u32;
    const uint32_t chars = std::min<uint32_t>(src_bytes, dst_bytes / 2);
    const uint32_t read_bytes = chars;
    const uint32_t write_bytes = chars * 2;

    uint8_t* out = checked(r, fn, "Unicode destination", dst, write_bytes, Protect::ReadWrite);
    const uint8_t* in = checked(r, fn, "multibyte source", src, read_bytes, Protect::Read);
    if (written) checked(r, fn, "byte-count output", written, 4, Protect::ReadWrite);
    if (overlaps(dst, write_bytes, src, read_bytes))
        guest_fault(fn, "source/destination overlap is forbidden by the NT contract", dst, write_bytes);
    if (written && (overlaps(written, 4, dst, write_bytes) || overlaps(written, 4, src, read_bytes)))
        guest_fault(fn, "byte-count output overlaps conversion buffers", written, 4);

    for (uint32_t i = 0; i < chars; ++i) store_be16(out + i * 2, cp1252_to_unicode(in[i]));
    write_count(r, fn, written, write_bytes);
    c.r3.u64 = nt::kSuccess;
}

void RtlUnicodeToMultiByteN(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "RtlUnicodeToMultiByteN";
    Runtime& r = current(fn);
    const uint32_t dst = c.r3.u32, dst_bytes = c.r4.u32, written = c.r5.u32;
    const uint32_t src = c.r6.u32, src_bytes = c.r7.u32;
    const uint32_t chars = std::min<uint32_t>(src_bytes / 2, dst_bytes);
    const uint32_t read_bytes = chars * 2;

    uint8_t* out = checked(r, fn, "multibyte destination", dst, chars, Protect::ReadWrite);
    const uint8_t* in = checked(r, fn, "Unicode source", src, read_bytes, Protect::Read);
    if (written) checked(r, fn, "byte-count output", written, 4, Protect::ReadWrite);
    if (overlaps(dst, chars, src, read_bytes))
        guest_fault(fn, "source/destination overlap is forbidden by the NT contract", dst, chars);
    if (written && (overlaps(written, 4, dst, chars) || overlaps(written, 4, src, read_bytes)))
        guest_fault(fn, "byte-count output overlaps conversion buffers", written, 4);

    for (uint32_t i = 0; i < chars; ++i) out[i] = unicode_to_cp1252(load_be16(in + i * 2));
    write_count(r, fn, written, chars);
    c.r3.u64 = nt::kSuccess;
}

void RtlUnicodeStringToAnsiString(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "RtlUnicodeStringToAnsiString";
    Runtime& r = current(fn);
    const uint32_t dst_address = c.r3.u32, src_address = c.r4.u32;
    const bool allocate = (c.r5.u32 & 0xFFu) != 0;
    uint8_t* dst_descriptor = checked(r, fn, "ANSI_STRING destination", dst_address, 8,
                                      Protect::ReadWrite);
    const StringDescriptor dst = {load_be16(dst_descriptor), load_be16(dst_descriptor + 2),
                                  load_be32(dst_descriptor + 4)};
    const StringDescriptor src = load_descriptor(r, fn, "UNICODE_STRING source", src_address,
                                                 Protect::Read);
    const uint32_t chars = src.length / 2;
    const uint8_t* source = checked(r, fn, "Unicode source buffer", src.buffer, chars * 2u,
                                    Protect::Read);

    std::vector<uint8_t> converted;
#if defined(__cpp_exceptions)
    try {
#endif
        converted.resize(chars);
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        c.r3.u64 = nt::kNoMemory;
        return;
    }
#endif
    for (uint32_t i = 0; i < chars; ++i)
        converted[i] = unicode_to_cp1252(load_be16(source + i * 2));

    if (allocate) {
        const uint32_t bytes = chars + 1;
        uint32_t address = 0;
        {
            std::lock_guard<std::mutex> lock(r.ansi_string_mutex);
#if defined(__cpp_exceptions)
            try {
#endif
                r.ansi_string_allocations.push_front({0, bytes});
#if defined(__cpp_exceptions)
            } catch (const std::bad_alloc&) {
                c.r3.u64 = nt::kNoMemory;
                return;
            }
#endif
            Status result = r.heap.alloc(bytes, GuestHeap::kMinAlign, false, &address);
            if (result != Status::Ok) {
                r.ansi_string_allocations.pop_front();
                c.r3.u64 = to_ntstatus(result);
                return;
            }
            r.ansi_string_allocations.front().address = address;
        }
        uint8_t* out = checked(r, fn, "allocated ANSI buffer", address, bytes, Protect::ReadWrite);
        if (chars) std::memcpy(out, converted.data(), chars);
        out[chars] = 0;
        store_descriptor(dst_descriptor,
                         {(uint16_t)chars, (uint16_t)bytes, address});
        c.r3.u64 = nt::kSuccess;
        return;
    }

    const uint32_t payload = dst.maximum_length ?
        std::min<uint32_t>(chars, (uint32_t)dst.maximum_length - 1) : 0;
    const uint32_t bytes_to_touch = dst.maximum_length ? payload + 1 : 0;
    uint8_t* out = checked(r, fn, "caller ANSI buffer", dst.buffer, bytes_to_touch,
                           Protect::ReadWrite);
    if (bytes_to_touch && overlaps(dst_address, 8, dst.buffer, bytes_to_touch))
        guest_fault(fn, "destination descriptor overlaps destination buffer", dst.buffer,
                    bytes_to_touch);
    if (payload) std::memcpy(out, converted.data(), payload);
    if (bytes_to_touch) out[payload] = 0;
    store_descriptor(dst_descriptor, {(uint16_t)payload, dst.maximum_length, dst.buffer});
    c.r3.u64 = dst.maximum_length > chars ? nt::kSuccess : kStatusBufferOverflow;
}

void RtlFreeAnsiString(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "RtlFreeAnsiString";
    Runtime& r = current(fn);
    const uint32_t descriptor_address = c.r3.u32;
    uint8_t* descriptor = checked(r, fn, "ANSI_STRING descriptor", descriptor_address, 8,
                                  Protect::ReadWrite);
    const StringDescriptor value = {load_be16(descriptor), load_be16(descriptor + 2),
                                    load_be32(descriptor + 4)};
    if (!value.buffer) {
        store_descriptor(descriptor, {});
        return;
    }

    Status result = Status::NotAllocated;
    bool owned = false;
    uint32_t allocation_size = 0;
    {
        std::lock_guard<std::mutex> lock(r.ansi_string_mutex);
        auto it = std::find_if(r.ansi_string_allocations.begin(), r.ansi_string_allocations.end(),
                               [value](const Runtime::AnsiStringAllocation& a) {
                                   return a.address == value.buffer;
                               });
        if (it != r.ansi_string_allocations.end()) {
            owned = true;
            allocation_size = it->requested_size;
            if (overlaps(descriptor_address, 8, value.buffer, allocation_size)) {
                // Leave both allocation and descriptor intact; fatal outside the lock below.
                result = Status::Conflict;
            } else {
                result = r.heap.free(value.buffer);
                if (result == Status::Ok) r.ansi_string_allocations.erase(it);
            }
        }
    }
    if (!owned) guest_fault(fn, "buffer is not owned by RtlUnicodeStringToAnsiString", value.buffer, 0);
    if (result == Status::Conflict)
        guest_fault(fn, "descriptor overlaps owned ANSI allocation", descriptor_address, 8);
    if (result != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s failed to release owned buffer 0x%08X: %s", fn,
                    value.buffer, status_name(result));
    store_descriptor(descriptor, {});
}

void RtlUpcaseUnicodeChar(PPCContext& c, uint8_t*) {
    c.r3.u64 = nt_upcase((uint16_t)c.r3.u32);
}

void RtlNtStatusToDosError(PPCContext& c, uint8_t*) { c.r3.u64 = ntstatus_to_dos(c.r3.u32); }

void XeCryptSha(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "XeCryptSha";
    Runtime& r = current(fn);
    const uint32_t addresses[3] = {c.r3.u32, c.r5.u32, c.r7.u32};
    const uint32_t sizes[3] = {c.r4.u32, c.r6.u32, c.r8.u32};
    const uint32_t output = c.r9.u32;
    const uint32_t output_size = c.r10.u32;

    const uint8_t* inputs[3] = {};
    for (unsigned i = 0; i < 3; ++i)
        inputs[i] = checked(r, fn, "SHA-1 input", addresses[i], sizes[i], Protect::Read);
    const uint32_t digest_bytes = std::min<uint32_t>(20, output_size);
    uint8_t* out = checked(r, fn, "SHA-1 output", output, digest_bytes, Protect::ReadWrite);

    Sha1 sha;
    for (unsigned i = 0; i < 3; ++i)
        if (sizes[i]) sha.update(inputs[i], sizes[i]);
    const auto digest = sha.finish();
    if (digest_bytes) std::memcpy(out, digest.data(), digest_bytes);
}

// XeCryptShaInit(state), XeCryptShaUpdate(state, input, size), XeCryptShaFinal(state, output, output_size): the streaming form, the state lives in guest memory.
void XeCryptShaInit(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "XeCryptShaInit";
    Runtime& r = current(fn);
    uint8_t* state = checked(r, fn, "SHA-1 state", c.r3.u32, Sha1::kGuestStateBytes, Protect::ReadWrite);
    Sha1().init_guest_state(state);
}

void XeCryptShaUpdate(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "XeCryptShaUpdate";
    Runtime& r = current(fn);
    const uint32_t size = c.r5.u32;
    uint8_t* state = checked(r, fn, "SHA-1 state", c.r3.u32, Sha1::kGuestStateBytes, Protect::ReadWrite);
    const uint8_t* input = checked(r, fn, "SHA-1 input", c.r4.u32, size, Protect::Read);
    Sha1 sha;
    sha.load(state);
    if (size) sha.update(input, size);
    sha.store(state);
#if RCOMP_RUNTIME_DIAGNOSTICS
    {   // bring-up tracing: which guest blocks a title hashes (integrity checks of its data files)
        static std::atomic<uint32_t> lines{0};
        if (lines.fetch_add(1) < 256)
            std::fprintf(stderr, "RCOMP-SHA update state=0x%08X input=0x%08X size=0x%X first=%02X%02X%02X%02X lr=0x%08X\n",
                         c.r3.u32, c.r4.u32, size, size > 0 ? input[0] : 0, size > 1 ? input[1] : 0,
                         size > 2 ? input[2] : 0, size > 3 ? input[3] : 0, uint32_t(c.lr));
    }
#endif
}

void XeCryptShaFinal(PPCContext& c, uint8_t*) {
    constexpr const char* fn = "XeCryptShaFinal";
    Runtime& r = current(fn);
    const uint32_t digest_bytes = std::min<uint32_t>(20, c.r5.u32);
    uint8_t* state = checked(r, fn, "SHA-1 state", c.r3.u32, Sha1::kGuestStateBytes, Protect::ReadWrite);
    uint8_t* out = checked(r, fn, "SHA-1 output", c.r4.u32, digest_bytes, Protect::ReadWrite);
    Sha1 sha;
    sha.load(state);
    const auto digest = sha.finish();
    if (digest_bytes) std::memcpy(out, digest.data(), digest_bytes);
#if RCOMP_RUNTIME_DIAGNOSTICS
    {
        static std::atomic<uint32_t> lines{0};
        if (lines.fetch_add(1) < 128) {
            char hex[41] = {};
            for (unsigned i = 0; i < 20; ++i) std::snprintf(hex + 2 * i, 3, "%02X", digest[i]);
            std::fprintf(stderr, "RCOMP-SHA final state=0x%08X digest=%s lr=0x%08X\n", c.r3.u32, hex, uint32_t(c.lr));
        }
    }
#endif
}

struct Impl { uint32_t ordinal; const char* name; PPCFunc* fn; };
constexpr Impl kImpls[] = {
    {0x0127, "RtlFreeAnsiString", &RtlFreeAnsiString},
    {0x0133, "RtlMultiByteToUnicodeN", &RtlMultiByteToUnicodeN},
    {0x0135, "RtlNtStatusToDosError", &RtlNtStatusToDosError},
    {0x0142, "RtlUnicodeStringToAnsiString", &RtlUnicodeStringToAnsiString},
    {0x0143, "RtlUnicodeToMultiByteN", &RtlUnicodeToMultiByteN},
    {0x0149, "RtlUpcaseUnicodeChar", &RtlUpcaseUnicodeChar},
    {0x018F, "XeCryptShaInit", &XeCryptShaInit},
    {0x0190, "XeCryptShaUpdate", &XeCryptShaUpdate},
    {0x0191, "XeCryptShaFinal", &XeCryptShaFinal},
    {0x0192, "XeCryptSha", &XeCryptSha},
};

}  // namespace

uint8_t ansi_from_unicode(uint16_t value) { return unicode_to_cp1252(value); }

uint16_t unicode_from_ansi(uint8_t value) { return cp1252_to_unicode(value); }

Status register_xboxkrnl_strings_hle() {
    for (const Impl& i : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, i.name, &ordinal) || ordinal != i.ordinal)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "string/RTL ordinal mismatch: %s", i.name);
        Status result = register_import(kModuleXboxkrnl, i.ordinal, i.fn, i.name);
        if (result != Status::Ok) return result;
    }
    // AES, random, PKCS#1 and RSA services: src/hle_xboxkrnl_crypto_more.cpp.
    return register_xboxkrnl_crypto_more_hle();
}

}  // namespace rcomp::rt
