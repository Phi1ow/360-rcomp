// CPU test runner (Agent 6). Runs XenonRecomp-generated functions against the
// fixture oracles and prints one JSON object per test on stdout.
//
// Usage: rcomp_cpu_tests [--start N] [--list]
// On a crash (signal) the current test is reported as FAIL/execute and the
// process exits with code 3; the driver restarts at N+1.
//
// Built with RCOMP_HARNESS_LIBRARY, the runner is exported as
// rcomp_cpu_selftest(out, start) for the PS5 test title (no main, results
// written to the given FILE*).
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <initializer_list>
#include <new>

#include "cpu_harness.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/isa.h"
#include "rcomp/runtime_state.h"

namespace {

// Guest pages committed for tests. Xenia fixture semantics: all memory zero
// except MEMORY_IN. Accesses outside these arenas fault and are reported.
struct Arena { uint64_t addr, size; };
const Arena kArenas[] = {{0x00000000ull, 0x20000ull}, {0x10000000ull, 0x20000ull}};

rcomp::GuestMemory g_mem;
sigjmp_buf g_fatal_jmp;
char g_fatal_kind[64];
const char* g_current_id = "";
int g_current_index = -1;
int g_out_fd = 1;

void fatal_hook(rcomp_fatal_kind kind, const char*) {
    snprintf(g_fatal_kind, sizeof g_fatal_kind, "%s", rcomp_fatal_kind_name(kind));
    siglongjmp(g_fatal_jmp, 1);
}

void crash_handler(int sig) {
    char buf[512];
    int n = snprintf(buf, sizeof buf,
                     "{\"id\":\"%s\",\"index\":%d,\"status\":\"FAIL\",\"stage\":\"execute\",\"reason\":\"signal %d\"}\n",
                     g_current_id, g_current_index, sig);
    if (n > 0) (void)!write(g_out_fd, buf, (size_t)n);
    _exit(3);
}

PPCRegister* gpr(PPCContext& c, int n) {
    PPCRegister* t[32] = {&c.r0, &c.r1, &c.r2, &c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10,
                          &c.r11, &c.r12, &c.r13, &c.r14, &c.r15, &c.r16, &c.r17, &c.r18, &c.r19, &c.r20,
                          &c.r21, &c.r22, &c.r23, &c.r24, &c.r25, &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31};
    return t[n];
}
PPCRegister* fpr(PPCContext& c, int n) {
    PPCRegister* t[32] = {&c.f0, &c.f1, &c.f2, &c.f3, &c.f4, &c.f5, &c.f6, &c.f7, &c.f8, &c.f9, &c.f10,
                          &c.f11, &c.f12, &c.f13, &c.f14, &c.f15, &c.f16, &c.f17, &c.f18, &c.f19, &c.f20,
                          &c.f21, &c.f22, &c.f23, &c.f24, &c.f25, &c.f26, &c.f27, &c.f28, &c.f29, &c.f30, &c.f31};
    return t[n];
}
PPCVRegister* vr(PPCContext& c, int n) {
    // v0..v127 are laid out contiguously in PPCContext (checked by static_assert below).
    return &c.v0 + n;
}
static_assert(offsetof(PPCContext, v127) - offsetof(PPCContext, v0) == 127 * sizeof(PPCVRegister),
              "PPCContext vector registers are not contiguous");

PPCCRRegister* crf(PPCContext& c, int n) {
    PPCCRRegister* t[8] = {&c.cr0, &c.cr1, &c.cr2, &c.cr3, &c.cr4, &c.cr5, &c.cr6, &c.cr7};
    return t[n];
}

uint32_t cr_value(PPCContext& c) {
    uint32_t v = 0;
    for (int i = 0; i < 8; ++i) {
        PPCCRRegister* f = crf(c, i);
        uint32_t nib = (f->lt ? 8u : 0u) | (f->gt ? 4u : 0u) | (f->eq ? 2u : 0u) | (f->so ? 1u : 0u);
        v |= nib << (28 - 4 * i);
    }
    return v;
}

// Contract (docs/ARCHITECTURE.md, "Vector register layout"): XenonRecomp keeps
// VR elements in host order, guest word i <-> u32[3 - i].
void set_reg(PPCContext& c, const RegVal& r) {
    int n = atoi(r.reg + 1);
    switch (r.kind) {
    case RK_GPR: gpr(c, n)->u64 = r.u; break;
    case RK_FVAL:
    case RK_FBITS: fpr(c, n)->u64 = r.u; break;
    case RK_VR: for (int i = 0; i < 4; ++i) vr(c, n)->u32[3 - i] = r.w[i]; break;
    case RK_CTR: c.ctr.u64 = r.u; break;
    default: fprintf(stderr, "unsupported input register %s\n", r.reg); abort();
    }
}

struct Mismatch { char text[256]; };

bool check_reg(PPCContext& c, const RegVal& r, char* out, size_t cap) {
    int n = (r.kind == RK_GPR || r.kind == RK_FVAL || r.kind == RK_FBITS || r.kind == RK_VR) ? atoi(r.reg + 1) : 0;
    switch (r.kind) {
    case RK_GPR: {
        uint64_t a = gpr(c, n)->u64;
        if (a == r.u) return true;
        snprintf(out, cap, "%s expected 0x%016llX actual 0x%016llX", r.reg, (unsigned long long)r.u, (unsigned long long)a);
        return false;
    }
    case RK_CTR: {
        if (c.ctr.u64 == r.u) return true;
        snprintf(out, cap, "ctr expected 0x%llX actual 0x%llX", (unsigned long long)r.u, (unsigned long long)c.ctr.u64);
        return false;
    }
    case RK_FVAL: {
        double e, a = fpr(c, n)->f64;
        memcpy(&e, &r.u, 8);
        if (a == e) return true;
        snprintf(out, cap, "%s expected %.17g actual %.17g (0x%016llX)", r.reg, e, a, (unsigned long long)fpr(c, n)->u64);
        return false;
    }
    case RK_FBITS: {
        uint64_t a = fpr(c, n)->u64;
        if (a == r.u) return true;
        snprintf(out, cap, "%s expected bits 0x%016llX actual 0x%016llX", r.reg, (unsigned long long)r.u, (unsigned long long)a);
        return false;
    }
    case RK_VR: {
        PPCVRegister* v = vr(c, n);
        bool ok = true;
        for (int i = 0; i < 4; ++i) ok &= v->u32[3 - i] == r.w[i];
        if (ok) return true;
        snprintf(out, cap, "%s expected [%08X %08X %08X %08X] actual [%08X %08X %08X %08X]", r.reg, r.w[0], r.w[1],
                 r.w[2], r.w[3], v->u32[3], v->u32[2], v->u32[1], v->u32[0]);
        return false;
    }
    case RK_CR: {
        uint32_t a = cr_value(c);
        if (a == (uint32_t)r.u) return true;
        snprintf(out, cap, "cr expected 0x%08X actual 0x%08X", (uint32_t)r.u, a);
        return false;
    }
    case RK_XER_CA:
    case RK_XER_OV:
    case RK_XER_SO: {
        uint8_t a = r.kind == RK_XER_CA ? c.xer.ca : r.kind == RK_XER_OV ? c.xer.ov : c.xer.so;
        if (a == r.u) return true;
        snprintf(out, cap, "%s expected %llu actual %u", r.reg, (unsigned long long)r.u, a);
        return false;
    }
    }
    return false;
}

void json_escape(const char* s, FILE* f) {
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
}

}  // namespace

static int run_corpus(FILE* out, FILE* err, int start, bool list) {
    g_out_fd = fileno(out);

    if (!list) {
        char isa[1024];
        int missing = rcomp::check_isa(isa, sizeof isa);
        fprintf(err, "%s\n", isa);
        if (missing != 0) {
            fprintf(err, "refusing to run: CPU lacks ISA features the corpus was compiled for\n");
            return 4;
        }
        rcomp::MemStatus s = g_mem.reserve();
        if (s != rcomp::MemStatus::Ok) {
            fprintf(err, "guest reserve failed: %s\n", rcomp::mem_status_name(s));
            return 2;
        }
        for (const Arena& a : kArenas) {
            s = g_mem.commit(a.addr, a.size, rcomp::Protect::ReadWrite);
            if (s != rcomp::MemStatus::Ok) {
                fprintf(err, "arena commit failed: %s\n", rcomp::mem_status_name(s));
                return 2;
            }
        }
        rcomp::set_active_guest_memory(&g_mem);
        rcomp_set_fatal_hook(fatal_hook);
        for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP}) signal(sig, crash_handler);
    }

    int index = 0, pass = 0, fail = 0;
    for (int u = 0; u < kUnitCount; ++u) {
        const TestUnit& unit = *kUnits[u];
        for (int k = 0; k < unit.n_cases; ++k, ++index) {
            const TestCase& t = unit.cases[k];
            if (list) { fprintf(out, "%d %s\n", index, t.id); continue; }
            if (index < start) continue;
            // Each unit's functions are registered alone: a test can only
            // reach functions of its own fixture file.
            if (!rcomp::register_functions(unit.funcs, (size_t)unit.n_funcs)) {
                fprintf(err, "function registry rejected %s: invalid address/host, duplicate address, or allocation failure\n", unit.name);
                return 2;
            }
            if (t.program) {
                fprintf(out, "{\"id\":\"%s\",\"index\":%d,\"status\":\"FAIL\",\"stage\":\"harness\","
                        "\"reason\":\"PROGRAM test needs the guest runtime runner (tests/m3)\"}\n", t.id, index);
                ++fail;
                continue;
            }
            g_current_id = t.id;
            g_current_index = index;
            // Crash marker: on PS5 signal handlers do not fire (sigaction
            // reported ineffective on console), so a faulting test only
            // leaves this line; the collector turns it into FAIL/execute.
            fprintf(out, "RCOMP-CPU begin index=%d id=%s\n", index, t.id);
            fflush(out);
            uint8_t* base = g_mem.base();
            for (const Arena& a : kArenas) memset(base + a.addr, 0, a.size);
            for (int m = 0; m < t.n_mem_in; ++m) memcpy(base + t.mem_in[m].addr, t.mem_in[m].bytes, t.mem_in[m].size);

            // Value-initialisation: every register zero, msr keeps its
            // ppc_context.h default (0x200A000).
            alignas(64) static PPCContext ctx;
            ctx = PPCContext{};
            ctx.fpscr.loadFromHost();
            for (int r = 0; r < t.n_in; ++r) set_reg(ctx, t.in[r]);

            char reasons[2048] = "";
            size_t used = 0;
            bool ok = true;
            g_fatal_kind[0] = 0;
            if (sigsetjmp(g_fatal_jmp, 1) == 0) {
                t.fn(ctx, base);
                if (t.expect_fatal) {
                    ok = false;
                    used += snprintf(reasons + used, sizeof reasons - used, "expected fatal %s but returned; ", t.expect_fatal);
                }
            } else {
                if (!t.expect_fatal || strcmp(t.expect_fatal, g_fatal_kind) != 0) {
                    ok = false;
                    used += snprintf(reasons + used, sizeof reasons - used, "unexpected fatal %s; ", g_fatal_kind);
                }
            }
            if (!t.expect_fatal) {
                for (int r = 0; r < t.n_out && used < sizeof reasons - 300; ++r) {
                    char buf[256];
                    if (!check_reg(ctx, t.out[r], buf, sizeof buf)) {
                        ok = false;
                        used += snprintf(reasons + used, sizeof reasons - used, "%s; ", buf);
                    }
                }
                for (int m = 0; m < t.n_mem_out && used < sizeof reasons - 300; ++m) {
                    const MemVal& mv = t.mem_out[m];
                    for (uint32_t b = 0; b < mv.size; ++b) {
                        if (base[mv.addr + b] != mv.bytes[b]) {
                            ok = false;
                            used += snprintf(reasons + used, sizeof reasons - used,
                                             "mem[0x%08X] expected %02X actual %02X; ", mv.addr + b, mv.bytes[b],
                                             base[mv.addr + b]);
                            break;
                        }
                    }
                }
            }
            fprintf(out, "{\"id\":\"%s\",\"index\":%d,\"status\":\"%s\",\"stage\":\"execute\",\"reason\":\"", t.id, index,
                   ok ? "PASS" : "FAIL");
            json_escape(reasons, out);
            fprintf(out, "\"}\n");
            fflush(out);
            ok ? ++pass : ++fail;
        }
    }
    if (!list) fprintf(err, "executed=%d pass=%d fail=%d\n", pass + fail, pass, fail);
    fflush(out);
    return 0;
}

#ifdef RCOMP_HARNESS_LIBRARY
extern "C" int rcomp_cpu_selftest(FILE* out, int start) {
    return run_corpus(out, out, start, false);
}
#else
int main(int argc, char** argv) {
    int start = 0;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--start") && i + 1 < argc) start = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--list")) list = true;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    return run_corpus(stdout, stderr, start, list);
}
#endif
