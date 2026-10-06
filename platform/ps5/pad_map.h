// PS5 pad sample -> XINPUT state (owner: Agent 2). Pure, host-testable
// (platform/tests/test_pad_map.cpp).
//
// Sample layout and button bits: facts verified on console by ProsperoLight
// and recorded in PS5_RetroArch@18dc105 src/input_ps5.cpp (GPL-3.0; only the
// ABI facts are used, no code): 120-byte samples, u32 buttons at 0x00, sticks
// (0..255, 128 centred, y down) at 0x04..0x07, L2/R2 at 0x08/0x09, s32
// connected at 0x4C; bit 31 = the shell is intercepting the pad.
#pragma once

#include <stdint.h>

#include "rcomp/input.h"

namespace rcomp::ps5pad {

constexpr uint32_t kL3 = 0x000002, kR3 = 0x000004, kOptions = 0x000008, kUp = 0x000010, kRight = 0x000020,
                   kDown = 0x000040, kLeft = 0x000080, kL1 = 0x000400, kR1 = 0x000800, kTriangle = 0x001000,
                   kCircle = 0x002000, kCross = 0x004000, kSquare = 0x008000, kTouchPad = 0x100000,
                   kIntercepted = 0x80000000u;
constexpr uint32_t kSampleSize = 120, kConnectedOffset = 0x4C;

// 0..255 (128 centre) -> -32768..32767 with 128 -> 0 exactly.
inline int16_t axis(uint8_t b) {
    const int d = int(b) - 128;
    return int16_t(d >= 0 ? d * 32767 / 127 : d * 256);
}
// Vertical axes: the pad reports down as positive, XInput up as positive.
inline int16_t axis_up(uint8_t b) {
    const int v = -int(axis(b));
    return int16_t(v > 32767 ? 32767 : v);
}

// Cross/Circle/Square/Triangle -> A/B/X/Y (same positions), Options -> START,
// touch pad click -> BACK. While the shell intercepts the pad, the state is
// neutral (nothing pressed, sticks centred).
inline rcomp_pad from_sample(uint32_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, uint8_t l2,
                             uint8_t r2) {
    rcomp_pad p{};
    if (buttons & kIntercepted) return p;
    const struct {
        uint32_t ps5;
        uint16_t xinput;
    } map[] = {{kUp, RCOMP_XINPUT_DPAD_UP},       {kDown, RCOMP_XINPUT_DPAD_DOWN},
               {kLeft, RCOMP_XINPUT_DPAD_LEFT},   {kRight, RCOMP_XINPUT_DPAD_RIGHT},
               {kOptions, RCOMP_XINPUT_START},    {kTouchPad, RCOMP_XINPUT_BACK},
               {kL3, RCOMP_XINPUT_LEFT_THUMB},    {kR3, RCOMP_XINPUT_RIGHT_THUMB},
               {kL1, RCOMP_XINPUT_LEFT_SHOULDER}, {kR1, RCOMP_XINPUT_RIGHT_SHOULDER},
               {kCross, RCOMP_XINPUT_A},          {kCircle, RCOMP_XINPUT_B},
               {kSquare, RCOMP_XINPUT_X},         {kTriangle, RCOMP_XINPUT_Y}};
    for (const auto& m : map)
        if (buttons & m.ps5) p.buttons |= m.xinput;
    p.left_trigger = l2;
    p.right_trigger = r2;
    p.thumb_lx = axis(lx);
    p.thumb_ly = axis_up(ly);
    p.thumb_rx = axis(rx);
    p.thumb_ry = axis_up(ry);
    return p;
}

}  // namespace rcomp::ps5pad
