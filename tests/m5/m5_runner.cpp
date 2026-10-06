// M5 runner (Agent 6): first integrated graphics slice. Runs `#_ PROGRAM`
// fixtures that also carry `#_ GFX_EXPECT` on the guest runtime with the
// Vulkan renderer attached to the gfx HLE calls (include/rcomp/gfx.h), then
// checks the presented image against the oracle:
//   GFX_EXPECT size=WxH clear=RRGGBBAA tri=x0,y0,x1,y1,x2,y2 colour=RRGGBBAA
//              frames=N draws=N
// Pixel centres at >= 1.5 px inside every edge must equal the colour, at
// >= 1.5 px outside one edge the clear colour; pixels in between are excluded
// (rasterisation rules are not under test). Exact 8-bit values: the colours
// are exact in UNORM8.
//
// Usage: rcomp_m5_tests [--ppm OUT.ppm]; the title calls rcomp_m5_selftest().
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "cpu_harness.h"
#include "rcomp/diag.h"
#include "rcomp/gfx.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime_state.h"

namespace {

FILE* g_out = stdout;
rcomp::GuestMemory g_mem;
sigjmp_buf g_fatal_jmp;
char g_fatal[512];

void fatal_hook(rcomp_fatal_kind kind, const char* msg) {
    snprintf(g_fatal, sizeof g_fatal, "%s: %s", rcomp_fatal_kind_name(kind), msg);
    siglongjmp(g_fatal_jmp, 1);
}

void emit(const char* id, int index, bool ok, const std::string& reason) {
    fprintf(g_out, "{\"id\":\"%s\",\"index\":%d,\"status\":\"%s\",\"stage\":\"execute\",\"reason\":\"", id, index,
            ok ? "PASS" : "FAIL");
    for (char c : reason) {
        if (c == '"' || c == '\\') fputc('\\', g_out);
        fputc(c, g_out);
    }
    fprintf(g_out, "\"}\n");
    fflush(g_out);
}

struct Expect {
    unsigned w = 0, h = 0, frames = 0, draws = 0;
    uint8_t clear[4], colour[4];
    double tri[6];
};

bool hex_rgba(const char* s, uint8_t out[4]) {
    unsigned long v = strtoul(s, nullptr, 16);
    for (int i = 0; i < 4; ++i) out[i] = (uint8_t)(v >> (24 - 8 * i));
    return strlen(s) >= 8;
}

bool parse_expect(const char* spec, Expect* e) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", spec);
    int got = 0;
    for (char* tok = strtok(buf, " "); tok; tok = strtok(nullptr, " ")) {
        if (sscanf(tok, "size=%ux%u", &e->w, &e->h) == 2) got |= 1;
        else if (!strncmp(tok, "clear=", 6) && hex_rgba(tok + 6, e->clear)) got |= 2;
        else if (!strncmp(tok, "colour=", 7) && hex_rgba(tok + 7, e->colour)) got |= 4;
        else if (sscanf(tok, "tri=%lf,%lf,%lf,%lf,%lf,%lf", &e->tri[0], &e->tri[1], &e->tri[2], &e->tri[3],
                        &e->tri[4], &e->tri[5]) == 6) got |= 8;
        else if (sscanf(tok, "frames=%u", &e->frames) == 1) got |= 16;
        else if (sscanf(tok, "draws=%u", &e->draws) == 1) got |= 32;
        else return false;
    }
    return got == 63 && e->w && e->h && e->w <= 256 && e->h <= 256;
}

// Signed distance of (px,py) to each edge, positive inside (either winding).
int classify(const Expect& e, double px, double py) {
    const double* t = e.tri;
    double area = (t[2] - t[0]) * (t[5] - t[1]) - (t[3] - t[1]) * (t[4] - t[0]);
    double sgn = area > 0 ? 1.0 : -1.0;
    bool inside = true;
    for (int i = 0; i < 3; ++i) {
        double ax = t[2 * i], ay = t[2 * i + 1], bx = t[2 * ((i + 1) % 3)], by = t[2 * ((i + 1) % 3) + 1];
        double len = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        double d = sgn * ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / len;
        if (d <= -1.5) return -1;
        if (d < 1.5) inside = false;
    }
    return inside ? 1 : 0;
}

int run_m5(const char* ppm) {
    if (g_mem.reserve() != rcomp::MemStatus::Ok ||
        g_mem.commit(0x10000000ull, 0x20000ull, rcomp::Protect::ReadWrite) != rcomp::MemStatus::Ok) {
        fprintf(g_out, "RCOMP-M5 error guest memory setup failed\n");
        return 2;
    }
    rcomp::set_active_guest_memory(&g_mem);
    namespace rt = rcomp::rt;
    if (rt::runtime_init(&g_mem) != rt::Status::Ok || rt::register_xboxkrnl_hle() != rt::Status::Ok) {
        fprintf(g_out, "RCOMP-M5 error runtime init failed\n");
        return 2;
    }
    rcomp_set_fatal_hook(fatal_hook);
    rcomp_gfx_hle_set_trace(g_out);

    int index = 0, pass = 0, fail = 0;
    for (int u = 0; u < kUnitCount; ++u) {
        const TestUnit& unit = *kUnits[u];
        rcomp::register_functions(unit.funcs, (size_t)unit.n_funcs);
        for (int k = 0; k < unit.n_cases; ++k, ++index) {
            const TestCase& t = unit.cases[k];
            fprintf(g_out, "RCOMP-M5 begin index=%d id=%s\n", index, t.id);
            fflush(g_out);
            Expect ex;
            if (!t.program || !t.gfx_expect || !parse_expect(t.gfx_expect, &ex)) {
                emit(t.id, index, false, "not a PROGRAM + GFX_EXPECT test");
                ++fail;
                continue;
            }
            rcomp_gfx* gfx = nullptr;
            int gs = rcomp_gfx_create(ex.w, ex.h, &gfx);
            if (gs != RCOMP_GFX_OK) {
                emit(t.id, index, false, "renderer creation failed, status " + std::to_string(gs));
                ++fail;
                continue;
            }
            fprintf(g_out, "RCOMP-M5 device=\"%s\"\n", rcomp_gfx_device_name(gfx));
            rcomp_gfx_hle_attach(gfx, ex.w, ex.h);

            uint8_t* base = g_mem.base();
            memset(base + 0x10000000, 0, 0x20000);
            for (int m = 0; m < t.n_mem_in; ++m) memcpy(base + t.mem_in[m].addr, t.mem_in[m].bytes, t.mem_in[m].size);
            rt::Runtime& r = *rt::runtime();
            uint32_t heap_before = r.heap.stats().live_allocations;
            alignas(64) static PPCContext ctx;
            ctx = PPCContext{};
            ctx.fpscr.loadFromHost();
            rt::GuestThread th;
            std::string reason;
            bool ok = true;
            if (rt::create_guest_thread(r.heap, {0x10000, t.guest, 0}, &ctx, &th) != rt::Status::Ok) {
                emit(t.id, index, false, "create_guest_thread failed");
                ++fail;
                rcomp_gfx_hle_attach(nullptr, 0, 0);
                rcomp_gfx_destroy(gfx);
                continue;
            }
            uint32_t exit_code = 0;
            if (sigsetjmp(g_fatal_jmp, 1) == 0) {
                if (rt::run_guest_thread(th, ctx, base, t.fn, &exit_code) != rt::Status::Ok) {
                    ok = false;
                    reason += "run_guest_thread failed; ";
                }
            } else {
                ok = false;
                reason += std::string("fatal ") + g_fatal + "; ";
            }
            for (int i = 0; i < t.n_out; ++i) {
                const RegVal& rv = t.out[i];
                uint64_t actual = rv.reg[1] == '3' ? ctx.r3.u64 : ctx.r4.u64;
                if (rv.kind != RK_GPR || (strcmp(rv.reg, "r3") && strcmp(rv.reg, "r4"))) {
                    ok = false;
                    reason += "unsupported output; ";
                } else if (actual != rv.u) {
                    ok = false;
                    char b[128];
                    snprintf(b, sizeof b, "%s expected 0x%llX actual 0x%llX; ", rv.reg, (unsigned long long)rv.u,
                             (unsigned long long)actual);
                    reason += b;
                }
            }
            // Image oracle.
            uint32_t frames = 0;
            const uint8_t* img = rcomp_gfx_hle_last_frame(&frames);
            if (frames != ex.frames) {
                ok = false;
                reason += "presented frames " + std::to_string(frames) + " expected " + std::to_string(ex.frames) + "; ";
            }
            unsigned inside = 0, outside = 0, edge = 0, bad = 0;
            if (img) {
                for (unsigned y = 0; y < ex.h; ++y)
                    for (unsigned x = 0; x < ex.w; ++x) {
                        int c = classify(ex, x + 0.5, y + 0.5);
                        if (c == 0) { ++edge; continue; }
                        const uint8_t* p = img + 4 * (y * ex.w + x);
                        const uint8_t* want = c > 0 ? ex.colour : ex.clear;
                        c > 0 ? ++inside : ++outside;
                        if (memcmp(p, want, 4)) {
                            if (bad < 4) {
                                char b[160];
                                snprintf(b, sizeof b, "pixel (%u,%u) %s got %u,%u,%u,%u want %u,%u,%u,%u; ", x, y,
                                         c > 0 ? "inside" : "outside", p[0], p[1], p[2], p[3], want[0], want[1],
                                         want[2], want[3]);
                                reason += b;
                            }
                            ++bad;
                        }
                    }
                if (bad) ok = false;
                if (ppm) {
                    FILE* f = fopen(ppm, "wb");
                    if (f) {
                        fprintf(f, "P6\n%u %u\n255\n", ex.w, ex.h);
                        for (unsigned i = 0; i < ex.w * ex.h; ++i) fwrite(img + 4 * i, 1, 3, f);
                        fclose(f);
                    }
                }
            }
            fprintf(g_out, "RCOMP-M5 image inside=%u outside=%u edge_excluded=%u mismatches=%u\n", inside, outside,
                    edge, bad);
            rt::destroy_guest_thread(r.heap, &th);
            if (r.heap.stats().live_allocations != heap_before) {
                ok = false;
                reason += "heap allocations leaked; ";
            }
            if (r.handles.live_count() != 0) {
                ok = false;
                reason += "handles leaked; ";
            }
            rcomp_gfx_hle_attach(nullptr, 0, 0);
            rcomp_gfx_destroy(gfx);
            emit(t.id, index, ok, reason);
            ok ? ++pass : ++fail;
        }
    }
    fprintf(g_out, "RCOMP-M5 executed=%d pass=%d fail=%d\n", pass + fail, pass, fail);
    fflush(g_out);
    return fail ? 1 : 0;
}

}  // namespace

#ifdef RCOMP_HARNESS_LIBRARY
extern "C" int rcomp_m5_selftest(FILE* out) {
    g_out = out;
    return run_m5(nullptr);
}
#else
int main(int argc, char** argv) {
    const char* ppm = nullptr;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
    return run_m5(ppm);
}
#endif
