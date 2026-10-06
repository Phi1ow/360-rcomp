// Import registry: (module, ordinal) -> PPCFunc implementation
// (owner: Agent 3, runtime/).
//
// How XenonRecomp @ddd128b represents imports (XenonUtils/xex.cpp,
// XenonRecomp/recompiler.cpp):
//   * for each XEX import thunk of a function import whose ordinal is in
//     XenonUtils/xbox/{xboxkrnl,xam}_table.inc, it overwrites the 16-byte thunk
//     with nop,nop,nop,blr and adds a symbol "__imp__<Name>" at the thunk
//     address. The thunk is not recompiled (it is skipped as an existing
//     symbol); generated code only *declares* it
//       PPC_EXTERN_FUNC(__imp__<Name>);   // C++ linkage, global namespace
//     and calls it directly: `__imp__<Name>(ctx, base);`. It also appears in
//     PPCFuncMappings[] so indirect calls through the thunk address reach it.
//   * the ordinal is NOT present in the generated code; only the name.
//
// The runtime therefore defines one strong `__imp__<Name>` per *function*
// export of both tables (src/import_thunks.cpp, generated at compile time from
// the same .inc files). Each forwards to dispatch_import({module, ordinal,
// name}), which calls the registered implementation or ends in
// rcomp::hle_missing_import() -> rcomp_fatal(RCOMP_FATAL_MISSING_IMPORT).
//
// An import whose ordinal is missing from the tables, or from any other module,
// gets `__imp__rcomp_unresolved_<module>_<ordinal>` (cpu/patches/xenonrecomp/
// 0005) which ends in RCOMP_FATAL_MISSING_IMPORT when called.
//
// Variable imports (kernel data such as XboxHardwareInfo) are not seen by the
// generator: the XEX import record is a slot in the image that the loader
// fills with the guest address of the variable (runtime/xex_loader.h). Their
// storage and contents are registered here by whoever implements them.
#pragma once

#include <stdint.h>

#include "rcomp/func_table.h"
#include "rcomp/hle.h"
#include "rcomp/runtime/status.h"

namespace rcomp::rt {

constexpr const char* kModuleXboxkrnl = "xboxkrnl.exe";
constexpr const char* kModuleXam = "xam.xex";

// The two modules with a lock-free dispatch row of their own (dispatch_import_fast).
enum class ImportModule : uint8_t { Xboxkrnl = 0, Xam = 1 };

// `registry_name` identifies the implementation (test doubles must start with
// "TESTDOUBLE_"). Registering the same (module, ordinal) twice with a
// different function returns AlreadyExists.
Status register_import(const char* module, uint32_t ordinal, PPCFunc* fn,
                       const char* registry_name);
Status unregister_import(const char* module, uint32_t ordinal);
PPCFunc* find_import(const char* module, uint32_t ordinal);
// The actual linked __imp__ wrapper, distinct from its HLE implementation.
// Used to prove that a guest thunk mapping is the correct AOT import address.
PPCFunc* import_thunk(const char* module, uint32_t ordinal);
const char* import_registry_name(const char* module, uint32_t ordinal);
void clear_imports();

// Export table lookups (from XenonRecomp's copy of Xenia's tables).
enum class ExportKind { Unknown, Function, Variable };
const char* export_name(const char* module, uint32_t ordinal);  // nullptr if unknown
ExportKind export_kind(const char* module, uint32_t ordinal);
bool export_ordinal(const char* module, const char* name, uint32_t* ordinal);

// Variable exports: `guest_address` is the committed guest storage of the
// variable (big-endian contents, as the title expects). Only ordinals the
// tables list as variables are accepted (InvalidArgument otherwise).
Status register_variable_import(const char* module, uint32_t ordinal, uint32_t guest_address,
                                const char* registry_name);
bool find_variable_import(const char* module, uint32_t ordinal, uint32_t* guest_address);
Status unregister_variable_import(const char* module, uint32_t ordinal);

// General entry: finds the module by name.
void dispatch_import(const ImportId& id, PPCContext& ctx, uint8_t* base);
// Called by every __imp__ thunk, which knows its module when it is compiled: no name comparison on
// the path of the millions of imports a second the guest makes (two strcasecmp per import before).
void dispatch_import_fast(ImportModule module, const ImportId& id, PPCContext& ctx, uint8_t* base);

}  // namespace rcomp::rt
