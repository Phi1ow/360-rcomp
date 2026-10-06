// The local player profile (owner: Agent 3, runtime/).
//
// The PS5 has no Xbox gamertag or Live account, so the title sees one local, offline profile signed in
// on controller slot 0: XamUserGetSigninState answers "signed in locally" (1) for that slot and "not
// signed in" for the others; every Live-only service (presence, friends, marketplace, QoS, sessions on
// Live) answers as a console whose profile is not signed in to Xbox LIVE. Its saves and profile settings
// persist in the title's own app folder (see xam_content.h), so they survive reinstalls by R-comp
// Installer, which never deletes on the console.
#pragma once

#include <stdint.h>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

constexpr uint32_t kLocalUserIndex = 0;
// Sign-in states of XamUserGetSigninState / X_USER_SIGNIN_INFO.SigninState.
constexpr uint32_t kSigninStateNotSignedIn = 0, kSigninStateSignedInLocally = 1;
// An offline XUID (high byte 0xE0, as consoles assign to local profiles), stable across runs so the
// saves bound to it stay readable.
constexpr uint64_t kLocalUserXuid = 0xE000000052434F4Dull;  // "RCOM"
// The gamertag the title displays (15 characters at most on the console).
constexpr const char* kLocalUserName = "Player";

inline bool is_local_user(uint32_t user_index) { return user_index == kLocalUserIndex; }

// XamUserReadProfileSettings / XamUserWriteProfileSettings (runtime/src/hle_xam_profile.cpp).
Status register_xam_profile_hle();

}  // namespace rcomp::rt
