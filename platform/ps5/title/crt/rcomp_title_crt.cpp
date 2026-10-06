// R-comp PS5 title entry point (owner: Agent 2).
//
// Written for this project. Facts it relies on (from the PS5_RetroArch
// title pipeline, documented in platform/ps5/README.md; NOT TESTED here):
//   * the loader enters `_start(params, teardown)`: `params` points at the
//     process parameter block {int argc; <pad>; char* argv[]...} (argv at +8),
//     `teardown` is an optional loader callback to run at exit;
//   * the loader does not zero the tail of the writable segment (.bss), so it
//     is cleared here before any C/C++ code can observe a static;
//   * libSceLibcInternal's `_init_env(params)` must run before libc is used;
//   * static constructors are not run by the loader: the init arrays are.
//
// Built with -fno-builtin so the BSS clear is not turned into a libc call.
#include <stddef.h>
#include <stdint.h>

int main(int argc, char** argv);

extern "C" {
typedef void (*rcomp_fn)(void);

void _init_env(void* params);
int atexit(rcomp_fn fn);
[[noreturn]] void exit(int status);

extern char __rcomp_bss_begin[];
extern char __rcomp_bss_end[];
extern rcomp_fn __rcomp_preinit_array_begin[] __attribute__((weak));
extern rcomp_fn __rcomp_preinit_array_end[] __attribute__((weak));
extern rcomp_fn __rcomp_init_array_begin[] __attribute__((weak));
extern rcomp_fn __rcomp_init_array_end[] __attribute__((weak));
extern rcomp_fn __rcomp_fini_array_begin[] __attribute__((weak));
extern rcomp_fn __rcomp_fini_array_end[] __attribute__((weak));
}

namespace {

void clear_bss() {
    // rep stosb: no dependency on libc, no vectorisation surprises.
    size_t n = (size_t)(__rcomp_bss_end - __rcomp_bss_begin);
    void* dst = __rcomp_bss_begin;
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(0) : "memory");
}

void run_array(rcomp_fn* b, rcomp_fn* e) {
    if (!b || !e) return;
    for (; b < e; ++b)
        if (*b) (*b)();
}

void run_fini() {
    static bool done = false;
    if (done) return;
    done = true;
    rcomp_fn* b = __rcomp_fini_array_begin;
    rcomp_fn* e = __rcomp_fini_array_end;
    if (!b || !e) return;
    while (e > b) {
        --e;
        if (*e) (*e)();
    }
}

}  // namespace

// DT_INIT / DT_FINI. The ELF->PS5 conversion always writes both tags; with no
// _init/_fini in the PIE they were 0, and libSceLibcInternal's exit() called
// the module's DT_FINI = image base + 0 (an int3: SIGTRAP at 0x400001 in the
// PS5 run of kit f414844). lld sets the tags from these names. _start runs the
// init arrays itself, so _init has nothing left to do; _fini runs the fini
// arrays once (run_fini is also registered with atexit).
extern "C" __attribute__((used)) void _init(void) {}
extern "C" __attribute__((used)) void _fini(void) { run_fini(); }

extern "C" [[noreturn]] __attribute__((used, section(".text.rcomp_entry"))) void
_start(void* params, rcomp_fn teardown) {
    clear_bss();
    _init_env(params);
    if (teardown) atexit(teardown);
    atexit(run_fini);
    run_array(__rcomp_preinit_array_begin, __rcomp_preinit_array_end);
    run_array(__rcomp_init_array_begin, __rcomp_init_array_end);

    int argc = params ? *(const int*)params : 0;
    char** argv = params ? (char**)((uint8_t*)params + 8) : nullptr;
    exit(main(argc, argv));
}

// FreeBSD's assert() target (<assert.h>: __assert(func, file, line, expr)).
// libc++abi and libunwind reference it; the title's libc module does not
// export it. Report and abort, exactly like the libc implementation.
extern "C" {
int fprintf(void* stream, const char* fmt, ...);
extern void* __stderrp;
[[noreturn]] void abort(void);
[[noreturn]] void __assert(const char* func, const char* file, int line, const char* expr) {
    fprintf(__stderrp, "Assertion failed: (%s), function %s, file %s, line %d.\n", expr ? expr : "?",
            func ? func : "?", file ? file : "?", line);
    abort();
}
}
