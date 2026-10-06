// xboxkrnl XConfig query state (isolated until PRIME wires it into Runtime).
#pragma once

#include <stdint.h>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

namespace xconfig {
constexpr uint16_t kSecuredCategory = 0x0002;
constexpr uint16_t kSecuredAvRegion = 0x0002;
constexpr uint16_t kUserCategory = 0x0003;
constexpr uint16_t kUserTimeZoneBias = 0x0001;
constexpr uint16_t kUserTimeZoneStdName = 0x0002;
constexpr uint16_t kUserTimeZoneDltName = 0x0003;
constexpr uint16_t kUserTimeZoneStdDate = 0x0004;
constexpr uint16_t kUserTimeZoneDltDate = 0x0005;
constexpr uint16_t kUserTimeZoneStdBias = 0x0006;
constexpr uint16_t kUserTimeZoneDltBias = 0x0007;
constexpr uint16_t kUserLanguage = 0x0009;
constexpr uint16_t kUserVideoFlags = 0x000A;
constexpr uint16_t kUserRetailFlags = 0x000C;

constexpr uint32_t kVideoFlagWidescreen = 0x00010000u;

// NTSTATUS values returned by the Xbox kernel export. Parameter suffixes name
// the 1-based argument whose value is invalid.
constexpr uint32_t kStatusBufferTooSmall = 0xC0000023u;
constexpr uint32_t kStatusNotSupported = 0xC00000BBu;
constexpr uint32_t kStatusInvalidParameter1 = 0xC00000EFu;
constexpr uint32_t kStatusInvalidParameter2 = 0xC00000F0u;
constexpr uint32_t kStatusInvalidParameter3 = 0xC00000F1u;
} // namespace xconfig

// Guest AVRegion values. These are the full 32-bit values stored in the
// secured XConfig record, not the one-byte Free60 display-standard index.
enum class XConfigAvRegion : uint32_t {
  Invalid = 0,
  NtscM = 0x00400100u,
  NtscJ = 0x00400200u,
  Pal50 = 0x00800300u,
  Pal60 = 0x00400400u,
};

// Internal profile fact. Connector kind is retained so bootstrap configuration
// can describe the display it actually presents; setting 0x0A itself only
// exposes the documented aspect-ratio flag.
enum class XConfigVideoConnector : uint8_t {
  Unknown = 0,
  Hdmi,
  Component,
  Vga,
};

struct XConfigVideoProfile {
  uint32_t display_width = 0;
  uint32_t display_height = 0;
  uint32_t refresh_millihz = 0;
  bool interlaced = false;
  bool widescreen = false;
  bool high_definition = false;
  XConfigVideoConnector connector = XConfigVideoConnector::Unknown;
};

struct XConfigTimeZoneDate {
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t day_of_week = 0;
  uint8_t hour = 0;
};

// Settings 1..7 are one coherent profile. Names and dates are four raw guest
// bytes; bias fields are guest big-endian 32-bit integers.
struct XConfigTimeZoneProfile {
  bool configured = false;
  uint32_t bias = 0;
  uint8_t standard_name[4] = {};
  uint8_t daylight_name[4] = {};
  XConfigTimeZoneDate standard_date{};
  XConfigTimeZoneDate daylight_date{};
  uint32_t standard_bias = 0;
  uint32_t daylight_bias = 0;
};

struct XConfigOptionalU32 {
  bool configured = false;
  uint32_t value = 0;
};

// Bootstrap-owned facts for the active guest. There are deliberately no
// production defaults: language, AV standard and display are supplied beside
// TitleConfig/XamRuntimeConfig, while timezone/retail fields are opt-in.
struct XConfigProfile {
  uint32_t language = 0;
  XConfigAvRegion av_region = XConfigAvRegion::Invalid;
  XConfigVideoProfile video{};
  XConfigTimeZoneProfile time_zone{};
  XConfigOptionalU32 retail_flags{};
};

// Binds a profile to the active Runtime generation. A second configure in the
// same generation is rejected until runtime_reset_xconfig().
Status runtime_configure_xconfig(const XConfigProfile &profile);
void runtime_reset_xconfig();

// Registers xboxkrnl.exe!ExGetXConfigSetting (ordinal 0x0010).
Status register_xboxkrnl_xconfig_hle();

} // namespace rcomp::rt
