// xam.xex: overlapped results, custom gamercard actions, notification delay, content licences, user
// device context, voice process state and the custom message composer (owner: Agent 3, runtime/).
// Contract: runtime/docs/XAM.md, "Second title wave".
//
// The title sees one local, offline profile (rcomp/runtime/xam_profile.h): user 0 is signed in locally,
// never to Xbox LIVE, and the console's system UI (Guide, gamercards, notifications) does not exist.
//
// ABI sources. Halo 3 (4D5307E6, recompiled with the pinned XenonRecomp):
//   XCustomSetDynamicActions(user, XUID (r4), const XCUSTOMACTION*, WORD count) -> DWORD   0x8239E9BC;
//       XCUSTOMACTION is 52 bytes: +0 WORD id, +2 WCHAR text[23], +0x30 DWORD flags (the title fills
//       an array with that stride and copies at most 23 characters).
//   XCustomGetLastActionPressEx(PDWORD user, PDWORD action, XUID*, BYTE* payload, WORD* cbPayload)
//       -> DWORD, 0 when an action was pressed                                                 0x8239E400
//   XCustomGetCurrentGamercard(PDWORD user, XUID*) -> BOOL                                     0x8239E304
//   XCustomRegisterDynamicActions(void), XCustomUnregisterDynamicActions(void)        0x8246E7B4/0x8246EA54
//   XNotifyDelayUI(DWORD milliseconds) (0 and 0x7FFFFFFF)                                       0x82197D50
//   XamShowCustomMessageComposeUI(user, ... 15 arguments)                                       0x8259FF54
// Xenia 95a5c3e / rexglue-sdk c94f5eb (BSD-3, read only, no code copied): XamGetOverlappedResult(
// XOVERLAPPED*, PDWORD, BOOL wait), XMsgCompleteIORequest(XOVERLAPPED*, result, extended error, length),
// XamContentGetLicenseMask(PDWORD, XOVERLAPPED*), XamUserGetDeviceContext(user, DWORD, PDWORD),
// XamVoiceIsActiveProcess(void).
#include <stdio.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "hle_more.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/input.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/handle_table.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/io_event.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam_enum.h"
#include "rcomp/runtime/xam_profile.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kErrorSuccess = 0, kErrorInvalidHandle = 6, kErrorInvalidParameter = 0x57,
                   kErrorIoIncomplete = 0x3E4, kErrorIoPending = 0x3E5, kErrorNotFound = 0x490,
                   kErrorNotLoggedOn = 0x4DD, kErrorNoSuchUser = 0x525;
constexpr uint32_t kHresultInvalidArgument = 0x80070057u, kHresultDeviceNotConnected = 0x8007048Fu;
constexpr uint32_t kStatusInvalidHandle = 0xC0000008u;
constexpr uint32_t kOverlappedBytes = 0x1C;
constexpr uint32_t kMaxUsers = 4, kUserIndexAny = 0xFF;
constexpr uint32_t kOrdinalNtWaitForSingleObjectEx = 0x00FD;
constexpr uint32_t kCustomActionBytes = 52;

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r || !r->mem) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s before runtime_init", fn);
    return *r;
}
bool readable(Runtime& r, uint32_t address, uint64_t size) {
    return address && r.mem->is_accessible(address, size, Protect::Read);
}
bool writable(Runtime& r, uint32_t address, uint64_t size) {
    return address && r.mem->is_accessible(address, size, Protect::ReadWrite);
}

// ---- XOVERLAPPED ------------------------------------------------------------------------------------
// +0x00 InternalLow (result), +0x04 InternalHigh (length), +0x0C hEvent, +0x10 completion routine,
// +0x18 extended error.

// XMsgCompleteIORequest (0x01F5): completes a request a title (or a XAM app) is serving: length,
// extended error, then the result (written last, so a poller never sees a half-written completion),
// then the event. ERROR_INVALID_PARAMETER for an unwritable overlapped, ERROR_INVALID_HANDLE for an
// hEvent that is not an event (nothing written).
void XMsgCompleteIORequest(PPCContext& ctx, uint8_t*) {
    const char* fn = "XMsgCompleteIORequest";
    Runtime& r = current(fn);
    const uint32_t overlapped = ctx.r3.u32, result = ctx.r4.u32, extended = ctx.r5.u32, length = ctx.r6.u32;
    if (!writable(r, overlapped, kOverlappedBytes)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    uint32_t handle = 0, routine = 0;
    guest_read_be32(overlapped + 0x0C, &handle);
    guest_read_be32(overlapped + 0x10, &routine);
    if (routine)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s overlapped completion routine 0x%08X not supported lr=0x%08X",
                    fn, routine, uint32_t(ctx.lr));
    std::shared_ptr<HandleObject> event;
    if (handle && reference_io_event(handle, &event) != Status::Ok) {
        ctx.r3.u64 = kErrorInvalidHandle;
        return;
    }
    guest_write_be32(overlapped + 0x04, length);
    guest_write_be32(overlapped + 0x18, extended);
    guest_write_be32(overlapped + 0x00, result);
    if (event) set_io_event(event, true);
    ctx.r3.u64 = kErrorSuccess;
}

// XamGetOverlappedResult (0x01FB): (XOVERLAPPED*, PDWORD pdwResult, BOOL bWait). A completed request
// returns its result and stores InternalHigh in *pdwResult; a pending one returns ERROR_IO_INCOMPLETE,
// or with bWait waits for hEvent (NtWaitForSingleObjectEx, so the guest thread blocks like any kernel
// wait) and then reports the completion.
void XamGetOverlappedResult(PPCContext& ctx, uint8_t* base) {
    const char* fn = "XamGetOverlappedResult";
    Runtime& r = current(fn);
    const uint32_t overlapped = ctx.r3.u32, result_out = ctx.r4.u32;
    const bool wait = ctx.r5.u32 != 0;
    if (!readable(r, overlapped, kOverlappedBytes) || (result_out && !writable(r, result_out, 4))) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    uint32_t result = 0;
    guest_read_be32(overlapped, &result);
    if (result == kErrorIoPending) {
        if (!wait) {
            ctx.r3.u64 = kErrorIoIncomplete;
            return;
        }
        uint32_t handle = 0;
        guest_read_be32(overlapped + 0x0C, &handle);
        if (!handle)
            rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                        "xam.xex!%s bWait on an overlapped without hEvent (no wait contract) lr=0x%08X", fn,
                        uint32_t(ctx.lr));
        PPCFunc* const nt_wait = find_import(kModuleXboxkrnl, kOrdinalNtWaitForSingleObjectEx);
        if (!nt_wait)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s needs xboxkrnl.exe!NtWaitForSingleObjectEx registered", fn);
        alignas(64) PPCContext call = ctx;
        call.r3.u64 = handle;
        call.r4.u64 = 1;  // UserMode
        call.r5.u64 = 0;  // not alertable
        call.r6.u64 = 0;  // no timeout
        nt_wait(call, base);
        const uint32_t status = call.r3.u32;
        if (status == kStatusInvalidHandle) {
            ctx.r3.u64 = kErrorInvalidHandle;
            return;
        }
        if (status != 0)
            rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s wait on hEvent 0x%08X returned 0x%08X lr=0x%08X", fn,
                        handle, status, uint32_t(ctx.lr));
        guest_read_be32(overlapped, &result);
        if (result == kErrorIoPending) {  // the event was signalled by something else
            ctx.r3.u64 = kErrorIoIncomplete;
            return;
        }
    }
    if (result_out) {
        uint32_t length = 0;
        guest_read_be32(overlapped + 0x04, &length);
        guest_write_be32(result_out, length);
    }
    ctx.r3.u64 = result;
}

// ---- users, voice, licences, UI -------------------------------------------------------------------
// XamUserGetDeviceContext (0x0208): (user, DWORD, PDWORD context) -> HRESULT. The device context of a
// user's controller; R-comp reports the controller port of the user (0..3; XUSER_INDEX_ANY is port 0,
// the value the reference gives for user 0) when that controller is connected. Otherwise *context = 0
// and HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED); E_INVALIDARG for a user index above 3.
void XamUserGetDeviceContext(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamUserGetDeviceContext");
    uint32_t user = ctx.r3.u32;
    const uint32_t out = ctx.r5.u32;
    if (!writable(r, out, 4)) {
        ctx.r3.u64 = kHresultInvalidArgument;
        return;
    }
    guest_write_be32(out, 0);
    if (user == kUserIndexAny) user = 0;
    if (user >= kMaxUsers) {
        ctx.r3.u64 = kHresultInvalidArgument;
        return;
    }
    rcomp_pad pad{};
    if (rcomp_input_read(user, &pad) != RCOMP_INPUT_OK) {
        ctx.r3.u64 = kHresultDeviceNotConnected;
        return;
    }
    guest_write_be32(out, user);
    ctx.r3.u64 = 0;
}

// XamVoiceIsActiveProcess (0x0497): no voice device exists, so voice is never active (FALSE).
void XamVoiceIsActiveProcess(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }

// XamContentGetLicenseMask (0x0266): (PDWORD mask, XOVERLAPPED*). Licences are granted by the Xbox LIVE
// marketplace; this console has none, so the mask is 0 (no licence bit) and the query succeeds,
// directly or through the overlapped (ERROR_IO_PENDING).
void XamContentGetLicenseMask(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamContentGetLicenseMask");
    const uint32_t mask = ctx.r3.u32, overlapped = ctx.r4.u32;
    if (!writable(r, mask, 4) || (overlapped && !writable(r, overlapped, kOverlappedBytes))) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    guest_write_be32(mask, 0);
    if (overlapped) {
        if (!xam_complete_overlapped(overlapped, kErrorSuccess, 0)) {
            ctx.r3.u64 = kErrorInvalidParameter;
            return;
        }
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = kErrorSuccess;
}

// XamShowCustomMessageComposeUI (0x02E5): composing a message to other gamers sends it through Xbox
// LIVE. The answer of the LIVE-only system UIs (src/hle_xam_misc.cpp): ERROR_NOT_LOGGED_ON for the
// local profile, ERROR_NO_SUCH_USER for an empty slot, ERROR_INVALID_PARAMETER for an index above 3.
void XamShowCustomMessageComposeUI(PPCContext& ctx, uint8_t*) {
    const uint32_t user = ctx.r3.u32;
    const uint32_t result = user >= kMaxUsers ? kErrorInvalidParameter : !is_local_user(user) ? kErrorNoSuchUser
                                                                                              : kErrorNotLoggedOn;
    fprintf(stderr, "RCOMP-XAM-UI CustomMessageCompose user=0x%X -> 0x%X\n", user, result);
    ctx.r3.u64 = result;
}

// ---- notification delay and custom gamercard actions ------------------------------------------------
// Per Runtime generation. No notification popup and no Guide exist here, so nothing is ever displayed
// or pressed; the state is what the title configured.
struct CustomAction {
    uint8_t bytes[kCustomActionBytes];
};
struct XamMoreState {
    std::mutex mutex;
    uint64_t generation = 0;
    uint32_t notify_delay_ms = 0;
    bool actions_registered = false;
    uint32_t actions_user = 0;
    uint64_t actions_xuid = 0;
    std::vector<CustomAction> actions;
};
XamMoreState g_state;

void sync_locked(const Runtime& r) {
    if (g_state.generation == r.generation) return;
    g_state.generation = r.generation;
    g_state.notify_delay_ms = 0;
    g_state.actions_registered = false;
    g_state.actions_user = 0;
    g_state.actions_xuid = 0;
    g_state.actions.clear();
}

// XNotifyDelayUI (0x028D): (DWORD milliseconds) -> ERROR_SUCCESS. Records how long notification popups
// are held back (0x7FFFFFFF holds them indefinitely); none is ever shown here.
void XNotifyDelayUI(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XNotifyDelayUI");
    std::lock_guard<std::mutex> lock(g_state.mutex);
    sync_locked(r);
    g_state.notify_delay_ms = ctx.r3.u32;
    ctx.r3.u64 = kErrorSuccess;
}

// XCustomRegisterDynamicActions (0x01DD) / XCustomUnregisterDynamicActions (0x01DE): the title takes part
// in (or leaves) dynamic gamercard actions; leaving forgets its action set.
void XCustomRegisterDynamicActions(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XCustomRegisterDynamicActions");
    std::lock_guard<std::mutex> lock(g_state.mutex);
    sync_locked(r);
    g_state.actions_registered = true;
}
void XCustomUnregisterDynamicActions(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XCustomUnregisterDynamicActions");
    std::lock_guard<std::mutex> lock(g_state.mutex);
    sync_locked(r);
    g_state.actions_registered = false;
    g_state.actions_user = 0;
    g_state.actions_xuid = 0;
    g_state.actions.clear();
}

// XCustomSetDynamicActions (0x01DA): replaces the action set shown on the gamercard of `xuid` for `user`.
// ERROR_SUCCESS; ERROR_INVALID_PARAMETER for a user index above 3 or an unreadable action array.
void XCustomSetDynamicActions(PPCContext& ctx, uint8_t*) {
    const char* fn = "XCustomSetDynamicActions";
    Runtime& r = current(fn);
    const uint32_t user = ctx.r3.u32, actions = ctx.r5.u32, count = ctx.r6.u32 & 0xFFFF;
    const uint64_t xuid = ctx.r4.u64;
    if (user >= kMaxUsers || (count && !readable(r, actions, uint64_t(count) * kCustomActionBytes))) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    std::vector<CustomAction> copy(count);
    for (uint32_t i = 0; i < count; ++i)
        std::memcpy(copy[i].bytes, r.mem->host(actions + i * kCustomActionBytes), kCustomActionBytes);
    bool registered = false;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        sync_locked(r);
        registered = g_state.actions_registered;
        if (registered) {
            g_state.actions_user = user;
            g_state.actions_xuid = xuid;
            g_state.actions.swap(copy);
        }
    }
    if (!registered)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xam.xex!%s before XCustomRegisterDynamicActions (no contract established) lr=0x%08X", fn,
                    uint32_t(ctx.lr));
    ctx.r3.u64 = kErrorSuccess;
}

// XCustomGetLastActionPressEx (0x01DC): an action is pressed on a gamercard the Guide shows; with no
// Guide none ever is. The answer is a non-zero code (Halo 3 treats any non-zero result as "no press");
// R-comp uses ERROR_NOT_FOUND, the exact console code is not established. Outputs untouched.
void XCustomGetLastActionPressEx(PPCContext& ctx, uint8_t*) {
    current("XCustomGetLastActionPressEx");
    ctx.r3.u64 = kErrorNotFound;
}

// XCustomGetCurrentGamercard (0x01DF): FALSE, no gamercard is being displayed. Outputs untouched.
void XCustomGetCurrentGamercard(PPCContext& ctx, uint8_t*) {
    current("XCustomGetCurrentGamercard");
    ctx.r3.u64 = 0;
}

struct Entry {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Entry kEntries[] = {
    {0x01DA, "XCustomSetDynamicActions", &XCustomSetDynamicActions},
    {0x01DC, "XCustomGetLastActionPressEx", &XCustomGetLastActionPressEx},
    {0x01DD, "XCustomRegisterDynamicActions", &XCustomRegisterDynamicActions},
    {0x01DE, "XCustomUnregisterDynamicActions", &XCustomUnregisterDynamicActions},
    {0x01DF, "XCustomGetCurrentGamercard", &XCustomGetCurrentGamercard},
    {0x01F5, "XMsgCompleteIORequest", &XMsgCompleteIORequest},
    {0x01FB, "XamGetOverlappedResult", &XamGetOverlappedResult},
    {0x0208, "XamUserGetDeviceContext", &XamUserGetDeviceContext},
    {0x0266, "XamContentGetLicenseMask", &XamContentGetLicenseMask},
    {0x028D, "XNotifyDelayUI", &XNotifyDelayUI},
    {0x02E5, "XamShowCustomMessageComposeUI", &XamShowCustomMessageComposeUI},
    {0x0497, "XamVoiceIsActiveProcess", &XamVoiceIsActiveProcess},
};

}  // namespace

Status register_xam_more_hle() {
    for (const auto& entry : kEntries) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXam, entry.name, &ordinal) || ordinal != entry.ordinal) return Status::Conflict;
        const Status status = register_import(kModuleXam, entry.ordinal, entry.function, entry.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
