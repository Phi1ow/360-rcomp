// Xbox LIVE dialogs (src/hle_xam_live_dialogs.cpp): XamShowFriendsUI and XamShowMessageComposeUI answer
// for the local offline profile. Nothing is displayed and no call reports ERROR_SUCCESS: an unknown user
// index is ERROR_INVALID_PARAMETER, an empty slot ERROR_NO_SUCH_USER, the local profile (slot 0)
// ERROR_NOT_LOGGED_ON; malformed recipients/text are ERROR_INVALID_PARAMETER.
#include <string.h>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam_live_dialogs.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__XamShowFriendsUI);
PPC_EXTERN_FUNC(__imp__XamShowMessageComposeUI);

using namespace rcomp;
using namespace rcomp::rt;

namespace {

constexpr uint32_t S = 0x30000000;     // committed read/write
constexpr uint32_t kBad = 0x30010000;  // committed, then made inaccessible
constexpr uint32_t kXuids = S + 0x100, kText = S + 0x400;
constexpr uint32_t kErrorSuccess = 0, kErrorInvalidParameter = 0x57, kErrorNotLoggedOn = 0x4DD,
                   kErrorNoSuchUser = 0x525;

GuestMemory g_mem;

bool guest_write_be16(uint32_t address, uint16_t value) {
    if (!g_mem.is_accessible(address, 2, Protect::ReadWrite)) return false;
    const uint16_t be = __builtin_bswap16(value);
    memcpy(g_mem.base() + address, &be, sizeof(be));
    return true;
}

uint32_t call(PPCFunc* fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0) {
    alignas(64) PPCContext ctx{};
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    fn(ctx, g_mem.base());
    return ctx.r3.u32;
}

void utf16be(uint32_t address, const char* text) {
    for (size_t i = 0;; ++i) {
        CHECK(guest_write_be16(address + 2 * static_cast<uint32_t>(i), static_cast<uint16_t>(text[i])));
        if (!text[i]) break;
    }
}

}  // namespace

int main() {
    if (g_mem.reserve() != MemStatus::Ok) return 2;
    CHECK(g_mem.commit(S, 0x20000, Protect::ReadWrite) == MemStatus::Ok);
    CHECK(g_mem.protect(kBad, 0x10000, Protect::None) == MemStatus::Ok);
    CHECK_ST(runtime_init(&g_mem), Status::Ok);
    clear_imports();
    CHECK_ST(register_xam_live_dialogs_hle(), Status::Ok);
    CHECK_ST(register_xam_live_dialogs_hle(), Status::Ok);  // idempotent

    // Registered at their xam_table.inc ordinals.
    struct Expected {
        uint32_t ordinal;
        const char* name;
        PPCFunc* thunk;
    };
    const Expected expected[] = {
        {0x02BF, "XamShowFriendsUI", __imp__XamShowFriendsUI},
        {0x02CC, "XamShowMessageComposeUI", __imp__XamShowMessageComposeUI},
    };
    for (const Expected& e : expected) {
        uint32_t ordinal = 0;
        CHECK(export_ordinal(kModuleXam, e.name, &ordinal));
        CHECK_EQ(ordinal, e.ordinal);
        CHECK(find_import(kModuleXam, e.ordinal) != nullptr);
        CHECK(import_thunk(kModuleXam, e.ordinal) == e.thunk);
        const char* registered = import_registry_name(kModuleXam, e.ordinal);
        CHECK(registered && strcmp(registered, e.name) == 0);
    }

    // XamShowFriendsUI(dwUserIndex).
    CHECK_EQ(call(__imp__XamShowFriendsUI, 0), kErrorNotLoggedOn);
    for (uint32_t user = 1; user < 4; ++user) CHECK_EQ(call(__imp__XamShowFriendsUI, user), kErrorNoSuchUser);
    CHECK_EQ(call(__imp__XamShowFriendsUI, 4), kErrorInvalidParameter);
    CHECK_EQ(call(__imp__XamShowFriendsUI, 0xFF), kErrorInvalidParameter);
    CHECK_EQ(call(__imp__XamShowFriendsUI, 0xFFFFFFFFu), kErrorInvalidParameter);

    // XamShowMessageComposeUI(dwUserIndex, const XUID* recipients, DWORD count, LPCWSTR text), as Gears
    // of War 2 calls it: one recipient and a text.
    CHECK(guest_write_be64(kXuids, 0x0009000000000001ull));
    CHECK(guest_write_be64(kXuids + 8, 0x0009000000000002ull));
    utf16be(kText, "Hello");
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 0, kXuids, 1, kText), kErrorNotLoggedOn);
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 0, kXuids, 2, kText), kErrorNotLoggedOn);
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 0, 0, 0, 0), kErrorNotLoggedOn);  // no recipient, no text
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 2, kXuids, 1, kText), kErrorNoSuchUser);
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 4, kXuids, 1, kText), kErrorInvalidParameter);
    // Malformed arguments are refused whatever the user.
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 0, 0, 1, kText), kErrorInvalidParameter);
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 0, kBad, 1, kText), kErrorInvalidParameter);
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 0, kBad - 8, 2, kText), kErrorInvalidParameter);  // straddles
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 0, kXuids, 101, kText), kErrorInvalidParameter);
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 0, kXuids, 1, kBad), kErrorInvalidParameter);
    // A text running into an inaccessible page before its terminator.
    CHECK(guest_write_be16(kBad - 2, 'A'));  // last character of the accessible page
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 0, kXuids, 1, kBad - 2), kErrorInvalidParameter);
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 1, 0, 1, kText), kErrorInvalidParameter);
    // A text without terminator within the 1024-character bound is checked up to the bound only.
    for (uint32_t i = 0; i < 1100; ++i) CHECK(guest_write_be16(S + 0x1000 + 2 * i, 'x'));
    CHECK(guest_write_be16(S + 0x1000 + 2 * 1100, 0));
    CHECK_EQ(call(__imp__XamShowMessageComposeUI, 0, kXuids, 1, S + 0x1000), kErrorNotLoggedOn);
    // Nothing in this unit ever reports success.
    CHECK(call(__imp__XamShowFriendsUI, 0) != kErrorSuccess);

    runtime_shutdown();
    clear_imports();
    g_mem.release();
    return test_result("rt_xam_live_dialogs");
}
