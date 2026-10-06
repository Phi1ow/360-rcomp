// XeCrypt AES-128 / random / PKCS#1 / RSA services (src/hle_xboxkrnl_crypto_more.cpp) through the same
// __imp__ symbols the generated code calls. Oracles: FIPS-197 Appendix A.1 and C.1, NIST SP 800-38A
// F.2.1/F.2.2 (CBC-AES128), and RSA/PKCS#1 v1.5 values computed independently with Python big
// integers (tests/crypto_rsa_oracle.py -> tests/crypto_rsa_vectors.inc).
#include <initializer_list>
#include <vector>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__XeCryptAesKey);
PPC_EXTERN_FUNC(__imp__XeCryptAesCbc);
PPC_EXTERN_FUNC(__imp__XeCryptRandom);
PPC_EXTERN_FUNC(__imp__XeCryptBnQw_SwapDwQwLeBe);
PPC_EXTERN_FUNC(__imp__XeCryptBnQwNeRsaPubCrypt);
PPC_EXTERN_FUNC(__imp__XeCryptBnDwLePkcs1Format);
PPC_EXTERN_FUNC(__imp__XeCryptBnDwLePkcs1Verify);

using namespace rcomp;
using namespace rcomp::rt;

namespace {
#include "crypto_rsa_vectors.inc"

GuestMemory g_mem;
uint8_t* g_base;
uint32_t g_buf;
enum : uint32_t {
    kState = 0x0000,  // XECRYPT_AES_STATE (0x160)
    kKey = 0x0200,
    kFeed = 0x0220,
    kData = 0x0300,   // 0x100
    kData2 = 0x0400,  // 0x100
    kRsa = 0x0800,    // XECRYPT_RSA + modulus (0x10 + 0x200)
    kBigA = 0x0C00,   // 0x200
    kBigB = 0x0E00,   // 0x200
    kHash = 0x1000,
    kSig = 0x1100,    // 0x200
};

uint32_t call(PPCFunc* f, std::initializer_list<uint64_t> args) {
    alignas(64) PPCContext ctx{};
    PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    size_t i = 0;
    for (uint64_t v : args) regs[i++]->u64 = v;
    f(ctx, g_base);
    return ctx.r3.u32;
}

uint8_t* at(uint32_t offset) { return g_base + g_buf + offset; }
void put(uint32_t offset, const std::vector<uint8_t>& bytes) { memcpy(at(offset), bytes.data(), bytes.size()); }
bool equal(uint32_t offset, const std::vector<uint8_t>& bytes) { return memcmp(at(offset), bytes.data(), bytes.size()) == 0; }
std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> out;
    for (; s[0] && s[1]; s += 2) {
        unsigned v = 0;
        sscanf(s, "%2x", &v);
        out.push_back(uint8_t(v));
    }
    return out;
}
void put_qw(uint32_t offset, const uint64_t* values, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) guest_write_be64(g_buf + offset + 8 * i, values[i]);
}
bool equal_qw(uint32_t offset, const uint64_t* values, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t v = 0;
        guest_read_be64(g_buf + offset + 8 * i, &v);
        if (v != values[i]) return false;
    }
    return true;
}
void put_rsa(const uint64_t* modulus, uint32_t count, uint32_t exponent) {
    guest_write_be32(g_buf + kRsa, count);
    guest_write_be32(g_buf + kRsa + 4, exponent);
    guest_write_be64(g_buf + kRsa + 8, 0);
    put_qw(kRsa + 0x10, modulus, count);
}

void aes() {
    // FIPS-197 Appendix A.1 key schedule: round 10 key = w[40..43].
    put(kKey, hex("2b7e151628aed2a6abf7158809cf4f3c"));
    call(__imp__XeCryptAesKey, {g_buf + kState, g_buf + kKey});
    CHECK(equal(kState, hex("2b7e151628aed2a6abf7158809cf4f3c")));
    CHECK(equal(kState + 16, hex("a0fafe1788542cb123a339392a6c7605")));
    CHECK(equal(kState + 160, hex("d014f9a8c9ee2589e13f0cc8b6630ca6")));
    // Equivalent inverse cipher schedule: dec[0] = enc[10], dec[10] = enc[0].
    CHECK(equal(kState + 0xB0, hex("d014f9a8c9ee2589e13f0cc8b6630ca6")));
    CHECK(equal(kState + 0xB0 + 160, hex("2b7e151628aed2a6abf7158809cf4f3c")));

    // SP 800-38A F.2.1 CBC-AES128.Encrypt, in place (Halo 3 passes in == out), chained over two calls.
    const auto plain = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                           "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    const auto cipher = hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
                            "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
    const auto iv = hex("000102030405060708090a0b0c0d0e0f");
    put(kData, plain);
    put(kFeed, iv);
    call(__imp__XeCryptAesCbc, {g_buf + kState, g_buf + kData, 32, g_buf + kData, g_buf + kFeed, 1});  // VOID
    CHECK(equal(kFeed, std::vector<uint8_t>(cipher.begin() + 16, cipher.begin() + 32)));  // last block
    call(__imp__XeCryptAesCbc, {g_buf + kState, g_buf + kData + 32, 32, g_buf + kData + 32, g_buf + kFeed, 1});
    CHECK(equal(kData, cipher));
    // F.2.2 CBC-AES128.Decrypt into a separate buffer, then in place.
    put(kFeed, iv);
    call(__imp__XeCryptAesCbc, {g_buf + kState, g_buf + kData, 64, g_buf + kData2, g_buf + kFeed, 0});
    CHECK(equal(kData2, plain));
    CHECK(equal(kFeed, std::vector<uint8_t>(cipher.begin() + 48, cipher.end())));
    put(kFeed, iv);
    call(__imp__XeCryptAesCbc, {g_buf + kState, g_buf + kData, 64, g_buf + kData, g_buf + kFeed, 0});
    CHECK(equal(kData, plain));

    // FIPS-197 Appendix C.1: one block with a zero IV is the ECB cipher.
    put(kKey, hex("000102030405060708090a0b0c0d0e0f"));
    call(__imp__XeCryptAesKey, {g_buf + kState, g_buf + kKey});
    put(kData, hex("00112233445566778899aabbccddeeff"));
    put(kFeed, std::vector<uint8_t>(16, 0));
    call(__imp__XeCryptAesCbc, {g_buf + kState, g_buf + kData, 16, g_buf + kData, g_buf + kFeed, 1});
    CHECK(equal(kData, hex("69c4e0d86a7b0430d8cdb78070b4c55a")));

    // No partial-block contract: explicit fatal. Zero length: nothing written.
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__XeCryptAesCbc, {g_buf + kState, g_buf + kData, 15, g_buf + kData, g_buf + kFeed, 1}),
                  fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    // An unmapped state is a guest-access fatal.
    CAPTURE_FATAL(call(__imp__XeCryptAesKey, {0x00001000, g_buf + kKey}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
}

void random_bytes() {
    memset(at(kData), 0, 64);
    memset(at(kData2), 0, 64);
    call(__imp__XeCryptRandom, {g_buf + kData, 64});
    call(__imp__XeCryptRandom, {g_buf + kData2, 64});
    CHECK(memcmp(at(kData), at(kData2), 64) != 0);
    bool zero = true;
    for (int i = 0; i < 64; ++i) zero &= at(kData)[i] == 0;
    CHECK(!zero);
    // Length 0 touches nothing (even a NULL buffer).
    call(__imp__XeCryptRandom, {0, 0});
    // Exactly cb bytes.
    memset(at(kData), 0xA5, 32);
    call(__imp__XeCryptRandom, {g_buf + kData, 7});
    for (int i = 7; i < 32; ++i) CHECK_EQ(at(kData)[i], 0xA5);
}

void swap() {
    put(kBigA, hex("0001020304050607" "08090a0b0c0d0e0f"));
    call(__imp__XeCryptBnQw_SwapDwQwLeBe, {g_buf + kBigA, g_buf + kBigB, 2});
    CHECK(equal(kBigB, hex("0706050403020100" "0f0e0d0c0b0a0908")));
    call(__imp__XeCryptBnQw_SwapDwQwLeBe, {g_buf + kBigB, g_buf + kBigB, 2});  // in place, back
    CHECK(equal(kBigB, hex("0001020304050607" "08090a0b0c0d0e0f")));
}

void rsa() {
    put_rsa(kRsa1024N, 16, 65537);
    put_qw(kBigA, kRsa1024M, 16);
    CHECK_EQ(call(__imp__XeCryptBnQwNeRsaPubCrypt, {g_buf + kBigA, g_buf + kBigB, g_buf + kRsa}), 1u);
    CHECK(equal_qw(kBigB, kRsa1024C, 16));
    // In place (Halo 3 passes a == b).
    CHECK_EQ(call(__imp__XeCryptBnQwNeRsaPubCrypt, {g_buf + kBigA, g_buf + kBigA, g_buf + kRsa}), 1u);
    CHECK(equal_qw(kBigA, kRsa1024C, 16));

    put_rsa(kRsa1024e3N, 16, 3);
    put_qw(kBigA, kRsa1024e3M, 16);
    CHECK_EQ(call(__imp__XeCryptBnQwNeRsaPubCrypt, {g_buf + kBigA, g_buf + kBigB, g_buf + kRsa}), 1u);
    CHECK(equal_qw(kBigB, kRsa1024e3C, 16));

    // An input not below the modulus is outside the primitive's domain: FALSE, output untouched.
    put_qw(kBigA, kRsa1024e3N, 16);
    memset(at(kBigB), 0x5A, 128);
    CHECK_EQ(call(__imp__XeCryptBnQwNeRsaPubCrypt, {g_buf + kBigA, g_buf + kBigB, g_buf + kRsa}), 0u);
    CHECK_EQ(at(kBigB)[0], 0x5A);

    // No contract for an empty / oversized key or an even modulus.
    bool fatal = false;
    guest_write_be32(g_buf + kRsa, 65);
    CAPTURE_FATAL(call(__imp__XeCryptBnQwNeRsaPubCrypt, {g_buf + kBigA, g_buf + kBigB, g_buf + kRsa}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    put_rsa(kRsa1024e3N, 16, 3);
    guest_write_be64(g_buf + kRsa + 0x10, kRsa1024e3N[0] & ~1ull);
    CAPTURE_FATAL(call(__imp__XeCryptBnQwNeRsaPubCrypt, {g_buf + kBigA, g_buf + kBigB, g_buf + kRsa}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
}

// EM = 00 01 FF.. 00 || T || H, stored little-endian (byte i = EM[k - 1 - i]).
std::vector<uint8_t> expected_block(const std::vector<uint8_t>& t, uint32_t k) {
    std::vector<uint8_t> em(k, 0xFF);
    em[0] = 0;
    em[1] = 1;
    em[k - t.size() - 20 - 1] = 0;
    memcpy(&em[k - t.size() - 20], t.data(), t.size());
    memcpy(&em[k - 20], kSha1Abc, 20);
    return std::vector<uint8_t>(em.rbegin(), em.rend());
}

void pkcs1() {
    memcpy(at(kHash), kSha1Abc, 20);
    const auto no_null = hex("301f300706052b0e03021a0414");
    const auto with_null = hex("3021300906052b0e03021a05000414");
    const std::vector<uint8_t> none;
    const std::vector<uint8_t>* forms[3] = {&none, &no_null, &with_null};
    for (uint32_t type = 0; type < 3; ++type) {
        for (uint32_t size : {39u, 128u, 256u, 512u}) {
            if (size < 3 + forms[type]->size() + 20 + 1) continue;
            memset(at(kSig), 0x11, 0x201);
            call(__imp__XeCryptBnDwLePkcs1Format, {g_buf + kHash, type, g_buf + kSig, size});
            CHECK(equal(kSig, expected_block(*forms[type], size)));
            CHECK_EQ(at(kSig)[size], 0x11);  // exactly cb bytes
            CHECK_EQ(call(__imp__XeCryptBnDwLePkcs1Verify, {g_buf + kHash, g_buf + kSig, size}), 1u);
            at(kSig)[size / 2] ^= 0x01;  // a padding or hash byte changes: rejected
            CHECK_EQ(call(__imp__XeCryptBnDwLePkcs1Verify, {g_buf + kHash, g_buf + kSig, size}), 0u);
        }
    }
    // Another hash does not verify; sizes no encoding can have are FALSE.
    call(__imp__XeCryptBnDwLePkcs1Format, {g_buf + kHash, 2, g_buf + kSig, 256});
    at(kHash)[0] ^= 0x80;
    CHECK_EQ(call(__imp__XeCryptBnDwLePkcs1Verify, {g_buf + kHash, g_buf + kSig, 256}), 0u);
    at(kHash)[0] ^= 0x80;
    CHECK_EQ(call(__imp__XeCryptBnDwLePkcs1Verify, {g_buf + kHash, g_buf + kSig, 38}), 0u);
    CHECK_EQ(call(__imp__XeCryptBnDwLePkcs1Verify, {g_buf + kHash, g_buf + kSig, 513}), 0u);
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__XeCryptBnDwLePkcs1Format, {g_buf + kHash, 3, g_buf + kSig, 256}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(call(__imp__XeCryptBnDwLePkcs1Format, {g_buf + kHash, 0, g_buf + kSig, 38}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);

    // Halo 3's verification sequence (0x8219F244..0x8219F27C) on an independently made 2048-bit
    // signature: swap, public operation in place, swap, verify.
    put_rsa(kRsa2048N, 32, 65537);
    memcpy(at(kSig), kSigAbcLe, 256);
    call(__imp__XeCryptBnQw_SwapDwQwLeBe, {g_buf + kSig, g_buf + kSig, 32});
    CHECK_EQ(call(__imp__XeCryptBnQwNeRsaPubCrypt, {g_buf + kSig, g_buf + kSig, g_buf + kRsa}), 1u);
    call(__imp__XeCryptBnQw_SwapDwQwLeBe, {g_buf + kSig, g_buf + kSig, 32});
    CHECK(equal(kSig, expected_block(with_null, 256)));
    CHECK_EQ(call(__imp__XeCryptBnDwLePkcs1Verify, {g_buf + kHash, g_buf + kSig, 256}), 1u);
    // A forged signature (one bit flipped before the public operation) fails.
    memcpy(at(kSig), kSigAbcLe, 256);
    at(kSig)[100] ^= 0x04;
    call(__imp__XeCryptBnQw_SwapDwQwLeBe, {g_buf + kSig, g_buf + kSig, 32});
    CHECK_EQ(call(__imp__XeCryptBnQwNeRsaPubCrypt, {g_buf + kSig, g_buf + kSig, g_buf + kRsa}), 1u);
    call(__imp__XeCryptBnQw_SwapDwQwLeBe, {g_buf + kSig, g_buf + kSig, 32});
    CHECK_EQ(call(__imp__XeCryptBnDwLePkcs1Verify, {g_buf + kHash, g_buf + kSig, 256}), 0u);
}

}  // namespace

int main() {
    if (g_mem.reserve() != MemStatus::Ok) return 2;
    g_base = g_mem.base();
    CHECK_ST(runtime_init(&g_mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x2000, 64, true, &g_buf), Status::Ok);
    aes();
    random_bytes();
    swap();
    rsa();
    pkcs1();
    runtime_shutdown();
    clear_imports();
    return test_result("rt_crypto_more");
}
