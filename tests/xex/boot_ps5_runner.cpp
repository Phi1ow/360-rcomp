// Packaged original PPC through production TitleRuntime, on host or PS5.
// No host fixture creation: boot-data.bin is packaged beside the XEX.
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

#if defined(RCOMP_BOOT_PS5)
constexpr const char* kScope = "PS5";
#else
constexpr const char* kScope = "host";
#endif

struct Results {
    FILE* out;
    int checks = 0, failures = 0;
    void check(const char* id, bool ok, const std::string& reason = {}) {
        ++checks; failures += !ok;
        std::fprintf(out, "{\"id\":\"xex/packaged_boot/%s\",\"status\":\"%s\",\"scope\":\"%s\",\"reason\":\"",
                     id, ok ? "PASS" : "FAIL", kScope);
        for (unsigned char c : reason) {
            if (c < 0x20) std::fprintf(out, "\\u%04x", unsigned(c));
            else { if (c == '"' || c == '\\') std::fputc('\\', out); std::fputc(c, out); }
        }
        std::fputs("\"}\n", out); std::fflush(out);
    }
    int finish() {
        std::fprintf(out, "RCOMP-BOOT-PACKAGED checks=%d pass=%d fail=%d scope=%s graphics=NOT_TESTED%s\n",
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
                std::fprintf(self.out_, "FAIL xex/packaged_boot/watchdog: exceeded 45 seconds or clock failed\n");
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

bool read_bounded(const std::string& path, size_t limit, std::vector<uint8_t>* bytes) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    uint8_t chunk[4096];
    bool ok = true;
    for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), file)) != 0;) {
        if (n > limit - bytes->size()) { ok = false; break; }
        bytes->insert(bytes->end(), chunk, chunk + n);
    }
    ok = !std::ferror(file) && ok;
    return std::fclose(file) == 0 && ok;
}

bool clean(uint32_t entry) {
    return !rt::runtime() && !rcomp::active_guest_memory() && !rcomp::lookup_function(entry) &&
           !rt::find_import(rt::kModuleXboxkrnl, 0x00CC);
}

void mounted_read(Results& result, const char* device) {
    auto& runtime = *rt::runtime();
    const std::string path = std::string(device) + "/boot-data.bin";
    uint32_t handle = 0, count = 0;
    auto status = runtime.vfs.open(runtime.handles, path, false, &handle);
    std::shared_ptr<rt::GuestFile> file;
    uint8_t bytes[4]{};
    bool ok = status == rt::Status::Ok &&
              runtime.handles.lookup_as<rt::GuestFile>(handle, &file) == rt::Status::Ok;
    if (ok) ok = file->read(bytes, 4, &count) == rt::Status::Ok && count == 4 &&
                 bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4;
    if (handle) ok = runtime.handles.close(handle) == rt::Status::Ok && ok;
    result.check(device[0] == 'g' ? "game_mount_read" : "d_mount_read", ok, path);
}

int run_packaged(FILE* out, const char* xex_path) {
    if (!out) return 2;
    Results result{out};
    Watchdog watchdog;
    result.check("watchdog_started", watchdog.start(out));
    if (result.failures) return result.finish();

    TitleConfig cfg;
    const std::string path = xex_path ? xex_path : "";
    const size_t slash = path.find_last_of("/\\");
    cfg.game_root = slash == std::string::npos ? "." : path.substr(0, slash);
    if (cfg.game_root.empty()) cfg.game_root = "/";
    cfg.log = out; cfg.screen = false; cfg.main_stack_size = 0x10000;
    result.check("packaged_xex_read", !path.empty() && read_bounded(path, 4 * 1024 * 1024, &cfg.xex), path);
    if (result.failures) return result.finish();
    std::vector<uint8_t> data;
    const std::string data_path = cfg.game_root + "/boot-data.bin";
    result.check("packaged_data_exact", read_bounded(data_path, 4, &data) &&
                 data == std::vector<uint8_t>({1, 2, 3, 4}), data_path);
    if (result.failures) return result.finish();
    for (auto* mapping = PPCFuncMappings; mapping->host; ++mapping)
        cfg.functions.push_back({uint32_t(mapping->guest), mapping->host, nullptr});

    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        std::string error;
        auto title = TitleRuntime::Create(cfg, &error);
        result.check("title_create", bool(title), error);
        if (!title) break;
        const uint32_t entry = title->entry_point();
        mounted_read(result, "game:");
        mounted_read(result, "d:");
        auto* owner = rt::runtime();
        const auto before = owner->heap.stats().live_allocations;
        for (unsigned run = 0; run < 2; ++run) {
            uint32_t code = 0;
            bool ok = title->RunEntry(0, &code, &error);
            char detail[128];
            std::snprintf(detail, sizeof(detail), "cycle=%u run=%u exit=0x%08X expected=0x0000006D",
                          cycle, run, code);
            result.check("ppc_tls_file_workers_termination", ok && code == 0x6D,
                         std::string(detail) + (error.empty() ? "" : "; " + error));
            result.check("ppc_releases_allocations_handles",
                         owner->heap.stats().live_allocations == before && owner->handles.live_count() == 0);
            if (!ok || code != 0x6D) break;
        }
        title.reset();
        result.check("destroy_clears_runtime_memory_imports_functions", clean(entry));
        if (result.failures) break;
    }
    return result.finish();
}
} // namespace

extern "C" int rcomp_xex_selftest(FILE* out, const char* xex_path) {
    return run_packaged(out, xex_path);
}
