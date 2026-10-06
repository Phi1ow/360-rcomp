// Shared interface (owner: PRIME; implementation cpu/runtime/isa_check.cpp).
#pragma once
#include <stddef.h>

namespace rcomp {
// Writes a JSON object describing required/present ISA features into
// `report`. Returns the number of required features missing (0 = OK), -1 if
// CPUID is unusable. Callers must refuse to run generated code if != 0.
int check_isa(char* report, size_t cap);
}  // namespace rcomp
