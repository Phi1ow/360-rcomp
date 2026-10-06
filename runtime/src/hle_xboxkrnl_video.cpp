#include <stdio.h>
#include "physical_window.h"
#include "diagnostics.h"
#include <time.h>
// xboxkrnl.exe HLE: video (Vd*) over the emulated Xenos GPU
// (owner: Agent 3, runtime/; library rcomp_runtime_video).
//
// Same rules as hle_xboxkrnl.cpp: each export states the Xbox 360 semantics
// and the implemented subset; anything outside it ends in
// RCOMP_FATAL_UNIMPLEMENTED, never in a fake success.
//
// On the Xbox 360, Direct3D is a static library inside the XEX: it writes
// PM4 packets into a ring buffer in physical memory and moves the GPU's
// write pointer (register CP_RB_WPTR). The kernel only sets the GPU up
// (ring buffer, read-pointer write-back, interrupt callback), describes the
// display, and builds the swap packet (VdSwap). The GPU itself is
// gpu/xenos (include/rcomp/xenos_gpu.h).
//
// Semantics are taken from public research as recorded in the pinned
// rexglue-sdk/Xenia sources (BSD-3, consulted, not copied): packet formats,
// structure layouts, the constant values titles are known to accept.
//
// Deliberately NOT registered (trap as missing imports until a title needs
// them and their semantics are known): VdGetSystemCommandBuffer,
// VdSetSystemCommandBufferGpuIdentifierAddress,
// VdCallGraphicsNotificationRoutines, VdPersistDisplay, VdInitializeEDRAM,
// VdEnumerateVideoModes and the HDCP / closed caption / WSS / CGMS family.
#include "rcomp/runtime/video.h"

#include <pthread.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <new>
#include <thread>

#include "rcomp/diag.h"
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/xenos_gpu.h"
#include "swap_pacing.h"

namespace rcomp::rt {
extern "C" void rcomp_unregister_thread_ctx(uint32_t, void*) __attribute__((weak));

namespace {

namespace xg = rcomp::xenos;

// PM4 (Xenos command processor) encodings.
constexpr uint32_t pm4_type0(uint32_t reg, uint32_t count) { return ((count - 1) << 16) | reg; }
constexpr uint32_t pm4_type3(uint32_t opcode, uint32_t count) { return (3u << 30) | ((count - 1) << 16) | (opcode << 8); }
constexpr uint32_t kPm4Type2Nop = 2u << 30;
constexpr uint32_t kPm4XeSwap = 0x64;          // swap token understood by gpu/xenos
constexpr uint32_t kSwapSignature = 0x53574150;  // 'SWAP'
constexpr uint32_t kRegFetchConstant0 = 0x4800;  // SHADER_CONSTANT_FETCH_00_0
constexpr uint32_t kSwapBufferDwords = 64;       // space D3D reserves in the ring for VdSwap
// Front buffer texture formats (Xenos TextureFormat).
constexpr uint32_t kTex8888 = 6, kTex2101010As16161616 = 54;

Runtime& rt_or_die(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s called before rcomp::rt::runtime_init", fn);
    return *r;
}

[[noreturn]] void unimplemented(const char* fn, PPCContext& ctx, const char* what, uint32_t v) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xboxkrnl.exe!%s %s=0x%08X lr=0x%08X (not implemented)", fn, what, v,
                (uint32_t)ctx.lr);
}

void gpu_or_die(const char* fn) {
    if (!xg::gpu_running())
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xboxkrnl.exe!%s: the GPU was not started (title glue must call gpu_start)",
                    fn);
}

uint8_t* guest_or_die(uint32_t addr, uint32_t size, const char* fn, PPCContext& ctx) {
    uint8_t* p = rt_or_die(fn).mem->translate(addr, size);
    if (!p)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s: 0x%08X+%u not committed lr=0x%08X", fn, addr, size,
                    (uint32_t)ctx.lr);
    return p;
}
void put32(uint8_t* p, uint32_t v) {
    v = __builtin_bswap32(v);
    memcpy(p, &v, 4);
}
void put16(uint8_t* p, uint16_t v) {
    v = __builtin_bswap16(v);
    memcpy(p, &v, 2);
}
void putf(uint8_t* p, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    put32(p, v);
}
uint32_t get32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

uint32_t stack_arg(PPCContext& ctx, int n, const char* fn) {
    uint32_t v = 0;
    const uint32_t addr = ctx.r1.u32 + 0x54 + 8u * (uint32_t)(n - 8);
    if (!guest_read_be32(addr, &v))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s: stack argument %d at 0x%08X not readable", fn, n, addr);
    return v;
}

std::atomic<uint32_t> g_display_mode_flags{0};
std::atomic<uint64_t> g_video_swaps{0};
SwapPacing g_swap_pacing;

// Launch-option frame-rate cap (video_set_frame_rate_cap). The schedule is only touched by VdSwap.
std::atomic<uint32_t> g_frame_rate_cap{0};
std::mutex g_frame_cap_mutex;
bool g_frame_cap_started = false;
std::chrono::steady_clock::time_point g_frame_cap_next;

// Holds the swapping thread until the next slot of the cap's schedule, as a vsync wait would.
void wait_for_frame_cap() {
    const uint32_t fps = g_frame_rate_cap.load(std::memory_order_relaxed);
    if (fps == 0) return;
    const auto period = std::chrono::nanoseconds(1000000000ull / fps);
    std::lock_guard<std::mutex> lock(g_frame_cap_mutex);
    auto now = std::chrono::steady_clock::now();
    if (!g_frame_cap_started || now - g_frame_cap_next > period) {
        g_frame_cap_started = true;  // first swap, or more than one period late: restart from now
        g_frame_cap_next = now;
    } else if (now < g_frame_cap_next) {
        std::this_thread::sleep_until(g_frame_cap_next);
    }
    g_frame_cap_next += period;
}
std::mutex g_video_generation_mutex;
uint64_t g_video_generation = 0;

// ---- VdQueryVideoMode (0x01CA) -------------------------------------------
// Xbox 360: VOID VdQueryVideoMode(PX_VIDEO_MODE). X_VIDEO_MODE (48 bytes,
// big-endian): +0 display_width, +4 display_height, +8 is_interlaced,
// +0xC is_widescreen, +0x10 is_hi_def, +0x14 float refresh_rate,
// +0x18 video_standard (1 = NTSC), +0x1C 0x4A, +0x20 1, +0x24 reserved[3].
// Implemented: the mode of the emulated display (gpu_display_mode()),
// progressive; the two constant words are the values the console reports.
void fill_video_mode(uint8_t* m) {
    const xg::DisplayMode d = xg::gpu_display_mode();
    memset(m, 0, 48);
    put32(m + 0x00, d.width);
    put32(m + 0x04, d.height);
    put32(m + 0x08, 0);
    put32(m + 0x0C, d.width * 3 >= d.height * 4 ? 1 : 0);
    put32(m + 0x10, (d.width >= 1280 || d.height >= 720) ? 1 : 0);
    putf(m + 0x14, d.refresh_hz);
    put32(m + 0x18, 1);
    put32(m + 0x1C, 0x4A);
    put32(m + 0x20, 0x01);
}

void VdQueryVideoMode(PPCContext& ctx, uint8_t*) {
    const char* fn = "VdQueryVideoMode";
    gpu_or_die(fn);
    fill_video_mode(guest_or_die(ctx.r3.u32, 48, fn, ctx));
}

// ---- VdQueryVideoFlags (0x01C9) ------------------------------------------
// Xbox 360: DWORD VdQueryVideoFlags(): 1 = widescreen, 2 = width >= 1024,
// 4 = width >= 1920. Implemented from the display mode.
void VdQueryVideoFlags(PPCContext& ctx, uint8_t*) {
    gpu_or_die("VdQueryVideoFlags");
    const xg::DisplayMode d = xg::gpu_display_mode();
    ctx.r3.u64 = (d.width * 3 >= d.height * 4 ? 1u : 0u) | (d.width >= 1024 ? 2u : 0u) | (d.width >= 1920 ? 4u : 0u);
}

// ---- VdGetCurrentDisplayInformation (0x01BA) -----------------------------
// Xbox 360: VOID VdGetCurrentDisplayInformation(PX_DISPLAY_INFO). 0x58 bytes:
// +0 u16 front buffer width, +2 height, +4 u8 colour format, +5 u8 pixel
// format, +8 scaler parameters {rect x1,y1,x2,y2; u32 scaled width, height;
// u32 vertical filter type; 3 floats; u32 horizontal filter type; 3
// floats}, +0x40 u16 overscan left/top/right/bottom, +0x48 u16 display
// width, +0x4A height, +0x4C float refresh, +0x50 u32 interlaced, +0x54 u8
// colour format, +0x56 u16 actual display width.
// Implemented: front buffer = display = gpu_display_mode(), identity
// scaler (source rect = full frame, filter type 1), overscan = a quarter of
// each dimension (the values Xenia reports and titles accept), formats 0.
void VdGetCurrentDisplayInformation(PPCContext& ctx, uint8_t*) {
    const char* fn = "VdGetCurrentDisplayInformation";
    gpu_or_die(fn);
    uint8_t* p = guest_or_die(ctx.r3.u32, 0x58, fn, ctx);
    const xg::DisplayMode d = xg::gpu_display_mode();
    const uint16_t w = (uint16_t)d.width, h = (uint16_t)d.height;
    memset(p, 0, 0x58);
    put16(p + 0x00, w);
    put16(p + 0x02, h);
    put32(p + 0x10, w);  // scaler source rect x2
    put32(p + 0x14, h);  // y2
    put32(p + 0x18, w);  // scaled output
    put32(p + 0x1C, h);
    put32(p + 0x20, 1);  // vertical filter type
    put32(p + 0x30, 1);  // horizontal filter type
    put16(p + 0x40, w / 4);
    put16(p + 0x42, h / 4);
    put16(p + 0x44, w / 4);
    put16(p + 0x46, h / 4);
    put16(p + 0x48, w);
    put16(p + 0x4A, h);
    putf(p + 0x4C, d.refresh_hz);
    put16(p + 0x56, w);
}

// ---- VdGetCurrentDisplayGamma (0x01B9) -----------------------------------
// Xbox 360: VOID VdGetCurrentDisplayGamma(PDWORD Type, PFLOAT Power): the
// display's transfer curve, used by D3D gamma ramps. 1 = sRGB, 2 = BT.709,
// 3 = power curve with *Power. Implemented: 2 (BT.709, HDTV output) and
// power 2.2222223.
void VdGetCurrentDisplayGamma(PPCContext& ctx, uint8_t*) {
    const char* fn = "VdGetCurrentDisplayGamma";
    put32(guest_or_die(ctx.r3.u32, 4, fn, ctx), 2);
    putf(guest_or_die(ctx.r4.u32, 4, fn, ctx), 2.22222233f);
}

// ---- VdSetDisplayMode (0x01D3) -------------------------------------------
// Xbox 360: DWORD VdSetDisplayMode(DWORD Flags): front buffer description
// (0x2 = SD resolution, 0x08000000 = 10-bit front buffer, 0x30000000 =
// colour space, 0x40000000 always set). The display output of this runtime
// has a fixed mode (the presenter converts the front buffer VdSwap names),
// so the flags are only recorded. Returns 0 (success). Unknown bits trap.
void VdSetDisplayMode(PPCContext& ctx, uint8_t*) {
    const uint32_t flags = ctx.r3.u32;
    if (flags & ~0x78000002u) unimplemented("VdSetDisplayMode", ctx, "flags", flags);
    g_display_mode_flags = flags;
    ctx.r3.u64 = 0;
}

// ---- VdInitializeEngines (0x01C2) / VdShutdownEngines (0x01DC) ----------
// Xbox 360: BOOL VdInitializeEngines(DWORD, PVOID callback, PVOID arg,
// PDWORD PfpMicrocode, PDWORD MeMicrocode): resets the command processor and
// loads its microcode. The command processor of gpu/xenos interprets PM4 at
// packet level and needs no microcode; it is running once gpu_start
// succeeded, which this checks. Returns TRUE. VdShutdownEngines: titles
// pair it with a later re-initialisation; the engine stays running (no
// state is lost by a CP reset that is immediately followed by an init).
void VdInitializeEngines(PPCContext& ctx, uint8_t*) {
    gpu_or_die("VdInitializeEngines");
    ctx.r3.u64 = 1;
}
void VdShutdownEngines(PPCContext&, uint8_t*) { gpu_or_die("VdShutdownEngines"); }

// ---- VdGetGraphicsAsicID (0x01BC) ----------------------------------------
// Xbox 360: DWORD VdGetGraphicsAsicID(): GPU revision. Titles compare it
// with 0x10 to pick the EDRAM training path. Implemented: 0x11 (a revision
// whose EDRAM needs no VdInitializeEDRAM, which is not implemented).
void VdGetGraphicsAsicID(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0x11; }

// ---- link / clock management with no emulated counterpart ----------------
// VdEnableDisableClockGating (0x01B4): GPU clock gating (power only).
// VdIsHSIOTrainingSucceeded (0x01C6): state of the CPU-GPU high speed link.
// VdRetrainEDRAM (0x0269), VdRetrainEDRAMWorker (0x026A): EDRAM link
// training. The emulated GPU has no clocks and no links: gating has no
// effect, the link is always trained (TRUE), retraining completes at once
// (0 = success).
void VdEnableDisableClockGating(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }
void VdIsHSIOTrainingSucceeded(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 1; }
void VdRetrainEDRAM(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }
void VdRetrainEDRAMWorker(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }

// ---- VdSetGraphicsInterruptCallback (0x01D5) -----------------------------
// Xbox 360: VOID VdSetGraphicsInterruptCallback(PVOID Callback, PVOID
// UserData): the GPU calls Callback(source, UserData) on vblank (source 0)
// and on command-stream interrupts (source 1). Implemented through the GPU
// and dispatch_graphics_interrupt(); Callback 0 removes it.
void VdSetGraphicsInterruptCallback(PPCContext& ctx, uint8_t*) {
    gpu_or_die("VdSetGraphicsInterruptCallback");
    const uint32_t cb = ctx.r3.u32;
    if (cb && !lookup_function(cb))
        rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET,
                    "xboxkrnl.exe!VdSetGraphicsInterruptCallback 0x%08X has no recompiled function lr=0x%08X", cb,
                    (uint32_t)ctx.lr);
    xg::gpu_set_interrupt_callback(cb, ctx.r4.u32);
}

// ---- system command buffer / persisted display ---------------------------
// R-comp runs no system command buffer (the kernel-owned PM4 stream that
// programs the scaler and posts GPU identifiers) and persists no display
// across titles; these exports report exactly that.
//
// VdGetSystemCommandBuffer (0x01BD): (PVOID Buffer, PDWORD Identifier). The
// 0x94-byte buffer is zeroed and its first word plus *Identifier receive the
// opaque tokens the public rexglue-sdk c94f5eb reference publishes; the title
// passes them back untouched to VdSwap, which ignores them.
void VdGetSystemCommandBuffer(PPCContext& ctx, uint8_t*) {
    const char* fn = "VdGetSystemCommandBuffer";
    const uint32_t buffer = ctx.r3.u32, identifier = ctx.r4.u32;
    guest_or_die(buffer, 0x94, fn, ctx);
    guest_or_die(identifier, 4, fn, ctx);
    for (uint32_t i = 0; i < 0x94; i += 4) guest_write_be32(buffer + i, 0);
    guest_write_be32(buffer, 0xBEEF0000u);
    guest_write_be32(identifier, 0xBEEF0001u);
}

// VdSetSystemCommandBufferGpuIdentifierAddress (0x01D9): where the system
// command buffer would post the GPU identifier. With no system command
// buffer the word is never written; the address is only validated (0 clears).
void VdSetSystemCommandBufferGpuIdentifierAddress(PPCContext& ctx, uint8_t*) {
    if (ctx.r3.u32) guest_or_die(ctx.r3.u32, 4, "VdSetSystemCommandBufferGpuIdentifierAddress", ctx);
}

// VdCallGraphicsNotificationRoutines (0x01B1): (DWORD Kind, PVOID Args)
// calls kernel-registered graphics notification routines. R-comp registers
// none (the title only receives VdSetGraphicsInterruptCallback), so there is
// nothing to call and the result is 0. Kind 1 is the only documented value.
void VdCallGraphicsNotificationRoutines(PPCContext& ctx, uint8_t*) {
    if (ctx.r3.u32 != 1) unimplemented("VdCallGraphicsNotificationRoutines", ctx, "kind", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

// VdPersistDisplay (0x01C7): (DWORD, PVOID* Out) keeps the last frame on
// screen across a title exit and hands back a physical block to free. R-comp
// does not persist a display: returns FALSE and leaves *Out untouched.
void VdPersistDisplay(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }

// ---- VdInitializeRingBuffer (0x01C3) -------------------------------------
// Xbox 360: VOID VdInitializeRingBuffer(ULONG_PTR PhysicalAddress, DWORD
// SizeLog2): primary ring buffer of 1 << (SizeLog2 + 3) bytes at a physical
// address (from MmGetPhysicalAddress). Implemented: the range must be
// committed physical memory; handed to the command processor.
void VdInitializeRingBuffer(PPCContext& ctx, uint8_t*) {
    const char* fn = "VdInitializeRingBuffer";
    gpu_or_die(fn);
    const uint32_t phys = ctx.r3.u32, log2 = ctx.r4.u32;
    if (log2 > 25 || phys >= xg::kXenosPhysicalSize) unimplemented(fn, ctx, "ring", phys);
    guest_or_die(xg::kXenosPhysicalWindow + phys, 1u << (log2 + 3), fn, ctx);
    xg::gpu_initialize_ring_buffer(phys, log2);
}

// ---- VdEnableRingBufferRPtrWriteBack (0x01B6) ----------------------------
// Xbox 360: VOID VdEnableRingBufferRPtrWriteBack(PVOID Address, DWORD
// BlockSizeLog2): the GPU writes its read pointer (in dwords) to Address
// after every 1 << BlockSizeLog2 dwords consumed. Implemented: Address is a
// physical-window virtual address or a physical address (the GPU masks it
// to 512 MiB, like the hardware); the word must be committed.
void VdEnableRingBufferRPtrWriteBack(PPCContext& ctx, uint8_t*) {
    const char* fn = "VdEnableRingBufferRPtrWriteBack";
    gpu_or_die(fn);
    const uint32_t addr = ctx.r3.u32, log2 = ctx.r4.u32;
    const uint32_t phys = physical_of_window(*runtime()->mem, addr);
    if (addr >= xg::kXenosPhysicalSize && addr < xg::kXenosPhysicalWindow)  // any of the three windows
        unimplemented(fn, ctx, "address_outside_physical_window", addr);
    if (log2 > 19) unimplemented(fn, ctx, "block_size_log2", log2);
    guest_or_die(xg::kXenosPhysicalWindow + phys, 4, fn, ctx);
    xg::gpu_enable_read_pointer_writeback(phys, log2);
}

// ---- VdInitializeScalerCommandBuffer (0x01C5) ----------------------------
// Xbox 360: DWORD VdInitializeScalerCommandBuffer(source xy, source wh,
// output xy, output wh, front buffer wh, vertical filter type, PFILTER
// vertical params, horizontal filter type, PFILTER horizontal params,
// PVOID, PDWORD Dest, DWORD DestCount): writes the PM4 that programs the
// display scaler into Dest and returns the dwords written. The display
// scaler is not part of the emulated GPU (the presenter scales the front
// buffer), so the buffer is filled with type-2 NOP packets the command
// processor skips; returns DestCount.
void VdInitializeScalerCommandBuffer(PPCContext& ctx, uint8_t*) {
    const char* fn = "VdInitializeScalerCommandBuffer";
    const uint32_t dest = stack_arg(ctx, 10, fn), count = stack_arg(ctx, 11, fn);
    if (count > 0x10000) unimplemented(fn, ctx, "dest_count", count);
    uint8_t* p = count ? guest_or_die(dest, count * 4, fn, ctx) : nullptr;
    for (uint32_t i = 0; i < count; ++i) put32(p + 4 * i, kPm4Type2Nop);
    ctx.r3.u64 = count;
}

// ---- VdSwap (0x025B) -----------------------------------------------------
// Xbox 360: VOID VdSwap(PDWORD RingSpace, PVOID FetchConstant, PVOID,
// PVOID, PVOID, PDWORD FrontBuffer, PDWORD TextureFormat, PDWORD ColorSpace,
// PDWORD Width, PDWORD Height). Direct3D reserves 64 dwords in the ring at
// RingSpace and asks the kernel to fill them with the commands that present
// the front buffer; FetchConstant is the 6-dword texture fetch constant of
// the front buffer from the D3D texture header (its base address is a
// virtual address).
// Implemented: the 64 dwords become {type-0 write of the fetch constant,
// with the physical base, to SHADER_CONSTANT_FETCH_00; XE_SWAP 'SWAP'
// physical base, width, height; type-2 NOPs}, the packet gpu/xenos turns
// into a present. Checked (trap otherwise): front buffer in the physical
// window and equal to *FrontBuffer, format k_8_8_8_8 or
// k_2_10_10_10_AS_16_16_16_16, colour space 0 (RGB), Width/Height equal to
// the fetch constant's size.
void VdSwap(PPCContext& ctx, uint8_t*) {
    const char* fn = "VdSwap";
    gpu_or_die(fn);
    wait_for_frame_cap();
    {   // bring-up tracing: title frame rate (one line per 30 swaps)
        const uint64_t n = g_video_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
        g_swap_pacing.swap();
        if (n == 1 || n % 30 == 0) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            fprintf(stderr, "RCOMP-VD swap #%llu at %llu ms\n", (unsigned long long)n, (unsigned long long)(uint64_t(now.tv_sec) * 1000u + uint64_t(now.tv_nsec) / 1000000u));
        }
    }
    uint8_t* ring = guest_or_die(ctx.r3.u32, kSwapBufferDwords * 4, fn, ctx);
    const uint8_t* fp = guest_or_die(ctx.r4.u32, 24, fn, ctx);
    const uint32_t front_ptr = get32(guest_or_die(ctx.r8.u32, 4, fn, ctx));
    const uint32_t format = get32(guest_or_die(ctx.r9.u32, 4, fn, ctx));
    const uint32_t color_space = get32(guest_or_die(ctx.r10.u32, 4, fn, ctx));
    const uint32_t width = get32(guest_or_die(stack_arg(ctx, 8, fn), 4, fn, ctx));
    const uint32_t height = get32(guest_or_die(stack_arg(ctx, 9, fn), 4, fn, ctx));
    uint32_t fetch[6];
    for (int i = 0; i < 6; ++i) fetch[i] = get32(fp + 4 * i);
    const uint32_t virt = fetch[1] & 0xFFFFF000u;
    if (virt < xg::kXenosPhysicalWindow)  // any of the three windows
        unimplemented(fn, ctx, "front_buffer_outside_physical_window", virt);
    if (physical_canonical(*runtime()->mem, front_ptr) != physical_canonical(*runtime()->mem, virt))
        unimplemented(fn, ctx, "front_buffer_mismatch", front_ptr);
    if (format != kTex8888 && format != kTex2101010As16161616) unimplemented(fn, ctx, "texture_format", format);
    if (color_space != 0) unimplemented(fn, ctx, "color_space", color_space);
    const uint32_t fw = (fetch[2] & 0x1FFF) + 1, fh = ((fetch[2] >> 13) & 0x1FFF) + 1;
    if (width != fw || height != fh) unimplemented(fn, ctx, "size_mismatch", (width << 16) | height);
    const uint32_t phys = physical_of_window(*runtime()->mem, virt);
    fetch[1] = (fetch[1] & 0xFFFu) | phys;

    uint32_t w[kSwapBufferDwords];
    uint32_t n = 0;
    w[n++] = pm4_type0(kRegFetchConstant0, 6);
    for (int i = 0; i < 6; ++i) w[n++] = fetch[i];
    w[n++] = pm4_type3(kPm4XeSwap, 4);
    w[n++] = kSwapSignature;
    w[n++] = phys;
    w[n++] = width;
    w[n++] = height;
    while (n < kSwapBufferDwords) w[n++] = kPm4Type2Nop;
    for (uint32_t i = 0; i < kSwapBufferDwords; ++i) put32(ring + 4 * i, w[i]);
}

// ---- graphics interrupts ---------------------------------------------------

struct IrqThread {
    GuestThread thread;
    alignas(64) PPCContext ctx;
    uint64_t owner_generation = 0;
};
pthread_key_t g_irq_key;
std::once_flag g_irq_once;
std::atomic<uint64_t> g_irq_count{0};

void irq_thread_end(void* p) {
    auto* it = static_cast<IrqThread*>(p);
    if (rcomp_unregister_thread_ctx) rcomp_unregister_thread_ctx(it->thread.thread_id, &it->ctx);
    if (Runtime* r = runtime(); r && r->generation == it->owner_generation) {
        const Status retired = retire_guest_thread(it->thread, 0);
        if (retired != Status::Ok)
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "graphics interrupt retirement: %s", status_name(retired));
        const Status status=destroy_guest_thread(r->heap,&it->thread);
        if(status!=Status::Ok)rcomp_fatal(RCOMP_FATAL_PLATFORM,"graphics interrupt cleanup: %s",status_name(status));
    }
    delete it;
}

constexpr uint32_t kIrqStackSize = 0x40000;  // 256 KiB, the ExCreateThread default

// Xbox 360: the GPU interrupt runs the registered callback on a hardware
// thread (the `cpu` argument) at raised IRQL. Here it runs on the host
// thread that raised it, on a reusable guest stack/PCR/TLS. The processor
// byte is published below. Automatic ISR IRQL elevation remains unsupported;
// this delta does not change IRQL or the synchronous callback ordering.
}  // namespace

void dispatch_graphics_interrupt(uint32_t callback, uint32_t user_data, uint32_t source, uint32_t cpu) {
    if (source == 0) g_swap_pacing.vblank();
    // Preserve the runner's non-reentrancy contract before touching the live
    // PPCContext or its PCR. IRQ producers run on their own host threads.
    if (current_guest_thread())
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "graphics interrupt cannot reenter an active guest context");
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "graphics interrupt before rcomp::rt::runtime_init");
    PPCFunc* fn = lookup_function(callback);
    if (!fn)
        rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET, "graphics interrupt callback 0x%08X has no recompiled function",
                    callback);
    std::call_once(g_irq_once, [] {
        if (pthread_key_create(&g_irq_key, irq_thread_end) != 0)
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "graphics interrupt: pthread_key_create failed");
    });
    auto* it = static_cast<IrqThread*>(pthread_getspecific(g_irq_key));
    if (it && it->owner_generation != r->generation) {
        if (pthread_setspecific(g_irq_key, nullptr) != 0)
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "graphics interrupt: clearing pthread state failed");
        if (rcomp_unregister_thread_ctx) rcomp_unregister_thread_ctx(it->thread.thread_id, &it->ctx);
        delete it;  // former heap is gone; never free through the new heap
        it = nullptr;
    }
    if (it && thread_object_exited(it->thread.identity)) {
        // Explicit ExTerminateThread ended the previous guest context.
        if (pthread_setspecific(g_irq_key, nullptr) != 0)
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "graphics interrupt: clearing terminated context failed");
        irq_thread_end(it);
        it = nullptr;
    }
    if (!it) {
        it = new IrqThread();
        it->owner_generation = r->generation;
        if (create_guest_thread(r->heap, {kIrqStackSize, callback, 0}, &it->ctx, &it->thread) != Status::Ok) {
            delete it;
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "graphics interrupt: no guest memory for the interrupt stack");
        }
        if (pthread_setspecific(g_irq_key, it) != 0) {
            const Status status=destroy_guest_thread(r->heap,&it->thread);
            if(status!=Status::Ok)rcomp_fatal(RCOMP_FATAL_PLATFORM,"graphics interrupt rollback: %s",status_name(status));
            delete it;
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "graphics interrupt: pthread_setspecific failed");
        }
    }
    PPCContext& c = it->ctx;
    // The interrupt runs "on" hardware thread `cpu`: the title's handler reads it back from PCR+0x10C.
    r->mem->base()[it->thread.pcr + kPcrProcessorNumber] = uint8_t(cpu);
    c = PPCContext{};
    c.r1.u64 = it->thread.initial_r1;
    c.r13.u64 = it->thread.pcr;
    c.r3.u64 = source;
    c.r4.u64 = user_data;
    c.lr = kGuestLrSentinel;
    c.fpscr.loadFromHost();
    uint32_t code = 0;
#if RCOMP_RUNTIME_DIAGNOSTICS
    static std::atomic<uint32_t> irq_traced{0};
    const bool trace_irq = source != 0 && irq_traced.fetch_add(1) < 40;
    if (trace_irq) std::fprintf(stderr, "RCOMP-IRQ callback begin tid=%u source=%u cpu=%u\n", it->thread.thread_id, source, cpu);
#endif
    if (run_guest_callback(it->thread, c, r->mem->base(), fn, &code) != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "graphics interrupt raised from inside guest code on the same host thread");
#if RCOMP_RUNTIME_DIAGNOSTICS
    if (trace_irq) std::fprintf(stderr, "RCOMP-IRQ callback end tid=%u\n", it->thread.thread_id);
#endif
    g_irq_count.fetch_add(1);
}

uint64_t graphics_interrupts_dispatched() { return g_irq_count.load(); }

uint64_t video_swaps_submitted() { return g_video_swaps.load(std::memory_order_relaxed); }

void video_swap_vblank_histogram(uint64_t out[8]) { g_swap_pacing.histogram(out); }

void video_set_frame_rate_cap(uint32_t fps) {
    std::lock_guard<std::mutex> lock(g_frame_cap_mutex);
    g_frame_rate_cap.store(fps, std::memory_order_relaxed);
    g_frame_cap_started = false;
}

uint32_t video_frame_rate_cap() { return g_frame_rate_cap.load(std::memory_order_relaxed); }

namespace {

// ---- variables ---------------------------------------------------------------
// VdGlobalDevice (0x01BE), VdGlobalXamDevice (0x01BF): pointers to the D3D
// device of the title / of XAM; titles store theirs, XAM has none here: 0.
// VdGpuClockInMHz (0x01C0): 500 (Xenos clock). VdHSIOCalibrationLock
// (0x01C1): an RTL_CRITICAL_SECTION, initialised.
Status register_variables() {
    Runtime& r = rt_or_die("register_xboxkrnl_video_hle");
    uint32_t block = 0;
    Status s = r.heap.alloc(0x40, 16, true, &block);
    if (s != Status::Ok) return s;
    guest_write_be32(block + 0x0, 0);
    guest_write_be32(block + 0x4, 0);
    guest_write_be32(block + 0x8, 500);
    struct Var {
        uint32_t ordinal;
        uint32_t addr;
        const char* name;
    } vars[] = {{0x01BE, block + 0x0, "VdGlobalDevice"},
                {0x01BF, block + 0x4, "VdGlobalXamDevice"},
                {0x01C0, block + 0x8, "VdGpuClockInMHz"},
                {0x01C1, block + 0x10, "VdHSIOCalibrationLock"}};
    // The lock is initialised by the registered RtlInitializeCriticalSection.
    PPCFunc* init_cs = find_import(kModuleXboxkrnl, 0x012E);
    if (!init_cs) return Status::NotInitialized;  // register_xboxkrnl_hle() first
    alignas(64) PPCContext ctx{};
    ctx.r3.u64 = block + 0x10;
    init_cs(ctx, r.mem->base());
    for (const Var& v : vars) {
        s = register_variable_import(kModuleXboxkrnl, v.ordinal, v.addr, v.name);
        if (s != Status::Ok) return s;
    }
    return Status::Ok;
}

struct Impl {
    uint32_t ordinal;
    const char* name;
    PPCFunc* fn;
};

const Impl kImpls[] = {
    {0x01B4, "VdEnableDisableClockGating", &VdEnableDisableClockGating},
    {0x01B6, "VdEnableRingBufferRPtrWriteBack", &VdEnableRingBufferRPtrWriteBack},
    {0x01B9, "VdGetCurrentDisplayGamma", &VdGetCurrentDisplayGamma},
    {0x01BA, "VdGetCurrentDisplayInformation", &VdGetCurrentDisplayInformation},
    {0x01BC, "VdGetGraphicsAsicID", &VdGetGraphicsAsicID},
    {0x01B1, "VdCallGraphicsNotificationRoutines", &VdCallGraphicsNotificationRoutines},
    {0x01BD, "VdGetSystemCommandBuffer", &VdGetSystemCommandBuffer},
    {0x01C7, "VdPersistDisplay", &VdPersistDisplay},
    {0x01D9, "VdSetSystemCommandBufferGpuIdentifierAddress", &VdSetSystemCommandBufferGpuIdentifierAddress},
    {0x01C2, "VdInitializeEngines", &VdInitializeEngines},
    {0x01C3, "VdInitializeRingBuffer", &VdInitializeRingBuffer},
    {0x01C5, "VdInitializeScalerCommandBuffer", &VdInitializeScalerCommandBuffer},
    {0x01C6, "VdIsHSIOTrainingSucceeded", &VdIsHSIOTrainingSucceeded},
    {0x01C9, "VdQueryVideoFlags", &VdQueryVideoFlags},
    {0x01CA, "VdQueryVideoMode", &VdQueryVideoMode},
    {0x01D3, "VdSetDisplayMode", &VdSetDisplayMode},
    {0x01D5, "VdSetGraphicsInterruptCallback", &VdSetGraphicsInterruptCallback},
    {0x01DC, "VdShutdownEngines", &VdShutdownEngines},
    {0x025B, "VdSwap", &VdSwap},
    {0x0269, "VdRetrainEDRAM", &VdRetrainEDRAM},
    {0x026A, "VdRetrainEDRAMWorker", &VdRetrainEDRAMWorker},
};

}  // namespace

Status register_xboxkrnl_video_hle() {
    for (const Impl& i : kImpls) {
        uint32_t ord = 0;
        if (!export_ordinal(kModuleXboxkrnl, i.name, &ord) || ord != i.ordinal)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "xboxkrnl HLE %s: ordinal 0x%04X not in export table", i.name,
                        i.ordinal);
        Status s = register_import(kModuleXboxkrnl, i.ordinal, i.fn, i.name);
        if (s != Status::Ok) return s;
    }
    const Status s = register_variables();
    if (s == Status::Ok) {
        std::lock_guard<std::mutex> lock(g_video_generation_mutex);
        const uint64_t generation = rt_or_die("register_xboxkrnl_video_hle").generation;
        if (g_video_generation != generation) {
            g_video_swaps.store(0, std::memory_order_relaxed);
            g_swap_pacing.reset();
            g_video_generation = generation;
        }
    }
    return s;
}

}  // namespace rcomp::rt
