// Kernel video exports (runtime/src/hle_xboxkrnl_video.cpp) and physical
// memory (hle_xboxkrnl_memory.cpp) driving the Xenos command processor
// (gpu/xenos/rexglue) the way a title's Direct3D does, through the same
// __imp__ symbols generated code calls. Rendering goes to the recording
// TESTDOUBLE backend. The graphics interrupt callback is a guest function
// (registered in the function table) run by the runtime's dispatcher.
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "../../gpu/xenos/rexglue/rcomp/xenos_host.h"
#include "TESTDOUBLE_recording_backend.h"
#include "rcomp/diag.h"
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/video.h"

PPC_EXTERN_FUNC(__imp__MmAllocatePhysicalMemoryEx);
PPC_EXTERN_FUNC(__imp__MmGetPhysicalAddress);
PPC_EXTERN_FUNC(__imp__VdQueryVideoMode);
PPC_EXTERN_FUNC(__imp__VdQueryVideoFlags);
PPC_EXTERN_FUNC(__imp__VdGetCurrentDisplayInformation);
PPC_EXTERN_FUNC(__imp__VdInitializeEngines);
PPC_EXTERN_FUNC(__imp__VdSetGraphicsInterruptCallback);
PPC_EXTERN_FUNC(__imp__VdInitializeRingBuffer);
PPC_EXTERN_FUNC(__imp__VdEnableRingBufferRPtrWriteBack);
PPC_EXTERN_FUNC(__imp__VdSwap);
PPC_EXTERN_FUNC(__imp__VdInitializeScalerCommandBuffer);
PPC_EXTERN_FUNC(__imp__VdGetSystemCommandBuffer);

using namespace rcomp;
using namespace rcomp::rt;
using namespace rcomp::xenos;

namespace {

int g_fail = 0, g_pass = 0;
FILE* g_out = stdout;
void check(const char* id, bool ok, const char* why = "") {
    fprintf(g_out, "%-52s %s%s%s\n", id, ok ? "PASS" : "FAIL", ok ? "" : " ", ok ? "" : why);
    fflush(g_out);
    g_fail += !ok;
    g_pass += ok;
}

GuestMemory g_mem;
uint8_t* g_base;
alignas(64) PPCContext g_ctx;  // the "title" thread's context
GuestThread g_thread;
uint32_t g_scratch;

constexpr uint32_t kIrqCallback = 0x82001000;
constexpr uint32_t RW = 0x04, WC = 0x400, LARGE = 0x20000000;
constexpr uint32_t kWidth = 1280, kHeight = 720;

uint32_t rd32(uint32_t a) {
    uint32_t v = 0;
    guest_read_be32(a, &v);
    return v;
}
void wr32(uint32_t a, uint32_t v) { guest_write_be32(a, v); }
float rdf(uint32_t a) {
    uint32_t v = rd32(a);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
uint16_t rd16(uint32_t a) {
    uint16_t v = 0;
    guest_read_be16(a, &v);
    return v;
}

// Calls an import like generated code: arguments in r3.., the rest of the
// context is the title thread's (r1 = its guest stack for stack arguments).
uint32_t call(PPCFunc* f, std::initializer_list<uint32_t> args) {
    PPCContext& c = g_ctx;
    uint64_t* regs[] = {&c.r3.u64, &c.r4.u64, &c.r5.u64, &c.r6.u64, &c.r7.u64, &c.r8.u64, &c.r9.u64, &c.r10.u64};
    size_t i = 0;
    for (uint32_t a : args) {
        if (i < 8) *regs[i] = a;
        else {  // 8-byte parameter slots at r1 + 0x50, value in the low (second) word
            wr32(c.r1.u32 + 0x50 + 8 * uint32_t(i - 8), 0);
            wr32(c.r1.u32 + 0x54 + 8 * uint32_t(i - 8), a);
        }
        ++i;
    }
    f(c, g_base);
    return c.r3.u32;
}

// Guest graphics interrupt callback: (source, user_data). Proves it runs as
// guest code on a guest stack: writes through `base` to user_data.
std::atomic<int> g_irq_calls{0}, g_irq_vblank{0}, g_irq_cmd{0};
std::atomic<bool> g_irq_env_ok{true};
void irq_callback(PPCContext& ctx, uint8_t* base) {
    const uint32_t source = ctx.r3.u32, user = ctx.r4.u32;
    if (!ctx.r1.u32 || !ctx.r13.u32 || base != g_base) g_irq_env_ok = false;
    uint32_t v;
    memcpy(&v, base + user, 4);
    v = __builtin_bswap32(__builtin_bswap32(v) + 1);
    memcpy(base + user, &v, 4);
    ++g_irq_calls;
    (source == 0 ? g_irq_vblank : g_irq_cmd)++;
}

// Fatal capture: the hook longjmps back instead of exiting.
jmp_buf g_fatal_jb;
int g_fatal_kind = 0;
void fatal_hook(enum rcomp_fatal_kind kind, const char*) {
    g_fatal_kind = (int)kind;
    longjmp(g_fatal_jb, 1);
}
template <typename F>
int fatal_kind_of(F f) {
    g_fatal_kind = 0;
    rcomp_set_fatal_hook(fatal_hook);
    if (setjmp(g_fatal_jb) == 0) f();
    rcomp_set_fatal_hook(nullptr);
    return g_fatal_kind;
}

TESTDOUBLE_RecordingBackend* g_backend = nullptr;
BackendFactory recording_factory() {
    return [](rex::graphics::GpuHost* h) {
        auto b = std::make_unique<TESTDOUBLE_RecordingBackend>(h);
        g_backend = b.get();
        return std::unique_ptr<rex::graphics::CommandProcessor>(std::move(b));
    };
}

bool wait_for(const std::function<bool()>& cond, int ms = 2000) {
    for (int i = 0; i < ms; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return cond();
}

void kick(uint32_t wptr_dwords) {
    const uint32_t v = __builtin_bswap32(wptr_dwords);
    __atomic_store_n(reinterpret_cast<uint32_t*>(g_base + kXenosRegisterWindow + 4 * kRegCpRbWptr), v, __ATOMIC_RELEASE);
}

int run_all() {
    if (g_mem.reserve() != MemStatus::Ok) return 2;
    g_base = g_mem.base();
    const bool rt_ok = runtime_init(&g_mem) == Status::Ok && register_xboxkrnl_hle() == Status::Ok;
    FuncEntry fe{kIrqCallback, &irq_callback, "irq_callback"};
    const bool fn_ok = register_functions(&fe, 1);
    Runtime& r = *runtime();
    const bool th_ok = create_guest_thread(r.heap, {0x40000, 0, 0}, &g_ctx, &g_thread) == Status::Ok &&
                       r.heap.alloc(0x1000, 16, true, &g_scratch) == Status::Ok;
    HostConfig cfg;
    cfg.vblank_hz = 0;  // command-stream part first, deterministic
    const bool gpu_ok = gpu_start(g_mem, cfg, dispatch_graphics_interrupt, recording_factory());
    const bool vd_ok = gpu_ok && register_xboxkrnl_video_hle() == Status::Ok;
    check("xenos_video/start", rt_ok && fn_ok && th_ok && gpu_ok && vd_ok);
    if (!(rt_ok && fn_ok && th_ok && vd_ok)) return 1;

    // Variables.
    uint32_t clk = 0, lock = 0, dev = 0;
    const bool vars = find_variable_import(kModuleXboxkrnl, 0x01C0, &clk) &&
                      find_variable_import(kModuleXboxkrnl, 0x01C1, &lock) &&
                      find_variable_import(kModuleXboxkrnl, 0x01BE, &dev);
    check("xenos_video/variables",
          vars && rd32(clk) == 500 && rd32(dev) == 0 && rd32(lock + 0x10) == 0xFFFFFFFFu && g_base[lock] == 1);

    // Display description.
    const uint32_t mode = g_scratch + 0x100, info = g_scratch + 0x200;
    call(__imp__VdQueryVideoMode, {mode});
    check("xenos_video/query_video_mode", rd32(mode) == kWidth && rd32(mode + 4) == kHeight && rd32(mode + 8) == 0 &&
                                              rd32(mode + 0xC) == 1 && rd32(mode + 0x10) == 1 &&
                                              rdf(mode + 0x14) == 60.0f && rd32(mode + 0x18) == 1);
    check("xenos_video/query_video_flags", call(__imp__VdQueryVideoFlags, {}) == 3);
    call(__imp__VdGetCurrentDisplayInformation, {info});
    check("xenos_video/display_information", rd16(info) == kWidth && rd16(info + 2) == kHeight &&
                                                 rd16(info + 0x48) == kWidth && rd16(info + 0x4A) == kHeight &&
                                                 rdf(info + 0x4C) == 60.0f && rd32(info + 0x18) == kWidth);
    check("xenos_video/initialize_engines", call(__imp__VdInitializeEngines, {0, 0, 0, 0, 0}) == 1);

    // Ring buffer, read pointer write-back and interrupt callback, as D3D
    // sets them up: physical allocations, MmGetPhysicalAddress.
    const uint32_t ring = call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x10000, RW | WC | LARGE, 0, 0xFFFFFFFF, 0});
    const uint32_t wb = call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x1000, RW, 0, 0xFFFFFFFF, 0});
    const uint32_t fb = call(__imp__MmAllocatePhysicalMemoryEx, {0, kWidth * kHeight * 4, RW | WC | LARGE, 0, 0xFFFFFFFF, 0x1000});
    const uint32_t ring_phys = call(__imp__MmGetPhysicalAddress, {ring});
    check("xenos_video/physical_allocations", ring && wb && fb && ring_phys == ring - kXenosPhysicalWindow);
    call(__imp__VdInitializeRingBuffer, {ring_phys, 13});  // 64 KiB
    call(__imp__VdEnableRingBufferRPtrWriteBack, {wb, 6});  // virtual address, as D3D passes it
    const uint32_t counter = g_scratch + 0x300;
    call(__imp__VdSetGraphicsInterruptCallback, {kIrqCallback, counter});

    // Command stream: an INTERRUPT packet, then the VdSwap block.
    uint32_t w = 0;
    auto put = [&](uint32_t v) { wr32(ring + 4 * w++, v); };
    put((3u << 30) | (0u << 16) | (0x54u << 8));  // INTERRUPT, 1 dword
    put(0x4);                                      // cpu 2
    // D3D's front buffer texture fetch constant (virtual base address).
    const uint32_t fetch = g_scratch + 0x400;
    wr32(fetch + 0, 0x2);  // type texture
    wr32(fetch + 4, (fb & 0xFFFFF000u) | 6);  // k_8_8_8_8
    wr32(fetch + 8, (kWidth - 1) | ((kHeight - 1) << 13));
    for (uint32_t i = 3; i < 6; ++i) wr32(fetch + 4 * i, 0);
    const uint32_t args = g_scratch + 0x500;
    wr32(args + 0, fb);
    wr32(args + 4, 6);
    wr32(args + 8, 0);
    wr32(args + 12, kWidth);
    wr32(args + 16, kHeight);
    const uint32_t swap_at = ring + 4 * w;
    call(__imp__VdSwap, {swap_at, fetch, 0, 0, 0, args, args + 4, args + 8, args + 12, args + 16});
    const bool packet_ok = rd32(swap_at) == ((5u << 16) | 0x4800) && rd32(swap_at + 8) == (fb - kXenosPhysicalWindow) + 6 &&
                           rd32(swap_at + 28) == ((3u << 30) | (3u << 16) | (0x64u << 8)) &&
                           rd32(swap_at + 32) == 0x53574150 && rd32(swap_at + 36) == fb - kXenosPhysicalWindow &&
                           rd32(swap_at + 252) == 0x80000000u;
    check("xenos_video/vd_swap_packet", packet_ok);
    w += 64;
    kick(w);
    const bool consumed = wait_for([&] { return rd32(wb) == w; });
    check("xenos_video/ring_consumed_via_vd_setup", consumed);
    bool swapped = false;
    {
        std::lock_guard<std::mutex> lk(g_backend->mu);
        swapped = g_backend->swaps.size() == 1 && g_backend->swaps[0].frontbuffer == fb - kXenosPhysicalWindow &&
                  g_backend->swaps[0].width == kWidth && g_backend->swaps[0].height == kHeight;
    }
    check("xenos_video/swap_reaches_backend", swapped);
    const bool irq = wait_for([] { return g_irq_cmd.load() == 1; });
    check("xenos_video/interrupt_runs_guest_callback", irq && g_irq_env_ok && rd32(counter) == 1);

    // Scaler command buffer: NOPs, count returned (stack arguments 11, 12).
    const uint32_t scaler = g_scratch + 0x800;
    const uint32_t n = call(__imp__VdInitializeScalerCommandBuffer, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, scaler, 16});
    check("xenos_video/scaler_command_buffer_nops", n == 16 && rd32(scaler) == 0x80000000u && rd32(scaler + 60) == 0x80000000u);

    // Out of the implemented subset: a 16-bit front buffer format traps;
    // an unimplemented export traps as a missing import.
    wr32(args + 4, 1);
    const int bad_format = fatal_kind_of(
        [&] { call(__imp__VdSwap, {swap_at, fetch, 0, 0, 0, args, args + 4, args + 8, args + 12, args + 16}); });
    wr32(args + 4, 6);
    const int missing = fatal_kind_of([&] { call(__imp__VdGetSystemCommandBuffer, {g_scratch + 0xC00, g_scratch + 0xD00}); });
    check("xenos_video/outside_subset_is_fatal",
          bad_format == RCOMP_FATAL_UNIMPLEMENTED && missing == RCOMP_FATAL_MISSING_IMPORT);

    gpu_stop();

    // vblank: restart the GPU with a 200 Hz timer; the callback runs with
    // source 0 on the bridge thread (a new host thread, new guest stack).
    cfg.vblank_hz = 200;
    const bool restarted = gpu_start(g_mem, cfg, dispatch_graphics_interrupt, recording_factory());
    if (restarted) call(__imp__VdSetGraphicsInterruptCallback, {kIrqCallback, counter});
    const int before = g_irq_vblank.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int vblanks = g_irq_vblank.load() - before;
    gpu_stop();
    check("xenos_video/vblank_runs_guest_callback", restarted && vblanks >= 30 && vblanks <= 60 && g_irq_env_ok);
    check("xenos_video/guest_stacks_released", r.heap.stats().live_allocations == 5);  // title thread (3) + scratch + vars

    fprintf(g_out, "RCOMP-XENOS-VIDEO pass=%d fail=%d\n", g_pass, g_fail);
    fflush(g_out);
    return g_fail ? 1 : 0;
}

}  // namespace

#ifdef RCOMP_HARNESS_LIBRARY
extern "C" int rcomp_xenos_video_selftest(FILE* out) {
    g_out = out;
    return run_all();
}
#else
int main() { return run_all(); }
#endif
