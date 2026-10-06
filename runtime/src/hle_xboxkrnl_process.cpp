// Xbox title process identity and cooperative title lifecycle services.
// ABI/source pins and integration constraints: runtime/docs/PROCESS_LIFECYCLE.md.
#include "rcomp/runtime/process_lifecycle.h"

#include <algorithm>
#include <climits>
#include <mutex>
#include <vector>

#include "rcomp/diag.h"
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kTitleTerminateRegistrationBytes = 16;
constexpr uint32_t kHalRebootRoutine = 1;

struct TerminateCallback {
    uint32_t registration = 0;
    uint32_t routine = 0;
    uint32_t priority = 0;
    uint64_t sequence = 0;
};

struct CriticalRegionState {
    uint32_t thread_id = 0;
    int32_t apc_disable_count = 0;
};

std::mutex g_lifecycle_mutex;
uint64_t g_generation = 0;
TitleLifecyclePhase g_phase = TitleLifecyclePhase::Running;
TitleTerminationReason g_reason = TitleTerminationReason::XamLoader;
uint32_t g_terminal_code = 0;
uint64_t g_next_sequence = 0;
std::vector<TerminateCallback> g_callbacks;
std::vector<TerminateCallback> g_dispatch;
std::vector<CriticalRegionState> g_critical_regions;
TitleQuiesceRequestFn g_quiesce_request = nullptr;

void reset_for_generation_locked(uint64_t generation) {
    if (g_generation == generation) return;
    g_generation = generation;
    g_phase = TitleLifecyclePhase::Running;
    g_reason = TitleTerminationReason::XamLoader;
    g_terminal_code = 0;
    g_next_sequence = 0;
    g_callbacks.clear();
    g_dispatch.clear();
    g_critical_regions.clear();
}

Runtime* active_runtime() {
    Runtime* r = runtime();
    return r && r->mem ? r : nullptr;
}

GuestThread* active_title_thread() {
    GuestThread* thread = current_guest_thread();
    return thread && thread->identity ? thread : nullptr;
}

void require_title_context(const char* function) {
    if (!active_runtime() || !active_title_thread())
        rcomp_fatal(RCOMP_FATAL_INTERNAL,
                    "xboxkrnl.exe!%s requires an active runtime title GuestThread", function);
}

bool registration_accessible(Runtime& r, uint32_t address) {
    return address && !(address & 3u) &&
           r.mem->is_accessible(address, kTitleTerminateRegistrationBytes, Protect::ReadWrite);
}

CriticalRegionState* critical_state_locked(uint32_t thread_id, bool create) {
    for (auto& state : g_critical_regions)
        if (state.thread_id == thread_id) return &state;
    if (!create) return nullptr;
    g_critical_regions.push_back({thread_id, 0});
    return &g_critical_regions.back();
}

[[noreturn]] void invalid_registration(const char* detail, uint32_t registration) {
    rcomp_fatal(RCOMP_FATAL_GUEST_TRAP,
                "xboxkrnl.exe!ExRegisterTitleTerminateNotification %s registration=0x%08X",
                detail, registration);
}

void KeGetCurrentProcessType(PPCContext& ctx, uint8_t*) {
    require_title_context("KeGetCurrentProcessType");
    // R-comp currently executes title AOT code in the title/user process. It
    // does not fabricate idle/system process contexts for host helper threads.
    ctx.r3.u64 = kProcessTypeUser;
}

// KeSetCurrentProcessType (0x009A): VOID (ULONG Type), X_PROCTYPE_IDLE 0 /
// USER (title) 1 / SYSTEM 2 (Xenia; it stores the type for later threads and
// for the current DPC). The title process R-comp runs is the user process,
// the value KeGetCurrentProcessType reports: Type 1 is the type already in
// effect and changes nothing. Moving title code to the idle or system process
// has no R-comp model (no system process, pools or threads exist) and traps;
// a value above 2 is not a process type (guest error).
void KeSetCurrentProcessType(PPCContext& ctx, uint8_t*) {
    require_title_context("KeSetCurrentProcessType");
    const uint32_t type = ctx.r3.u32;
    if (type > 2)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!KeSetCurrentProcessType type=%u lr=0x%08X", type,
                    (uint32_t)ctx.lr);
    if (type != kProcessTypeUser)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xboxkrnl.exe!KeSetCurrentProcessType type=%u lr=0x%08X: R-comp runs title code only in the user "
                    "(title) process",
                    type, (uint32_t)ctx.lr);
}

void ExRegisterTitleTerminateNotification(PPCContext& ctx, uint8_t*) {
    require_title_context("ExRegisterTitleTerminateNotification");
    Runtime& r = *runtime();
    GuestThread& thread = *current_guest_thread();
    const uint32_t registration = ctx.r3.u32;
    const bool create = ctx.r4.u32 != 0;

    if (!registration_accessible(r, registration))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,
                    "xboxkrnl.exe!ExRegisterTitleTerminateNotification registration=0x%08X "
                    "requires 16 readable/writable bytes aligned to 4",
                    registration);

    uint32_t routine = 0;
    uint32_t priority = 0;
    if (!guest_read_be32(registration, &routine) || !guest_read_be32(registration + 4, &priority))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,
                    "xboxkrnl.exe!ExRegisterTitleTerminateNotification registration read failed "
                    "at 0x%08X",
                    registration);

    if (create) {
        if (!routine || (routine & 3u) || !lookup_function(routine))
            rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET,
                        "xboxkrnl.exe!ExRegisterTitleTerminateNotification callback=0x%08X "
                        "registration=0x%08X has no recompiled function",
                        routine, registration);

        bool duplicate = false;
        bool terminating = false;
        {
            std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
            reset_for_generation_locked(r.generation);
            terminating = g_phase != TitleLifecyclePhase::Running;
            if (!terminating) {
                for (const auto& callback : g_callbacks)
                    if (callback.registration == registration) {
                        duplicate = true;
                        break;
                    }
                if (!duplicate)
                    g_callbacks.push_back(
                        {registration, routine, priority, g_next_sequence++});
            }
        }
        if (terminating)
            invalid_registration("create after title termination was requested", registration);
        if (duplicate) invalid_registration("duplicate create", registration);
    } else {
        bool removed = false;
        bool terminating = false;
        {
            std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
            reset_for_generation_locked(r.generation);
            terminating = g_phase != TitleLifecyclePhase::Running;
            if (!terminating) {
                for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ++it) {
                    if (it->registration == registration) {
                        g_callbacks.erase(it);
                        removed = true;
                        break;
                    }
                }
            }
        }
        if (terminating)
            invalid_registration("remove after title termination was requested", registration);
        if (!removed) invalid_registration("remove of an unregistered entry", registration);
    }

    // The embedded LIST_ENTRY is kernel-private bookkeeping. R-comp keeps the
    // list host-side and never invents guest kernel addresses for Flink/Blink.
    (void)thread;
}

void KeEnterCriticalRegion(PPCContext&, uint8_t*) {
    require_title_context("KeEnterCriticalRegion");
    Runtime& r = *runtime();
    const uint32_t thread_id = current_guest_thread()->thread_id;
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        reset_for_generation_locked(r.generation);
        CriticalRegionState* state = critical_state_locked(thread_id, true);
        if (state->apc_disable_count == INT32_MIN)
            overflow = true;
        else
            --state->apc_disable_count;
    }
    if (overflow)
        rcomp_fatal(RCOMP_FATAL_GUEST_TRAP,
                    "xboxkrnl.exe!KeEnterCriticalRegion APC disable count overflow thread_id=%u",
                    thread_id);
}

void KeLeaveCriticalRegion(PPCContext&, uint8_t*) {
    require_title_context("KeLeaveCriticalRegion");
    Runtime& r = *runtime();
    const uint32_t thread_id = current_guest_thread()->thread_id;
    bool underflow = false;
    {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        reset_for_generation_locked(r.generation);
        CriticalRegionState* state = critical_state_locked(thread_id, true);
        if (state->apc_disable_count >= 0)
            underflow = true;
        else
            ++state->apc_disable_count;
    }
    if (underflow)
        rcomp_fatal(RCOMP_FATAL_GUEST_TRAP,
                    "xboxkrnl.exe!KeLeaveCriticalRegion without matching enter thread_id=%u",
                    thread_id);
    // Reaching zero would permit queued kernel APC delivery on the Xbox. R-comp
    // has no APC queue yet, so there is no deferred APC work to drain here.
}

void HalReturnToFirmware(PPCContext& ctx, uint8_t* base) {
    require_title_context("HalReturnToFirmware");
    const uint32_t mode = ctx.r3.u32;
    if (mode != kHalRebootRoutine)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xboxkrnl.exe!HalReturnToFirmware mode=%u is a physical/power firmware action "
                    "outside R-comp's virtual title lifecycle",
                    mode);

    uint32_t terminal_code = 0;
    const Status status = request_title_termination(
        ctx, base, TitleTerminationReason::HalRebootToDashboard, 0, &terminal_code);
    if (status == Status::AlreadyExists) return;
    if (status != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL,
                    "xboxkrnl.exe!HalReturnToFirmware virtual dashboard transition failed: %s",
                    status_name(status));

    // request_title_termination has returned: no lifecycle mutex, vector
    // iterator, or other C++ RAII object is live across this longjmp.
    exit_current_guest_thread(terminal_code);
}

struct Impl {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};

constexpr Impl kImpls[] = {
    {0x0015, "ExRegisterTitleTerminateNotification", &ExRegisterTitleTerminateNotification},
    {0x0028, "HalReturnToFirmware", &HalReturnToFirmware},
    {0x005F, "KeEnterCriticalRegion", &KeEnterCriticalRegion},
    {0x0066, "KeGetCurrentProcessType", &KeGetCurrentProcessType},
    {0x009A, "KeSetCurrentProcessType", &KeSetCurrentProcessType},
    {0x007D, "KeLeaveCriticalRegion", &KeLeaveCriticalRegion},
};

}  // namespace

Status configure_title_lifecycle_quiesce_request(TitleQuiesceRequestFn request) {
    if (!request) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    if (g_quiesce_request && g_quiesce_request != request) return Status::AlreadyExists;
    g_quiesce_request = request;
    return Status::Ok;
}

Status request_title_termination(PPCContext& ctx, uint8_t* base,
                                 TitleTerminationReason reason, uint32_t terminal_code,
                                 uint32_t* effective_terminal_code) {
    Runtime* r = active_runtime();
    GuestThread* thread = active_title_thread();
    if (!r || !thread || !effective_terminal_code) return Status::NotInitialized;
    if (base != r->mem->base()) return Status::InvalidArgument;

    TitleQuiesceRequestFn quiesce = nullptr;
    bool nested = false;
    uint32_t selected_terminal_code = 0;
    {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        reset_for_generation_locked(r->generation);
        if (g_phase != TitleLifecyclePhase::Running) {
            nested = true;
            selected_terminal_code = g_terminal_code;
        } else {
            quiesce = g_quiesce_request;
            if (!quiesce) return Status::NotInitialized;
            g_phase = TitleLifecyclePhase::TerminationRequested;
            g_reason = reason;
            g_terminal_code = terminal_code;
            selected_terminal_code = terminal_code;
            g_dispatch.clear();
            g_dispatch.swap(g_callbacks);
            std::sort(g_dispatch.begin(), g_dispatch.end(),
                      [](const TerminateCallback& a, const TerminateCallback& b) {
                          if (a.priority != b.priority) return a.priority > b.priority;
                          return a.sequence < b.sequence;
                      });
        }
    }
    *effective_terminal_code = selected_terminal_code;
    if (nested) return Status::AlreadyExists;

    // Request worker/wait cancellation before guest callbacks. This hook must
    // be non-blocking when called by a GuestThread; the owner performs finish
    // after the guest entry has unwound.
    quiesce();

    {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        if (g_generation != r->generation || g_phase != TitleLifecyclePhase::TerminationRequested)
            return Status::Conflict;
        g_phase = TitleLifecyclePhase::CallbacksRunning;
    }

    // No C++ RAII object spans a guest callback. Mark each entry consumed before
    // entering guest code, so even a non-local guest-thread exit cannot cause a
    // callback to be dispatched twice.
    size_t index = 0;
    for (;;) {
        uint32_t routine = 0;
        {
            std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
            if (index >= g_dispatch.size()) break;
            routine = g_dispatch[index].routine;
            g_dispatch[index].routine = 0;
            ++index;
        }
        if (!routine) continue;
        PPCFunc* callback = lookup_function(routine);
        if (!callback)
            rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET,
                        "title terminate callback 0x%08X disappeared from the AOT function table",
                        routine);
        callback(ctx, base);
    }

    {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        if (g_generation != r->generation) return Status::Conflict;
        g_dispatch.clear();
        g_phase = TitleLifecyclePhase::CallbacksComplete;
    }
    return Status::Ok;
}

Status title_lifecycle_snapshot(TitleLifecycleSnapshot* out) {
    if (!out) return Status::InvalidArgument;
    Runtime* r = active_runtime();
    if (!r) return Status::NotInitialized;
    TitleLifecycleSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        reset_for_generation_locked(r->generation);
        snapshot.generation = g_generation;
        snapshot.phase = g_phase;
        snapshot.reason = g_reason;
        snapshot.terminal_code = g_terminal_code;
        snapshot.registered_callbacks = g_callbacks.size();
    }
    *out = snapshot;
    return Status::Ok;
}

void reset_title_lifecycle() {
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    g_generation = 0;
    g_phase = TitleLifecyclePhase::Running;
    g_reason = TitleTerminationReason::XamLoader;
    g_terminal_code = 0;
    g_next_sequence = 0;
    g_callbacks.clear();
    g_dispatch.clear();
    g_critical_regions.clear();
    g_quiesce_request = nullptr;
}

void title_lifecycle_thread_exited(uint32_t thread_id) {
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    for (auto it = g_critical_regions.begin(); it != g_critical_regions.end(); ++it) {
        if (it->thread_id == thread_id) {
            g_critical_regions.erase(it);
            break;
        }
    }
}

Status current_guest_apc_disable_count(int32_t* out) {
    if (!out) return Status::InvalidArgument;
    Runtime* r = active_runtime();
    GuestThread* thread = active_title_thread();
    if (!r || !thread) return Status::NotInitialized;
    int32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        reset_for_generation_locked(r->generation);
        CriticalRegionState* state = critical_state_locked(thread->thread_id, false);
        if (state) count = state->apc_disable_count;
    }
    *out = count;
    return Status::Ok;
}

Status register_xboxkrnl_process_hle() {
    if (!runtime()) return Status::NotInitialized;
    for (const Impl& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            rcomp_fatal(RCOMP_FATAL_INTERNAL,
                        "xboxkrnl process HLE %s: ordinal 0x%04X not in export table",
                        impl.name, impl.ordinal);
        const Status status = register_import(kModuleXboxkrnl, impl.ordinal, impl.function, impl.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
