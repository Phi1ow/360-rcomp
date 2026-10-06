// Host test of the integer division helpers of include/rcomp/ppc_prelude.h
// (PPC_DIVW, PPC_DIVWU, PPC_DIVD, PPC_DIVDU and the PPC_*_OVERFLOW predicates of
// the OE forms, emitted by cpu/patches/xenonrecomp/0023). Expected values come
// from wider arithmetic, independently of the helpers:
//   divisor 0                    -> quotient 0, overflow 1
//   most negative / -1 (signed)  -> most negative (wrap), overflow 1
//   anything else                -> truncated quotient, overflow 0
// The zero and most-negative / -1 values are R-comp's deterministic choice for
// quotients Xenon leaves undefined, not a claim about the hardware.
// Build (host, no runtime needed; -fsanitize-trap turns any undefined behaviour,
// e.g. a signed overflow or a division by zero in a helper, into a trap):
//   clang++ -std=c++20 -O2 -Wall -Wextra -Werror -I include \
//       -fsanitize=undefined -fsanitize-trap=undefined \
//       cpu/tests/test_ppc_division.cpp -o build/.../test_ppc_division
#include "rcomp/ppc_prelude.h"

#include <cstdint>
#include <cstdio>

namespace {
int failures = 0;
int checks = 0;

void check(const char* what, uint64_t a, uint64_t b, uint64_t got, uint64_t expected) {
    ++checks;
    if (got != expected) {
        ++failures;
        std::printf("ppc_division/%s a=0x%llX b=0x%llX FAIL got=0x%llX expected=0x%llX\n", what, (unsigned long long)a,
                    (unsigned long long)b, (unsigned long long)got, (unsigned long long)expected);
    }
}

// The helpers must be usable where the generated code uses them, and constant
// folding must agree with the run-time path.
static_assert(sizeof(PPC_DIVW(int32_t(1), int32_t(1))) == 4 && sizeof(PPC_DIVD(int64_t(1), int64_t(1))) == 8);

void word(int32_t a, int32_t b) {
    const uint64_t ua = uint32_t(a), ub = uint32_t(b);
    // Signed: the exact quotient in 64 bits, wrapped to 32 when it does not fit.
    const bool sundef = b == 0 || (b == -1 && a == INT32_MIN);
    const int32_t sq = b == 0 ? 0 : int32_t(uint32_t(int64_t(a) / int64_t(b)));
    check("divw", ua, ub, uint32_t(PPC_DIVW(a, b)), uint32_t(sq));
    check("divw_overflow", ua, ub, PPC_DIVW_OVERFLOW(a, b), sundef);
    const uint32_t uq = b == 0 ? 0u : uint32_t(ua / ub);
    check("divwu", ua, ub, PPC_DIVWU(uint32_t(a), uint32_t(b)), uq);
    check("divwu_overflow", ua, ub, PPC_DIVWU_OVERFLOW(uint32_t(a), uint32_t(b)), b == 0);
}

void dword(int64_t a, int64_t b) {
    const bool sundef = b == 0 || (b == -1 && a == INT64_MIN);
    const __int128 exact = b == 0 ? 0 : __int128(a) / __int128(b);
    const int64_t sq = int64_t(uint64_t((unsigned __int128)exact));
    check("divd", uint64_t(a), uint64_t(b), uint64_t(PPC_DIVD(a, b)), uint64_t(sq));
    check("divd_overflow", uint64_t(a), uint64_t(b), PPC_DIVD_OVERFLOW(a, b), sundef);
    const uint64_t uq = b == 0 ? 0 : uint64_t((unsigned __int128)uint64_t(a) / uint64_t(b));
    check("divdu", uint64_t(a), uint64_t(b), PPC_DIVDU(uint64_t(a), uint64_t(b)), uq);
    check("divdu_overflow", uint64_t(a), uint64_t(b), PPC_DIVDU_OVERFLOW(uint64_t(a), uint64_t(b)), b == 0);
}

uint64_t rng = 0x9E3779B97F4A7C15ull;
uint64_t next() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}
}  // namespace

int main() {
    // The documented values, spelled out.
    check("divw 0 divisor", 5, 0, uint32_t(PPC_DIVW(5, 0)), 0);
    check("divw -5 / 0", uint32_t(-5), 0, uint32_t(PPC_DIVW(-5, 0)), 0);
    check("divw INT_MIN / 0", uint32_t(INT32_MIN), 0, uint32_t(PPC_DIVW(INT32_MIN, 0)), 0);
    check("divw INT_MIN / -1", uint32_t(INT32_MIN), uint32_t(-1), uint32_t(PPC_DIVW(INT32_MIN, -1)), 0x80000000u);
    check("divw INT_MIN / -1 overflow", 0, 0, PPC_DIVW_OVERFLOW(INT32_MIN, -1), 1);
    check("divw 7 / -1", 7, uint32_t(-1), uint32_t(PPC_DIVW(7, -1)), uint32_t(-7));
    check("divw -7 / 2", uint32_t(-7), 2, uint32_t(PPC_DIVW(-7, 2)), uint32_t(-3));
    check("divw 7 / -2", 7, uint32_t(-2), uint32_t(PPC_DIVW(7, -2)), uint32_t(-3));
    check("divw 100 / 7", 100, 7, uint32_t(PPC_DIVW(100, 7)), 14);
    check("divw INT_MIN / 1", uint32_t(INT32_MIN), 1, uint32_t(PPC_DIVW(INT32_MIN, 1)), 0x80000000u);
    check("divw INT_MIN / -1 overflow off for 1", 0, 0, PPC_DIVW_OVERFLOW(INT32_MIN, 1), 0);
    check("divwu 0 divisor", 5, 0, PPC_DIVWU(5u, 0u), 0);
    check("divwu max / 2", 0xFFFFFFFFu, 2, PPC_DIVWU(0xFFFFFFFFu, 2u), 0x7FFFFFFFu);
    check("divwu 0x80000000 / max", 0x80000000u, 0xFFFFFFFFu, PPC_DIVWU(0x80000000u, 0xFFFFFFFFu), 0);
    check("divwu overflow only on 0", 0, 0, PPC_DIVWU_OVERFLOW(0x80000000u, 0xFFFFFFFFu), 0);
    check("divd 0 divisor", 5, 0, uint64_t(PPC_DIVD(int64_t(5), int64_t(0))), 0);
    check("divd INT64_MIN / -1", uint64_t(INT64_MIN), ~0ull, uint64_t(PPC_DIVD(INT64_MIN, int64_t(-1))),
          0x8000000000000000ull);
    check("divd INT64_MIN / -1 overflow", 0, 0, PPC_DIVD_OVERFLOW(INT64_MIN, int64_t(-1)), 1);
    check("divd -9 / 4", uint64_t(-9), 4, uint64_t(PPC_DIVD(int64_t(-9), int64_t(4))), uint64_t(-2));
    check("divd 2^40 / 3", 1ull << 40, 3, uint64_t(PPC_DIVD(int64_t(1) << 40, int64_t(3))), (1ull << 40) / 3);
    check("divdu 0 divisor", 5, 0, PPC_DIVDU(uint64_t(5), uint64_t(0)), 0);
    check("divdu max / 3", ~0ull, 3, PPC_DIVDU(~0ull, uint64_t(3)), 0x5555555555555555ull);
    check("divdu overflow on 0", 0, 0, PPC_DIVDU_OVERFLOW(uint64_t(1), uint64_t(0)), 1);

    // Edge grid and random operands against the wide-arithmetic reference.
    const int32_t words[] = {0, 1, -1, 2, -2, 3, -3, 7, -7, 0x7FFFFFFF, INT32_MIN, INT32_MIN + 1, 0x10000, -0x10000};
    for (int32_t a : words)
        for (int32_t b : words) word(a, b);
    const int64_t dwords[] = {0, 1, -1, 2, -2, 3, 7, -7, 0x7FFFFFFF, INT32_MIN, 0x100000000ll, -0x100000000ll,
                              INT64_MAX, INT64_MIN, INT64_MIN + 1};
    for (int64_t a : dwords)
        for (int64_t b : dwords) dword(a, b);
    for (int i = 0; i < 200000; ++i) {
        const uint64_t x = next(), y = next();
        // Small divisors are the interesting ones: shift some of them down.
        const uint64_t yy = (i & 3) == 0 ? y >> (y & 63) : y;
        word(int32_t(uint32_t(x)), int32_t(uint32_t(yy)));
        dword(int64_t(x), int64_t(yy));
    }
    std::printf("ppc_division %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
