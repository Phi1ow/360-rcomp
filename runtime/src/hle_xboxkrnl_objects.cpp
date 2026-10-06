// xboxkrnl object-manager subset backed by real R-comp handle/thread state.
#include "rcomp/runtime/objects.h"
#include "rcomp/runtime/xam_enum.h"
#include "rcomp/runtime/xam_misc.h"

#include <memory>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/handle_table.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/xboxkrnl_devices.h"
#include "kernel_internal.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kCurrentThread = 0xFFFFFFFEu;
constexpr uint32_t kCurrentProcess = 0xFFFFFFFFu;

Runtime& rt_or_die(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s before runtime_init", fn);
    return *r;
}

[[noreturn]] void unimplemented(const char* fn, PPCContext& ctx, const char* what,
                                uint32_t value) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                "xboxkrnl.exe!%s %s=0x%08X lr=0x%08X (not implemented)", fn,
                what, value, (uint32_t)ctx.lr);
}

bool writable_word(Runtime& r, uint32_t address) {
    return address && !(address & 3) &&
           r.mem->is_accessible(address, 4, Protect::ReadWrite);
}

uint32_t object_body_from_handle(Runtime& r, uint32_t handle, uint32_t type,
                                 const char* fn, PPCContext& ctx,
                                 std::shared_ptr<HandleObject>* retained) {
    const uint32_t thread_type = thread_object_type_address();
    const uint32_t event_type = event_object_type_address();
    if (type && event_type && type == event_type) {
        // ExEventObjectType filter: only an event handle matches. R-comp's
        // events are host dispatcher objects without a guest KEVENT Body, so a
        // matching event cannot be referenced honestly; anything else is the
        // console's type mismatch.
        if (handle == kCurrentThread || handle == kCurrentProcess) return nt::kObjectTypeMismatch;
        std::shared_ptr<HandleObject> object;
        if (r.handles.lookup_any(handle, &object) != Status::Ok) return nt::kInvalidHandle;
        if (object->kind() == HandleKind::Event)
            unimplemented(fn, ctx, "event_handle_has_no_guest_KEVENT_body", handle);
        return nt::kObjectTypeMismatch;
    }
    if (type && (!thread_type || type != thread_type)) return nt::kObjectTypeMismatch;

    if (handle == kCurrentProcess)
        unimplemented(fn, ctx, "current_process_pseudo_handle", handle);

    if (handle == kCurrentThread) {
        GuestThread* current = current_guest_thread();
        if (!current || !current->identity) return nt::kInvalidHandle;
        const uint32_t body = thread_object_body(current->identity);
        if (!body) return nt::kInvalidHandle;
        return body;
    }

    std::shared_ptr<HandleObject> object;
    const Status status = r.handles.lookup_any(handle, &object);
    if (status != Status::Ok) return nt::kInvalidHandle;
    if (object->kind() != HandleKind::Thread) {
        if (type) return nt::kObjectTypeMismatch;
        // R-comp currently has no guest Body identity for the other handle
        // classes. Advertising one here would manufacture an object ABI.
        unimplemented(fn, ctx, "untyped_handle_kind", (uint32_t)object->kind());
    }
    const uint32_t body = object->guest_object_body();
    if (!body)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s live thread handle has no object body", fn);
    if (retained) *retained = std::move(object);
    return body;
}

// NtDuplicateObject(HANDLE Source, PHANDLE Target, ULONG Options).
// Only the corroborated options==0 ordinary-handle subset is published. The
// output is validated before the table transaction, so failure cannot leak an
// unreachable duplicate.
void NtDuplicateObject(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtDuplicateObject";
    Runtime& r = rt_or_die(fn);
    const uint32_t source = ctx.r3.u32, output = ctx.r4.u32, options = ctx.r5.u32;
    if (!writable_word(r, output)) { ctx.r3.u64 = nt::kAccessViolation; return; }
    if (options) unimplemented(fn, ctx, "options", options);
    if (source == kCurrentThread || source == kCurrentProcess)
        unimplemented(fn, ctx, "pseudo_source", source);
    uint32_t duplicate = 0;
    const Status status = r.handles.duplicate(source, &duplicate);
    if (status != Status::Ok) { ctx.r3.u64 = to_ntstatus(status); return; }
    if (!guest_write_be32(output, duplicate))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s validated output became unwritable", fn);
    ctx.r3.u64 = nt::kSuccess;
}

// ObReferenceObjectByHandle(HANDLE, POBJECT_TYPE optional, PVOID* Body).
// GTA IV's actual ExThreadObjectType use is identity-only. Body references are
// independent from handle ownership and remain valid after NtClose(handle).
void ObReferenceObjectByHandle(PPCContext& ctx, uint8_t*) {
    const char* fn = "ObReferenceObjectByHandle";
    Runtime& r = rt_or_die(fn);
    const uint32_t output = ctx.r5.u32;
    if (!writable_word(r, output)) { ctx.r3.u64 = nt::kAccessViolation; return; }
    std::shared_ptr<HandleObject> retained;
    const uint32_t body_or_status = object_body_from_handle(
        r, ctx.r3.u32, ctx.r4.u32, fn, ctx, &retained);
    if (body_or_status == nt::kInvalidHandle || body_or_status == nt::kObjectTypeMismatch) {
        ctx.r3.u64 = body_or_status;
        return;
    }
    const uint32_t body = body_or_status;
    const Status status = reference_thread_object(body);
    if (status == Status::NotFound || status == Status::NotInitialized) {
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }
    if (status != Status::Ok)
        unimplemented(fn, ctx, "reference_state", (uint32_t)status);
    if (!guest_write_be32(output, body))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s validated output became unwritable", fn);
    ctx.r3.u64 = nt::kSuccess;
}

// ObReferenceObject(PVOID Body) is a VOID-style acquisition in the independent
// NT reference. The exact Xbox360 return register is not asserted: r3 is left
// unchanged. Only an already registered thread Body or a device object from
// IoCreateDevice (src/hle_xboxkrnl_devices.cpp) is accepted.
void ObReferenceObject(PPCContext& ctx, uint8_t*) {
    const uint32_t body = ctx.r3.u32;
    const Status device = reference_device_object(body);
    if (device == Status::Ok) return;
    if (device == Status::Conflict) unimplemented("ObReferenceObject", ctx, "reference_overflow", body);
    const Status status = reference_thread_object(body);
    if (status == Status::Ok) return;
    if (status == Status::Conflict)
        unimplemented("ObReferenceObject", ctx, "reference_overflow", body);
    rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,
                "ObReferenceObject unknown/stale body 0x%08X lr=0x%08X", body,
                (uint32_t)ctx.lr);
}

// ObDereferenceObject(PVOID Body), likewise treated as VOID. Unknown Body or
// underflow is a concrete guest misuse; it is never silently ignored.
void ObDereferenceObject(PPCContext& ctx, uint8_t*) {
    const uint32_t body = ctx.r3.u32;
    if (dereference_enum_body(body) == Status::Ok) return;
    const Status session = dereference_session_body(body);
    if (session == Status::Ok) return;
    if (session != Status::NotFound)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,
                    "ObDereferenceObject session refcount underflow 0x%08X status=%s lr=0x%08X",
                    body, status_name(session), (uint32_t)ctx.lr);
    const Status device = dereference_device_object(body);
    if (device == Status::Ok) return;
    if (device != Status::NotFound && device != Status::NotInitialized)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,
                    "ObDereferenceObject device refcount underflow 0x%08X status=%s lr=0x%08X",
                    body, status_name(device), (uint32_t)ctx.lr);
    const Status status = dereference_thread_object(body);
    if (status == Status::Ok) return;
    rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,
                "ObDereferenceObject invalid body/refcount 0x%08X status=%s lr=0x%08X",
                body, status_name(status), (uint32_t)ctx.lr);
}

// ObLookupAnyThreadByThreadId (0x010A): NTSTATUS (DWORD ThreadId, PKTHREAD*
// Thread). Signature: community xbdm reverse engineering (KernelExports.h,
// copeison/xbdm) and Xenia's ObLookupThreadByThreadId. On success the thread
// object gains one reference the caller releases with ObDereferenceObject
// (NT PsLookupThreadByThreadId contract). Every runtime thread (bootstrap,
// workers, interrupt/DPC contexts) has an identity while its object lives,
// including after exit until its last owner releases it, as NT finds a thread
// object until it is deleted. Unknown id -> STATUS_INVALID_PARAMETER (the NT
// PsLookupThreadByThreadId code; rexglue returns STATUS_NOT_FOUND, the console
// value is not independently established), output untouched.
void ObLookupAnyThreadByThreadId(PPCContext& ctx, uint8_t*) {
    const char* fn = "ObLookupAnyThreadByThreadId";
    Runtime& r = rt_or_die(fn);
    const uint32_t thread_id = ctx.r3.u32, output = ctx.r4.u32;
    if (!writable_word(r, output)) { ctx.r3.u64 = nt::kAccessViolation; return; }
    uint32_t body = 0;
    const Status status = reference_thread_object_by_thread_id(thread_id, &body);
    if (status == Status::NotFound || status == Status::InvalidArgument || status == Status::NotInitialized) {
        ctx.r3.u64 = nt::kInvalidParameter;
        return;
    }
    if (status != Status::Ok) unimplemented(fn, ctx, "reference_state", (uint32_t)status);
    if (!guest_write_be32(output, body))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s validated output became unwritable", fn);
    ctx.r3.u64 = nt::kSuccess;
}

// ObLookupThreadByThreadId (0x010B): the title-process form of ObLookupAnyThreadByThreadId (same signature and
// reference contract; the console excludes system-process threads). R-comp's guest-visible threads are all the
// title's: the only ids a title can learn are those of its own threads (creation, KeGetCurrentThread), the
// runtime's interrupt/DPC contexts never hand theirs out, so both lookups find the same objects. Needed by Halo 3's
// Waves DLLs (L360, Q10).
void ObLookupThreadByThreadId(PPCContext& ctx, uint8_t* base) { ObLookupAnyThreadByThreadId(ctx, base); }

// ObOpenObjectByPointer (0x010E): NTSTATUS (PVOID Object, PHANDLE Handle), the
// two-argument Xbox 360 form (xbdm KernelExports.h; Xenia). Creates a new
// handle to a referenced object; the caller's pointer reference is unchanged.
// Implemented for worker thread Bodies (the handle is an ordinary thread
// handle, as NtDuplicateObject creates). A thread Body without a handle
// object (bootstrap thread, interrupt/DPC contexts) and device objects have no
// handle class in R-comp and trap; an unknown pointer is a guest fault.
void ObOpenObjectByPointer(PPCContext& ctx, uint8_t*) {
    const char* fn = "ObOpenObjectByPointer";
    Runtime& r = rt_or_die(fn);
    const uint32_t body = ctx.r3.u32, output = ctx.r4.u32;
    if (!writable_word(r, output)) { ctx.r3.u64 = nt::kAccessViolation; return; }
    std::shared_ptr<ThreadObjectIdentity> identity;
    if (find_thread_object(body, &identity) != Status::Ok) {
        if (reference_device_object(body) == Status::Ok) {
            (void)dereference_device_object(body);
            unimplemented(fn, ctx, "device_object_handle", body);
        }
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s unknown/stale object 0x%08X lr=0x%08X", fn, body,
                    (uint32_t)ctx.lr);
    }
    uint32_t handle = 0;
    const Status status = open_thread_handle_for_body(body, &handle);
    if (status == Status::NotFound) unimplemented(fn, ctx, "thread_without_handle_object", body);
    if (status != Status::Ok) { ctx.r3.u64 = to_ntstatus(status); return; }
    if (!guest_write_be32(output, handle))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s validated output became unwritable", fn);
    ctx.r3.u64 = nt::kSuccess;
}

struct Impl { uint32_t ordinal; const char* name; PPCFunc* function; };
constexpr Impl kImpls[] = {
    {0x00DA, "NtDuplicateObject", &NtDuplicateObject},
    {0x010A, "ObLookupAnyThreadByThreadId", &ObLookupAnyThreadByThreadId},
    {0x010B, "ObLookupThreadByThreadId", &ObLookupThreadByThreadId},
    {0x010E, "ObOpenObjectByPointer", &ObOpenObjectByPointer},
    {0x0105, "ObDereferenceObject", &ObDereferenceObject},
    {0x010F, "ObReferenceObject", &ObReferenceObject},
    {0x0110, "ObReferenceObjectByHandle", &ObReferenceObjectByHandle},
};

}  // namespace

Status register_xboxkrnl_object_hle() {
    Status status = Status::Ok;
    for (const auto& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            return Status::InvalidArgument;
        status = register_import(kModuleXboxkrnl, impl.ordinal, impl.function, impl.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
