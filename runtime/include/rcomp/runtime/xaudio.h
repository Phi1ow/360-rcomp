// xboxkrnl XAudio render driver (frame clock + client callbacks).
// See runtime/docs/XAUDIO.md.
#pragma once

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Registers XAudio{Register,Unregister}RenderDriverClient and
// XAudioSubmitRenderDriverFrame. The frame clock thread starts with the first
// client.
Status register_xboxkrnl_xaudio_hle();
// Stops and joins the frame clock; called by runtime_shutdown before guest
// memory is released. Idempotent.
void shutdown_xaudio();

}  // namespace rcomp::rt
