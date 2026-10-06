// Pulls in XenonRecomp's PPCContext exactly like the generated code does
// (owner: Agent 3, runtime/).
//
// ABI WARNING: PPCContext's layout depends on the PPC_CONFIG_* macros set by
// the title's ppc_config.h (e.g. PPC_CONFIG_NON_ARGUMENT_AS_LOCAL removes
// r0/r2/r11/r12/f0 from the struct). The runtime is compiled with the
// upstream default (no PPC_CONFIG_* flag), like cpu/runtime/indirect.cpp.
// Generated code MUST be compiled with the same configuration; see the
// static_asserts in src/guest_context_check.cpp and the interface proposal in
// the Agent 3 report (shared include/rcomp/ppc_config.h).
#pragma once

#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED
#endif
#include "rcomp/ppc_prelude.h"
#include <ppc_context.h>

#include <stddef.h>
namespace rcomp::rt {
size_t ppc_context_size_seen_by_runtime();  // src/guest_context_check.cpp
}
