// Shared interface (owner: PRIME). Ahead-of-time compiled secondary XEX modules (title DLLs).
//
// A title can load other XEX images at run time with XexLoadImage (Halo 3: WaveShell-Xbox.dll,
// WavesLibDLL.dll, waves/L360.dll, waves/Q10.dll). R-comp has no JIT: the code of every such module found on
// the disc is recompiled by XenonRecomp at build time, with its generated symbols prefixed so that several
// modules link into one title, and linked into the title next to the main module.
//
// Contract between the build (cpu/, tools/, app/m6) and the runtime (runtime/):
//  * For each secondary module the build emits one `AotModule` descriptor and the title registers all of them
//    with register_aot_modules() before the guest starts (app/m6 generates the list).
//  * The descriptor names the module as it appears on the disc (`disc_path`, relative to the game root, '/'
//    separators, case as on the disc, e.g. "waves/L360.dll") and its import-library name (`module_name`,
//    e.g. "L360.dll", the name other modules import it by). `image_base` / `image_size` are those of the XEX.
//  * `functions` / `function_count` are the module's (guest address -> host function) mapping, the same data
//    as the main module's PPCFuncMappings. Addresses of different modules never overlap (fixed XEX bases).
//  * The runtime's XexLoadImage(path) finds the descriptor by disc path, maps the module's decoded image at
//    image_base from the file on the disc (the image bytes are data, not code), registers `functions` in the
//    global function table (add, never replace), resolves the module's imports through the import registry
//    (xboxkrnl.exe / xam.xex HLE, or exports of other loaded AOT modules), registers the module's own exports
//    under `module_name`, and calls its entry point with DLL_PROCESS_ATTACH. Unknown modules keep the explicit
//    fatal of XexLoadImage.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rcomp/func_table.h"

namespace rcomp {

struct AotModule {
    const char* disc_path;    // relative to the game root, '/' separators
    const char* module_name;  // import-library name of the module
    uint32_t image_base;
    uint32_t image_size;
    uint32_t entry_point;     // guest address (0: none)
    const FuncEntry* functions;
    size_t function_count;
};

// Registers the secondary modules compiled into this title (copied pointers: descriptors are static data).
// Returns false on a duplicate disc path or overlapping image ranges.
bool register_aot_modules(const AotModule* modules, size_t count);
// The module compiled for `disc_path` (case-insensitive, '/' or '\\' separators), or nullptr.
const AotModule* find_aot_module(const char* disc_path);
// All registered modules.
const AotModule* aot_modules(size_t* count);

// Adds entries to the global function table without removing existing ones (func_table.cpp).
// Returns false on a duplicate guest address or invalid input, leaving the table unchanged.
bool add_functions(const FuncEntry* entries, size_t count);

}  // namespace rcomp
