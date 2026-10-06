// XAM services whose values must come from the active title/runtime state.
#pragma once

#include <stdint.h>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {
Status register_xam_system_hle();

// What XamResetInactivity / XamEnableInactivityProcessing recorded since the last registration: this platform has no XAM idle policy to drive (the PS5 owns it),
// so these two calls only count; the two raw arguments of the last enable request are kept as the title passed them.
struct XamInactivityStats {
    uint64_t resets = 0;
    uint64_t enable_requests = 0;
    uint32_t last_enable_args[2] = {0, 0};
};
XamInactivityStats xam_inactivity_stats();

// Raw fields written by XGetVideoMode. No display mode is inferred here:
// TitleRuntime must pass the mode it actually configured for the title.
struct XamVideoMode {
    uint32_t display_width = 0;
    uint32_t display_height = 0;
    bool interlaced = false;
    bool widescreen = false;
    bool high_definition = false;
    float refresh_rate_hz = 0.0f;
};

// Console/profile values exposed by the four synchronous XGet* queries.
// They intentionally have no defaults: an owner must configure them before
// guest execution instead of silently inventing a locale/region/AV pack.
struct XamRuntimeConfig {
    uint32_t language = 0;
    uint32_t game_region = 0;
    uint32_t av_pack = 0;
    XamVideoMode video;
};

// Configure once for the active Runtime lifetime. Returns NotInitialized when
// there is no runtime, InvalidArgument for an unusable display mode, Conflict
// when an owner attempts to replace an already configured profile.
Status runtime_configure_xam(const XamRuntimeConfig& config);

// Guest address of the main XEX execution-info header (read-only); fatal when no main module is loaded.
uint32_t xam_execution_info_pointer(const char* fn);
// Title id of the finalized main XEX (execution-info header); fatal when no main module is loaded.
// `fn` names the caller in the fatal message.
uint32_t xam_main_title_id(const char* fn);

}  // namespace rcomp::rt
