// R-comp PS5 test title (owner: Agent 2).
//
// Prints its identity, runs the platform self-test (guest memory on the PS5
// os_vm backend: sceKernelReserveVirtualRange + direct memory), reports the
// memory budgets it can query, and reports a status in its last line
// "RCOMP-TITLE end status=<n>", then parks until the harness closes it
// (rcomp_title_park: a PS5 application cannot end itself):
//   0  all checks passed        1  a check failed
//   2  the log file could not be opened (results still on stdout)
//   3  CPU corpus mode: rcomp_cpu_selftest did not complete (returned != 0)
//   4  M3 mode: rcomp_m3_selftest failed
//   5  XEX mode (-DRCOMP_TITLE_XEX, build_title.sh --xex): the synthetic XEX
//      packaged in fixtures/xex/ did not load or run as expected
//   6  Xenos command processor mode (-DRCOMP_TITLE_XENOS_CP, --xenos-cp):
//      tests/xenos_cp command processor test failed
//   7  same mode: tests/xenos_cp video exports test (test_video.cpp) failed
//
// CPU corpus mode (-DRCOMP_TITLE_CPU_CORPUS, build_title.sh --cpu-corpus):
// after the platform self-test, runs tests/cpu's rcomp_cpu_selftest(log, start)
// which appends one JSON line per test to the same log. `start` is read from
// /app0/rcomp_cpu_start.txt when present (resume after a crash: the harness
// writes a FAIL line for the crashing test and _exit(3)s, so the title ends
// without an "end" line), otherwise 0. Per-test verdicts live in the JSON; the
// title status only says whether the platform test passed and the corpus run
// completed.
//
// Log: /app0/rcomp_title.log (the title's own folder; for a homebrew title
// placed in /data/homebrew/<TITLE_ID>/ that is the same directory seen over
// FTP). Lines are appended, each run starts with "RCOMP-TITLE begin" and ends
// with "RCOMP-TITLE end status=<n>" so a collector can pick the last run.
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../ps5_kernel.h"

#ifndef RCOMP_TITLE_ID
#error "RCOMP_TITLE_ID must be defined by the build (e.g. -DRCOMP_TITLE_ID=\"PPSA88360\")"
#endif
#ifndef RCOMP_TITLE_BUILD_ID
#define RCOMP_TITLE_BUILD_ID "unknown"
#endif

extern "C" int rcomp_platform_selftest(FILE* log);
#ifdef RCOMP_TITLE_CPU_CORPUS
extern "C" int rcomp_cpu_selftest(FILE* out, int start);
#endif
#ifdef RCOMP_TITLE_M3
extern "C" int rcomp_m3_selftest(FILE* out, const char* root);
#endif
#ifdef RCOMP_TITLE_XEX
extern "C" int rcomp_xex_selftest(FILE* out, const char* xex_path);
#endif
#ifdef RCOMP_TITLE_XENOS_CP
extern "C" int rcomp_xenos_cp_selftest(FILE* out);
extern "C" int rcomp_xenos_video_selftest(FILE* out);
#endif

namespace {

// Absolute paths only (no cwd on PS5). The title's data folder is written
// directly: /app0 may be a read-only mount (Emu-3 eden docs/PS5_ARCHITECTURE.md,
// "Filesystem": /app0 read-only for packaged files). /app0 stays a fallback.
#define RCOMP_TITLE_DIR "/data/homebrew/" RCOMP_TITLE_ID
const char* const kLogPaths[] = {RCOMP_TITLE_DIR "/rcomp_title.log", "/app0/rcomp_title.log"};
const char* g_log_path = "stdout";

// The title never ends itself: on this console every way out that an
// application has fails (PS5 run of kit f414844, docs/PS5_RESULTS.md O1):
// libc exit() makes the kernel raise SIGSYS and suspend the process, LoadExec
// fails and sceLncUtilKillApp on itself is refused with 0x8094000f (Emu-3
// eden docs/PS5_PORT.md). Once the log is complete the title parks here; the
// harness sees "RCOMP-TITLE end" and closes it (ps5vkctl "kill <ID>").
[[noreturn]] void rcomp_title_park() {
    for (;;) sceKernelUsleep(1000000);
}

#ifdef RCOMP_TITLE_CPU_CORPUS
int cpu_start_index() {
    FILE* f = fopen(RCOMP_TITLE_DIR "/rcomp_cpu_start.txt", "r");
    if (!f) f = fopen("/app0/rcomp_cpu_start.txt", "r");
    if (!f) return 0;
    int v = 0;
    if (fscanf(f, "%d", &v) != 1 || v < 0) v = 0;
    fclose(f);
    return v;
}
#endif

void report_budgets(FILE* f, const char* when) {
    size_t flex_avail = 0, flex_conf = 0;
    int ra = sceKernelAvailableFlexibleMemorySize(&flex_avail);
    int rc = sceKernelConfiguredFlexibleMemorySize(&flex_conf);
    int64_t dsize = sceKernelGetDirectMemorySize();
    fprintf(f, "RCOMP-TITLE mem %s flexible_available=%zu (rc=0x%08x) flexible_configured=%zu "
               "(rc=0x%08x) direct_size=%lld\n",
            when, flex_avail, (unsigned)ra, flex_conf, (unsigned)rc, (long long)dsize);
}

}  // namespace

int main(int argc, char** argv) {
    FILE* log = nullptr;
    for (const char* p : kLogPaths) {
        if ((log = fopen(p, "a")) != nullptr) {
            g_log_path = p;
            break;
        }
    }
    FILE* f = log ? log : stdout;
    fprintf(f, "RCOMP-TITLE begin title=%s build=%s pid=%d argc=%d argv0=%s\n", RCOMP_TITLE_ID,
            RCOMP_TITLE_BUILD_ID, (int)getpid(), argc, (argc > 0 && argv && argv[0]) ? argv[0] : "-");
    if (!log) fprintf(stdout, "RCOMP-TITLE warning: cannot open %s or %s\n", kLogPaths[0], kLogPaths[1]);
    else fprintf(f, "RCOMP-TITLE log path=%s\n", g_log_path);
    report_budgets(f, "start");
    fflush(f);

    int status = rcomp_platform_selftest(f) == 0 ? 0 : 1;
    fprintf(f, "RCOMP-TITLE platform status=%d\n", status);
    fflush(f);
#ifdef RCOMP_TITLE_CPU_CORPUS
    {
        int start = cpu_start_index();
        fprintf(f, "RCOMP-TITLE cpu begin start=%d\n", start);
        fflush(f);
        int cpu = rcomp_cpu_selftest(f, start);
        fprintf(f, "RCOMP-TITLE cpu status=%d\n", cpu);
        fflush(f);
        if (cpu != 0 && status == 0) status = 3;
    }
#endif

#ifdef RCOMP_TITLE_M3
    {
        // M3 fixture root: packaged read-only data, via /app0 first.
        const char* roots[] = {"/app0/fixtures/m3", RCOMP_TITLE_DIR "/fixtures/m3"};
        const char* root = nullptr;
        for (const char* r : roots) {
            char probe[256];
            snprintf(probe, sizeof probe, "%s/data/m3.bin", r);
            FILE* t = fopen(probe, "rb");
            if (t) {
                fclose(t);
                root = r;
                break;
            }
        }
        fprintf(f, "RCOMP-TITLE m3 begin root=%s\n", root ? root : "(missing)");
        fflush(f);
        int m3 = root ? rcomp_m3_selftest(f, root) : 2;
        fprintf(f, "RCOMP-TITLE m3 status=%d\n", m3);
        fflush(f);
        if (m3 != 0 && status == 0) status = 4;
    }
#endif

#ifdef RCOMP_TITLE_XEX
    {
        const char* paths[] = {"/app0/fixtures/xex/rcomp_title.xex",
                               RCOMP_TITLE_DIR "/fixtures/xex/rcomp_title.xex"};
        const char* path = nullptr;
        for (const char* p : paths) {
            FILE* t = fopen(p, "rb");
            if (t) {
                fclose(t);
                path = p;
                break;
            }
        }
        fprintf(f, "RCOMP-TITLE xex begin path=%s\n", path ? path : "(missing)");
        fflush(f);
        int xs = path ? rcomp_xex_selftest(f, path) : 2;
        fprintf(f, "RCOMP-TITLE xex status=%d\n", xs);
        fflush(f);
        if (xs != 0 && status == 0) status = 5;
    }
#endif

#ifdef RCOMP_TITLE_XENOS_CP
    {
        fprintf(f, "RCOMP-TITLE xenos_cp begin\n");
        fflush(f);
        int xs = rcomp_xenos_cp_selftest(f);
        fprintf(f, "RCOMP-TITLE xenos_cp status=%d\n", xs);
        fflush(f);
        if (xs != 0 && status == 0) status = 6;
        // Kernel video exports + physical memory over the same GPU (the
        // command processor test has stopped its GPU instance).
        fprintf(f, "RCOMP-TITLE xenos_video begin\n");
        fflush(f);
        int vs = rcomp_xenos_video_selftest(f);
        fprintf(f, "RCOMP-TITLE xenos_video status=%d\n", vs);
        fflush(f);
        if (vs != 0 && status == 0) status = 7;
    }
#endif

    report_budgets(f, "end");
    if (!log && status == 0) status = 2;
    fprintf(f, "RCOMP-TITLE end status=%d\n", status);
    fflush(f);
    if (log) {
        fclose(log);
        fprintf(stdout, "RCOMP-TITLE %s status=%d log=%s\n", RCOMP_TITLE_ID, status, g_log_path);
    }
    fflush(stdout);
    rcomp_title_park();
}
