// Registration entry points of the HLE units added for the second title wave (Halo 3, NARUTO STORM 3,
// Skate 3, Fallout: New Vegas, Gears of War 2). Each is called by the registration function of the
// existing unit it extends, so the public registration order (register_xboxkrnl_hle /
// register_xam_hle) is unchanged. Not a public interface.
#pragma once

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// src/hle_xboxkrnl_crypto_more.cpp: XeCryptAes*, XeCryptRandom, XeCryptBn* (from the strings unit).
Status register_xboxkrnl_crypto_more_hle();
// src/hle_xboxkrnl_xma_api.cpp: the XMA* context helpers (from the XMA unit, after its device exists).
Status register_xboxkrnl_xma_api_hle();
// src/hle_xboxkrnl_audio_ducker.cpp: XAudio*Ducker* (from the XAudio unit).
Status register_xboxkrnl_audio_ducker_hle();
// src/hle_xam_more.cpp: overlapped results, custom gamercard actions, notifications delay, licences,
// device context, voice process state, custom message compose (from register_xam_hle).
Status register_xam_more_hle();
// src/hle_xam_net_more.cpp: XNet keys, connect, address and QoS queries (from the NetDll unit).
Status register_xam_net_more_hle();

}  // namespace rcomp::rt
