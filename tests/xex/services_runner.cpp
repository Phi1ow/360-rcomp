// Original service-oracle PPC through production TitleRuntime. The unused GPU
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
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime_state.h"

namespace {
namespace rt = rcomp::rt;
using rcomp::app::TitleConfig;
using rcomp::app::TitleRuntime;

#if defined(RCOMP_SERVICES_PS5)
constexpr const char* kScope = "PS5";
#else
constexpr const char* kScope = "host";
#endif

constexpr uint32_t kGuestSuccess = 0x71;

struct Results {
    FILE* out;
    int checks = 0;
    int failures = 0;

    void check(const char* id, bool ok, const std::string& reason = {}) {
        ++checks;
        failures += !ok;
        std::fprintf(out,
                     "{\"id\":\"xex/services/%s\",\"status\":\"%s\",\"scope\":\"%s\",\"reason\":\"",
                     id, ok ? "PASS" : "FAIL", kScope);
        for (unsigned char c : reason) {
            if (c < 0x20) {
                std::fprintf(out, "\\u%04x", unsigned(c));
            } else {
                if (c == '"' || c == '\\') std::fputc('\\', out);
                std::fputc(c, out);
            }
        }
        std::fputs("\"}\n", out);
        std::fflush(out);
    }

    int finish() {
        std::fprintf(out,
                     "RCOMP-SERVICES checks=%d pass=%d fail=%d scope=%s graphics=NOT_TESTED%s\n",
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
                std::fprintf(self.out_, "FAIL xex/services/watchdog: exceeded 45 seconds or clock failed\n");
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
    if (!path || !*path || !bytes) return false;
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    constexpr size_t limit = 4 * 1024 * 1024;
    uint8_t chunk[4096];
    bool ok = true;
    for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), file)) != 0;) {
        if (n > limit - bytes->size()) {
            ok = false;
            break;
        }
        bytes->insert(bytes->end(), chunk, chunk + n);
    }
    ok = !std::ferror(file) && ok;
    return std::fclose(file) == 0 && ok;
}

std::string parent_directory(const char* path) {
    if (!path || !*path) return {};
    std::string value(path);
    const auto slash = value.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    if (slash == 0) return value.substr(0, 1);
    return value.substr(0, slash);
}

const char* guest_check(uint32_t code) {
    switch (code) {
    case 0xE501: return "xam_alloc";
    case 0xE502: return "xam_free";
    case 0xE503: return "rtl_multibyte_to_unicode";
    case 0xE504: return "rtl_unicode_to_multibyte";
    case 0xE505: return "rtl_upcase_unicode";
    case 0xE506: return "rtl_ntstatus_to_dos";
    case 0xE507: return "rtl_allocated_ansi_string";
    case 0xE508: return "io_full_attributes";
    case 0xE509: return "io_open";
    case 0xE50A: return "io_read";
    case 0xE50B: return "io_position";
    case 0xE50C: return "io_network_open";
    case 0xE50D: return "io_close";
    case 0xE50E: return "object_current_thread_reference";
    case 0xE50F: return "object_body_reference_lifetime";
    case 0xE510: return "object_invalid_handle_preserves_output";
    default: break;
    }
    return "unexpected_exit";
}

int run(FILE* out, const char* xex_path) {
    if (!out) return 2;
    Results result{out};
    Watchdog watchdog;
    result.check("watchdog_started", watchdog.start(out));
    if (result.failures) return result.finish();

    TitleConfig cfg;
    cfg.guest_path = "game:\\rcomp_services.xex";
    cfg.command_line = "rcomp_services.xex";
    cfg.log = out;
    cfg.screen = false;
    cfg.main_stack_size = 0x10000;
    cfg.game_root = parent_directory(xex_path);
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
        const auto before_handles = owner->handles.live_count();

        for (unsigned iteration = 0; iteration < 2; ++iteration) {
            uint32_t code = 0;
            error.clear();
            const bool ok = title->RunEntry(0, &code, &error);
            char detail[192];
            std::snprintf(detail, sizeof(detail),
                          "cycle=%u run=%u exit=0x%08X expected=0x%08X %s",
                          cycle, iteration, code, kGuestSuccess,
                          code == kGuestSuccess ? "original_ppc_checks_completed" : guest_check(code));
            result.check("ppc_service_oracle", ok && code == kGuestSuccess,
                         std::string(detail) + (error.empty() ? "" : "; " + error));

            const auto after = owner->heap.stats();
            char resources[192];
            const auto after_handles = owner->handles.live_count();
            std::snprintf(resources, sizeof(resources),
                          "cycle=%u run=%u heap_live=%u->%u heap_bytes=%llu->%llu handles=%u->%u",
                          cycle, iteration, unsigned(before.live_allocations),
                          unsigned(after.live_allocations),
                          (unsigned long long)before.allocated_bytes,
                          (unsigned long long)after.allocated_bytes,
                          unsigned(before_handles), unsigned(after_handles));
            result.check("ppc_releases_transient_resources",
                         after.live_allocations == before.live_allocations &&
                         after.allocated_bytes == before.allocated_bytes &&
                         after_handles == before_handles, resources);
            if (!ok || code != kGuestSuccess) break;
        }

        title.reset();
        result.check("destroy_clears_runtime_memory_functions",
                     !rt::runtime() && !rcomp::active_guest_memory() && !rcomp::lookup_function(entry));
        if (result.failures) break;
    }

    return result.finish();
}
}  // namespace

extern "C" int rcomp_xex_selftest(FILE* out, const char* xex_path) {
    return run(out, xex_path);
}
