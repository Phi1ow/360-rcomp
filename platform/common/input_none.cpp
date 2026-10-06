// Input backend for builds without a controller (host titles): no controller
// is connected (owner: Agent 2). A real state, not a stub: XAM reports
// ERROR_DEVICE_NOT_CONNECTED to the title.
#include "rcomp/input.h"

extern "C" int rcomp_input_read(uint32_t, rcomp_pad*) { return RCOMP_INPUT_NOT_CONNECTED; }
extern "C" int rcomp_input_set_vibration(uint32_t, uint16_t, uint16_t) { return RCOMP_INPUT_NOT_CONNECTED; }
