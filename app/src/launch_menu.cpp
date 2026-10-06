#include "rcomp/app/launch_menu.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cstdlib>

#include "rcomp/input.h"

namespace rcomp::app {
namespace {

struct Glyph {
    char character;
    uint8_t rows[7];  // five bits per row, the most significant of the five is the leftmost pixel
};

// 5x7 capitals, digits and the punctuation the menu uses.
constexpr Glyph kGlyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'&', {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}},
    {'\'', {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}},
    {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
};

const Glyph* glyph(char c) {
    c = char(toupper((unsigned char)c));
    for (const Glyph& g : kGlyphs)
        if (g.character == c) return &g;
    return nullptr;
}

struct Color {
    uint8_t r, g, b;
};
constexpr Color kBackground = {10, 16, 13}, kPanel = {22, 34, 28}, kSelected = {46, 120, 76},
                kText = {232, 240, 235}, kMuted = {130, 160, 142}, kAccent = {120, 220, 160};

class Canvas {
public:
    Canvas(std::vector<uint8_t>* rgbx, uint32_t w, uint32_t h) : p_(rgbx), w_(w), h_(h) { p_->assign(size_t(w) * h * 4, 0); }
    void fill(int x, int y, int w, int h, Color c) {
        const int x0 = std::max(0, x), y0 = std::max(0, y);
        const int x1 = std::min<int>(int(w_), x + w), y1 = std::min<int>(int(h_), y + h);
        for (int yy = y0; yy < y1; ++yy)
            for (int xx = x0; xx < x1; ++xx) {
                uint8_t* px = p_->data() + (size_t(yy) * w_ + xx) * 4;
                px[0] = c.r; px[1] = c.g; px[2] = c.b; px[3] = 0xFF;
            }
    }
    static int text_width(const std::string& text, int scale) { return int(text.size()) * 6 * scale - scale; }
    void text(int x, int y, int scale, Color c, const std::string& s) {
        for (char ch : s) {
            if (const Glyph* g = glyph(ch))
                for (int row = 0; row < 7; ++row)
                    for (int col = 0; col < 5; ++col)
                        if (g->rows[row] & (0x10 >> col)) fill(x + col * scale, y + row * scale, scale, scale, c);
            x += 6 * scale;
        }
    }
    void centered(int y, int scale, Color c, const std::string& s) { text((int(w_) - text_width(s, scale)) / 2, y, scale, c, s); }
    // Centred at the largest scale <= max_scale that fits the width minus a margin (at least scale 1).
    void fitted(int y, int max_scale, int margin, Color c, const std::string& s) {
        int scale = max_scale;
        while (scale > 1 && text_width(s, scale) > int(w_) - 2 * margin) --scale;
        centered(y, scale, c, s);
    }
    uint32_t width() const { return w_; }

private:
    std::vector<uint8_t>* p_;
    uint32_t w_, h_;
};

constexpr uint16_t kUp = RCOMP_XINPUT_DPAD_UP, kDown = RCOMP_XINPUT_DPAD_DOWN, kLeft = RCOMP_XINPUT_DPAD_LEFT,
                   kRight = RCOMP_XINPUT_DPAD_RIGHT, kConfirm = RCOMP_XINPUT_A, kStart = RCOMP_XINPUT_START;

// The frame-rate caps the menu offers, in the order its row cycles through them (0 = unlocked).
constexpr uint32_t kFrameRateCaps[] = {0, 30, 40, 50, 60};
constexpr size_t kFrameRateCapCount = sizeof(kFrameRateCaps) / sizeof(kFrameRateCaps[0]);

bool valid_frame_rate_cap(uint32_t cap) {
    for (uint32_t c : kFrameRateCaps)
        if (c == cap) return true;
    return false;
}

// The cap one place after (step > 0) or before (step < 0) `cap` in the list, wrapping around; an unknown cap counts as unlocked.
uint32_t stepped_frame_rate_cap(uint32_t cap, int step) {
    size_t index = 0;
    for (size_t i = 0; i < kFrameRateCapCount; ++i)
        if (kFrameRateCaps[i] == cap) index = i;
    const size_t delta = step > 0 ? 1 : kFrameRateCapCount - 1;
    return kFrameRateCaps[(index + delta) % kFrameRateCapCount];
}
}  // namespace

const char* draw_scale_label(uint32_t scale) {
    switch (scale) {
    case 1: return "1280 X 720";
    case 2: return "2560 X 1440";
    case 3: return "3840 X 2160 (4K)";
    default: return "?";
    }
}

const char* frame_rate_cap_label(uint32_t cap) {
    switch (cap) {
    case 30: return "30 FPS";
    case 40: return "40 FPS";
    case 50: return "50 FPS";
    case 60: return "60 FPS";
    default: return "UNLOCKED";
    }
}

bool parse_launch_options(const std::string& text, LaunchOptions* out) {
    bool any = false;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(i, end - i);
        i = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "draw_scale" && (value == "1" || value == "2" || value == "3")) {
            out->draw_scale = uint32_t(value[0] - '0');
            any = true;
        } else if (key == "frame_rate_cap") {
            for (uint32_t cap : kFrameRateCaps) {
                if (value == std::to_string(cap)) {
                    out->frame_rate_cap = cap;
                    any = true;
                }
            }
        }
    }
    return any;
}

std::string serialize_launch_options(const LaunchOptions& options) {
    return "draw_scale=" + std::to_string(options.draw_scale) + "\nframe_rate_cap=" +
           std::to_string(options.frame_rate_cap) + "\n";
}

LaunchMenu::LaunchMenu(std::string title, LaunchOptions initial, uint32_t countdown_seconds)
    : title_(std::move(title)), options_(initial), countdown_(countdown_seconds), counting_(countdown_seconds > 0) {
    if (options_.draw_scale < 1 || options_.draw_scale > 3) options_.draw_scale = 3;
    if (!valid_frame_rate_cap(options_.frame_rate_cap)) options_.frame_rate_cap = 0;
}

bool LaunchMenu::update(uint16_t buttons, double now_s) {
    if (started_) return true;
    if (first_update_s_ < 0) first_update_s_ = now_s;
    if (!primed_) {
        // A button already held when the menu appears (the one that launched the title) is not a press.
        primed_ = true;
        previous_ = buttons;
    }
    const uint16_t pressed = uint16_t(buttons & ~previous_);
    previous_ = buttons;
    if (pressed) counting_ = false;
    if (pressed & kUp) row_ = (row_ + kRows - 1) % kRows;
    if (pressed & kDown) row_ = (row_ + 1) % kRows;
    const int step = (pressed & kRight) ? 1 : (pressed & kLeft) ? -1 : ((pressed & kConfirm) && row_ < kRows - 1) ? 1 : 0;
    if (step && row_ == 0) options_.draw_scale = uint32_t((int(options_.draw_scale) - 1 + step + 3) % 3 + 1);
    if (step && row_ == 1) options_.frame_rate_cap = stepped_frame_rate_cap(options_.frame_rate_cap, step);
    if ((pressed & kStart) || ((pressed & kConfirm) && row_ == kRows - 1)) started_ = true;
    if (counting_ && now_s - first_update_s_ >= double(countdown_)) started_ = true;
    return started_;
}

void LaunchMenu::render(std::vector<uint8_t>* rgbx, uint32_t width, uint32_t height, double now_s) const {
    Canvas c(rgbx, width, height);
    c.fill(0, 0, int(width), int(height), kBackground);
    const int unit = std::max(1, int(height) / 180);  // 4 at 720 lines
    std::string title = title_;
    for (char& ch : title) ch = char(toupper((unsigned char)ch));
    // A long name goes on two lines, split at the space nearest its middle.
    const int margin = 8 * unit;
    if (Canvas::text_width(title, unit * 2) > int(width) - 2 * margin && title.find(' ') != std::string::npos) {
        size_t split = title.find(' ');
        for (size_t i = 0; i < title.size(); ++i)
            if (title[i] == ' ' && std::abs(int(i) - int(title.size() / 2)) < std::abs(int(split) - int(title.size() / 2))) split = i;
        c.fitted(8 * unit, unit * 2, margin, kText, title.substr(0, split));
        c.fitted(24 * unit, unit * 2, margin, kText, title.substr(split + 1));
    } else {
        c.fitted(16 * unit, unit * 2, margin, kText, title);
    }
    c.fitted(44 * unit, unit, margin, kAccent, "GRAPHICS OPTIONS");

    const int panel_w = std::min(int(width) - 16 * unit, 240 * unit), panel_x = (int(width) - panel_w) / 2;
    const std::string labels[kRows] = {"RESOLUTION", "FRAME RATE", ""};
    const std::string values[kRows] = {draw_scale_label(options_.draw_scale),
                                       frame_rate_cap_label(options_.frame_rate_cap), "START GAME"};
    for (uint32_t r = 0; r < kRows; ++r) {
        const int y = (58 + int(r) * 25) * unit;
        const bool selected = r == row_;
        c.fill(panel_x, y, panel_w, 20 * unit, selected ? kSelected : kPanel);
        if (r < kRows - 1) {
            c.text(panel_x + 6 * unit, y + 6 * unit, unit, selected ? kText : kMuted, labels[r]);
            const std::string value = (selected ? "< " : "  ") + values[r] + (selected ? " >" : "  ");
            c.text(panel_x + panel_w - 6 * unit - Canvas::text_width(value, unit), y + 6 * unit, unit, kText, value);
        } else {
            c.text(panel_x + (panel_w - Canvas::text_width(values[r], unit)) / 2, y + 6 * unit, unit, kText, values[r]);
        }
    }
    c.fitted(140 * unit, unit, margin, kMuted, "D-PAD: CHOOSE AND CHANGE   CROSS: CONFIRM   OPTIONS: START");
    if (counting_ && !started_) {
        const double elapsed = first_update_s_ < 0 ? 0.0 : now_s - first_update_s_;
        const int left = std::max(0, int(double(countdown_) - elapsed + 0.999));
        c.fitted(154 * unit, unit, margin, kAccent, "STARTING IN " + std::to_string(left) + " S - PRESS A BUTTON TO CHANGE");
    }
    c.fitted(168 * unit, unit, margin, kMuted, "R-COMP: YOUR CHOICE IS KEPT FOR THE NEXT LAUNCH");
}

}  // namespace rcomp::app
