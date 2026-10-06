// R-comp - M5 graphics slice: guest bindings of the identified render calls
// (include/rcomp/gfx.h). SPDX-License-Identifier: GPL-3.0-or-later
//
// Each function has the recompiled-function signature and is bound to a guest
// helper by `#_ HLE gfx_<Name>` (tests/cpu/gen_harness.py). Guest data is read
// through GuestMemory::translate (checked; an uncommitted pointer is an error
// status, never a fault) and converted from big-endian.
#define PPC_CONFIG_H_INCLUDED
#include "rcomp/ppc_prelude.h"
#include <ppc_context.h>

#include <stdio.h>
#include <string.h>

#include <vector>

#include "rcomp/gfx.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime_state.h"

namespace {

rcomp_gfx* g_gfx = nullptr;
uint32_t g_w = 0, g_h = 0;
FILE* g_trace = nullptr;
std::vector<uint8_t> g_frame;
uint32_t g_frames = 0, g_frame_draws = 0;
bool g_frame_open = false;

FILE* trace() { return g_trace ? g_trace : stderr; }

void unpack_rgba(uint32_t c, float out[4]) {
    out[0] = ((c >> 24) & 0xFF) / 255.0f;
    out[1] = ((c >> 16) & 0xFF) / 255.0f;
    out[2] = ((c >> 8) & 0xFF) / 255.0f;
    out[3] = (c & 0xFF) / 255.0f;
}

void finish(PPCContext& ctx, const char* name, int status, const char* args) {
    fprintf(trace(), "RCOMP-GFX call=%s lr=0x%08llX %s frame=%u draw=%u result=%d\n", name,
            (unsigned long long)ctx.lr, args, g_frames, g_frame_draws, status);
    fflush(trace());
    ctx.r3.u64 = (uint32_t)status;
}

}  // namespace

extern "C" void rcomp_gfx_hle_attach(rcomp_gfx* g, uint32_t width, uint32_t height) {
    g_gfx = g;
    g_w = width;
    g_h = height;
    g_frame.assign((size_t)width * height * 4, 0);
    g_frames = g_frame_draws = 0;
    g_frame_open = false;
}

extern "C" const uint8_t* rcomp_gfx_hle_last_frame(uint32_t* frame_count) {
    if (frame_count) *frame_count = g_frames;
    return g_frames ? g_frame.data() : nullptr;
}

extern "C" void rcomp_gfx_hle_set_trace(void* file) { g_trace = static_cast<FILE*>(file); }

PPC_FUNC(rcomp_hle_gfx_Clear) {
    (void)base;
    uint32_t c = ctx.r3.u32;
    char args[64];
    snprintf(args, sizeof args, "clear=0x%08X", c);
    if (!g_gfx) return finish(ctx, "Clear", RCOMP_GFX_NOT_INITIALISED, args);
    float rgba[4];
    unpack_rgba(c, rgba);
    int s = rcomp_gfx_begin_frame(g_gfx, rgba);
    if (s == RCOMP_GFX_OK) {
        g_frame_open = true;
        g_frame_draws = 0;
    }
    finish(ctx, "Clear", s, args);
}

PPC_FUNC(rcomp_hle_gfx_DrawTriangles) {
    (void)base;
    uint32_t ptr = ctx.r3.u32, n = ctx.r4.u32, c = ctx.r5.u32;
    char args[96];
    snprintf(args, sizeof args, "vertices=0x%08X count=%u colour=0x%08X", ptr, n, c);
    if (!g_gfx) return finish(ctx, "DrawTriangles", RCOMP_GFX_NOT_INITIALISED, args);
    if (!g_frame_open) return finish(ctx, "DrawTriangles", RCOMP_GFX_BAD_STATE, args);
    if (n == 0 || n % 3 || n > 3 * 64) return finish(ctx, "DrawTriangles", RCOMP_GFX_BAD_ARGUMENT, args);
    rcomp::GuestMemory* mem = rcomp::active_guest_memory();
    const uint8_t* src = mem ? mem->translate(ptr, n * 8) : nullptr;
    if (!src) return finish(ctx, "DrawTriangles", RCOMP_GFX_GUEST_FAULT, args);
    std::vector<float> xy(n * 2);
    for (uint32_t i = 0; i < n * 2; ++i) {
        uint32_t w;
        memcpy(&w, src + 4 * i, 4);
        w = __builtin_bswap32(w);
        memcpy(&xy[i], &w, 4);
    }
    float rgba[4];
    unpack_rgba(c, rgba);
    int s = rcomp_gfx_draw(g_gfx, xy.data(), n, rgba);
    if (s == RCOMP_GFX_OK) ++g_frame_draws;
    finish(ctx, "DrawTriangles", s, args);
}

PPC_FUNC(rcomp_hle_gfx_Present) {
    (void)base;
    if (!g_gfx) return finish(ctx, "Present", RCOMP_GFX_NOT_INITIALISED, "");
    if (!g_frame_open) return finish(ctx, "Present", RCOMP_GFX_BAD_STATE, "");
    int s = rcomp_gfx_end_frame(g_gfx, g_frame.data());
    g_frame_open = false;
    if (s == RCOMP_GFX_OK) ++g_frames;
    finish(ctx, "Present", s, "");
}
