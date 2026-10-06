// xam.xex tasks, sessions, system UI, voice, QoS and XNetLogon (src/hle_xam_misc.cpp) for the one local
// profile signed in offline. Calls go through the same __imp__ symbols generated code uses; the task
// callbacks are host functions registered at guest addresses, as test_threads.cpp does.
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <thread>
#include <vector>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/modules.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam_profile.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__NetDll_XNetQosListen);
PPC_EXTERN_FUNC(__imp__NetDll_XNetQosLookup);
PPC_EXTERN_FUNC(__imp__NetDll_XNetQosRelease);
PPC_EXTERN_FUNC(__imp__XNetLogonGetMachineID);
PPC_EXTERN_FUNC(__imp__XNetLogonGetTitleID);
PPC_EXTERN_FUNC(__imp__XamTaskSchedule);
PPC_EXTERN_FUNC(__imp__XamTaskCloseHandle);
PPC_EXTERN_FUNC(__imp__XamTaskShouldExit);
PPC_EXTERN_FUNC(__imp__XamUserAreUsersFriends);
PPC_EXTERN_FUNC(__imp__XamUserGetMembershipTierFromXUID);
PPC_EXTERN_FUNC(__imp__XamUserGetOnlineCountryFromXUID);
PPC_EXTERN_FUNC(__imp__XamShowPlayerReviewUI);
PPC_EXTERN_FUNC(__imp__XamShowMarketplaceUI);
PPC_EXTERN_FUNC(__imp__XamShowGamerCardUIForXUID);
PPC_EXTERN_FUNC(__imp__XamShowMessageBoxUIEx);
PPC_EXTERN_FUNC(__imp__XamShowMarketplaceDownloadItemsUI);
PPC_EXTERN_FUNC(__imp__XamVoiceCreate);
PPC_EXTERN_FUNC(__imp__XamVoiceHeadsetPresent);
PPC_EXTERN_FUNC(__imp__XamVoiceSubmitPacket);
PPC_EXTERN_FUNC(__imp__XamVoiceClose);
PPC_EXTERN_FUNC(__imp__XamSessionCreateHandle);
PPC_EXTERN_FUNC(__imp__XamSessionRefObjByHandle);
PPC_EXTERN_FUNC(__imp__ObDereferenceObject);
PPC_EXTERN_FUNC(__imp__NtDuplicateObject);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);

using namespace rcomp;
using namespace rcomp::rt;

namespace {

GuestMemory g_mem;
uint8_t* g_base;
uint32_t g_buf;  // guest scratch block
enum : uint32_t {
    kOut = 0x000,         // general outputs
    kOut2 = 0x008,
    kEventHandle = 0x010,
    kTimeout = 0x018,     // LARGE_INTEGER 0
    kResult = 0x020,      // MESSAGEBOX_RESULT
    kHresult = 0x028,
    kXuids = 0x030,       // two XUIDs
    kAttributes = 0x040,  // XTASK attribute block
    kOverlapped = 0x080,  // 0x1C bytes
    kButtons = 0x0C0,     // three button pointers
    kFrame = 0x100,       // caller frame for stack arguments (r1)
    kStrings = 0x400,     // UTF-16BE strings
    kTaskContext = 0x800,
};
constexpr uint32_t kTaskFn = 0x82100100, kGatedTaskFn = 0x82100200, kLoopTaskFn = 0x82100300;
constexpr uint32_t kWsaEinval = 10022;

uint32_t rd32(uint32_t a) {
    uint32_t v = 0;
    guest_read_be32(a, &v);
    return v;
}
uint64_t rd64(uint32_t a) {
    uint64_t v = 0;
    guest_read_be64(a, &v);
    return v;
}
void wr32(uint32_t a, uint32_t v) { guest_write_be32(a, v); }
void zero(uint32_t a, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 4) wr32(a + i, 0);
}
uint32_t put_wstring(uint32_t a, const char* s) {
    uint32_t i = 0;
    for (; s[i]; ++i) g_base[a + 2 * i] = 0, g_base[a + 2 * i + 1] = uint8_t(s[i]);
    g_base[a + 2 * i] = 0;
    g_base[a + 2 * i + 1] = 0;
    return a;
}

// r3.. from `args`; `stack` holds arguments 9.. in the caller frame at r1 + 0x54 + 8*i.
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

// Runs `fn` as a guest thread on the calling host thread (waits need one).
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

// ---- tasks ----
std::atomic<uint32_t> g_task_context{0}, g_task_should_exit{99}, g_task_done{0};
std::atomic<uint32_t> g_gate{0}, g_gated_should_exit{99}, g_gated_done{0};
std::atomic<uint32_t> g_loop_started{0}, g_loop_exited{0};

void task_fn(PPCContext& ctx, uint8_t* base) {
    g_task_context = ctx.r3.u32;
    __imp__XamTaskShouldExit(ctx, base);
    g_task_should_exit = ctx.r3.u32;
    g_task_done = 1;
    ctx.r3.u64 = 0;
}
void gated_task_fn(PPCContext& ctx, uint8_t* base) {
    while (!g_gate.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    __imp__XamTaskShouldExit(ctx, base);
    g_gated_should_exit = ctx.r3.u32;
    g_gated_done = 1;
    ctx.r3.u64 = 0;
}
// The XDK task loop: run until XamTaskShouldExit reports TRUE.
void loop_task_fn(PPCContext& ctx, uint8_t* base) {
    g_loop_started = 1;
    for (;;) {
        __imp__XamTaskShouldExit(ctx, base);
        if (ctx.r3.u32) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    g_loop_exited = 1;
    ctx.r3.u64 = 0;
}

bool wait_for(const std::atomic<uint32_t>& flag) {
    for (int i = 0; i < 5000 && !flag.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return flag.load() != 0;
}

void tasks_body() {
    Runtime& r = *runtime();
    const uint32_t live = r.handles.live_count();
    wr32(g_buf + kAttributes, 0x02080002u);
    wr32(g_buf + kAttributes + 4, 0);
    CHECK_EQ(call(__imp__XamTaskSchedule, {kTaskFn, g_buf + kTaskContext, g_buf + kAttributes, g_buf + kOut}), 0u);
    const uint32_t task = rd32(g_buf + kOut);
    CHECK(task != 0);
    CHECK(wait_for(g_task_done));
    CHECK_EQ(g_task_context.load(), g_buf + kTaskContext);  // callback(context)
    CHECK_EQ(g_task_should_exit.load(), 0u);                 // nothing asked it to stop
    CHECK_EQ(call(__imp__XamTaskCloseHandle, {task}), 1u);
    CHECK_EQ(call(__imp__XamTaskCloseHandle, {task}), 0u);  // already closed
    CHECK_EQ(call(__imp__XamTaskCloseHandle, {0xF0000004u}), 0u);

    // As GTA IV does: close the task handle at once; the task still runs to its end.
    CHECK_EQ(call(__imp__XamTaskSchedule, {kGatedTaskFn, 0, g_buf + kAttributes, g_buf + kOut}), 0u);
    CHECK_EQ(call(__imp__XamTaskCloseHandle, {rd32(g_buf + kOut)}), 1u);
    g_gate = 1;
    CHECK(wait_for(g_gated_done));
    CHECK_EQ(g_gated_should_exit.load(), 0u);
    for (int i = 0; i < 2000 && r.handles.live_count() != live; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK_EQ(r.handles.live_count(), live);

    // A task handle is not a session handle, and the reverse.
    CHECK_EQ(call(__imp__XamSessionCreateHandle, {g_buf + kOut2}), 0u);
    CHECK_EQ(call(__imp__XamTaskCloseHandle, {rd32(g_buf + kOut2)}), 0u);
    CHECK_EQ(call(__imp__NtClose, {rd32(g_buf + kOut2)}), 0u);

    // Only the established attribute block is accepted; anything else stops instead of guessing.
    bool fatal = false;
    wr32(g_buf + kAttributes, 0);
    CAPTURE_FATAL(call(__imp__XamTaskSchedule, {kTaskFn, 0, g_buf + kAttributes, g_buf + kOut}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(call(__imp__XamTaskSchedule, {kTaskFn, 0, 0, g_buf + kOut}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    wr32(g_buf + kAttributes, 0x02080002u);
    CAPTURE_FATAL(call(__imp__XamTaskSchedule, {0x82000F00u, 0, g_buf + kAttributes, g_buf + kOut}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_INDIRECT_TARGET);
    CHECK_EQ(call(__imp__XamTaskSchedule, {kTaskFn, 0, g_buf + kAttributes, 0}), 0x80070057u);
    CHECK_EQ(r.handles.live_count(), live);

    // XamTaskShouldExit belongs to task threads.
    CAPTURE_FATAL(call(__imp__XamTaskShouldExit, {}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(g_fatal_msg.find("not a XamTaskSchedule task") != std::string::npos);
}

// ---- sessions ----
void sessions_body() {
    Runtime& r = *runtime();
    const uint32_t live = r.handles.live_count();
    const uint32_t allocations = r.heap.stats().live_allocations;
    CHECK_EQ(call(__imp__XamSessionCreateHandle, {g_buf + kOut}), 0u);
    const uint32_t session = rd32(g_buf + kOut);
    CHECK_EQ(r.handles.live_count(), live + 1);
    CHECK_EQ(call(__imp__XamSessionRefObjByHandle, {session, g_buf + kOut2}), 0u);
    const uint32_t body = rd32(g_buf + kOut2);
    CHECK(body != 0 && r.mem->is_accessible(body, 16, Protect::ReadWrite));
    CHECK_EQ(call(__imp__XamSessionRefObjByHandle, {session, g_buf + kOut2}), 0u);
    CHECK_EQ(rd32(g_buf + kOut2), body);  // one object, two references
    call(__imp__ObDereferenceObject, {body});
    // The Body outlives the handle while a reference exists.
    CHECK_EQ(call(__imp__NtClose, {session}), 0u);
    CHECK_EQ(call(__imp__XamSessionRefObjByHandle, {session, g_buf + kOut2}), 6u);  // ERROR_INVALID_HANDLE
    CHECK_EQ(r.heap.stats().live_allocations, allocations + 1);
    call(__imp__ObDereferenceObject, {body});
    CHECK_EQ(r.heap.stats().live_allocations, allocations);  // last reference: Body released
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__ObDereferenceObject, {body}), fatal);  // stale Body
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);

    // Underflow on a live Body, and a duplicated handle keeps the object.
    CHECK_EQ(call(__imp__XamSessionCreateHandle, {g_buf + kOut}), 0u);
    const uint32_t second = rd32(g_buf + kOut);
    CHECK_EQ(call(__imp__XamSessionRefObjByHandle, {second, g_buf + kOut2}), 0u);
    const uint32_t body2 = rd32(g_buf + kOut2);
    call(__imp__ObDereferenceObject, {body2});
    CAPTURE_FATAL(call(__imp__ObDereferenceObject, {body2}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    CHECK(g_fatal_msg.find("session refcount underflow") != std::string::npos);
    CHECK_EQ(call(__imp__NtDuplicateObject, {second, g_buf + kOut, 0}), 0u);
    const uint32_t duplicate = rd32(g_buf + kOut);
    CHECK_EQ(call(__imp__NtClose, {second}), 0u);
    CHECK_EQ(call(__imp__XamSessionRefObjByHandle, {duplicate, g_buf + kOut2}), 0u);
    CHECK_EQ(rd32(g_buf + kOut2), body2);
    call(__imp__ObDereferenceObject, {body2});
    CHECK_EQ(call(__imp__NtClose, {duplicate}), 0u);
    CHECK_EQ(r.heap.stats().live_allocations, allocations);
    CHECK_EQ(r.handles.live_count(), live);

    // Invalid outputs and handles.
    CHECK_EQ(call(__imp__XamSessionCreateHandle, {0}), 0x57u);
    CHECK_EQ(call(__imp__XamSessionRefObjByHandle, {0xF0000004u, g_buf + kOut2}), 6u);
}

// ---- system UI ----
void ui_body() {
    const uint32_t s = g_buf + kStrings;
    const uint32_t text = put_wstring(s, "The disc \"cannot\" be read.\x01");
    const uint32_t ok = put_wstring(s + 0x100, "OK");
    const uint32_t retry = put_wstring(s + 0x120, "Retry");
    wr32(g_buf + kButtons, ok);
    wr32(g_buf + kButtons + 4, retry);
    CHECK_EQ(call(__imp__NtCreateEvent, {g_buf + kEventHandle, 0, 1, 0}), 0u);
    const uint32_t event = rd32(g_buf + kEventHandle);
    const uint32_t ov = g_buf + kOverlapped;
    wr32(g_buf + kTimeout, 0);
    wr32(g_buf + kTimeout + 4, 0);

    // GTA IV's call shape: user 0xFF, NULL title, XMB_ERRORICON, unknown argument 1, overlapped.
    zero(ov, 0x1C);
    wr32(ov + 0x0C, event);
    wr32(g_buf + kResult, 0xDEAD);
    CHECK_EQ(call(__imp__XamShowMessageBoxUIEx, {0xFF, 0, text, 2, g_buf + kButtons, 1, 1, 1},
                  {g_buf + kResult, ov}),
             0x3E5u);  // ERROR_IO_PENDING, already complete
    CHECK_EQ(rd32(g_buf + kResult), 1u);  // the designated default button
    CHECK_EQ(rd32(ov + 0x00), 0u);
    CHECK_EQ(rd32(ov + 0x04), 0u);
    CHECK_EQ(rd32(ov + 0x18), 0u);
    CHECK(event_signalled(event));
    // Synchronous form.
    wr32(g_buf + kResult, 0xDEAD);
    CHECK_EQ(call(__imp__XamShowMessageBoxUIEx, {0, put_wstring(s + 0x200, "Title"), text, 1, g_buf + kButtons, 0, 0, 0},
                  {g_buf + kResult, 0}),
             0u);
    CHECK_EQ(rd32(g_buf + kResult), 0u);
    // Rejected requests leave the result and the overlapped untouched.
    wr32(g_buf + kResult, 0xDEAD);
    wr32(ov, 0x1234);
    CHECK_EQ(call(__imp__XamShowMessageBoxUIEx, {0, 0, text, 2, g_buf + kButtons, 2, 0, 0}, {g_buf + kResult, ov}),
             0x57u);  // default button outside the list
    CHECK_EQ(call(__imp__XamShowMessageBoxUIEx, {0, 0, text, 0, g_buf + kButtons, 0, 0, 0}, {g_buf + kResult, ov}),
             0x57u);
    CHECK_EQ(call(__imp__XamShowMessageBoxUIEx, {0, 0, text, 4, g_buf + kButtons, 0, 0, 0}, {g_buf + kResult, ov}),
             0x57u);
    CHECK_EQ(call(__imp__XamShowMessageBoxUIEx, {7, 0, text, 1, g_buf + kButtons, 0, 0, 0}, {g_buf + kResult, ov}),
             0x57u);
    CHECK_EQ(call(__imp__XamShowMessageBoxUIEx, {0, 0, text, 1, g_buf + kButtons, 0, 0, 0}, {0, ov}), 0x57u);
    wr32(g_buf + kButtons + 8, 0);
    CHECK_EQ(call(__imp__XamShowMessageBoxUIEx, {0, 0, text, 3, g_buf + kButtons, 0, 0, 0}, {g_buf + kResult, ov}),
             0x57u);  // NULL button string
    wr32(ov + 0x0C, 0xF0000004u);
    CHECK_EQ(call(__imp__XamShowMessageBoxUIEx, {0, 0, text, 1, g_buf + kButtons, 0, 0, 0}, {g_buf + kResult, ov}),
             6u);  // the overlapped names no event
    CHECK_EQ(rd32(g_buf + kResult), 0xDEADu);
    CHECK_EQ(rd32(ov), 0x1234u);
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__XamShowMessageBoxUIEx, {0, 0, text, 1, g_buf + kButtons, 0, 0x10000, 0},
                       {g_buf + kResult, 0}),
                  fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(call(__imp__XamShowMessageBoxUIEx, {0, 0, text, 1, g_buf + kButtons, 0, 0x100, 0},
                       {g_buf + kResult, 0}),
                  fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);

    // LIVE-only UIs: the local profile is not signed in to LIVE, slots 1-3 are empty.
    const uint64_t other = 0x0009000000000001ull;
    CHECK_EQ(call(__imp__XamShowGamerCardUIForXUID, {0, kLocalUserXuid, 0}), 0u);  // own offline gamercard
    CHECK_EQ(call(__imp__XamShowGamerCardUIForXUID, {0, other, 0}), 0x4DDu);
    CHECK_EQ(call(__imp__XamShowGamerCardUIForXUID, {1, other, 0}), 0x525u);
    CHECK_EQ(call(__imp__XamShowGamerCardUIForXUID, {4, other, 0}), 0x57u);
    CHECK_EQ(call(__imp__XamShowPlayerReviewUI, {0, other}), 0x4DDu);
    CHECK_EQ(call(__imp__XamShowPlayerReviewUI, {2, other}), 0x525u);
    CHECK_EQ(call(__imp__XamShowMarketplaceUI, {0, 0, 0x5454000100000001ull, 0xFFFFFFFFu, 0, 0}), 0x4DDu);
    CHECK_EQ(call(__imp__XamShowMarketplaceUI, {3, 0, 0, 0xFFFFFFFFu, 0, 0}), 0x525u);
    guest_write_be64(g_buf + kXuids, 0x5454000100000001ull);
    zero(ov, 0x1C);
    wr32(ov + 0x0C, event);
    wr32(g_buf + kHresult, 0);
    CHECK_EQ(call(__imp__XamShowMarketplaceDownloadItemsUI, {0, 0, g_buf + kXuids, 1, 0, g_buf + kHresult, ov, 0}),
             0x3E5u);
    CHECK_EQ(rd32(g_buf + kHresult), 0x800704DDu);
    CHECK_EQ(rd32(ov + 0x00), 0x4DDu);
    CHECK_EQ(rd32(ov + 0x18), 0x800704DDu);
    CHECK(event_signalled(event));
    CHECK_EQ(call(__imp__XamShowMarketplaceDownloadItemsUI, {0, 0, g_buf + kXuids, 1, 0, g_buf + kHresult, 0, 0}),
             0x4DDu);
    CAPTURE_FATAL(
        call(__imp__XamShowMarketplaceDownloadItemsUI, {0, 0, g_buf + kXuids, 1, 1, g_buf + kHresult, 0, 0}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);

    // Friends: synchronous (GTA IV) and overlapped forms; *pfResult is always FALSE.
    guest_write_be64(g_buf + kXuids, other);
    wr32(g_buf + kOut, 1);
    CHECK_EQ(call(__imp__XamUserAreUsersFriends, {0, g_buf + kXuids, 1, g_buf + kOut, 0}), 0x4DDu);
    CHECK_EQ(rd32(g_buf + kOut), 0u);
    wr32(g_buf + kOut, 1);
    CHECK_EQ(call(__imp__XamUserAreUsersFriends, {1, g_buf + kXuids, 1, g_buf + kOut, 0}), 0x525u);
    CHECK_EQ(rd32(g_buf + kOut), 0u);
    CHECK_EQ(call(__imp__XamUserAreUsersFriends, {4, g_buf + kXuids, 1, g_buf + kOut, 0}), 0x57u);
    CHECK_EQ(call(__imp__XamUserAreUsersFriends, {0, g_buf + kXuids, 0, g_buf + kOut, 0}), 0x57u);
    CHECK_EQ(call(__imp__XamUserAreUsersFriends, {0, g_buf + kXuids, 1, 0, 0}), 0x57u);
    zero(ov, 0x1C);
    wr32(ov + 0x0C, event);
    CHECK_EQ(call(__imp__XamUserAreUsersFriends, {0, g_buf + kXuids, 1, 0, ov}), 0x3E5u);
    CHECK_EQ(rd32(ov + 0x00), 0x4DDu);
    CHECK_EQ(rd32(ov + 0x18), 0x800704DDu);
    CHECK(event_signalled(event));
    CHECK_EQ(call(__imp__XamUserGetMembershipTierFromXUID, {kLocalUserXuid}), 0u);
    CHECK_EQ(call(__imp__XamUserGetMembershipTierFromXUID, {other}), 0u);
    CHECK_EQ(call(__imp__XamUserGetOnlineCountryFromXUID, {kLocalUserXuid}), 0u);
    CHECK_EQ(call(__imp__NtClose, {event}), 0u);
}

// ---- voice, QoS, logon ----
void voice_net_body() {
    wr32(g_buf + kOut, 0xDEAD);
    CHECK_EQ(call(__imp__XamVoiceCreate, {0, 0xF, g_buf + kOut}), 0x8007048Fu);  // no voice device
    CHECK_EQ(rd32(g_buf + kOut), 0u);
    CHECK_EQ(call(__imp__XamVoiceCreate, {4, 0xF, g_buf + kOut}), 0x80070057u);
    CHECK_EQ(call(__imp__XamVoiceCreate, {0, 0xF, 0}), 0x80070057u);
    CHECK_EQ(call(__imp__XamVoiceHeadsetPresent, {0}), 0u);
    CHECK_EQ(call(__imp__XamVoiceHeadsetPresent, {g_buf}), 0u);
    CHECK_EQ(call(__imp__XamVoiceSubmitPacket, {g_buf, 0, g_buf + kStrings}), 0x80070006u);
    CHECK_EQ(call(__imp__XamVoiceClose, {g_buf}), 0x80070006u);

    // GTA IV's QoS listen (flags ENABLE|SET_DATA) and lookup (one peer) shapes.
    CHECK_EQ(call(__imp__NetDll_XNetQosListen, {1, g_buf + kXuids, g_buf + kStrings, 8, 0, 5}), kWsaEinval);
    wr32(g_buf + kOut2, 0xDEAD);
    CHECK_EQ(call(__imp__NetDll_XNetQosLookup,
                  {1, 1, g_buf + kButtons, g_buf + kButtons, g_buf + kButtons, 0, 0, 0}, {0, 0, 0, 0, g_buf + kOut2}),
             kWsaEinval);
    CHECK_EQ(rd32(g_buf + kOut2), 0u);  // no XNQOS was allocated
    CHECK_EQ(call(__imp__NetDll_XNetQosRelease, {1, g_buf + kOut2}), kWsaEinval);
    CHECK_EQ(call(__imp__NetDll_XNetQosRelease, {1, 0}), kWsaEinval);

    guest_write_be64(g_buf + kOut, 0x1122334455667788ull);
    CHECK_EQ(call(__imp__XNetLogonGetMachineID, {g_buf + kOut}), 0x80151802u);  // XONLINE_E_LOGON_NOT_LOGGED_ON
    CHECK_EQ(rd64(g_buf + kOut), 0ull);
    CHECK_EQ(call(__imp__XNetLogonGetMachineID, {0}), 0x80070057u);
}

// A synthetic main XEX (original bytes, as test_xam.cpp builds) whose execution info carries a title id.
void be32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    b[o] = uint8_t(v >> 24), b[o + 1] = uint8_t(v >> 16), b[o + 2] = uint8_t(v >> 8), b[o + 3] = uint8_t(v);
}
void le16(std::vector<uint8_t>& b, size_t o, uint16_t v) { b[o] = uint8_t(v), b[o + 1] = uint8_t(v >> 8); }
void le32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + i] = uint8_t(v >> (8 * i));
}
constexpr uint32_t kImageBase = 0x82000000, kImageSize = 0x3000, kImageEntry = kImageBase + 0x1000;
constexpr uint32_t kTitleId = 0x545407F2;
XexImage main_image() {
    std::vector<uint8_t> image(kImageSize);
    le16(image, 0, 0x5A4D);
    le32(image, 0x3C, 0x80);
    const size_t nt = 0x80, optional = nt + 24, section = optional + 224;
    le32(image, nt, 0x4550);
    le16(image, nt + 4, 0x1F2);
    le16(image, nt + 6, 1);
    le16(image, nt + 20, 224);
    le16(image, optional, 0x10B);
    le32(image, optional + 16, 0x1000);
    le32(image, optional + 28, kImageBase);
    le32(image, optional + 32, 0x1000);
    le32(image, optional + 36, 0x200);
    le32(image, optional + 56, kImageSize);
    le32(image, optional + 60, 0x200);
    le32(image, optional + 92, 16);
    le32(image, section + 8, 0x1000);
    le32(image, section + 12, 0x1000);
    le32(image, section + 16, 0x1000);
    le32(image, section + 36, 0x60000020);
    CHECK_ST(load_raw_image(g_mem, kImageBase, image.data(), image.size()), Status::Ok);
    std::vector<uint8_t> header(0x1000);
    be32(header, 0, 0x58455832);  // XEX2
    be32(header, 8, 0x1000);
    be32(header, 16, 0x400);
    be32(header, 20, 3);
    const uint32_t fields[][2] = {{0x00010100, kImageEntry}, {0x00010201, kImageBase}, {0x00040006, 0x100}};
    for (size_t i = 0; i < 3; ++i) be32(header, 24 + i * 8, fields[i][0]), be32(header, 28 + i * 8, fields[i][1]);
    be32(header, 0x100, 0x4A53F9F6);
    be32(header, 0x104, 6);
    be32(header, 0x108, 5);
    be32(header, 0x10C, kTitleId);
    header[0x112] = 1;
    header[0x113] = 2;
    be32(header, 0x404, kImageSize);
    be32(header, 0x510, kImageBase);
    XexImage result{};
    result.base = kImageBase;
    result.size = kImageSize;
    result.entry_point = kImageEntry;
    result.header = std::move(header);
    return result;
}

}  // namespace

int main() {
    if (g_mem.reserve() != MemStatus::Ok) return 2;
    g_base = g_mem.base();
    CHECK_ST(runtime_init(&g_mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(register_xam_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &g_buf), Status::Ok);
    const FuncEntry funcs[] = {{kTaskFn, task_fn, "task_fn"},
                               {kGatedTaskFn, gated_task_fn, "gated_task_fn"},
                               {kLoopTaskFn, loop_task_fn, "loop_task_fn"}};
    CHECK(register_functions(funcs, 3));

    // XNetLogonGetTitleID reports the running title's id, which needs the finalized main XEX.
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__XNetLogonGetTitleID, {9}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_ST(runtime_prepare_main_module({"game:\\Original.xex", "Original.xex"}), Status::Ok);
    CHECK_ST(runtime_finalize_main_module(main_image()), Status::Ok);
    CHECK_EQ(call(__imp__XNetLogonGetTitleID, {9}), kTitleId);

    as_guest(voice_net_body);
    as_guest(ui_body);
    as_guest(sessions_body);
    as_guest(tasks_body);
    // Outside any guest thread.
    CAPTURE_FATAL(call(__imp__XamTaskShouldExit, {}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);

    // A task looping on XamTaskShouldExit leaves once the runtime quiesces its threads: shutdown must
    // not hang on it (the ctest timeout would fail this test).
    as_guest([] {
        CHECK_EQ(call(__imp__XamTaskSchedule, {kLoopTaskFn, 0, g_buf + kAttributes, g_buf + kOut}), 0u);
        CHECK_EQ(call(__imp__XamTaskCloseHandle, {rd32(g_buf + kOut)}), 1u);
    });
    CHECK(wait_for(g_loop_started));
    CHECK_EQ(g_loop_exited.load(), 0u);
    runtime_shutdown();
    CHECK_EQ(g_loop_exited.load(), 1u);
    clear_imports();
    return test_result("rt_xam_misc");
}
