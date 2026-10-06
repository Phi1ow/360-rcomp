// XAM title-loader lifecycle entry points. The shared state and Xbox kernel
// notification implementation live in hle_xboxkrnl_process.cpp.
#include "rcomp/runtime/process_lifecycle.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"

namespace rcomp::rt {
namespace {

// Ends the running title through the lifecycle of hle_xboxkrnl_process.cpp
// (termination callbacks, then the exit of the calling GuestThread). Does not
// return, except to an outer termination request that is already unwinding.
void end_title(PPCContext& ctx, uint8_t* base, TitleTerminationReason reason, const char* fn) {
    Runtime* r = runtime();
    GuestThread* thread = current_guest_thread();
    if (!r || !r->mem || !thread || !thread->identity)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s requires an active runtime title GuestThread", fn);

    uint32_t terminal_code = 0;
    const Status status = request_title_termination(ctx, base, reason, 0, &terminal_code);
    if (status == Status::AlreadyExists) return;
    if (status != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s lifecycle request failed: %s", fn, status_name(status));

    // request_title_termination has returned and destroyed all automatic C++
    // state. The longjmp below therefore obeys GuestThread's unwind contract.
    exit_current_guest_thread(terminal_code);
}

void XamLoaderTerminateTitle(PPCContext& ctx, uint8_t* base) {
    end_title(ctx, base, TitleTerminationReason::XamLoader, "XamLoaderTerminateTitle");
}

// XamLoaderLaunchTitle (0x01A4): (LPCSTR Name, DWORD Flags). Ends the running
// title and starts the executable Name (a path, or a file beside the running
// XEX) or, when Name is NULL, the dashboard; it does not return (Xenia
// 95a5c3e xam_info.cc, XDK XLaunchNewImage). Implemented: Name NULL, the return
// to the dashboard, which is the end of the title in R-comp: the same
// lifecycle as XamLoaderTerminateTitle, recorded with its own reason. Starting
// another executable needs a second recompiled image in the title and traps.
void XamLoaderLaunchTitle(PPCContext& ctx, uint8_t* base) {
    const uint32_t name = ctx.r3.u32;
    if (name)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xam.xex!XamLoaderLaunchTitle name=0x%08X flags=0x%08X lr=0x%08X: launching another "
                    "executable is not implemented (only NULL, the dashboard)", name, ctx.r4.u32, (uint32_t)ctx.lr);
    end_title(ctx, base, TitleTerminationReason::XamLoaderLaunchDashboard, "XamLoaderLaunchTitle");
}

// XamShowDirtyDiscErrorUI (0x02D9): (DWORD UserIndex). Shows the system's
// disc-read error screen, which never returns: the title is over (Xenia
// 95a5c3e xam_ui.cc: "This is death, and should never return"). Implemented as
// that end: a terminal diagnostic naming the title's own report, distinct from
// a missing import, since a read error of the disc tree is what the title saw.
void XamShowDirtyDiscErrorUI(PPCContext& ctx, uint8_t*) {
    // Titles reach this screen from a shared halt handler: the guest call chain (PPC back-chain: [sp] = caller's
    // sp, saved LR at [caller_sp - 8]) names the site that decided the disc was bad.
    char callers[400] = {};
    int n = 0;
    uint32_t sp = ctx.r1.u32;
    for (int frame = 0; frame < 16 && sp && n < int(sizeof callers) - 12; ++frame) {
        uint32_t back = 0, saved = 0;
        if (!guest_read_be32(sp, &back) || !back || !guest_read_be32(back - 8, &saved)) break;
        n += std::snprintf(callers + n, sizeof callers - n, " 0x%08X", saved);
        sp = back;
    }
    rcomp_fatal(RCOMP_FATAL_GUEST_TRAP,
                "xam.xex!XamShowDirtyDiscErrorUI user=%u lr=0x%08X: the title reported a disc read error "
                "(the dirty-disc screen ends the title) callers:%s", ctx.r3.u32, (uint32_t)ctx.lr, callers);
}

// Launch data is the block a title hands to the next one with XamLoaderSetLaunchData before
// XamLoaderLaunchTitle. XAM keeps one launch-data buffer: XamLoaderSetLaunchData replaces it (size 0
// clears it) and XamLoaderGetLaunchDataSize / XamLoaderGetLaunchData read it, so a title that set data
// reads it back, as on the console and in the references (Xenia Canary / rexglue-sdk c94f5eb
// xam_info.cpp, read only). R-comp starts the title directly (nobody launched it), so the buffer starts
// empty: both queries give the console's "no launch data" answer, ERROR_NOT_FOUND. Gears of War 2
// (sub_8280F0B0) presets the size to 0, calls XamLoaderGetLaunchDataSize(PDWORD) and reads the data
// with XamLoaderGetLaunchData(buffer, size) only when the first call returned 0. The buffer belongs to
// one Runtime generation. No console size limit is established: a block above kMaxLaunchData is an
// explicit fatal instead of a guess.
constexpr uint32_t kErrorInvalidParameter = 0x57, kErrorNotFound = 0x490;
constexpr uint32_t kMaxLaunchData = 0x10000;

struct LaunchData {
    std::mutex mutex;
    uint64_t generation = 0;
    bool present = false;
    std::vector<uint8_t> bytes;
};
LaunchData g_launch;

// Caller holds g_launch.mutex.
void sync_launch_locked(const Runtime& r) {
    if (g_launch.generation == r.generation) return;
    g_launch.generation = r.generation;
    g_launch.present = false;
    g_launch.bytes.clear();
}

Runtime& loader_runtime(const char* fn) {
    Runtime* r = runtime();
    if (!r || !r->mem) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s before runtime_init", fn);
    return *r;
}

// XamLoaderSetLaunchData (0x01A6): (PVOID Data, DWORD Size) -> ERROR_SUCCESS. ERROR_INVALID_PARAMETER
// when Size bytes at Data are not readable.
void XamLoaderSetLaunchData(PPCContext& ctx, uint8_t*) {
    const char* fn = "XamLoaderSetLaunchData";
    Runtime& r = loader_runtime(fn);
    const uint32_t data = ctx.r3.u32, size = ctx.r4.u32;
    if (size > kMaxLaunchData)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s size %u above the bound %u (no console limit established) lr=0x%08X",
                    fn, size, kMaxLaunchData, uint32_t(ctx.lr));
    if (size && (!data || !r.mem->is_accessible(data, size, Protect::Read))) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    std::lock_guard<std::mutex> lock(g_launch.mutex);
    sync_launch_locked(r);
    const uint8_t* source = size ? r.mem->host(data) : nullptr;
    g_launch.bytes.assign(source, source + size);
    g_launch.present = size != 0;
    ctx.r3.u64 = 0;
}

// XamLoaderGetLaunchDataSize (0x01A7): (PDWORD Size). *Size = the stored size and ERROR_SUCCESS, or
// *Size = 0 and ERROR_NOT_FOUND; ERROR_INVALID_PARAMETER for an unwritable Size.
void XamLoaderGetLaunchDataSize(PPCContext& ctx, uint8_t*) {
    Runtime& r = loader_runtime("XamLoaderGetLaunchDataSize");
    const uint32_t size = ctx.r3.u32;
    if (!size || !r.mem->is_accessible(size, 4, Protect::ReadWrite)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    std::lock_guard<std::mutex> lock(g_launch.mutex);
    sync_launch_locked(r);
    guest_write_be32(size, g_launch.present ? uint32_t(g_launch.bytes.size()) : 0);
    ctx.r3.u64 = g_launch.present ? 0 : kErrorNotFound;
}

// XamLoaderGetLaunchData (0x01A8): (PVOID Buffer, DWORD Size). Copies min(Size, stored size) bytes and
// returns ERROR_SUCCESS; ERROR_NOT_FOUND (buffer untouched) when there is none; ERROR_INVALID_PARAMETER
// for a buffer that cannot take the copy.
void XamLoaderGetLaunchData(PPCContext& ctx, uint8_t*) {
    Runtime& r = loader_runtime("XamLoaderGetLaunchData");
    const uint32_t buffer = ctx.r3.u32, size = ctx.r4.u32;
    std::lock_guard<std::mutex> lock(g_launch.mutex);
    sync_launch_locked(r);
    if (!g_launch.present) {
        ctx.r3.u64 = kErrorNotFound;
        return;
    }
    const uint32_t copy = std::min<uint32_t>(size, uint32_t(g_launch.bytes.size()));
    if (copy && (!buffer || !r.mem->is_accessible(buffer, copy, Protect::ReadWrite))) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    if (copy) std::memcpy(r.mem->host(buffer), g_launch.bytes.data(), copy);
    ctx.r3.u64 = 0;
}

struct Impl {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
const Impl kImpls[] = {
    {0x01A4, "XamLoaderLaunchTitle", &XamLoaderLaunchTitle},
    {0x01A6, "XamLoaderSetLaunchData", &XamLoaderSetLaunchData},
    {0x01A7, "XamLoaderGetLaunchDataSize", &XamLoaderGetLaunchDataSize},
    {0x01A8, "XamLoaderGetLaunchData", &XamLoaderGetLaunchData},
    {0x01A9, "XamLoaderTerminateTitle", &XamLoaderTerminateTitle},
    {0x02D9, "XamShowDirtyDiscErrorUI", &XamShowDirtyDiscErrorUI},
};

}  // namespace

Status register_xam_loader_hle() {
    if (!runtime()) return Status::NotInitialized;
    for (const auto& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXam, impl.name, &ordinal) || ordinal != impl.ordinal)
            rcomp_fatal(RCOMP_FATAL_INTERNAL,
                        "xam loader HLE %s: ordinal 0x%04X not in export table",
                        impl.name, impl.ordinal);
        const Status status = register_import(kModuleXam, impl.ordinal, impl.function, impl.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
