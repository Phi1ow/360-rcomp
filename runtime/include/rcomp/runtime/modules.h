// Bootstrap and query services for the main AOT image and the two HLE libraries.
#pragma once
#include <stdint.h>
#include <string>
#include "rcomp/runtime/status.h"
#include "rcomp/runtime/xex_loader.h"

namespace rcomp::rt {
struct ModuleConfig {
    std::string guest_path = "game:\\default.xex";
    // Exact ASCII command line supplied by the launcher; no invented quoting.
    // Empty remains an exact empty command line (no arguments inferred).
    std::string command_line;
};
// Bootstrap only, before load_xex_image relocates variable imports. Requires
// runtime_init; owns storage until shutdown. Failure publishes no new bindings.
Status runtime_prepare_main_module(const ModuleConfig& config);
// After load_xex_image and register_functions, before starting guest threads.
// Validates/retains the loaded header, PE metadata and real AOT import mappings.
// Failure leaves HMODULE=0. Retry is permitted unless failed cleanup had to
// retain a protected allocation, in which case destroy the Runtime first.
Status runtime_finalize_main_module(const XexImage& image);
Status register_xboxkrnl_module_hle();

// AOT secondary modules (title DLLs, include/rcomp/aot_modules.h;
// runtime/docs/MODULES.md "AOT secondary modules").
// Host directory holding the decoded (plain XEX2) image of every encrypted or
// compressed module, at <host_root>/<AotModule::disc_path>. Absolute host path.
Status runtime_configure_module_images(const std::string& host_root);
// Forgets the registered descriptors and the image root (bootstrap/tests only:
// no secondary module may be loaded in a live runtime).
void clear_aot_modules();
}  // namespace rcomp::rt

struct PPCContext;
// Call through an import thunk of a secondary module whose library is another
// AOT module (XenonRecomp names it __imp__rcomp_unresolved_<library>_<ordinal>
// and emits PPC_UNRESOLVED_IMPORT(library, ordinal) with non-alphanumerics of
// the library name as '_'). Calls the export's AOT function on the caller's
// context, as the console thunk's `mtctr; bctr` does; any other library or
// ordinal stops with RCOMP_FATAL_MISSING_IMPORT (same line as
// rcomp_unresolved_import).
void rcomp_module_import(PPCContext& ctx, uint8_t* base, const char* module, uint32_t ordinal);
