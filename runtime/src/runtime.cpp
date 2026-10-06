#include "physical_window.h"
#include "rcomp/guest_write_tracking.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xma.h"
#include "rcomp/runtime/xaudio.h"

#include <string.h>

#include <memory>

#include "rcomp/guest_memory.h"
#include "rcomp/diag.h"
#include "rcomp/runtime/kernel_variables.h"
#include "rcomp/runtime/process_lifecycle.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/virtual_fields.h"
#include "rcomp/runtime/xam_net.h"
#include "rcomp/runtime/notifications.h"
#include "rcomp/runtime/xam_enum.h"
#include "rcomp/runtime/xam_content.h"
#include "rcomp/runtime/xconfig.h"
#include "module_state.h"
#include "kernel_internal.h"
#include "host_fiber.h"
#include "xam_state.h"

namespace rcomp::rt {
Runtime::Runtime(uint32_t max_handles) : handles(max_handles) {}
Runtime::~Runtime() = default;

namespace {
std::unique_ptr<Runtime> g_rt;
std::atomic<uint64_t> g_next_generation{1};
}

Status runtime_init(GuestMemory* mem, const RuntimeConfig& cfg) {
    if (g_rt) return Status::AlreadyExists;
    if (!mem || !mem->reserved()) return Status::NotInitialized;
    Status s = reserve_thread_object_arena(*mem);
    if (s != Status::Ok) return s;
    // Reserving the arena also installs a process-local field provider. An
    // unsuccessful bootstrap must release that ownership, otherwise a later
    // title on another GuestMemory would conflict with a dead arena owner.
    struct BootstrapRollback {
        bool armed = true;
        ~BootstrapRollback() {
            if (!armed) return;
            g_rt.reset();
            thread_objects_prepare_shutdown();
            thread_objects_reset();
            reset_title_lifecycle();
        }
    } rollback;
#if defined(__cpp_exceptions)
    try {
#endif
    auto rt = std::make_unique<Runtime>(cfg.max_handles);
    rt->mem = mem;
    mem->set_physical_4k_offset(cfg.physical_4k_window_offset);
    s = rt->heap.init(mem, cfg.heap_lo, cfg.heap_hi);
    if (s != Status::Ok) return s;
    if ((uint64_t)cfg.phys_lo < cfg.heap_hi && (uint64_t)cfg.heap_lo < cfg.phys_hi) return Status::InvalidArgument;
    s = rt->physical.init(mem, cfg.phys_lo, cfg.phys_hi);
    if (s != Status::Ok) return s;
    rt->generation = g_next_generation.fetch_add(1);
    g_rt = std::move(rt);
    s = configure_title_lifecycle_quiesce_request(&runtime_request_title_quiesce);
    if (s != Status::Ok) {
        return s;
    }
    rollback.armed = false;
    return Status::Ok;
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return Status::OutOfMemory;
    }
#endif
}

void runtime_threading_reset();

Status runtime_set_static_tls(const XexTlsInfo& tls) {
    if (!g_rt) return Status::NotInitialized;
    if (g_rt->static_tls_frozen.load()) return Status::Conflict;
    Status s = validate_xex_tls(tls);
    if (s != Status::Ok) return s;
    std::vector<uint8_t> initial;
    if (tls.raw_data_size) {
        const uint8_t* data = g_rt->mem->translate(tls.raw_data_address, tls.raw_data_size);
        if (!data) return Status::InvalidArgument;
        initial.assign(data, data + tls.raw_data_size);
    }
    g_rt->static_tls = tls;
    g_rt->static_tls_template = std::move(initial);
    return Status::Ok;
}

void runtime_request_title_quiesce() {
    if (!g_rt) return;
    // Wake dispatcher waits immediately; then interrupt native socket leases.
    // Neither step joins the calling guest thread.
    runtime_request_thread_quiesce();
    const Status s = begin_shutdown_xam_net_hle();
    if (s != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "title network cancellation: %s", status_name(s));
}

void runtime_shutdown() {
    if (!g_rt) return;
    runtime_request_title_quiesce();
    runtime_finish_thread_quiesce();
    shutdown_dpc_worker();  // after the managed workers, before the heap goes
    shutdown_xaudio();
    const Status network = shutdown_xam_net_hle();
    if (network != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "title network cleanup: %s", status_name(network));
    // The owner joins external entry/IRQ producers before runtime_shutdown.
    // Quiescing managed workers alone does not end those producers' lifetimes.
    host_fibers_shutdown();  // suspended guest fiber host stacks (after managed-thread quiescence)
    thread_objects_prepare_shutdown();
    xboxkrnl_kernel_variables_prepare_shutdown();
    virtual_fields_reset();
    reset_xma();
    runtime_reset_xconfig();
    reset_title_lifecycle();
    reset_xam_notifications();
    reset_xam_enum();
    reset_xam_content();
    runtime_remove_module_bindings();
    g_rt.reset();
    runtime_threading_reset();
}

Runtime* runtime() { return g_rt.get(); }

namespace {
uint8_t* ptr(uint32_t addr, uint32_t size) {
    return g_rt && g_rt->mem ? g_rt->mem->translate(addr, size) : nullptr;
}
}  // namespace

bool guest_read_be16(uint32_t addr, uint16_t* v) {
    uint8_t* p = ptr(addr, 2);
    if (!p) return false;
    uint16_t x;
    memcpy(&x, p, 2);
    *v = __builtin_bswap16(x);
    return true;
}

bool guest_read_be32(uint32_t addr, uint32_t* v) {
    uint8_t* p = ptr(addr, 4);
    if (!p) return false;
    uint32_t x;
    memcpy(&x, p, 4);
    *v = __builtin_bswap32(x);
    return true;
}

bool guest_write_be32(uint32_t addr, uint32_t v) {
    uint8_t* p = ptr(addr, 4);
    if (!p) return false;
    v = __builtin_bswap32(v);
    memcpy(p, &v, 4);
    note_title_write(addr, 4);
    return true;
}

bool guest_read_be64(uint32_t addr, uint64_t* v) {
    uint8_t* p = ptr(addr, 8);
    if (!p) return false;
    uint64_t x;
    memcpy(&x, p, 8);
    *v = __builtin_bswap64(x);
    return true;
}

bool guest_write_be64(uint32_t addr, uint64_t v) {
    uint8_t* p = ptr(addr, 8);
    if (!p) return false;
    v = __builtin_bswap64(v);
    memcpy(p, &v, 8);
    note_title_write(addr, 8);
    return true;
}

}  // namespace rcomp::rt
