#include <atomic>
#include <thread>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/process_lifecycle.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__ExCreateThread);
PPC_EXTERN_FUNC(__imp__ExRegisterTitleTerminateNotification);
PPC_EXTERN_FUNC(__imp__HalReturnToFirmware);
PPC_EXTERN_FUNC(__imp__KeEnterCriticalRegion);
PPC_EXTERN_FUNC(__imp__KeGetCurrentProcessType);
PPC_EXTERN_FUNC(__imp__KeInitializeEvent);
PPC_EXTERN_FUNC(__imp__KeLeaveCriticalRegion);
PPC_EXTERN_FUNC(__imp__KeWaitForSingleObject);
PPC_EXTERN_FUNC(__imp__XamLoaderTerminateTitle);
PPC_EXTERN_FUNC(__imp__XamLoaderLaunchTitle);
PPC_EXTERN_FUNC(__imp__XamShowDirtyDiscErrorUI);

using namespace rcomp;
using namespace rcomp::rt;

namespace {

GuestMemory g_memory;
uint8_t* g_base = nullptr;
uint32_t g_scratch = 0;

constexpr uint32_t kWorkerEntry = 0x82000100;
constexpr uint32_t kMainCallback = 0x82000200;
constexpr uint32_t kWorkerCallback = 0x82000300;
constexpr uint32_t kRemovedCallback = 0x82000400;

constexpr uint32_t kRegMain = 0x00;
constexpr uint32_t kRegWorker = 0x20;
constexpr uint32_t kRegRemoved = 0x40;
constexpr uint32_t kRegStale = 0x60;
constexpr uint32_t kEvent = 0x80;
constexpr uint32_t kHandle = 0xA0;

std::atomic<bool> g_worker_registered{false};
std::atomic<bool> g_worker_returned{false};
std::atomic<uint32_t> g_worker_wait_status{0};
std::atomic<bool> g_quiesce_finished{false};
std::atomic<uint32_t> g_callback_count{0};
std::atomic<uint32_t> g_removed_count{0};
std::atomic<uint32_t> g_stale_count{0};
std::atomic<uint32_t> g_order0{0};
std::atomic<uint32_t> g_order1{0};

void set_args(PPCContext& ctx, uint32_t r3 = 0, uint32_t r4 = 0, uint32_t r5 = 0,
              uint32_t r6 = 0, uint32_t r7 = 0, uint32_t r8 = 0, uint32_t r9 = 0) {
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    ctx.r7.u64 = r7;
    ctx.r8.u64 = r8;
    ctx.r9.u64 = r9;
}

void write_registration(uint32_t offset, uint32_t routine, uint32_t priority) {
    CHECK(guest_write_be32(g_scratch + offset + 0, routine));
    CHECK(guest_write_be32(g_scratch + offset + 4, priority));
    CHECK(guest_write_be32(g_scratch + offset + 8, 0));
    CHECK(guest_write_be32(g_scratch + offset + 12, 0));
}

void join_quiesce() {
    // The real runtime now separates the guest-safe request from this owner
    // wait. No test double performs cancellation or substitutes a dispatcher.
    runtime_finish_thread_quiesce();
    g_quiesce_finished = true;
}

void TESTDOUBLE_main_callback(PPCContext& ctx, uint8_t* base) {
    __imp__KeGetCurrentProcessType(ctx, base);
    CHECK_EQ(ctx.r3.u32, kProcessTypeUser);
    const uint32_t slot = g_callback_count.fetch_add(1);
    if (slot == 0) g_order0 = 1;
    if (slot == 1) g_order1 = 1;
}

void TESTDOUBLE_worker_registered_callback(PPCContext& ctx, uint8_t* base) {
    __imp__KeGetCurrentProcessType(ctx, base);
    CHECK_EQ(ctx.r3.u32, kProcessTypeUser);
    const uint32_t slot = g_callback_count.fetch_add(1);
    if (slot == 0) g_order0 = 2;
    if (slot == 1) g_order1 = 2;
}

void TESTDOUBLE_removed_callback(PPCContext&, uint8_t*) { ++g_removed_count; }
void TESTDOUBLE_stale_callback(PPCContext&, uint8_t*) { ++g_stale_count; }

void TESTDOUBLE_worker_entry(PPCContext& ctx, uint8_t* base) {
    set_args(ctx, g_scratch + kRegWorker, 1);
    __imp__ExRegisterTitleTerminateNotification(ctx, base);
    g_worker_registered = true;

    set_args(ctx, g_scratch + kEvent, 0, 0, 0, 0);
    __imp__KeWaitForSingleObject(ctx, base);
    g_worker_wait_status = ctx.r3.u32;
    g_worker_returned = true;
}

void TESTDOUBLE_process_and_critical(PPCContext& ctx, uint8_t* base) {
    __imp__KeGetCurrentProcessType(ctx, base);
    CHECK_EQ(ctx.r3.u32, kProcessTypeUser);
    int32_t count = 99;
    CHECK_ST(current_guest_apc_disable_count(&count), Status::Ok);
    CHECK_EQ(count, 0);
    __imp__KeEnterCriticalRegion(ctx, base);
    __imp__KeEnterCriticalRegion(ctx, base);
    CHECK_ST(current_guest_apc_disable_count(&count), Status::Ok);
    CHECK_EQ(count, -2);
    __imp__KeLeaveCriticalRegion(ctx, base);
    CHECK_ST(current_guest_apc_disable_count(&count), Status::Ok);
    CHECK_EQ(count, -1);
    __imp__KeLeaveCriticalRegion(ctx, base);
    CHECK_ST(current_guest_apc_disable_count(&count), Status::Ok);
    CHECK_EQ(count, 0);
}

void TESTDOUBLE_unmatched_leave(PPCContext& ctx, uint8_t* base) {
    __imp__KeLeaveCriticalRegion(ctx, base);
}

void TESTDOUBLE_invalid_registration(PPCContext& ctx, uint8_t* base) {
    set_args(ctx, 0x1234, 1);
    __imp__ExRegisterTitleTerminateNotification(ctx, base);
}

void TESTDOUBLE_duplicate_registration(PPCContext& ctx, uint8_t* base) {
    set_args(ctx, g_scratch + kRegMain, 1);
    __imp__ExRegisterTitleTerminateNotification(ctx, base);
    set_args(ctx, g_scratch + kRegMain, 1);
    __imp__ExRegisterTitleTerminateNotification(ctx, base);
}

void TESTDOUBLE_remove_main_registration(PPCContext& ctx, uint8_t* base) {
    set_args(ctx, g_scratch + kRegMain, 0);
    __imp__ExRegisterTitleTerminateNotification(ctx, base);
}

void TESTDOUBLE_termination_main(PPCContext& ctx, uint8_t* base) {
    __imp__KeGetCurrentProcessType(ctx, base);
    CHECK_EQ(ctx.r3.u32, kProcessTypeUser);

    set_args(ctx, g_scratch + kEvent, 0, 0);
    __imp__KeInitializeEvent(ctx, base);

    set_args(ctx, g_scratch + kRegMain, 1);
    __imp__ExRegisterTitleTerminateNotification(ctx, base);
    set_args(ctx, g_scratch + kRegRemoved, 1);
    __imp__ExRegisterTitleTerminateNotification(ctx, base);
    set_args(ctx, g_scratch + kRegRemoved, 0);
    __imp__ExRegisterTitleTerminateNotification(ctx, base);

    set_args(ctx, g_scratch + kHandle, 0x10000, 0, 0, kWorkerEntry, 0, 0);
    __imp__ExCreateThread(ctx, base);
    CHECK_EQ(ctx.r3.u32, 0u);

    for (uint32_t spin = 0; spin < 2000000 && !g_worker_registered.load(); ++spin)
        std::this_thread::yield();
    CHECK(g_worker_registered.load());

    ctx.r3.u64 = 0xDEADBEEFu;
    __imp__XamLoaderTerminateTitle(ctx, base);
    CHECK(false);  // the ordinary title-termination path does not return
}

void TESTDOUBLE_custom_terminal(PPCContext& ctx, uint8_t* base) {
    uint32_t terminal = 0;
    const Status status = request_title_termination(
        ctx, base, TitleTerminationReason::XamLoader, 0xA5A55A5Au, &terminal);
    CHECK_ST(status, Status::Ok);
    CHECK_EQ(terminal, 0xA5A55A5Au);
    exit_current_guest_thread(terminal);
}

void TESTDOUBLE_register_main(PPCContext& ctx, uint8_t* base) {
    set_args(ctx, g_scratch + kRegMain, 1);
    __imp__ExRegisterTitleTerminateNotification(ctx, base);
}

void TESTDOUBLE_register_stale(PPCContext& ctx, uint8_t* base) {
    set_args(ctx, g_scratch + kRegStale, 1);
    __imp__ExRegisterTitleTerminateNotification(ctx, base);
}

void TESTDOUBLE_hal_reboot(PPCContext& ctx, uint8_t* base) {
    ctx.r3.u64 = 1;
    __imp__HalReturnToFirmware(ctx, base);
    CHECK(false);
}

void TESTDOUBLE_launch_dashboard(PPCContext& ctx, uint8_t* base) {
    set_args(ctx, 0, 0x00000001);  // XamLoaderLaunchTitle(NULL, flags): the dashboard
    __imp__XamLoaderLaunchTitle(ctx, base);
    CHECK(false);  // it does not return
}

void TESTDOUBLE_launch_other_executable(PPCContext& ctx, uint8_t* base) {
    set_args(ctx, g_scratch + kHandle, 0);
    __imp__XamLoaderLaunchTitle(ctx, base);
}

void TESTDOUBLE_dirty_disc(PPCContext& ctx, uint8_t* base) {
    set_args(ctx, 0);
    __imp__XamShowDirtyDiscErrorUI(ctx, base);
}

void TESTDOUBLE_hal_powerdown(PPCContext& ctx, uint8_t* base) {
    ctx.r3.u64 = 5;
    __imp__HalReturnToFirmware(ctx, base);
}

Status run_guest(PPCFunc* entry, uint32_t* exit_code = nullptr) {
    GuestThread thread;
    alignas(64) PPCContext ctx;
    Status status = create_guest_thread(runtime()->heap, {0x10000, 0, 0}, &ctx, &thread);
    if (status != Status::Ok) return status;
    uint32_t code = 0;
    status = run_guest_thread(thread, ctx, g_base, entry, &code);
    const Status cleanup = destroy_guest_thread(runtime()->heap, &thread);
    if (status == Status::Ok) status = cleanup;
    if (exit_code) *exit_code = code;
    return status;
}

bool run_guest_fatal(PPCFunc* entry) {
    GuestThread thread;
    alignas(64) PPCContext ctx;
    CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0, 0}, &ctx, &thread), Status::Ok);
    uint32_t code = 0;
    bool fatal = false;
    CAPTURE_FATAL(run_guest_thread(thread, ctx, g_base, entry, &code), fatal);
    if (fatal) abandon_current_guest_thread_after_fatal();
    CHECK_ST(destroy_guest_thread(runtime()->heap, &thread), Status::Ok);
    return fatal;
}

void start_runtime() {
    CHECK_ST(runtime_init(&g_memory), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(register_xboxkrnl_process_hle(), Status::Ok);
    CHECK_ST(register_xam_loader_hle(), Status::Ok);
    g_quiesce_finished = false;
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &g_scratch), Status::Ok);
}

void stop_runtime() {
    join_quiesce();
    runtime_shutdown();
    clear_imports();
}

}  // namespace

int main() {
    CHECK_EQ(g_memory.reserve(), MemStatus::Ok);
    g_base = g_memory.base();

    const FuncEntry functions[] = {
        {kWorkerEntry, &TESTDOUBLE_worker_entry, "TESTDOUBLE_worker_entry"},
        {kMainCallback, &TESTDOUBLE_main_callback, "TESTDOUBLE_main_callback"},
        {kWorkerCallback, &TESTDOUBLE_worker_registered_callback,
         "TESTDOUBLE_worker_registered_callback"},
        {kRemovedCallback, &TESTDOUBLE_removed_callback, "TESTDOUBLE_removed_callback"},
        {0x82000500, &TESTDOUBLE_stale_callback, "TESTDOUBLE_stale_callback"},
    };
    CHECK(register_functions(functions, sizeof(functions) / sizeof(functions[0])));

    // Process identity and real per-GuestThread APC-disable nesting.
    start_runtime();
    write_registration(kRegMain, kMainCallback, 0x7C800000u);
    CHECK_ST(run_guest(&TESTDOUBLE_process_and_critical), Status::Ok);

    // The process type is meaningful only inside an actual title GuestThread.
    {
        alignas(64) PPCContext ctx{};
        bool fatal = false;
        CAPTURE_FATAL(__imp__KeGetCurrentProcessType(ctx, g_base), fatal);
        CHECK(fatal);
        CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_INTERNAL);
        CHECK(g_fatal_msg.find("active runtime title GuestThread") != std::string::npos);
    }

    CHECK(run_guest_fatal(&TESTDOUBLE_unmatched_leave));
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_GUEST_TRAP);
    CHECK(g_fatal_msg.find("without matching enter") != std::string::npos);

    CHECK(run_guest_fatal(&TESTDOUBLE_invalid_registration));
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_GUEST_ACCESS);

    CHECK(run_guest_fatal(&TESTDOUBLE_duplicate_registration));
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_GUEST_TRAP);
    CHECK(g_fatal_msg.find("duplicate create") != std::string::npos);
    CHECK_ST(run_guest(&TESTDOUBLE_remove_main_registration), Status::Ok);

    // Power-down is a real firmware enum value but intentionally has no host
    // power mapping in R-comp.
    CHECK(run_guest_fatal(&TESTDOUBLE_hal_powerdown));
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_UNIMPLEMENTED);
    stop_runtime();

    // Explicit terminal-code propagation through the lifecycle helper. The
    // production request wakes work from inside the guest, and the owner waits
    // only after the entry has unwound, with no guest-thread RAII crossed.
    start_runtime();
    uint32_t code = 0;
    CHECK_ST(run_guest(&TESTDOUBLE_custom_terminal, &code), Status::Ok);
    CHECK_EQ(code, 0xA5A55A5Au);
    join_quiesce();
    CHECK(g_quiesce_finished.load());
    TitleLifecycleSnapshot snapshot;
    CHECK_ST(title_lifecycle_snapshot(&snapshot), Status::Ok);
    CHECK_EQ(snapshot.phase, TitleLifecyclePhase::CallbacksComplete);
    CHECK_EQ(snapshot.terminal_code, 0xA5A55A5Au);
    stop_runtime();

    // A main GuestThread and a real ExCreateThread worker both register AOT
    // termination callbacks. The worker then blocks in a kernel wait. The
    // high-priority main callback runs first; the removed callback never runs.
    start_runtime();
    g_worker_registered = false;
    g_worker_returned = false;
    g_worker_wait_status = 0;
    g_callback_count = 0;
    g_removed_count = 0;
    g_order0 = g_order1 = 0;
    write_registration(kRegMain, kMainCallback, 0x7C800000u);
    write_registration(kRegWorker, kWorkerCallback, 0u);
    write_registration(kRegRemoved, kRemovedCallback, 0x100u);
    CHECK_ST(run_guest(&TESTDOUBLE_termination_main, &code), Status::Ok);
    CHECK_EQ(code, 0u);
    join_quiesce();
    CHECK(g_quiesce_finished.load());
    CHECK(g_worker_returned.load());
    CHECK_EQ(g_worker_wait_status.load(), 0xC000004Bu);
    CHECK_EQ(g_callback_count.load(), 2u);
    CHECK_EQ(g_order0.load(), 1u);
    CHECK_EQ(g_order1.load(), 2u);
    CHECK_EQ(g_removed_count.load(), 0u);
    CHECK_ST(title_lifecycle_snapshot(&snapshot), Status::Ok);
    CHECK_EQ(snapshot.phase, TitleLifecyclePhase::CallbacksComplete);
    CHECK_EQ(snapshot.reason, TitleTerminationReason::XamLoader);
    CHECK_EQ(snapshot.registered_callbacks, (size_t)0);
    stop_runtime();

    // Registration state belongs to one Runtime generation. A callback left
    // registered in a destroyed title cannot fire in the next title. Mode 1
    // takes the supported virtual-dashboard Hal path and never touches host
    // power state.
    start_runtime();
    g_stale_count = 0;
    write_registration(kRegStale, 0x82000500, 0x7C800000u);
    CHECK_ST(run_guest(&TESTDOUBLE_register_stale), Status::Ok);
    stop_runtime();

    start_runtime();
    CHECK_ST(run_guest(&TESTDOUBLE_hal_reboot, &code), Status::Ok);
    CHECK_EQ(code, 0u);
    join_quiesce();
    CHECK_EQ(g_stale_count.load(), 0u);
    CHECK_ST(title_lifecycle_snapshot(&snapshot), Status::Ok);
    CHECK_EQ(snapshot.reason, TitleTerminationReason::HalRebootToDashboard);
    stop_runtime();

    // XamLoaderLaunchTitle(NULL) returns to the dashboard: the title ends
    // through the same lifecycle, with its own reason; registered callbacks run.
    start_runtime();
    g_callback_count = 0;
    write_registration(kRegMain, kMainCallback, 0x7C800000u);
    CHECK_ST(run_guest(&TESTDOUBLE_register_main), Status::Ok);
    CHECK_ST(run_guest(&TESTDOUBLE_launch_dashboard, &code), Status::Ok);
    CHECK_EQ(code, 0u);
    join_quiesce();
    CHECK_EQ(g_callback_count.load(), 1u);
    CHECK_ST(title_lifecycle_snapshot(&snapshot), Status::Ok);
    CHECK_EQ(snapshot.phase, TitleLifecyclePhase::CallbacksComplete);
    CHECK_EQ(snapshot.reason, TitleTerminationReason::XamLoaderLaunchDashboard);
    stop_runtime();

    // Launching another executable and the dirty-disc screen end in explicit
    // diagnostics, never in a return that pretends the work was done.
    start_runtime();
    CHECK(guest_write_be32(g_scratch + kHandle, 0x67616D65u));  // "game"
    CHECK(run_guest_fatal(&TESTDOUBLE_launch_other_executable));
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(g_fatal_msg.find("XamLoaderLaunchTitle") != std::string::npos);
    CHECK_ST(title_lifecycle_snapshot(&snapshot), Status::Ok);
    CHECK_EQ(snapshot.phase, TitleLifecyclePhase::Running);
    CHECK(run_guest_fatal(&TESTDOUBLE_dirty_disc));
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_GUEST_TRAP);
    CHECK(g_fatal_msg.find("disc read error") != std::string::npos);
    stop_runtime();

    clear_functions();
    return test_result("rt_process_lifecycle");
}
