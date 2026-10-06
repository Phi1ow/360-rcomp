// Exhaustive differential test of the float16 helpers of patch 0004
// (ppc_f32_to_f16_rz / ppc_f16_to_f32, patched XenonUtils/ppc_context.h)
// against the host's F16C instructions (vcvtps2ph with round-toward-zero,
// vcvtph2ps), which are the reference the helpers emulate. All 2^32 float bit
// patterns and all 2^16 half patterns. MXCSR is left at its default (no DAZ/FTZ).
#define PPC_CONFIG_H_INCLUDED
#include <ppc_context.h>

#include <immintrin.h>
#include <stdio.h>

__attribute__((target("f16c"))) static uint16_t hw_f2h(uint32_t bits) {
    return (uint16_t)_cvtss_sh(ppc_bits_to_f32(bits), _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
}
__attribute__((target("f16c"))) static uint32_t hw_h2f(uint16_t h) { return ppc_f32_to_bits(_cvtsh_ss(h)); }

int main() {
    unsigned long long bad = 0;
    for (uint64_t b = 0; b <= 0xFFFFFFFFull; ++b) {
        uint16_t a = ppc_f32_to_f16_rz((uint32_t)b), r = hw_f2h((uint32_t)b);
        if (a != r && bad++ < 8) printf("f32->f16 0x%08llX: helper 0x%04X hw 0x%04X\n", (unsigned long long)b, a, r);
    }
    unsigned bad_h = 0;
    for (uint32_t h = 0; h < 0x10000; ++h) {
        uint32_t a = ppc_f16_to_f32((uint16_t)h), r = hw_h2f((uint16_t)h);
        if (a != r && bad_h++ < 8) printf("f16->f32 0x%04X: helper 0x%08X hw 0x%08X\n", h, a, r);
    }
    printf("f32->f16 mismatches: %llu / 4294967296\nf16->f32 mismatches: %u / 65536\n", bad, bad_h);
    return (bad || bad_h) ? 1 : 0;
}
