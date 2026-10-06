// The main XEX's execution-info header (key 0x00040006, 24 bytes: media id, version, base version, title id,
// platform, executable type, disc number, disc count, savegame id), shared by XamGetExecutionId and the
// storage services that key their data by title id. Kept apart from hle_xam.cpp so the storage code does not
// pull the input backend into every program that links the runtime.
#include "module_state.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam.h"

namespace rcomp::rt {
namespace {
constexpr uint32_t kExecutionInfoKey = 0x00040006;
constexpr uint32_t kExecutionInfoBytes = 24;
}  // namespace

uint32_t xam_execution_info_pointer(const char* fn) {
    Runtime* rp = runtime();
    if (!rp) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s before runtime_init", fn);
    Runtime& r = *rp;
    const ModuleState* m = r.modules.get();
    if (!m || !m->ready || m->generation != r.generation)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s requires a finalized main XEX module", fn);
    for (const auto& f : m->fields) {
        if (f.key != kExecutionInfoKey) continue;
        if (f.size != kExecutionInfoBytes || uint64_t(f.offset) + f.size > m->header_size)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s has invalid XEX execution-info metadata", fn);
        const uint32_t address = m->header_storage + f.offset;
        if (!r.mem->is_accessible(address, f.size, Protect::Read) ||
            r.mem->is_accessible(address, f.size, Protect::ReadWrite))
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s execution-info storage is not read-only", fn);
        return address;
    }
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s main XEX has no 0x%08X execution-info header", fn,
                kExecutionInfoKey);
}

uint32_t xam_main_title_id(const char* fn) {
    uint32_t title_id = 0;
    if (!guest_read_be32(xam_execution_info_pointer(fn) + 12, &title_id))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s execution-info title id unreadable", fn);
    return title_id;
}
}  // namespace rcomp::rt
