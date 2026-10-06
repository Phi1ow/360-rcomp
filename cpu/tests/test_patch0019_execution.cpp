// Original fixture only (patch0019_original.s). Runs the unmodified generated
// C++ of the synthetic XEX; no runtime stubs. Expected values come from the
// PowerPC definitions (bdnzt/bdnzf, vnor, vctuxs: round toward zero, unsigned
// saturation, NaN -> 0), computed here independently of the generator.
#include <ppc_recomp_shared.h>
#include <patch0019_decls.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <sys/mman.h>

namespace {
uint8_t* base = nullptr;
int failures = 0;
constexpr uint32_t kBuffer = 0x40000000;  // 16-byte aligned guest scratch

void check(const char* what, uint64_t got, uint64_t expected) {
    const bool ok = got == expected;
    std::printf("patch0019/%s %s got=0x%llX expected=0x%llX\n", what, ok ? "PASS" : "FAIL",
                (unsigned long long)got, (unsigned long long)expected);
    failures += !ok;
}
uint32_t call(PPCFunc* fn, uint32_t r3) {
    PPCContext ctx{};
    ctx.r1.u64 = kBuffer + 0x8000;  // a guest stack inside the scratch mapping
    ctx.r3.u64 = r3;
    fn(ctx, base);
    return ctx.r3.u32;
}
void store_be32(uint32_t address, uint32_t value) {
    for (int i = 0; i < 4; ++i) base[address + i] = uint8_t(value >> (24 - 8 * i));
}
uint32_t load_be32(uint32_t address) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = v << 8 | base[address + i];
    return v;
}
uint32_t float_bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
uint32_t vctuxs(float f, unsigned shift) {
    const double x = double(f) * std::ldexp(1.0, int(shift));
    if (std::isnan(x) || x <= 0.0) return 0;
    if (x >= 4294967295.0) return 0xFFFFFFFFu;
    return uint32_t(x);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::fseek(f, 0, SEEK_END);
    const size_t bytes = size_t(std::ftell(f));
    std::rewind(f);
    base = static_cast<uint8_t*>(mmap(nullptr, size_t(1) << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (base == MAP_FAILED || bytes > PPC_IMAGE_SIZE ||
        mprotect(base + PPC_IMAGE_BASE, PPC_IMAGE_SIZE, PROT_READ | PROT_WRITE) ||
        mprotect(base + kBuffer, 0x10000, PROT_READ | PROT_WRITE)) return 2;
    if (std::fread(base + PPC_IMAGE_BASE, 1, bytes, f) != bytes) return 2;
    std::fclose(f);

    check("owner", call(execute_owner, 0), 8);
    check("tail_jump", call(execute_tail_jump, 0), 161);
    check("tail_call", call(execute_tail_call, 0), 1021);
    check("chain_owner", call(execute_chain_owner, 0), 4);
    check("chain_jump", call(execute_chain_jump, 0), 13);

    for (uint32_t n : {1u, 3u, 5u, 9u}) {
        check("bdnzt", call(execute_bdnzt, n), n < 5 ? n : 5);
        check("bdnzf", call(execute_bdnzf, n), n < 5 ? n : 5);
    }

    const uint32_t a[4] = {0x00FF00FFu, 0x12345678u, 0x00000000u, 0xFFFFFFFFu};
    const uint32_t b[4] = {0x0F0F0F0Fu, 0x80000001u, 0x00000000u, 0x00000000u};
    for (int i = 0; i < 4; ++i) {
        store_be32(kBuffer + 4 * i, a[i]);
        store_be32(kBuffer + 16 + 4 * i, b[i]);
    }
    call(execute_vnor, kBuffer);
    for (int i = 0; i < 4; ++i) {
        check("vnor128", load_be32(kBuffer + 32 + 4 * i), uint32_t(~(a[i] | b[i])));
        check("vnor", load_be32(kBuffer + 48 + 4 * i), uint32_t(~(a[i] | b[i])));
        check("vnor128_high", load_be32(kBuffer + 64 + 4 * i), uint32_t(~(a[i] | b[i])));
    }

    const float inputs[2][4] = {{1.5f, -2.0f, 1e10f, std::nanf("")},
                                {0.0f, 0.124f, 536870911.0f, -0.0f}};
    for (const auto& in : inputs) {
        for (int i = 0; i < 4; ++i) store_be32(kBuffer + 4 * i, float_bits(in[i]));
        call(execute_vcfpuxws, kBuffer);
        for (int i = 0; i < 4; ++i) {
            check("vcfpuxws128_scale3", load_be32(kBuffer + 16 + 4 * i), vctuxs(in[i], 3));
            check("vcfpuxws128_scale0", load_be32(kBuffer + 32 + 4 * i), vctuxs(in[i], 0));
            check("vcfpuxws128_scale31_high", load_be32(kBuffer + 48 + 4 * i), vctuxs(in[i], 31));
        }
    }

    const uint32_t expected[] = {11, 22, 33, 44, 55, 55, 55, 55};
    const uint32_t indices[] = {0, 1, 2, 3, 4, 255, 0x80000000u, 0xFFFFFFFFu};
    for (int i = 0; i < 8; ++i) check("hoisted_switch", call(execute_hoisted_switch, indices[i]), expected[i]);

    std::printf("patch0019/execution %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
