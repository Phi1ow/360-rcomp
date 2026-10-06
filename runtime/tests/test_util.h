// Minimal test helpers for runtime/tests (plain C++, exit code != 0 on failure).
#pragma once

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "rcomp/diag.h"

static int g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        auto _va = (a);                                                                    \
        auto _vb = (b);                                                                    \
        if (!(_va == _vb)) {                                                               \
            fprintf(stderr, "CHECK_EQ FAILED %s:%d: %s (0x%llx) != %s (0x%llx)\n", __FILE__, \
                    __LINE__, #a, (unsigned long long)_va, #b, (unsigned long long)_vb);   \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

#define CHECK_ST(expr, expected)                                                         \
    do {                                                                                 \
        auto _s = (expr);                                                                \
        if (_s != (expected)) {                                                          \
            fprintf(stderr, "CHECK_ST FAILED %s:%d: %s -> %s, expected %s\n", __FILE__,  \
                    __LINE__, #expr, rcomp::rt::status_name(_s),                         \
                    rcomp::rt::status_name(expected));                                   \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

// Fatal capture: rcomp_fatal() prints its line, then calls the hook, which
// longjmps back here.
static jmp_buf g_fatal_jb;
static int g_fatal_kind = 0;
static std::string g_fatal_msg;
[[maybe_unused]] static void test_fatal_hook(enum rcomp_fatal_kind kind, const char* message) {
    g_fatal_kind = (int)kind;
    g_fatal_msg = message;
    longjmp(g_fatal_jb, 1);
}

// Runs `stmt`; true if it ended in rcomp_fatal (kind/message captured).
#define CAPTURE_FATAL(stmt, out_bool)            \
    do {                                         \
        g_fatal_kind = 0;                        \
        g_fatal_msg.clear();                     \
        rcomp_set_fatal_hook(test_fatal_hook);   \
        if (setjmp(g_fatal_jb) == 0) {           \
            stmt;                                \
            out_bool = false;                    \
        } else {                                 \
            out_bool = true;                     \
        }                                        \
        rcomp_set_fatal_hook(nullptr);           \
    } while (0)

[[maybe_unused]] static int test_result(const char* name) {
    if (g_failures) {
        fprintf(stderr, "%s: FAIL (%d)\n", name, g_failures);
        return 1;
    }
    printf("%s: PASS\n", name);
    return 0;
}
