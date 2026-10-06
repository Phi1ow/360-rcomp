// XMsgInProcessCall (0x1F4), XMsgStartIORequest (0x1F7), XMsgCancelIORequest
// (0x1F8) and XMsgStartIORequestEx (0x1FC): a title sends a message to a XAM
// "app" (0xFA is the media player, 0xFC/0xFD the Live and system apps, ...).
//
// R-comp hosts no XAM app: there is no Live client and no dashboard. A message
// to an app that is not there fails with ERROR_NOT_FOUND (0x490), the code the
// XAM app manager returns for an unregistered app. Exceptions: the LIVE app
// (0xFC) is always there and fails every request as for a profile not signed in
// to LIVE, and the media player (0xFA) answers its playback-controller messages
// (below). The
// asynchronous forms complete their XOVERLAPPED at once with that result and
// return ERROR_IO_PENDING, or return the error directly when the caller gave no
// overlapped. ABI: public XDK/Xenia.
#include "rcomp/runtime/xam_msg.h"

#include <stdio.h>

#include <mutex>
#include <set>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam_enum.h"

namespace rcomp::rt {
namespace {
constexpr uint32_t kErrorNotFound = 0x490, kErrorIoPending = 0x3E5;

// The Xbox LIVE app (0xFC) is always present on a console. R-comp's profile is a local one that is
// never signed in to LIVE, so every LIVE request fails as it does for such a profile:
// HRESULT_FROM_WIN32(ERROR_NOT_LOGGED_ON). The failure must be a negative HRESULT: the XDK wrappers
// test the result with `blt`/`bge` and, on a non-negative value, read outputs never written (Gears
// of War 2: friends enumeration 0x58020 asked for 1 GiB; NAT type 0x58006, 0x58004 and 0x58046 at the
// campaign start returned stack garbage). On failure they take their offline paths (NAT open, ...).
constexpr uint32_t kAppXLive = 0xFC;
constexpr uint32_t kHresultNotLoggedOn = 0x800704DDu;

// The media player app (0xFA, XMP) is always present on a console. R-comp has no user music: nothing
// but the title ever plays music, so the title always holds playback. Two messages are served with that
// state (layouts as Xenia's xmp_app, confirmed by Halo 3's wrappers sub_826F3400 / sub_8208E310):
//  0x0007001A XMPSetPlaybackController {be32 client, be32 controller, be32 locked}: the title asks for
//             playback; with no other player there is nothing to take it from: success.
//  0x0007001B XMPGetPlaybackController {be32 client, be32 controller_ptr, be32 locked_ptr}: writes
//             controller 0 (the title) and locked 0, success. Halo 3 reads (0, 0) as "the title has
//             playback control"; an error made it warn that the user's music was playing.
// Other media player messages keep ERROR_NOT_FOUND (reported once), as before.
constexpr uint32_t kAppXmp = 0xFA;
constexpr uint32_t kXmpSetPlaybackController = 0x0007001A, kXmpGetPlaybackController = 0x0007001B;

bool readable(uint32_t address, uint32_t size) {
    const Runtime* r = runtime();
    return address && r && r->mem && r->mem->is_accessible(address, size, Protect::Read);
}
bool writable(uint32_t address) {
    const Runtime* r = runtime();
    return address && !(address & 3) && r && r->mem && r->mem->is_accessible(address, 4, Protect::ReadWrite);
}

// The result of an XMP message the runtime serves, or false when it is not one.
bool xmp_message(uint32_t app, uint32_t message, uint32_t buffer, uint32_t* result) {
    if (app != kAppXmp || (message != kXmpSetPlaybackController && message != kXmpGetPlaybackController)) return false;
    *result = 0x57;  // ERROR_INVALID_PARAMETER: unreadable arguments
    if (!readable(buffer, 12)) return true;
    if (message == kXmpSetPlaybackController) {
        *result = 0;
        return true;
    }
    uint32_t controller = 0, locked = 0;
    if (!guest_read_be32(buffer + 4, &controller) || !guest_read_be32(buffer + 8, &locked) || !writable(controller) ||
        !writable(locked))
        return true;
    guest_write_be32(controller, 0);
    guest_write_be32(locked, 0);
    *result = 0;
    return true;
}

// Each distinct (app, message) is reported once, so a title's use of these services is visible in its log.
void report_message(uint32_t app, uint32_t message, uint32_t lr, uint32_t result) {
    static std::mutex mutex;
    static std::set<uint64_t> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen.insert((uint64_t(app) << 32) | message).second) return;
    std::fprintf(stderr, "RCOMP-XMSG app=0x%X message=0x%08X lr=0x%08X: %s\n", app, message, lr,
                 result == kHresultNotLoggedOn ? "not signed in to LIVE (0x800704DD)" : "no such app (ERROR_NOT_FOUND)");
    std::fflush(stderr);
}

uint32_t message_result(uint32_t app) { return app == kAppXLive ? kHresultNotLoggedOn : kErrorNotFound; }

// XMsgInProcessCall(app, message, arg1, arg2)
void XMsgInProcessCall(PPCContext& ctx, uint8_t*) {
    const uint32_t app = ctx.r3.u32, message = ctx.r4.u32;
    uint32_t result = 0;
    if (xmp_message(app, message, ctx.r5.u32, &result)) {
        ctx.r3.u64 = result;
        return;
    }
    result = message_result(app);
    report_message(app, message, uint32_t(ctx.lr), result);
    ctx.r3.u64 = result;
}

// XMsgStartIORequest(app, message, overlapped, buffer, cbBuffer);
// XMsgStartIORequestEx takes one more trailing argument.
void start_io_request(PPCContext& ctx) {
    const uint32_t app = ctx.r3.u32, overlapped = ctx.r5.u32;
    uint32_t result = 0;
    if (!xmp_message(app, ctx.r4.u32, ctx.r6.u32, &result)) {
        result = message_result(app);
        report_message(app, ctx.r4.u32, uint32_t(ctx.lr), result);
    }
    if (!overlapped) {
        ctx.r3.u64 = result;
        return;
    }
    if (!xam_complete_overlapped(overlapped, result, 0)) {
        ctx.r3.u64 = 0x57;  // ERROR_INVALID_PARAMETER: unusable overlapped
        return;
    }
    ctx.r3.u64 = kErrorIoPending;
}
void XMsgStartIORequest(PPCContext& ctx, uint8_t*) { start_io_request(ctx); }
void XMsgStartIORequestEx(PPCContext& ctx, uint8_t*) { start_io_request(ctx); }

// XMsgCancelIORequest(overlapped, wait): every request completed when it was
// started, so there is never one to cancel.
void XMsgCancelIORequest(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kErrorNotFound; }

struct Entry {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Entry entries[] = {
    {0x01F4, "XMsgInProcessCall", &XMsgInProcessCall},
    {0x01F7, "XMsgStartIORequest", &XMsgStartIORequest},
    {0x01F8, "XMsgCancelIORequest", &XMsgCancelIORequest},
    {0x01FC, "XMsgStartIORequestEx", &XMsgStartIORequestEx},
};
}  // namespace

Status register_xam_msg_hle() {
    for (const auto& entry : entries) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXam, entry.name, &ordinal) || ordinal != entry.ordinal) return Status::Conflict;
        const Status status = register_import(kModuleXam, entry.ordinal, entry.function, entry.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}
}  // namespace rcomp::rt
