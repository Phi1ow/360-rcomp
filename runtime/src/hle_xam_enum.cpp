// Enumerator handles: XamCreateEnumeratorHandle (0x24E), the private
// structure accessor (0x24F), XamEnumerate (0x250), XamContentCreateEnumerator
// (0x25C) and the signed-out profile enumerators.
//
// Content enumerators list the packages that really exist on the hard drive
// (rcomp/runtime/xam_content.h); the other enumerators enumerate an empty
// domain: R-comp keeps no achievement records and has no Live storage, so
// XamEnumerate reports ERROR_NO_MORE_FILES with zero items, the answer a
// console gives for a domain with nothing in it. ABI: public XDK/Xenia; the private-structure layout comes
// from the title use (a body of the requested size, first 24 bytes owned by
// XAM, the rest by the title).
#include "rcomp/runtime/xam_enum.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/io_event.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam_content.h"
#include "rcomp/runtime/xam_profile.h"

namespace rcomp::rt {
namespace {
constexpr uint32_t kErrorNoMoreFiles = 0x12, kErrorInvalidHandle = 6, kErrorIoPending = 0x3E5,
                   kErrorInvalidParameter = 0x57, kErrorOutOfMemory = 0xE, kErrorInsufficientBuffer = 0x7A,
                   kErrorNoSuchUser = 0x525, kErrorNotLoggedOn = 0x4DD;
constexpr uint32_t kHeaderBytes = 24, kMaxBodyBytes = 4096;
// X_ACHIEVEMENT_DETAILS, and with its strings when the title asks for them (flags & 7).
constexpr uint32_t kAchievementBytes = 36, kAchievementWithStringsBytes = 500;
constexpr uint32_t kOverlappedBytes = 0x1C;

struct EnumObject final : HandleObject {
    static constexpr HandleKind kKind = HandleKind::Enumerator;
    uint32_t body = 0, item_size = 0, max_items = 0;
    uint32_t refs = 0;  // private-structure references
    bool closed = false;
    // Items not yet returned by XamEnumerate (each item_size bytes, guest byte order).
    std::vector<std::vector<uint8_t>> items;
    size_t next = 0;
    HandleKind kind() const override { return kKind; }
    void handle_closed() override;
};

std::mutex g_mutex;
std::map<uint32_t, std::shared_ptr<EnumObject>> g_bodies;

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s before runtime_init", fn);
    return *r;
}

// Caller holds g_mutex.
void release_locked(EnumObject& e) {
    if (!e.body) return;
    const uint32_t body = e.body;
    e.body = 0;
    Runtime* r = runtime();
    if (r) r->heap.free(body);
    // The map may hold the last reference to `e`: nothing touches `e` after this erase.
    g_bodies.erase(body);
}

void EnumObject::handle_closed() {
    std::lock_guard<std::mutex> lock(g_mutex);
    closed = true;
    if (refs == 0) release_locked(*this);
}

bool writable(Runtime& r, uint32_t address, uint32_t size) {
    return address && r.mem->is_accessible(address, size, Protect::ReadWrite);
}

// Allocates a zeroed body of `body_bytes` and the handle to it.
uint32_t create_enumerator(Runtime& r, uint32_t body_bytes, uint32_t item_size, uint32_t max_items,
                           uint32_t* handle) {
    std::shared_ptr<EnumObject> e;
#if defined(__cpp_exceptions)
    try {
#endif
        e = std::make_shared<EnumObject>();
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) { return kErrorOutOfMemory; }
#endif
    uint32_t address = 0;
    const Status heap = r.heap.alloc(body_bytes, 16, false, &address);
    if (heap == Status::OutOfMemory) return kErrorOutOfMemory;
    if (heap != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex! enumerator body allocation: %s", status_name(heap));
    if (uint8_t* p = r.mem->translate(address, body_bytes))
        for (uint32_t i = 0; i < body_bytes; ++i) p[i] = 0;
    e->body = address;
    e->item_size = item_size;
    e->max_items = max_items;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_bodies[address] = e;
    }
    const Status inserted = r.handles.insert(e, handle);
    if (inserted != Status::Ok) {
        std::lock_guard<std::mutex> lock(g_mutex);
        e->closed = true;
        release_locked(*e);
        return kErrorOutOfMemory;
    }
    return 0;
}

// XamCreateEnumeratorHandle(user, body_bytes, key_a, key_b, item_size, max_items, flags, PHANDLE).
void XamCreateEnumeratorHandle(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamCreateEnumeratorHandle");
    const uint32_t body_bytes = ctx.r4.u32, item_size = ctx.r7.u32, max_items = ctx.r8.u32, output = ctx.r10.u32;
    if (!writable(r, output, 4) || body_bytes < kHeaderBytes || body_bytes > kMaxBodyBytes || !item_size ||
        !max_items) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    uint32_t handle = 0;
    const uint32_t result = create_enumerator(r, body_bytes, item_size, max_items, &handle);
    if (result == 0 && !guest_write_be32(output, handle))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!XamCreateEnumeratorHandle validated output became unwritable");
    ctx.r3.u64 = result;
}

// XamGetPrivateEnumStructureFromHandle(HANDLE, PVOID* Body): takes a reference dropped by ObDereferenceObject.
void XamGetPrivateEnumStructureFromHandle(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamGetPrivateEnumStructureFromHandle");
    const uint32_t output = ctx.r4.u32;
    if (!writable(r, output, 4)) { ctx.r3.u64 = kErrorInvalidParameter; return; }
    std::shared_ptr<EnumObject> e;
    if (r.handles.lookup_as<EnumObject>(ctx.r3.u32, &e) != Status::Ok) { ctx.r3.u64 = kErrorInvalidHandle; return; }
    uint32_t body;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!e->body) { ctx.r3.u64 = kErrorInvalidHandle; return; }
        ++e->refs;
        body = e->body;
    }
    if (!guest_write_be32(output, body))
        rcomp_fatal(RCOMP_FATAL_INTERNAL,
                    "xam.xex!XamGetPrivateEnumStructureFromHandle validated output became unwritable");
    ctx.r3.u64 = 0;
}

// XamEnumerate(HANDLE, Flags, Buffer, cbBuffer, PDWORD ItemsReturned, PXOVERLAPPED).
// Returns up to the enumerator's items-per-call that fit the buffer, then
// ERROR_NO_MORE_FILES once every item was returned. With an overlapped the
// request completes at once (InternalLow = result, InternalHigh = items,
// extended error at +0x18, hEvent signalled) and ERROR_IO_PENDING is returned,
// as for an operation that has already finished.
void XamEnumerate(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamEnumerate");
    const uint32_t buffer = ctx.r5.u32, buffer_size = ctx.r6.u32, items_out = ctx.r7.u32, overlapped = ctx.r8.u32;
    std::shared_ptr<EnumObject> e;
    if (r.handles.lookup_as<EnumObject>(ctx.r3.u32, &e) != Status::Ok) { ctx.r3.u64 = kErrorInvalidHandle; return; }
    if (overlapped && !writable(r, overlapped, kOverlappedBytes)) { ctx.r3.u64 = kErrorInvalidParameter; return; }
    uint32_t result = kErrorNoMoreFiles, count = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const size_t remaining = e->items.size() - e->next;
        if (remaining) {
            const uint32_t fit = e->item_size ? buffer_size / e->item_size : 0;
            count = uint32_t(std::min<size_t>({remaining, size_t(fit), size_t(e->max_items)}));
            if (!count || !writable(r, buffer, count * e->item_size)) {
                result = count ? kErrorInvalidParameter : kErrorInsufficientBuffer;
                count = 0;
            } else {
                for (uint32_t i = 0; i < count; ++i) {
                    const std::vector<uint8_t>& item = e->items[e->next + i];
                    uint8_t* dst = r.mem->translate(buffer + i * e->item_size, e->item_size);
                    std::memset(dst, 0, e->item_size);
                    std::memcpy(dst, item.data(), std::min<size_t>(item.size(), e->item_size));
                }
                e->next += count;
                result = 0;
            }
        }
    }
    if (overlapped) {
        xam_complete_overlapped(overlapped, result, count);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    if (items_out && writable(r, items_out, 4)) guest_write_be32(items_out, count);
    ctx.r3.u64 = result;
}

// XamContentCreateEnumerator(user, device, type, flags, items_per_enumerate, PDWORD cbBuffer, PHANDLE).
// The enumerator snapshots the packages present now (one XCONTENT_DATA each).
void XamContentCreateEnumerator(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamContentCreateEnumerator");
    const uint32_t user = ctx.r3.u32, device = ctx.r4.u32, type = ctx.r5.u32, per = ctx.r7.u32,
                   size_out = ctx.r8.u32, handle_out = ctx.r9.u32;
    if (!per || !writable(r, size_out, 4) || !writable(r, handle_out, 4)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    std::vector<std::vector<uint8_t>> items;
    if (list_content(user, device, type, &items) != Status::Ok) { ctx.r3.u64 = kErrorInvalidParameter; return; }
    uint32_t handle = 0;
    const uint32_t result = create_enumerator(r, kHeaderBytes, kXContentDataBytes, per, &handle);
    if (result == 0) {
        std::shared_ptr<EnumObject> e;
        if (r.handles.lookup_as<EnumObject>(handle, &e) != Status::Ok)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!XamContentCreateEnumerator lost its new handle");
        std::lock_guard<std::mutex> lock(g_mutex);
        e->items = std::move(items);
    }
    if (result == 0 &&
        (!guest_write_be32(size_out, per * kXContentDataBytes) || !guest_write_be32(handle_out, handle)))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!XamContentCreateEnumerator validated output became unwritable");
    ctx.r3.u64 = result;
}

}  // namespace

bool xam_complete_overlapped(uint32_t overlapped, uint32_t result, uint32_t length) {
    Runtime* r = runtime();
    if (!r || !r->mem->is_accessible(overlapped, kOverlappedBytes, Protect::ReadWrite)) return false;
    uint32_t event = 0, routine = 0;
    guest_read_be32(overlapped + 0x0C, &event);
    guest_read_be32(overlapped + 0x10, &routine);
    if (routine)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex! overlapped completion routine 0x%08X not supported", routine);
    guest_write_be32(overlapped + 0x00, result);
    guest_write_be32(overlapped + 0x04, length);
    guest_write_be32(overlapped + 0x18, result ? 0x80070000u | (result & 0xFFFFu) : 0u);  // HRESULT_FROM_WIN32
    if (event) {
        std::shared_ptr<HandleObject> ev;
        if (reference_io_event(event, &ev) == Status::Ok) set_io_event(ev, true);
    }
    return true;
}

namespace {
// XamUserCreateAchievementEnumerator(title_id, user_index, xuid, flags, offset, count, PDWORD cbBuffer,
// PHANDLE): R-comp keeps no achievement records, so the local player's enumerator is valid and empty;
// the empty slots 1-3 have no user.
void XamUserCreateAchievementEnumerator(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamUserCreateAchievementEnumerator");
    const uint32_t user = ctx.r4.u32, flags = ctx.r6.u32, count = ctx.r8.u32, size_out = ctx.r9.u32,
                   handle_out = ctx.r10.u32;
    if (!is_local_user(user) && user != 0xFF) {
        ctx.r3.u64 = user < 4 ? kErrorNoSuchUser : kErrorInvalidParameter;
        return;
    }
    if (!count || !writable(r, size_out, 4) || !writable(r, handle_out, 4)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    const uint32_t item = (flags & 7) ? kAchievementWithStringsBytes : kAchievementBytes;
    uint32_t handle = 0;
    const uint32_t result = create_enumerator(r, kHeaderBytes, item, count, &handle);
    if (result == 0 && (!guest_write_be32(size_out, item * count) || !guest_write_be32(handle_out, handle)))
        rcomp_fatal(RCOMP_FATAL_INTERNAL,
                    "xam.xex!XamUserCreateAchievementEnumerator validated output became unwritable");
    ctx.r3.u64 = result;
}
// Statistics enumerate Xbox LIVE leaderboards: the local profile is not signed in to LIVE.
void XamUserCreateStatsEnumerator(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kErrorNotLoggedOn; }

struct Entry {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Entry entries[] = {
    {0x024E, "XamCreateEnumeratorHandle", &XamCreateEnumeratorHandle},
    {0x024F, "XamGetPrivateEnumStructureFromHandle", &XamGetPrivateEnumStructureFromHandle},
    {0x0250, "XamEnumerate", &XamEnumerate},
    {0x025C, "XamContentCreateEnumerator", &XamContentCreateEnumerator},
    {0x02EE, "XamUserCreateAchievementEnumerator", &XamUserCreateAchievementEnumerator},
    {0x02F7, "XamUserCreateStatsEnumerator", &XamUserCreateStatsEnumerator},
};
}  // namespace

Status dereference_enum_body(uint32_t body) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_bodies.find(body);
    if (it == g_bodies.end()) return Status::NotFound;
    EnumObject& e = *it->second;
    if (e.refs == 0) return Status::Conflict;
    if (--e.refs == 0 && e.closed) release_locked(e);
    return Status::Ok;
}

void reset_xam_enum() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bodies.clear();
}

Status register_xam_enum_hle() {
    for (const auto& entry : entries) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXam, entry.name, &ordinal) || ordinal != entry.ordinal) return Status::Conflict;
        const Status status = register_import(kModuleXam, entry.ordinal, entry.function, entry.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}
}  // namespace rcomp::rt
