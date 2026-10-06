// Runtime check that the CPU supports every ISA extension the generated code
// was compiled for (see RCOMP_ISA_FLAGS). Compile-time macros are compared
// with CPUID so a title built for x86-64-v3 refuses to run on a CPU without
// e.g. FMA instead of computing wrong results or faulting later.
#include "rcomp/isa.h"

#include <cpuid.h>
#include <stdio.h>
#include <string.h>

namespace rcomp {

static bool bit(unsigned reg, int b) { return (reg >> b) & 1u; }

int check_isa(char* report, size_t cap) {
    unsigned a, b, c, d, b7 = 0, c7 = 0, d7 = 0, ce = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d)) {
        snprintf(report, cap, "{\"isa\":\"cpuid unavailable\"}");
        return -1;
    }
    unsigned a7, maxl = __get_cpuid_max(0, nullptr);
    if (maxl >= 7) __cpuid_count(7, 0, a7, b7, c7, d7);
    unsigned ae, be, de;
    if (__get_cpuid_max(0x80000000u, nullptr) >= 0x80000001u) __get_cpuid(0x80000001u, &ae, &be, &ce, &de);
    // OS support for AVX state (XGETBV XCR0 bits 1,2).
    bool osxsave = bit(c, 27), ymm_os = false;
    if (osxsave) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        ymm_os = (lo & 6u) == 6u;
    }
    struct F { const char* name; bool required; bool present; } f[] = {
        {"sse4.2", true, bit(c, 20)},
        {"popcnt", true, bit(c, 23)},
#ifdef __AVX__
        {"avx", true, bit(c, 28) && ymm_os},
#endif
#ifdef __AVX2__
        {"avx2", true, bit(b7, 5) && ymm_os},
#endif
#ifdef __FMA__
        {"fma", true, bit(c, 12) && ymm_os},
#endif
#ifdef __F16C__
        {"f16c", true, bit(c, 29)},
#endif
#ifdef __BMI__
        {"bmi1", true, bit(b7, 3)},
#endif
#ifdef __BMI2__
        {"bmi2", true, bit(b7, 8)},
#endif
#ifdef __MOVBE__
        {"movbe", true, bit(c, 22)},
#endif
#ifdef __LZCNT__
        {"lzcnt", true, bit(ce, 5)},
#endif
        {"avx512f(info)", false, bit(b7, 16)},
    };
    int missing = 0;
    size_t used = (size_t)snprintf(report, cap, "{\"isa\":{");
    for (size_t i = 0; i < sizeof f / sizeof f[0] && used < cap; ++i) {
        if (f[i].required && !f[i].present) ++missing;
        used += (size_t)snprintf(report + used, cap - used, "%s\"%s\":\"%s\"", i ? "," : "", f[i].name,
                                 f[i].present ? "yes" : (f[i].required ? "MISSING" : "no"));
    }
    if (used < cap) snprintf(report + used, cap - used, "},\"missing_required\":%d}", missing);
    return missing;
}

}  // namespace rcomp
