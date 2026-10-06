// Compiled beside the unmodified XenonRecomp output for analyse_tail_branch.s.
#define PPC_CONFIG_H_INCLUDED
#include <ppc_context.h>
#include <cstdio>
PPC_FUNC(analyse_tail_branch_C);

int main() {
    alignas(32) uint8_t memory[32]{};
    PPCContext taken{}, fallthrough{};
    taken.r3.s64 = 0;
    fallthrough.r3.s64 = 1;
    analyse_tail_branch_C(taken, memory);
    analyse_tail_branch_C(fallthrough, memory);
    const bool ok = taken.r3.s64 == 77 && fallthrough.r3.s64 == 11;
    std::printf("analyse/tail_execution %s taken=%lld fallthrough=%lld\n",
                ok ? "PASS" : "FAIL", static_cast<long long>(taken.r3.s64),
                static_cast<long long>(fallthrough.r3.s64));
    return ok ? 0 : 1;
}
