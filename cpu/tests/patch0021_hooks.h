// Test doubles for the runtime hooks the generated code of patch0021_original.s
// reaches (tests only; the title links cpu/runtime/indirect.cpp). Force-included
// after rcomp/ppc_prelude.h by run_patch0021_regression.py.
#pragma once
#include <cstdint>

struct PPCContext;
[[noreturn]] void TESTDOUBLE_guest_trap(uint32_t address);
[[noreturn]] void TESTDOUBLE_switch_out_of_range(uint32_t guest_pc, uint32_t index);
void TESTDOUBLE_call_indirect(PPCContext& ctx, uint8_t* base, uint32_t target);

#undef PPC_TRAP
#define PPC_TRAP(address) TESTDOUBLE_guest_trap((uint32_t)(address))
#undef PPC_SWITCH_OUT_OF_RANGE
#define PPC_SWITCH_OUT_OF_RANGE(guest_pc, index) TESTDOUBLE_switch_out_of_range((uint32_t)(guest_pc), (uint32_t)(index))
#undef PPC_CALL_INDIRECT_FUNC
#define PPC_CALL_INDIRECT_FUNC(x) TESTDOUBLE_call_indirect(ctx, base, (uint32_t)(x))
