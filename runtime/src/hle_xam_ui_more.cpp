// More xam.xex system dialogs (owner: Agent 3, runtime/): the Guide screens Gears of War 2 (Unreal Engine 3,
// title 4D53082D) opens through its XDK XShow* wrappers.
//
// The PS5 shows no Xbox system UI. Owner decision (as in hle_xam_misc.cpp): a local dialog completes with the
// default the title designated and is logged as RCOMP-XAM-UI; a dialog that needs Xbox LIVE answers as a
// console whose only profile (rcomp/runtime/xam_profile.h) is signed in locally, never to LIVE:
// ERROR_NOT_LOGGED_ON for that profile, ERROR_NO_SUCH_USER for the empty slots 1-3 and
// ERROR_INVALID_PARAMETER for an index past them.
//
// ABI sources: Xenia master and Xenia Canary src/xenia/kernel/xam/xam_ui.cc (BSD-3, read only, no code
// copied) for XamShowKeyboardUI and XamShowAchievementsUI, and the XDK wrappers and their callers in the
// recompiled Gears of War 2 image (XenonRecomp output, gow2-inventory/ppc):
//   XamShowMessagesUI(user)                         sub_82AAB720, from sub_8282A238 (user < 4 checked).
//   XamShowKeyboardUI(user, flags, default text, title, description, buffer, buffer length in WCHARs,
//       PXOVERLAPPED)                               sub_82AAB728, from sub_8282A340: the buffer holds
//       length WCHARs; default, title and description are an empty string when the title has none; the
//       overlapped is always passed; 0 and ERROR_IO_PENDING are the accepted returns.
//   XamShowAchievementsUI(user, title id)           sub_82AAB738 sets the second argument to 0 (Xenia
//       Canary: 0 = the running title), from sub_8282A290.
//   XamShowPlayersUI(user)                          sub_82AAB718, from sub_8282A2E8.
//   XamShowGameInviteUI(user, const XUID* recipients, count, LPCWSTR text)
//                                                   sub_82AAB768, from sub_8282DDC8 (recipients NULL,
//                                                   count 0, text NULL or a string).
//   XamShowFriendRequestUI(user, XUID)              sub_82AAB770, from sub_8282A130 (XUID loaded as a
//                                                   64-bit value).
//   XamShowCustomPlayerListUI(user, flags, title, description, image, image bytes, players, player count,
//       [stack 8] X button, [9] Y button, [10] result, [11] PXOVERLAPPED)
//                                                   sub_82AAC490 forwards the four stack slots, from
//                                                   sub_82832DB8 (buttons and result NULL, overlapped set).
// All but the keyboard and the custom player list are synchronous: GoW2 treats a zero return as shown.
#include "rcomp/runtime/xam_misc.h"

#include <stdio.h>
#include <string.h>

#include <vector>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam_enum.h"
#include "rcomp/runtime/xam_profile.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kErrorSuccess = 0, kErrorInvalidParameter = 0x57, kErrorIoPending = 0x3E5,
                   kErrorNotLoggedOn = 0x4DD, kErrorNoSuchUser = 0x525;
constexpr uint32_t kOverlappedBytes = 0x1C;
constexpr uint32_t kMaxUsers = 4;
constexpr uint32_t kUserIndexFocus = 0xFD, kUserIndexAny = 0xFF;  // 0xFD focus, 0xFE none, 0xFF any
constexpr uint32_t kMaxGuestChars = 1024, kLogChars = 96;

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

// 9th+ arguments (index n from 0): r1 + 0x54 + 8*(n-8) in the caller frame (runtime/docs/RUNTIME.md).
uint32_t stack_arg(Runtime& r, PPCContext& ctx, int n, const char* fn) {
    const uint64_t address = uint64_t(ctx.r1.u32) + 0x54 + 8u * uint32_t(n - 8);
    uint32_t value = 0;
    if (address > UINT32_MAX || !r.mem->is_accessible(address, 4, Protect::Read) ||
        !guest_read_be32(uint32_t(address), &value))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xam.xex!%s stack argument %d at 0x%08X not readable lr=0x%08X", fn,
                    n, uint32_t(address), uint32_t(ctx.lr));
    return value;
}

// Reads a NUL-terminated UTF-16BE guest string, at most `max` characters (a longer string is cut there).
// NULL reads as "". False when the string becomes unreadable before its terminator or the bound.
bool read_wide(Runtime& r, uint32_t address, uint32_t max, std::vector<uint16_t>* out) {
    out->clear();
    if (!address) return true;
    for (uint32_t i = 0; i < max; ++i) {
        const uint64_t at = uint64_t(address) + 2u * i;
        uint16_t c = 0;
        if (at > UINT32_MAX - 1 || !r.mem->is_accessible(at, 2, Protect::Read) || !guest_read_be16(uint32_t(at), &c))
            return false;
        if (!c) return true;
        out->push_back(c);
    }
    return true;
}

// Printable-ASCII, bounded log form of a guest string ("" for NULL). False when it is unreadable.
struct UiText {
    char text[kLogChars + 4] = {};
};
bool read_ui_string(Runtime& r, uint32_t address, UiText* out) {
    *out = UiText{};
    std::vector<uint16_t> wide;
    if (!read_wide(r, address, kMaxGuestChars, &wide)) return false;
    size_t n = 0;
    for (uint16_t c : wide) {
        if (n == kLogChars) {
            memcpy(out->text + n, "...", 3);
            break;
        }
        out->text[n++] = c == '"' ? '\'' : (c >= 0x20 && c < 0x7F ? char(c) : '?');
    }
    return true;
}

bool ui_user(uint32_t user) { return user < kMaxUsers || (user >= kUserIndexFocus && user <= kUserIndexAny); }

// The answer of a system UI that needs an Xbox LIVE logon (same rule as hle_xam_misc.cpp).
uint32_t live_ui_result(uint32_t user) {
    if (user >= kMaxUsers) return kErrorInvalidParameter;
    if (!is_local_user(user)) return kErrorNoSuchUser;
    return kErrorNotLoggedOn;
}

// ---- LIVE-only Guide screens (synchronous) ---------------------------------------------------------
// XamShowMessagesUI (0x02C0): the message inbox is an Xbox LIVE service.
void XamShowMessagesUI(PPCContext& ctx, uint8_t*) {
    const uint32_t user = ctx.r3.u32, result = live_ui_result(user);
    fprintf(stderr, "RCOMP-XAM-UI Messages user=0x%X -> 0x%X\n", user, result);
    ctx.r3.u64 = result;
}

// XamShowPlayersUI (0x02C8): the players list (recent players, their gamercards and feedback) is kept by
// Xbox LIVE.
void XamShowPlayersUI(PPCContext& ctx, uint8_t*) {
    const uint32_t user = ctx.r3.u32, result = live_ui_result(user);
    fprintf(stderr, "RCOMP-XAM-UI Players user=0x%X -> 0x%X\n", user, result);
    ctx.r3.u64 = result;
}

// XamShowFriendRequestUI (0x02CE): friend requests are sent through Xbox LIVE.
void XamShowFriendRequestUI(PPCContext& ctx, uint8_t*) {
    const uint32_t user = ctx.r3.u32, result = live_ui_result(user);
    fprintf(stderr, "RCOMP-XAM-UI FriendRequest user=0x%X xuid=0x%016llX -> 0x%X\n", user,
            (unsigned long long)ctx.r4.u64, result);
    ctx.r3.u64 = result;
}

// XamShowGameInviteUI (0x02CD): game invitations are Xbox LIVE messages. Unreadable recipients or text are
// a caller error.
void XamShowGameInviteUI(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamShowGameInviteUI");
    const uint32_t user = ctx.r3.u32, recipients = ctx.r4.u32, count = ctx.r5.u32, text = ctx.r6.u32;
    UiText text_log;
    if ((count && !readable(r, recipients, uint64_t(count) * 8)) || !read_ui_string(r, text, &text_log)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    const uint32_t result = live_ui_result(user);
    fprintf(stderr, "RCOMP-XAM-UI GameInvite user=0x%X recipients=%u text=\"%s\" -> 0x%X\n", user, count,
            text_log.text, result);
    ctx.r3.u64 = result;
}

// XamShowCustomPlayerListUI (0x02E6): a list of other players (XUIDs the title passes) whose actions
// (gamercard, invite, feedback) are Xbox LIVE services. The failure completes the overlapped when the title
// passed one (GoW2 does) and the call returns ERROR_IO_PENDING; the result block is left untouched, as for
// any failed dialog.
void XamShowCustomPlayerListUI(PPCContext& ctx, uint8_t*) {
    const char* fn = "XamShowCustomPlayerListUI";
    Runtime& r = current(fn);
    const uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, title = ctx.r5.u32, description = ctx.r6.u32,
                   image = ctx.r7.u32, image_bytes = ctx.r8.u32, players = ctx.r9.u32, count = ctx.r10.u32;
    const uint32_t overlapped = stack_arg(r, ctx, 11, fn);
    UiText title_log, description_log;
    if ((image_bytes && !readable(r, image, image_bytes)) || (count && !players) ||
        (overlapped && !writable(r, overlapped, kOverlappedBytes)) || !read_ui_string(r, title, &title_log) ||
        !read_ui_string(r, description, &description_log)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    const uint32_t result = live_ui_result(user);
    fprintf(stderr, "RCOMP-XAM-UI CustomPlayerList user=0x%X flags=0x%X title=\"%s\" description=\"%s\" players=%u -> 0x%X\n",
            user, flags, title_log.text, description_log.text, count, result);
    if (overlapped) {
        xam_complete_overlapped(overlapped, result, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = result;
}

// ---- local dialogs ---------------------------------------------------------------------------------
// XamShowAchievementsUI (0x02C5): the profile's achievement list is local. R-comp keeps no achievement
// records (the achievement enumerators are valid and empty, hle_xam_enum.cpp), so the screen would list no
// earned achievement; with no system UI it closes at once, as viewed: ERROR_SUCCESS for the local profile.
void XamShowAchievementsUI(PPCContext& ctx, uint8_t*) {
    const uint32_t user = ctx.r3.u32, title_id = ctx.r4.u32;
    const uint32_t result = user >= kMaxUsers ? kErrorInvalidParameter
                                              : (is_local_user(user) ? kErrorSuccess : kErrorNoSuchUser);
    fprintf(stderr, "RCOMP-XAM-UI Achievements user=0x%X title=0x%08X -> 0x%X%s\n", user, title_id, result,
            result == kErrorSuccess ? " (no achievement records; closed at once as viewed, no system UI)" : "");
    ctx.r3.u64 = result;
}

// XamShowKeyboardUI (0x02C1). Owner decision: the virtual keyboard closes as if the user accepted the text
// the title proposed: the default text (empty for NULL), cut to buffer length - 1 WCHARs and NUL-terminated,
// is the entered text. The overlapped completes with ERROR_SUCCESS (InternalHigh 0, extended error 0, as
// Xenia's accepted keyboard) and the call returns ERROR_IO_PENDING; without one the call returns
// ERROR_SUCCESS. The keyboard flags only choose the layout of characters a user could type, so they do not
// change the answer and are logged. Every argument is validated before the buffer is written.
void XamShowKeyboardUI(PPCContext& ctx, uint8_t*) {
    const char* fn = "XamShowKeyboardUI";
    Runtime& r = current(fn);
    const uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, default_text = ctx.r5.u32, title = ctx.r6.u32,
                   description = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, overlapped = ctx.r10.u32;
    UiText title_log, description_log, default_log;
    std::vector<uint16_t> text;
    if (!ui_user(user) || !length || !writable(r, buffer, uint64_t(length) * 2) ||
        (overlapped && !writable(r, overlapped, kOverlappedBytes)) || !read_ui_string(r, title, &title_log) ||
        !read_ui_string(r, description, &description_log) || !read_ui_string(r, default_text, &default_log) ||
        !read_wide(r, default_text, length - 1, &text)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    uint8_t* out = r.mem->translate(buffer, uint32_t((text.size() + 1) * 2));
    if (!out) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s validated buffer 0x%08X became unmapped", fn, buffer);
    for (size_t i = 0; i < text.size(); ++i) {
        out[2 * i] = uint8_t(text[i] >> 8);
        out[2 * i + 1] = uint8_t(text[i]);
    }
    out[2 * text.size()] = 0;
    out[2 * text.size() + 1] = 0;
    fprintf(stderr,
            "RCOMP-XAM-UI Keyboard user=0x%X flags=0x%X title=\"%s\" description=\"%s\" default=\"%s\" -> default "
            "text accepted (%zu of %u WCHARs)\n",
            user, flags, title_log.text, description_log.text, default_log.text, text.size(), length);
    if (overlapped) {
        xam_complete_overlapped(overlapped, kErrorSuccess, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = kErrorSuccess;
}

struct Entry {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Entry kEntries[] = {
    {0x02C0, "XamShowMessagesUI", &XamShowMessagesUI},
    {0x02C1, "XamShowKeyboardUI", &XamShowKeyboardUI},
    {0x02C5, "XamShowAchievementsUI", &XamShowAchievementsUI},
    {0x02C8, "XamShowPlayersUI", &XamShowPlayersUI},
    {0x02CD, "XamShowGameInviteUI", &XamShowGameInviteUI},
    {0x02CE, "XamShowFriendRequestUI", &XamShowFriendRequestUI},
    {0x02E6, "XamShowCustomPlayerListUI", &XamShowCustomPlayerListUI},
};

}  // namespace

Status register_xam_ui_more_hle() {
    for (const auto& entry : kEntries) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXam, entry.name, &ordinal) || ordinal != entry.ordinal) return Status::Conflict;
        const Status status = register_import(kModuleXam, entry.ordinal, entry.function, entry.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
