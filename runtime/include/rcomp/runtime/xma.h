// XMA audio hardware model (register window, context array). See runtime/docs/XMA.md.
#pragma once

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Registers XMACreateContext/XMAReleaseContext, allocates the physical context
// array and installs the 0x7FEA0000 register provider. Idempotent per Runtime.
Status register_xboxkrnl_xma_hle();
// Marks the model unusable (called from runtime shutdown).
void reset_xma();

}  // namespace rcomp::rt
