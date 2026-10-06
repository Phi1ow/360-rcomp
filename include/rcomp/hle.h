// Shared interface (owner: PRIME; implementation: runtime/, Agent 3).
//
// Import/HLE contract between recompiled guest code and the runtime.
//
// XenonRecomp emits calls to imported functions as ordinary PPC_FUNC calls to
// a symbol named after the import (e.g. __imp__NtAllocateVirtualMemory). The
// runtime provides those symbols. Each implementation receives the guest
// context (arguments in r3..r10 / f1..f13, result in r3/f1) and the guest
// memory base, exactly like a recompiled function.
//
// Rules:
//  * An import with no implementation is bound to a trap that calls
//    rcomp::hle_missing_import(), which prints module, ordinal/symbol, the
//    guest caller (ctx.lr) and r3..r6, then ends in rcomp_fatal().
//  * No implementation returns a success status without doing the work.
//  * Test doubles are only compiled into test binaries and carry the
//    "TESTDOUBLE_" prefix in their registry name.
#pragma once

#include <stdint.h>

struct PPCContext;

namespace rcomp {

struct ImportId {
    const char* module;  // e.g. "xboxkrnl.exe"
    uint32_t ordinal;    // 0 if resolved by name only
    const char* name;    // symbolic name, may be null
};

[[noreturn]] void hle_missing_import(const ImportId& id, PPCContext& ctx);

// Guest stack: distinct from the native stack. Allocated in guest memory,
// r1 points at its top (16-byte aligned, with the 0x50-byte PPC ABI frame
// reserved above it by the caller convention).
struct GuestThreadInit {
    uint32_t stack_size;      // bytes, multiple of 64 KiB
    uint32_t entry;           // guest address of the entry function
    uint32_t r3;              // first argument
};

}  // namespace rcomp
