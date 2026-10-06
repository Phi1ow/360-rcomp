// XeCrypt AES-128, random bytes, PKCS#1 v1.5 blocks and the RSA public-key operation (owner: Agent 3,
// runtime/). Contract, layouts and references: runtime/docs/CRYPTO.md.
//
// ABI (Halo 3 4D5307E6 call sites, recompiled with the pinned XenonRecomp; argument registers r3..r8):
//   XeCryptAesKey(XECRYPT_AES_STATE* state, const BYTE key[16])                      0x821A7E94
//   XeCryptAesCbc(state, const BYTE* in, DWORD cb, BYTE* out, BYTE feed[16], BOOL enc) 0x821A8050, in == out
//   XeCryptRandom(BYTE* buffer, DWORD cb)                                              0x822FEC90
//   XeCryptBnQw_SwapDwQwLeBe(const QWORD* in, QWORD* out, DWORD cqw)                   0x8219F244, in == out
//   XeCryptBnQwNeRsaPubCrypt(const QWORD* a, QWORD* b, const XECRYPT_RSA* key) -> BOOL 0x8219F254, a == b
//   XeCryptBnDwLePkcs1Format(const BYTE hash[20], DWORD type, BYTE* sig, DWORD cb)     0x822FEFEC (type 0)
//   XeCryptBnDwLePkcs1Verify(const BYTE hash[20], const BYTE* sig, DWORD cb) -> BOOL   0x8219F27C (cb 256)
// Structure layouts cross-checked with rexglue-sdk c94f5eb src/kernel/xboxkrnl/xboxkrnl_crypt.cpp and
// Xenia 95a5c3e xboxkrnl_crypt.cc (BSD-3, read only, no code copied).
#include <stdio.h>

#include <array>
#include <cstring>
#include <vector>

#include "hle_more.h"
#include "host_random.h"
#include "physical_window.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"

namespace rcomp::rt {
namespace {

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r || !r->mem) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xboxkrnl.exe!%s called without runtime", fn);
    return *r;
}

// Host view of a guest range; an inaccessible range is a guest-access fatal (the console would fault).
uint8_t* span(Runtime& r, const char* fn, const char* what, uint32_t address, uint64_t size, Protect rights) {
    if (!size) return nullptr;
    if (!address || size > 0x100000000ull - address || !r.mem->is_accessible(address, size, rights))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s %s address=0x%08X length=0x%llX", fn, what, address,
                    (unsigned long long)size);
    return r.mem->host(address);
}

uint64_t load_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
void store_be64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i, v >>= 8) p[i] = uint8_t(v);
}
uint32_t load_be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// ---- AES-128 (FIPS-197) ---------------------------------------------------------------------------------
// XECRYPT_AES_STATE (0x160 bytes): +0x000 keytabenc[11][16], +0x0B0 keytabdec[11][16]. keytabenc[r] is round
// key r as the FIPS-197 byte sequence w[4r]..w[4r+3] (big-endian words, which is also the Xenon word
// layout). keytabdec holds the equivalent inverse cipher schedule (FIPS-197 5.3.5): dec[0] = enc[10],
// dec[r] = InvMixColumns(enc[10 - r]) for r = 1..9, dec[10] = enc[0]. Decryption uses keytabdec, so a
// state a title builds or copies itself is honoured as the kernel's would be.
constexpr uint32_t kAesStateBytes = 0x160, kAesDecOffset = 0xB0, kAesRounds = 10;

uint8_t xtime(uint8_t a) { return uint8_t((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }
uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (; b; b >>= 1, a = xtime(a))
        if (b & 1) p ^= a;
    return p;
}

struct SBoxes {
    uint8_t s[256], inv[256];
    SBoxes() {
        for (int x = 0; x < 256; ++x) {
            uint8_t inverse = 0;
            if (x)
                for (int y = 1; y < 256; ++y)
                    if (gmul(uint8_t(x), uint8_t(y)) == 1) {
                        inverse = uint8_t(y);
                        break;
                    }
            uint8_t v = inverse;
            for (int k = 1; k <= 4; ++k) v ^= uint8_t((inverse << k) | (inverse >> (8 - k)));
            s[x] = uint8_t(v ^ 0x63);
        }
        for (int x = 0; x < 256; ++x) inv[s[x]] = uint8_t(x);
    }
};
const SBoxes& sboxes() {
    static const SBoxes boxes;
    return boxes;
}

using Block = std::array<uint8_t, 16>;  // FIPS-197 state, byte index = 4 * column + row

void add_round_key(Block& s, const uint8_t* key) {
    for (int i = 0; i < 16; ++i) s[i] ^= key[i];
}
void shift_rows(Block& s, bool inverse) {
    const Block t = s;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) s[4 * c + r] = t[4 * ((inverse ? c + 4 - r : c + r) % 4) + r];
}
void mix_column(uint8_t* a, bool inverse) {
    const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
    if (!inverse) {
        a[0] = uint8_t(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
        a[1] = uint8_t(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
        a[2] = uint8_t(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
        a[3] = uint8_t(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
    } else {
        a[0] = uint8_t(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
        a[1] = uint8_t(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
        a[2] = uint8_t(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
        a[3] = uint8_t(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
    }
}
void mix_columns(Block& s, bool inverse) {
    for (int c = 0; c < 4; ++c) mix_column(&s[4 * c], inverse);
}
void sub_bytes(Block& s, bool inverse) {
    const SBoxes& b = sboxes();
    for (auto& v : s) v = inverse ? b.inv[v] : b.s[v];
}

void aes_key_schedule(const uint8_t key[16], uint8_t state[kAesStateBytes]) {
    const SBoxes& b = sboxes();
    uint8_t* enc = state;
    std::memcpy(enc, key, 16);
    uint8_t rcon = 1;
    for (uint32_t i = 4; i < 4 * (kAesRounds + 1); ++i) {
        uint8_t t[4];
        std::memcpy(t, enc + 4 * (i - 1), 4);
        if (i % 4 == 0) {
            const uint8_t first = t[0];
            t[0] = uint8_t(b.s[t[1]] ^ rcon);
            t[1] = b.s[t[2]];
            t[2] = b.s[t[3]];
            t[3] = b.s[first];
            rcon = xtime(rcon);
        }
        for (int k = 0; k < 4; ++k) enc[4 * i + k] = uint8_t(enc[4 * (i - 4) + k] ^ t[k]);
    }
    uint8_t* dec = state + kAesDecOffset;
    std::memcpy(dec, enc + 16 * kAesRounds, 16);
    for (uint32_t r = 1; r < kAesRounds; ++r) {
        std::memcpy(dec + 16 * r, enc + 16 * (kAesRounds - r), 16);
        for (int c = 0; c < 4; ++c) mix_column(dec + 16 * r + 4 * c, true);
    }
    std::memcpy(dec + 16 * kAesRounds, enc, 16);
}

void aes_encrypt(const uint8_t* state, Block& s) {
    add_round_key(s, state);
    for (uint32_t round = 1; round <= kAesRounds; ++round) {
        sub_bytes(s, false);
        shift_rows(s, false);
        if (round != kAesRounds) mix_columns(s, false);
        add_round_key(s, state + 16 * round);
    }
}

void aes_decrypt(const uint8_t* state, Block& s) {
    const uint8_t* dec = state + kAesDecOffset;
    add_round_key(s, dec);
    for (uint32_t round = 1; round <= kAesRounds; ++round) {
        sub_bytes(s, true);
        shift_rows(s, true);
        if (round != kAesRounds) mix_columns(s, true);
        add_round_key(s, dec + 16 * round);
    }
}

// XeCryptAesKey (0x0159): expands the 128-bit key into the state.
void XeCryptAesKey(PPCContext& ctx, uint8_t*) {
    const char* fn = "XeCryptAesKey";
    Runtime& r = current(fn);
    const uint32_t state_address = ctx.r3.u32, key_address = ctx.r4.u32;
    uint8_t key[16];
    std::memcpy(key, span(r, fn, "key", key_address, 16, Protect::Read), 16);
    uint8_t* state = span(r, fn, "state", state_address, kAesStateBytes, Protect::ReadWrite);
    uint8_t expanded[kAesStateBytes];
    aes_key_schedule(key, expanded);
    std::memcpy(state, expanded, sizeof(expanded));
    note_title_write(state_address, kAesStateBytes);
}

// XeCryptAesCbc (0x015B): CBC over cb bytes (whole blocks); feed is the IV on entry and the last
// ciphertext block on return, so consecutive calls chain. Input and output may be the same buffer.
void XeCryptAesCbc(PPCContext& ctx, uint8_t*) {
    const char* fn = "XeCryptAesCbc";
    Runtime& r = current(fn);
    const uint32_t state_address = ctx.r3.u32, in = ctx.r4.u32, size = ctx.r5.u32, out = ctx.r6.u32,
                   feed_address = ctx.r7.u32;
    const bool encrypt = ctx.r8.u32 != 0;
    if (size % 16)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xboxkrnl.exe!%s length 0x%X is not a whole number of AES blocks (no partial-block contract) "
                    "lr=0x%08X",
                    fn, size, uint32_t(ctx.lr));
    uint8_t state[kAesStateBytes];
    std::memcpy(state, span(r, fn, "state", state_address, kAesStateBytes, Protect::Read), kAesStateBytes);
    uint8_t* feed = span(r, fn, "feed", feed_address, 16, Protect::ReadWrite);
    const uint8_t* input = span(r, fn, "input", in, size, Protect::Read);
    uint8_t* output = span(r, fn, "output", out, size, Protect::ReadWrite);
    Block chain;
    std::memcpy(chain.data(), feed, 16);
    for (uint32_t offset = 0; offset < size; offset += 16) {
        Block block;
        std::memcpy(block.data(), input + offset, 16);  // read before the (possibly aliased) write
        if (encrypt) {
            for (int i = 0; i < 16; ++i) block[i] ^= chain[i];
            aes_encrypt(state, block);
            chain = block;
            std::memcpy(output + offset, block.data(), 16);
        } else {
            const Block cipher = block;
            aes_decrypt(state, block);
            for (int i = 0; i < 16; ++i) block[i] ^= chain[i];
            chain = cipher;
            std::memcpy(output + offset, block.data(), 16);
        }
    }
    std::memcpy(feed, chain.data(), 16);
    if (size) note_title_write(out, size);
    note_title_write(feed_address, 16);
}

// ---- random -----------------------------------------------------------------------------------------------
// XeCryptRandom (0x018A): cb bytes from the host random source (src/host_random.h).
void XeCryptRandom(PPCContext& ctx, uint8_t*) {
    const char* fn = "XeCryptRandom";
    Runtime& r = current(fn);
    const uint32_t buffer = ctx.r3.u32, size = ctx.r4.u32;
    if (!size) return;
    uint8_t* out = span(r, fn, "buffer", buffer, size, Protect::ReadWrite);
    if (!host_random_bytes(out, size))
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "xboxkrnl.exe!%s: no host random source (RDRAND/getentropy failed)", fn);
    note_title_write(buffer, size);
}

// ---- bignums ----------------------------------------------------------------------------------------------
// XeCryptBnQw_SwapDwQwLeBe (0x0170): byte-reverses each of the cqw qwords (qword order kept). Converts a
// little-endian byte string ("DwLe") into the native qword bignum ("QwNe", least significant qword first,
// big-endian qwords) and back. In and out may be the same buffer.
void XeCryptBnQw_SwapDwQwLeBe(PPCContext& ctx, uint8_t*) {
    const char* fn = "XeCryptBnQw_SwapDwQwLeBe";
    Runtime& r = current(fn);
    const uint32_t in = ctx.r3.u32, out = ctx.r4.u32, count = ctx.r5.u32;
    const uint64_t bytes = uint64_t(count) * 8;
    if (!bytes) return;
    const uint8_t* input = span(r, fn, "input", in, bytes, Protect::Read);
    std::vector<uint8_t> copy(input, input + bytes);
    uint8_t* output = span(r, fn, "output", out, bytes, Protect::ReadWrite);
    for (uint64_t q = 0; q < count; ++q)
        for (int i = 0; i < 8; ++i) output[8 * q + i] = copy[8 * q + 7 - i];
    note_title_write(out, bytes);
}

// RSA public-key operation b = a^e mod n with Montgomery multiplication (64-bit limbs, least
// significant first). XECRYPT_RSA: +0 DWORD cqw (modulus size in qwords), +4 DWORD public exponent,
// +8 QWORD reserved, +0x10 QWORD modulus[cqw] (QwNe), then private data the public operation ignores.
constexpr uint32_t kRsaMaxQwords = 64;  // 4096-bit, XeCrypt's largest key (XECRYPT_RSAPUB_4096)

struct Montgomery {
    uint32_t s = 0;
    uint64_t n[kRsaMaxQwords] = {};
    uint64_t ninv = 0;  // -n^-1 mod 2^64

    void mul(const uint64_t* a, const uint64_t* b, uint64_t* out) const {
        using u128 = unsigned __int128;
        uint64_t t[kRsaMaxQwords + 2] = {};
        for (uint32_t i = 0; i < s; ++i) {
            uint64_t carry = 0;
            for (uint32_t j = 0; j < s; ++j) {
                const u128 p = u128(a[i]) * b[j] + t[j] + carry;
                t[j] = uint64_t(p);
                carry = uint64_t(p >> 64);
            }
            u128 p = u128(t[s]) + carry;
            t[s] = uint64_t(p);
            t[s + 1] = uint64_t(p >> 64);
            const uint64_t mu = t[0] * ninv;
            p = u128(mu) * n[0] + t[0];
            carry = uint64_t(p >> 64);
            for (uint32_t j = 1; j < s; ++j) {
                p = u128(mu) * n[j] + t[j] + carry;
                t[j - 1] = uint64_t(p);
                carry = uint64_t(p >> 64);
            }
            p = u128(t[s]) + carry;
            t[s - 1] = uint64_t(p);
            t[s] = t[s + 1] + uint64_t(p >> 64);
        }
        if (t[s] || !less(t, n, s)) subtract(t, n, s);
        std::memcpy(out, t, 8 * s);
    }
    static bool less(const uint64_t* a, const uint64_t* b, uint32_t s) {
        for (uint32_t i = s; i-- > 0;)
            if (a[i] != b[i]) return a[i] < b[i];
        return false;
    }
    static void subtract(uint64_t* a, const uint64_t* b, uint32_t s) {
        uint64_t borrow = 0;
        for (uint32_t i = 0; i < s; ++i) {
            const uint64_t bi = b[i] + borrow;
            const uint64_t next = (bi < borrow) || (a[i] < bi) ? 1 : 0;
            a[i] -= bi;
            borrow = next;
        }
    }
};

// XeCryptBnQwNeRsaPubCrypt (0x016D): TRUE with b = a^e mod n; FALSE when a is not below the modulus
// (outside the RSA primitive's domain, PKCS#1 RSAEP/RSAVP1 "out of range"). a and b may alias.
void XeCryptBnQwNeRsaPubCrypt(PPCContext& ctx, uint8_t*) {
    const char* fn = "XeCryptBnQwNeRsaPubCrypt";
    Runtime& r = current(fn);
    const uint32_t a_address = ctx.r3.u32, b_address = ctx.r4.u32, key = ctx.r5.u32;
    const uint8_t* header = span(r, fn, "key header", key, 16, Protect::Read);
    const uint32_t count = load_be32(header), exponent = load_be32(header + 4);
    if (count == 0 || count > kRsaMaxQwords)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xboxkrnl.exe!%s key size %u qwords not established lr=0x%08X", fn,
                    count, uint32_t(ctx.lr));
    Montgomery m;
    m.s = count;
    const uint8_t* modulus = span(r, fn, "modulus", key + 16, uint64_t(count) * 8, Protect::Read);
    for (uint32_t i = 0; i < count; ++i) m.n[i] = load_be64(modulus + 8 * i);
    if (!(m.n[0] & 1) || (count == 1 && m.n[0] == 1))
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xboxkrnl.exe!%s even or unit modulus (not an RSA key; no contract) lr=0x%08X", fn,
                    uint32_t(ctx.lr));
    uint64_t a[kRsaMaxQwords] = {};
    const uint8_t* input = span(r, fn, "input", a_address, uint64_t(count) * 8, Protect::Read);
    for (uint32_t i = 0; i < count; ++i) a[i] = load_be64(input + 8 * i);
    uint8_t* output = span(r, fn, "output", b_address, uint64_t(count) * 8, Protect::ReadWrite);
    if (!Montgomery::less(a, m.n, count)) {
        ctx.r3.u64 = 0;
        return;
    }
    uint64_t inverse = m.n[0];  // Newton: correct to 3 bits, doubled each step
    for (int i = 0; i < 6; ++i) inverse *= 2 - m.n[0] * inverse;
    m.ninv = 0 - inverse;
    // R^2 mod n, R = 2^(64 * count), by doubling 1 (always below n) 128 * count times.
    uint64_t r2[kRsaMaxQwords] = {1};
    for (uint32_t step = 0; step < 128 * count; ++step) {
        uint64_t carry = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t next = r2[i] >> 63;
            r2[i] = (r2[i] << 1) | carry;
            carry = next;
        }
        if (carry || !Montgomery::less(r2, m.n, count)) Montgomery::subtract(r2, m.n, count);
    }
    uint64_t base_m[kRsaMaxQwords], x[kRsaMaxQwords], one[kRsaMaxQwords] = {1};
    m.mul(a, r2, base_m);  // a * R mod n
    m.mul(one, r2, x);     // R mod n: Montgomery 1
    for (int bit = 31; bit >= 0; --bit) {
        m.mul(x, x, x);
        if ((exponent >> bit) & 1) m.mul(x, base_m, x);
    }
    m.mul(x, one, x);
    for (uint32_t i = 0; i < count; ++i) store_be64(output + 8 * i, x[i]);
    note_title_write(b_address, uint64_t(count) * 8);
    ctx.r3.u64 = 1;
}

// ---- PKCS#1 v1.5 signature blocks over a SHA-1 hash ---------------------------------------------------
// The block is EM = 00 01 FF..FF 00 || T || H (RFC 8017 EMSA-PKCS1-v1_5), stored "DwLe": byte i of the
// guest buffer is byte (cb - 1 - i) of EM, i.e. the little-endian byte string of the integer.
// T by type: 0 = none (bare hash), 1 = SHA-1 DigestInfo without the NULL parameters,
// 2 = SHA-1 DigestInfo with NULL parameters. Sizes 39..512 bytes (XeCrypt's 4096-bit maximum).
constexpr uint32_t kHashBytes = 20, kPkcs1MinBytes = 39, kPkcs1MaxBytes = 512, kPkcs1Types = 3;
constexpr uint8_t kDigestInfoNoNull[] = {0x30, 0x1F, 0x30, 0x07, 0x06, 0x05, 0x2B, 0x0E, 0x03, 0x02, 0x1A, 0x04, 0x14};
constexpr uint8_t kDigestInfoNull[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E,
                                       0x03, 0x02, 0x1A, 0x05, 0x00, 0x04, 0x14};

void pkcs1_block(const uint8_t* hash, uint32_t type, uint32_t size, uint8_t* le_out) {
    const uint8_t* info = type == 1 ? kDigestInfoNoNull : type == 2 ? kDigestInfoNull : nullptr;
    const uint32_t info_bytes = type == 1 ? sizeof(kDigestInfoNoNull) : type == 2 ? sizeof(kDigestInfoNull) : 0;
    uint8_t em[kPkcs1MaxBytes];
    std::memset(em, 0xFF, size);
    em[0] = 0x00;
    em[1] = 0x01;
    const uint32_t tail = info_bytes + kHashBytes;
    em[size - tail - 1] = 0x00;
    if (info_bytes) std::memcpy(em + size - tail, info, info_bytes);
    std::memcpy(em + size - kHashBytes, hash, kHashBytes);
    for (uint32_t i = 0; i < size; ++i) le_out[i] = em[size - 1 - i];
}

// XeCryptBnDwLePkcs1Format (0x0162).
void XeCryptBnDwLePkcs1Format(PPCContext& ctx, uint8_t*) {
    const char* fn = "XeCryptBnDwLePkcs1Format";
    Runtime& r = current(fn);
    const uint32_t hash_address = ctx.r3.u32, type = ctx.r4.u32, sig = ctx.r5.u32, size = ctx.r6.u32;
    if (type >= kPkcs1Types || size < kPkcs1MinBytes || size > kPkcs1MaxBytes)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xboxkrnl.exe!%s type=%u size=%u not established lr=0x%08X", fn,
                    type, size, uint32_t(ctx.lr));
    uint8_t hash[kHashBytes];
    std::memcpy(hash, span(r, fn, "hash", hash_address, kHashBytes, Protect::Read), kHashBytes);
    uint8_t block[kPkcs1MaxBytes];
    pkcs1_block(hash, type, size, block);
    std::memcpy(span(r, fn, "signature", sig, size, Protect::ReadWrite), block, size);
    note_title_write(sig, size);
}

// XeCryptBnDwLePkcs1Verify (0x0163): TRUE when the block is a PKCS#1 v1.5 encoding of the hash in any
// of the three forms; FALSE otherwise, and for a size no encoding can have.
void XeCryptBnDwLePkcs1Verify(PPCContext& ctx, uint8_t*) {
    const char* fn = "XeCryptBnDwLePkcs1Verify";
    Runtime& r = current(fn);
    const uint32_t hash_address = ctx.r3.u32, sig = ctx.r4.u32, size = ctx.r5.u32;
    if (size < kPkcs1MinBytes || size > kPkcs1MaxBytes) {
        ctx.r3.u64 = 0;
        return;
    }
    uint8_t hash[kHashBytes];
    std::memcpy(hash, span(r, fn, "hash", hash_address, kHashBytes, Protect::Read), kHashBytes);
    const uint8_t* block = span(r, fn, "signature", sig, size, Protect::Read);
    for (uint32_t type = 0; type < kPkcs1Types; ++type) {
        uint8_t expected[kPkcs1MaxBytes];
        pkcs1_block(hash, type, size, expected);
        if (std::memcmp(expected, block, size) == 0) {
            ctx.r3.u64 = 1;
            return;
        }
    }
    ctx.r3.u64 = 0;
}

struct Impl {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Impl kImpls[] = {
    {0x0159, "XeCryptAesKey", &XeCryptAesKey},
    {0x015B, "XeCryptAesCbc", &XeCryptAesCbc},
    {0x0162, "XeCryptBnDwLePkcs1Format", &XeCryptBnDwLePkcs1Format},
    {0x0163, "XeCryptBnDwLePkcs1Verify", &XeCryptBnDwLePkcs1Verify},
    {0x016D, "XeCryptBnQwNeRsaPubCrypt", &XeCryptBnQwNeRsaPubCrypt},
    {0x0170, "XeCryptBnQw_SwapDwQwLeBe", &XeCryptBnQw_SwapDwQwLeBe},
    {0x018A, "XeCryptRandom", &XeCryptRandom},
};

}  // namespace

Status register_xboxkrnl_crypto_more_hle() {
    for (const Impl& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            return Status::Conflict;
        const Status status = register_import(kModuleXboxkrnl, impl.ordinal, impl.function, impl.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
