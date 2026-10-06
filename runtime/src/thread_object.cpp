#include "rcomp/runtime/thread_object.h"

#include <pthread.h>

#include <atomic>
#include <map>
#include <mutex>
#include <new>
#include <utility>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_heap.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/kernel_variables.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/virtual_fields.h"

namespace rcomp::rt {
namespace {
constexpr uint32_t kOpaqueArenaBase = kOpaqueRuntimeArenaBase;
constexpr uint32_t kOpaqueArenaSize = kOpaqueRuntimeArenaSize;
constexpr uint32_t kOpaqueTokenAlign = kThreadVirtualTokenStride;
constexpr char kOpaqueArenaTag[] = "rcomp-thread-object-tokens";

VirtualAccessStatus thread_field_read(uint32_t, uint8_t, uint64_t*, VirtualAccessDiagnostic*);
VirtualAccessStatus thread_field_write(uint32_t, uint8_t, uint64_t, VirtualAccessDiagnostic*);
const VirtualFieldProvider kThreadFields{
    kThreadVirtualTokenBase, kOpaqueRuntimeArenaEnd - kThreadVirtualTokenBase,
    "thread-object", &thread_field_read, &thread_field_write};

struct RegistryEntry {
    std::weak_ptr<ThreadObjectIdentity> object;
    std::shared_ptr<ThreadObjectIdentity> guest_owner;
};

std::mutex g_object_mutex;
std::map<uint32_t, RegistryEntry> g_objects;
GuestMemory* g_arena_memory = nullptr;
uint32_t g_next_token = kThreadVirtualTokenBase;
uint32_t g_type_address = 0;
uint32_t g_event_type_address = 0;
bool g_shutting_down = false;

Status from_mem(MemStatus status) {
    switch (status) {
    case MemStatus::Ok: return Status::Ok;
    case MemStatus::NotReserved: return Status::NotInitialized;
    case MemStatus::OutOfMemory: return Status::OutOfMemory;
    case MemStatus::Conflict: return Status::Conflict;
    case MemStatus::InvalidArgument:
    case MemStatus::PlatformError: return Status::InvalidArgument;
    }
    return Status::InvalidArgument;
}

Status ensure_arena_locked(GuestMemory& memory) {
    if (g_arena_memory && g_arena_memory != &memory) return Status::Conflict;
    Status status = from_mem(memory.reserve_opaque_runtime_range(
        kOpaqueArenaBase, kOpaqueArenaSize, kOpaqueArenaTag));
    if (status == Status::Ok) status = register_virtual_field_provider(kThreadFields);
    if (status == Status::Ok && !g_arena_memory) {
        g_arena_memory = &memory;
        g_next_token = kThreadVirtualTokenBase;
    }
    return status;
}

Status mint_token_locked(uint32_t* out) {
    if (!out || !g_arena_memory) return Status::NotInitialized;
    if (g_next_token < kThreadVirtualTokenBase ||
        g_next_token > kOpaqueArenaBase + kOpaqueArenaSize - kOpaqueTokenAlign)
        return Status::OutOfMemory;
    *out = g_next_token;
    g_next_token += kOpaqueTokenAlign;
    return Status::Ok;
}
}  // namespace

struct ThreadObjectIdentity {
    uint64_t generation = 0;
    uint32_t body = 0;
    uint32_t thread_id = 0;
    uint32_t entry = 0;
    std::atomic<uint32_t> guest_refs{0};
    std::atomic<uint32_t> handle_refs{0};
    std::atomic<bool> exited{false};
    std::atomic<uint32_t> exit_code{0};
    std::atomic<uint32_t> last_error{0};
    std::atomic<int32_t> base_priority{0};
    std::atomic<uint32_t> affinity{kDefaultThreadAffinity};
    std::atomic<uint32_t> disable_boost{0};
    // KTHREAD stack fields (+0xD0 StackAllocBase, +0x5C StackBase, +0x60
    // StackLimit) and xapi's fiber pointer (+0x164), runtime/docs/THREAD_OBJECTS.md ("Guest fibers").
    std::atomic<uint32_t> stack_alloc_base{0};
    std::atomic<uint32_t> stack_base{0};
    std::atomic<uint32_t> stack_limit{0};
    std::atomic<uint32_t> fiber{0};
    bool registered = false;
    ~ThreadObjectIdentity();
};

namespace {
// Published 32-bit KTHREAD fields other than +0x58 (kernel time, see below).
// Returns false for an offset without a contract.
bool published_field(const ThreadObjectIdentity& identity, uint32_t offset, uint64_t* value) {
    switch (offset) {
    case 0x05C: *value = identity.stack_base.load(std::memory_order_relaxed); return true;
    case 0x060: *value = identity.stack_limit.load(std::memory_order_relaxed); return true;
    case 0x084: *value = kTitleProcessVirtualAddress; return true;  // the title process (kernel_variables.h)
    case 0x0D0: *value = identity.stack_alloc_base.load(std::memory_order_relaxed); return true;
    case 0x14C: *value = identity.thread_id; return true;
    case 0x160: *value = identity.last_error.load(std::memory_order_relaxed); return true;
    case 0x164: *value = identity.fiber.load(std::memory_order_relaxed); return true;
    default: return false;
    }
}
// Guest-writable fields: LastError and the fiber pointer. The others are
// published read-only (the stack fields change through KeSetCurrentStackPointers).
VirtualAccessStatus write_field(ThreadObjectIdentity& identity, uint32_t offset, uint64_t value) {
    switch (offset) {
    case 0x160: identity.last_error.store(uint32_t(value), std::memory_order_relaxed); return VirtualAccessStatus::Handled;
    case 0x164: identity.fiber.store(uint32_t(value), std::memory_order_relaxed); return VirtualAccessStatus::Handled;
    case 0x05C: case 0x060: case 0x084: case 0x0D0: case 0x14C: return VirtualAccessStatus::ReadOnly;
    default: return VirtualAccessStatus::UnknownField;
    }
}

void thread_field_diagnostic(uint32_t ea, uint32_t body, VirtualAccessDiagnostic* diagnostic) {
    if (!diagnostic) return;
    diagnostic->provider = "thread-object";
    diagnostic->region_base = body;
    diagnostic->offset = ea - body;
}

// Kernel-time fast path. GTA IV reads its own counter thousands of times per
// frame; a registry lookup plus a CPU-time system call per read dominated its
// main thread. The counter's unit is 20 ms, so the calling thread keeps its own
// Body (verified once through the registry) and the last value, refreshed from
// CLOCK_THREAD_CPUTIME_ID at most once per millisecond of TSC time
// (cpu/runtime/timebase.cpp; without a calibrated TSC every read refreshes).
extern "C" uint64_t rcomp_tsc_to_tb __attribute__((weak));  // 32.32 fixed point; absent in host tests
// One cache per native thread, behind a pthread key rather than a thread_local: the SDK forces
// emulated TLS, where every thread_local access is a call into __emutls_get_address, which calls
// pthread_getspecific itself (together 7 % of the main thread's samples in phase 60, mostly this
// read in the guest's GPU-wait loop). One pthread_getspecific per field access remains.
struct SelfCache {
    uint32_t body = 0;
    ThreadObjectIdentity* identity = nullptr;  // the calling thread keeps its own object alive
    uint64_t kernel_time_tsc = 0;
    uint32_t kernel_time_value = 0;
    bool kernel_time_valid = false;
};
pthread_key_t g_self_key;
pthread_once_t g_self_once = PTHREAD_ONCE_INIT;
std::atomic<bool> g_self_key_ready{false};
void self_key_create() {
    if (pthread_key_create(&g_self_key, [](void* cache) { delete static_cast<SelfCache*>(cache); }) != 0)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "pthread_key_create failed for the thread-object cache");
    g_self_key_ready.store(true, std::memory_order_release);
}
// The calling thread's cache, or nullptr until its first slow-path read created one.
inline SelfCache* self_cache() {
    return g_self_key_ready.load(std::memory_order_acquire)
               ? static_cast<SelfCache*>(pthread_getspecific(g_self_key)) : nullptr;
}
// The calling thread's cache, created on demand.
SelfCache& self_cache_create() {
    pthread_once(&g_self_once, self_key_create);
    SelfCache* cache = static_cast<SelfCache*>(pthread_getspecific(g_self_key));
    if (!cache) {
        cache = new (std::nothrow) SelfCache();
        if (!cache || pthread_setspecific(g_self_key, cache) != 0)
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "thread-object cache allocation failed");
    }
    return *cache;
}

uint32_t read_kernel_time_ms20() {
    timespec cpu{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    // Compatibility scaling (kThreadKernelTimeDivisor): the title's D3D GPU-hang watchdog declares a
    // hang when this counter advances by 5000 while the GPU makes no progress. Host-side shader
    // translation and pipeline creation are far slower than a console GPU, so the counter runs at
    // 1/kThreadKernelTimeDivisor of the measured CPU milliseconds. The unit of the real counter is
    // not independently established, so this is a documented assumption, not a timing claim.
    constexpr uint64_t kThreadKernelTimeDivisor = 20;
    return uint32_t((uint64_t(cpu.tv_sec) * 1000u + uint64_t(cpu.tv_nsec) / 1000000u) / kThreadKernelTimeDivisor);
}

// TSC ticks per millisecond, from the start-up calibration (0: not calibrated). The scale is
// fixed once the title runs, so the 64-bit division is done once instead of on every read.
uint64_t kernel_time_tsc_per_ms() {
    static const uint64_t value = [] {
        const uint64_t scale = &rcomp_tsc_to_tb ? rcomp_tsc_to_tb : 0;
        return scale ? (uint64_t(50000) << 32) / scale : uint64_t(0);
    }();
    return value;
}

bool kernel_time_fast(SelfCache& self, uint32_t body, uint8_t width, uint64_t* value) {
    if (!self.body || body != self.body || width != 4 || !value) return false;
#if defined(__x86_64__)
    const uint64_t now = __builtin_ia32_rdtsc();
    const uint64_t tsc_per_ms = kernel_time_tsc_per_ms();
    if (!self.kernel_time_valid || !tsc_per_ms || now - self.kernel_time_tsc >= tsc_per_ms) {
        self.kernel_time_value = read_kernel_time_ms20();
        self.kernel_time_tsc = now;
        self.kernel_time_valid = true;
    }
    *value = self.kernel_time_value;
#else
    *value = read_kernel_time_ms20();
#endif
    return true;
}

VirtualAccessStatus thread_field_read(uint32_t ea, uint8_t width, uint64_t* value,
                                     VirtualAccessDiagnostic* diagnostic) {
    const uint32_t body = ea - (ea - kThreadVirtualTokenBase) % kThreadVirtualTokenStride;
    if (SelfCache* self = self_cache(); self && body == self->body && self->identity && width == 4 && value) {
        const uint32_t offset = ea - body;
        if (offset == 0x58 && kernel_time_fast(*self, body, width, value)) return VirtualAccessStatus::Handled;
        if (published_field(*self->identity, offset, value)) return VirtualAccessStatus::Handled;
    }
    thread_field_diagnostic(ea, body, diagnostic);
    std::shared_ptr<ThreadObjectIdentity> identity;
    if (find_thread_object(body, &identity) != Status::Ok) return VirtualAccessStatus::Stale;
    const uint32_t offset = ea - body;
    if (GuestThread* current = current_guest_thread(); current && current->identity == identity) {
        SelfCache& self = self_cache_create();
        self.body = body;
        self.identity = identity.get();
    }
    if (offset == 0x58) {
        // Kernel-time counter. GTA IV reads its own thread's value in pairs
        // (scope begin/end) and subtracts; no other use is established, and the
        // pinned public headers name the bytes only "unk_58" / "kernel time".
        // Published as the reading thread's own CPU time in milliseconds
        // (monotonic, wraps at 32 bits). Units are an assumption; another
        // thread's counter has no source and stays unknown.
        GuestThread* current = current_guest_thread();
        if (!current || current->identity != identity) return VirtualAccessStatus::UnknownField;
        if (width != 4 || !value) return VirtualAccessStatus::InvalidWidth;
        SelfCache& self = self_cache_create();
        self.kernel_time_valid = false;
        kernel_time_fast(self, body, width, value);
        return VirtualAccessStatus::Handled;
    }
    uint64_t field = 0;
    if (!published_field(*identity, offset, &field)) return VirtualAccessStatus::UnknownField;
    if (width != 4 || !value) return VirtualAccessStatus::InvalidWidth;
    *value = field;
    return VirtualAccessStatus::Handled;
}

VirtualAccessStatus thread_field_write(uint32_t ea, uint8_t width, uint64_t value,
                                      VirtualAccessDiagnostic* diagnostic) {
    const uint32_t body = ea - (ea - kThreadVirtualTokenBase) % kThreadVirtualTokenStride;
    if (SelfCache* self = self_cache(); self && body == self->body && self->identity && width == 4 &&
        (ea - body == 0x160 || ea - body == 0x164))
        return write_field(*self->identity, ea - body, value);
    thread_field_diagnostic(ea, body, diagnostic);
    std::shared_ptr<ThreadObjectIdentity> identity;
    if (find_thread_object(body, &identity) != Status::Ok) return VirtualAccessStatus::Stale;
    const uint32_t offset = ea - body;
    uint64_t ignored = 0;
    if (!published_field(*identity, offset, &ignored)) return VirtualAccessStatus::UnknownField;
    if (width != 4) return VirtualAccessStatus::InvalidWidth;
    return write_field(*identity, offset, value);
}
} // namespace

ThreadObjectIdentity::~ThreadObjectIdentity() {
    if (!registered) return;
    std::lock_guard<std::mutex> lock(g_object_mutex);
    auto it = g_objects.find(body);
    if (it != g_objects.end()) g_objects.erase(it);
    registered = false;
}

Status reserve_thread_object_arena(GuestMemory& memory) {
    // runtime_init may still fail after this reservation (invalid heap/physical
    // configuration, platform init, ...). Do not adopt global registry state
    // until a live Runtime later creates an identity or type token.
    return from_mem(memory.reserve_opaque_runtime_range(
        kOpaqueArenaBase, kOpaqueArenaSize, kOpaqueArenaTag));
}

Status create_thread_object_identity(GuestHeap& heap, uint32_t thread_id, uint32_t entry,
                                     std::shared_ptr<ThreadObjectIdentity>* out) {
    if (!out || !thread_id) return Status::InvalidArgument;
    Runtime* owner = runtime();
    if (!owner || &owner->heap != &heap || !owner->mem) return Status::NotInitialized;
    std::shared_ptr<ThreadObjectIdentity> identity;
#if defined(__cpp_exceptions)
    try { identity = std::make_shared<ThreadObjectIdentity>(); }
    catch (const std::bad_alloc&) { return Status::OutOfMemory; }
#else
    identity = std::make_shared<ThreadObjectIdentity>();
#endif
#if defined(__cpp_exceptions)
    try {
#endif
        std::lock_guard<std::mutex> lock(g_object_mutex);
        if (g_shutting_down) return Status::Conflict;
        Status status = ensure_arena_locked(*owner->mem);
        if (status != Status::Ok) return status;
        status = mint_token_locked(&identity->body);
        if (status != Status::Ok) return status;
        identity->generation = owner->generation;
        identity->thread_id = thread_id;
        identity->entry = entry;
        if (!g_objects.emplace(identity->body, RegistryEntry{identity, {}}).second)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "opaque thread token collision 0x%08X",
                        identity->body);
        identity->registered = true;
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) { return Status::OutOfMemory; }
#endif
    *out = std::move(identity);
    return Status::Ok;
}

uint32_t thread_object_body(const std::shared_ptr<ThreadObjectIdentity>& identity) {
    return identity ? identity->body : 0;
}

void thread_object_handle_opened(const std::shared_ptr<ThreadObjectIdentity>& identity) {
    if (identity) identity->handle_refs.fetch_add(1, std::memory_order_relaxed);
}

void thread_object_handle_closed(const std::shared_ptr<ThreadObjectIdentity>& identity) {
    if (!identity) return;
    const uint32_t old = identity->handle_refs.fetch_sub(1, std::memory_order_relaxed);
    if (!old) rcomp_fatal(RCOMP_FATAL_INTERNAL, "thread object handle reference underflow");
}

uint32_t thread_object_handle_count(const std::shared_ptr<ThreadObjectIdentity>& identity) {
    return identity ? identity->handle_refs.load(std::memory_order_relaxed) : 0;
}

Status find_thread_object(uint32_t body, std::shared_ptr<ThreadObjectIdentity>* out) {
    if (!body || !out) return Status::InvalidArgument;
    std::shared_ptr<ThreadObjectIdentity> identity;
    {
        std::lock_guard<std::mutex> lock(g_object_mutex);
        auto it = g_objects.find(body);
        if (it == g_objects.end()) return Status::NotFound;
        identity = it->second.object.lock();
        if (!identity || identity->body != body) return Status::NotFound;
    }
    // Replacing the caller's previous shared_ptr may destroy another identity,
    // whose destructor re-enters g_object_mutex. Publish only after unlock.
    *out = std::move(identity);
    return Status::Ok;
}

Status reference_thread_object(uint32_t body, std::shared_ptr<ThreadObjectIdentity>* out) {
    if (!body) return Status::InvalidArgument;
    std::shared_ptr<ThreadObjectIdentity> identity;
    {
        std::lock_guard<std::mutex> lock(g_object_mutex);
        if (g_shutting_down) return Status::NotInitialized;
        auto it = g_objects.find(body);
        if (it == g_objects.end()) return Status::NotFound;
        identity = it->second.object.lock();
        if (!identity || identity->body != body) return Status::NotFound;
        const uint32_t refs = identity->guest_refs.load(std::memory_order_relaxed);
        if (refs == UINT32_MAX) return Status::Conflict;
        if (!refs) it->second.guest_owner = identity;
        identity->guest_refs.store(refs + 1, std::memory_order_relaxed);
    }
    if (out) *out = std::move(identity);
    return Status::Ok;
}

Status dereference_thread_object(uint32_t body) {
    if (!body) return Status::InvalidArgument;
    std::shared_ptr<ThreadObjectIdentity> released;
    {
        std::lock_guard<std::mutex> lock(g_object_mutex);
        auto it = g_objects.find(body);
        if (it == g_objects.end()) return Status::NotFound;
        auto identity = it->second.object.lock();
        if (!identity || identity->body != body) return Status::NotFound;
        const uint32_t refs = identity->guest_refs.load(std::memory_order_relaxed);
        if (!refs) return Status::InvalidArgument;
        identity->guest_refs.store(refs - 1, std::memory_order_relaxed);
        if (refs == 1) released = std::move(it->second.guest_owner);
    }
    return Status::Ok;
}

// ObLookupAnyThreadByThreadId: the live identity with this thread id gains one
// guest Body reference (released by ObDereferenceObject), as reference_thread_object.
Status reference_thread_object_by_thread_id(uint32_t thread_id, uint32_t* body) {
    if (!thread_id || !body) return Status::InvalidArgument;
    std::shared_ptr<ThreadObjectIdentity> identity;
    {
        std::lock_guard<std::mutex> lock(g_object_mutex);
        if (g_shutting_down) return Status::NotInitialized;
        for (auto& item : g_objects) {
            auto candidate = item.second.object.lock();
            if (!candidate || candidate->thread_id != thread_id || candidate->body != item.first) continue;
            const uint32_t refs = candidate->guest_refs.load(std::memory_order_relaxed);
            if (refs == UINT32_MAX) return Status::Conflict;
            if (!refs) item.second.guest_owner = candidate;
            candidate->guest_refs.store(refs + 1, std::memory_order_relaxed);
            *body = candidate->body;
            identity = std::move(candidate);  // released after the registry mutex
            break;
        }
    }
    return identity ? Status::Ok : Status::NotFound;
}

uint32_t thread_object_guest_reference_count(const std::shared_ptr<ThreadObjectIdentity>& identity) {
    return identity ? identity->guest_refs.load(std::memory_order_relaxed) : 0;
}

void thread_object_mark_exited(const std::shared_ptr<ThreadObjectIdentity>& identity,
                               uint32_t exit_code) {
    if (!identity) return;
    identity->exit_code.store(exit_code, std::memory_order_relaxed);
    identity->exited.store(true, std::memory_order_release);
}

bool thread_object_exited(const std::shared_ptr<ThreadObjectIdentity>& identity) {
    return identity && identity->exited.load(std::memory_order_acquire);
}

uint32_t thread_object_exit_code(const std::shared_ptr<ThreadObjectIdentity>& identity) {
    return identity ? identity->exit_code.load(std::memory_order_relaxed) : 0;
}

uint32_t thread_object_thread_id(const std::shared_ptr<ThreadObjectIdentity>& identity) {
    return identity ? identity->thread_id : 0;
}

int32_t thread_object_exchange_base_priority(const std::shared_ptr<ThreadObjectIdentity>& identity,
                                             int32_t value) {
    return identity ? identity->base_priority.exchange(value, std::memory_order_relaxed) : 0;
}

int32_t thread_object_base_priority(const std::shared_ptr<ThreadObjectIdentity>& identity) {
    return identity ? identity->base_priority.load(std::memory_order_relaxed) : 0;
}

uint32_t thread_object_exchange_affinity(const std::shared_ptr<ThreadObjectIdentity>& identity,
                                         uint32_t value) {
    return identity ? identity->affinity.exchange(value, std::memory_order_relaxed) : 0;
}

uint32_t thread_object_affinity(const std::shared_ptr<ThreadObjectIdentity>& identity) {
    return identity ? identity->affinity.load(std::memory_order_relaxed) : 0;
}

uint32_t thread_object_exchange_disable_boost(const std::shared_ptr<ThreadObjectIdentity>& identity,
                                              uint32_t value) {
    return identity ? identity->disable_boost.exchange(value, std::memory_order_relaxed) : 0;
}

void thread_object_set_stack(const std::shared_ptr<ThreadObjectIdentity>& identity, uint32_t alloc_base,
                             uint32_t stack_base, uint32_t stack_limit) {
    if (!identity) return;
    identity->stack_alloc_base.store(alloc_base, std::memory_order_relaxed);
    identity->stack_base.store(stack_base, std::memory_order_relaxed);
    identity->stack_limit.store(stack_limit, std::memory_order_relaxed);
}

uint32_t thread_object_fiber(const std::shared_ptr<ThreadObjectIdentity>& identity) {
    return identity ? identity->fiber.load(std::memory_order_relaxed) : 0;
}

uint32_t thread_object_type_address() {
    std::lock_guard<std::mutex> lock(g_object_mutex);
    return g_type_address;
}

Status register_thread_object_type_variable() {
    Runtime* owner = runtime();
    if (!owner || !owner->mem) return Status::NotInitialized;
    uint32_t address = 0;
    {
        std::lock_guard<std::mutex> lock(g_object_mutex);
        if (g_shutting_down) return Status::Conflict;
        Status status = ensure_arena_locked(*owner->mem);
        if (status != Status::Ok) return status;
        g_type_address = kExThreadObjectTypeVirtualAddress;
        g_event_type_address = kExEventObjectTypeVirtualAddress;
        address = g_type_address;
    }
    Status status = register_variable_import(kModuleXboxkrnl, 0x001B, address,
                                             "ExThreadObjectType");
    if (status != Status::Ok) return status;
    // ExEventObjectType (0x000E): the same opaque identity-only contract, used as
    // the type filter of ObReferenceObjectByHandle (src/hle_xboxkrnl_objects.cpp).
    status = register_variable_import(kModuleXboxkrnl, 0x000E, kExEventObjectTypeVirtualAddress,
                                      "ExEventObjectType");
    if (status != Status::Ok) unregister_variable_import(kModuleXboxkrnl, 0x001B);
    return status;
}

uint32_t event_object_type_address() {
    std::lock_guard<std::mutex> lock(g_object_mutex);
    return g_event_type_address;
}

void thread_objects_prepare_shutdown() {
    {
        std::lock_guard<std::mutex> lock(g_object_mutex);
        g_shutting_down = true;
        for (auto& item : g_objects) {
            if (auto identity = item.second.object.lock()) {
                identity->guest_refs.store(0, std::memory_order_relaxed);
                identity->registered = false;
            }
            item.second.guest_owner.reset();
        }
        g_objects.clear();
        g_type_address = 0;
        g_event_type_address = 0;
    }
    unregister_variable_import(kModuleXboxkrnl, 0x001B);
    unregister_variable_import(kModuleXboxkrnl, 0x000E);
    unregister_virtual_field_provider(kThreadFields);
}

void thread_objects_reset() {
    std::lock_guard<std::mutex> lock(g_object_mutex);
    g_objects.clear();
    g_arena_memory = nullptr;
    g_next_token = kThreadVirtualTokenBase;
    g_type_address = 0;
    g_event_type_address = 0;
    g_shutting_down = false;
}

bool thread_kernel_time_fast_read(uint32_t ea, uint8_t width, uint64_t* value) {
    if (width != 4 || !value) return false;
    SelfCache* self = self_cache();
    if (!self || !self->identity || !self->body) return false;
    // Token pages are kThreadVirtualTokenStride-aligned: the body is the page, the field its offset.
    const uint32_t body = ea & ~(kThreadVirtualTokenStride - 1);
    if (body != self->body || ea - body != 0x58) return false;
    return kernel_time_fast(*self, body, width, value);
}

}  // namespace rcomp::rt
