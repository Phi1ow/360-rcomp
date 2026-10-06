// Original fixtures only (patch0023_division.s, patch0023_overflow.s). Runs the
// unmodified generated C++ of the synthetic XEX, compiled either with
// include/rcomp/ppc_prelude.h (the R-comp helpers) or with the ppc_context.h
// defaults of patch 0023 alone. Expected values are computed here with wider
// arithmetic, independently of both:
//   quotient  = the truncated quotient when it is defined (divisor != 0 and it
//               fits the destination width); otherwise R-comp's deterministic
//               choice: 0 for a zero divisor, the most negative value for
//               most negative / -1 (Xenon leaves those quotients undefined)
//   XER[OV]   = 1 exactly when the quotient is undefined (OE forms); XER[SO] |= OV
//   CR0       = compare<int32_t>(low word of the result, 0) with SO = XER[SO]
//               (the record forms as the generator emitted them before patch 0023)
// usage: test_patch0023_execution all | zero_divisor_probe
// zero_divisor_probe divides by zero through fn_divw (fn_divwo) and prints a marker if the
// process survives (a generator without patch 0023 raises SIGFPE on x86-64).
#include <ppc_recomp_shared.h>
#include <patch0023_decls.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
alignas(64) uint8_t g_base[64];
int failures = 0;
int checks = 0;

void check(const char* what, uint64_t a, uint64_t b, uint64_t got, uint64_t expected) {
    const bool ok = got == expected;
    ++checks;
    if (!ok)
        std::printf("patch0023/%s a=0x%llX b=0x%llX FAIL got=0x%llX expected=0x%llX\n", what,
                    (unsigned long long)a, (unsigned long long)b, (unsigned long long)got, (unsigned long long)expected);
    failures += !ok;
}

struct Result {
    uint64_t r3;
    uint8_t ov, so;
    uint8_t lt, gt, eq, crso;
};
// XER[OV] starts at 2 (neither 0 nor 1): a form without OE must leave it untouched.
constexpr uint8_t kOvUntouched = 2;

Result call(PPCFunc* fn, uint64_t a, uint64_t b, uint8_t so) {
    PPCContext ctx{};
    ctx.r3.u64 = a;
    ctx.r4.u64 = b;
    ctx.xer.so = so;
    ctx.xer.ov = kOvUntouched;
    ctx.cr0.lt = ctx.cr0.gt = ctx.cr0.eq = ctx.cr0.so = 7;
    fn(ctx, g_base);
    return {ctx.r3.u64, ctx.xer.ov, ctx.xer.so, ctx.cr0.lt, ctx.cr0.gt, ctx.cr0.eq, ctx.cr0.so};
}

// Reference quotients (wider arithmetic, never a trapping division).
struct Ref { uint64_t q; bool undefined; };
Ref ref_divw(int32_t a, int32_t b) {
    if (b == 0) return {0, true};
    const int64_t q = int64_t(a) / int64_t(b);  // exact in 64 bits
    if (q > INT32_MAX) return {uint64_t(uint32_t(INT32_MIN)), true};
    return {uint64_t(uint32_t(int32_t(q))), false};
}
Ref ref_divwu(uint32_t a, uint32_t b) {
    if (b == 0) return {0, true};
    return {uint64_t(uint32_t(uint64_t(a) / uint64_t(b))), false};
}
Ref ref_divd(int64_t a, int64_t b) {
    if (b == 0) return {0, true};
    const __int128 q = __int128(a) / __int128(b);  // exact in 128 bits
    if (q > INT64_MAX) return {uint64_t(INT64_MIN), true};
    return {uint64_t(int64_t(q)), false};
}
Ref ref_divdu(uint64_t a, uint64_t b) {
    if (b == 0) return {0, true};
    return {uint64_t((unsigned __int128)a / b), false};
}

enum class Width { Word, Double };
void check_result(const char* what, uint64_t a, uint64_t b, const Result& r, const Ref& ref, Width width,
                  bool oe, bool record, uint8_t so_in) {
    char name[96];
    const uint64_t mask = width == Width::Word ? 0xFFFFFFFFull : ~0ull;
    std::snprintf(name, sizeof name, "%s/quotient", what);
    check(name, a, b, r.r3 & mask, ref.q & mask);
    const uint8_t ov = oe ? uint8_t(ref.undefined) : kOvUntouched;
    const uint8_t so = uint8_t(so_in | (oe ? uint8_t(ref.undefined) : 0));
    std::snprintf(name, sizeof name, "%s/xer_ov", what);
    check(name, a, b, r.ov, ov);
    std::snprintf(name, sizeof name, "%s/xer_so", what);
    check(name, a, b, r.so, so);
    const int32_t low = int32_t(uint32_t(r.r3));
    std::snprintf(name, sizeof name, "%s/cr0", what);
    const uint64_t cr = uint64_t(r.lt) << 24 | uint64_t(r.gt) << 16 | uint64_t(r.eq) << 8 | r.crso;
    const uint64_t expected = record ? (uint64_t(low < 0) << 24 | uint64_t(low > 0) << 16 | uint64_t(low == 0) << 8 | so)
                                     : 0x07070707ull;
    check(name, a, b, cr, expected);
}

constexpr int32_t kWords[] = {0, 1, -1, 2, -2, 3, 7, -7, 100, -100, 0x7FFFFFFF, INT32_MIN, INT32_MIN + 1, 0x12345678,
                              -0x12345678, 0x40000000};
constexpr int64_t kDoubles[] = {0, 1, -1, 2, -2, 3, 7, -7, 0x7FFFFFFF, INT32_MIN, 0x100000000ll, -0x100000000ll,
                                INT64_MAX, INT64_MIN, INT64_MIN + 1, 0x123456789ABCDEFll, -0x123456789ABCDEFll};

template <typename F>
void for_word_pairs(F f) {
    for (int32_t a : kWords)
        for (int32_t b : kWords)
            for (uint8_t so : {0, 1}) f(a, b, so);
}
template <typename F>
void for_double_pairs(F f) {
    for (int64_t a : kDoubles)
        for (int64_t b : kDoubles)
            for (uint8_t so : {0, 1}) f(a, b, so);
}
// Word forms see only the low word of rA and rB; the high words are noise here.
constexpr uint64_t kHighNoise = 0xA5A5A5A500000000ull;
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "zero_divisor_probe") == 0) {
#if defined(execute_divw)
        const Result r = call(execute_divw, 5, 0, 0);
#elif defined(execute_divwo)
        const Result r = call(execute_divwo, 5, 0, 0);
#else
        return 2;
#endif
        std::printf("patch0023/zero_divisor_probe survived r3=0x%llX\n", (unsigned long long)r.r3);
        return 0;
    }
    if (std::strcmp(argv[1], "all") != 0) return 2;

#ifdef execute_divw
    for_word_pairs([](int32_t a, int32_t b, uint8_t so) {
        const uint64_t ra = kHighNoise | uint32_t(a), rb = kHighNoise | uint32_t(b);
        check_result("divw", ra, rb, call(execute_divw, ra, rb, so), ref_divw(a, b), Width::Word, false, false, so);
        check_result("divwu", ra, rb, call(execute_divwu, ra, rb, so), ref_divwu(uint32_t(a), uint32_t(b)), Width::Word,
                     false, false, so);
        check_result("divw.", ra, rb, call(execute_divw_rc, ra, rb, so), ref_divw(a, b), Width::Word, false, true, so);
        check_result("divwu.", ra, rb, call(execute_divwu_rc, ra, rb, so), ref_divwu(uint32_t(a), uint32_t(b)),
                     Width::Word, false, true, so);
        check_result("divw_alias", ra, rb, call(execute_divw_alias, ra, rb, so), ref_divw(a, b), Width::Word, false,
                     false, so);
        // Halo 3 shape: quotient computed first, replaced by 1 when the divisor is <= 0.
        const Result g = call(execute_guarded_quotient, ra, rb, so);
        check("guarded_quotient", ra, rb, g.r3 & 0xFFFFFFFFu, b > 0 ? ref_divw(a, b).q : 1);
    });
    for_double_pairs([](int64_t a, int64_t b, uint8_t so) {
        check_result("divd", uint64_t(a), uint64_t(b), call(execute_divd, a, b, so), ref_divd(a, b), Width::Double,
                     false, false, so);
        check_result("divdu", uint64_t(a), uint64_t(b), call(execute_divdu, a, b, so),
                     ref_divdu(uint64_t(a), uint64_t(b)), Width::Double, false, false, so);
        check_result("divdu.", uint64_t(a), uint64_t(b), call(execute_divdu_rc, a, b, so),
                     ref_divdu(uint64_t(a), uint64_t(b)), Width::Double, false, true, so);
    });
#endif
#ifdef execute_divwo
    for_word_pairs([](int32_t a, int32_t b, uint8_t so) {
        const uint64_t ra = kHighNoise | uint32_t(a), rb = kHighNoise | uint32_t(b);
        check_result("divwo", ra, rb, call(execute_divwo, ra, rb, so), ref_divw(a, b), Width::Word, true, false, so);
        check_result("divwuo", ra, rb, call(execute_divwuo, ra, rb, so), ref_divwu(uint32_t(a), uint32_t(b)),
                     Width::Word, true, false, so);
        check_result("divwo.", ra, rb, call(execute_divwo_rc, ra, rb, so), ref_divw(a, b), Width::Word, true, true, so);
        check_result("divwuo.", ra, rb, call(execute_divwuo_rc, ra, rb, so), ref_divwu(uint32_t(a), uint32_t(b)),
                     Width::Word, true, true, so);
        check_result("divwo_alias", ra, rb, call(execute_divwo_alias, ra, rb, so), ref_divw(a, b), Width::Word, true,
                     false, so);
    });
    for_double_pairs([](int64_t a, int64_t b, uint8_t so) {
        check_result("divdo", uint64_t(a), uint64_t(b), call(execute_divdo, a, b, so), ref_divd(a, b), Width::Double,
                     true, false, so);
        check_result("divduo", uint64_t(a), uint64_t(b), call(execute_divduo, a, b, so),
                     ref_divdu(uint64_t(a), uint64_t(b)), Width::Double, true, false, so);
        check_result("divduo.", uint64_t(a), uint64_t(b), call(execute_divduo_rc, a, b, so),
                     ref_divdu(uint64_t(a), uint64_t(b)), Width::Double, true, true, so);
    });
#endif
#ifndef PATCH0023_ARM_NAME
#define PATCH0023_ARM_NAME "unnamed"
#endif
    const char* helpers = PATCH0023_ARM_NAME;
    std::printf("patch0023/execution %s (%d checks, %d failures, helpers: %s)\n", failures || !checks ? "FAIL" : "PASS",
                checks, failures, helpers);
    return failures || !checks ? 1 : 0;
}
