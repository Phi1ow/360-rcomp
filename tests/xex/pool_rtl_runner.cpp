// Original pool/Rtl PPC through production TitleRuntime. The unused GPU
// boundary is the existing TESTDOUBLE_cpu_only_gpu; no HLE is substituted.
#include <pthread.h>
#include <time.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "ppc_recomp_shared.h"
#include "rcomp/app/title_runtime.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime_state.h"

namespace {
namespace rt = rcomp::rt;
using rcomp::app::TitleConfig;
using rcomp::app::TitleRuntime;
#if defined(RCOMP_POOL_RTL_PS5)
constexpr const char* kScope = "PS5";
#else
constexpr const char* kScope = "host";
#endif

struct Results {
    FILE* out;
    int checks = 0, failures = 0;
    void check(const char* id, bool ok, const std::string& reason = {}) {
        ++checks; failures += !ok;
        std::fprintf(out, "{\"id\":\"xex/pool_rtl/%s\",\"status\":\"%s\",\"scope\":\"%s\",\"reason\":\"",
                     id, ok ? "PASS" : "FAIL", kScope);
        for (unsigned char c : reason) {
            if (c < 0x20) std::fprintf(out, "\\u%04x", unsigned(c));
            else { if (c == '"' || c == '\\') std::fputc('\\', out); std::fputc(c, out); }
        }
        std::fputs("\"}\n", out); std::fflush(out);
    }
    int finish() {
        std::fprintf(out, "RCOMP-POOL-RTL checks=%d pass=%d fail=%d scope=%s graphics=NOT_TESTED%s\n",
                     checks, checks - failures, failures, kScope,
                     std::strcmp(kScope, "host") == 0 ? " PS5=NOT_TESTED" : "");
        std::fflush(out);
        return failures ? 1 : 0;
    }
};

class Watchdog {
public:
    bool start(FILE* out) {
        out_ = out;
        if (clock_gettime(CLOCK_MONOTONIC, &started_) != 0) return false;
        running_ = pthread_create(&thread_, nullptr, run, this) == 0;
        return running_;
    }
    ~Watchdog() {
        stopped_.store(true);
        if (running_) pthread_join(thread_, nullptr);
    }
private:
    static void* run(void* arg) {
        auto& self = *static_cast<Watchdog*>(arg);
        while (!self.stopped_.load()) {
            timespec now{};
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
                now.tv_sec - self.started_.tv_sec >= 45) {
                std::fprintf(self.out_, "FAIL xex/pool_rtl/watchdog: exceeded 45 seconds or clock failed\n");
                std::fflush(self.out_);
                std::abort();
            }
            timespec pause{0, 100000000};
            nanosleep(&pause, nullptr);
        }
        return nullptr;
    }
    FILE* out_ = nullptr;
    timespec started_{};
    pthread_t thread_{};
    std::atomic<bool> stopped_{false};
    bool running_ = false;
};

bool read_xex(const char* path, std::vector<uint8_t>* bytes) {
    if (!path || !*path) return false;
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    constexpr size_t limit = 4 * 1024 * 1024;
    uint8_t chunk[4096];
    bool ok = true;
    for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), file)) != 0;) {
        if (n > limit - bytes->size()) { ok = false; break; }
        bytes->insert(bytes->end(), chunk, chunk + n);
    }
    ok = !std::ferror(file) && ok;
    return std::fclose(file) == 0 && ok;
}

const char* guest_check(uint32_t code) {
    static const char* names[] = {
        "none", "pool_small_alignment", "fill_endian_and_guards", "compare_all_bytes",
        "compare_prefix_bytes", "compare_empty", "fill_empty", "pool_page_alignment",
        "pool_free", "init_ansi_fields", "init_unicode_fields", "init_ansi_null",
        "init_unicode_null", "compare_count_bound", "compare_order", "compare_case_fold",
        "compare_case_sensitive", "compare_empty_strings", "compare_prefix_lengths",
        "compare_embedded_nul", "compare_terminated_sentinel", "compare_latin1", "compare_ff_fold"
    };
    return code >= 0xE101 && code <= 0xE116 ? names[code - 0xE100] : "unexpected_exit";
}

int run(FILE* out, const char* xex_path) {
    if (!out) return 2;
    Results result{out};
    Watchdog watchdog;
    result.check("watchdog_started", watchdog.start(out));
    if (result.failures) return result.finish();
    TitleConfig cfg;
    cfg.log = out; cfg.screen = false; cfg.main_stack_size = 0x10000;
    result.check("packaged_xex_read", read_xex(xex_path, &cfg.xex), xex_path ? xex_path : "");
    if (result.failures) return result.finish();
    for (auto* mapping = PPCFuncMappings; mapping->host; ++mapping)
        cfg.functions.push_back({uint32_t(mapping->guest), mapping->host, nullptr});

    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        std::string error;
        auto title = TitleRuntime::Create(cfg, &error);
        result.check("title_create", bool(title), error);
        if (!title) break;
        const uint32_t entry = title->entry_point();
        auto* owner = rt::runtime();
        const auto before = owner->heap.stats();
        for (unsigned iteration = 0; iteration < 2; ++iteration) {
            uint32_t code = 0;
            const bool ok = title->RunEntry(0, &code, &error);
            char detail[192];
            std::snprintf(detail, sizeof(detail), "cycle=%u run=%u exit=0x%08X expected=0x0000006E %s",
                          cycle, iteration, code, code == 0x6E ? "22_original_ppc_checks_completed" : guest_check(code));
            result.check("ppc_pool_memory_strings", ok && code == 0x6E,
                         std::string(detail) + (error.empty() ? "" : "; " + error));
            const auto after = owner->heap.stats();
            result.check("ppc_releases_pool_and_thread_allocations",
                         after.live_allocations == before.live_allocations &&
                         after.allocated_bytes == before.allocated_bytes && owner->handles.live_count() == 0);
            if (!ok || code != 0x6E) break;
        }
        title.reset();
        result.check("destroy_clears_runtime_memory_imports_functions",
                     !rt::runtime() && !rcomp::active_guest_memory() && !rcomp::lookup_function(entry) &&
                     !rt::find_import(rt::kModuleXboxkrnl, 0x000B));
        if (result.failures) break;
    }
    return result.finish();
}
} // namespace

extern "C" int rcomp_xex_selftest(FILE* out, const char* xex_path) {
    return run(out, xex_path);
}
