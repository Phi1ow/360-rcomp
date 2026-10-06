// Shared interface (owner: PRIME). M5 graphics boundary, option A
// (docs/ARCHITECTURE.md): identified guest render calls are replaced by host
// implementations on Vulkan (PS5_Vulkan on the console).
//
// Guest-facing calls (bound like imports: a weak recompiled helper is
// overridden by rcomp_hle_gfx_<Name>, see tests/cpu/gen_harness.py `#_ HLE`).
// Arguments in r3..r5, result in r3 (0 = done, otherwise an rcomp_gfx_status;
// never a success without the work).
//
//   gfx_Clear(r3 = clear colour 0xRRGGBBAA)        opens a frame
//   gfx_DrawTriangles(r3 = guest address of N x {float x, float y} big-endian
//                     NDC positions, r4 = N (multiple of 3, <= 3*64),
//                     r5 = colour 0xRRGGBBAA)       records one draw
//   gfx_Present(r3 = 0)                             ends the frame: render,
//                                                   read back, keep the image
//
// Every call prints one trace line:
//   RCOMP-GFX call=<name> lr=0x... <args> frame=<n> draw=<n> result=<status>
// linking guest call, resources, submission and image (M5 traceability).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum rcomp_gfx_status {
    RCOMP_GFX_OK = 0,
    RCOMP_GFX_NOT_INITIALISED = 1,
    RCOMP_GFX_BAD_STATE = 2,      // e.g. draw outside a frame, present without clear
    RCOMP_GFX_BAD_ARGUMENT = 3,   // count not a multiple of 3, too many vertices
    RCOMP_GFX_GUEST_FAULT = 4,    // guest pointer not committed
    RCOMP_GFX_TOO_MANY_DRAWS = 5,
    RCOMP_GFX_VULKAN_ERROR = 6,   // details on stderr
};

typedef struct rcomp_gfx rcomp_gfx;

// Offscreen renderer (gpu/hle/gfx_vk.c). width/height <= 256. Vulkan 1.0
// only: classic render pass, one R8G8B8A8_UNORM target, vertex buffer, one
// UBO per draw, explicit barriers, fence, readback.
int rcomp_gfx_create(uint32_t width, uint32_t height, rcomp_gfx** out);
void rcomp_gfx_destroy(rcomp_gfx* g);
int rcomp_gfx_begin_frame(rcomp_gfx* g, const float clear_rgba[4]);
int rcomp_gfx_draw(rcomp_gfx* g, const float* xy, uint32_t vertex_count, const float rgba[4]);
// Submits, waits (5 s) and copies the frame into `rgba_out` (width*height*4).
int rcomp_gfx_end_frame(rcomp_gfx* g, uint8_t* rgba_out);
const char* rcomp_gfx_device_name(const rcomp_gfx* g);

// Guest binding state (gpu/hle/gfx_hle.cpp): the renderer used by the HLE
// calls and the last presented frame.
void rcomp_gfx_hle_attach(rcomp_gfx* g, uint32_t width, uint32_t height);
const uint8_t* rcomp_gfx_hle_last_frame(uint32_t* frame_count);
// Trace destination (a FILE*); stderr by default.
void rcomp_gfx_hle_set_trace(void* file);

#ifdef __cplusplus
}
#endif
