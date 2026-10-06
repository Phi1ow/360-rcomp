// Missing import -> exact RCOMP-FATAL diagnostic; registry dispatch.
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "test_util.h"

// Exactly what XenonRecomp emits in ppc_recomp_shared.h for import thunks.
PPC_EXTERN_FUNC(__imp__NtCreateFile);
PPC_EXTERN_FUNC(__imp__XamInputGetState);

using namespace rcomp::rt;

namespace {
int g_calls = 0;
void TESTDOUBLE_XamInputGetState(PPCContext& ctx, uint8_t*) {
    ++g_calls;
    ctx.r3.u64 = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED, what the double models
}
void other(PPCContext&, uint8_t*) {}
void TESTDOUBLE_count(PPCContext&, uint8_t*) { ++g_calls; }

// Runs fn with stderr redirected to a file and returns what was written.
template <class F>
std::string capture_stderr(F fn, bool* fatal) {
    char path[] = "/tmp/rcomp_rt_stderr_XXXXXX";
    int fd = mkstemp(path);
    fflush(stderr);
    int saved = dup(2);
    dup2(fd, 2);
    bool f = false;
    CAPTURE_FATAL(fn(), f);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    *fatal = f;
    std::string out;
    lseek(fd, 0, SEEK_SET);
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    close(fd);
    unlink(path);
    return out;
}
}  // namespace

int main() {
    clear_imports();
    alignas(64) PPCContext ctx{};
    ctx.lr = 0x82001234;
    ctx.r3.u64 = 0x1;
    ctx.r4.u64 = 0x2;
    ctx.r5.u64 = 0xFFFFFFFF00000003ull;  // only the low 32 bits are printed
    ctx.r6.u64 = 0x4;
    uint8_t* base = nullptr;  // never dereferenced on this path

    bool fatal = false;
    std::string err = capture_stderr([&] { __imp__NtCreateFile(ctx, base); }, &fatal);
    const std::string expect_msg =
        "module=xboxkrnl.exe ordinal=0x00D2 name=NtCreateFile lr=0x82001234 r3=0x00000001 "
        "r4=0x00000002 r5=0x00000003 r6=0x00000004";
    CHECK(fatal);
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_MISSING_IMPORT);
    CHECK(g_fatal_msg == expect_msg);
    CHECK(err == "RCOMP-FATAL kind=missing_import " + expect_msg + "\n");
    if (err != "RCOMP-FATAL kind=missing_import " + expect_msg + "\n")
        fprintf(stderr, "got stderr: [%s]\n", err.c_str());

    // Direct hle_missing_import with a module outside the tables.
    rcomp::ImportId unknown{"xbdm.xex", 0x5, nullptr};
    err = capture_stderr([&] { rcomp::hle_missing_import(unknown, ctx); }, &fatal);
    CHECK(fatal);
    CHECK(g_fatal_msg.rfind("module=xbdm.xex ordinal=0x0005 name=? lr=0x82001234", 0) == 0);

    // Registered implementation is dispatched; duplicates are refused.
    uint32_t ord = 0;
    CHECK(export_ordinal(kModuleXam, "XamInputGetState", &ord));
    CHECK_EQ(ord, 0x191u);
    CHECK(export_name(kModuleXboxkrnl, 0xD2) != nullptr &&
          strcmp(export_name(kModuleXboxkrnl, 0xD2), "NtCreateFile") == 0);
    CHECK_ST(register_import(kModuleXam, ord, TESTDOUBLE_XamInputGetState,
                             "TESTDOUBLE_XamInputGetState"),
             Status::Ok);
    CHECK_ST(register_import("XAM.XEX", ord, other, "other"), Status::AlreadyExists);
    CHECK(strcmp(import_registry_name(kModuleXam, ord), "TESTDOUBLE_XamInputGetState") == 0);
    CAPTURE_FATAL(__imp__XamInputGetState(ctx, base), fatal);
    CHECK(!fatal);
    CHECK_EQ(g_calls, 1);
    CHECK_EQ(ctx.r3.u64, 0x48Full);

    // Unregistering brings the trap back.
    CHECK_ST(unregister_import(kModuleXam, ord), Status::Ok);
    err = capture_stderr([&] { __imp__XamInputGetState(ctx, base); }, &fatal);
    CHECK(fatal);
    CHECK(g_fatal_msg.rfind("module=xam.xex ordinal=0x0191 name=XamInputGetState ", 0) == 0);

    // The general entry point finds modules by name, including ones without a dispatch row, and
    // ordinals beyond the fast table; the thunks' entry point takes the module from the caller.
    {
        const int before = g_calls;
        const rcomp::ImportId odd{"xbdm.xex", 0x5, "TESTDOUBLE_odd"};
        CHECK_ST(register_import("xbdm.xex", 0x5, TESTDOUBLE_count, "TESTDOUBLE_count"), Status::Ok);
        dispatch_import(odd, ctx, base);
        CHECK_EQ(g_calls, before + 1);
        const rcomp::ImportId high{kModuleXboxkrnl, 0x1800, "TESTDOUBLE_high"};
        CHECK_ST(register_import(kModuleXboxkrnl, 0x1800, TESTDOUBLE_count, "TESTDOUBLE_count"), Status::Ok);
        dispatch_import(high, ctx, base);
        dispatch_import_fast(ImportModule::Xboxkrnl, high, ctx, base);
        CHECK_EQ(g_calls, before + 3);
        const rcomp::ImportId row{kModuleXam, 0x191, "TESTDOUBLE_row"};
        CHECK_ST(register_import(kModuleXam, 0x191, TESTDOUBLE_count, "TESTDOUBLE_count"), Status::Ok);
        dispatch_import(row, ctx, base);
        dispatch_import_fast(ImportModule::Xam, row, ctx, base);
        CHECK_EQ(g_calls, before + 5);
        CHECK_ST(unregister_import("xbdm.xex", 0x5), Status::Ok);
        CHECK_ST(unregister_import(kModuleXboxkrnl, 0x1800), Status::Ok);
        CHECK_ST(unregister_import(kModuleXam, 0x191), Status::Ok);
    }

    // Without the hook the process really exits with RCOMP_EXIT_FATAL.
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, 2);
        __imp__NtCreateFile(ctx, base);
        _exit(0);
    }
    int ws = 0;
    waitpid(pid, &ws, 0);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == RCOMP_EXIT_FATAL);

    return test_result("rt_test_missing_import");
}
