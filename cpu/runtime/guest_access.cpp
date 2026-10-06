// Checked guest access hooks used when RCOMP_CHECKED_GUEST_ACCESS is defined.
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/ppc_prelude.h"
#include "rcomp/runtime_state.h"
#include <atomic>
#if defined(RCOMP_VIRTUAL_GUEST_ACCESS)
#include "rcomp/runtime/virtual_fields.h"
#include "rcomp/runtime/thread_object.h"
#endif

namespace rcomp {

namespace {
GuestMemory* g_mem = nullptr;
}

void set_active_guest_memory(GuestMemory* mem) { g_mem = mem; }
GuestMemory* active_guest_memory() { return g_mem; }

namespace {
// Accesses inside the coarse range [0x6FFFFFF0, 0x80000000) that no provider window claimed: ordinary
// accesses of the guest heap's upper part (include/rcomp/ppc_prelude.h). Expected to stay near zero.
std::atomic<uint64_t> g_window_fallbacks{0};
}
uint64_t virtual_window_fallback_count() { return g_window_fallbacks.load(std::memory_order_relaxed); }

bool guest_access_ok(uint32_t addr, uint32_t size) {
    return g_mem && g_mem->is_committed(addr, size);
}

// The generated code passes the unshifted guest address. With RCOMP_PHYSICAL_4K_WINDOW_OFFSET=1 the
// title runs GuestMemory with set_physical_4k_offset(true), and is_accessible() then checks an access
// inside the 0xE0000000 window on the page it reaches (ea + 0x1000), like the generated code's
// PPC_HOST_PTR; bad_guest_access reports the guest address.
bool guest_access_permitted(uint32_t addr, uint32_t size, int is_store) {
    return g_mem && g_mem->is_accessible(addr, size,
        is_store ? Protect::ReadWrite : Protect::Read);
}

void bad_guest_access(uint32_t addr, uint32_t size, int is_store) {
    rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "addr=0x%08X size=%u op=%s", addr, size,
                is_store ? "store" : "load");
}

#if defined(RCOMP_VIRTUAL_GUEST_ACCESS)
static_assert(kVirtualGuestArenaBase == rt::kOpaqueRuntimeArenaBase);
static_assert(kVirtualGuestArenaEnd == rt::kOpaqueRuntimeArenaEnd);
// The windows' fallbacks below form host pointers with PPC_HOST_PTR like the generated code; the coarse
// range lies below the 0xE0000000 window, so RCOMP_PHYSICAL_4K_WINDOW_OFFSET never shifts them and their
// permission checks take the guest address itself.
static_assert(uint64_t(kVirtualCoarseBase) + kVirtualCoarseSize <= kPhysical4KWindowBase);

namespace {
[[noreturn]] void virtual_access_failed(uint32_t address, uint8_t width,
                                      uint32_t lr, bool write,
                                      rt::VirtualAccessStatus status,
                                      const rt::VirtualAccessDiagnostic& diagnostic) {
    // Provider calls have returned; no provider mutex/shared_ptr is live across
    // a fatal callback or the host test harness's non-local exit.
    const auto kind = (status == rt::VirtualAccessStatus::ReadOnly ||
                       status == rt::VirtualAccessStatus::Stale)
        ? RCOMP_FATAL_GUEST_ACCESS : RCOMP_FATAL_UNIMPLEMENTED;
    rcomp_fatal(kind,
        "virtual_field provider=%s addr=0x%08X base=0x%08X offset=0x%X "
        "width=%u op=%s lr=0x%08X status=%s",
        diagnostic.provider ? diagnostic.provider : "unregistered-opaque-arena",
        address, diagnostic.region_base, diagnostic.offset, unsigned(width),
        write ? "store" : "load", lr, rt::virtual_access_status_name(status));
}
}

uint64_t virtual_guest_load(uint32_t address, uint8_t width, uint32_t lr) {
    uint64_t value = 0;
    rt::VirtualAccessDiagnostic diagnostic{};
    const auto status = rt::runtime_virtual_read(address, width, lr, &value, &diagnostic);
    if (status == rt::VirtualAccessStatus::Handled) return value;
    virtual_access_failed(address, width, lr, false, status, diagnostic);
}

void virtual_guest_store(uint32_t address, uint8_t width, uint64_t value, uint32_t lr) {
    rt::VirtualAccessDiagnostic diagnostic{};
    const auto status = rt::runtime_virtual_write(address, width, value, lr, &diagnostic);
    if (status == rt::VirtualAccessStatus::Handled) return;
    virtual_access_failed(address, width, lr, true, status, diagnostic);
}

// The generated code reaches these for every access inside the coarse range. The exact windows keep
// their provider dispatch (and the fatal for an address nobody serves); everything else is an
// ordinary access, checked against the committed pages like the checked build does.
uint64_t virtual_window_load(uint8_t* base, uint32_t address, uint8_t width, uint32_t lr) {
    // The calling thread's kernel-time counter (KTHREAD +0x58 of its own thread token) is the hot
    // virtual read of GTA IV (thousands a frame): answered from the thread's cache before the
    // provider dispatch (runtime/src/thread_object.cpp, thread_kernel_time_fast_read).
    if (uint32_t(address - rt::kThreadVirtualTokenBase) < rt::kOpaqueRuntimeArenaEnd - rt::kThreadVirtualTokenBase) {
        uint64_t value = 0;
        if (rt::thread_kernel_time_fast_read(address, width, &value)) return value;
    }
    if (touches_virtual_guest_arena(address, width)) return virtual_guest_load(address, width, lr);
    g_window_fallbacks.fetch_add(1, std::memory_order_relaxed);
    if (!guest_access_permitted(address, width, 0)) bad_guest_access(address, width, 0);
    switch (width) {
    case 1: return guest_scalar_endian(reinterpret_cast<volatile GuestScalar<uint8_t>*>(PPC_HOST_PTR(address))->value);
    case 2: return guest_scalar_endian(reinterpret_cast<volatile GuestScalar<uint16_t>*>(PPC_HOST_PTR(address))->value);
    case 4: return guest_scalar_endian(reinterpret_cast<volatile GuestScalar<uint32_t>*>(PPC_HOST_PTR(address))->value);
    default: return guest_scalar_endian(reinterpret_cast<volatile GuestScalar<uint64_t>*>(PPC_HOST_PTR(address))->value);
    }
}

void virtual_window_store(uint8_t* base, uint32_t address, uint8_t width, uint64_t value, uint32_t lr) {
    if (touches_virtual_guest_arena(address, width)) {
        virtual_guest_store(address, width, value, lr);
        return;
    }
    g_window_fallbacks.fetch_add(1, std::memory_order_relaxed);
    if (!guest_access_permitted(address, width, 1)) bad_guest_access(address, width, 1);
    switch (width) {
    case 1: reinterpret_cast<volatile GuestScalar<uint8_t>*>(PPC_HOST_PTR(address))->value = uint8_t(value); break;
    case 2: reinterpret_cast<volatile GuestScalar<uint16_t>*>(PPC_HOST_PTR(address))->value = guest_scalar_endian(uint16_t(value)); break;
    case 4: reinterpret_cast<volatile GuestScalar<uint32_t>*>(PPC_HOST_PTR(address))->value = guest_scalar_endian(uint32_t(value)); break;
    default: reinterpret_cast<volatile GuestScalar<uint64_t>*>(PPC_HOST_PTR(address))->value = guest_scalar_endian(value); break;
    }
    // The coarse range lies below the physical windows: nothing for the GPU write tracking to record.
}
#endif

}  // namespace rcomp
