// Original fixture only (patch0021_original.s). Runs the unmodified generated
// C++ of the synthetic XEX. Expected values come from the PowerPC definitions
// (load/store with update, lhbrx, bdzf/bdzt, the VMX integer forms, vsldoi128,
// record forms in 64-bit mode) and from the fixture's own case values, computed
// here independently of the generator. The runtime hooks are the test doubles of
// patch0021_hooks.h.
#include <ppc_recomp_shared.h>
#include <patch0021_decls.h>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <sys/mman.h>

namespace {
uint8_t* image = nullptr;
int failures = 0;
int checks = 0;
constexpr uint32_t kBuffer = 0x40000000;  // 64 KiB guest scratch
constexpr uint32_t B = kBuffer + 0x1000;   // base register of the update-form tests

struct Trapped { uint32_t address; };
struct OutOfRange { uint32_t pc, index; };
int indirectCalls = 0;

void check(const char* what, uint64_t got, uint64_t expected) {
    const bool ok = got == expected;
    ++checks;
    std::printf("patch0021/%s %s got=0x%llX expected=0x%llX\n", what, ok ? "PASS" : "FAIL",
                (unsigned long long)got, (unsigned long long)expected);
    failures += !ok;
}
void store_be(uint32_t address, uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) image[address + i] = uint8_t(value >> (8 * (bytes - 1 - i)));
}
uint64_t load_be(uint32_t address, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = v << 8 | image[address + i];
    return v;
}
uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
uint64_t dbits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }
float bitsf(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

PPCContext run(PPCFunc* fn, const std::function<void(PPCContext&)>& setup) {
    PPCContext ctx{};
    ctx.r1.u64 = kBuffer + 0x8000;
    setup(ctx);
    fn(ctx, image);
    return ctx;
}
uint32_t call(PPCFunc* fn, uint32_t r3, uint32_t r4 = 0) {
    return run(fn, [&](PPCContext& c) { c.r3.u64 = r3; c.r4.u64 = r4; }).r3.u32;
}
uint32_t rotl(uint32_t x, uint32_t n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }
}  // namespace

[[noreturn]] void TESTDOUBLE_guest_trap(uint32_t address) { throw Trapped{address}; }
[[noreturn]] void TESTDOUBLE_switch_out_of_range(uint32_t pc, uint32_t index) { throw OutOfRange{pc, index}; }
void TESTDOUBLE_call_indirect(PPCContext& ctx, uint8_t* base, uint32_t target) {
    ++indirectCalls;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m)
        if (m->guest == target) { m->host(ctx, base); return; }
    throw Trapped{target};
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::fseek(f, 0, SEEK_END);
    const size_t bytes = size_t(std::ftell(f));
    std::rewind(f);
    image = static_cast<uint8_t*>(mmap(nullptr, size_t(1) << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (image == MAP_FAILED || bytes > PPC_IMAGE_SIZE ||
        mprotect(image + PPC_IMAGE_BASE, PPC_IMAGE_SIZE, PROT_READ | PROT_WRITE) ||
        mprotect(image + kBuffer, 0x10000, PROT_READ | PROT_WRITE)) return 2;
    if (std::fread(image + PPC_IMAGE_BASE, 1, bytes, f) != bytes) return 2;
    std::fclose(f);
    for (uint32_t k = 0; k < 0x200; ++k) image[B - 0x100 + k] = uint8_t(k * 37 + 11);

    // ---- load/store with update ----------------------------------------------
    PPCContext c = run(execute_lhzu, [](PPCContext& x) { x.r3.u64 = B; });
    check("lhzu_rt", c.r4.u64, load_be(B + 6, 2));
    check("lhzu_ra", c.r3.u64, B + 6);
    store_be(B - 2, 0x8001, 2);
    c = run(execute_lhau, [](PPCContext& x) { x.r3.u64 = B; });
    check("lhau_rt", c.r4.u64, 0xFFFFFFFFFFFF8001ull);
    check("lhau_ra", c.r3.u64, B - 2);
    store_be(B + 8, 0x3EAAAAABu, 4);
    c = run(execute_lfsu, [](PPCContext& x) { x.r3.u64 = B; });
    check("lfsu_ft", c.f1.u64, dbits(double(bitsf(0x3EAAAAABu))));
    check("lfsu_ra", c.r3.u64, B + 8);
    store_be(B + 16, dbits(-1.0e300), 8);
    c = run(execute_lfdu, [](PPCContext& x) { x.r3.u64 = B; });
    check("lfdu_ft", c.f1.u64, dbits(-1.0e300));
    check("lfdu_ra", c.r3.u64, B + 16);
    c = run(execute_sthu, [](PPCContext& x) { x.r3.u64 = B; x.r4.u64 = 0x12345678ABCDull; });
    check("sthu_mem", load_be(B + 4, 2), 0xABCD);
    check("sthu_ra", c.r3.u64, B + 4);
    c = run(execute_stfsu, [](PPCContext& x) { x.r3.u64 = B; x.f1.f64 = 1.0 / 3.0; });
    check("stfsu_mem_rounded", load_be(B + 12, 4), 0x3EAAAAABu);
    check("stfsu_ra", c.r3.u64, B + 12);
    c = run(execute_stfdu, [](PPCContext& x) { x.r3.u64 = B; x.f1.f64 = -2.5; });
    check("stfdu_mem", load_be(B - 24, 8), 0xC004000000000000ull);
    check("stfdu_ra", c.r3.u64, B - 24);
    c = run(execute_lbzux, [](PPCContext& x) { x.r3.u64 = B; x.r5.u64 = 7; });
    check("lbzux_rt", c.r4.u64, image[B + 7]);
    check("lbzux_ra", c.r3.u64, B + 7);
    c = run(execute_lhzux, [](PPCContext& x) { x.r3.u64 = B; x.r5.u64 = uint64_t(-4); });
    check("lhzux_rt", c.r4.u64, load_be(B - 4, 2));
    check("lhzux_ra", c.r3.u32, B - 4);
    store_be(B + 10, 0xFFFE, 2);
    c = run(execute_lhaux, [](PPCContext& x) { x.r3.u64 = B; x.r5.u64 = 10; });
    check("lhaux_rt", c.r4.u64, uint64_t(-2));
    check("lhaux_ra", c.r3.u64, B + 10);
    store_be(B + 12, 0xCAFEF00Du, 4);
    c = run(execute_lwzux, [](PPCContext& x) { x.r3.u64 = B; x.r5.u64 = 12; });
    check("lwzux_rt_is_rb", c.r5.u64, 0xCAFEF00Du);
    check("lwzux_ra_uses_old_rb", c.r3.u64, B + 12);
    store_be(B + 20, 0x80000005u, 4);
    c = run(execute_lwaux, [](PPCContext& x) { x.r3.u64 = B; x.r5.u64 = 20; });
    check("lwaux_rt", c.r4.u64, 0xFFFFFFFF80000005ull);
    check("lwaux_ra", c.r3.u64, B + 20);
    store_be(B + 24, 0x0123456789ABCDEFull, 8);
    c = run(execute_ldux, [](PPCContext& x) { x.r3.u64 = B; x.r5.u64 = 24; });
    check("ldux_rt", c.r4.u64, 0x0123456789ABCDEFull);
    check("ldux_ra", c.r3.u64, B + 24);
    store_be(B + 32, 0xC0490FDBu, 4);
    c = run(execute_lfsux, [](PPCContext& x) { x.r3.u64 = B; x.r5.u64 = 32; });
    check("lfsux_ft", c.f1.u64, dbits(double(bitsf(0xC0490FDBu))));
    check("lfsux_ra", c.r3.u64, B + 32);
    store_be(B + 40, dbits(6.02214076e23), 8);
    c = run(execute_lfdux, [](PPCContext& x) { x.r3.u64 = B; x.r5.u64 = 40; });
    check("lfdux_ft", c.f1.u64, dbits(6.02214076e23));
    check("lfdux_ra", c.r3.u64, B + 40);
    c = run(execute_stbux, [](PPCContext& x) { x.r3.u64 = B; x.r4.u64 = 0x1FF; x.r5.u64 = 3; });
    check("stbux_mem", image[B + 3], 0xFF);
    check("stbux_ra", c.r3.u64, B + 3);
    c = run(execute_sthux, [](PPCContext& x) { x.r3.u64 = B; x.r5.u64 = 2; });
    check("sthux_rs_is_ra_stores_old", load_be(B + 2, 2), B & 0xFFFF);
    check("sthux_ra", c.r3.u64, B + 2);
    c = run(execute_stdux, [](PPCContext& x) { x.r3.u64 = B; x.r4.u64 = 0x1122334455667788ull; x.r5.u64 = 48; });
    check("stdux_mem", load_be(B + 48, 8), 0x1122334455667788ull);
    check("stdux_ra", c.r3.u64, B + 48);
    c = run(execute_stfsux, [](PPCContext& x) { x.r3.u64 = B; x.r5.u64 = 56; x.f1.f64 = 0.1; });
    check("stfsux_mem_rounded", load_be(B + 56, 4), 0x3DCCCCCDu);
    check("stfsux_ra", c.r3.u64, B + 56);
    c = run(execute_stfdux, [](PPCContext& x) { x.r3.u64 = B; x.r5.u64 = 64; x.f1.f64 = 3.25; });
    check("stfdux_mem", load_be(B + 64, 8), dbits(3.25));
    check("stfdux_ra", c.r3.u64, B + 64);
    store_be(B + 6, 0x1234, 2);
    c = run(execute_lhbrx, [](PPCContext& x) { x.r3.u64 = B; x.r5.u64 = 6; x.r7.u64 = B + 6; x.r4.u64 = ~0ull; });
    check("lhbrx", c.r4.u64, 0x3412);
    check("lhbrx_ra0", c.r6.u64, 0x3412);
    check("lhbrx_no_update", c.r3.u64, B);

    // ---- bdzf / bdzt ----------------------------------------------------------
    auto loop = [](uint64_t n, int32_t thr, bool zt, uint64_t& ctr) {
        ctr = n;
        int32_t i = 0;
        for (;;) {
            ++i;
            const bool bit = zt ? i > thr : i < thr;
            --ctr;
            if (uint32_t(ctr) == 0 && (zt ? bit : !bit)) break;
            if (!(i < 40)) break;
        }
        return uint32_t(i);
    };
    for (auto [n, thr] : std::initializer_list<std::pair<uint32_t, int32_t>>{{3, 1}, {3, 5}, {1, 0}, {5, 2}, {0, 3}, {4, 4}}) {
        uint64_t ctr = 0;
        const uint32_t i = loop(n, thr, false, ctr);
        c = run(execute_bdzf, [&](PPCContext& x) { x.r3.u64 = n; x.r4.u64 = uint64_t(int64_t(thr)); });
        check("bdzf_count", c.r3.u64, i);
        check("bdzf_ctr", c.r4.u64, ctr);
        const uint32_t j = loop(n, thr, true, ctr);
        c = run(execute_bdzt, [&](PPCContext& x) { x.r3.u64 = n; x.r4.u64 = uint64_t(int64_t(thr)); });
        check("bdzt_count", c.r3.u64, j);
        check("bdzt_ctr", c.r4.u64, ctr);
    }
    check("bdzf_tail_taken", call(execute_bdzf_tail, 1, 5), 2);
    check("bdzf_tail_cr_true", call(execute_bdzf_tail, 1, 0), 1);
    check("bdzf_tail_ctr_nonzero", call(execute_bdzf_tail, 2, 5), 1);

    // ---- VMX ----------------------------------------------------------------------
    const uint32_t sets[3][2][4] = {
        {{0x7FFFFFF0u, 0x80000005u, 0x00000010u, 0xFFFFFFFFu}, {0x00000020u, 0xFFFFFFF0u, 0x00000010u, 0x00000001u}},
        {{5u, 6u, 7u, 0xFFFFFFFFu}, {1u, 2u, 3u, 4u}},
        {{0x01020304u, 0x8000FFFFu, 0xFFFF0001u, 0x12345678u}, {0x01020304u, 0x8000FFFFu, 0xFFFF0001u, 0x12345678u}},
    };
    for (int s = 0; s < 3; ++s) {
        const auto& A = sets[s][0];
        const auto& Bv = sets[s][1];
        for (int i = 0; i < 4; ++i) { store_be(kBuffer + 4 * i, A[i], 4); store_be(kBuffer + 16 + 4 * i, Bv[i], 4); }
        run(execute_vmx, [](PPCContext& x) { x.r3.u64 = kBuffer; });
        bool all = true, none = true;
        for (int i = 0; i < 4; ++i) {
            const int64_t sum = int64_t(int32_t(A[i])) + int64_t(int32_t(Bv[i]));
            check("vaddsws", load_be(kBuffer + 32 + 4 * i, 4), uint32_t(sum > INT32_MAX ? INT32_MAX : sum < INT32_MIN ? INT32_MIN : sum));
            check("vsubuwm", load_be(kBuffer + 48 + 4 * i, 4), uint32_t(A[i] - Bv[i]));
            uint32_t bm = 0, hs = 0;
            for (int k = 0; k < 4; ++k) bm |= uint32_t(uint8_t((A[i] >> (8 * k)) - (Bv[i] >> (8 * k)))) << (8 * k);
            for (int k = 0; k < 2; ++k) {
                const uint32_t h = ((A[i] >> (16 * k)) & 0xFFFF) + ((Bv[i] >> (16 * k)) & 0xFFFF);
                hs |= (h > 0xFFFF ? 0xFFFF : h) << (16 * k);
            }
            check("vsububm", load_be(kBuffer + 64 + 4 * i, 4), bm);
            check("vadduhs", load_be(kBuffer + 80 + 4 * i, 4), hs);
            check("vsubuws", load_be(kBuffer + 96 + 4 * i, 4), A[i] > Bv[i] ? A[i] - Bv[i] : 0);
            check("vcmpgtuw", load_be(kBuffer + 112 + 4 * i, 4), A[i] > Bv[i] ? 0xFFFFFFFFu : 0);
            all = all && A[i] > Bv[i];
            none = none && !(A[i] > Bv[i]);
            check("vrlw", load_be(kBuffer + 128 + 4 * i, 4), rotl(A[i], Bv[i]));
            check("vrlw128", load_be(kBuffer + 144 + 4 * i, 4), rotl(A[i], Bv[i]));
        }
        uint8_t cat[32];
        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 4; ++k) { cat[4 * i + k] = uint8_t(A[i] >> (24 - 8 * k)); cat[16 + 4 * i + k] = uint8_t(Bv[i] >> (24 - 8 * k)); }
        for (int k = 0; k < 16; ++k) {
            check("vsldoi128_sh12_maclhwu_word", image[kBuffer + 160 + k], cat[12 + k]);
            check("vsldoi128_sh4_macchwu_word", image[kBuffer + 192 + k], cat[4 + k]);
        }
        check("vcmpgtuw_record_cr6", (load_be(kBuffer + 176, 4) >> 4) & 0xF, all ? 8 : none ? 2 : 0);
    }

    for (uint32_t k = 0; k < 80; ++k) image[kBuffer + k] = uint8_t(k * 7 + 3);
    run(execute_lru, [](PPCContext& x) { x.r3.u64 = kBuffer; });
    for (uint32_t k = 0; k < 16; ++k) check("lvxl_stvxl", image[kBuffer + 48 + k], uint8_t((16 + k) * 7 + 3));
    for (uint32_t k = 64; k < 80; ++k)
        check("lvxl128_stvlxl128", image[kBuffer + k], k < 69 ? uint8_t(k * 7 + 3) : uint8_t((32 + k - 69) * 7 + 3));

    // ---- record forms of doubleword results (CR0 from the 64-bit value) ----------
    c = run(execute_rldicl_rc, [](PPCContext& x) { x.r3.u64 = 0x8000000000000000ull; });
    check("rldicl_rc_value", c.r4.u64, 0x80000000u);
    check("rldicl_rc_cr0_gt_not_lt", c.r5.u32 >> 28, 4);
    check("sradi_rc_cr0_lt", c.r7.u32 >> 28, 8);
    check("rldicr_rc_cr0_lt", c.r9.u32 >> 28, 8);
    c = run(execute_rldicl_rc, [](PPCContext& x) { x.r3.u64 = 0x00000000FFFFFFFFull; });
    check("rldicl_rc_cr0_eq", c.r5.u32 >> 28, 2);
    check("sradi_rc_cr0_gt", c.r7.u32 >> 28, 4);
    check("rldicr_rc_cr0_eq", c.r9.u32 >> 28, 2);

    // ---- jump tables ------------------------------------------------------------
    const uint32_t outside[] = {4, 5, 255, 0x80000000u, 0xFFFFFFFFu};
    for (uint32_t i = 0; i < 4; ++i) check("switch_scheduled_absolute", call(execute_sw_sched, i), 10 + i);
    for (uint32_t i : outside) check("switch_scheduled_absolute_default", call(execute_sw_sched, i), 99);
    const uint32_t computed[] = {20, 21, 22, 21, 20};
    for (uint32_t i = 0; i < 5; ++i) check("switch_computed_nops", call(execute_sw_computed, i), computed[i]);
    for (uint32_t i : outside) if (i > 4) check("switch_computed_default", call(execute_sw_computed, i), 98);
    const uint32_t byteoff[] = {30, 31, 30};
    for (uint32_t i = 0; i < 3; ++i) check("switch_byte_offset_nops", call(execute_sw_byte, i), byteoff[i]);
    for (uint32_t i : outside) check("switch_byte_offset_default", call(execute_sw_byte, i), 97);
    const uint32_t shortoff[] = {42, 41, 40};
    for (uint32_t i = 0; i < 3; ++i) check("switch_short_offset_scheduled", call(execute_sw_short, i), shortoff[i]);
    for (uint32_t i : outside) check("switch_short_offset_default", call(execute_sw_short, i), 96);
    for (uint32_t i = 0; i < 3; ++i) check("switch_bge_guard", call(execute_sw_bge, i), 50 + i);
    for (uint32_t i : {3u, 4u, 0xFFFFFFFFu}) check("switch_bge_guard_default", call(execute_sw_bge, i), 95);
    for (uint32_t i = 0; i < 3; ++i) check("switch_ctr_target", call(execute_sw_reuse, i), 60 + i);
    for (uint32_t i : {3u, 0x80000000u}) check("switch_ctr_target_default", call(execute_sw_reuse, i), 94);
    for (uint32_t i = 0; i < 3; ++i) check("switch_guard_on_copied_register", call(execute_sw_copy, 0, i), 100 + i);
    for (uint32_t i : {3u, 0xFFFFFFFFu}) check("switch_guard_on_copied_register_default", call(execute_sw_copy, 0, i), 92);
    for (uint32_t i = 0; i < 2; ++i) check("switch_guard_over_branch", call(execute_sw_skip, i), 110 + i);
    for (uint32_t i : {2u, 0x80000000u}) check("switch_guard_over_branch_default", call(execute_sw_skip, i), 91);
    const uint32_t gap[] = {71, 70, 77};
    for (uint32_t i = 0; i < 3; ++i) check("switch_inline_code_after_table", call(execute_sw_inline_gap, i), gap[i]);
    for (uint32_t i = 0; i < 2; ++i) check("switch_other_register_compare", call(execute_sw_other_compare, i, 5000 + i), 80 + i);
    check("switch_unreachable_case_0", call(execute_sw_unreachable, 0), 90);
    check("switch_unreachable_next_function", call(execute_sw_unreachable, 2), 2);
    check("switch_unreachable_default", call(execute_sw_unreachable, 3), 93);
    try {
        call(execute_sw_unreachable, 1);
        check("switch_zero_word_traps", 0, 1);
    } catch (const Trapped& t) {
        check("switch_zero_word_traps", t.address, kZeroWord);
    }
    indirectCalls = 0;
    check("dispatch_0", call(execute_dispatch, 0), 2);
    check("dispatch_1", call(execute_dispatch, 1), 2);
    check("dispatch_is_indirect_call", indirectCalls, 2);
    check("dispatch_guard_returns", call(execute_dispatch, 7), 7);
    check("data_owner", call(execute_data_owner, 0), 5);
    check("after_data", call(execute_after_data, 0), 6);

    std::printf("patch0021/execution %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
