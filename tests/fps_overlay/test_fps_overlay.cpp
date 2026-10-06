// Host checks of the frame-rate counter text (gpu/xenos/rexglue/rcomp/fps_overlay.h).
// The Vulkan side (copy into the swapchain image) is not exercised here.
#include <cstdio>
#include <cstring>
#include <vector>

#include "fps_overlay.h"

using namespace rcomp::xenos;

static int g_errors = 0;
static void check(bool value, const char* what) {
    if (!value) {
        ++g_errors;
        std::printf("fps_overlay/check failed: %s\n", what);
    }
}
static int report(const char* name, int before) {
    const int errors = g_errors - before;
    std::printf("fps_overlay/%s %s errors=%d\n", name, errors ? "FAIL" : "PASS", errors);
    return g_errors;
}

int main() {
    int mark = 0;

    // Size: 6x8 cell per character plus a border of two glyph pixels, times scale.
    {
        OverlaySize size;
        check(overlay_size(17, 4, &size) && size.width == 17 * 6 * 4 + 16 && size.height == 8 * 4 + 16,
              "17 characters at scale 4 -> 424x48");
        check(overlay_size(1, 1, &size) && size.width == 10 && size.height == 12, "1 character at scale 1 -> 10x12");
        check(!overlay_size(0, 2, &size), "no characters");
        check(!overlay_size(kOverlayMaxChars + 1, 2, &size), "too many characters");
        check(overlay_size(kOverlayMaxChars, kOverlayMaxScale, &size), "largest accepted size");
        check(!overlay_size(4, 0, &size), "scale 0");
        check(!overlay_size(4, kOverlayMaxScale + 1, &size), "scale above the maximum");
        check(!overlay_size(4, 2, nullptr), "null output");
        mark = report("size", mark);
    }

    // Font: every character the counter prints has a glyph of five-bit rows.
    {
        for (const char* p = "0123456789.-:FPSM"; *p; ++p) {
            const OverlayGlyph* glyph = overlay_glyph(*p);
            check(glyph && glyph->character == *p, "glyph exists");
            if (!glyph) continue;
            bool any = false;
            for (uint8_t row : glyph->rows) {
                check(row < 32, "row fits five bits");
                any = any || row != 0;
            }
            check(any, "glyph is not empty");
        }
        check(!overlay_glyph(' '), "space has no glyph");
        check(!overlay_glyph('?'), "unknown character has no glyph");
        mark = report("font", mark);
    }

    // Pixels of "1" at scale 2: row 0 of '1' is 0x04, i.e. the middle column.
    {
        const uint32_t scale = 2;
        OverlaySize size, written;
        check(overlay_size(1, scale, &size), "size of one character");
        std::vector<uint32_t> pixels(size_t(size.width) * size.height, 0x12345678u);
        check(overlay_render("1", 1, scale, pixels.data(), pixels.size(), &written), "render");
        check(written.width == size.width && written.height == size.height, "reported size");
        auto at = [&](uint32_t x, uint32_t y) { return pixels[size_t(y) * size.width + x]; };
        const uint32_t border = 2 * scale;
        // Corners and the border are opaque black.
        check(at(0, 0) == kOverlayBlack && at(size.width - 1, size.height - 1) == kOverlayBlack, "corners black");
        // Column 2 of row 0 -> x = border + 2*scale, y = border, a scale x scale block.
        for (uint32_t dy = 0; dy < scale; ++dy)
            for (uint32_t dx = 0; dx < scale; ++dx)
                check(at(border + 2 * scale + dx, border + dy) == kOverlayWhite, "top of the 1 is white");
        // The pixel left of it (column 1, row 0) is not set in '1'.
        check(at(border + 1 * scale, border) == kOverlayBlack, "column 1 of row 0 stays black");
        // Row 6 of '1' is 0x0E: columns 1..3 set.
        check(at(border + 1 * scale, border + 6 * scale) == kOverlayWhite, "foot of the 1, column 1");
        check(at(border + 3 * scale, border + 6 * scale) == kOverlayWhite, "foot of the 1, column 3");
        check(at(border + 0 * scale, border + 6 * scale) == kOverlayBlack, "foot of the 1, column 0");
        check(at(border + 4 * scale, border + 6 * scale) == kOverlayBlack, "foot of the 1, column 4");
        mark = report("pixels", mark);
    }

    // Only opaque black and white are ever produced; the spacing column stays black.
    {
        const char text[] = "FPS 32.1  31.2 MS";
        const size_t chars = sizeof text - 1;
        OverlaySize size;
        check(overlay_size(chars, 4, &size), "size");
        std::vector<uint32_t> pixels(size_t(size.width) * size.height);
        check(overlay_render(text, chars, 4, pixels.data(), pixels.size(), nullptr), "render");
        size_t white = 0;
        for (uint32_t p : pixels) {
            check(p == kOverlayBlack || p == kOverlayWhite, "only black and white");
            white += p == kOverlayWhite;
        }
        check(white > 0, "some text is drawn");
        // Column 5 of every 6-column cell is spacing.
        const uint32_t border = 2 * 4;
        for (size_t n = 0; n < chars; ++n)
            for (uint32_t y = 0; y < size.height; ++y)
                for (uint32_t dx = 0; dx < 4; ++dx)
                    check(pixels[size_t(y) * size.width + border + (n * 6 + 5) * 4 + dx] == kOverlayBlack,
                          "spacing column stays black");
        mark = report("palette", mark);
    }

    // Unknown characters are blank; a blank string is a plain black box.
    {
        OverlaySize size;
        check(overlay_size(3, 2, &size), "size");
        std::vector<uint32_t> pixels(size_t(size.width) * size.height, 0u);
        check(overlay_render("? ~", 3, 2, pixels.data(), pixels.size(), nullptr), "render");
        for (uint32_t p : pixels) check(p == kOverlayBlack, "blank characters draw nothing");
        mark = report("blank", mark);
    }

    // Bounds: a too small capacity changes nothing; a right-sized one is never overrun.
    {
        OverlaySize size;
        check(overlay_size(8, 3, &size), "size");
        const size_t words = size_t(size.width) * size.height;
        std::vector<uint32_t> small(words - 1, 0xA5A5A5A5u);
        check(!overlay_render("FPS 60.0", 8, 3, small.data(), small.size(), nullptr), "capacity one word short is refused");
        for (uint32_t p : small) check(p == 0xA5A5A5A5u, "refused render leaves the buffer untouched");
        // Guard words before and after the exact-size window.
        std::vector<uint32_t> guarded(words + 2, 0xDEADBEEFu);
        check(overlay_render("FPS 60.0", 8, 3, guarded.data() + 1, words, nullptr), "exact capacity is accepted");
        check(guarded.front() == 0xDEADBEEFu && guarded.back() == 0xDEADBEEFu, "no write outside the window");
        check(!overlay_render(nullptr, 8, 3, guarded.data() + 1, words, nullptr), "null text");
        check(!overlay_render("FPS 60.0", 8, 3, nullptr, words, nullptr), "null pixels");
        check(!overlay_render("FPS 60.0", 0, 3, guarded.data() + 1, words, nullptr), "no characters");
        mark = report("bounds", mark);
    }

    std::printf("fps_overlay/all %s errors=%d\n", g_errors ? "FAIL" : "PASS", g_errors);
    return g_errors ? 1 : 0;
}
