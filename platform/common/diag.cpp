#include "rcomp/diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static rcomp_fatal_hook_fn g_hook = nullptr;

extern "C" void rcomp_set_fatal_hook(rcomp_fatal_hook_fn hook) { g_hook = hook; }

extern "C" const char* rcomp_fatal_kind_name(enum rcomp_fatal_kind kind) {
    switch (kind) {
    case RCOMP_FATAL_INDIRECT_TARGET: return "indirect_target";
    case RCOMP_FATAL_MISSING_IMPORT: return "missing_import";
    case RCOMP_FATAL_GUEST_ACCESS: return "guest_access";
    case RCOMP_FATAL_UNIMPLEMENTED: return "unimplemented";
    case RCOMP_FATAL_PLATFORM: return "platform";
    case RCOMP_FATAL_INTERNAL: return "internal";
    case RCOMP_FATAL_GUEST_TRAP: return "guest_trap";
    }
    return "unknown";
}

extern "C" void rcomp_fatal(enum rcomp_fatal_kind kind, const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "RCOMP-FATAL kind=%s %s\n", rcomp_fatal_kind_name(kind), msg);
    fflush(stderr);
    fflush(stdout);
    if (g_hook) g_hook(kind, msg);
    _Exit(RCOMP_EXIT_FATAL);
}
