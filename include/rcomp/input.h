// Shared interface (owner: PRIME). Game controller state for the XAM input
// exports (runtime/src/hle_xam.cpp), in Xbox 360 XINPUT terms. One backend is
// linked per title:
//   platform/common/input_none.cpp  no controller (host builds)
//   platform/ps5/input_ps5.cpp      the console's pad (scePad)
//   runtime/tests/TESTDOUBLE_input.cpp  scripted states (tests only)
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// XINPUT_GAMEPAD wButtons bits.
enum {
    RCOMP_XINPUT_DPAD_UP = 0x0001,
    RCOMP_XINPUT_DPAD_DOWN = 0x0002,
    RCOMP_XINPUT_DPAD_LEFT = 0x0004,
    RCOMP_XINPUT_DPAD_RIGHT = 0x0008,
    RCOMP_XINPUT_START = 0x0010,
    RCOMP_XINPUT_BACK = 0x0020,
    RCOMP_XINPUT_LEFT_THUMB = 0x0040,
    RCOMP_XINPUT_RIGHT_THUMB = 0x0080,
    RCOMP_XINPUT_LEFT_SHOULDER = 0x0100,
    RCOMP_XINPUT_RIGHT_SHOULDER = 0x0200,
    RCOMP_XINPUT_A = 0x1000,
    RCOMP_XINPUT_B = 0x2000,
    RCOMP_XINPUT_X = 0x4000,
    RCOMP_XINPUT_Y = 0x8000,
};

typedef struct rcomp_pad {
    uint16_t buttons;        // RCOMP_XINPUT_* bits
    uint8_t left_trigger;    // 0..255
    uint8_t right_trigger;
    int16_t thumb_lx;        // -32768..32767, +y = up (XInput convention)
    int16_t thumb_ly;
    int16_t thumb_rx;
    int16_t thumb_ry;
} rcomp_pad;

enum rcomp_input_status {
    RCOMP_INPUT_OK = 0,
    RCOMP_INPUT_NOT_CONNECTED = 1,
    RCOMP_INPUT_UNSUPPORTED = 2,  // e.g. vibration on a backend that cannot drive it
};

// Current state of controller `user` (0..3). Returns RCOMP_INPUT_OK and fills
// *out, or RCOMP_INPUT_NOT_CONNECTED (out untouched).
int rcomp_input_read(uint32_t user, rcomp_pad* out);
// Motor speeds 0..65535. RCOMP_INPUT_OK only if the motors were driven.
int rcomp_input_set_vibration(uint32_t user, uint16_t left_motor, uint16_t right_motor);

#ifdef __cplusplus
}
#endif
