// Shared interface (owner: PRIME; implementation: cpu/, Agent 1).
//
// Guest-address -> host-function registry used by indirect calls.
//
// Two sources are supported:
//  * a full XenonRecomp build (TOML mode) exposes PPCFuncMappings[] and the
//    PPC_CODE_BASE/PPC_CODE_SIZE/PPC_IMAGE_BASE/PPC_IMAGE_SIZE macros; the
//    runtime copies the mappings into the in-guest table at
//    PPC_IMAGE_BASE + PPC_IMAGE_SIZE (upstream layout, 8 bytes per 4-byte
//    instruction slot) and registers the range here;
//  * test corpora (XenonRecomp test mode) have no PPCFuncMappings, so the
//    generated harness registers each (guest address, function) pair.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct PPCContext;
typedef void PPCFunc(struct PPCContext& __restrict__ ctx, uint8_t* base);

namespace rcomp {

struct FuncEntry {
    uint32_t guest;
    PPCFunc* host;
    const char* name;  // may be null
};

// Registers a sorted-or-unsorted set of functions (copied). Replaces any
// previous registration. Returns false on duplicate/zero/misaligned addresses,
// null host functions, invalid input or allocation failure, preserving the old table.
bool register_functions(const FuncEntry* entries, size_t count);
// nullptr if no function starts at `guest`.
PPCFunc* lookup_function(uint32_t guest);
const char* function_name(uint32_t guest);
void clear_functions();

}  // namespace rcomp
