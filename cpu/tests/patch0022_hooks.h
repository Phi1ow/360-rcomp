// Test doubles for the runtime hooks the generated code of patch0022_*.s reaches
// (tests only; the title links cpu/runtime/indirect.cpp). Force-included after
// rcomp/ppc_prelude.h by run_patch0022_regression.py.
#pragma once
#include <cstdint>

struct PPCContext;
[[noreturn]] void TESTDOUBLE_guest_trap(uint32_t address);
void TESTDOUBLE_call_indirect(PPCContext& ctx, uint8_t* base, uint32_t target);
[[noreturn]] void TESTDOUBLE_unresolved_import(const char* module, uint32_t ordinal);

#undef PPC_TRAP
#define PPC_TRAP(address) TESTDOUBLE_guest_trap((uint32_t)(address))
#undef PPC_CALL_INDIRECT_FUNC
#define PPC_CALL_INDIRECT_FUNC(x) TESTDOUBLE_call_indirect(ctx, base, (uint32_t)(x))
#undef PPC_UNRESOLVED_IMPORT
#define PPC_UNRESOLVED_IMPORT(module, ordinal) TESTDOUBLE_unresolved_import(module, (uint32_t)(ordinal))
