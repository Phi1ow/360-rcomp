// Original PPC examples: no title bytes. Exercise the discovery boundaries
// independently of generated code (patch 0009). Host-only evidence.
#include <function.h>
#include <cstdint>
#include <byteswap.h>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

static uint32_t be(uint32_t word) { return ByteSwap(word); }
static uint32_t bc(int offset) { return be(0x41820000u | (uint32_t(offset) & 0xfffcu)); }
static uint32_t b(int offset) { return be(0x48000000u | (uint32_t(offset) & 0x03fffffcu)); }
static uint32_t blr() { return be(0x4e800020u); }
static uint32_t nop() { return be(0x60000000u); }
static int fails = 0;

static void check(const char* name, const uint32_t* code, size_t size, size_t base) {
    const auto fn = Function::Analyze(code, size, base);
    bool ok = fn.size == size;
    for (const auto& block : fn.blocks)
        ok = ok && block.base <= size && block.size <= size - block.base;
    std::printf("analyse/%s %s size=%zu blocks=%zu\n", name, ok ? "PASS" : "FAIL", fn.size, fn.blocks.size());
    fails += !ok;
}

int main(int argc, char** argv) {
    // Conditional branch to code preceding this function used to chase a
    // cycle forever, growing both the block vector and traversal stack.
    if (argc == 1 || std::strcmp(argv[1], "cycle") == 0) {
        const uint32_t words[] = {bc(8), nop(), bc(-8), blr()};
        check("conditional_tail_cycle", words + 2, 8, 0x1008);
        if (argc > 1) return fails ? 1 : 0;
    }
    {
        const uint32_t words[] = {bc(16), blr(), nop(), nop(), blr()};
        check("conditional_forward_tail", words, 8, 0x2000);
    }
    {
        const uint32_t words[] = {b(16), nop(), nop(), nop(), blr()};
        check("unconditional_forward_tail", words, 4, 0x3000);
    }
    {
        const uint32_t words[] = {nop(), bc(-4), blr()};
        check("internal_backedge", words, sizeof words, 0x4000);
    }
    {
        const uint32_t words[] = {bc(8), nop(), blr()};
        check("internal_forward", words, sizeof words, 0x5000);
    }
    {
        const uint32_t words[] = {nop(), b(-4)};
        check("internal_unconditional_loop", words, sizeof words, 0x6000);
    }
    {
        const uint32_t words[] = {bc(4), blr()};
        check("shared_taken_and_fallthrough", words, sizeof words, 0x7000);
    }
    {
        const uint32_t words[] = {be(0x4c820020u), blr()}; // bnelr; blr
        check("conditional_return", words, sizeof words, 0x8000);
    }
    // A readable word directly before a protected page proves neither the
    // shifted-tail lookahead nor the loop reads past the exclusive bound.
    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    auto* memory = static_cast<uint8_t*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (memory == MAP_FAILED || mprotect(memory + page, page, PROT_NONE) != 0) {
        std::puts("analyse/guard_page FAIL");
        return 1;
    }
    auto* last = reinterpret_cast<uint32_t*>(memory + page - 4);
    *last = nop();
    check("exclusive_end", last, 4, 0x9000);
    *last = blr();
    check("single_return_lookahead", last, 4, 0xa000);
    munmap(memory, page * 2);
    return fails ? 1 : 0;
}
