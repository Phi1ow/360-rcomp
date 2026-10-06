// Independent original oracles for generated PPC; host evidence only.
#include "instruction_decls.h"
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static_assert(sizeof(PPCFPSCRRegister) == 4, "FPSCR ABI");
static_assert(sizeof(PPCContext) == 2688, "context ABI");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
static_assert(offsetof(PPCContext, f1) == 336 && offsetof(PPCContext, v0) == 592, "register ABI");
#pragma clang diagnostic pop
static int failed = 0;
[[noreturn]] void TESTDOUBLE_guest_trap(uint32_t) { _exit(70); }
static void verdict(const char* name, bool pass) {
    std::printf("instructions/%s %s\n", name, pass ? "PASS" : "FAIL");
    failed += !pass;
}
alignas(64) static uint8_t scratch[256]{};

int main(int argc, char** argv) {
    const uint32_t savedHost = simde_mm_getcsr();
    if (argc == 2) {
        FILE* input = std::fopen(argv[1], "r");
        if (!input) return 2;
        unsigned long long bits;
        unsigned mode;
        while (std::fscanf(input, "%llx %u", &bits, &mode) == 2) {
            PPCContext ctx{};
            ctx.fpscr.storeFromGuest(mode);
            ctx.fpscr.enableFlushModeUnconditional();
            ctx.f2.u64 = bits;
            execute_estimate(ctx, scratch);
            std::printf("%016llx %u %016llx\n", bits, mode, (unsigned long long)ctx.f1.u64);
        }
        std::fclose(input);
        simde_mm_setcsr(savedHost);
        return 0;
    }
    struct Special { uint64_t input, output; uint32_t fpscr; };
    const Special specials[] = {
        {0, 0x7ff0000000000000ull, 0x84005000},
        {0x8000000000000000ull, 0xfff0000000000000ull, 0x84009000},
        {0x7ff0000000000000ull, 0, 0x00002000},
        {0xfff0000000000000ull, 0x7ff8000000000000ull, 0xa0011200},
        {0xbff0000000000000ull, 0x7ff8000000000000ull, 0xa0011200},
        {0x7ff0000000000042ull, 0x7ff8000000000042ull, 0xa1011000},
        {0xfff0000000000042ull, 0xfff8000000000042ull, 0xa1011000},
        {0x7ff8000000000042ull, 0x7ff8000000000042ull, 0x00011000},
        {0xfff8000000000042ull, 0xfff8000000000042ull, 0x00011000},
        {0x4010000000000000ull, 0x3fe0000000000000ull, 0x00004000},
    };
    bool specialOk = true, recordOk = true, aliasOk = true, errnoOk = true;
    for (const auto& test : specials) {
        PPCContext ctx{};
        ctx.fpscr.storeFromGuest(0);
        ctx.f2.u64 = test.input;
        ctx.cr1 = {7, 6, 5, 4};
        errno = ERANGE;
        execute_estimate(ctx, scratch);
        errnoOk &= errno == ERANGE;
        specialOk &= ctx.f1.u64 == test.output && ctx.fpscr.loadFromHost() == test.fpscr &&
            ctx.cr1.lt == 7 && ctx.cr1.gt == 6 && ctx.cr1.eq == 5 && ctx.cr1.so == 4;
        ctx = PPCContext{};
        ctx.fpscr.storeFromGuest(0);
        ctx.f2.u64 = test.input;
        execute_estimate_record(ctx, scratch);
        recordOk &= ctx.f1.u64 == test.output && ctx.cr1.lt == ((test.fpscr >> 31) & 1) &&
            ctx.cr1.gt == ((test.fpscr >> 30) & 1) && ctx.cr1.eq == ((test.fpscr >> 29) & 1) &&
            ctx.cr1.so == ((test.fpscr >> 28) & 1);
        ctx = PPCContext{};
        ctx.fpscr.storeFromGuest(0);
        ctx.f2.u64 = test.input;
        execute_estimate_alias(ctx, scratch);
        aliasOk &= ctx.f2.u64 == test.output;
    }
    verdict("frsqrte_specials_flags_and_unchanged_cr1", specialOk);
    verdict("frsqrte_record_cr1", recordOk);
    verdict("frsqrte_source_destination_alias", aliasOk);
    verdict("frsqrte_errno_preserved", errnoOk);
    bool suppressOk = true;
    for (const auto& test : specials) {
        if (!(test.fpscr & 0x84000000u)) continue;
        PPCContext ctx{};
        const uint32_t enable = (test.fpscr & 0x04000000u) ? 0x10 : 0x80;
        ctx.fpscr.storeFromGuest(enable | 0x12000);
        ctx.f1.u64 = 0x1122334455667788ull;
        ctx.f2.u64 = test.input;
        execute_estimate_record(ctx, scratch);
        suppressOk &= ctx.f1.u64 == 0x1122334455667788ull &&
            (ctx.fpscr.csr & 0x1f000) == 0x12000 && ctx.cr1.gt == 1;
    }
    verdict("frsqrte_enabled_exceptions_preserve_result", suppressOk);
    {
        PPCContext ctx{};
        ctx.msr |= 0x900;
        ctx.fpscr.storeFromGuest(0x80);
        ctx.f2.f64 = 4.0;
        execute_estimate(ctx, scratch);
        const bool normal = ctx.f1.f64 == 0.5;
        const pid_t child = fork();
        if (child == 0) {
            ctx.f2.f64 = -1.0;
            execute_estimate(ctx, scratch);
            _exit(88);
        }
        int status = 0;
        const bool waited = child > 0 && waitpid(child, &status, 0) == child;
        verdict("frsqrte_msr_enabled_exception_traps", normal && waited && WIFEXITED(status) && WEXITSTATUS(status) == 70);
    }
    bool fpscrOk = true;
    for (unsigned mode = 0; mode < 4; ++mode) {
        PPCContext ctx{};
        ctx.f2.u64 = 0x84005000;
        execute_write_fpscr(ctx, scratch);
        ctx.f2.u64 = mode;
        execute_write_rounding_only(ctx, scratch);
        ctx.fpscr.enableFlushModeUnconditional();
        ctx.fpscr.disableFlushModeUnconditional();
        execute_read_fpscr(ctx, scratch);
        fpscrOk &= ctx.f1.u64 == (0x84005000u | mode) &&
            (simde_mm_getcsr() & PPCFPSCRRegister::RoundMask) == PPCFPSCRRegister::GuestToHost[mode];
        execute_read_fpscr_record(ctx, scratch);
        fpscrOk &= ctx.cr1.lt == 1 && ctx.cr1.gt == 0 && ctx.cr1.eq == 0 && ctx.cr1.so == 0;
    }
    verdict("fpscr_mask_rounding_flush_and_mffs_neighbors", fpscrOk);

    {
        PPCContext ctx{};
        ctx.f2.u64 = 0x88004003; // FX, UX, positive-normal FPRF, RN=3.
        execute_write_fpscr(ctx, scratch);
        ctx.f2.u64 = 0x04000010; // Replace only cause nibble and enable nibble.
        ctx.cr1 = {7, 6, 5, 4};
        execute_write_fpscr_partial_record(ctx, scratch);
        execute_read_fpscr(ctx, scratch);
        // UX clears, ZX+ZE set FEX; FX, FPRF and RN are preserved.
        verdict("mtfsf_partial_record_preserves_other_fields", ctx.f1.u64 == 0xc4004013 &&
            ctx.cr1.lt == 1 && ctx.cr1.gt == 1 && ctx.cr1.eq == 0 && ctx.cr1.so == 0 &&
            (simde_mm_getcsr() & PPCFPSCRRegister::RoundMask) == PPCFPSCRRegister::GuestToHost[3]);
    }

    bool vectorsOk = true;
    for (unsigned round = 0; round < 256; ++round) {
        PPCContext ctx{};
        uint8_t a[16], b[16], mask[16], expected[16];
        for (unsigned j = 0; j < 16; ++j) {
            a[j] = uint8_t(round * 31 + j * 7);
            b[j] = uint8_t(round * 17 + j * 13);
            mask[j] = uint8_t(round + j * 19);
            expected[j] = uint8_t((a[j] & ~mask[j]) | (b[j] & mask[j]));
        }
        std::memcpy(ctx.v2.u8, a, 16); std::memcpy(ctx.v3.u8, b, 16); std::memcpy(ctx.v4.u8, mask, 16);
        execute_select_standard(ctx, scratch);
        vectorsOk &= std::memcmp(ctx.v1.u8, expected, 16) == 0;
        std::memcpy(ctx.v64.u8, a, 16); std::memcpy(ctx.v95.u8, b, 16); std::memcpy(ctx.v127.u8, mask, 16);
        execute_select_high(ctx, scratch);
        vectorsOk &= std::memcmp(ctx.v127.u8, expected, 16) == 0;
        for (unsigned j = 0; j < 16; ++j) expected[j] = uint8_t(a[j] & b[j]);
        execute_select_alias_a(ctx, scratch);
        vectorsOk &= std::memcmp(ctx.v64.u8, expected, 16) == 0;
        std::memcpy(ctx.v64.u8, a, 16); std::memcpy(ctx.v95.u8, b, 16);
        for (unsigned j = 0; j < 16; ++j) expected[j] = uint8_t(a[j] | b[j]);
        execute_select_alias_b(ctx, scratch);
        vectorsOk &= std::memcmp(ctx.v95.u8, expected, 16) == 0;
    }
    verdict("vsel128_bits_high_registers_aliases_and_vsel_neighbor", vectorsOk);

    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    auto* memory = static_cast<uint8_t*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (memory == MAP_FAILED || mprotect(memory + page, page, PROT_NONE)) return 2;
    PPCContext ctx{};
    ctx.r0.u64 = 0xdeadbeef; ctx.r4.u64 = 32;
    std::memset(memory, 0x59, page);
    execute_cache_zero_base(ctx, memory);
    ctx.r3.u64 = 0xfffffff0; ctx.r4.u64 = 0x30;
    execute_cache_indexed(ctx, memory); // 32-bit EA wraps to 0x20.
    bool cacheOk = true;
    for (size_t i = 0; i < page; ++i) cacheOk &= memory[i] == 0x59;
    verdict("dcbst_zero_ra_wrapped_ea_preserves_memory", cacheOk);
    std::atomic<unsigned> ready{0}, done{0};
    bool published = true;
    std::thread consumer([&] {
        for (unsigned i = 1; i <= 2000; ++i) {
            while (ready.load(std::memory_order_acquire) != i) std::this_thread::yield();
            uint32_t value; std::memcpy(&value, memory + 32, 4);
            published &= __builtin_bswap32(value) == i;
            done.store(i, std::memory_order_release);
        }
    });
    ctx.r4.u64 = 32;
    for (unsigned i = 1; i <= 2000; ++i) {
        ctx.r3.u64 = i;
        execute_cache_publish(ctx, memory);
        ready.store(i, std::memory_order_release);
        while (done.load(std::memory_order_acquire) != i) std::this_thread::yield();
    }
    consumer.join();
    execute_barrier_sync(ctx, memory); execute_barrier_light(ctx, memory); execute_barrier_io(ctx, memory);
    verdict("dcbst_store_sync_cpu_publication", published);
    const pid_t child = fork();
    if (child == 0) {
        ctx.r4.u64 = page;
        execute_cache_zero_base(ctx, memory);
        _exit(88);
    }
    int status = 0;
    const bool waited = child > 0 && waitpid(child, &status, 0) == child;
    verdict("dcbst_protected_address_must_fault", waited && WIFSIGNALED(status) &&
            (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS));
    munmap(memory, page * 2);
    simde_mm_setcsr(savedHost);
    return failed ? 1 : 0;
}
