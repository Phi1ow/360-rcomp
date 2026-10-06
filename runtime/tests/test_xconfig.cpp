#include <cstring>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xconfig.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__ExGetXConfigSetting);

using namespace rcomp;
using namespace rcomp::rt;

namespace {
GuestMemory memory;
constexpr uint32_t kScratch = 0x32000000u;
constexpr uint32_t kOutput = kScratch + 0x100;
constexpr uint32_t kRequired = kScratch + 0x200;

uint32_t call(uint32_t category, uint32_t setting, uint32_t buffer,
              uint32_t buffer_size, uint32_t required) {
  alignas(64) PPCContext c{};
  c.r3.u64 = category;
  c.r4.u64 = setting;
  c.r5.u64 = buffer;
  c.r6.u64 = buffer_size;
  c.r7.u64 = required;
  __imp__ExGetXConfigSetting(c, memory.base());
  return c.r3.u32;
}

void fill(uint32_t address, uint8_t value, uint32_t size) {
  uint8_t *p = memory.translate(address, size);
  CHECK(p != nullptr);
  if (p)
    std::memset(p, value, size);
}

void expect_bytes(uint32_t address, const uint8_t expected[4]) {
  const uint8_t *p = memory.translate(address, 4);
  CHECK(p != nullptr);
  if (p)
    CHECK(std::memcmp(p, expected, 4) == 0);
}

uint16_t required_word() {
  uint16_t value = 0;
  CHECK(guest_read_be16(kRequired, &value));
  return value;
}

XConfigProfile profile(uint32_t language, XConfigAvRegion av_region) {
  XConfigProfile p{};
  p.language = language;
  p.av_region = av_region;
  p.video.display_width = 1280;
  p.video.display_height = 720;
  p.video.refresh_millihz = 60000;
  p.video.interlaced = false;
  p.video.widescreen = true;
  p.video.high_definition = true;
  p.video.connector = XConfigVideoConnector::Hdmi;
  p.time_zone.configured = true;
  p.time_zone.bias = 0xFFFFFFC4u;
  const uint8_t std_name[4] = {'C', 'E', 'T', 0};
  const uint8_t dlt_name[4] = {'C', 'E', 'S', 'T'};
  std::memcpy(p.time_zone.standard_name, std_name, 4);
  std::memcpy(p.time_zone.daylight_name, dlt_name, 4);
  p.time_zone.standard_date = {10, 5, 0, 3};
  p.time_zone.daylight_date = {3, 5, 0, 2};
  p.time_zone.standard_bias = 0;
  p.time_zone.daylight_bias = 0xFFFFFFC4u;
  p.retail_flags.configured = true;
  p.retail_flags.value = 0x00000040u;
  return p;
}

void expect_setting(uint16_t setting, const uint8_t expected[4]) {
  fill(kOutput, 0xCC, 8);
  fill(kRequired, 0xA5, 2);
  CHECK_EQ(call(xconfig::kUserCategory, setting, kOutput, 4, kRequired),
           nt::kSuccess);
  expect_bytes(kOutput, expected);
  CHECK_EQ(required_word(), 4u);
  const uint8_t *tail = memory.translate(kOutput + 4, 4);
  CHECK(tail && tail[0] == 0xCC && tail[1] == 0xCC && tail[2] == 0xCC &&
        tail[3] == 0xCC);
}

void exact_values() {
  const uint8_t av_pal60[4] = {0x00, 0x40, 0x04, 0x00};
  fill(kRequired, 0xA5, 2);
  CHECK_EQ(call(xconfig::kSecuredCategory, xconfig::kSecuredAvRegion, kOutput,
                4, kRequired),
           nt::kSuccess);
  expect_bytes(kOutput, av_pal60);
  CHECK_EQ(required_word(), 4u);

  const uint8_t language_fr[4] = {0x00, 0x00, 0x00, 0x04};
  const uint8_t widescreen[4] = {0x00, 0x01, 0x00, 0x00};
  const uint8_t retail[4] = {0x00, 0x00, 0x00, 0x40};
  expect_setting(xconfig::kUserLanguage, language_fr);
  expect_setting(xconfig::kUserVideoFlags, widescreen);
  expect_setting(xconfig::kUserRetailFlags, retail);

  const uint8_t bias[4] = {0xFF, 0xFF, 0xFF, 0xC4};
  const uint8_t std_name[4] = {'C', 'E', 'T', 0};
  const uint8_t dlt_name[4] = {'C', 'E', 'S', 'T'};
  const uint8_t std_date[4] = {10, 5, 0, 3};
  const uint8_t dlt_date[4] = {3, 5, 0, 2};
  const uint8_t zero[4] = {0, 0, 0, 0};
  expect_setting(xconfig::kUserTimeZoneBias, bias);
  expect_setting(xconfig::kUserTimeZoneStdName, std_name);
  expect_setting(xconfig::kUserTimeZoneDltName, dlt_name);
  expect_setting(xconfig::kUserTimeZoneStdDate, std_date);
  expect_setting(xconfig::kUserTimeZoneDltDate, dlt_date);
  expect_setting(xconfig::kUserTimeZoneStdBias, zero);
  expect_setting(xconfig::kUserTimeZoneDltBias, bias);
}

void null_query_and_guards() {
  CHECK_EQ(call(xconfig::kUserCategory, xconfig::kUserLanguage, 0, 0, 0),
           nt::kSuccess);

  fill(kRequired, 0xA5, 2);
  CHECK_EQ(
      call(xconfig::kUserCategory, xconfig::kUserLanguage, 0, 0, kRequired),
      nt::kSuccess);
  CHECK_EQ(required_word(), 4u);

  fill(kRequired, 0xA5, 2);
  CHECK_EQ(
      call(xconfig::kUserCategory, xconfig::kUserLanguage, 0, 4, kRequired),
      xconfig::kStatusInvalidParameter3);
  CHECK_EQ(required_word(), 0xA5A5u);

  fill(kOutput, 0x7B, 4);
  fill(kRequired, 0xA5, 2);
  CHECK_EQ(call(xconfig::kUserCategory, xconfig::kUserLanguage, kOutput, 3,
                kRequired),
           xconfig::kStatusBufferTooSmall);
  const uint8_t preserved[4] = {0x7B, 0x7B, 0x7B, 0x7B};
  expect_bytes(kOutput, preserved);
  CHECK_EQ(required_word(), 0xA5A5u);

  fill(kOutput, 0x7B, 4);
  fill(kRequired, 0xA5, 2);
  CHECK_EQ(call(0x55, 1, kOutput, 4, kRequired),
           xconfig::kStatusInvalidParameter1);
  expect_bytes(kOutput, preserved);
  CHECK_EQ(required_word(), 0xA5A5u);
  CHECK_EQ(call(xconfig::kUserCategory, 0x55, kOutput, 4, kRequired),
           xconfig::kStatusInvalidParameter2);
  expect_bytes(kOutput, preserved);
  CHECK_EQ(required_word(), 0xA5A5u);
}

void access_faults() {
  fill(kOutput, 0x66, 4);
  CHECK_EQ(call(xconfig::kUserCategory, xconfig::kUserLanguage, 0x35000000u, 4,
                kRequired),
           nt::kAccessViolation);
  const uint8_t preserved[4] = {0x66, 0x66, 0x66, 0x66};
  expect_bytes(kOutput, preserved);
  CHECK_EQ(call(xconfig::kUserCategory, xconfig::kUserLanguage, kOutput, 4,
                0x35000000u),
           nt::kAccessViolation);
  expect_bytes(kOutput, preserved);
}

void unconfigured_and_reset() {
  runtime_reset_xconfig();
  fill(kOutput, 0x11, 4);
  fill(kRequired, 0xA5, 2);
  CHECK_EQ(call(xconfig::kUserCategory, xconfig::kUserLanguage, kOutput, 4,
                kRequired),
           xconfig::kStatusNotSupported);
  const uint8_t preserved[4] = {0x11, 0x11, 0x11, 0x11};
  expect_bytes(kOutput, preserved);
  CHECK_EQ(required_word(), 0xA5A5u);

  XConfigProfile partial = profile(1, XConfigAvRegion::NtscM);
  partial.time_zone.configured = false;
  partial.retail_flags.configured = false;
  CHECK_ST(runtime_configure_xconfig(partial), Status::Ok);
  CHECK_EQ(call(xconfig::kUserCategory, xconfig::kUserTimeZoneBias, kOutput, 4,
                kRequired),
           xconfig::kStatusNotSupported);
  CHECK_EQ(call(xconfig::kUserCategory, xconfig::kUserRetailFlags, kOutput, 4,
                kRequired),
           xconfig::kStatusNotSupported);
  const uint8_t english[4] = {0, 0, 0, 1};
  expect_setting(xconfig::kUserLanguage, english);

  runtime_reset_xconfig();
  CHECK_ST(runtime_configure_xconfig(profile(4, XConfigAvRegion::Pal60)),
           Status::Ok);
}

} // namespace

int main() {
  CHECK(memory.reserve() == MemStatus::Ok);
  CHECK(memory.commit(kScratch, kGuestPageSize, Protect::ReadWrite) ==
        MemStatus::Ok);
  CHECK_ST(runtime_init(&memory), Status::Ok);
  CHECK_ST(register_xboxkrnl_xconfig_hle(), Status::Ok);
  uint32_t ordinal = 0;
  CHECK(export_ordinal(kModuleXboxkrnl, "ExGetXConfigSetting", &ordinal));
  CHECK_EQ(ordinal, 0x10u);
  CHECK(find_import(kModuleXboxkrnl, ordinal) != nullptr);

  XConfigProfile invalid{};
  CHECK_ST(runtime_configure_xconfig(invalid), Status::InvalidArgument);
  CHECK_ST(runtime_configure_xconfig(profile(4, XConfigAvRegion::Pal60)),
           Status::Ok);
  CHECK_ST(runtime_configure_xconfig(profile(1, XConfigAvRegion::NtscM)),
           Status::Conflict);

  exact_values();
  null_query_and_guards();
  access_faults();
  unconfigured_and_reset();

  // A missed explicit reset must not leak the old profile into the next
  // Runtime lifetime. Generation binding rejects the stale state.
  runtime_shutdown();
  CHECK_ST(runtime_configure_xconfig(profile(1, XConfigAvRegion::NtscM)),
           Status::NotInitialized);
  CHECK_ST(runtime_init(&memory), Status::Ok);
  fill(kOutput, 0x33, 4);
  fill(kRequired, 0xA5, 2);
  CHECK_EQ(call(xconfig::kUserCategory, xconfig::kUserLanguage, kOutput, 4,
                kRequired),
           xconfig::kStatusNotSupported);
  const uint8_t stale_preserved[4] = {0x33, 0x33, 0x33, 0x33};
  expect_bytes(kOutput, stale_preserved);
  CHECK_EQ(required_word(), 0xA5A5u);
  CHECK_ST(runtime_configure_xconfig(profile(1, XConfigAvRegion::NtscM)),
           Status::Ok);
  const uint8_t english[4] = {0, 0, 0, 1};
  expect_setting(xconfig::kUserLanguage, english);

  runtime_reset_xconfig();
  runtime_shutdown();
  clear_imports();
  return test_result("rt_xconfig");
}
