// TESTDOUBLE input backend (include/rcomp/input.h) for runtime/tests only:
// controller 0 returns TESTDOUBLE_pad_state, the others are not connected;
// vibration requests are recorded.
#include "rcomp/input.h"

rcomp_pad TESTDOUBLE_pad_state{};
bool TESTDOUBLE_pad_connected = true;
uint16_t TESTDOUBLE_vibration[2] = {0, 0};

extern "C" int rcomp_input_read(uint32_t user, rcomp_pad* out) {
    if (user != 0 || !TESTDOUBLE_pad_connected) return RCOMP_INPUT_NOT_CONNECTED;
    *out = TESTDOUBLE_pad_state;
    return RCOMP_INPUT_OK;
}
extern "C" int rcomp_input_set_vibration(uint32_t user, uint16_t l, uint16_t r) {
    if (user != 0 || !TESTDOUBLE_pad_connected) return RCOMP_INPUT_NOT_CONNECTED;
    TESTDOUBLE_vibration[0] = l;
    TESTDOUBLE_vibration[1] = r;
    return RCOMP_INPUT_OK;
}
