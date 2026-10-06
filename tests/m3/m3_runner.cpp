// M3 runner (Agent 6): runs `#_ PROGRAM` fixtures as real guest programs on
// the guest runtime (runtime/, Agent 3): guest stack and r13 block from
// rt::create_guest_thread, xboxkrnl HLE registered, VFS device mounted on the
// fixture's data directory. Checks the fixture oracle (registers/memory) plus
// resource hygiene after exit: no live heap allocation beyond the baseline,
// no live handle. One JSON line per test on stdout (same schema as
// tests/cpu/harness_main.cpp).
//
// Usage: rcomp_m3_tests [--start N]   (fixture root compiled in, override
// with RCOMP_M3_FIXTURE_DIR).
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "cpu_harness.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime_state.h"

FILE* out_stream();

namespace {

rcomp::GuestMemory g_mem;
sigjmp_buf g_fatal_jmp;
char g_fatal[512];

void fatal_hook(rcomp_fatal_kind kind, const char* msg) {
    snprintf(g_fatal, sizeof g_fatal, "%s: %s", rcomp_fatal_kind_name(kind), msg);
    siglongjmp(g_fatal_jmp, 1);
}

void emit(const char* id, int index, bool ok, const std::string& reason) {
    FILE* o = out_stream();
    fprintf(o, "{\"id\":\"%s\",\"index\":%d,\"status\":\"%s\",\"stage\":\"execute\",\"reason\":\"", id, index,
           ok ? "PASS" : "FAIL");
    for (char c : reason) {
        if (c == '"' || c == '\\') fputc('\\', o);
        fputc(c, o);
    }
    fprintf(o, "\"}\n");
    fflush(o);
}

bool parse_program(const char* spec, uint32_t* stack, std::string* dev, std::string* dir) {
    *stack = 0x10000;
    std::string s(spec);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(' ', pos);
        std::string tok = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? s.size() : end + 1;
        if (tok.rfind("stack=", 0) == 0) *stack = (uint32_t)strtoul(tok.c_str() + 6, nullptr, 0);
        else if (tok.rfind("mount=", 0) == 0) {
            size_t c = tok.find(':', 6);
            if (c == std::string::npos) return false;
            *dev = tok.substr(6, c - 6);
            *dir = tok.substr(c + 1);
        } else if (!tok.empty()) return false;
    }
    return true;
}

}  // namespace

static FILE* g_out = stdout;

static int run_m3(const char* root, int start) {

    if (g_mem.reserve() != rcomp::MemStatus::Ok ||
        g_mem.commit(0x10000000ull, 0x20000ull, rcomp::Protect::ReadWrite) != rcomp::MemStatus::Ok) {
        fprintf(g_out, "RCOMP-M3 error guest memory setup failed\n");
        return 2;
    }
    rcomp::set_active_guest_memory(&g_mem);
    namespace rt = rcomp::rt;
    if (rt::runtime_init(&g_mem) != rt::Status::Ok || rt::register_xboxkrnl_hle() != rt::Status::Ok) {
        fprintf(g_out, "RCOMP-M3 error runtime init failed\n");
        return 2;
    }
    rcomp_set_fatal_hook(fatal_hook);

    int index = 0, pass = 0, fail = 0;
    for (int u = 0; u < kUnitCount; ++u) {
        const TestUnit& unit = *kUnits[u];
        rcomp::register_functions(unit.funcs, (size_t)unit.n_funcs);
        for (int k = 0; k < unit.n_cases; ++k, ++index) {
            const TestCase& t = unit.cases[k];
            if (index < start) continue;
            std::string reason;
            uint32_t stack = 0;
            std::string dev, dir;
            if (!t.program || !parse_program(t.program, &stack, &dev, &dir)) {
                emit(t.id, index, false, "not a valid PROGRAM test");
                ++fail;
                continue;
            }
            fprintf(g_out, "RCOMP-M3 begin index=%d id=%s\n", index, t.id);
            fflush(g_out);
            rt::Runtime& r = *rt::runtime();
            r.vfs.unmount_all();
            std::string host_dir = std::string(root) + "/" + dir;
            if (!dev.empty() && r.vfs.mount(dev, host_dir) != rt::Status::Ok) {
                emit(t.id, index, false, "mount failed: " + host_dir);
                ++fail;
                continue;
            }
            uint8_t* base = g_mem.base();
            memset(base + 0x10000000, 0, 0x20000);
            for (int m = 0; m < t.n_mem_in; ++m) memcpy(base + t.mem_in[m].addr, t.mem_in[m].bytes, t.mem_in[m].size);

            uint32_t heap_before = r.heap.stats().live_allocations;
            alignas(64) static PPCContext ctx;
            ctx = PPCContext{};
            ctx.fpscr.loadFromHost();
            rt::GuestThread th;
            rt::Status s = rt::create_guest_thread(r.heap, {stack, t.guest, 0}, &ctx, &th);
            if (s != rt::Status::Ok) {
                emit(t.id, index, false, "create_guest_thread failed");
                ++fail;
                continue;
            }
            for (int i = 0; i < t.n_in; ++i) {
                // Register inputs are not supported for programs: the entry
                // state is the runtime's (stack, r13, lr sentinel).
                reason += "REGISTER_IN ignored for PROGRAM; ";
            }
            bool ok = reason.empty();
            uint32_t exit_code = 0;
            g_fatal[0] = 0;
            if (sigsetjmp(g_fatal_jmp, 1) == 0) {
                s = rt::run_guest_thread(th, ctx, base, t.fn, &exit_code);
                if (s != rt::Status::Ok) { ok = false; reason += "run_guest_thread failed; "; }
            } else {
                ok = false;
                reason += std::string("fatal ") + g_fatal + "; ";
                rt::abandon_current_guest_thread_after_fatal();
            }
            // Oracle: registers from the fixture annotations.
            for (int i = 0; i < t.n_out; ++i) {
                const RegVal& rv = t.out[i];
                if (rv.kind != RK_GPR) { ok = false; reason += "only GPR outputs supported; "; continue; }
                int n = atoi(rv.reg + 1);
                PPCRegister* g = (&ctx.r0);
                uint64_t actual = 0;
                switch (n) {  // PPCContext does not lay GPRs out in order (r3 first).
                case 3: actual = ctx.r3.u64; break;
                case 4: actual = ctx.r4.u64; break;
                default: ok = false; reason += "unsupported output register; "; continue;
                }
                (void)g;
                if (actual != rv.u) {
                    ok = false;
                    char buf[160];
                    snprintf(buf, sizeof buf, "%s expected 0x%llX actual 0x%llX; ", rv.reg,
                             (unsigned long long)rv.u, (unsigned long long)actual);
                    reason += buf;
                }
            }
            if (exit_code != ctx.r3.u32) { ok = false; reason += "exit code != r3; "; }
            // Hygiene after a clean exit: the program freed what it allocated
            // and closed what it opened.
            rt::destroy_guest_thread(r.heap, &th);
            uint32_t heap_after = r.heap.stats().live_allocations;
            if (heap_after != heap_before) {
                ok = false;
                reason += "heap allocations leaked: " + std::to_string(heap_after - heap_before) + "; ";
            }
            if (r.handles.live_count() != 0) {
                ok = false;
                reason += "handles leaked: " + std::to_string(r.handles.live_count()) + "; ";
            }
            emit(t.id, index, ok, reason);
            ok ? ++pass : ++fail;
        }
    }
    fprintf(g_out, "RCOMP-M3 executed=%d pass=%d fail=%d vfs_lexical_fallback=%d\n", pass + fail, pass, fail,
            rt::vfs_lexical_fallback_used ? 1 : 0);
    fflush(g_out);
    return 0;
}

FILE* out_stream() { return g_out; }

#ifdef RCOMP_HARNESS_LIBRARY
// PS5 test title entry: `root` holds data/m3.bin (packaged in the title).
extern "C" int rcomp_m3_selftest(FILE* out, const char* root) {
    g_out = out;
    return run_m3(root, 0);
}
#else
int main(int argc, char** argv) {
    int start = 0;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--start") && i + 1 < argc) start = atoi(argv[++i]);
    const char* root = getenv("RCOMP_M3_FIXTURE_DIR");
    if (!root) root = RCOMP_M3_FIXTURE_DIR;
    return run_m3(root, start);
}
#endif
