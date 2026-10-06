// Host-only execution to discover the first real unsupported title operation.
// No HLE implementation is replaced; the explicit test GPU boundary aborts.
#include <cstdio>
#include <string>
#include <vector>
#include "ppc_recomp_shared.h"
#include "rcomp/app/title_runtime.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: rcomp_title_entry_diagnostic PLAIN_XEX GAME_ROOT\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    rcomp::app::TitleConfig cfg;
    cfg.log = stdout;
    cfg.screen = false;
    cfg.configure_kernel_variables = true;
    cfg.configure_xconfig = true;
    cfg.display_width = 1280;
    cfg.display_height = 720;
    cfg.refresh_hz = 60;
    cfg.xconfig_profile.av_region = rcomp::rt::XConfigAvRegion::Pal60;
    cfg.xconfig_profile.video.connector = rcomp::rt::XConfigVideoConnector::Hdmi;
    cfg.xconfig_profile.video.display_width = cfg.display_width;
    cfg.xconfig_profile.video.display_height = cfg.display_height;
    cfg.xconfig_profile.video.refresh_millihz = 60000;
    cfg.xconfig_profile.video.widescreen = true;
    cfg.xconfig_profile.video.high_definition = true;
    cfg.xconfig_profile.language = 1;
    // Values follow the public rexglue-sdk c94f5eb xam_info.cpp profile that
    // runs this title (AV pack 6, region 0xFFFF, English); they are a
    // compatibility profile, not a claim about the PS5 or a retail console.
    cfg.configure_xam = true;
    cfg.xam_profile.language = 1;
    cfg.xam_profile.game_region = 0xFFFF;
    cfg.xam_profile.av_pack = 6;
    cfg.xam_profile.video.display_width = cfg.display_width;
    cfg.xam_profile.video.display_height = cfg.display_height;
    cfg.xam_profile.video.interlaced = false;
    cfg.xam_profile.video.widescreen = true;
    cfg.xam_profile.video.high_definition = true;
    cfg.xam_profile.video.refresh_rate_hz = float(cfg.refresh_hz);
    cfg.game_root = argv[2];
    cfg.guest_path = "game:\\default.xex";
    FILE* file = std::fopen(argv[1], "rb");
    if (!file) return 2;
    constexpr size_t limit = 128 * 1024 * 1024;
    uint8_t bytes[65536];
    bool ok = true;
    while (const size_t n = std::fread(bytes, 1, sizeof(bytes), file)) {
        if (n > limit - cfg.xex.size()) { ok = false; break; }
        cfg.xex.insert(cfg.xex.end(), bytes, bytes + n);
    }
    ok = !std::ferror(file) && ok;
    ok = std::fclose(file) == 0 && ok;
    if (!ok) return 2;
    for (PPCFuncMapping* mapping = PPCFuncMappings; mapping->host; ++mapping)
        cfg.functions.push_back({uint32_t(mapping->guest), mapping->host, nullptr});
    std::fprintf(stdout,
        "RCOMP-ENTRY-DIAGNOSTIC scope=host-only graphics=NOT_TESTED gameplay=NOT_TESTED "
        "release_support=BLOCKED image_bytes=%zu functions=%zu "
        "guest_profile=PAL60-HDMI-1280x720-60-English game_mount=read-only\n",
        cfg.xex.size(), cfg.functions.size());
    std::string error;
    auto title = rcomp::app::TitleRuntime::Create(cfg, &error);
    if (!title) {
        std::fprintf(stderr, "RCOMP-ENTRY-DIAGNOSTIC stage=create_failed detail=%s\n", error.c_str());
        return 3;
    }
    uint32_t code = 0;
    if (!title->RunEntry(0, &code, &error)) {
        std::fprintf(stderr, "RCOMP-ENTRY-DIAGNOSTIC stage=run_failed detail=%s\n", error.c_str());
        return 4;
    }
    std::fprintf(stdout,
        "RCOMP-ENTRY-DIAGNOSTIC stage=entry_returned code=0x%08X "
        "graphics=NOT_TESTED gameplay=NOT_TESTED\n", code);
    return 0;
}
