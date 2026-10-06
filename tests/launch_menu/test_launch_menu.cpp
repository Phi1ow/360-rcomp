// Launch options menu model: options file, controller navigation, countdown and rendering.
#include <stdio.h>

#include <string>
#include <vector>

#include "rcomp/app/launch_menu.h"
#include "rcomp/input.h"

using rcomp::app::LaunchMenu;
using rcomp::app::LaunchOptions;

static int g_failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

int main() {
    // ---- options file -------------------------------------------------------------------------------
    LaunchOptions o;
    CHECK(o.draw_scale == 3);
    CHECK(rcomp::app::parse_launch_options("draw_scale=2\r\n", &o) && o.draw_scale == 2);
    CHECK(!rcomp::app::parse_launch_options("draw_scale=9\nnoise\n", &o) && o.draw_scale == 2);
    CHECK(o.frame_rate_cap == 0);  // a file from before the frame-rate option keeps the game's pacing
    CHECK(rcomp::app::serialize_launch_options(o) == "draw_scale=2\nframe_rate_cap=0\n");
    CHECK(rcomp::app::parse_launch_options("frame_rate_cap=30\n", &o) && o.frame_rate_cap == 30 && o.draw_scale == 2);
    CHECK(!rcomp::app::parse_launch_options("frame_rate_cap=45\n", &o) && o.frame_rate_cap == 30);
    LaunchOptions round;
    CHECK(rcomp::app::parse_launch_options(rcomp::app::serialize_launch_options(o), &round) && round.draw_scale == 2 &&
          round.frame_rate_cap == 30);
    for (uint32_t cap : {40u, 50u, 60u}) {  // every cap the menu offers is read and written back
        LaunchOptions each;
        CHECK(rcomp::app::parse_launch_options("frame_rate_cap=" + std::to_string(cap) + "\n", &each) && each.frame_rate_cap == cap);
        CHECK(rcomp::app::serialize_launch_options(each) == "draw_scale=3\nframe_rate_cap=" + std::to_string(cap) + "\n");
    }
    CHECK(!rcomp::app::parse_launch_options("frame_rate_cap=35\nframe_rate_cap=100\n", &o) && o.frame_rate_cap == 30);

    // ---- navigation ---------------------------------------------------------------------------------
    LaunchOptions start;
    start.draw_scale = 3;
    {
        // The button held when the menu appears is not a press; START GAME is preselected.
        LaunchMenu m("Grand Theft Auto IV", start, 15);
        CHECK(!m.update(RCOMP_XINPUT_A, 0.0));
        CHECK(m.selected_row() == 2);
        CHECK(!m.update(0, 0.1));
        CHECK(!m.update(RCOMP_XINPUT_DPAD_DOWN, 0.2));  // wraps to RESOLUTION
        CHECK(m.selected_row() == 0);
        CHECK(!m.update(0, 0.3));
        CHECK(!m.update(RCOMP_XINPUT_DPAD_LEFT, 0.4));
        CHECK(m.options().draw_scale == 2);
        CHECK(!m.update(0, 0.5));
        CHECK(!m.update(RCOMP_XINPUT_DPAD_LEFT, 0.6));
        CHECK(m.options().draw_scale == 1);
        CHECK(!m.update(0, 0.7));
        CHECK(!m.update(RCOMP_XINPUT_DPAD_LEFT, 0.8));  // wraps around
        CHECK(m.options().draw_scale == 3);
        CHECK(!m.update(RCOMP_XINPUT_DPAD_LEFT, 0.9));  // held: no repeat
        CHECK(m.options().draw_scale == 3);
        CHECK(!m.update(0, 1.0));
        CHECK(!m.update(RCOMP_XINPUT_A, 1.1));          // Cross on RESOLUTION cycles
        CHECK(m.options().draw_scale == 1);
        // A press stopped the countdown: nothing starts by itself.
        CHECK(!m.update(0, 60.0));
        CHECK(!m.update(RCOMP_XINPUT_DPAD_DOWN, 60.1));
        CHECK(m.selected_row() == 1);                   // FRAME RATE
        CHECK(m.options().frame_rate_cap == 0);
        CHECK(!m.update(RCOMP_XINPUT_DPAD_RIGHT, 60.2));
        CHECK(m.options().frame_rate_cap == 30);
        CHECK(!m.update(0, 60.3));
        CHECK(!m.update(RCOMP_XINPUT_A, 60.4));          // Cross on FRAME RATE goes to the next value
        CHECK(m.options().frame_rate_cap == 40);
        CHECK(!m.update(0, 60.5));
        CHECK(!m.update(RCOMP_XINPUT_DPAD_LEFT, 60.6));
        CHECK(m.options().frame_rate_cap == 30);
        CHECK(m.options().draw_scale == 1);             // the other row is untouched
        CHECK(!m.update(RCOMP_XINPUT_DPAD_DOWN, 60.7));
        CHECK(m.selected_row() == 2);
        CHECK(!m.update(0, 60.8));
        CHECK(m.update(RCOMP_XINPUT_A, 60.9));          // Cross on START GAME
        CHECK(m.started() && m.options().draw_scale == 1 && m.options().frame_rate_cap == 30);
    }
    {
        // OPTIONS starts from any row.
        LaunchMenu m("x", start, 0);
        CHECK(!m.update(0, 0.0));
        CHECK(!m.update(RCOMP_XINPUT_DPAD_UP, 0.1));
        CHECK(m.update(RCOMP_XINPUT_START, 0.2));
    }
    {
        // Without input the countdown starts the game with the shown choice; countdown 0 waits forever.
        LaunchMenu m("x", start, 15);
        CHECK(!m.update(0, 100.0));
        CHECK(!m.update(0, 114.9));
        CHECK(m.update(0, 115.0));
        CHECK(m.options().draw_scale == 3 && m.options().frame_rate_cap == 0);
        LaunchOptions capped = start;
        capped.frame_rate_cap = 30;
        LaunchMenu kept("x", capped, 15);  // a remembered cap is preselected and kept by the countdown
        CHECK(!kept.update(0, 0.0));
        CHECK(kept.update(0, 15.0) && kept.options().frame_rate_cap == 30);
        capped.frame_rate_cap = 60;        // every cap the menu offers is kept
        CHECK(LaunchMenu("x", capped, 15).options().frame_rate_cap == 60);
        capped.frame_rate_cap = 45;        // anything the menu does not offer means no cap
        CHECK(LaunchMenu("x", capped, 15).options().frame_rate_cap == 0);
        LaunchMenu wait("x", start, 0);
        CHECK(!wait.update(0, 0.0));
        CHECK(!wait.update(0, 1e6));
    }

    {
        // The FRAME RATE row cycles through unlocked, 30, 40, 50 and 60, and wraps in both directions.
        LaunchMenu m("x", start, 0);
        CHECK(!m.update(0, 0.0));
        CHECK(!m.update(RCOMP_XINPUT_DPAD_UP, 0.1));  // START GAME -> FRAME RATE
        CHECK(m.selected_row() == 1);
        double t = 0.2;
        for (uint32_t expected : {30u, 40u, 50u, 60u, 0u, 30u}) {
            CHECK(!m.update(RCOMP_XINPUT_DPAD_RIGHT, t));
            CHECK(m.options().frame_rate_cap == expected);
            CHECK(!m.update(0, t + 0.05));
            t += 0.1;
        }
        CHECK(!m.update(RCOMP_XINPUT_DPAD_LEFT, t));
        CHECK(m.options().frame_rate_cap == 0);  // back from 30 to unlocked
        CHECK(!m.update(0, t + 0.05));
        CHECK(!m.update(RCOMP_XINPUT_DPAD_LEFT, t + 0.1));
        CHECK(m.options().frame_rate_cap == 60);  // and around to 60
    }

    // ---- rendering ----------------------------------------------------------------------------------
    {
        LaunchMenu m("Grand Theft Auto: Episodes from Liberty City", start, 15);
        m.update(0, 0.0);
        std::vector<uint8_t> frame;
        m.render(&frame, 1280, 720, 1.0);
        CHECK(frame.size() == size_t(1280) * 720 * 4);
        size_t text = 0, highlight = 0;
        for (size_t i = 0; i < frame.size(); i += 4) {
            if (frame[i] > 200 && frame[i + 1] > 200) ++text;
            if (frame[i] == 46 && frame[i + 1] == 120) ++highlight;
            CHECK(frame[i + 3] == 0xFF);
            if (frame[i + 3] != 0xFF) break;
        }
        CHECK(text > 2000);       // title, labels and values are drawn
        CHECK(highlight > 10000); // the selected row is highlighted
        std::vector<uint8_t> small;
        m.render(&small, 640, 360, 1.0);  // other sizes scale the layout
        CHECK(small.size() == size_t(640) * 360 * 4);
    }

    if (g_failures) {
        fprintf(stderr, "launch_menu: FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("launch_menu: PASS\n");
    return 0;
}
