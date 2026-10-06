// Local PS5 entry point for an externally supplied, statically recompiled XEX.
// The image AND the extracted game's files are required; generated C++ is not edited.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <utility>
#include <vector>

#include "ppc_recomp_shared.h"
#include "rcomp/app/title.h"
#if RCOMP_M6_AOT_MODULES
// The other XEX DLLs of the disc, compiled into this title (rcomp/aot_modules.h; generated list
// ${M6_WORK}/aot_modules.cpp, see app/m6/CMakeLists.txt).
#include "rcomp/aot_modules.h"
#include "rcomp/runtime/modules.h"
extern const rcomp::AotModule rcomp_m6_aot_modules[];
extern const size_t rcomp_m6_aot_module_count;
#endif
#if RCOMP_M6_NATIVE_AUDIO_PROBE
extern "C" int rcomp_native_audio_probe();
#endif
#if defined(RCOMP_M6_RENDER_DIAGNOSTICS_START_FRAME) || defined(RCOMP_M6_XENOS_CVARS)
#include <rex/cvar.h>
#endif

#if RCOMP_M6_TIMEBASE_OBSERVER
#include <atomic>
#include "rcomp/runtime/video.h"
extern "C" void rcomp_timebase_probe_snapshot(FILE*, uint64_t, uint64_t);
namespace {
struct TimingObserver {
    rcomp::app::Title* title;
    std::atomic<bool> running{true};
};
void* observe_timing(void* argument) {
    auto& observer = *static_cast<TimingObserver*>(argument);
    while (observer.running.load(std::memory_order_acquire)) {
        rcomp_timebase_probe_snapshot(stderr, rcomp::rt::video_swaps_submitted(),
                                     observer.title->vblanks_issued());
        timespec delay{1, 0};
        nanosleep(&delay, nullptr);
    }
    return nullptr;
}
}  // namespace
#endif

#if defined(RCOMP_M6_DIAGNOSTIC_HEADLESS) && RCOMP_M6_CAPTURE_INTERVAL_SECONDS > 0
namespace {
// Exploratory title only: periodically save the last presented guest frame as a PPM next to
// the log so a run can be inspected remotely, and log a small content summary.
void* frame_dumper(void* argument) {
    auto* title = static_cast<rcomp::app::Title*>(argument);
    std::vector<uint8_t> frame;
    uint32_t sequence = 0;
    for (;;) {
        struct timespec delay = {RCOMP_M6_CAPTURE_INTERVAL_SECONDS, 0};
        nanosleep(&delay, nullptr);
        uint32_t w = 0, h = 0;
        if (!title->CaptureFrame(&frame, &w, &h) || !w || !h) {
            fprintf(stdout, "RCOMP-M6 frame none presented yet\n");
            continue;
        }
        uint64_t sum = 0, lit = 0;
        for (size_t i = 0; i + 3 < frame.size(); i += 4) {
            const unsigned v = frame[i] + frame[i + 1] + frame[i + 2];
            sum += frame[i] + 3u * frame[i + 1] + 5u * frame[i + 2] + i;
            lit += v > 24;
        }
        char path[64];
        char temporary[72];
        // Keep numbered captures (first 240), and the full-size latest frame.
        if (sequence < 240) {
            snprintf(path, sizeof path, "/app0/frame_%03u.ppm", sequence + 1);
            snprintf(temporary, sizeof temporary, "%s.tmp", path);
            if (FILE* small = fopen(temporary, "wb")) {
                const unsigned sw = w / 2, sh = h / 2;
                fprintf(small, "P6 %u %u 255\n", sw, sh);
                std::vector<uint8_t> rgb(size_t(sw) * sh * 3);
                for (unsigned y = 0; y < sh; ++y)
                    for (unsigned x = 0; x < sw; ++x) {
                        const size_t i = (size_t(y) * 2 * w + size_t(x) * 2) * 4, o = (size_t(y) * sw + x) * 3;
                        rgb[o] = frame[i]; rgb[o + 1] = frame[i + 1]; rgb[o + 2] = frame[i + 2];
                    }
                const bool written = fwrite(rgb.data(), 1, rgb.size(), small) == rgb.size();
                const bool closed = fclose(small) == 0;
                if (written && closed && rename(temporary, path) != 0)
                    fprintf(stderr, "RCOMP-M6 capture rename failed path=%s\n", path);
            }
        }
        snprintf(path, sizeof path, "/app0/frame_latest.ppm");
        snprintf(temporary, sizeof temporary, "%s.tmp", path);
        if (FILE* out = fopen(temporary, "wb")) {
            fprintf(out, "P6 %u %u 255\n", w, h);
            std::vector<uint8_t> rgb(size_t(w) * h * 3);
            for (size_t i = 0, o = 0; i + 3 < frame.size() && o + 2 < rgb.size(); i += 4, o += 3) {
                rgb[o] = frame[i]; rgb[o + 1] = frame[i + 1]; rgb[o + 2] = frame[i + 2];
            }
            const bool written = fwrite(rgb.data(), 1, rgb.size(), out) == rgb.size();
            const bool closed = fclose(out) == 0;
            if (written && closed && rename(temporary, path) != 0)
                fprintf(stderr, "RCOMP-M6 capture rename failed path=%s\n", path);
        }
        fprintf(stdout, "RCOMP-M6 frame #%u %ux%u lit_pixels=%llu checksum=%016llx\n", ++sequence, w, h,
                (unsigned long long)lit, (unsigned long long)sum);
    }
    return nullptr;
}
}  // namespace
#endif

extern "C" void rcomp_start_sampler();
#if RCOMP_M6_TOPOLOGY_PROBE
extern "C" void rcomp_topology_probe();
#endif
#if RCOMP_M6_LAUNCH_MENU
// The first "titleName" value of the app's param.json (the default language's display name), or
// "R-COMP" when the file or the key is missing. The launch menu only shows it.
static std::string app_title_name(const char* path) {
    std::string text;
    if (FILE* f = fopen(path, "r")) {
        char buffer[4096];
        size_t n = 0;
        while ((n = fread(buffer, 1, sizeof buffer, f)) > 0 && text.size() < 65536) text.append(buffer, n);
        fclose(f);
    }
    const size_t key = text.find("\"titleName\"");
    const size_t colon = key == std::string::npos ? key : text.find(':', key);
    const size_t open = colon == std::string::npos ? colon : text.find('"', colon);
    const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
    if (close == std::string::npos || close - open > 128) return "R-COMP";
    return text.substr(open + 1, close - open - 1);
}
#endif

extern "C" int rcomp_program_main(int, char**) {
#if RCOMP_M6_TOPOLOGY_PROBE
    rcomp_topology_probe();
#endif
#if RCOMP_M6_NATIVE_AUDIO_PROBE
    if (rcomp_native_audio_probe() != 0) return 7;
#endif
    // The decoded executable image is separate from the title's data mount.
    constexpr const char* kImage = "/app0/image/plain.xex";
    rcomp::app::TitleConfig cfg;
#if defined(RCOMP_M6_DRAW_RESOLUTION_SCALE)
    cfg.draw_resolution_scale_x = cfg.draw_resolution_scale_y = RCOMP_M6_DRAW_RESOLUTION_SCALE;
#endif
#if RCOMP_M6_DIRECT_GUEST_PRESENTATION
    cfg.direct_guest_presentation = true;
#endif
    // The frame-rate counter is on for every title (TitleConfig default); only an explicit -DRCOMP_M6_FPS_COUNTER=OFF build turns it off.
#if defined(RCOMP_M6_FPS_COUNTER) && !RCOMP_M6_FPS_COUNTER
    cfg.fps_counter = false;
#endif
    cfg.log = stdout;
#if defined(RCOMP_M6_DIAGNOSTIC_HEADLESS)
    // Exploratory boot title: the GPU runs offscreen and RCOMP_M6_GAME_ROOT names
    // the extracted game files installed once outside the title folder.
    cfg.game_root = RCOMP_M6_GAME_ROOT;
#ifdef RCOMP_M6_CACHE_ROOT
    cfg.cache_root = RCOMP_M6_CACHE_ROOT;
    cfg.cache_read_only = RCOMP_M6_CACHE_READ_ONLY;
#endif
    cfg.screen = true;
    cfg.require_display_plane = false;
#else
    cfg.game_root = "/app0/game";
    cfg.require_display_plane = true;
#endif
    // Saves and profile settings of the local player live in the title's own app folder (/app0 is
    // /data/homebrew/<title id>), which R-comp Installer never deletes, so they survive updates.
    cfg.save_root = "/app0/savedata";
#if RCOMP_M6_HDD
    // The Xbox 360 hard drive (utility partitions, runtime/docs/HDD.md) beside the saves, in the same
    // app folder R-comp Installer never deletes: the title's cache survives updates.
    cfg.hdd_root = "/app0/hdd";
#endif
#if RCOMP_M6_CONTENT_SELFTEST
    cfg.content_selftest = true;
#endif
#if RCOMP_PHYSICAL_4K_WINDOW_OFFSET
    // The generated code applies the console's 0xE0000000 shift: the runtime hands out the matching windows.
    cfg.physical_4k_window_offset = true;
#endif
#if RCOMP_M6_LAUNCH_MENU
    // Launch options (internal resolution) for every title, remembered beside the saves. The name shown is
    // the app's own display name from its param.json.
    cfg.launch_menu = true;
    cfg.launch_menu_countdown = RCOMP_M6_LAUNCH_MENU_COUNTDOWN;
    cfg.launch_options_path = cfg.save_root + "/rcomp_options.ini";
    cfg.launch_menu_title = app_title_name("/app0/sce_sys/param.json");
#endif
    // Guest profile: 1280x720@60 HDMI, English. Values follow the public rexglue-sdk
    // c94f5eb xam_info.cpp profile that runs this title (AV pack 6, region 0xFFFF);
    // they are compatibility values, not a claim about the PS5 or a retail console.
    cfg.display_width = 1280;
    cfg.display_height = 720;
    cfg.refresh_hz = 60;
    cfg.configure_kernel_variables = true;
    cfg.configure_xam = true;
    cfg.xam_profile.language = 1;
    cfg.xam_profile.game_region = 0xFFFF;
    cfg.xam_profile.av_pack = 6;
    cfg.xam_profile.video.display_width = cfg.display_width;
    cfg.xam_profile.video.display_height = cfg.display_height;
    cfg.xam_profile.video.widescreen = true;
    cfg.xam_profile.video.high_definition = true;
    cfg.xam_profile.video.refresh_rate_hz = float(cfg.refresh_hz);
    cfg.configure_xconfig = true;
    cfg.xconfig_profile.av_region = rcomp::rt::XConfigAvRegion::Pal60;
    cfg.xconfig_profile.video.connector = rcomp::rt::XConfigVideoConnector::Hdmi;
    cfg.xconfig_profile.video.display_width = cfg.display_width;
    cfg.xconfig_profile.video.display_height = cfg.display_height;
    cfg.xconfig_profile.video.refresh_millihz = 60000;
    cfg.xconfig_profile.video.widescreen = true;
    cfg.xconfig_profile.video.high_definition = true;
    cfg.xconfig_profile.language = 1;
    FILE* f = fopen(kImage, "rb");
    if (!f) {
        fprintf(stderr, "RCOMP-M6 error=cannot-open-xex path=%s\n", kImage);
        return 2;
    }
    uint8_t block[64 * 1024];
    for (size_t n; (n = fread(block, 1, sizeof block, f)) != 0;)
        cfg.xex.insert(cfg.xex.end(), block, block + n);
    if (ferror(f)) {
        fprintf(stderr, "RCOMP-M6 error=read-xex path=%s\n", kImage);
        fclose(f);
        return 2;
    }
    fclose(f);
    for (PPCFuncMapping* m = PPCFuncMappings; m->host; ++m)
        cfg.functions.push_back({static_cast<uint32_t>(m->guest), m->host, nullptr});
    fprintf(stdout, "RCOMP-M6 image_bytes=%zu functions=%zu game_root=%s require_display_plane=1\n",
            cfg.xex.size(), cfg.functions.size(), cfg.game_root.c_str());
#if RCOMP_M6_AOT_MODULES
    if (!rcomp::register_aot_modules(rcomp_m6_aot_modules, rcomp_m6_aot_module_count)) {
        fprintf(stderr, "RCOMP-M6 error=aot-modules count=%zu\n", rcomp_m6_aot_module_count);
        return 2;
    }
    fprintf(stdout, "RCOMP-M6 aot_modules=%zu\n", rcomp_m6_aot_module_count);
    // The disc's DLLs are encrypted/compressed XEX files: R-comp Installer packages their host-decoded images
    // (no key on the console) under image/modules/<disc path>, which XexLoadImage reads after checking them
    // against the disc file's header (runtime/docs/MODULES.md).
    if (rcomp::rt::runtime_configure_module_images("/app0/image/modules") != rcomp::rt::Status::Ok) {
        fprintf(stderr, "RCOMP-M6 error=aot-module-images\n");
        return 2;
    }
#endif
    fflush(stdout);
    std::string error;
#if defined(RCOMP_M6_RENDER_DIAGNOSTICS_START_FRAME)
    if (!rex::cvar::SetFlagByName("rcomp_render_diagnostics", "true") ||
        !rex::cvar::SetFlagByName("rcomp_render_diag_start_frame",
                                std::to_string(RCOMP_M6_RENDER_DIAGNOSTICS_START_FRAME)) ||
        !rex::cvar::SetFlagByName("rcomp_render_diag_frames", "8")) {
        fprintf(stderr, "RCOMP-RENDER-DIAG FAIL configure\n");
        return 6;
    }
    fprintf(stderr, "RCOMP-RENDER-DIAG configured first=%u frames=8\n",
            unsigned(RCOMP_M6_RENDER_DIAGNOSTICS_START_FRAME));
#endif
#if defined(RCOMP_M6_XENOS_CVARS)
    // Experiment knobs for the backend's own flags: NAME=VALUE pairs, comma separated (the CMake
    // pattern guarantees the shape). The flags are registered before main; the GPU reads them later.
    {
        char pairs[] = RCOMP_M6_XENOS_CVARS;
        for (char* item = strtok(pairs, ","); item; item = strtok(nullptr, ",")) {
            char* equals = strchr(item, '=');
            *equals = 0;
            if (!rex::cvar::SetFlagByName(item, equals + 1)) {
                std::fprintf(stderr, "RCOMP-XENOS-CVAR FAIL %s=%s\n", item, equals + 1);
                return 6;
            }
            std::fprintf(stderr, "RCOMP-XENOS-CVAR %s=%s\n", item, equals + 1);
        }
    }
#endif
#if defined(RCOMP_M6_TUNING_ENV)
    // Experiment knobs: NAME=VALUE pairs, comma separated, set before any subsystem reads its
    // environment (the driver's wait tuning, the runtime's spin knob). Logged for the evidence.
    {
        char pairs[] = RCOMP_M6_TUNING_ENV;
        for (char* item = strtok(pairs, ","); item; item = strtok(nullptr, ",")) {
            char* equals = strchr(item, '=');
            if (!equals || equals == item) {
                std::fprintf(stderr, "RCOMP-TUNING ignored '%s'\n", item);
                continue;
            }
            *equals = 0;
            setenv(item, equals + 1, 1);
            std::fprintf(stderr, "RCOMP-TUNING %s=%s\n", item, equals + 1);
        }
    }
#endif
#if defined(RCOMP_M6_STORE_COMPARE)
    std::fprintf(stderr, "RCOMP-STORE-COMPARE on (guest stores inside the physical windows mark a page only when they change bytes)\n");
#endif
#if defined(RCOMP_M6_RADV_DEBUG)
    // Opt-in RADV_DEBUG flags, read by RADV when the Vulkan instance is created.
    setenv("RADV_DEBUG", RCOMP_M6_RADV_DEBUG, 1);
    std::fprintf(stderr, "RCOMP-RADV-DEBUG requested=%s effective=%s\n", RCOMP_M6_RADV_DEBUG,
                 getenv("RADV_DEBUG") ? getenv("RADV_DEBUG") : "(unset)");
#endif
#if defined(RCOMP_M6_RADV_PERFTEST)
    setenv("RADV_PERFTEST", RCOMP_M6_RADV_PERFTEST, 1);
    std::fprintf(stderr, "RCOMP-RADV-PERFTEST requested=%s\n", RCOMP_M6_RADV_PERFTEST);
#endif
    auto title = rcomp::app::Title::Create(std::move(cfg), &error);
    if (!title) {
        fprintf(stderr, "RCOMP-M6 error=create detail=%s\n", error.c_str());
        return 3;
    }
#if defined(RCOMP_M6_DIAGNOSTIC_HEADLESS)
#if defined(RCOMP_M6_PC_SAMPLER)
    rcomp_start_sampler();
#endif
#if RCOMP_M6_CAPTURE_INTERVAL_SECONDS > 0
    {
        pthread_t dumper;
        if (pthread_create(&dumper, nullptr, frame_dumper, title.get()) == 0) pthread_detach(dumper);
    }
#endif
#endif
#if RCOMP_M6_TIMEBASE_OBSERVER
    TimingObserver observer{title.get()};
    pthread_t timing_thread;
    if (pthread_create(&timing_thread, nullptr, observe_timing, &observer) != 0) {
        fprintf(stderr, "RCOMP-TIMEBASE FAIL observer-thread-create\n");
        return 5;
    }
#endif
    uint32_t code = 0;
    const bool ran = title->RunEntry(0, &code, &error);
#if RCOMP_M6_TIMEBASE_OBSERVER
    observer.running.store(false, std::memory_order_release);
    if (pthread_join(timing_thread, nullptr) != 0) {
        fprintf(stderr, "RCOMP-TIMEBASE FAIL observer-thread-join\n");
        return 5;
    }
#endif
    if (!ran) {
        fprintf(stderr, "RCOMP-M6 error=run-entry detail=%s\n", error.c_str());
        return 4;
    }
    // A returned entry point is not evidence of a menu, a first guest frame or gameplay.
    fprintf(stdout, "RCOMP-M6 entry=0x%08x exit=%u gameplay=NOT_TESTED\n", title->entry_point(), code);
    return code == 0 ? 0 : 5;
}
