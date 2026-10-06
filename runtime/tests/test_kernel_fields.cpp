#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/kernel_variables.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/virtual_fields.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/xam.h"
#include "test_util.h"

using namespace rcomp;
using namespace rcomp::rt;

namespace {

constexpr uint32_t kHeaderSize = 0x600;
constexpr uint32_t kImportOffset = 0x100;
constexpr uint32_t kSecurityOffset = 0x400;
constexpr uint32_t kPacked6683 = 0x201A1B00u;

void put32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
  bytes[offset + 0] = uint8_t(value >> 24);
  bytes[offset + 1] = uint8_t(value >> 16);
  bytes[offset + 2] = uint8_t(value >> 8);
  bytes[offset + 3] = uint8_t(value);
}

void put16(std::vector<uint8_t> &bytes, size_t offset, uint16_t value) {
  bytes[offset + 0] = uint8_t(value >> 8);
  bytes[offset + 1] = uint8_t(value);
}

struct Claims {
  uint32_t kernel_version = kPacked6683;
  uint32_t kernel_minimum = kPacked6683;
  uint32_t xam_version = kPacked6683;
  uint32_t xam_minimum = kPacked6683;
};

std::vector<uint8_t> make_profile_xex(const Claims &claims = {}) {
  std::vector<uint8_t> xex(kHeaderSize, 0);
  put32(xex, 0, 0x58455832u); // XEX2
  put32(xex, 8, kHeaderSize);
  put32(xex, 16, kSecurityOffset);
  put32(xex, 20, 1);
  put32(xex, 24, 0x000103FFu);
  put32(xex, 28, kImportOffset);

  // validate_xex_header only needs the security header range to be present;
  // the compatibility parser does not consume any commercial image bytes.
  constexpr char kKernelName[] = "xboxkrnl.exe";
  constexpr char kXamName[] = "xam.xex";
  constexpr uint32_t kStringBytes = 24; // 16-byte + 8-byte padded names
  size_t offset = kImportOffset + 12;
  std::memcpy(xex.data() + offset, kKernelName, sizeof(kKernelName));
  offset += 16;
  std::memcpy(xex.data() + offset, kXamName, sizeof(kXamName));
  offset += 8;

  auto library = [&](uint16_t name_index, uint32_t version, uint32_t minimum) {
    constexpr uint32_t kLibraryBytes = 0x28;
    put32(xex, offset + 0x00, kLibraryBytes);
    put32(xex, offset + 0x1C, version);
    put32(xex, offset + 0x20, minimum);
    put16(xex, offset + 0x24, name_index);
    put16(xex, offset + 0x26, 0); // import count
    offset += kLibraryBytes;
  };
  library(0, claims.kernel_version, claims.kernel_minimum);
  library(1, claims.xam_version, claims.xam_minimum);

  put32(xex, kImportOffset + 0, uint32_t(offset - kImportOffset));
  put32(xex, kImportOffset + 4, kStringBytes);
  put32(xex, kImportOffset + 8, 2);
  return xex;
}

uint32_t g_test_word = 0x10203040u;
VirtualAccessStatus test_read(uint32_t ea, uint8_t width, uint64_t *value,
                              VirtualAccessDiagnostic *) {
  if (ea != 0x60000004u)
    return VirtualAccessStatus::UnknownField;
  if (width != 4)
    return VirtualAccessStatus::InvalidWidth;
  *value = g_test_word;
  return VirtualAccessStatus::Handled;
}
VirtualAccessStatus test_write(uint32_t ea, uint8_t width, uint64_t value,
                               VirtualAccessDiagnostic *) {
  if (ea != 0x60000004u)
    return VirtualAccessStatus::UnknownField;
  if (width != 4)
    return VirtualAccessStatus::InvalidWidth;
  g_test_word = uint32_t(value);
  return VirtualAccessStatus::Handled;
}
VirtualAccessStatus reentrant_read(uint32_t ea, uint8_t width, uint64_t *value,
                                   VirtualAccessDiagnostic *) {
  if (ea != 0x60000200u || width != 4)
    return VirtualAccessStatus::UnknownField;
  // This nested dispatch would deadlock if the outer dispatcher held the
  // virtual provider registry mutex while invoking callbacks.
  return runtime_virtual_read(0x60000004u, 4, 0, value);
}

} // namespace

int main(int argc, char **argv) {
  // Dispatcher contract: provider callback runs through POD values/statuses,
  // preserves LR for a later CPU diagnostic, and refuses overlaps.
  virtual_fields_reset();
  const VirtualFieldProvider provider{0x60000000u, 0x100u, "TESTDOUBLE_virtual",
                                      &test_read, &test_write};
  CHECK_ST(register_virtual_field_provider(provider), Status::Ok);
  CHECK_ST(register_virtual_field_provider(provider), Status::Ok);
  const VirtualFieldProvider overlap{0x60000080u, 0x100u, "TESTDOUBLE_overlap",
                                     &test_read, &test_write};
  CHECK_ST(register_virtual_field_provider(overlap), Status::Conflict);
  uint64_t value = 0;
  VirtualAccessDiagnostic diagnostic;
  CHECK_EQ(runtime_virtual_read(0x50000000u, 16, 0, &value),
           VirtualAccessStatus::NotVirtual);
  CHECK_EQ(runtime_virtual_read(0x60000004u, 16, 0, &value, &diagnostic),
           VirtualAccessStatus::InvalidWidth);
  CHECK_EQ(
      runtime_virtual_read(0x60000004u, 4, 0x82001234u, &value, &diagnostic),
      VirtualAccessStatus::Handled);
  CHECK_EQ(value, 0x10203040u);
  CHECK_EQ(diagnostic.lr, 0x82001234u);
  CHECK_EQ(diagnostic.region_base, 0x60000000u);
  CHECK_EQ(diagnostic.offset, 4u);
  CHECK_EQ(
      runtime_virtual_read(0x60000008u, 4, 0x82000000u, &value, &diagnostic),
      VirtualAccessStatus::UnknownField);
  CHECK_EQ(runtime_virtual_write(0x60000004u, 4, 0x55667788u, 0, &diagnostic),
           VirtualAccessStatus::Handled);
  CHECK_EQ(g_test_word, 0x55667788u);
  const VirtualFieldProvider reentrant{
      0x60000200u, 0x100u, "TESTDOUBLE_reentrant", &reentrant_read, nullptr};
  CHECK_ST(register_virtual_field_provider(reentrant), Status::Ok);
  CHECK_EQ(runtime_virtual_read(0x60000200u, 4, 0, &value, &diagnostic),
           VirtualAccessStatus::Handled);
  CHECK_EQ(value, 0x55667788u);
  CHECK_ST(unregister_virtual_field_provider(reentrant), Status::Ok);
  CHECK_ST(unregister_virtual_field_provider(provider), Status::Ok);
  CHECK_EQ(runtime_virtual_read(0x60000004u, 4, 0, &value),
           VirtualAccessStatus::NotVirtual);

  // Title-derived compatibility parser: exact GTA IV import-library claim.
  auto xex = make_profile_xex();
  KernelCompatibilityProfile profile;
  CHECK_ST(parse_kernel_compatibility_profile_from_xex(xex.data(), xex.size(),
                                                       &profile, nullptr),
           Status::Ok);
  CHECK_EQ(profile.packed, kPacked6683);
  CHECK_EQ(profile.major, 2u);
  CHECK_EQ(profile.minor, 0u);
  CHECK_EQ(profile.build, 6683u);
  CHECK_EQ(profile.qfe, 0u);
  if (argc > 1) {
    // Optional local-title proof. The file bytes remain outside the repo;
    // only validated version metadata is printed by the parser.
    std::ifstream file(argv[1], std::ios::binary);
    CHECK(file.good());
    std::vector<uint8_t> title((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    CHECK(!title.empty());
    KernelCompatibilityProfile actual;
    CHECK_ST(parse_kernel_compatibility_profile_from_xex(
                 title.data(), title.size(), &actual, stderr),
             Status::Ok);
    CHECK_EQ(actual.packed, kPacked6683);
    CHECK_EQ(actual.major, 2u);
    CHECK_EQ(actual.minor, 0u);
    CHECK_EQ(actual.build, 6683u);
    CHECK_EQ(actual.qfe, 0u);
  }
  {
    Claims conflict;
    conflict.xam_minimum = 0x201A1C00u;
    auto bad = make_profile_xex(conflict);
    CHECK_ST(parse_kernel_compatibility_profile_from_xex(bad.data(), bad.size(),
                                                         &profile, nullptr),
             Status::Conflict);
  }
  {
    // Owner decision (3 Oct 2026): the title sees the kernel version it was built against, which may be
    // newer than the minimum it declares.
    Claims newer;
    newer.kernel_version = 0x201A1C00u;
    auto ok = make_profile_xex(newer);
    KernelCompatibilityProfile built_against;
    CHECK_ST(parse_kernel_compatibility_profile_from_xex(ok.data(), ok.size(),
                                                         &built_against, nullptr),
             Status::Ok);
    CHECK_EQ(built_against.packed, 0x201A1C00u);
    CHECK_EQ(built_against.build, 6684u);
  }
  {
    // A version below the minimum the same library declares is an inconsistent XEX.
    Claims conflict;
    conflict.kernel_version = 0x201A1A00u;
    auto bad = make_profile_xex(conflict);
    CHECK_ST(parse_kernel_compatibility_profile_from_xex(bad.data(), bad.size(),
                                                         &profile, nullptr),
             Status::Conflict);
  }

  // Real registration uses the Runtime generation and exports only virtual
  // addresses inside the already-reserved opaque arena.
  GuestMemory memory;
  CHECK(memory.reserve() == MemStatus::Ok);
  CHECK_ST(runtime_init(&memory), Status::Ok);
  clear_imports();
  CHECK_ST(register_xam_system_hle(),Status::Ok);
  PPCFunc* system_version=find_import(kModuleXam,0x0282);
  CHECK(system_version!=nullptr);
  PPCContext system_context{};
  bool system_fatal=false;
  CAPTURE_FATAL(system_version(system_context,memory.base()),system_fatal);
  CHECK(system_fatal&&g_fatal_kind==RCOMP_FATAL_UNIMPLEMENTED);
  CHECK_ST(register_xboxkrnl_kernel_variables_from_xex(xex.data(), xex.size(),
                                                       nullptr),
           Status::Ok);
  CHECK_ST(xboxkrnl_kernel_compatibility_profile(&profile), Status::Ok);
  CHECK_EQ(profile.packed, kPacked6683);
  system_context.r3.u64=UINT64_MAX;
  system_version(system_context,memory.base());
  CHECK_EQ(system_context.r3.u64,uint64_t(kPacked6683));
  uint32_t address = 0;
  CHECK(find_variable_import(kModuleXboxkrnl, 0x0158, &address));
  CHECK_EQ(address, kXboxKrnlVersionVirtualAddress);
  CHECK(find_variable_import(kModuleXboxkrnl, 0x00AD, &address));
  CHECK_EQ(address, kKeTimeStampBundleVirtualAddress);
  CHECK(find_variable_import(kModuleXboxkrnl, 0x0156, &address));
  CHECK_EQ(address, kXboxHardwareInfoVirtualAddress);
  CHECK(!memory.is_committed(kKernelVirtualPageBase, kKernelVirtualPageSize));

  // XboxHardwareInfo: GoW2 reads the BE32 Flags word and tests bit 0x10
  // (DbgBreakPoint when set); +4 is the processor count. Nothing else exists.
  CHECK_EQ(runtime_virtual_read(kXboxHardwareInfoVirtualAddress, 4, 0x82AB3A90u,
                                &value, &diagnostic),
           VirtualAccessStatus::Handled);
  CHECK_EQ(value, 0x00000020u);
  CHECK_EQ(value & 0x10u, 0u);
  CHECK_EQ(runtime_virtual_read(kXboxHardwareInfoVirtualAddress + 4, 1, 0,
                                &value, &diagnostic),
           VirtualAccessStatus::Handled);
  CHECK_EQ(value, 6u);
  CHECK_EQ(runtime_virtual_read(kXboxHardwareInfoVirtualAddress + 3, 2, 0,
                                &value, &diagnostic),
           VirtualAccessStatus::Handled);
  CHECK_EQ(value, 0x2006u);
  CHECK_EQ(runtime_virtual_read(kXboxHardwareInfoVirtualAddress + 4, 2, 0,
                                &value, &diagnostic),
           VirtualAccessStatus::UnknownField);
  CHECK_EQ(runtime_virtual_read(kXboxHardwareInfoVirtualAddress + 8, 4, 0,
                                &value, &diagnostic),
           VirtualAccessStatus::UnknownField);
  CHECK_EQ(runtime_virtual_write(kXboxHardwareInfoVirtualAddress, 4, 0, 0,
                                 &diagnostic),
           VirtualAccessStatus::ReadOnly);
  CHECK_EQ(runtime_virtual_write(kXboxHardwareInfoVirtualAddress + 0xC, 4, 0,
                                 0, &diagnostic),
           VirtualAccessStatus::UnknownField);

  CHECK_EQ(runtime_virtual_read(kXboxKrnlVersionVirtualAddress, 8, 0x829A0000u,
                                &value, &diagnostic),
           VirtualAccessStatus::Handled);
  CHECK_EQ(value, 0x000200001A1B0000ull);
  CHECK_EQ(
      runtime_virtual_read(kXboxKrnlVersionVirtualAddress + 4, 2, 0, &value),
      VirtualAccessStatus::Handled);
  CHECK_EQ(value, 6683u);
  CHECK_EQ(runtime_virtual_write(kXboxKrnlVersionVirtualAddress, 2, 7, 0,
                                 &diagnostic),
           VirtualAccessStatus::ReadOnly);

  uint64_t first = 0, second = 0;
  CHECK_EQ(runtime_virtual_read(kKeTimeStampBundleVirtualAddress + 0x10, 4, 0,
                                &first, &diagnostic),
           VirtualAccessStatus::Handled);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  CHECK_EQ(runtime_virtual_read(kKeTimeStampBundleVirtualAddress + 0x10, 4, 0,
                                &second, &diagnostic),
           VirtualAccessStatus::Handled);
  CHECK(second >= first + 1);
  CHECK_EQ(runtime_virtual_read(kKeTimeStampBundleVirtualAddress + 0x0C, 4, 0,
                                &value, &diagnostic),
           VirtualAccessStatus::UnknownField);
  CHECK_EQ(runtime_virtual_read(kExThreadObjectTypeVirtualAddress, 4, 0, &value,
                                &diagnostic),
           VirtualAccessStatus::UnknownField);
  // ExEventObjectType: identity-only token, no field is served.
  CHECK_EQ(runtime_virtual_read(kExEventObjectTypeVirtualAddress + 8, 4, 0, &value,
                                &diagnostic),
           VirtualAccessStatus::UnknownField);
  CHECK(diagnostic.provider && strcmp(diagnostic.provider, "ExEventObjectType") == 0);

  {
    Claims another;
    another.kernel_version = another.kernel_minimum = 0x201A1C00u;
    another.xam_version = another.xam_minimum = 0x201A1C00u;
    auto other = make_profile_xex(another);
    // Active state is deliberately not replaced by another title profile.
    CHECK_ST(register_xboxkrnl_kernel_variables_from_xex(other.data(),
                                                         other.size(), nullptr),
             Status::Conflict);
  }
  {
    // A version outside the Xbox 360 kernel family 2.0.1888-2.0.17559 is refused, not guessed.
    Claims foreign;
    foreign.kernel_version = foreign.kernel_minimum = 0x20450000u;  // 2.0.17664.0
    foreign.xam_version = foreign.xam_minimum = 0x20450000u;
    auto other = make_profile_xex(foreign);
    CHECK_ST(register_xboxkrnl_kernel_variables_from_xex(other.data(),
                                                         other.size(), nullptr),
             Status::Unsupported);
  }

  {
    // The Episodes from Liberty City executable claims 2.0.8498.0: a supported profile (not Unsupported), but the active 6683 state is still not replaced.
    Claims episodes;
    episodes.kernel_version = episodes.kernel_minimum = 0x20213200u;
    episodes.xam_version = episodes.xam_minimum = 0x20213200u;
    auto other = make_profile_xex(episodes);
    KernelCompatibilityProfile parsed;
    CHECK_ST(parse_kernel_compatibility_profile_from_xex(other.data(), other.size(), &parsed, nullptr), Status::Ok);
    CHECK_EQ(parsed.packed, 0x20213200u);
    CHECK_EQ(parsed.build, 8498u);
    CHECK_ST(register_xboxkrnl_kernel_variables_from_xex(other.data(), other.size(), nullptr), Status::Conflict);
  }

  xboxkrnl_kernel_variables_prepare_shutdown();
  CHECK_EQ(runtime_virtual_read(kXboxKrnlVersionVirtualAddress, 2, 0, &value),
           VirtualAccessStatus::NotVirtual);
  CHECK(!find_variable_import(kModuleXboxkrnl, 0x0156, &address));

  {
    // Kernel version policy (3 Oct 2026): a title built with an XDK newer than
    // the final retail kernel (NARUTO STORM 3: 2.0.21173.0) runs when its
    // declared minimum is a real kernel, and sees the final retail kernel.
    Claims newer_xdk;
    newer_xdk.kernel_version = newer_xdk.xam_version = 0x2052B500u;  // 2.0.21173.0
    newer_xdk.kernel_minimum = newer_xdk.xam_minimum = 0x20077100u;  // 2.0.1905.0
    auto naruto = make_profile_xex(newer_xdk);
    KernelCompatibilityProfile claimed, presented;
    CHECK_ST(parse_kernel_compatibility_profile_from_xex(naruto.data(), naruto.size(), &claimed, nullptr),
             Status::Ok);
    CHECK_EQ(claimed.packed, 0x2052B500u);  // the parser reports the title's own claim
    CHECK_EQ(claimed.minimum_packed, 0x20077100u);
    CHECK_ST(presented_kernel_profile(claimed, &presented), Status::Ok);
    CHECK_EQ(presented.packed, kFinalRetailKernelPacked);
    CHECK_EQ(presented.build, 17559u);
    CHECK_EQ(presented.qfe, 0u);
    CHECK_ST(register_xboxkrnl_kernel_variables_from_xex(naruto.data(), naruto.size(), nullptr), Status::Ok);
    KernelCompatibilityProfile active;
    CHECK_ST(xboxkrnl_kernel_compatibility_profile(&active), Status::Ok);
    CHECK_EQ(active.packed, 0x20449700u);
    CHECK_EQ(runtime_virtual_read(kXboxKrnlVersionVirtualAddress + 4, 2, 0, &value), VirtualAccessStatus::Handled);
    CHECK_EQ(value, 17559u);
    xboxkrnl_kernel_variables_prepare_shutdown();

    // In-family profiles are presented unchanged (GTA IV, EFLC).
    KernelCompatibilityProfile gta;
    gta.packed = kPacked6683;
    gta.minimum_packed = kPacked6683;
    gta.build = 6683;
    CHECK_ST(presented_kernel_profile(gta, &presented), Status::Ok);
    CHECK_EQ(presented.packed, kPacked6683);

    // A minimum newer than any retail kernel cannot run on a console.
    Claims impossible = newer_xdk;
    impossible.kernel_minimum = impossible.xam_minimum = 0x20450000u;  // 2.0.17664.0
    auto bad = make_profile_xex(impossible);
    CHECK_ST(register_xboxkrnl_kernel_variables_from_xex(bad.data(), bad.size(), nullptr), Status::Unsupported);
    // A minimum below the family (or outside 2.0) is not a kernel.
    claimed.minimum_packed = 0x20010000u;  // 2.0.256.0
    CHECK_ST(presented_kernel_profile(claimed, &presented), Status::Unsupported);
    claimed.packed = 0x3052B500u;
    claimed.minimum_packed = 0x201A1B00u;
    CHECK_ST(presented_kernel_profile(claimed, &presented), Status::Unsupported);
  }
  runtime_shutdown();
  clear_imports();
  memory.release();
  return test_result("rt_test_kernel_fields");
}
