// Original PPC scalar fields, workers and lifecycle through production runtime.
#include <pthread.h>
#include <time.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "ppc_recomp_shared.h"
#include "rcomp/app/title_runtime.h"
#include "rcomp/runtime/process_lifecycle.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime_state.h"

namespace {
namespace rt = rcomp::rt;
#if defined(RCOMP_KERNEL_BOOT_PS5)
constexpr const char* scope = "PS5";
#else
constexpr const char* scope = "host";
#endif
struct Results {
    FILE* out;
    unsigned count = 0, failures = 0;
    void check(const char* name, bool ok) {
        ++count; failures += !ok;
        std::fprintf(out, "{\"id\":\"xex/kernel_boot/%s\",\"status\":\"%s\",\"scope\":\"%s\"}\n",
                     name, ok ? "PASS" : "FAIL", scope);
        std::fflush(out);
    }
    int finish() {
        std::fprintf(out, "RCOMP-KERNEL-BOOT checks=%u pass=%u fail=%u scope=%s graphics=NOT_TESTED\n",
                     count, count-failures, failures, scope);
        std::fflush(out);
        return failures ? 1 : 0;
    }
};
class Watchdog {
    std::atomic<bool> stopped{false};
    pthread_t worker{};
    bool running = false;
    timespec start_time{};
    static void* run(void* pointer) {
        auto& self = *static_cast<Watchdog*>(pointer);
        while (!self.stopped.load()) {
            timespec now{};
            if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec-self.start_time.tv_sec >= 45) {
                std::fprintf(stderr, "FAIL kernel_boot/watchdog\n");
                std::fflush(stderr);
                std::abort();
            }
            timespec delay{0, 100000000};
            nanosleep(&delay, nullptr);
        }
        return nullptr;
    }
public:
    bool start() {
        if (clock_gettime(CLOCK_MONOTONIC, &start_time)) return false;
        running = pthread_create(&worker, nullptr, &run, this) == 0;
        return running;
    }
    ~Watchdog() { stopped = true; if (running) pthread_join(worker, nullptr); }
};

bool read_file(const char* path, std::vector<uint8_t>* bytes) {
    FILE* f = path ? std::fopen(path, "rb") : nullptr;
    if (!f) return false;
    bool ok = true;
    uint8_t chunk[4096];
    while (const size_t n = std::fread(chunk, 1, sizeof(chunk), f)) {
        if (n > 4*1024*1024-bytes->size()) { ok = false; break; }
        bytes->insert(bytes->end(), chunk, chunk+n);
    }
    ok = !std::ferror(f) && ok;
    return std::fclose(f) == 0 && ok;
}

int run(FILE* out, const char* path) {
    if (!out) return 2;
    Results result{out};
    Watchdog watchdog;
    result.check("watchdog", watchdog.start());
    rcomp::app::TitleConfig cfg;
    cfg.log = out; cfg.screen = false; cfg.main_stack_size = 0x10000;
    cfg.configure_kernel_variables = true; cfg.configure_xconfig = true;
    cfg.display_width = 1280; cfg.display_height = 720; cfg.refresh_hz = 60;
    cfg.guest_path = "game:\\rcomp_kernel_boot.xex";
    cfg.xconfig_profile.language = 1;
    cfg.xconfig_profile.av_region = rt::XConfigAvRegion::Pal60;
    auto& video = cfg.xconfig_profile.video;
    video.connector = rt::XConfigVideoConnector::Hdmi;
    video.display_width = 1280; video.display_height = 720; video.refresh_millihz = 60000;
    video.widescreen = true; video.high_definition = true;
    result.check("read_xex", read_file(path, &cfg.xex));
    if (result.failures) return result.finish();
    for (auto* item = PPCFuncMappings; item->host; ++item)
        cfg.functions.push_back({uint32_t(item->guest), item->host, nullptr});
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        std::string error;
        auto title = rcomp::app::TitleRuntime::Create(cfg, &error);
        result.check("title_create", bool(title));
        if (!title) { std::fprintf(out, "kernel_boot/create: %s\n", error.c_str()); break; }
        const auto entry = title->entry_point();
        auto* owner = rt::runtime();
        uint32_t scratch = 0;
        const auto initial = owner->heap.stats();
        result.check("results_allocation", owner->heap.alloc(64, 16, true, &scratch) == rt::Status::Ok);
        if (!scratch) break;
        const auto baseline = owner->heap.stats();
        uint32_t code = 0;
        bool ok = title->RunEntry(scratch, &code, &error);
        std::fprintf(out, "RCOMP-KERNEL-BOOT phase=fields cycle=%u exit=0x%08X ppc_checks=18\n", cycle, code);
        result.check("ppc_kernel_fields_and_worker", ok && code == 0x72);
        if (!ok || code != 0x72) {
            for (unsigned offset = 0; offset < 40; offset += 4) {
                uint32_t value = 0;
                rt::guest_read_be32(scratch + offset, &value);
                std::fprintf(out, "kernel_boot/result offset=%u value=0x%08X\n", offset, value);
            }
        }
        auto after = owner->heap.stats();
        result.check("fields_release_thread_storage",
                     after.live_allocations == baseline.live_allocations &&
                     after.allocated_bytes == baseline.allocated_bytes && !owner->handles.live_count());
        if (!ok || code != 0x72) break;
        auto read = [&](unsigned offset) { uint32_t value=0; rt::guest_read_be32(scratch+offset,&value); return value; };
        rt::guest_write_be32(scratch, 1);
        code = 0xFFFFFFFFu;
        ok = title->RunEntry(scratch, &code, &error);
        result.check("termination_unwinds_entry", ok && code == 0);
        result.check("same_context_callback_once",
                     read(20) == 1 && read(24) == 0xCAFE1234u && read(28) == 0 &&
                     read(16) != 0 && read(16) != read(4) && read(16) != read(8));
        rt::TitleLifecycleSnapshot lifecycle;
        result.check("callbacks_completed", rt::title_lifecycle_snapshot(&lifecycle) == rt::Status::Ok &&
                     lifecycle.phase == rt::TitleLifecyclePhase::CallbacksComplete);
        const bool freed = owner->heap.free(scratch) == rt::Status::Ok;
        after = owner->heap.stats();
        result.check("all_transient_storage_released", freed &&
                     after.live_allocations == initial.live_allocations &&
                     after.allocated_bytes == initial.allocated_bytes && !owner->handles.live_count());
        title.reset();
        result.check("destroy_clears_title", !rt::runtime() && !rcomp::active_guest_memory() &&
                     !rcomp::lookup_function(entry));
        if (result.failures) break;
    }
    return result.finish();
}
} // namespace

extern "C" int rcomp_xex_selftest(FILE* out, const char* path) { return run(out, path); }
