// Shared interface (owner: PRIME). The emulated Xenos GPU as the runtime's
// video exports (runtime/src/hle_xboxkrnl_video.cpp) see it. Implemented by
// gpu/xenos/rexglue/rcomp/xenos_host.cpp (rexglue's command processor).
//
// Guest-visible layout:
//   * physical memory = guest window [kXenosPhysicalWindow, +512 MiB): a GPU
//     (physical) address P is guest address kXenosPhysicalWindow + P;
//   * GPU registers = guest window [kXenosRegisterWindow, +64 KiB), register
//     index r at kXenosRegisterWindow + 4*r, big-endian. It is ordinary guest
//     memory (no fault-based MMIO: signal handlers do not run on PS5); a
//     bridge thread forwards CP_RB_WPTR writes to the command processor and
//     keeps the registers titles read up to date.
#pragma once

#include <stdint.h>

namespace rcomp::xenos {

constexpr uint32_t kXenosPhysicalWindow = 0xA0000000u;
constexpr uint32_t kXenosPhysicalSize = 0x20000000u;
constexpr uint32_t kXenosRegisterWindow = 0x7FC80000u;
constexpr uint32_t kXenosRegisterWindowSize = 0x10000u;
constexpr uint32_t kRegCpRbWptr = 0x01C5;

// Runs the title's graphics interrupt callback as guest code:
// callback(source, user_data) with source 0 = vblank, 1 = command stream.
using InterruptDispatcher = void (*)(uint32_t callback, uint32_t user_data, uint32_t source, uint32_t cpu);

// What the video exports report (VdQueryVideoMode and friends): the mode
// the emulated display runs at, set when the GPU is started.
struct DisplayMode {
    uint32_t width;
    uint32_t height;
    float refresh_hz;
};

bool gpu_running();
// Fatal before gpu_start.
DisplayMode gpu_display_mode();
// VdInitializeRingBuffer: ring at physical `ptr`, 1 << (size_log2 + 3) bytes.
void gpu_initialize_ring_buffer(uint32_t ptr, uint32_t size_log2);
// VdEnableRingBufferRPtrWriteBack: read pointer written back to physical `ptr`.
void gpu_enable_read_pointer_writeback(uint32_t ptr, uint32_t block_size_log2);
// VdSetGraphicsInterruptCallback.
void gpu_set_interrupt_callback(uint32_t callback, uint32_t user_data);

}  // namespace rcomp::xenos
