// xam.xex tasks, sessions, system UI, voice, QoS and XNetLogon queries (owner: Agent 3, runtime/).
//
// The title sees one local, offline profile (rcomp/runtime/xam_profile.h): user 0 is signed in locally,
// never to Xbox LIVE, and nothing can be shown by the console's system UI. Every answer below is the one
// such a console gives; nothing reports a success for work that was not done.
//
// ABI sources. Argument registers and structure layouts were read from the call sites of the recompiled
// Grand Theft Auto IV and Episodes from Liberty City XDK libraries (XenonRecomp output under build/) and
// cross-checked with Xenia 95a5c3e (src/xenia/kernel/xam/xam_task.cc, xam_ui.cc, xam_user.cc,
// xam_voice.cc, xam_net.cc; BSD-3, read only, no code copied) and Xenia Canary (same files).
//   XamTaskSchedule(callback, context, PXTASK_ATTRIBUTES, PHANDLE) -> HRESULT. The task thread runs
//       callback(context) (GTA IV reads r3 as its context block). Attribute block +0 flags, +4 a value
//       neither title interprets; both titles pass flags 0x02080002, the only value established here.
//   XamTaskShouldExit(void) -> BOOL. Both titles call it from the task thread with no argument.
//   XamTaskCloseHandle(HANDLE) -> BOOL (Xenia Canary: NtClose, TRUE on success). Closing the handle does
//       not stop the task: the titles close it right after scheduling and wait on their own event.
//   XamSessionCreateHandle(PHANDLE) / XamSessionRefObjByHandle(HANDLE, PVOID* Body) -> Win32 error. The
//       XDK XSessionCreate passes the Body to the XGI message and drops it with ObDereferenceObject.
//   XamShowMessageBoxUIEx(user, title, text, cButtons, pwszButtons, dwFocusButton, dwFlags, dwUnknown,
//       PMESSAGEBOX_RESULT [stack 8], PXOVERLAPPED [stack 9]). GTA IV passes user 0xFF, a NULL title,
//       one button, XMB_ERRORICON and dwUnknown = 1; MESSAGEBOX_RESULT +0 is dwButtonPressed.
//   XamShowGamerCardUIForXUID(user, XUID, dwUnknown = 0), XamShowPlayerReviewUI(user, XUID),
//   XamShowMarketplaceUI(user, entry point, QWORD offer id, offer type = 0xFFFFFFFF, categories, title
//       id = 0) and XamShowMarketplaceDownloadItemsUI(user, entry point, PULONGLONG offers, count,
//       dwUnknown = 0, HRESULT* phrResult, PXOVERLAPPED, dwUnknown = 0): EFLC's XShow* wrappers.
//   XamUserAreUsersFriends(user, PXUID, count, PBOOL, PXOVERLAPPED); XamUserGetMembershipTierFromXUID(
//       XUID) and XamUserGetOnlineCountryFromXUID(XUID) return the value itself.
//   XamVoiceCreate(user, 0xF, PVOID* voice) -> HRESULT; XamVoiceHeadsetPresent(voice) -> BOOL;
//   XamVoiceSubmitPacket(voice, 0, packet) -> HRESULT; XamVoiceClose(voice).
//   NetDll_XNetQosListen(caller, XNKID*, data, cb, bits/s, flags), NetDll_XNetQosLookup(caller, cxna,
//       apxna, apxnkid, apxnkey, cina, aina, adwServiceId, [stack] cProbes, bits/s, flags, hEvent,
//       XNQOS**), NetDll_XNetQosRelease(caller, XNQOS*): INT, 0 or a WinSock error.
//   XNetLogonGetMachineID(PULONGLONG) -> HRESULT; XNetLogonGetTitleID(service id) -> title id.
#include "rcomp/runtime/xam_misc.h"

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "rcomp/diag.h"
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/handle_table.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/io_event.h"
#include "rcomp/runtime/process_lifecycle.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/xam.h"
#include "rcomp/runtime/xam_profile.h"

namespace rcomp::rt {
namespace {

// Win32 errors.
constexpr uint32_t kErrorSuccess = 0, kErrorInvalidHandle = 6, kErrorOutOfMemory = 0xE,
                   kErrorInvalidParameter = 0x57, kErrorIoPending = 0x3E5, kErrorDeviceNotConnected = 0x48F,
                   kErrorNotLoggedOn = 0x4DD, kErrorNoSuchUser = 0x525;
// HRESULTs.
constexpr uint32_t kHresultInvalidHandle = 0x80070006u, kHresultOutOfMemory = 0x8007000Eu,
                   kHresultInvalidArgument = 0x80070057u;
// XONLINE_E_LOGON_NOT_LOGGED_ON: the XNetLogon service has no Xbox LIVE logon.
constexpr uint32_t kXOnlineLogonNotLoggedOn = 0x80151802u;
constexpr uint32_t kWsaEinval = 10022;

constexpr uint32_t kOverlappedBytes = 0x1C;
constexpr uint32_t kMaxUsers = 4;
constexpr uint32_t kUserIndexFocus = 0xFD, kUserIndexAny = 0xFF;  // 0xFD focus, 0xFE none, 0xFF any
constexpr uint32_t kMessageBoxMaxButtons = 3;                       // XMB_MAXBUTTONS
constexpr uint32_t kMessageBoxIconMask = 0x0000000Fu;               // XMB_NOICON .. XMB_ALERTICON
constexpr uint32_t kMessageBoxPasscodeModes = 0x00030000u;          // XMB_PASSCODEMODE | XMB_VERIFYPASSCODEMODE
constexpr uint32_t kTaskObservedFlags = 0x02080002u;
constexpr uint32_t kSessionBodyBytes = 16;
constexpr uint32_t kOrdinalExCreateThread = 0x000D, kOrdinalNtResumeThread = 0x00F5;
constexpr uint32_t kCreateSuspended = 1;
constexpr uint32_t kMaxGuestChars = 1024, kLogChars = 96;
constexpr uint64_t kMembershipTierNone = 0;  // no Xbox LIVE account behind the profile
constexpr uint32_t kOnlineCountryNone = 0;   // no Xbox LIVE account, hence no online country

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s before runtime_init", fn);
    return *r;
}

bool readable(Runtime& r, uint32_t address, uint64_t size) {
    return address && r.mem->is_accessible(address, size, Protect::Read);
}
bool writable(Runtime& r, uint32_t address, uint64_t size) {
    return address && r.mem->is_accessible(address, size, Protect::ReadWrite);
}

uint32_t hresult_from_win32(uint32_t error) { return error ? 0x80070000u | (error & 0xFFFFu) : 0; }

// 9th+ arguments: r1 + 0x54 + 8*(n-8) in the caller frame (runtime/docs/RUNTIME.md).
uint32_t stack_arg(Runtime& r, PPCContext& ctx, int n, const char* fn) {
    const uint64_t address = uint64_t(ctx.r1.u32) + 0x54 + 8u * uint32_t(n - 8);
    uint32_t value = 0;
    if (address > UINT32_MAX || !r.mem->is_accessible(address, 4, Protect::Read) ||
        !guest_read_be32(uint32_t(address), &value))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xam.xex!%s stack argument %d at 0x%08X not readable lr=0x%08X", fn,
                    n, uint32_t(address), uint32_t(ctx.lr));
    return value;
}

// ---- XOVERLAPPED ------------------------------------------------------------------------------------
// +0x00 InternalLow (result), +0x04 InternalHigh (length), +0x0C hEvent, +0x10 completion routine,
// +0x18 extended error. Validation and the event reference happen before any guest-visible write, so a
// rejected request leaves the title's buffers untouched.
uint32_t begin_overlapped(Runtime& r, uint32_t overlapped, const char* fn, std::shared_ptr<HandleObject>* event) {
    if (!writable(r, overlapped, kOverlappedBytes)) return kErrorInvalidParameter;
    uint32_t handle = 0, routine = 0;
    guest_read_be32(overlapped + 0x0C, &handle);
    guest_read_be32(overlapped + 0x10, &routine);
    if (routine)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s overlapped completion routine 0x%08X not supported", fn,
                    routine);
    if (handle && reference_io_event(handle, event) != Status::Ok) return kErrorInvalidHandle;
    return kErrorSuccess;
}

// Completes at once: the result is final when the call returns ERROR_IO_PENDING. InternalLow is written
// last so a title polling it never sees a half-written completion.
void complete_overlapped(uint32_t overlapped, const std::shared_ptr<HandleObject>& event, uint32_t result,
                         uint32_t length) {
    guest_write_be32(overlapped + 0x04, length);
    guest_write_be32(overlapped + 0x18, hresult_from_win32(result));
    guest_write_be32(overlapped + 0x00, result);
    if (event) set_io_event(event, true);
}

// ---- system UI log ----------------------------------------------------------------------------------
struct UiText {
    char text[kLogChars + 4] = {};
    uint32_t length = 0;
};

// Reads a NUL-terminated UTF-16BE guest string into a printable-ASCII, bounded log form. NULL reads as
// "". False when the string becomes unreadable before its terminator.
bool read_ui_string(Runtime& r, uint32_t address, UiText* out) {
    *out = UiText{};
    if (!address) return true;
    for (uint32_t i = 0; i < kMaxGuestChars; ++i) {
        const uint64_t at = uint64_t(address) + 2u * i;
        uint16_t c = 0;
        if (at > UINT32_MAX - 1 || !r.mem->is_accessible(at, 2, Protect::Read) || !guest_read_be16(uint32_t(at), &c))
            return false;
        if (!c) return true;
        if (out->length < kLogChars) {
            out->text[out->length++] = c == '"' ? '\'' : (c >= 0x20 && c < 0x7F ? char(c) : '?');
        } else if (out->length == kLogChars) {
            out->text[out->length++] = '.';
            out->text[out->length++] = '.';
            out->text[out->length++] = '.';
        }
    }
    return true;  // longer than the bound: the logged prefix stands for it
}

bool message_box_user(uint32_t user) { return user < kMaxUsers || (user >= kUserIndexFocus && user <= kUserIndexAny); }

// The answer of a system UI that needs an Xbox LIVE logon: an unknown user index, a slot nobody is
// signed in to, or the local profile (signed in, but never to LIVE).
uint32_t live_ui_result(uint32_t user) {
    if (user >= kMaxUsers) return kErrorInvalidParameter;
    if (!is_local_user(user)) return kErrorNoSuchUser;
    return kErrorNotLoggedOn;
}

// XamShowMessageBoxUIEx (0x02DC) and XamShowMessageBoxUI (0x02CA, the same box without dwUnknown:
// PMESSAGEBOX_RESULT in r10, PXOVERLAPPED in the first stack slot; Xenia 95a5c3e xam_ui.cc). Owner
// decision: no system UI exists on the PS5, so the box closes as if the user confirmed the button the
// title designated as the focus/default button.
void show_message_box(PPCContext& ctx, const char* fn, uint32_t unknown, uint32_t result, uint32_t overlapped) {
    Runtime& r = current(fn);
    const uint32_t user = ctx.r3.u32, title = ctx.r4.u32, text = ctx.r5.u32, count = ctx.r6.u32,
                   buttons = ctx.r7.u32, focus = ctx.r8.u32, flags = ctx.r9.u32;
    if (flags & kMessageBoxPasscodeModes)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xam.xex!%s passcode flags=0x%08X: no passcode can be entered without system UI lr=0x%08X", fn,
                    flags, uint32_t(ctx.lr));
    if (flags & ~kMessageBoxIconMask)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s flags=0x%08X not established lr=0x%08X", fn, flags,
                    uint32_t(ctx.lr));
    UiText title_text, body_text, button_text[kMessageBoxMaxButtons];
    if (!message_box_user(user) || count == 0 || count > kMessageBoxMaxButtons || focus >= count ||
        !readable(r, buttons, uint64_t(count) * 4) || !writable(r, result, 4) ||
        !read_ui_string(r, title, &title_text) || !read_ui_string(r, text, &body_text)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t pointer = 0;
        guest_read_be32(buttons + 4 * i, &pointer);
        if (!pointer || !read_ui_string(r, pointer, &button_text[i])) {
            ctx.r3.u64 = kErrorInvalidParameter;
            return;
        }
    }
    std::shared_ptr<HandleObject> event;
    if (overlapped) {
        const uint32_t error = begin_overlapped(r, overlapped, fn, &event);
        if (error) {
            ctx.r3.u64 = error;
            return;
        }
    }
    char list[3 * (kLogChars + 8)] = {};
    int used = 0;
    for (uint32_t i = 0; i < count && used >= 0 && size_t(used) < sizeof(list); ++i)
        used += snprintf(list + used, sizeof(list) - size_t(used), "%s\"%s\"", i ? "," : "", button_text[i].text);
    fprintf(stderr,
            "RCOMP-XAM-UI MessageBox user=0x%X title=\"%s\" text=\"%s\" buttons=[%s] default=%u flags=0x%X "
            "unknown=0x%X -> %u\n",
            user, title_text.text, body_text.text, list, focus, flags, unknown, focus);
    guest_write_be32(result, focus);  // MESSAGEBOX_RESULT.dwButtonPressed
    if (overlapped) {
        complete_overlapped(overlapped, event, kErrorSuccess, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = kErrorSuccess;
}

void XamShowMessageBoxUIEx(PPCContext& ctx, uint8_t*) {
    const char* fn = "XamShowMessageBoxUIEx";
    Runtime& r = current(fn);
    const uint32_t unknown = ctx.r10.u32, result = stack_arg(r, ctx, 8, fn), overlapped = stack_arg(r, ctx, 9, fn);
    show_message_box(ctx, fn, unknown, result, overlapped);
}

void XamShowMessageBoxUI(PPCContext& ctx, uint8_t*) {
    const char* fn = "XamShowMessageBoxUI";
    Runtime& r = current(fn);
    const uint32_t result = ctx.r10.u32, overlapped = stack_arg(r, ctx, 8, fn);
    show_message_box(ctx, fn, 0, result, overlapped);
}

// XamShowGamerCardUIForXUID (0x02D5). The local profile's own gamercard exists offline: a console shows it
// and the user closes it; with no system UI it closes at once (owner decision), ERROR_SUCCESS. Any other
// XUID's gamercard comes from Xbox LIVE: ERROR_NOT_LOGGED_ON.
void XamShowGamerCardUIForXUID(PPCContext& ctx, uint8_t*) {
    const uint32_t user = ctx.r3.u32, unknown = ctx.r5.u32;
    const uint64_t xuid = ctx.r4.u64;
    uint32_t result = live_ui_result(user);
    if (result == kErrorNotLoggedOn && xuid == kLocalUserXuid) result = kErrorSuccess;
    fprintf(stderr, "RCOMP-XAM-UI GamerCard user=0x%X xuid=0x%016llX unknown=0x%X -> 0x%X%s\n", user,
            (unsigned long long)xuid, unknown, result,
            result == kErrorSuccess ? " (own offline gamercard; closed at once, no system UI)" : "");
    ctx.r3.u64 = result;
}

// XamShowPlayerReviewUI (0x02C6): player feedback is sent to Xbox LIVE.
void XamShowPlayerReviewUI(PPCContext& ctx, uint8_t*) {
    const uint32_t user = ctx.r3.u32, result = live_ui_result(user);
    fprintf(stderr, "RCOMP-XAM-UI PlayerReview user=0x%X xuid=0x%016llX -> 0x%X\n", user,
            (unsigned long long)ctx.r4.u64, result);
    ctx.r3.u64 = result;
}

// XamShowMarketplaceUI (0x02C7): the marketplace is an Xbox LIVE service.
void XamShowMarketplaceUI(PPCContext& ctx, uint8_t*) {
    const uint32_t user = ctx.r3.u32, result = live_ui_result(user);
    fprintf(stderr,
            "RCOMP-XAM-UI Marketplace user=0x%X entry=0x%X offer=0x%016llX type=0x%X categories=0x%X title=0x%X "
            "-> 0x%X\n",
            user, ctx.r4.u32, (unsigned long long)ctx.r5.u64, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, result);
    ctx.r3.u64 = result;
}

// XamShowMarketplaceDownloadItemsUI (0x02E7): downloads come from the LIVE marketplace. The failure is
// reported in *phrResult (HRESULT_FROM_WIN32) and through the overlapped when the title passed one.
void XamShowMarketplaceDownloadItemsUI(PPCContext& ctx, uint8_t*) {
    const char* fn = "XamShowMarketplaceDownloadItemsUI";
    Runtime& r = current(fn);
    const uint32_t user = ctx.r3.u32, entry = ctx.r4.u32, offers = ctx.r5.u32, count = ctx.r6.u32,
                   hresult = ctx.r8.u32, overlapped = ctx.r9.u32;
    if (ctx.r7.u32 || ctx.r10.u32)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s reserved arguments r7=0x%08X r10=0x%08X lr=0x%08X", fn,
                    ctx.r7.u32, ctx.r10.u32, uint32_t(ctx.lr));
    if ((hresult && !writable(r, hresult, 4)) || (count && !readable(r, offers, uint64_t(count) * 8))) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    std::shared_ptr<HandleObject> event;
    if (overlapped) {
        const uint32_t error = begin_overlapped(r, overlapped, fn, &event);
        if (error) {
            ctx.r3.u64 = error;
            return;
        }
    }
    const uint32_t result = live_ui_result(user);
    fprintf(stderr, "RCOMP-XAM-UI MarketplaceDownloadItems user=0x%X entry=0x%X offers=%u -> 0x%X\n", user, entry,
            count, result);
    if (hresult) guest_write_be32(hresult, hresult_from_win32(result));
    if (overlapped) {
        complete_overlapped(overlapped, event, result, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = result;
}

// ---- users --------------------------------------------------------------------------------------------
// XamUserAreUsersFriends (0x0213): the friends list lives on Xbox LIVE. The local profile is signed in but
// not to LIVE (ERROR_NOT_LOGGED_ON), slots 1-3 have nobody signed in (ERROR_NO_SUCH_USER). *pfResult is
// FALSE whatever the result: GTA IV reads it without checking the return value.
void XamUserAreUsersFriends(PPCContext& ctx, uint8_t*) {
    const char* fn = "XamUserAreUsersFriends";
    Runtime& r = current(fn);
    const uint32_t user = ctx.r3.u32, xuids = ctx.r4.u32, count = ctx.r5.u32, are_friends = ctx.r6.u32,
                   overlapped = ctx.r7.u32;
    if ((!are_friends && !overlapped) || (are_friends && !writable(r, are_friends, 4)) || !count ||
        !readable(r, xuids, uint64_t(count) * 8)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    std::shared_ptr<HandleObject> event;
    if (overlapped) {
        const uint32_t error = begin_overlapped(r, overlapped, fn, &event);
        if (error) {
            ctx.r3.u64 = error;
            return;
        }
    }
    const uint32_t result = live_ui_result(user);
    if (are_friends) guest_write_be32(are_friends, 0);
    if (overlapped) {
        complete_overlapped(overlapped, event, result, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = result;
}

// XamUserGetMembershipTierFromXUID (0x0217): no profile here has an Xbox LIVE account.
void XamUserGetMembershipTierFromXUID(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kMembershipTierNone; }

// XamUserGetOnlineCountryFromXUID (0x0218): the online country belongs to the LIVE account, and there is
// none; no console country is configured to stand in for it (runtime/include/rcomp/runtime/xconfig.h).
void XamUserGetOnlineCountryFromXUID(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kOnlineCountryNone; }

// ---- voice: no headset is ever connected on this platform --------------------------------------------
// XamVoiceCreate (0x030C): no voice device exists, so no voice object is created.
void XamVoiceCreate(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamVoiceCreate");
    const uint32_t user = ctx.r3.u32, output = ctx.r5.u32;
    if (!writable(r, output, 4)) {
        ctx.r3.u64 = kHresultInvalidArgument;
        return;
    }
    guest_write_be32(output, 0);
    ctx.r3.u64 = user >= kMaxUsers ? kHresultInvalidArgument : hresult_from_win32(kErrorDeviceNotConnected);
}
// XamVoiceHeadsetPresent (0x030D): FALSE.
void XamVoiceHeadsetPresent(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }
// XamVoiceClose (0x030F) / XamVoiceSubmitPacket (0x030E): XamVoiceCreate never creates a voice object,
// so no argument can name one.
void XamVoiceClose(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kHresultInvalidHandle; }
void XamVoiceSubmitPacket(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kHresultInvalidHandle; }

// ---- XNet QoS and logon --------------------------------------------------------------------------------
// Every QoS target (an XNKID for a peer or listener, or an IN_ADDR from XNetServerToInAddr) must be
// registered in the XNet security table. Nothing can be: there is no LIVE logon (XNetServerToInAddr
// fails, see hle_xam_net.cpp) and no XNet key registration exists. The XNet answer is WSAEINVAL.
void XNetQosListen(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kWsaEinval; }

void XNetQosLookup(PPCContext& ctx, uint8_t*) {
    const char* fn = "NetDll_XNetQosLookup";
    Runtime& r = current(fn);
    const uint32_t output = stack_arg(r, ctx, 12, fn);  // XNQOS**
    if (writable(r, output, 4)) guest_write_be32(output, 0);
    ctx.r3.u64 = kWsaEinval;
}

// XNetQosRelease: only XNQOS blocks returned by XNetQosLookup may be released; none ever is.
void XNetQosRelease(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kWsaEinval; }

// XNetLogonGetMachineID (0x0135): the machine id is assigned by the LIVE logon.
void XNetLogonGetMachineID(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XNetLogonGetMachineID");
    const uint32_t output = ctx.r3.u32;
    if (!writable(r, output, 8)) {
        ctx.r3.u64 = kHresultInvalidArgument;
        return;
    }
    guest_write_be64(output, 0);
    ctx.r3.u64 = kXOnlineLogonNotLoggedOn;
}

// XNetLogonGetTitleID (0x0136): the title id the logon presents for a service, which is the running
// title's own id (no alternate title id is configured through XNetLogonSetTitleID). Both EFLC call sites
// pass service 9 and use the value only on paths that require a LIVE sign-in.
void XNetLogonGetTitleID(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = xam_main_title_id("XNetLogonGetTitleID"); }

// ---- tasks -------------------------------------------------------------------------------------------
// A task is a guest thread created through the real ExCreateThread (same lifecycle, quiesce and handle
// rules as any title thread) that runs callback(context). The task handle names a Task object; the
// thread identity is kept weakly so XamTaskShouldExit can recognise the task's own thread.
struct TaskState {
    std::weak_ptr<ThreadObjectIdentity> thread;
};

struct TaskObject final : HandleObject {
    static constexpr HandleKind kKind = HandleKind::Task;
    std::shared_ptr<TaskState> state;
    HandleKind kind() const override { return kKind; }
};

std::mutex g_task_mutex;
uint64_t g_task_generation = 0;
std::vector<std::shared_ptr<TaskState>> g_tasks;

// Caller holds g_task_mutex. Forgets tasks of an earlier Runtime and tasks whose thread has ended.
void prune_tasks_locked(Runtime& r) {
    if (g_task_generation != r.generation) {
        g_tasks.clear();
        g_task_generation = r.generation;
    }
    for (size_t i = 0; i < g_tasks.size();) {
        const auto identity = g_tasks[i]->thread.lock();
        if (identity && !thread_object_exited(identity)) {
            ++i;
            continue;
        }
        g_tasks[i] = std::move(g_tasks.back());
        g_tasks.pop_back();
    }
}

PPCFunc* kernel_import(uint32_t ordinal, const char* fn) {
    PPCFunc* f = find_import(kModuleXboxkrnl, ordinal);
    if (!f)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s needs xboxkrnl.exe ordinal 0x%04X registered", fn, ordinal);
    return f;
}

// XamTaskSchedule (0x01AF).
void XamTaskSchedule(PPCContext& ctx, uint8_t* base) {
    const char* fn = "XamTaskSchedule";
    Runtime& r = current(fn);
    const uint32_t callback = ctx.r3.u32, context = ctx.r4.u32, attributes = ctx.r5.u32, output = ctx.r6.u32;
    if (!writable(r, output, 4)) {
        ctx.r3.u64 = kHresultInvalidArgument;
        return;
    }
    if (!attributes)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s without task attributes lr=0x%08X", fn, uint32_t(ctx.lr));
    uint32_t flags = 0;
    if (!readable(r, attributes, 8) || !guest_read_be32(attributes, &flags)) {
        ctx.r3.u64 = kHresultInvalidArgument;
        return;
    }
    if (flags != kTaskObservedFlags)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s task flags=0x%08X not established lr=0x%08X", fn, flags,
                    uint32_t(ctx.lr));
    if (!lookup_function(callback))
        rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET, "xam.xex!%s callback 0x%08X has no recompiled function lr=0x%08X",
                    fn, callback, uint32_t(ctx.lr));
    PPCFunc* const create = kernel_import(kOrdinalExCreateThread, fn);
    PPCFunc* const resume = kernel_import(kOrdinalNtResumeThread, fn);

    std::shared_ptr<TaskState> state;
    std::shared_ptr<TaskObject> object;
#if defined(__cpp_exceptions)
    try {
#endif
        state = std::make_shared<TaskState>();
        object = std::make_shared<TaskObject>();
        {
            std::lock_guard<std::mutex> lock(g_task_mutex);
            prune_tasks_locked(r);
            g_tasks.reserve(g_tasks.size() + 1);
        }
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        ctx.r3.u64 = kHresultOutOfMemory;
        return;
    }
#endif
    object->state = state;
    uint32_t task = 0;
    if (r.handles.insert(object, &task) != Status::Ok) {
        ctx.r3.u64 = kHresultOutOfMemory;
        return;
    }

    // ExCreateThread(PHANDLE, StackSize = default, ThreadId = NULL, XapiThreadStartup = NULL,
    // StartAddress = callback, StartContext = context, CREATE_SUSPENDED): suspended until the task is
    // registered, so XamTaskShouldExit always finds it.
    alignas(64) PPCContext call = ctx;
    call.r3.u64 = output;
    call.r4.u64 = 0;
    call.r5.u64 = 0;
    call.r6.u64 = 0;
    call.r7.u64 = callback;
    call.r8.u64 = context;
    call.r9.u64 = kCreateSuspended;
    create(call, base);
    const uint32_t created = call.r3.u32;
    if (created & 0x80000000u) {
        if (r.handles.close(task) != Status::Ok)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s task handle rollback failed", fn);
        ctx.r3.u64 = created;
        return;
    }
    uint32_t thread = 0;
    std::shared_ptr<HandleObject> thread_object;
    std::shared_ptr<ThreadObjectIdentity> identity;
    if (!guest_read_be32(output, &thread) || r.handles.lookup(thread, HandleKind::Thread, &thread_object) != Status::Ok ||
        find_thread_object(thread_object->guest_object_body(), &identity) != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s created thread handle 0x%08X has no thread identity", fn, thread);
    state->thread = identity;
    bool registered = false;
    {
        std::lock_guard<std::mutex> lock(g_task_mutex);
        prune_tasks_locked(r);
#if defined(__cpp_exceptions)
        try {
#endif
            g_tasks.push_back(state);  // normally within the capacity reserved above
            registered = true;
#if defined(__cpp_exceptions)
        } catch (const std::bad_alloc&) {
        }
#endif
    }
    if (!registered) {
        // The thread never ran: it stays suspended and is cancelled when the runtime quiesces.
        if (r.handles.close(thread) != Status::Ok || r.handles.close(task) != Status::Ok)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s rollback after host allocation failure", fn);
        guest_write_be32(output, 0);
        ctx.r3.u64 = kHresultOutOfMemory;
        return;
    }
    alignas(64) PPCContext go = ctx;
    go.r3.u64 = thread;
    go.r4.u64 = 0;
    resume(go, base);
    if (go.r3.u32 != 0)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s NtResumeThread(0x%08X) = 0x%08X", fn, thread, go.r3.u32);
    // The task keeps running after the thread handle closes, as after NtClose of any thread handle.
    if (r.handles.close(thread) != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s cannot close task thread handle 0x%08X", fn, thread);
    guest_write_be32(output, task);
    ctx.r3.u64 = kErrorSuccess;
}

// XamTaskShouldExit (0x01B3): TRUE once the task is asked to stop. Nothing cancels a single task here
// (XamTaskCancel is not provided), so that happens when the title terminates or the runtime quiesces its
// threads; a task's waits then return at once and it must leave.
void XamTaskShouldExit(PPCContext& ctx, uint8_t*) {
    const char* fn = "XamTaskShouldExit";
    Runtime& r = current(fn);
    GuestThread* thread = current_guest_thread();
    if (!thread || !thread->identity)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s outside a guest thread", fn);
    bool task = false;
    {
        std::lock_guard<std::mutex> lock(g_task_mutex);
        prune_tasks_locked(r);
        for (const auto& state : g_tasks)
            if (state->thread.lock() == thread->identity) task = true;
    }
    if (!task)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s from thread %u, which is not a XamTaskSchedule task lr=0x%08X",
                    fn, thread->thread_id, uint32_t(ctx.lr));
    TitleLifecycleSnapshot lifecycle;
    const bool terminating =
        title_lifecycle_snapshot(&lifecycle) == Status::Ok && lifecycle.phase != TitleLifecyclePhase::Running;
    ctx.r3.u64 = terminating || runtime_thread_quiesce_requested() ? 1 : 0;
}

// XamTaskCloseHandle (0x01B1): TRUE when the task handle was closed, FALSE for anything else.
void XamTaskCloseHandle(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamTaskCloseHandle");
    std::shared_ptr<TaskObject> task;
    if (r.handles.lookup_as<TaskObject>(ctx.r3.u32, &task) != Status::Ok || r.handles.close(ctx.r3.u32) != Status::Ok) {
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = 1;
}

// ---- sessions ----------------------------------------------------------------------------------------
// A session handle names a Session object whose Body is a small zeroed guest block (no XSESSION field
// layout is published). The Body lives while a handle or a Body reference exists.
struct SessionObject final : HandleObject {
    static constexpr HandleKind kKind = HandleKind::Session;
    uint64_t generation = 0;
    uint32_t body = 0;
    uint32_t handles = 0;  // open handles (insert and NtDuplicateObject)
    uint32_t refs = 0;     // XamSessionRefObjByHandle references
    HandleKind kind() const override { return kKind; }
    void handle_opened() override;
    void handle_closed() override;
};

std::mutex g_session_mutex;
uint64_t g_session_generation = 0;
std::map<uint32_t, std::shared_ptr<SessionObject>> g_sessions;  // by Body

// Caller holds g_session_mutex and a reference to `s` (the map entry may be the last one).
void release_session_locked(SessionObject& s) {
    const uint32_t body = s.body;
    if (!body) return;
    s.body = 0;
    if (g_session_generation == s.generation) g_sessions.erase(body);
    Runtime* r = runtime();
    if (!r || r->generation != s.generation) return;  // the old Runtime's heap is gone
    const Status freed = r->heap.free(body);
    if (freed != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex! session body 0x%08X free failed: %s", body, status_name(freed));
}

void sync_sessions_locked(Runtime& r) {
    if (g_session_generation == r.generation) return;
    g_sessions.clear();
    g_session_generation = r.generation;
}

void SessionObject::handle_opened() {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    ++handles;
}

void SessionObject::handle_closed() {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    if (handles) --handles;
    if (!handles && !refs) release_session_locked(*this);
}

// XamSessionCreateHandle (0x0316).
void XamSessionCreateHandle(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamSessionCreateHandle");
    const uint32_t output = ctx.r3.u32;
    if (!writable(r, output, 4)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    std::shared_ptr<SessionObject> session;
#if defined(__cpp_exceptions)
    try {
#endif
        session = std::make_shared<SessionObject>();
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        ctx.r3.u64 = kErrorOutOfMemory;
        return;
    }
#endif
    uint32_t body = 0;
    const Status heap = r.heap.alloc(kSessionBodyBytes, 16, true, &body);
    if (heap == Status::OutOfMemory) {
        ctx.r3.u64 = kErrorOutOfMemory;
        return;
    }
    if (heap != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!XamSessionCreateHandle body allocation: %s", status_name(heap));
    session->generation = r.generation;
    session->body = body;
    bool published = false;
    {
        std::lock_guard<std::mutex> lock(g_session_mutex);
        sync_sessions_locked(r);
#if defined(__cpp_exceptions)
        try {
#endif
            g_sessions[body] = session;
            published = true;
#if defined(__cpp_exceptions)
        } catch (const std::bad_alloc&) {
            release_session_locked(*session);
        }
#endif
    }
    if (!published) {
        ctx.r3.u64 = kErrorOutOfMemory;
        return;
    }
    uint32_t handle = 0;
    if (r.handles.insert(session, &handle) != Status::Ok) {
        std::lock_guard<std::mutex> lock(g_session_mutex);
        release_session_locked(*session);
        ctx.r3.u64 = kErrorOutOfMemory;
        return;
    }
    guest_write_be32(output, handle);
    ctx.r3.u64 = kErrorSuccess;
}

// XamSessionRefObjByHandle (0x0317): a referenced Body, dropped by ObDereferenceObject; it stays valid
// after the handle is closed.
void XamSessionRefObjByHandle(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamSessionRefObjByHandle");
    const uint32_t output = ctx.r4.u32;
    if (!writable(r, output, 4)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    std::shared_ptr<SessionObject> session;
    if (r.handles.lookup_as<SessionObject>(ctx.r3.u32, &session) != Status::Ok) {
        ctx.r3.u64 = kErrorInvalidHandle;
        return;
    }
    uint32_t body = 0;
    {
        std::lock_guard<std::mutex> lock(g_session_mutex);
        if (!session->body || session->generation != r.generation) {
            ctx.r3.u64 = kErrorInvalidHandle;
            return;
        }
        if (session->refs == UINT32_MAX)
            rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!XamSessionRefObjByHandle reference count overflow");
        ++session->refs;
        body = session->body;
    }
    guest_write_be32(output, body);
    ctx.r3.u64 = kErrorSuccess;
}

struct Entry {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Entry kEntries[] = {
    {0x0045, "NetDll_XNetQosListen", &XNetQosListen},
    {0x0046, "NetDll_XNetQosLookup", &XNetQosLookup},
    {0x0048, "NetDll_XNetQosRelease", &XNetQosRelease},
    {0x0135, "XNetLogonGetMachineID", &XNetLogonGetMachineID},
    {0x0136, "XNetLogonGetTitleID", &XNetLogonGetTitleID},
    {0x01AF, "XamTaskSchedule", &XamTaskSchedule},
    {0x01B1, "XamTaskCloseHandle", &XamTaskCloseHandle},
    {0x01B3, "XamTaskShouldExit", &XamTaskShouldExit},
    {0x0213, "XamUserAreUsersFriends", &XamUserAreUsersFriends},
    {0x0217, "XamUserGetMembershipTierFromXUID", &XamUserGetMembershipTierFromXUID},
    {0x0218, "XamUserGetOnlineCountryFromXUID", &XamUserGetOnlineCountryFromXUID},
    {0x02C6, "XamShowPlayerReviewUI", &XamShowPlayerReviewUI},
    {0x02C7, "XamShowMarketplaceUI", &XamShowMarketplaceUI},
    {0x02CA, "XamShowMessageBoxUI", &XamShowMessageBoxUI},
    {0x02D5, "XamShowGamerCardUIForXUID", &XamShowGamerCardUIForXUID},
    {0x02DC, "XamShowMessageBoxUIEx", &XamShowMessageBoxUIEx},
    {0x02E7, "XamShowMarketplaceDownloadItemsUI", &XamShowMarketplaceDownloadItemsUI},
    {0x030C, "XamVoiceCreate", &XamVoiceCreate},
    {0x030D, "XamVoiceHeadsetPresent", &XamVoiceHeadsetPresent},
    {0x030E, "XamVoiceSubmitPacket", &XamVoiceSubmitPacket},
    {0x030F, "XamVoiceClose", &XamVoiceClose},
    {0x0316, "XamSessionCreateHandle", &XamSessionCreateHandle},
    {0x0317, "XamSessionRefObjByHandle", &XamSessionRefObjByHandle},
};

}  // namespace

Status dereference_session_body(uint32_t body) {
    Runtime* r = runtime();
    if (!r || !body) return Status::NotFound;
    std::lock_guard<std::mutex> lock(g_session_mutex);
    sync_sessions_locked(*r);
    const auto it = g_sessions.find(body);
    if (it == g_sessions.end()) return Status::NotFound;
    const std::shared_ptr<SessionObject> session = it->second;  // outlives the map entry
    if (session->refs == 0) return Status::Conflict;
    if (--session->refs == 0 && session->handles == 0) release_session_locked(*session);
    return Status::Ok;
}

Status register_xam_misc_hle() {
    for (const auto& entry : kEntries) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXam, entry.name, &ordinal) || ordinal != entry.ordinal) return Status::Conflict;
        const Status status = register_import(kModuleXam, entry.ordinal, entry.function, entry.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
