#include "rcomp/app/title_runtime.h"
#include "rcomp/app/content_selftest.h"

#include "rcomp/diag.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/hdd.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/modules.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/wait_stats.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/kernel_variables.h"
#include "rcomp/runtime/xconfig.h"
#include "rcomp/runtime/video.h"
#include "rcomp/runtime/xam_content.h"
#include "rcomp/runtime/xex_loader.h"
#include "rcomp/runtime_state.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

namespace rcomp::app {
namespace rt = rcomp::rt;
namespace {
bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}
}

std::unique_ptr<TitleRuntime> TitleRuntime::Create(const TitleConfig& cfg, std::string* error) {
    // Creation/destruction are serialized by the owner, as for Title.
    if (rt::runtime() || active_guest_memory())
        return fail(error, "a guest runtime is already active"), nullptr;
    auto t = std::unique_ptr<TitleRuntime>(new TitleRuntime);
    if (t->mem_.reserve() != MemStatus::Ok)
        return fail(error, "guest memory reservation failed"), nullptr;
    rt::RuntimeConfig runtime_config;
    runtime_config.physical_4k_window_offset = cfg.physical_4k_window_offset;
    if (rt::runtime_init(&t->mem_, runtime_config) != rt::Status::Ok)
        return fail(error, "runtime_init failed"), nullptr;
    t->owned_ = true;
    t->stack_size_ = cfg.main_stack_size;
    t->log_ = cfg.log;
    set_active_guest_memory(&t->mem_);
    rt::clear_imports();
    if (rt::register_xboxkrnl_hle() != rt::Status::Ok || rt::register_xam_hle() != rt::Status::Ok)
        return fail(error, "kernel/XAM HLE registration failed"), nullptr;
    if (cfg.configure_xam) {
        const auto& mode = cfg.xam_profile.video;
        if (mode.display_width != cfg.display_width || mode.display_height != cfg.display_height ||
            mode.refresh_rate_hz != float(cfg.refresh_hz))
            return fail(error, "XAM video profile differs from the configured title display"), nullptr;
        if (rt::runtime_configure_xam(cfg.xam_profile) != rt::Status::Ok)
            return fail(error, "XAM guest profile configuration failed"), nullptr;
    }
    if (cfg.configure_xconfig) {
        if (cfg.xconfig_profile.video.display_width != cfg.display_width ||
            cfg.xconfig_profile.video.display_height != cfg.display_height ||
            uint64_t(cfg.xconfig_profile.video.refresh_millihz) != uint64_t(cfg.refresh_hz) * 1000 ||
            (cfg.configure_xam && cfg.xconfig_profile.language != cfg.xam_profile.language))
            return fail(error, "XConfig and title/XAM profiles disagree"), nullptr;
        const auto configured = rt::runtime_configure_xconfig(cfg.xconfig_profile);
        if (configured != rt::Status::Ok)
            return fail(error, std::string("XConfig profile: ") + rt::status_name(configured)), nullptr;
    }
    if (cfg.configure_kernel_variables) {
        const auto configured = rt::register_xboxkrnl_kernel_variables_from_xex(
            cfg.xex.data(), cfg.xex.size(), cfg.log);
        if (configured != rt::Status::Ok)
            return fail(error, std::string("kernel compatibility profile: ") + rt::status_name(configured)), nullptr;
    }
    // Imports are relocated once, so video variables must exist now.
    if (rt::register_xboxkrnl_video_hle() != rt::Status::Ok)
        return fail(error, "video HLE registration failed"), nullptr;
    if (rt::register_thread_object_type_variable() != rt::Status::Ok)
        return fail(error, "thread object type identity preparation failed"), nullptr;
    auto s = rt::runtime_prepare_main_module({cfg.guest_path, cfg.command_line});
    if (s != rt::Status::Ok)
        return fail(error, std::string("module preparation: ") + rt::status_name(s)), nullptr;
    if (!cfg.game_root.empty()) {
        for (const char* device : {"game:", "d:"}) {
            auto s = rt::runtime()->vfs.mount(device, cfg.game_root);
            if (s != rt::Status::Ok)
                return fail(error, std::string("mount ") + device + ": " + rt::status_name(s)), nullptr;
        }
    }
    if (!cfg.cache_root.empty()) {
        // The HDD cache partitions, created on first use. Writable, the title builds its valid.txt files in them itself and installs archives into them; read-only
        // (cache_read_only), the valid.txt marker is created here, once, and the title finds a write-protected disk and streams from the disc.
        ::mkdir(cfg.cache_root.c_str(), 0755);
        for (const char* name : {"cache", "cache1"}) {
            const std::string directory = cfg.cache_root + "/" + name;
            ::mkdir(directory.c_str(), 0755);
            if (cfg.cache_read_only) {
                const int marker = ::open((directory + "/valid.txt").c_str(), O_WRONLY | O_CREAT, 0644);
                if (marker < 0)
                    return fail(error, std::string("cache marker ") + directory + "/valid.txt: " + strerror(errno)), nullptr;
                ::close(marker);
            }
            auto mounted = rt::runtime()->vfs.mount(std::string(name) + ":", directory,
                                                    cfg.cache_read_only ? rt::MountAccess::ReadOnly : rt::MountAccess::ReadWrite);
            if (mounted != rt::Status::Ok)
                return fail(error, std::string("mount ") + name + ": " + directory + ": " + rt::status_name(mounted)), nullptr;
        }
        if (cfg.log) {
            fprintf(cfg.log, "RCOMP-APP cache_mounts root=%s\n", cfg.cache_root.c_str());
            fflush(cfg.log);
        }
    }
    if (!cfg.save_root.empty()) {
        const rt::Status configured = rt::runtime_configure_save_root(cfg.save_root);
        if (configured != rt::Status::Ok)
            return fail(error, std::string("save root ") + cfg.save_root + ": " + rt::status_name(configured)), nullptr;
        if (cfg.log) {
            fprintf(cfg.log, "RCOMP-APP save_root=%s\n", cfg.save_root.c_str());
            fflush(cfg.log);
        }
    }
    if (!cfg.hdd_root.empty()) {
        const rt::Status configured = rt::runtime_configure_hdd(cfg.hdd_root);
        if (configured != rt::Status::Ok)
            return fail(error, std::string("hard drive ") + cfg.hdd_root + ": " + rt::status_name(configured)), nullptr;
        if (cfg.log) {
            fprintf(cfg.log, "RCOMP-APP hdd_root=%s\n", cfg.hdd_root.c_str());
            fflush(cfg.log);
        }
    }
    rt::XexImage img{};
    s = rt::load_xex_image(t->mem_, cfg.xex.data(), cfg.xex.size(), &img, cfg.log);
    if (s != rt::Status::Ok)
        return fail(error, std::string("XEX load: ") + rt::status_name(s)), nullptr;
    if (img.variables_unresolved)
        return fail(error, "XEX has " + std::to_string(img.variables_unresolved) +
                           " unresolved variable imports; inspect RCOMP-XEX diagnostics"), nullptr;
    s = rt::runtime_set_static_tls(img.tls);
    if (s != rt::Status::Ok)
        return fail(error, std::string("XEX TLS: ") + rt::status_name(s)), nullptr;
    if (!register_functions(cfg.functions.data(), cfg.functions.size()))
        return fail(error, "invalid or duplicate entries in the function table"), nullptr;
    if (!lookup_function(img.entry_point))
        return fail(error, "entry point has no recompiled function"), nullptr;
    s = rt::runtime_finalize_main_module(img);
    if (s != rt::Status::Ok)
        return fail(error, std::string("module finalization: ") + rt::status_name(s)), nullptr;
    t->entry_ = img.entry_point;
    if (cfg.content_selftest) run_content_selftest(cfg.log ? cfg.log : stderr);
    if (cfg.log) {
        fprintf(cfg.log, "RCOMP-APP stage=image_loaded entry=0x%08X variables=%u unresolved=%u\n",
                img.entry_point, unsigned(img.variables_resolved), unsigned(img.variables_unresolved));
        fflush(cfg.log);
    }
    return t;
}

TitleRuntime::~TitleRuntime() {
    if (!owned_) return;
    // Shutdown waits for secondary guest execution while imports/memory live.
    rt::runtime_shutdown();
    rt::clear_imports();
    clear_functions();
    if (active_guest_memory() == &mem_) set_active_guest_memory(nullptr);
}

bool TitleRuntime::RunEntry(uint32_t arg, uint32_t* exit_code, std::string* error) {
    auto* r = rt::runtime();
    if (!exit_code || !r || r->mem != &mem_)
        return fail(error, "entry execution needs this title's runtime and an exit-code destination");
    PPCFunc* fn = lookup_function(entry_);
    if (!fn) return fail(error, "entry point has no recompiled function");
    alignas(64) PPCContext ctx{};
    rt::GuestThread thread;
    auto s = rt::create_guest_thread(r->heap, {stack_size_, entry_, arg}, &ctx, &thread);
    if (s != rt::Status::Ok) return fail(error, "main guest thread creation failed");
    ctx.fpscr.loadFromHost();
    if (log_) {
        fprintf(log_, "RCOMP-APP stage=entry_begin pc=0x%08X\n", entry_);
        fflush(log_);
    }
    rt::note_main_guest_thread();  // its CPU time joins the RCOMP-THREADS report
    s = rt::run_guest_thread(thread, ctx, mem_.base(), fn, exit_code);
    const auto cleanup = rt::destroy_guest_thread(r->heap, &thread);
    // This stack-local owner cannot retain allocations for a later retry.
    // A failed cleanup must stop here rather than report a completed entry
    // and abandon the remaining guest stack/PCR/TLS allocations.
    if (cleanup != rt::Status::Ok)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "main guest thread cleanup: %s", rt::status_name(cleanup));
    if (s != rt::Status::Ok) return fail(error, std::string("run_guest_thread: ") + rt::status_name(s));
    if (log_) {
        fprintf(log_, "RCOMP-APP stage=entry_returned code=0x%08X\n", *exit_code);
        fflush(log_);
    }
    return true;
}
} // namespace rcomp::app
