// Defines `__imp__<Name>` for every *function* export of xboxkrnl.exe and
// xam.xex, matching the declarations XenonRecomp emits
// (PPC_EXTERN_FUNC(__imp__<Name>) in ppc_recomp_shared.h). Each symbol only
// forwards to the import registry; no behaviour lives here. Unregistered
// imports end in rcomp::hle_missing_import().
//
// The .inc tables (XenonRecomp's vendored copy of Xenia's tables, used
// read-only) end every entry with a comma, meant for array initializers. To
// define functions from them, every expansion is wrapped in a chain of unused
// `int` declarators so that the separating commas stay valid C++:
//
//   [[maybe_unused]] static const int head = 0,
//       pre_A = 0; <thunk A> [[maybe_unused]] static const int post_A = 0,
//       pre_B = 0; <thunk B> [[maybe_unused]] static const int post_B = 0,
//   tail = 0;
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include <string.h>

// Dispatch row of the module a thunk belongs to (its MOD argument below).
#define RCOMP_IMPORT_MODULE_krnl Xboxkrnl
#define RCOMP_IMPORT_MODULE_xam Xam
#define RCOMP_THUNK_kFunction(MOD, MODULE_STR, ORDINAL, NAME)                  \
    rcomp_pre_##MOD##_##NAME = 0;                                              \
    PPC_FUNC(__imp__##NAME) {                                                  \
        static const rcomp::ImportId id{MODULE_STR, ORDINAL, #NAME};           \
        rcomp::rt::dispatch_import_fast(rcomp::rt::ImportModule::RCOMP_IMPORT_MODULE_##MOD, id, ctx, base);                             \
    }                                                                          \
    [[maybe_unused]] static const int rcomp_post_##MOD##_##NAME = 0
// Data imports: XenonRecomp does not bind them; nothing to define.
#define RCOMP_THUNK_kVariable(MOD, MODULE_STR, ORDINAL, NAME) rcomp_var_##MOD##_##NAME = 0

// Global namespace on purpose: the thunks need external C++ linkage.
#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) \
    RCOMP_THUNK_##TYPE(krnl, "xboxkrnl.exe", ORDINAL, NAME)
[[maybe_unused]] static const int rcomp_head_krnl = 0,
#include "xbox/xboxkrnl_table.inc"
    rcomp_tail_krnl = 0;
#undef XE_EXPORT

#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) RCOMP_THUNK_##TYPE(xam, "xam.xex", ORDINAL, NAME)
[[maybe_unused]] static const int rcomp_head_xam = 0,
#include "xbox/xam_table.inc"
    rcomp_tail_xam = 0;
#undef XE_EXPORT

namespace rcomp::rt {
PPCFunc* import_thunk(const char* module, uint32_t ordinal) {
    struct Row { uint32_t ordinal; PPCFunc* function; };
#define RCOMP_ADDRESS_kFunction(NAME) &__imp__##NAME
#define RCOMP_ADDRESS_kVariable(NAME) nullptr
#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) {ORDINAL, RCOMP_ADDRESS_##TYPE(NAME)}
    static const Row kernel[] = {
#include "xbox/xboxkrnl_table.inc"
    };
    static const Row xam[] = {
#include "xbox/xam_table.inc"
    };
#undef XE_EXPORT
#undef RCOMP_ADDRESS_kVariable
#undef RCOMP_ADDRESS_kFunction
    if (module && !strcasecmp(module,kModuleXboxkrnl))
        for (const auto& r:kernel) if(r.ordinal==ordinal) return r.function;
    if (module && !strcasecmp(module,kModuleXam))
        for (const auto& r:xam) if(r.ordinal==ordinal) return r.function;
    return nullptr;
}
} // namespace rcomp::rt
