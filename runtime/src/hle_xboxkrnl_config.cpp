// xboxkrnl.exe!ExGetXConfigSetting over an explicit guest XConfig profile.
#include "rcomp/runtime/xconfig.h"

#include <cstring>
#include <mutex>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kExGetXConfigSettingOrdinal = 0x0010;

struct XConfigState {
  std::mutex mutex;
  bool configured = false;
  uint64_t generation = 0;
  XConfigProfile profile{};
};

XConfigState g_xconfig;

enum class SettingKind {
  AvRegion,
  TimeZoneBias,
  TimeZoneStdName,
  TimeZoneDltName,
  TimeZoneStdDate,
  TimeZoneDltDate,
  TimeZoneStdBias,
  TimeZoneDltBias,
  Language,
  VideoFlags,
  RetailFlags,
};

struct SettingValue {
  uint16_t size = 0;
  uint8_t bytes[4]{};
};

bool valid_av_region(XConfigAvRegion region) {
  switch (region) {
  case XConfigAvRegion::NtscM:
  case XConfigAvRegion::NtscJ:
  case XConfigAvRegion::Pal50:
  case XConfigAvRegion::Pal60:
    return true;
  default:
    return false;
  }
}

bool valid_connector(XConfigVideoConnector connector) {
  switch (connector) {
  case XConfigVideoConnector::Hdmi:
  case XConfigVideoConnector::Component:
  case XConfigVideoConnector::Vga:
    return true;
  default:
    return false;
  }
}

bool valid_profile(const XConfigProfile &p) {
  return p.language != 0 && valid_av_region(p.av_region) &&
         p.video.display_width != 0 && p.video.display_height != 0 &&
         p.video.refresh_millihz != 0 && valid_connector(p.video.connector);
}

void store_be32(uint8_t *p, uint32_t value) {
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

bool write_be16(Runtime &r, uint32_t address, uint16_t value) {
  uint8_t *p = r.mem->translate(address, 2);
  if (!p)
    return false;
  p[0] = uint8_t(value >> 8);
  p[1] = uint8_t(value);
  return true;
}

bool classify(uint16_t category, uint16_t setting, SettingKind *kind) {
  if (category == xconfig::kSecuredCategory) {
    if (setting != xconfig::kSecuredAvRegion)
      return false;
    *kind = SettingKind::AvRegion;
    return true;
  }
  if (category != xconfig::kUserCategory)
    return false;
  switch (setting) {
  case xconfig::kUserTimeZoneBias:
    *kind = SettingKind::TimeZoneBias;
    return true;
  case xconfig::kUserTimeZoneStdName:
    *kind = SettingKind::TimeZoneStdName;
    return true;
  case xconfig::kUserTimeZoneDltName:
    *kind = SettingKind::TimeZoneDltName;
    return true;
  case xconfig::kUserTimeZoneStdDate:
    *kind = SettingKind::TimeZoneStdDate;
    return true;
  case xconfig::kUserTimeZoneDltDate:
    *kind = SettingKind::TimeZoneDltDate;
    return true;
  case xconfig::kUserTimeZoneStdBias:
    *kind = SettingKind::TimeZoneStdBias;
    return true;
  case xconfig::kUserTimeZoneDltBias:
    *kind = SettingKind::TimeZoneDltBias;
    return true;
  case xconfig::kUserLanguage:
    *kind = SettingKind::Language;
    return true;
  case xconfig::kUserVideoFlags:
    *kind = SettingKind::VideoFlags;
    return true;
  case xconfig::kUserRetailFlags:
    *kind = SettingKind::RetailFlags;
    return true;
  default:
    return false;
  }
}

bool snapshot_profile(XConfigProfile *out) {
  Runtime *r = runtime();
  if (!r)
    return false;
  std::lock_guard<std::mutex> lock(g_xconfig.mutex);
  if (!g_xconfig.configured || g_xconfig.generation != r->generation)
    return false;
  *out = g_xconfig.profile;
  return true;
}

bool build_setting(const XConfigProfile &p, SettingKind kind,
                   SettingValue *out) {
  out->size = 4;
  switch (kind) {
  case SettingKind::AvRegion:
    store_be32(out->bytes, static_cast<uint32_t>(p.av_region));
    return true;
  case SettingKind::Language:
    store_be32(out->bytes, p.language);
    return true;
  case SettingKind::VideoFlags:
    store_be32(out->bytes,
               p.video.widescreen ? xconfig::kVideoFlagWidescreen : 0u);
    return true;
  case SettingKind::RetailFlags:
    if (!p.retail_flags.configured)
      return false;
    store_be32(out->bytes, p.retail_flags.value);
    return true;
  default:
    if (!p.time_zone.configured)
      return false;
    break;
  }

  switch (kind) {
  case SettingKind::TimeZoneBias:
    store_be32(out->bytes, p.time_zone.bias);
    break;
  case SettingKind::TimeZoneStdName:
    std::memcpy(out->bytes, p.time_zone.standard_name, 4);
    break;
  case SettingKind::TimeZoneDltName:
    std::memcpy(out->bytes, p.time_zone.daylight_name, 4);
    break;
  case SettingKind::TimeZoneStdDate:
    out->bytes[0] = p.time_zone.standard_date.month;
    out->bytes[1] = p.time_zone.standard_date.day;
    out->bytes[2] = p.time_zone.standard_date.day_of_week;
    out->bytes[3] = p.time_zone.standard_date.hour;
    break;
  case SettingKind::TimeZoneDltDate:
    out->bytes[0] = p.time_zone.daylight_date.month;
    out->bytes[1] = p.time_zone.daylight_date.day;
    out->bytes[2] = p.time_zone.daylight_date.day_of_week;
    out->bytes[3] = p.time_zone.daylight_date.hour;
    break;
  case SettingKind::TimeZoneStdBias:
    store_be32(out->bytes, p.time_zone.standard_bias);
    break;
  case SettingKind::TimeZoneDltBias:
    store_be32(out->bytes, p.time_zone.daylight_bias);
    break;
  default:
    return false;
  }
  return true;
}

void ExGetXConfigSetting(PPCContext &c, uint8_t *) {
  Runtime *r = runtime();
  if (!r || !r->mem) {
    c.r3.u64 = nt::kUnsuccessful;
    return;
  }

  const uint16_t category = uint16_t(c.r3.u32);
  const uint16_t setting = uint16_t(c.r4.u32);
  const uint32_t buffer = c.r5.u32;
  const uint16_t buffer_size = uint16_t(c.r6.u32);
  const uint32_t required_size = c.r7.u32;

  if (required_size &&
      !r->mem->is_accessible(required_size, 2, Protect::ReadWrite)) {
    c.r3.u64 = nt::kAccessViolation;
    return;
  }
  if (!buffer && buffer_size) {
    c.r3.u64 = xconfig::kStatusInvalidParameter3;
    return;
  }

  SettingKind kind{};
  if (category != xconfig::kSecuredCategory &&
      category != xconfig::kUserCategory) {
    c.r3.u64 = xconfig::kStatusInvalidParameter1;
    return;
  }
  if (!classify(category, setting, &kind)) {
    c.r3.u64 = xconfig::kStatusInvalidParameter2;
    return;
  }

  XConfigProfile profile{};
  if (!snapshot_profile(&profile)) {
    c.r3.u64 = xconfig::kStatusNotSupported;
    return;
  }
  SettingValue value{};
  if (!build_setting(profile, kind, &value)) {
    c.r3.u64 = xconfig::kStatusNotSupported;
    return;
  }

  if (buffer) {
    if (buffer_size < value.size) {
      c.r3.u64 = xconfig::kStatusBufferTooSmall;
      return;
    }
    if (!r->mem->is_accessible(buffer, value.size, Protect::ReadWrite)) {
      c.r3.u64 = nt::kAccessViolation;
      return;
    }
  }

  // Every possible target has been validated before the first guest write.
  if (buffer) {
    uint8_t *out = r->mem->translate(buffer, value.size);
    if (!out) {
      c.r3.u64 = nt::kAccessViolation;
      return;
    }
    std::memcpy(out, value.bytes, value.size);
  }
  if (required_size && !write_be16(*r, required_size, value.size)) {
    c.r3.u64 = nt::kAccessViolation;
    return;
  }
  c.r3.u64 = nt::kSuccess;
}

} // namespace

Status runtime_configure_xconfig(const XConfigProfile &profile) {
  Runtime *r = runtime();
  if (!r)
    return Status::NotInitialized;
  if (!valid_profile(profile))
    return Status::InvalidArgument;
  std::lock_guard<std::mutex> lock(g_xconfig.mutex);
  if (g_xconfig.configured && g_xconfig.generation == r->generation)
    return Status::Conflict;
  g_xconfig.profile = profile;
  g_xconfig.generation = r->generation;
  g_xconfig.configured = true;
  return Status::Ok;
}

void runtime_reset_xconfig() {
  std::lock_guard<std::mutex> lock(g_xconfig.mutex);
  g_xconfig.profile = {};
  g_xconfig.generation = 0;
  g_xconfig.configured = false;
}

Status register_xboxkrnl_xconfig_hle() {
  struct Impl { uint32_t ordinal; const char* name; PPCFunc* function; };
  const Impl entries[] = {{0x0010, "ExGetXConfigSetting", &ExGetXConfigSetting}};
  for (const auto& entry : entries) {
    uint32_t ordinal = 0;
    if (!export_ordinal(kModuleXboxkrnl, entry.name, &ordinal) ||
        ordinal != entry.ordinal || ordinal != kExGetXConfigSettingOrdinal)
      return Status::Conflict;
    const Status status = register_import(kModuleXboxkrnl, entry.ordinal, entry.function, entry.name);
    if (status != Status::Ok) return status;
  }
  return Status::Ok;
}

} // namespace rcomp::rt
