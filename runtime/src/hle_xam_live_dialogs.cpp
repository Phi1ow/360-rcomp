// xam.xex Xbox LIVE dialogs: friends list and message composition (owner: Agent 3, runtime/).
//
// Both dialogs show Xbox LIVE data (the friends list, LIVE messages). The title's only profile is
// signed in locally, never to LIVE (rcomp/runtime/xam_profile.h), so they answer exactly as the LIVE
// dialogs of src/hle_xam_misc.cpp do (XamShowPlayerReviewUI, XamShowMarketplaceUI): an unknown user
// index is ERROR_INVALID_PARAMETER, a slot nobody is signed in to ERROR_NO_SUCH_USER and the local
// profile ERROR_NOT_LOGGED_ON. Nothing is displayed and nothing reports a success.
//
// ABI: Gears of War 2 (4D53082D) reaches the exports through the XDK wrappers XShowFriendsUI(dwUserIndex)
// and XShowMessageComposeUI(dwUserIndex, const XUID* pXuidRecipients, DWORD cRecipients, LPCWSTR
// wszText), which branch to the import with the arguments unchanged (ppc_recomp.172.cpp). Its call
// sites check the user index below 4 first; the message composer is given one recipient and a text
// (ppc_recomp.126.cpp), and both test only for ERROR_SUCCESS.
#include "rcomp/runtime/xam_live_dialogs.h"

#include <stdio.h>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam_profile.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kErrorInvalidParameter = 0x57, kErrorNotLoggedOn = 0x4DD, kErrorNoSuchUser = 0x525;
constexpr uint32_t kMaxUsers = 4;
constexpr uint32_t kMaxRecipients = 100;  // bounds the XUID array read; larger counts are refused
constexpr uint32_t kMaxTextChars = 1024;

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s before runtime_init", fn);
    return *r;
}

// Same rule as hle_xam_misc.cpp live_ui_result().
uint32_t live_ui_result(uint32_t user) {
    if (user >= kMaxUsers) return kErrorInvalidParameter;
    if (!is_local_user(user)) return kErrorNoSuchUser;
    return kErrorNotLoggedOn;
}

// Length of a NUL-terminated UTF-16BE string, or false when it is unreadable before its terminator
// within the bound. NULL is an empty text.
bool text_length(Runtime& r, uint32_t address, uint32_t* length) {
    *length = 0;
    if (!address) return true;
    for (uint32_t i = 0; i < kMaxTextChars; ++i) {
        const uint64_t at = uint64_t(address) + 2u * i;
        uint16_t c = 0;
        if (at > UINT32_MAX - 1 || !r.mem->is_accessible(at, 2, Protect::Read) || !guest_read_be16(uint32_t(at), &c))
            return false;
        if (!c) return true;
        ++*length;
    }
    return true;  // longer than the bound: only its prefix was checked
}

// XamShowFriendsUI (0x02BF): the friends list lives on Xbox LIVE.
void XamShowFriendsUI(PPCContext& ctx, uint8_t*) {
    const uint32_t user = ctx.r3.u32, result = live_ui_result(user);
    fprintf(stderr, "RCOMP-XAM-UI Friends user=0x%X -> 0x%X\n", user, result);
    ctx.r3.u64 = result;
}

// XamShowMessageComposeUI (0x02CC): LIVE messages are sent through Xbox LIVE.
void XamShowMessageComposeUI(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamShowMessageComposeUI");
    const uint32_t user = ctx.r3.u32, recipients = ctx.r4.u32, count = ctx.r5.u32, text = ctx.r6.u32;
    uint32_t length = 0;
    if (count > kMaxRecipients || (count && (!recipients || !r.mem->is_accessible(recipients, uint64_t(count) * 8,
                                                                                  Protect::Read))) ||
        !text_length(r, text, &length)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    const uint32_t result = live_ui_result(user);
    fprintf(stderr, "RCOMP-XAM-UI MessageCompose user=0x%X recipients=%u text_chars=%u -> 0x%X\n", user, count,
            length, result);
    ctx.r3.u64 = result;
}

struct Entry {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Entry kEntries[] = {
    {0x02BF, "XamShowFriendsUI", &XamShowFriendsUI},
    {0x02CC, "XamShowMessageComposeUI", &XamShowMessageComposeUI},
};

}  // namespace

Status register_xam_live_dialogs_hle() {
    for (const auto& entry : kEntries) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXam, entry.name, &ordinal) || ordinal != entry.ordinal) return Status::Conflict;
        const Status status = register_import(kModuleXam, entry.ordinal, entry.function, entry.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
