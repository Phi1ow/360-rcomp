// xboxkrnl.exe video exports (Vd*) over the emulated Xenos GPU
// (owner: Agent 3, runtime/). Separate library rcomp_runtime_video: a title
// that links it must also link a GPU implementation of
// include/rcomp/xenos_gpu.h (gpu/xenos/rexglue: rcomp_xenos_host).
//
// Start-up order for a title:
//   runtime_init(mem) ; register_xboxkrnl_hle() ;
//   register_xboxkrnl_video_hle() ; load_xex_image(...) ; runtime_set_static_tls(...) ;
//   rcomp::xenos::gpu_start(mem, cfg, rcomp::rt::dispatch_graphics_interrupt, backend) ;
//   ... run the XEX entry point.
#pragma once

#include <stdint.h>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Registers the Vd* functions and variables of src/hle_xboxkrnl_video.cpp.
// Requires runtime_init; the variables are allocated from the runtime heap.
Status register_xboxkrnl_video_hle();

// Existing VdSwap counter, sampled without synchronizing guest/GPU work.
// Reset when video HLE is registered for a new runtime generation.
uint64_t video_swaps_submitted();

// Swaps by the number of vertical blanks since the previous VdSwap: out[k] counts the swaps that
// came k vblanks after their predecessor (k = 0..6, out[7] seven or more). Seconds at exactly 30 fps
// put every swap in out[2]; the ~36 fps seconds mix out[1] and out[2]. Reset with the swap counter.
void video_swap_vblank_histogram(uint64_t out[8]);

// Frame-rate cap chosen in the launch options (0 = none, the default). With a cap, VdSwap returns no
// sooner than 1/fps after the previous swap returned, on a steady schedule: the title's render thread
// waits there as it would for vsync, so the game runs and presents at most `fps` frames per second. A
// swap more than one period late restarts the schedule instead of letting later frames catch up.
// Kept across runtime generations; set it before the title starts.
void video_set_frame_rate_cap(uint32_t fps);
uint32_t video_frame_rate_cap();

// rcomp::xenos::InterruptDispatcher for gpu_start: runs the title's graphics
// interrupt callback as guest code, callback(source, user_data), on the
// calling host thread (the GPU's bridge or worker thread). Each such host
// thread gets its own guest stack and PCR on first use, released when the
// host thread ends or replaced when it enters a new runtime. Stop/join callback
// producers before runtime_shutdown. Lifetime identities prevent stale cached
// guest addresses being freed in a subsequent runtime.
// A callback address with no recompiled function is fatal.
void dispatch_graphics_interrupt(uint32_t callback, uint32_t user_data, uint32_t source, uint32_t cpu);

// Number of callbacks run so far (diagnostics and tests).
uint64_t graphics_interrupts_dispatched();

}  // namespace rcomp::rt
