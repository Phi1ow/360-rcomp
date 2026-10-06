// Original fixture only. No runtime stubs and no edits to generated C++.
#include <ppc_recomp_shared.h>
#include <switch_decls.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::fseek(f, 0, SEEK_END);
    const size_t bytes = size_t(std::ftell(f));
    std::rewind(f);
    const size_t capacity = size_t(1) << 32;
    auto* base = static_cast<uint8_t*>(mmap(nullptr, capacity, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (base == MAP_FAILED || bytes > PPC_IMAGE_SIZE ||
        mprotect(base + PPC_IMAGE_BASE, PPC_IMAGE_SIZE, PROT_READ | PROT_WRITE)) return 2;
    if (std::fread(base + PPC_IMAGE_BASE, 1, bytes, f) != bytes) return 2;
    std::fclose(f);
    const uint32_t indices[] = {0, 1, 2, 3, 4, 5, 255, 0x80000000, 0xffffffff};
    int failures = 0;
    for (unsigned mode = 0; mode < 2; ++mode) {
        for (const auto index : indices) {
            PPCContext ctx{};
            ctx.r3.u64 = index;
            ctx.r4.u64 = mode;
            _xstart(ctx, base);
            const uint32_t expected = mode ? ((index <= 2) ? 77 : 88)
                                           : ((index <= 3) ? 10 * (index + 1) : 99);
            const bool ok = ctx.r3.u32 == expected;
            std::printf("switch/execute %s mode=%u index=%u got=%u expected=%u\n",
                        ok ? "PASS" : "FAIL", mode, index, ctx.r3.u32, expected);
            failures += !ok;
        }
    }
    PPCContext ctx{};
    execute_boundary(ctx, base);
    const bool boundary_ok = ctx.r3.u32 == 123;
    std::printf("switch/authoritative_boundary %s\n", boundary_ok ? "PASS" : "FAIL");
    munmap(base, capacity);
    return failures || !boundary_ok ? 1 : 0;
}
