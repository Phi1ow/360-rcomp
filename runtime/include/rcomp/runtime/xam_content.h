// Content packages (saves) and profile settings of the local player (owner: Agent 3, runtime/).
//
// One storage device exists: the hard drive (device id 1), backed by a host directory the title owner
// configures (on the PS5 the title's own app folder, /app0/savedata, which R-comp Installer never deletes).
// A content package is a directory; XamContentCreateEx mounts it as the title's root name ("save:")
// read-write and XamContentClose unmounts it. Layout under the save root:
//   <XUID>/<TitleID>/<ContentType>/<FileName>/           package contents (what the title writes)
//   <XUID>/<TitleID>/<ContentType>/<FileName>.xcontent   metadata: display name, creator
//   <XUID>/<TitleID>/<ContentType>/<FileName>.thumbnail  thumbnail set by XamContentSetThumbnail
//   <XUID>/<TitleID>/profile/<SettingId>.setting         title-specific profile settings
// XUID is the local profile's (xam_profile.h) for saved games, 0000000000000000 for shared content.
// All names are 8 or 16 upper-case hex digits; FileName is the title's XCONTENT_DATA file name after
// validation (no separators, no "..", printable ASCII only).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

constexpr uint32_t kContentDeviceHdd = 1;
constexpr uint32_t kContentTypeSavedGame = 1;
constexpr uint32_t kXContentDataBytes = 0x134;

// Sets the host directory that backs the hard drive. It is created if missing. Conflict when a different
// root was already configured for this runtime; InvalidArgument for an empty path.
Status runtime_configure_save_root(const std::string& host_dir);
// The configured root, empty when none (content calls then fail as with no storage device).
std::string save_root();

Status register_xam_content_hle();
void reset_xam_content();

// Existing packages for an enumerator: one XCONTENT_DATA (0x134 bytes, guest byte order) per package of
// `content_type` (0 = every type) on `device_id` (0 = any device), for `user_index`, in name order.
Status list_content(uint32_t user_index, uint32_t device_id, uint32_t content_type,
                    std::vector<std::vector<uint8_t>>* items);

}  // namespace rcomp::rt
