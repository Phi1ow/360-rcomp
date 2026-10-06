// Synthetic graphics XEX (fixtures/xex/rcomp_gfx_title.s) through the game
// shell (app/): the recompiled PPC code allocates GPU memory, sets up the
// ring buffer and the interrupt callback, draws its front buffer on the CPU,
// calls VdSwap and moves the GPU write pointer; the Xenos GPU (Vulkan backend)
// presents the frame and the shell puts it on the screen. No game content.
// Host: lavapipe, headless surface. PS5: PS5_Vulkan, display plane.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../../gpu/xenos/rexglue/rcomp/vulkan_output.h"
#include "ppc_recomp_shared.h"
#include "rcomp/app/title.h"

namespace {

FILE* g_out = stdout;
int g_pass = 0, g_fail = 0;
void check(const char* id, bool ok, const std::string& why = "") {
    fprintf(g_out, "%-52s %s%s%s\n", id, ok ? "PASS" : "FAIL", ok ? "" : " ", ok ? "" : why.c_str());
    fflush(g_out);
    (ok ? g_pass : g_fail)++;
}

int run(const char* xex_path) {
    rcomp::app::TitleConfig cfg;
    FILE* f = fopen(xex_path, "rb");
    if (!f) {
        fprintf(g_out, "RCOMP-XEX-GFX error: cannot open %s\n", xex_path);
        return 2;
    }
    uint8_t buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) cfg.xex.insert(cfg.xex.end(), buf, buf + n);
    fclose(f);
    for (PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) cfg.functions.push_back({uint32_t(m->guest), m->host, nullptr});
    cfg.display_width = 256;
    cfg.display_height = 128;
    cfg.log = g_out;
    std::string err;
    auto title = rcomp::app::Title::Create(std::move(cfg), &err);
    check("xex_gfx/shell_ready", title != nullptr, err);
    if (!title) return 1;

    uint32_t code = 0xFFFFFFFF;
    const bool ran = title->RunEntry(0, &code, &err);
    char why[96];
    snprintf(why, sizeof why, "%s exit=%u (0 = ok, else the failed step of rcomp_gfx_title.s)", err.c_str(), code);
    check("xex_gfx/recompiled_title_drew_and_swapped", ran && code == 0, why);

    // The presented frame is the front buffer the recompiled code drew.
    std::vector<uint8_t> img;
    uint32_t w = 0, h = 0, bad = 0, first = UINT32_MAX;
    const bool cap = title->output().Capture(&img, &w, &h) && w == 256 && h == 128;
    for (uint32_t i = 0; cap && i < w * h; ++i) {
        const uint8_t* px = &img[4 * i];
        if (abs(px[0] - int(i % 256)) > 1 || abs(px[1] - int(i / 256)) > 1 || abs(px[2] - 0x40) > 1) {
            if (first == UINT32_MAX) first = i;
            ++bad;
        }
    }
    snprintf(why, sizeof why, "captured=%d %ux%u bad=%u first=%d got=%u,%u,%u", cap, w, h, bad, int(first),
             first == UINT32_MAX ? 0 : img[4 * first], first == UINT32_MAX ? 0 : img[4 * first + 1],
             first == UINT32_MAX ? 0 : img[4 * first + 2]);
    check("xex_gfx/presented_frame_is_the_guest_drawing", cap && bad == 0, why);

    // The screen shows it (display plane on PS5, headless surface on the host).
    if (title->display()) {
        for (int i = 0; i < 200 && title->frames_shown() < 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        snprintf(why, sizeof why, "frames_shown=%llu (%s)", (unsigned long long)title->frames_shown(),
                 title->screen_status().c_str());
        check("xex_gfx/frame_on_screen", title->frames_shown() >= 3, why);
        // On a real screen, leave it up a few seconds.
        if (title->display()->kind() == rcomp::xenos::VulkanDisplay::Kind::kDisplayPlane)
            std::this_thread::sleep_for(std::chrono::seconds(5));
    } else {
        fprintf(g_out, "%-52s NOT TESTED (%s)\n", "xex_gfx/frame_on_screen", title->screen_status().c_str());
    }
    title.reset();
    fprintf(g_out, "RCOMP-XEX-GFX pass=%d fail=%d\n", g_pass, g_fail);
    fflush(g_out);
    return g_fail ? 1 : 0;
}

}  // namespace

#ifdef RCOMP_HARNESS_LIBRARY
extern "C" int rcomp_xex_gfx_selftest(FILE* out, const char* xex_path) {
    g_out = out;
    return run(xex_path);
}
#else
int main(int argc, char** argv) { return run(argc > 1 ? argv[1] : RCOMP_XEX_PATH); }
#endif
