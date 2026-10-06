// Runtime-owned thread object identity. The guest-visible Body value is an
// opaque token in an inaccessible guest range. Generated scalar accesses may
// read ThreadId (+0x14C), read/write LastError (+0x160) and FiberPtr (+0x164),
// and read the stack fields (+0x5C, +0x60, +0xD0) and the process pointer
// (+0x84) through the virtual provider (runtime/docs/KERNEL_FIELDS.md). Other
// KTHREAD/OBJECT_HEADER fields have no published layout.
#pragma once

#include <stdint.h>
#include <memory>

#include "rcomp/runtime/status.h"

namespace rcomp { class GuestMemory; }

namespace rcomp::rt {

class GuestHeap;
struct ThreadObjectIdentity;

// Reserve the fixed opaque-token arena before GuestHeap::init so the heap
// excludes it from its free list. The range stays uncommitted; supported fields
// are supplied by the virtual-access provider. Exact repeat is idempotent.
Status reserve_thread_object_arena(::rcomp::GuestMemory& memory);

// Creates one identity for a real runtime GuestThread. The opaque Body stays
// valid while execution, a thread HandleObject, or a guest Body reference
// owns the identity. Standalone GuestThread instances outside Runtime do not
// call this API.
Status create_thread_object_identity(GuestHeap& heap, uint32_t thread_id, uint32_t entry,
                                     std::shared_ptr<ThreadObjectIdentity>* out);
uint32_t thread_object_body(const std::shared_ptr<ThreadObjectIdentity>& identity);

// Handle ownership is tracked independently from guest Body references.
void thread_object_handle_opened(const std::shared_ptr<ThreadObjectIdentity>& identity);
void thread_object_handle_closed(const std::shared_ptr<ThreadObjectIdentity>& identity);
uint32_t thread_object_handle_count(const std::shared_ptr<ThreadObjectIdentity>& identity);

// Guest pointer references acquired by ObReferenceObject* persist after the
// numeric handle is closed. Unknown/stale Body values and underflow fail.
Status reference_thread_object(uint32_t body, std::shared_ptr<ThreadObjectIdentity>* out = nullptr);
Status dereference_thread_object(uint32_t body);
Status find_thread_object(uint32_t body, std::shared_ptr<ThreadObjectIdentity>* out);
// ObLookupAnyThreadByThreadId: adds one guest Body reference to the live
// identity with this thread id and returns its Body. NotFound when no thread
// object with that id exists (never created, or its last owner released it).
Status reference_thread_object_by_thread_id(uint32_t thread_id, uint32_t* body);
uint32_t thread_object_guest_reference_count(const std::shared_ptr<ThreadObjectIdentity>& identity);

void thread_object_mark_exited(const std::shared_ptr<ThreadObjectIdentity>& identity,
                               uint32_t exit_code);

// Fast path of the most frequent virtual field read: the calling thread's own kernel-time counter
// (KTHREAD +0x58, read by GTA IV thousands of times a frame around its profiling scopes). True and
// *value set when `ea` is that field of the calling thread's own Body and its cache is warm; false
// otherwise (the caller then takes the provider path, which also warms the cache). No registry
// lookup, no provider search, no diagnostic record: a thread-local lookup and the TSC.
bool thread_kernel_time_fast_read(uint32_t ea, uint8_t width, uint64_t* value);
bool thread_object_exited(const std::shared_ptr<ThreadObjectIdentity>& identity);
uint32_t thread_object_exit_code(const std::shared_ptr<ThreadObjectIdentity>& identity);
uint32_t thread_object_thread_id(const std::shared_ptr<ThreadObjectIdentity>& identity);

// Scheduler controls published through KeSet*Thread. They are per-thread
// state whose previous value is returned to the caller; the host scheduler is
// not reconfigured. Defaults: increment 0, boost enabled, all six Xbox 360
// hardware threads (mask 0x3F). Each exchange returns the previous value.
constexpr uint32_t kDefaultThreadAffinity = 0x3F;
int32_t thread_object_exchange_base_priority(const std::shared_ptr<ThreadObjectIdentity>& identity,
                                             int32_t value);
// Current base-priority increment (KeQueryBasePriorityThread).
int32_t thread_object_base_priority(const std::shared_ptr<ThreadObjectIdentity>& identity);
uint32_t thread_object_exchange_affinity(const std::shared_ptr<ThreadObjectIdentity>& identity,
                                         uint32_t value);
// Current hardware-thread mask (kDefaultThreadAffinity when the title never set one; 0 without identity).
uint32_t thread_object_affinity(const std::shared_ptr<ThreadObjectIdentity>& identity);
uint32_t thread_object_exchange_disable_boost(const std::shared_ptr<ThreadObjectIdentity>& identity,
                                              uint32_t value);
// KTHREAD stack fields published through the virtual provider: +0x5C
// StackBase (high end), +0x60 StackLimit (low end), +0xD0 StackAllocBase.
// Set from the GuestThread at creation and by KeSetCurrentStackPointers
// (runtime/docs/THREAD_OBJECTS.md ("Guest fibers")).
void thread_object_set_stack(const std::shared_ptr<ThreadObjectIdentity>& identity, uint32_t alloc_base,
                             uint32_t stack_base, uint32_t stack_limit);
// KTHREAD+0x164: the current xapi fiber block (0 until ConvertThreadToFiber).
uint32_t thread_object_fiber(const std::shared_ptr<ThreadObjectIdentity>& identity);

// Exact descriptor identity used only as an ObReferenceObjectByHandle type
// filter. It occupies the fixed type identity in the kernel token page.
uint32_t thread_object_type_address();
// Allocates and registers the ExThreadObjectType variable as an opaque,
// inaccessible type-identity token. This contract is intentionally limited to
// identity comparison; guest field dereferences fault rather than observe a
// fabricated OBJECT_TYPE layout.
// Also registers ExEventObjectType (0x000E) at kExEventObjectTypeVirtualAddress,
// the same identity-only token contract (runtime/docs/THREAD_OBJECTS.md).
Status register_thread_object_type_variable();
uint32_t event_object_type_address();  // 0 before registration

// Called while Runtime/heap are still alive, after all managed workers stop.
// It invalidates every Body token and removes the virtual provider even when
// a leaked guest reference exists. reset() runs after Runtime destruction.
void thread_objects_prepare_shutdown();
void thread_objects_reset();

}  // namespace rcomp::rt
