// Synthetic XEX title runner (Agent 6 / M6 rehearsal without game content).
//
// Chain under test: fixtures/xex/rcomp_title.s -> cpu/tools/mkxex.py (XEX2)
// -> XenonRecomp XEX/TOML mode (patched) -> this binary: the runtime loads the
// XEX image into guest memory, registers PPCFuncMappings for indirect calls,
// and runs the entry point (_xstart) on a guest thread with xboxkrnl HLE.
// Oracles (fixtures/xex/rcomp_title.s header, computed by hand):
//   mode 0: r3 = 0x6D (sum 1..8 = 36, x3 = 108, +1 = 109), no heap/handle leak
//   mode 1: ends in RCOMP-FATAL kind=missing_import module=xbdm_xex ordinal=0x0012
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "ppc_recomp_shared.h"
#include "rcomp/diag.h"
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xex_loader.h"
#include "rcomp/runtime_state.h"

namespace {

rcomp::GuestMemory g_mem;
sigjmp_buf g_jmp;
std::string g_fatal;
FILE* g_out = stdout;
int g_fails = 0;
// First word of the test double behind XboxKrnlVersion (arbitrary test data).
constexpr uint32_t kTestKrnlVersionWord = 0x00020000;

struct ModeCase {
    uint32_t mode;
    const char* id;
    uint64_t expect_r3;  // for modes that return normally
};

void fatal_hook(rcomp_fatal_kind kind, const char* msg) {
    g_fatal = std::string(rcomp_fatal_kind_name(kind)) + ": " + msg;
    siglongjmp(g_jmp, 1);
}

void emit(const char* id, bool ok, const std::string& reason) {
    fprintf(g_out, "{\"id\":\"%s\",\"status\":\"%s\",\"stage\":\"execute\",\"reason\":\"", id,
            ok ? "PASS" : "FAIL");
    for (char c : reason) {
        if (c == '"' || c == '\\') fputc('\\', g_out);
        fputc(c, g_out);
    }
    fprintf(g_out, "\"}\n");
    fflush(g_out);
}

// Returns 0 when both cases pass, 1 when one fails, 2 on a setup error.
int run_xex(const char* xex_path) {
    FILE* f = fopen(xex_path, "rb");
    if (!f) {
        fprintf(g_out, "RCOMP-XEX error: cannot open %s\n", xex_path);
        return 2;
    }
    std::vector<uint8_t> xex;
    uint8_t buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) xex.insert(xex.end(), buf, buf + n);
    fclose(f);

    namespace rt = rcomp::rt;
    if (g_mem.reserve() != rcomp::MemStatus::Ok) {
        fprintf(g_out, "RCOMP-XEX error: guest memory reservation failed\n");
        return 2;
    }
    rcomp::set_active_guest_memory(&g_mem);
    if (rt::runtime_init(&g_mem) != rt::Status::Ok || rt::register_xboxkrnl_hle() != rt::Status::Ok) {
        fprintf(g_out, "RCOMP-XEX error: runtime init failed\n");
        return 2;
    }
    // Variable import for mode 2: test data only (the runtime implements no
    // kernel variable yet), registered before the load resolves the slots.
    uint32_t var_addr = 0;
    if (rt::runtime()->heap.alloc(0x10000, 0x10000, true, &var_addr) != rt::Status::Ok ||
        !rt::guest_write_be32(var_addr, kTestKrnlVersionWord) ||
        rt::register_variable_import(rt::kModuleXboxkrnl, 0x0158, var_addr, "TESTDOUBLE_XboxKrnlVersion") !=
            rt::Status::Ok) {
        fprintf(g_out, "RCOMP-XEX error: cannot register the test variable\n");
        return 2;
    }
    rt::XexImage img{};
    rt::Status s = rt::load_xex_image(g_mem, xex.data(), xex.size(), &img, g_out);
    fprintf(g_out, "RCOMP-XEX load path=%s bytes=%zu status=%s base=0x%08X size=0x%X entry=0x%08X "
                   "(PPC_IMAGE_BASE=0x%llX PPC_IMAGE_SIZE=0x%llX) libraries=%u thunks=%u function_slots=%u "
                   "variables_resolved=%u variables_unresolved=%u\n",
            xex_path, xex.size(), rt::status_name(s), img.base, img.size, img.entry_point,
            (unsigned long long)PPC_IMAGE_BASE, (unsigned long long)PPC_IMAGE_SIZE, img.import_libraries,
            img.function_thunks, img.function_slots, img.variables_resolved, img.variables_unresolved);
    if (s != rt::Status::Ok || img.base != PPC_IMAGE_BASE || img.size != PPC_IMAGE_SIZE) return 2;
    {
        // Loader contract: counts of this fixture, and a second load is refused.
        bool ok = img.import_libraries == 2 && img.function_thunks == 3 && img.function_slots == 1 &&
                  img.variables_resolved == 1 && img.variables_unresolved == 1;
        rt::XexImage again{};
        rt::Status s2 = rt::load_xex_image(g_mem, xex.data(), xex.size(), &again, nullptr);
        if (s2 != rt::Status::Conflict) ok = false;
        char b[160];
        snprintf(b, sizeof b, "second load -> %s (expected Conflict)", rt::status_name(s2));
        emit("xex/rcomp_title/loader_imports", ok, ok ? "" : b);
        if (!ok) ++g_fails;
    }

    std::vector<rcomp::FuncEntry> funcs;
    PPCFunc* entry_fn = nullptr;
    for (PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
        funcs.push_back({(uint32_t)m->guest, m->host, nullptr});
        if (m->guest == img.entry_point) entry_fn = m->host;
    }
    rcomp::register_functions(funcs.data(), funcs.size());
    fprintf(g_out, "RCOMP-XEX functions=%zu entry_mapped=%d\n", funcs.size(), entry_fn != nullptr);
    fflush(g_out);
    rcomp_set_fatal_hook(fatal_hook);

    int fails = g_fails;
    const ModeCase cases[] = {
        {0, "xex/rcomp_title/mode0_run", 0x6D},
        {1, "xex/rcomp_title/mode1_unresolved_import", 0},
        {2, "xex/rcomp_title/mode2_variable_import", kTestKrnlVersionWord},
        {3, "xex/rcomp_title/mode3_unresolved_variable_poison", rt::kUnresolvedImportPoison(0x0156)},
        {4, "xex/rcomp_title/mode4_function_slot_poison", rt::kUnresolvedImportPoison(0x00CC)},
        {5, "xex/rcomp_title/mode5_jump_table_absolute", 0x0A0B0C0D63ull},
        {6, "xex/rcomp_title/mode6_jump_table_computed", 0x1415161763ull},
        {7, "xex/rcomp_title/mode7_jump_table_byte_offset", 0x1E1F202163ull},
        {8, "xex/rcomp_title/mode8_jump_table_short_offset", 0x28292A2B63ull},
        {9, "xex/rcomp_title/mode9_savegpr_restgpr_helpers", 0x237},
    };
    for (const ModeCase& c : cases) {
        const uint32_t mode = c.mode;
        rt::Runtime& r = *rt::runtime();
        uint32_t heap_before = r.heap.stats().live_allocations;
        alignas(64) static PPCContext ctx;
        ctx = PPCContext{};
        ctx.fpscr.loadFromHost();
        rt::GuestThread th;
        std::string reason;
        bool ok = entry_fn != nullptr;
        if (!ok) reason = "entry point not in PPCFuncMappings; ";
        if (ok && rt::create_guest_thread(r.heap, {0x10000, img.entry_point, mode}, &ctx, &th) != rt::Status::Ok) {
            ok = false;
            reason += "create_guest_thread failed; ";
        }
        uint32_t exit_code = 0;
        g_fatal.clear();
        bool fatal = false;
        if (ok) {
            if (sigsetjmp(g_jmp, 1) == 0) rt::run_guest_thread(th, ctx, g_mem.base(), entry_fn, &exit_code);
            else {
                fatal = true;
                rt::abandon_current_guest_thread_after_fatal();
            }
        }
        const char* id = c.id;
        if (mode != 1 && ok) {
            if (fatal) { ok = false; reason += "unexpected fatal " + g_fatal + "; "; }
            if (ctx.r3.u64 != c.expect_r3) {
                ok = false;
                char b[96];
                snprintf(b, sizeof b, "r3 expected 0x%llX actual 0x%llX; ", (unsigned long long)c.expect_r3,
                         (unsigned long long)ctx.r3.u64);
                reason += b;
            }
        } else if (mode == 1 && ok) {
            const char* want = "missing_import: module=xbdm_xex ordinal=0x0012";
            if (!fatal || g_fatal.compare(0, strlen(want), want) != 0) {
                ok = false;
                reason += "expected fatal '" + std::string(want) + "' got '" + g_fatal + "'; ";
            } else {
                reason = g_fatal;
            }
        }
        // Both runs must release the thread's stack/TLS and leave no handle,
        // including after a fatal unwind.
        if (th.alloc_base) rt::destroy_guest_thread(r.heap, &th);
        if (r.heap.stats().live_allocations != heap_before) { ok = false; reason += "; heap leak"; }
        if (r.handles.live_count()) { ok = false; reason += "; handle leak"; }
        emit(id, ok, reason);
        fails += !ok;
    }
    rcomp_set_fatal_hook(nullptr);
    const int total = 1 + int(sizeof cases / sizeof cases[0]);
    fprintf(g_out, "RCOMP-XEX executed=%d pass=%d fail=%d\n", total, total - fails, fails);
    fflush(g_out);
    return fails ? 1 : 0;
}

}  // namespace

#ifdef RCOMP_HARNESS_LIBRARY
// PS5 test title entry: `xex_path` is the packaged synthetic XEX.
extern "C" int rcomp_xex_selftest(FILE* out, const char* xex_path) {
    g_out = out;
    return run_xex(xex_path);
}
#else
int main(int argc, char** argv) {
    return run_xex(argc > 1 ? argv[1] : RCOMP_XEX_PATH);
}
#endif
