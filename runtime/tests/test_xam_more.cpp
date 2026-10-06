// xam.xex services of src/hle_xam_more.cpp, XamShowMessageBoxUI (src/hle_xam_misc.cpp) and launch data
// (src/hle_xam_loader.cpp) for the one local profile signed in offline, through the __imp__ symbols the
// generated code calls.
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <thread>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam_profile.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__XamShowMessageBoxUI);
PPC_EXTERN_FUNC(__imp__XamShowCustomMessageComposeUI);
PPC_EXTERN_FUNC(__imp__XamUserGetDeviceContext);
PPC_EXTERN_FUNC(__imp__XamVoiceIsActiveProcess);
PPC_EXTERN_FUNC(__imp__XamGetOverlappedResult);
PPC_EXTERN_FUNC(__imp__XMsgCompleteIORequest);
PPC_EXTERN_FUNC(__imp__XamLoaderSetLaunchData);
PPC_EXTERN_FUNC(__imp__XamLoaderGetLaunchDataSize);
PPC_EXTERN_FUNC(__imp__XamLoaderGetLaunchData);
PPC_EXTERN_FUNC(__imp__XNotifyDelayUI);
PPC_EXTERN_FUNC(__imp__XamContentGetLicenseMask);
PPC_EXTERN_FUNC(__imp__XCustomGetCurrentGamercard);
PPC_EXTERN_FUNC(__imp__XCustomGetLastActionPressEx);
PPC_EXTERN_FUNC(__imp__XCustomRegisterDynamicActions);
PPC_EXTERN_FUNC(__imp__XCustomSetDynamicActions);
PPC_EXTERN_FUNC(__imp__XCustomUnregisterDynamicActions);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);

extern bool TESTDOUBLE_pad_connected;

using namespace rcomp;
using namespace rcomp::rt;

namespace {

GuestMemory g_mem;
uint8_t* g_base;
uint32_t g_buf;
enum : uint32_t {
    kOut = 0x000,
    kOut2 = 0x008,
    kEventHandle = 0x010,
    kTimeout = 0x018,  // LARGE_INTEGER 0
    kResult = 0x020,
    kOverlapped = 0x080,
    kButtons = 0x0C0,
    kFrame = 0x100,  // caller frame for stack arguments
    kStrings = 0x400,
    kData = 0x800,    // launch data / actions
    kData2 = 0xC00,
};
constexpr uint32_t kIoPending = 0x3E5, kIoIncomplete = 0x3E4;

uint32_t rd32(uint32_t a) {
    uint32_t v = 0;
    guest_read_be32(a, &v);
    return v;
}
void wr32(uint32_t a, uint32_t v) { guest_write_be32(a, v); }
uint32_t put_wstring(uint32_t a, const char* s) {
    uint32_t i = 0;
    for (; s[i]; ++i) g_base[a + 2 * i] = 0, g_base[a + 2 * i + 1] = uint8_t(s[i]);
    g_base[a + 2 * i] = 0;
    g_base[a + 2 * i + 1] = 0;
    return a;
}

uint32_t call(PPCFunc* f, std::initializer_list<uint64_t> args, std::initializer_list<uint32_t> stack = {}) {
    alignas(64) PPCContext ctx{};
    PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    size_t i = 0;
    for (uint64_t v : args) regs[i++]->u64 = v;
    ctx.r1.u64 = g_buf + kFrame;
    i = 0;
    for (uint32_t v : stack) wr32(g_buf + kFrame + 0x54 + 8 * uint32_t(i++), v);
    f(ctx, g_base);
    return ctx.r3.u32;
}

void (*g_guest_body)() = nullptr;
void guest_entry(PPCContext&, uint8_t*) { g_guest_body(); }
void as_guest(void (*fn)()) {
    alignas(64) PPCContext ctx;
    GuestThread t;
    if (create_guest_thread(runtime()->heap, {0x10000, 0, 0}, &ctx, &t) != Status::Ok) {
        CHECK(false);
        return;
    }
    g_guest_body = fn;
    uint32_t code = 0;
    run_guest_thread(t, ctx, g_base, guest_entry, &code);
    destroy_guest_thread(runtime()->heap, &t);
}

bool event_signalled(uint32_t handle) {
    return call(__imp__NtWaitForSingleObjectEx, {handle, 0, 0, g_buf + kTimeout}) == 0;
}

uint32_t new_event() {
    CHECK_EQ(call(__imp__NtCreateEvent, {g_buf + kEventHandle, 0, 0, 0}), 0u);  // notification, clear
    return rd32(g_buf + kEventHandle);
}

void message_box() {
    const uint32_t s = g_buf + kStrings;
    wr32(g_buf + kButtons, put_wstring(s, "Yes"));
    wr32(g_buf + kButtons + 4, put_wstring(s + 0x40, "No"));
    const uint32_t text = put_wstring(s + 0x80, "Save?");
    // Synchronous: result in r10, the designated default button is pressed.
    wr32(g_buf + kResult, 0xDEAD);
    CHECK_EQ(call(__imp__XamShowMessageBoxUI, {0, 0, text, 2, g_buf + kButtons, 1, 0, g_buf + kResult}, {0}), 0u);
    CHECK_EQ(rd32(g_buf + kResult), 1u);
    // Overlapped in the first stack slot: completes at once.
    const uint32_t ov = g_buf + kOverlapped, event = new_event();
    memset(g_base + ov, 0, 0x1C);
    wr32(ov + 0x0C, event);
    wr32(g_buf + kResult, 0xDEAD);
    CHECK_EQ(call(__imp__XamShowMessageBoxUI, {0xFF, 0, text, 2, g_buf + kButtons, 0, 1, g_buf + kResult}, {ov}),
             kIoPending);
    CHECK_EQ(rd32(g_buf + kResult), 0u);
    CHECK_EQ(rd32(ov), 0u);
    CHECK(event_signalled(event));
    // Same validation as the Ex form.
    CHECK_EQ(call(__imp__XamShowMessageBoxUI, {0, 0, text, 2, g_buf + kButtons, 2, 0, g_buf + kResult}, {0}), 0x57u);
    CHECK_EQ(call(__imp__XamShowMessageBoxUI, {0, 0, text, 1, g_buf + kButtons, 0, 0, 0}, {0}), 0x57u);
}

std::atomic<uint32_t> g_completer_done{0};

void overlapped() {
    const uint32_t ov = g_buf + kOverlapped, event = new_event();
    // XMsgCompleteIORequest: length, extended error, result, event.
    memset(g_base + ov, 0, 0x1C);
    wr32(ov, kIoPending);
    wr32(ov + 0x0C, event);
    CHECK_EQ(call(__imp__XamGetOverlappedResult, {ov, g_buf + kOut, 0}), kIoIncomplete);
    CHECK_EQ(call(__imp__XMsgCompleteIORequest, {ov, 0x10, 0x80070010u, 0x44}), 0u);
    CHECK_EQ(rd32(ov), 0x10u);
    CHECK_EQ(rd32(ov + 4), 0x44u);
    CHECK_EQ(rd32(ov + 0x18), 0x80070010u);
    CHECK(event_signalled(event));
    wr32(g_buf + kOut, 0);
    CHECK_EQ(call(__imp__XamGetOverlappedResult, {ov, g_buf + kOut, 0}), 0x10u);  // the request's result
    CHECK_EQ(rd32(g_buf + kOut), 0x44u);
    CHECK_EQ(call(__imp__XamGetOverlappedResult, {ov, 0, 1}), 0x10u);  // pdwResult optional
    // Rejections: unwritable overlapped, an hEvent that is not an event (nothing written).
    CHECK_EQ(call(__imp__XMsgCompleteIORequest, {0, 0, 0, 0}), 0x57u);
    wr32(ov, kIoPending);
    wr32(ov + 0x0C, 0xF0FFFFF0u);  // no such handle
    CHECK_EQ(call(__imp__XMsgCompleteIORequest, {ov, 0, 0, 0}), 6u);
    CHECK_EQ(rd32(ov), kIoPending);
    CHECK_EQ(call(__imp__XamGetOverlappedResult, {0, g_buf + kOut, 0}), 0x57u);
    bool fatal = false;
    wr32(ov + 0x0C, 0);
    CAPTURE_FATAL(call(__imp__XamGetOverlappedResult, {ov, g_buf + kOut, 1}), fatal);  // wait without event
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    wr32(ov + 0x10, 0x82000000u);
    CAPTURE_FATAL(call(__imp__XMsgCompleteIORequest, {ov, 0, 0, 0}), fatal);  // completion routine
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);

    // bWait blocks in the kernel wait until another thread completes the request.
    const uint32_t event2 = new_event();
    memset(g_base + ov, 0, 0x1C);
    wr32(ov, kIoPending);
    wr32(ov + 0x0C, event2);
    g_completer_done = 0;
    std::thread completer([ov] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        call(__imp__XMsgCompleteIORequest, {ov, 0, 0, 7});
        g_completer_done = 1;
    });
    wr32(g_buf + kOut2, 0);
    CHECK_EQ(call(__imp__XamGetOverlappedResult, {ov, g_buf + kOut2, 1}), 0u);
    CHECK_EQ(g_completer_done.load(), 1u);
    CHECK_EQ(rd32(g_buf + kOut2), 7u);
    completer.join();
}

void users_and_ui() {
    // Device context: the controller port of a connected user's controller.
    wr32(g_buf + kOut, 0xDEAD);
    CHECK_EQ(call(__imp__XamUserGetDeviceContext, {0, 0, g_buf + kOut}), 0u);
    CHECK_EQ(rd32(g_buf + kOut), 0u);
    CHECK_EQ(call(__imp__XamUserGetDeviceContext, {0xFF, 0, g_buf + kOut}), 0u);
    wr32(g_buf + kOut, 0xDEAD);
    CHECK_EQ(call(__imp__XamUserGetDeviceContext, {1, 0, g_buf + kOut}), 0x8007048Fu);
    CHECK_EQ(rd32(g_buf + kOut), 0u);
    TESTDOUBLE_pad_connected = false;
    CHECK_EQ(call(__imp__XamUserGetDeviceContext, {0, 0, g_buf + kOut}), 0x8007048Fu);
    TESTDOUBLE_pad_connected = true;
    CHECK_EQ(call(__imp__XamUserGetDeviceContext, {4, 0, g_buf + kOut}), 0x80070057u);
    CHECK_EQ(call(__imp__XamUserGetDeviceContext, {0, 0, 0}), 0x80070057u);

    CHECK_EQ(call(__imp__XamVoiceIsActiveProcess, {}), 0u);

    // Licences: none.
    wr32(g_buf + kOut, 0xDEAD);
    CHECK_EQ(call(__imp__XamContentGetLicenseMask, {g_buf + kOut, 0}), 0u);
    CHECK_EQ(rd32(g_buf + kOut), 0u);
    const uint32_t ov = g_buf + kOverlapped, event = new_event();
    memset(g_base + ov, 0, 0x1C);
    wr32(ov, kIoPending);
    wr32(ov + 0x0C, event);
    wr32(g_buf + kOut, 0xDEAD);
    CHECK_EQ(call(__imp__XamContentGetLicenseMask, {g_buf + kOut, ov}), kIoPending);
    CHECK_EQ(rd32(g_buf + kOut), 0u);
    CHECK_EQ(rd32(ov), 0u);
    CHECK(event_signalled(event));
    CHECK_EQ(call(__imp__XamContentGetLicenseMask, {0, 0}), 0x57u);

    // Message composing goes through Xbox LIVE.
    CHECK_EQ(call(__imp__XamShowCustomMessageComposeUI, {0, 0, 0, 0, 0, 0, 0, 0}), 0x4DDu);
    CHECK_EQ(call(__imp__XamShowCustomMessageComposeUI, {2, 0, 0, 0, 0, 0, 0, 0}), 0x525u);
    CHECK_EQ(call(__imp__XamShowCustomMessageComposeUI, {9, 0, 0, 0, 0, 0, 0, 0}), 0x57u);

    CHECK_EQ(call(__imp__XNotifyDelayUI, {0x7FFFFFFF}), 0u);
    CHECK_EQ(call(__imp__XNotifyDelayUI, {0}), 0u);
}

void custom_actions() {
    // Halo 3's sequence: register, set an action array (52-byte XCUSTOMACTION), poll, unregister.
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__XCustomSetDynamicActions, {0, kLocalUserXuid, g_buf + kData, 1}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);  // before registration
    call(__imp__XCustomRegisterDynamicActions, {});
    for (uint32_t i = 0; i < 3; ++i) {
        guest_write_be32(g_buf + kData + 52 * i, (i + 1) << 16);  // WORD id, then text
        put_wstring(g_buf + kData + 52 * i + 2, "Invite");
        wr32(g_buf + kData + 52 * i + 48, 0);
    }
    CHECK_EQ(call(__imp__XCustomSetDynamicActions, {0, 0x0009000012345678ull, g_buf + kData, 3}), 0u);
    CHECK_EQ(call(__imp__XCustomSetDynamicActions, {0, 0x0009000012345678ull, 0, 0}), 0u);  // empty set
    CHECK_EQ(call(__imp__XCustomSetDynamicActions, {4, 0, g_buf + kData, 1}), 0x57u);
    CHECK_EQ(call(__imp__XCustomSetDynamicActions, {0, 0, 0x00001000, 1}), 0x57u);
    // No Guide: nothing pressed, no gamercard shown; outputs untouched.
    wr32(g_buf + kOut, 0xDEAD);
    g_base[g_buf + kOut2] = 0;  // WORD cbPayload = 144
    g_base[g_buf + kOut2 + 1] = 144;
    CHECK(call(__imp__XCustomGetLastActionPressEx,
               {g_buf + kOut + 4, g_buf + kOut, g_buf + kData2, g_buf + kData2 + 8, g_buf + kOut2}) != 0);
    CHECK_EQ(rd32(g_buf + kOut), 0xDEADu);
    CHECK_EQ(call(__imp__XCustomGetCurrentGamercard, {g_buf + kOut, g_buf + kData2}), 0u);
    CHECK_EQ(rd32(g_buf + kOut), 0xDEADu);
    call(__imp__XCustomUnregisterDynamicActions, {});
    CAPTURE_FATAL(call(__imp__XCustomSetDynamicActions, {0, 0, g_buf + kData, 1}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
}

void launch_data() {
    // Nothing set: the console's "no launch data" answer.
    wr32(g_buf + kOut, 0xDEAD);
    CHECK_EQ(call(__imp__XamLoaderGetLaunchDataSize, {g_buf + kOut}), 0x490u);
    CHECK_EQ(rd32(g_buf + kOut), 0u);
    CHECK_EQ(call(__imp__XamLoaderGetLaunchData, {g_buf + kData2, 16}), 0x490u);
    // Set, then read back (also truncated to the caller's size).
    for (uint32_t i = 0; i < 10; ++i) g_base[g_buf + kData + i] = uint8_t(0xA0 + i);
    CHECK_EQ(call(__imp__XamLoaderSetLaunchData, {g_buf + kData, 10}), 0u);
    for (uint32_t i = 0; i < 10; ++i) g_base[g_buf + kData + i] = 0;  // the copy is XAM's own
    CHECK_EQ(call(__imp__XamLoaderGetLaunchDataSize, {g_buf + kOut}), 0u);
    CHECK_EQ(rd32(g_buf + kOut), 10u);
    memset(g_base + g_buf + kData2, 0xEE, 16);
    CHECK_EQ(call(__imp__XamLoaderGetLaunchData, {g_buf + kData2, 16}), 0u);
    for (uint32_t i = 0; i < 10; ++i) CHECK_EQ(g_base[g_buf + kData2 + i], 0xA0 + i);
    CHECK_EQ(g_base[g_buf + kData2 + 10], 0xEE);
    memset(g_base + g_buf + kData2, 0xEE, 16);
    CHECK_EQ(call(__imp__XamLoaderGetLaunchData, {g_buf + kData2, 4}), 0u);
    CHECK_EQ(g_base[g_buf + kData2 + 3], 0xA3);
    CHECK_EQ(g_base[g_buf + kData2 + 4], 0xEE);
    // Rejections, then size 0 clears.
    CHECK_EQ(call(__imp__XamLoaderSetLaunchData, {0x00001000, 4}), 0x57u);
    CHECK_EQ(call(__imp__XamLoaderGetLaunchData, {0x00001000, 4}), 0x57u);
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__XamLoaderSetLaunchData, {g_buf + kData, 0x10001}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_EQ(call(__imp__XamLoaderSetLaunchData, {0, 0}), 0u);
    CHECK_EQ(call(__imp__XamLoaderGetLaunchDataSize, {g_buf + kOut}), 0x490u);
}

}  // namespace

int main() {
    if (g_mem.reserve() != MemStatus::Ok) return 2;
    g_base = g_mem.base();
    CHECK_ST(runtime_init(&g_mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(register_xam_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &g_buf), Status::Ok);
    as_guest(message_box);
    as_guest(overlapped);
    as_guest(users_and_ui);
    as_guest(custom_actions);
    as_guest(launch_data);

    // A new Runtime generation forgets launch data and the action registration.
    as_guest([] { CHECK_EQ(call(__imp__XamLoaderSetLaunchData, {g_buf + kData, 4}), 0u); });
    runtime_shutdown();
    clear_imports();
    CHECK_ST(runtime_init(&g_mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(register_xam_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &g_buf), Status::Ok);
    as_guest([] {
        CHECK_EQ(call(__imp__XamLoaderGetLaunchDataSize, {g_buf + kOut}), 0x490u);
        bool fatal = false;
        CAPTURE_FATAL(call(__imp__XCustomSetDynamicActions, {0, 0, g_buf + kData, 1}), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    });
    runtime_shutdown();
    clear_imports();
    return test_result("rt_xam_more");
}
