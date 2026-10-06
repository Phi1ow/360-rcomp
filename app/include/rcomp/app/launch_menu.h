// Launch options menu (owner: PRIME, app/): before the guest starts, every R-comp title shows one screen
// where the player picks the internal rendering resolution and an optional frame-rate cap (30, 40, 50 or 60 fps), then starts the
// game. The choice is remembered in the title's save folder and preselected at the next launch. MSAA stays
// the game's own: the only 1x collapse available (rcomp_force_msaa_1x) leaves part of the picture undrawn
// (owner decision, 2 Oct 2026).
//
// This header is the host-testable model: state, controller navigation, the options file format and the
// rendering of the screen into an R8G8B8X8 image. Title::Create presents that image and reads the pad.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace rcomp::app {

struct LaunchOptions {
    // Internal Xenos draw resolution scale: 1 = 1280x720, 2 = 2560x1440, 3 = 3840x2160.
    uint32_t draw_scale = 3;
    // Frame-rate cap: 0 = none (the game's own pacing), 30, 40, 50 or 60 = at most that many swaps per
    // second (rcomp::rt::video_set_frame_rate_cap).
    uint32_t frame_rate_cap = 0;
};

// "draw_scale=N" and "frame_rate_cap=0|30|40|50|60" lines. Unknown keys and lines are ignored; an invalid value
// keeps the value already in *out. False when the text holds no valid setting at all.
bool parse_launch_options(const std::string& text, LaunchOptions* out);
std::string serialize_launch_options(const LaunchOptions& options);

// Button bits are rcomp/input.h RCOMP_XINPUT_* values.
class LaunchMenu {
public:
    // `title` is shown at the top (upper-cased; characters without a glyph are drawn blank).
    // `countdown_seconds` > 0: the game starts by itself after that long without any input, so unattended
    // launches still reach the game; the first button press stops the countdown.
    LaunchMenu(std::string title, LaunchOptions initial, uint32_t countdown_seconds);

    // One controller sample at time `now_s` (seconds, monotonic). Returns true once the player has started
    // the game (Cross on START GAME, or the OPTIONS button anywhere) or the countdown ran out.
    bool update(uint16_t buttons, double now_s);
    const LaunchOptions& options() const { return options_; }
    bool started() const { return started_; }
    uint32_t selected_row() const { return row_; }

    // Draws the screen into `rgbx` (width * height * 4 bytes: R, G, B, X), resizing it.
    void render(std::vector<uint8_t>* rgbx, uint32_t width, uint32_t height, double now_s) const;

    static constexpr uint32_t kRows = 3;  // resolution, frame rate, start

private:
    std::string title_;
    LaunchOptions options_;
    uint32_t countdown_ = 0;
    double first_update_s_ = -1.0;
    bool counting_ = true;
    bool started_ = false;
    uint32_t row_ = kRows - 1;  // START GAME is preselected
    uint16_t previous_ = 0;
    bool primed_ = false;       // the buttons held when the menu appeared do not count as presses
};

// Label of a draw scale ("3840 X 2160 (4K)"), for the menu and the log.
const char* draw_scale_label(uint32_t scale);
// Label of a frame-rate cap ("30 FPS" ... "60 FPS", "UNLOCKED").
const char* frame_rate_cap_label(uint32_t cap);

}  // namespace rcomp::app
