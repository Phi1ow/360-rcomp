// Gregorian FILETIME conversion with integer arithmetic and explicit domains.
// Contract and primary references: runtime/docs/OBJECTS_TIME.md.
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include <cstring>

namespace rcomp::rt {
namespace {
constexpr uint64_t kTicksPerDay = 864000000000ull;
constexpr int kLastFieldsYear = 30827;
bool leap(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
int month_days(int year, int month) {
  constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return days[month - 1] + (month == 2 && leap(year));
}
uint64_t year_days(int year) {
  const uint64_t before = (uint64_t)year - 1;
  return ((uint64_t)year - 1601) * 365 + before / 4 - 400 -
         (before / 100 - 16) + before / 400 - 4;
}
Runtime &runtime_for(const char *name) {
  Runtime *r = runtime();
  if (!r)
    rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s before runtime_init", name);
  return *r;
}
void ranges(Runtime &r, const char *fn, uint32_t in, uint32_t in_bytes,
            uint32_t out, uint32_t out_bytes) {
  if (!in || !out || !r.mem->is_accessible(in, in_bytes, Protect::Read) ||
      !r.mem->is_accessible(out, out_bytes, Protect::ReadWrite))
    rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,
                "%s input/output not accessible: 0x%08X/0x%08X", fn, in, out);
}
int32_t signed_be16(const uint8_t *p) {
  uint32_t n = (uint32_t(p[0]) << 8) | p[1];
  return n & 0x8000 ? int32_t(n) - 0x10000 : int32_t(n);
}
void RtlTimeFieldsToTime(PPCContext &c, uint8_t *) {
  const char *fn = "RtlTimeFieldsToTime";
  auto &r = runtime_for(fn);
  const uint32_t input = c.r3.u32, output = c.r4.u32;
  ranges(r, fn, input, 16, output, 8);
  int32_t f[8];
  for (unsigned i = 0; i < 8; ++i)
    f[i] = signed_be16(r.mem->host(input) + 2 * i);
  const int y = f[0], m = f[1], d = f[2];
  if (y < 1601 || m < 1 || m > 12 || d < 1 || d > month_days(y, m) ||
      f[3] < 0 || f[3] > 23 || f[4] < 0 || f[4] > 59 || f[5] < 0 || f[5] > 59 ||
      f[6] < 0 || f[6] > 999) {
    c.r3.u64 = 0;
    return;
  }
  // Sources disagree on future-year boundaries. Do not falsely classify a
  // valid date outside the corroborated domain as an invalid calendar date.
  if (y > kLastFieldsYear)
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                "%s year %d outside verified domain 1601..30827", fn, y);
  uint64_t days = year_days(y) + (uint64_t)d - 1;
  for (int month = 1; month < m; ++month)
    days += (uint64_t)month_days(y, month);
  const uint64_t ticks = days * kTicksPerDay + uint64_t(f[3]) * 36000000000ull +
                         uint64_t(f[4]) * 600000000ull +
                         uint64_t(f[5]) * 10000000ull +
                         uint64_t(f[6]) * 10000ull;
  uint8_t bytes[8];
  for (unsigned i = 0; i < 8; ++i)
    bytes[i] = (uint8_t)(ticks >> (56 - 8 * i));
  memcpy(r.mem->host(output), bytes, sizeof bytes);
  c.r3.u64 = 1;
}
void RtlTimeToTimeFields(PPCContext &c, uint8_t *) {
  const char *fn = "RtlTimeToTimeFields";
  auto &r = runtime_for(fn);
  const uint32_t input = c.r3.u32, output = c.r4.u32;
  ranges(r, fn, input, 8, output, 16);
  uint64_t ticks = 0;
  for (unsigned i = 0; i < 8; ++i)
    ticks = (ticks << 8) | r.mem->base()[input + i];
  if (ticks >> 63)
    rcomp_fatal(
        RCOMP_FATAL_UNIMPLEMENTED,
        "%s negative signed FILETIME is outside the supported absolute domain",
        fn);
  const uint64_t days = ticks / kTicksPerDay;
  int lo = 1601, hi = 32768;
  while (lo + 1 < hi) {
    const int middle = lo + (hi - lo) / 2;
    if (year_days(middle) <= days)
      lo = middle;
    else
      hi = middle;
  }
  uint64_t day = days - year_days(lo);
  int month = 1;
  while (day >= (uint64_t)month_days(lo, month)) {
    day -= (uint64_t)month_days(lo, month);
    ++month;
  }
  uint64_t milliseconds = (ticks % kTicksPerDay) / 10000;
  const uint16_t hour = (uint16_t)(milliseconds / 3600000);
  milliseconds %= 3600000;
  const uint16_t minute = (uint16_t)(milliseconds / 60000);
  milliseconds %= 60000;
  const uint16_t second = (uint16_t)(milliseconds / 1000);
  milliseconds %= 1000;
  const uint16_t f[] = {(uint16_t)lo,
                        (uint16_t)month,
                        (uint16_t)(day + 1),
                        hour,
                        minute,
                        second,
                        (uint16_t)milliseconds,
                        (uint16_t)((days + 1) % 7)};
  uint8_t bytes[16];
  for (unsigned i = 0; i < 8; ++i) {
    bytes[2 * i] = (uint8_t)(f[i] >> 8);
    bytes[2 * i + 1] = (uint8_t)f[i];
  }
  memcpy(r.mem->host(output), bytes,
         sizeof bytes); // VOID: no invented return value
}
} // namespace
Status register_xboxkrnl_time_hle() {
  struct Impl {
    uint32_t ordinal;
    const char *name;
    PPCFunc *function;
  };
  const Impl entries[] = {
      {0x013F, "RtlTimeFieldsToTime", &RtlTimeFieldsToTime},
      {0x0140, "RtlTimeToTimeFields", &RtlTimeToTimeFields}};
  for (const auto &i : entries) {
    uint32_t ordinal = 0;
    if (!export_ordinal(kModuleXboxkrnl, i.name, &ordinal) ||
        ordinal != i.ordinal)
      return Status::InvalidArgument;
    const Status s =
        register_import(kModuleXboxkrnl, i.ordinal, i.function, i.name);
    if (s != Status::Ok)
      return s;
  }
  return Status::Ok;
}
} // namespace rcomp::rt
