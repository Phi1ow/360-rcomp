// xam.xex XMsg (src/hle_xam_msg.cpp): every LIVE app (0xFC) request fails with a negative HRESULT for
// the local profile (never signed in to LIVE); messages to other apps answer ERROR_NOT_FOUND.
// Calls go through the same __imp__ symbols generated code uses.
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam_msg.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__XMsgInProcessCall);
PPC_EXTERN_FUNC(__imp__XMsgStartIORequest);

using namespace rcomp;
using namespace rcomp::rt;

namespace {
GuestMemory g_mem;

uint32_t in_process(uint32_t app, uint32_t message, uint32_t arg1, uint32_t arg2) {
    alignas(64) PPCContext ctx{};
    ctx.r3.u64 = app;
    ctx.r4.u64 = message;
    ctx.r5.u64 = arg1;
    ctx.r6.u64 = arg2;
    ctx.lr = 0x82000000u;
    __imp__XMsgInProcessCall(ctx, g_mem.base());
    return ctx.r3.u32;
}
}  // namespace

int main() {
    CHECK(g_mem.reserve() == MemStatus::Ok);
    CHECK_ST(runtime_init(&g_mem), Status::Ok);
    clear_imports();
    CHECK_ST(register_xam_msg_hle(), Status::Ok);

    // Gears of War 2: XFriendsCreateEnumerator = XMsgInProcessCall(0xFC, 0x58020, ...). The XDK wrapper
    // tests the result with `blt`: it must be negative, or the title reads a size it never got.
    const uint32_t friends = in_process(0xFC, 0x00058020u, 0, 0x40001000u);
    CHECK_EQ(friends, 0x800704DDu);
    CHECK(int32_t(friends) < 0);

    // Every LIVE request fails the same way (NAT type 0x58006, 0x58004, 0x58046 at GoW2's campaign start).
    CHECK_EQ(in_process(0xFC, 0x00058004u, 0, 0), 0x800704DDu);
    CHECK_EQ(in_process(0xFC, 0x00058006u, 0x40001000u, 0), 0x800704DDu);
    CHECK_EQ(in_process(0xFC, 0x00058046u, 0, 0x40001000u), 0x800704DDu);
    // Another app is not there: ERROR_NOT_FOUND, as the XAM app manager answers.
    CHECK_EQ(in_process(0xFA, 0x00058020u, 0, 0), 0x490u);
    CHECK_EQ(in_process(0xFA, 0x00058020u, 0, 0), 0x490u);  // reported once, still answered

    // Asynchronous form without an overlapped: the error directly.
    alignas(64) PPCContext ctx{};
    ctx.r3.u64 = 0xFC;
    ctx.r4.u64 = 0x00058020u;
    ctx.r5.u64 = 0;
    __imp__XMsgStartIORequest(ctx, g_mem.base());
    CHECK_EQ(ctx.r3.u32, 0x800704DDu);
    ctx.r3.u64 = 0xFA;
    ctx.r5.u64 = 0;
    __imp__XMsgStartIORequest(ctx, g_mem.base());
    CHECK_EQ(ctx.r3.u32, 0x490u);

    // Media player (0xFA): with no user music the title holds playback. Halo 3's XMPTitleHasPlaybackControl
    // wrapper asks GetPlaybackController {client 2, &controller, &locked} and wants (0, 0).
    uint32_t scratch = 0;
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &scratch), Status::Ok);
    CHECK(guest_write_be32(scratch + 0x00, 2));
    CHECK(guest_write_be32(scratch + 0x04, scratch + 0x20));
    CHECK(guest_write_be32(scratch + 0x08, scratch + 0x24));
    CHECK(guest_write_be32(scratch + 0x20, 0xAAAAAAAAu));
    CHECK(guest_write_be32(scratch + 0x24, 0xBBBBBBBBu));
    CHECK_EQ(in_process(0xFA, 0x0007001Bu, scratch, 0), 0u);
    uint32_t controller = 1, locked = 1;
    CHECK(guest_read_be32(scratch + 0x20, &controller));
    CHECK(guest_read_be32(scratch + 0x24, &locked));
    CHECK_EQ(controller, 0u);
    CHECK_EQ(locked, 0u);
    CHECK_EQ(in_process(0xFA, 0x0007001Bu, 0, 0), 0x57u);  // no argument block
    // SetPlaybackController {client, controller, locked}, synchronous StartIORequest (no overlapped).
    CHECK(guest_write_be32(scratch + 0x30, 2));
    CHECK(guest_write_be32(scratch + 0x34, 0));
    CHECK(guest_write_be32(scratch + 0x38, 1));
    ctx.r3.u64 = 0xFA;
    ctx.r4.u64 = 0x0007001Au;
    ctx.r5.u64 = 0;
    ctx.r6.u64 = scratch + 0x30;
    ctx.r7.u64 = 12;
    __imp__XMsgStartIORequest(ctx, g_mem.base());
    CHECK_EQ(ctx.r3.u32, 0u);
    CHECK_ST(runtime()->heap.free(scratch), Status::Ok);

    runtime_shutdown();
    clear_imports();
    g_mem.release();
    return test_result("rt_xam_msg");
}
