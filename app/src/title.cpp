// R-comp game shell (owner: PRIME, app/). See rcomp/app/title.h.
#include "rcomp/app/title.h"
#include "rcomp/app/launch_menu.h"
#include "rcomp/app/title_runtime.h"

#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <rcomp_xenos/host_thread.h>

#include "../../gpu/xenos/rexglue/rcomp/vulkan_output.h"
#include "rcomp/diag.h"
#include "rcomp/fast_clock.h"
#include "rcomp/guest_memory.h"
#include "rcomp/input.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/vfs.h"
#include "rcomp/runtime/video.h"
#include "rcomp/runtime/wait_stats.h"
#include "rcomp/runtime/xex_loader.h"
#include "rcomp/runtime_state.h"

namespace rcomp::app {

namespace rt = rcomp::rt;
namespace xg = rcomp::xenos;

struct Title::Impl {
    TitleConfig cfg;
    std::unique_ptr<TitleRuntime> runtime;
    bool gpu_started = false;
    std::atomic<bool> screen_running{false};
    std::atomic<uint64_t> frames{0};
    xg::HostThread screen_thread;
};

uint64_t Title::vblanks_issued() const {
    const auto* host = impl_ && impl_->gpu_started ? xg::gpu_host() : nullptr;
    return host ? host->vblank_count() : 0;
}

namespace {
bool fail(std::string* error, const std::string& what) {
    if (error) *error = what;
    return false;
}

// The launch options menu (rcomp/app/launch_menu.h) on `display`, before the GPU starts: reads the
// remembered choice, lets the player change it with the controller, saves it and applies the resolution.
bool run_launch_menu(const TitleConfig& cfg, xg::VulkanOutput& output, xg::VulkanDisplay& display,
                     std::string* error) {
    LaunchOptions options;
    options.draw_scale = cfg.draw_resolution_scale_x;
    if (!cfg.launch_options_path.empty()) {
        if (FILE* f = fopen(cfg.launch_options_path.c_str(), "r")) {
            char text[256] = {};
            const size_t n = fread(text, 1, sizeof text - 1, f);
            fclose(f);
            parse_launch_options(std::string(text, n), &options);
        }
    }
    LaunchMenu menu(cfg.launch_menu_title, options, cfg.launch_menu_countdown);
    constexpr uint32_t kWidth = 1280, kHeight = 720;
    std::vector<uint8_t> frame;
    std::string err;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        rcomp_pad pad{};
        const uint16_t buttons = rcomp_input_read(0, &pad) == RCOMP_INPUT_OK ? pad.buttons : 0;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const bool done = menu.update(buttons, now);
        menu.render(&frame, kWidth, kHeight, now);
        if (!display.Present(frame.data(), kWidth, kHeight, nullptr, &err)) return fail(error, "launch menu: " + err);
        if (done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    const LaunchOptions chosen = menu.options();
    if (!cfg.launch_options_path.empty()) {
        // Remembered for the next launch (write then rename, so a crash never leaves a partial file).
        const std::string tmp = cfg.launch_options_path + ".tmp";
        const std::string text = serialize_launch_options(chosen);
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f || fwrite(text.data(), 1, text.size(), f) != text.size() || fclose(f) != 0 ||
            rename(tmp.c_str(), cfg.launch_options_path.c_str()) != 0)
            fprintf(cfg.log ? cfg.log : stderr, "RCOMP-APP launch_options not saved to %s\n",
                    cfg.launch_options_path.c_str());
    }
    if (!output.SetDrawResolutionScale(chosen.draw_scale, chosen.draw_scale, &err)) return fail(error, "launch menu: " + err);
    rt::video_set_frame_rate_cap(chosen.frame_rate_cap);
    if (cfg.log) {
        fprintf(cfg.log, "RCOMP-APP launch_options draw_scale=%u (%s) frame_rate_cap=%u (%s) after %.1f s\n",
                chosen.draw_scale, draw_scale_label(chosen.draw_scale), chosen.frame_rate_cap,
                frame_rate_cap_label(chosen.frame_rate_cap),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        fflush(cfg.log);
    }
    return true;
}
}  // namespace

std::unique_ptr<Title> Title::Create(TitleConfig cfg, std::string* error) {
    // Runtime, import tables and active memory are process-wide. Creation and
    // destruction are serialized by the caller; a second title must not tear
    // down the first one on its failure path.
    if (rt::runtime() || active_guest_memory())
        return fail(error, "a guest runtime is already active"), nullptr;
    if (cfg.require_display_plane && !cfg.screen)
        return fail(error, "a required display plane cannot have screen disabled"), nullptr;
    std::unique_ptr<Title> t(new Title());
    t->impl_ = std::make_unique<Impl>();
    Impl& im = *t->impl_;
    im.cfg = std::move(cfg);
    FILE* log = im.cfg.log;

    // The same production bootstrap is exercised by CPU-only host rehearsals.
    im.runtime = TitleRuntime::Create(im.cfg, error);
    if (!im.runtime) return nullptr;
    t->entry_ = im.runtime->entry_point();

    // GPU: Vulkan backend. Video HLE was registered before image relocation.
    std::string e;
    t->output_ = xg::VulkanOutput::Create(im.cfg.screen, &e, im.cfg.draw_resolution_scale_x, im.cfg.draw_resolution_scale_y);
    if (!t->output_ && im.cfg.screen && !im.cfg.require_display_plane) {
        t->screen_status_ = "no screen output: " + e;
        t->output_ = xg::VulkanOutput::Create(false, &e, im.cfg.draw_resolution_scale_x, im.cfg.draw_resolution_scale_y);
    }
    if (!t->output_) return fail(error, "Vulkan: " + e), nullptr;
    // Launch options: the screen exists before the GPU, which reads the chosen resolution when it starts.
    if (im.cfg.launch_menu && im.cfg.screen && t->screen_status_.empty()) {
        t->display_ = t->output_->CreateDisplay(im.cfg.display_width, im.cfg.display_height, &e);
        if (!t->display_) {
            t->screen_status_ = "no screen: " + e;
            if (im.cfg.require_display_plane) return fail(error, t->screen_status_), nullptr;
        } else if (!run_launch_menu(im.cfg, *t->output_, *t->display_, error)) {
            return nullptr;
        }
    }
    xg::HostConfig hc;
    hc.display_width = im.cfg.display_width;
    hc.display_height = im.cfg.display_height;
    hc.vblank_hz = im.cfg.refresh_hz;
    hc.refresh_hz = float(im.cfg.refresh_hz);
    t->output_->Attach(hc);
    if (!xg::gpu_start(im.runtime->memory(), hc, rt::dispatch_graphics_interrupt, t->output_->backend_factory()))
        return fail(error, "GPU start failed"), nullptr;
    im.gpu_started = true;

    // Screen: the presented frame, refreshed at the display rate.
    if (im.cfg.screen && t->screen_status_.empty()) {
        if (!t->display_) t->display_ = t->output_->CreateDisplay(im.cfg.display_width, im.cfg.display_height, &e);
        if (!t->display_) {
            t->screen_status_ = "no screen: " + e;
            if (im.cfg.require_display_plane) return fail(error, t->screen_status_), nullptr;
        } else {
            if (im.cfg.require_display_plane && t->display_->kind() != xg::VulkanDisplay::Kind::kDisplayPlane)
                return fail(error, "PS5 display plane required; headless surface is not a screen"), nullptr;
            im.screen_running = true;
            Title* self = t.get();
            im.screen_thread.Start([self] {
                Impl& i = *self->impl_;
                const auto period = std::chrono::microseconds(1000000 / (i.cfg.refresh_hz ? i.cfg.refresh_hz : 60));
                auto next = std::chrono::steady_clock::now();
                std::vector<uint8_t> frame;
                std::string err;
                // Frame-rate counter (opt-in): guest swaps per second, from the counter
                // the RCOMP-VD swap lines and the timebase observer also use.
                uint64_t fps_swaps = rt::video_swaps_submitted();
                auto fps_start = next;
                int fps_overlay_state = -1;  // -1 not reported yet, 0 unavailable, 1 active
                // Process CPU time (all threads), to report how many cores the title keeps busy.
                auto process_cpu_seconds = [] {
                    struct rusage usage;
                    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1.0;
                    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                           1e-6 * double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
                };
                double fps_cpu = process_cpu_seconds();
                // Per-second page faults (minor:major), context switches (voluntary:involuntary) and the peak resident size of the process: a frame rate that falls while
                // the faults or the involuntary switches rise points at memory pressure or at threads fighting for CPUs, not at the guest or the GPU.
                struct SystemUsage { long minflt = 0, majflt = 0, nvcsw = 0, nivcsw = 0, maxrss = 0; };
                auto read_system_usage = [] {
                    SystemUsage u;
                    struct rusage usage;
                    if (getrusage(RUSAGE_SELF, &usage) == 0) {
                        u.minflt = usage.ru_minflt;
                        u.majflt = usage.ru_majflt;
                        u.nvcsw = usage.ru_nvcsw;
                        u.nivcsw = usage.ru_nivcsw;
                        u.maxrss = usage.ru_maxrss;
                    }
                    return u;
                };
                SystemUsage fps_system = read_system_usage();
                uint64_t fps_fsyncs = rt::vfs_fsync_count();  // fsync() calls of the guest's file system per second
                // Cost of one steady_clock::now() (a system call on the console): the mean of 128 back-to-back calls timed on the TSC clock. A kernel clock source that degrades
                // (the TSC declared unstable, HPET reads) shows up here, and every wait that polls the clock pays for it.
                auto clock_call_ns = [] {
                    constexpr int kCalls = 128;
                    volatile int64_t sink = 0;
                    const uint64_t begin = rcomp::fast_monotonic_ns();
                    for (int i = 0; i < kCalls; ++i) sink = std::chrono::steady_clock::now().time_since_epoch().count();
                    (void)sink;
                    return double(rcomp::fast_monotonic_ns() - begin) / kCalls;
                };
                // Swaps by the vblanks since the previous swap: which pacing regime each second was in.
                uint64_t fps_pacing[8] = {};
                rt::video_swap_vblank_histogram(fps_pacing);
                rt::WaitStats fps_waits = rt::wait_stats();
                uint64_t fps_window_fallbacks = virtual_window_fallback_count();
                while (i.screen_running) {
                    if (i.cfg.direct_guest_presentation) {
                        if (self->output_->PresentGuestFrame(*self->display_, &err))
                            i.frames.fetch_add(1);
                        else if (!err.empty())
                            rcomp_fatal(RCOMP_FATAL_PLATFORM, "screen: %s", err.c_str());
                    } else {
                        uint32_t w = 0, h = 0;
                        // Nothing presented yet: nothing to show.
                        if (self->output_->Capture(&frame, &w, &h) && w && h) {
                            if (!self->display_->Present(frame.data(), w, h, nullptr, &err))
                                rcomp_fatal(RCOMP_FATAL_PLATFORM, "screen: %s", err.c_str());
                            i.frames.fetch_add(1);
                        }
                    }
                    if (i.cfg.fps_counter) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now - fps_start >= std::chrono::seconds(1)) {
                            const uint64_t swaps = rt::video_swaps_submitted();
                            const double seconds = std::chrono::duration<double>(now - fps_start).count();
                            const double fps = double(swaps - fps_swaps) / seconds;
                            const double frame_ms = fps > 0.05 ? 1000.0 / fps : 0.0;
                            const double cpu = process_cpu_seconds();
                            const double cores = cpu >= 0 && fps_cpu >= 0 ? (cpu - fps_cpu) / seconds : -1.0;
                            fps_cpu = cpu;
                            fps_swaps = swaps;
                            fps_start = now;
                            uint64_t pacing[8];
                            rt::video_swap_vblank_histogram(pacing);
                            char vblanks[96];
                            size_t used = 0;
                            vblanks[0] = 0;
                            for (unsigned k = 0; k < 8; ++k) {
                                if (pacing[k] == fps_pacing[k]) continue;
                                used += size_t(snprintf(vblanks + used, sizeof vblanks - used, "%s%u:%llu", used ? "," : "", k,
                                                        (unsigned long long)(pacing[k] - fps_pacing[k])));
                                if (used >= sizeof vblanks) break;
                            }
                            if (!used) snprintf(vblanks, sizeof vblanks, "-");
                            memcpy(fps_pacing, pacing, sizeof pacing);
                            // Wait outcomes of the second (only when the runtime counts them): ready at once, polls
                            // that timed out, sleeps.
                            const rt::WaitStats waits = rt::wait_stats();
                            char wait_field[80] = "";
                            if (waits.ready_at_once + waits.immediate_timeouts + waits.blocked)
                                snprintf(wait_field, sizeof wait_field, " waits=%llu:%llu:%llu",
                                         (unsigned long long)(waits.ready_at_once - fps_waits.ready_at_once),
                                         (unsigned long long)(waits.immediate_timeouts - fps_waits.immediate_timeouts),
                                         (unsigned long long)(waits.blocked - fps_waits.blocked));
                            fps_waits = waits;
                            // Guest accesses of the coarse range that no provider claimed (heap above the arena): only
                            // printed when there were some, as they are expected to be none.
                            char window_field[40] = "";
                            const uint64_t window_fallbacks = virtual_window_fallback_count();
                            if (window_fallbacks != fps_window_fallbacks)
                                snprintf(window_field, sizeof window_field, " vwin=%llu",
                                         (unsigned long long)(window_fallbacks - fps_window_fallbacks));
                            fps_window_fallbacks = window_fallbacks;
                            const SystemUsage system_now = read_system_usage();
                            fprintf(stderr, "RCOMP-FPS fps=%.2f frame_ms=%.2f swaps=%llu cpu_cores=%.2f vb=%s%s%s flt=%ld:%ld cs=%ld:%ld rss_kb=%ld clk_ns=%.0f fsync=%llu\n", fps,
                                    frame_ms, (unsigned long long)swaps, cores, vblanks, wait_field, window_field,
                                    system_now.minflt - fps_system.minflt, system_now.majflt - fps_system.majflt,
                                    system_now.nvcsw - fps_system.nvcsw, system_now.nivcsw - fps_system.nivcsw, system_now.maxrss,
                                    clock_call_ns(), (unsigned long long)(rt::vfs_fsync_count() - fps_fsyncs));
                            fps_fsyncs = rt::vfs_fsync_count();
                            fps_system = system_now;
                            // Every 5 s: CPU milliseconds of each guest thread over those seconds (RCOMP-THREADS;
                            // runtime/include/rcomp/runtime/wait_stats.h). A thread near 5,000 paces the frame.
                            static unsigned fps_seconds = 0;
                            if (++fps_seconds % 5 == 0) {
                                char threads[1536];
                                if (rt::guest_thread_cpu_report(threads, sizeof threads))
                                    fprintf(stderr, "RCOMP-THREADS seconds=5%s\n", threads);
                                // Wall milliseconds each thread spent blocked in waits and sleeps (RCOMP-WAITMS): with the
                                // CPU milliseconds above, the rest of the 5 s is yields and polls.
                                if (rt::guest_thread_wait_report(threads, sizeof threads))
                                    fprintf(stderr, "RCOMP-WAITMS seconds=5%s\n", threads);
                            }
                            char text[48];
                            snprintf(text, sizeof text, "FPS %.1f  %.1f MS", fps, frame_ms);
                            std::string overlay_error;
                            const bool overlay_ok = self->display_->SetOverlayText(text, &overlay_error);
                            if (int(overlay_ok) != fps_overlay_state) {  // the first state and any change
                                fps_overlay_state = int(overlay_ok);
                                if (overlay_ok)
                                    fprintf(stderr, "RCOMP-FPS overlay active: \"%s\"\n", text);
                                else
                                    fprintf(stderr, "RCOMP-FPS overlay unavailable: %s\n", overlay_error.c_str());
                            }
                        }
                    }
                    next += period;
                    std::this_thread::sleep_until(next);
                }
            });
            t->screen_status_ = std::string("screen: ") +
                                (t->display_->kind() == xg::VulkanDisplay::Kind::kDisplayPlane ? "display plane "
                                                                                              : "headless ") +
                                std::to_string(t->display_->width()) + "x" + std::to_string(t->display_->height());
        }
    }
    if (log) fprintf(log, "RCOMP-APP ready entry=0x%08X device=%s %s\n", t->entry_, t->output_->device_name().c_str(),
                     t->screen_status_.c_str());
    return t;
}

Title::~Title() {
    if (!impl_) return;
    if (impl_->runtime) rt::runtime_quiesce_threads();
    impl_->screen_running = false;
    impl_->screen_thread.join();
    display_.reset();
    if (impl_->gpu_started) xg::gpu_stop();
    output_.reset();
    impl_->runtime.reset();
}

bool Title::RunEntry(uint32_t arg, uint32_t* exit_code, std::string* error) {
    return impl_->runtime->RunEntry(arg, exit_code, error);
}

bool Title::CaptureFrame(std::vector<uint8_t>* rgbx, uint32_t* width, uint32_t* height) const {
    return output_ && output_->Capture(rgbx, width, height);
}

uint64_t Title::frames_shown() const { return impl_->frames.load(); }

}  // namespace rcomp::app
