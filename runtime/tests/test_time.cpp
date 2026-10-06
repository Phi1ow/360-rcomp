// Primary ABI plus independent Windows ntdll oracle values provided by PRIME.
// These check the implemented Gregorian subset, not a hardware Xbox oracle.
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/clock_sync.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"
#include <array>
#include <cstring>

PPC_EXTERN_FUNC(__imp__RtlTimeFieldsToTime);
PPC_EXTERN_FUNC(__imp__RtlTimeToTimeFields);
PPC_EXTERN_FUNC(__imp__KeQuerySystemTime);
using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory mem;
constexpr uint32_t scratch = 0x30000000, fields = scratch + 0x100,
                   time_value = scratch + 0x200;
using Fields = std::array<int16_t, 8>;
void put_fields(uint32_t address, const Fields &f) {
  for (size_t i = 0; i < f.size(); ++i) {
    mem.base()[address + i * 2] = (uint8_t)(uint16_t(f[i]) >> 8);
    mem.base()[address + i * 2 + 1] = (uint8_t)f[i];
  }
}
void expect_fields(uint32_t address, const Fields &f) {
  for (size_t i = 0; i < f.size(); ++i) {
    uint16_t value = 0;
    CHECK(guest_read_be16(address + (uint32_t)i * 2, &value));
    CHECK_EQ(value, uint16_t(f[i]));
  }
}
uint32_t call(PPCFunc *fn, uint32_t first, uint32_t second = 0) {
  PPCContext c{};
  c.r3.u64 = first;
  c.r4.u64 = second;
  fn(c, mem.base());
  return c.r3.u32;
}
uint64_t read_time() {
  uint64_t t = 0;
  CHECK(guest_read_be64(time_value, &t));
  return t;
}
} // namespace
int main() {
  CHECK(mem.reserve() == MemStatus::Ok);
  CHECK(mem.commit(scratch, 0x20000, Protect::ReadWrite) == MemStatus::Ok);
  CHECK_ST(runtime_init(&mem), Status::Ok);
  CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
  struct Case {
    Fields fields;
    uint64_t ticks;
    int16_t weekday;
  };
  const Case cases[] = {
      {{1601, 1, 1, 0, 0, 0, 0, 0}, 0, 1},
      {{1970, 1, 1, 0, 0, 0, 0, 0}, 116444736000000000ull, 4},
      {{2000, 2, 29, 12, 34, 56, 789, -123}, 125963012967890000ull, 2},
      {{2400, 2, 29, 0, 0, 0, 0, 0}, 252190368000000000ull, 2},
      {{9999, 12, 31, 23, 59, 59, 999, 0}, 2650467743999990000ull, 5},
      {{10000, 1, 1, 0, 0, 0, 0, 0}, 2650467744000000000ull, 6},
      {{30827, 12, 31, 23, 59, 59, 999, 0}, 9223149887999990000ull, 5},
  };
  for (const auto &test : cases) {
    put_fields(fields, test.fields);
    CHECK_EQ(call(__imp__RtlTimeFieldsToTime, fields, time_value), 1u);
    CHECK_EQ(read_time(), test.ticks);
    call(__imp__RtlTimeToTimeFields, time_value, fields);
    auto result = test.fields;
    result[7] = test.weekday;
    expect_fields(fields, result);
  }
  constexpr uint64_t sentinel = 0x123456789ABCDEF;
  const Fields invalids[] = {
      {1600, 1, 1, 0, 0, 0, 0, 0},    {-1, 1, 1, 0, 0, 0, 0, 0},
      {2000, 0, 1, 0, 0, 0, 0, 0},    {2000, 13, 1, 0, 0, 0, 0, 0},
      {2000, 2, 0, 0, 0, 0, 0, 0},    {2000, 2, 30, 0, 0, 0, 0, 0},
      {1900, 2, 29, 0, 0, 0, 0, 0},   {2100, 2, 29, 0, 0, 0, 0, 0},
      {2001, 4, 31, 0, 0, 0, 0, 0},   {2000, 1, 1, -1, 0, 0, 0, 0},
      {2000, 1, 1, 24, 0, 0, 0, 0},   {2000, 1, 1, 0, -1, 0, 0, 0},
      {2000, 1, 1, 0, 60, 0, 0, 0},   {2000, 1, 1, 0, 0, -1, 0, 0},
      {2000, 1, 1, 0, 0, 60, 0, 0},   {2000, 1, 1, 0, 0, 0, -1, 0},
      {2000, 1, 1, 0, 0, 0, 1000, 0},
  };
  for (const auto &f : invalids) {
    put_fields(fields, f);
    CHECK(guest_write_be64(time_value, sentinel));
    CHECK_EQ(call(__imp__RtlTimeFieldsToTime, fields, time_value), 0u);
    CHECK_EQ(read_time(), sentinel);
  }
  const Case inverses[] = {
      {{1601, 1, 1, 0, 0, 0, 0, 1}, 1, 1},
      {{1601, 1, 1, 0, 0, 0, 0, 1}, 9999, 1},
      {{1601, 1, 1, 0, 0, 0, 1, 1}, 10000, 1},
      {{1601, 1, 1, 0, 0, 0, 999, 1}, 9999999, 1},
      {{1601, 1, 1, 0, 0, 1, 0, 1}, 10000000, 1},
      {{1601, 1, 1, 23, 59, 59, 999, 1}, 863999999999ull, 1},
      {{1601, 1, 2, 0, 0, 0, 0, 2}, 864000000000ull, 2},
      {{30828, 9, 14, 2, 48, 5, 477, 4}, INT64_MAX, 4},
  };
  for (const auto &test : inverses) {
    CHECK(guest_write_be64(time_value, test.ticks));
    call(__imp__RtlTimeToTimeFields, time_value, fields);
    expect_fields(fields, test.fields);
  }
  // Aliased input/output is fully read before the first write.
  put_fields(fields, cases[2].fields);
  CHECK_EQ(call(__imp__RtlTimeFieldsToTime, fields, fields), 1u);
  uint64_t alias = 0;
  CHECK(guest_read_be64(fields, &alias));
  CHECK_EQ(alias, cases[2].ticks);
  call(__imp__RtlTimeToTimeFields, fields, fields);
  auto aliased = cases[2].fields;
  aliased[7] = 2;
  expect_fields(fields, aliased);
  bool fatal = false;
  put_fields(fields, {30828, 1, 1, 0, 0, 0, 0, 0});
  CHECK(guest_write_be64(time_value, sentinel));
  CAPTURE_FATAL(call(__imp__RtlTimeFieldsToTime, fields, time_value), fatal);
  CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
  CHECK_EQ(read_time(), sentinel);
  CHECK(guest_write_be64(time_value, UINT64_MAX));
  memset(mem.base() + fields, 0xBC, 16);
  CAPTURE_FATAL(call(__imp__RtlTimeToTimeFields, time_value, fields), fatal);
  CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
  for (unsigned i = 0; i < 16; ++i)
    CHECK_EQ(mem.base()[fields + i], 0xBCu);
  // No input/output pointer may cross a protection boundary. Output guards
  // are independent of the readable input page.
  put_fields(fields, cases[0].fields);
  CHECK(mem.protect(scratch + 0x10000, 0x10000, Protect::None) ==
        MemStatus::Ok);
  CAPTURE_FATAL(call(__imp__RtlTimeFieldsToTime, fields, scratch + 0xFFFC),
                fatal);
  CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
  CAPTURE_FATAL(call(__imp__RtlTimeFieldsToTime, scratch + 0xFFF8, time_value),
                fatal);
  CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
  CAPTURE_FATAL(call(__imp__RtlTimeToTimeFields, time_value, scratch + 0xFFF8),
                fatal);
  CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
  CHECK(mem.protect(scratch + 0x10000, 0x10000, Protect::ReadWrite) ==
        MemStatus::Ok);
  const uint64_t before = system_filetime();
  call(__imp__KeQuerySystemTime, time_value);
  const uint64_t after = system_filetime();
  CHECK(read_time() >= before && read_time() <= after);
  CHECK(mem.protect(scratch, 0x10000, Protect::Read) == MemStatus::Ok);
  CAPTURE_FATAL(call(__imp__KeQuerySystemTime, time_value), fatal);
  CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
  put_fields(scratch + 0x10100, cases[0].fields);
  CAPTURE_FATAL(call(__imp__RtlTimeFieldsToTime, scratch + 0x10100, time_value),
                fatal);
  CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
  CAPTURE_FATAL(call(__imp__RtlTimeToTimeFields, time_value, fields), fatal);
  CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
  CHECK(mem.protect(scratch, 0x10000, Protect::ReadWrite) == MemStatus::Ok);
  runtime_shutdown();
  clear_imports();
  mem.release();
  return test_result("rt_time");
}
