// Shared interface (owner: PRIME). Fatal diagnostics for the AOT runtime.
//
// Every "cannot continue" condition (unknown indirect target, missing import,
// out-of-range guest access, unimplemented service) ends in rcomp_fatal():
// it prints one machine-parsable line to stderr and terminates the process
// with RCOMP_EXIT_FATAL. Nothing in the runtime is allowed to swallow such a
// condition and return a success code.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum rcomp_fatal_kind {
    RCOMP_FATAL_INDIRECT_TARGET = 1,  // indirect call to an address with no function
    RCOMP_FATAL_MISSING_IMPORT = 2,   // guest import with no HLE implementation
    RCOMP_FATAL_GUEST_ACCESS = 3,     // guest access outside committed memory
    RCOMP_FATAL_UNIMPLEMENTED = 4,    // runtime service known but not implemented
    RCOMP_FATAL_PLATFORM = 5,         // host/PS5 platform call failed
    RCOMP_FATAL_INTERNAL = 6,         // runtime invariant violated
    RCOMP_FATAL_GUEST_TRAP = 7,       // guest tw/twi/td/tdi condition true
};

#define RCOMP_EXIT_FATAL 70

// Line format (stable, parsed by tests/): "RCOMP-FATAL kind=<name> <message>\n"
__attribute__((noreturn, format(printf, 2, 3)))
void rcomp_fatal(enum rcomp_fatal_kind kind, const char* fmt, ...);

const char* rcomp_fatal_kind_name(enum rcomp_fatal_kind kind);

// Test hook: when set, rcomp_fatal() calls it (after printing) instead of
// exiting, so death tests can observe the diagnostic. The hook must not return
// normally (longjmp or _exit). Production code never sets it.
typedef void (*rcomp_fatal_hook_fn)(enum rcomp_fatal_kind kind, const char* message);
void rcomp_set_fatal_hook(rcomp_fatal_hook_fn hook);

#ifdef __cplusplus
}
#endif
