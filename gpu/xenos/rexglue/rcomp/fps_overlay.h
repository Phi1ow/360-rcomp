// R-comp: text of the on-screen frame-rate counter (owner: gpu/xenos).
//
// A 5x7 bitmap font and a renderer with no Vulkan dependency, so the layout is
// tested on the host (tests/fps_overlay). VulkanDisplay copies the bitmap into
// the swapchain image after the frame blit. Pixels are opaque black
// (0xFF000000) and opaque white (0xFFFFFFFF): the same little-endian word in
// R8G8B8A8 and B8G8R8A8, so the swapchain format does not matter.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rcomp::xenos {

constexpr size_t kOverlayMaxChars = 48;
constexpr uint32_t kOverlayMaxScale = 16;
constexpr uint32_t kOverlayBlack = 0xFF000000u;
constexpr uint32_t kOverlayWhite = 0xFFFFFFFFu;

struct OverlayGlyph {
    char character;
    uint8_t rows[7];  // five bits per row, the most significant of the five is the leftmost pixel
};

// nullptr for a character without a glyph (drawn blank).
inline const OverlayGlyph* overlay_glyph(char c) {
    static const OverlayGlyph kGlyphs[] = {
        {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
        {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
        {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
        {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
        {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
        {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
        {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
        {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
        {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
        {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
        {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
        {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
        {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
        {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
        {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
        {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    };
    for (const OverlayGlyph& glyph : kGlyphs)
        if (glyph.character == c) return &glyph;
    return nullptr;
}

struct OverlaySize {
    uint32_t width = 0, height = 0;
};

// Size of the bitmap for `chars` characters: a 6x8 cell per character (5x7
// glyph plus one column and one row of spacing) and a black border of two
// glyph pixels, everything multiplied by `scale`. False when out of range.
inline bool overlay_size(size_t chars, uint32_t scale, OverlaySize* out) {
    if (!out || chars == 0 || chars > kOverlayMaxChars || scale == 0 || scale > kOverlayMaxScale) return false;
    const uint32_t border = 2 * scale;
    out->width = uint32_t(chars) * 6 * scale + 2 * border;
    out->height = 8 * scale + 2 * border;
    return true;
}

// Renders `text` (`chars` characters, no terminator needed) into `pixels`:
// width x height words, row-major and tightly packed. False, with `pixels`
// untouched, if the text or scale is out of range or `capacity` (in words) is
// too small.
inline bool overlay_render(const char* text, size_t chars, uint32_t scale, uint32_t* pixels, size_t capacity,
                           OverlaySize* size_out) {
    OverlaySize size;
    if (!text || !pixels || !overlay_size(chars, scale, &size)) return false;
    const size_t words = size_t(size.width) * size.height;
    if (capacity < words) return false;
    for (size_t i = 0; i < words; ++i) pixels[i] = kOverlayBlack;
    const uint32_t border = 2 * scale;
    for (size_t n = 0; n < chars; ++n) {
        const OverlayGlyph* glyph = overlay_glyph(text[n]);
        if (!glyph) continue;
        for (uint32_t row = 0; row < 7; ++row) {
            for (uint32_t column = 0; column < 5; ++column) {
                if (!((glyph->rows[row] >> (4 - column)) & 1u)) continue;
                const uint32_t x = border + (uint32_t(n) * 6 + column) * scale;
                const uint32_t y = border + row * scale;
                for (uint32_t dy = 0; dy < scale; ++dy)
                    for (uint32_t dx = 0; dx < scale; ++dx)
                        pixels[size_t(y + dy) * size.width + x + dx] = kOverlayWhite;
            }
        }
    }
    if (size_out) *size_out = size;
    return true;
}

}  // namespace rcomp::xenos
