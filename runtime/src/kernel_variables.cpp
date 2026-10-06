#include "rcomp/runtime/kernel_variables.h"

#include <mutex>
#include <string>
#include <vector>

#include "rcomp/runtime/clock_sync.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xex_loader.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kHeaderImportLibraries = 0x000103FFu;
constexpr size_t kImportLibraryHeader = 0x28u;
// Owner decision (3 Oct 2026): every title sees the kernel version it was built against (the version of its
// xboxkrnl.exe import library, never below that library's minimum). GTA IV (2.0.6683.0) and the Episodes from
// Liberty City executable (2.0.8498.0) import version == minimum, so their profile is unchanged.
// Amendment (3 Oct 2026, Agent 3): a console runs a title when the title's declared MINIMUM kernel is not newer
// than the console's kernel; the import 'version' is the XDK the title was built with, which can be newer than any
// retail kernel (NARUTO STORM 3: 2.0.21173.0). Such a title is accepted when its minimum is inside the family and
// is presented the final retail kernel 2.0.17559.0, the newest kernel a real console could have run it on. A
// minimum outside the Xbox 360 kernel family 2.0 (builds 1888 to 17559) is refused, not guessed.
constexpr uint32_t kMinKernelBuild = 1888, kMaxKernelBuild = 17559;
bool profile_is_supported(uint32_t packed) {
  const uint32_t major = (packed >> 28) & 0xFu, minor = (packed >> 24) & 0xFu, build = (packed >> 8) & 0xFFFFu;
  return major == 2 && minor == 0 && build >= kMinKernelBuild && build <= kMaxKernelBuild;
}
static_assert(((kFinalRetailKernelPacked >> 8) & 0xFFFFu) == kMaxKernelBuild, "final retail kernel");
constexpr uint32_t kXboxKrnlVersionOrdinal = 0x0158u;
constexpr uint32_t kKeTimeStampBundleOrdinal = 0x00ADu;
constexpr uint32_t kXboxHardwareInfoOrdinal = 0x0156u;

uint32_t be32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t be16(const uint8_t *p) {
  return uint16_t((uint16_t(p[0]) << 8) | p[1]);
}

bool ascii_ieq(const std::string &a, const char *b) {
  if (!b)
    return false;
  size_t i = 0;
  for (; i < a.size() && b[i]; ++i) {
    char ac = a[i], bc = b[i];
    if (ac >= 'A' && ac <= 'Z')
      ac = char(ac - 'A' + 'a');
    if (bc >= 'A' && bc <= 'Z')
      bc = char(bc - 'A' + 'a');
    if (ac != bc)
      return false;
  }
  return i == a.size() && b[i] == '\0';
}

KernelCompatibilityProfile unpack_profile(uint32_t packed) {
  KernelCompatibilityProfile profile;
  profile.major = uint16_t((packed >> 28) & 0xFu);
  profile.minor = uint16_t((packed >> 24) & 0xFu);
  profile.build = uint16_t((packed >> 8) & 0xFFFFu);
  profile.qfe = uint16_t(packed & 0xFFu);
  profile.packed = packed;
  return profile;
}

struct VersionClaim {
  bool present = false;
  uint32_t version = 0;
  uint32_t minimum = 0;
};

struct KernelVariableState {
  bool active = false;
  uint64_t generation = 0;
  uint64_t origin_ns = 0;
  KernelCompatibilityProfile profile{};
};

std::mutex g_kernel_variable_mu;
KernelVariableState g_kernel_variable_state;

bool valid_scalar_width(uint8_t width) {
  return width == 1 || width == 2 || width == 4 || width == 8;
}

void set_diag(VirtualAccessDiagnostic *diagnostic, const char *name,
              uint32_t base, uint32_t ea) {
  if (!diagnostic)
    return;
  diagnostic->provider = name;
  diagnostic->region_base = base;
  diagnostic->offset = ea - base;
}

VirtualAccessStatus kernel_variable_read(uint32_t ea, uint8_t width,
                                         uint64_t *value,
                                         VirtualAccessDiagnostic *diagnostic) {
  if (!value || !valid_scalar_width(width))
    return VirtualAccessStatus::InvalidWidth;

  if (ea >= kXboxKrnlVersionVirtualAddress &&
      uint64_t(ea) + width <= uint64_t(kXboxKrnlVersionVirtualAddress) + 8u) {
    KernelCompatibilityProfile profile;
    {
      std::lock_guard<std::mutex> lock(g_kernel_variable_mu);
      if (!g_kernel_variable_state.active)
        return VirtualAccessStatus::Stale;
      profile = g_kernel_variable_state.profile;
    }
    const uint8_t bytes[8] = {
        uint8_t(profile.major >> 8), uint8_t(profile.major),
        uint8_t(profile.minor >> 8), uint8_t(profile.minor),
        uint8_t(profile.build >> 8), uint8_t(profile.build),
        uint8_t(profile.qfe >> 8),   uint8_t(profile.qfe),
    };
    const uint32_t offset = ea - kXboxKrnlVersionVirtualAddress;
    uint64_t result = 0;
    for (uint8_t i = 0; i < width; ++i)
      result = (result << 8) | bytes[offset + i];
    *value = result;
    set_diag(diagnostic, "XboxKrnlVersion", kXboxKrnlVersionVirtualAddress, ea);
    return VirtualAccessStatus::Handled;
  }

  if (ea >= kKeTimeStampBundleVirtualAddress &&
      ea < kKeTimeStampBundleVirtualAddress + 0x100u) {
    set_diag(diagnostic, "KeTimeStampBundle", kKeTimeStampBundleVirtualAddress,
             ea);
    if (ea != kKeTimeStampBundleVirtualAddress + 0x10u)
      return VirtualAccessStatus::UnknownField;
    if (width != 4)
      return VirtualAccessStatus::InvalidWidth;
    uint32_t millis = 0;
    const Status status = xboxkrnl_uptime_millis(&millis);
    if (status != Status::Ok)
      return VirtualAccessStatus::Stale;
    *value = millis;
    return VirtualAccessStatus::Handled;
  }

  if (ea >= kXboxHardwareInfoVirtualAddress &&
      ea < kXboxHardwareInfoVirtualAddress + 0x100u) {
    set_diag(diagnostic, "XboxHardwareInfo", kXboxHardwareInfoVirtualAddress,
             ea);
    // Established bytes: Flags (BE32 at +0) and the processor count (+4).
    if (uint64_t(ea) + width > uint64_t(kXboxHardwareInfoVirtualAddress) + 5u)
      return VirtualAccessStatus::UnknownField;
    {
      std::lock_guard<std::mutex> lock(g_kernel_variable_mu);
      if (!g_kernel_variable_state.active)
        return VirtualAccessStatus::Stale;
    }
    const uint8_t bytes[5] = {
        uint8_t(kXboxHardwareInfoFlags >> 24),
        uint8_t(kXboxHardwareInfoFlags >> 16),
        uint8_t(kXboxHardwareInfoFlags >> 8), uint8_t(kXboxHardwareInfoFlags),
        kXboxHardwareInfoProcessorCount};
    const uint32_t offset = ea - kXboxHardwareInfoVirtualAddress;
    uint64_t result = 0;
    for (uint8_t i = 0; i < width; ++i)
      result = (result << 8) | bytes[offset + i];
    *value = result;
    return VirtualAccessStatus::Handled;
  }

  if (ea >= kExThreadObjectTypeVirtualAddress &&
      ea < kExThreadObjectTypeVirtualAddress + 0x100u) {
    set_diag(diagnostic, "ExThreadObjectType",
             kExThreadObjectTypeVirtualAddress, ea);
    return VirtualAccessStatus::UnknownField;
  }
  if (ea >= kExEventObjectTypeVirtualAddress &&
      ea < kExEventObjectTypeVirtualAddress + 0x100u) {
    set_diag(diagnostic, "ExEventObjectType",
             kExEventObjectTypeVirtualAddress, ea);
    return VirtualAccessStatus::UnknownField;
  }
  if (ea >= kTitleProcessVirtualAddress &&
      ea < kTitleProcessVirtualAddress + 0x100u) {
    set_diag(diagnostic, "KPROCESS(title)", kTitleProcessVirtualAddress, ea);
    if (ea != kTitleProcessVirtualAddress + kProcessDefaultStackSizeOffset)
      return VirtualAccessStatus::UnknownField;
    if (width != 4)
      return VirtualAccessStatus::InvalidWidth;
    {
      std::lock_guard<std::mutex> lock(g_kernel_variable_mu);
      if (!g_kernel_variable_state.active)
        return VirtualAccessStatus::Stale;
    }
    *value = kDefaultGuestThreadStackSize;
    return VirtualAccessStatus::Handled;
  }

  set_diag(diagnostic, "kernel-token-page", kKernelVirtualPageBase, ea);
  return VirtualAccessStatus::UnknownField;
}

VirtualAccessStatus kernel_variable_write(uint32_t ea, uint8_t width, uint64_t,
                                          VirtualAccessDiagnostic *diagnostic) {
  if (!valid_scalar_width(width))
    return VirtualAccessStatus::InvalidWidth;
  if (ea >= kXboxKrnlVersionVirtualAddress &&
      uint64_t(ea) + width <= uint64_t(kXboxKrnlVersionVirtualAddress) + 8u) {
    set_diag(diagnostic, "XboxKrnlVersion", kXboxKrnlVersionVirtualAddress, ea);
    return VirtualAccessStatus::ReadOnly;
  }
  if (ea == kKeTimeStampBundleVirtualAddress + 0x10u && width == 4) {
    set_diag(diagnostic, "KeTimeStampBundle", kKeTimeStampBundleVirtualAddress,
             ea);
    return VirtualAccessStatus::ReadOnly;
  }
  if (ea >= kKeTimeStampBundleVirtualAddress &&
      ea < kKeTimeStampBundleVirtualAddress + 0x100u) {
    set_diag(diagnostic, "KeTimeStampBundle", kKeTimeStampBundleVirtualAddress,
             ea);
    return VirtualAccessStatus::UnknownField;
  }
  if (ea >= kXboxHardwareInfoVirtualAddress &&
      ea < kXboxHardwareInfoVirtualAddress + 0x100u) {
    set_diag(diagnostic, "XboxHardwareInfo", kXboxHardwareInfoVirtualAddress,
             ea);
    return uint64_t(ea) + width <= uint64_t(kXboxHardwareInfoVirtualAddress) + 5u
               ? VirtualAccessStatus::ReadOnly
               : VirtualAccessStatus::UnknownField;
  }
  if (ea >= kExThreadObjectTypeVirtualAddress &&
      ea < kExThreadObjectTypeVirtualAddress + 0x100u) {
    set_diag(diagnostic, "ExThreadObjectType",
             kExThreadObjectTypeVirtualAddress, ea);
    return VirtualAccessStatus::UnknownField;
  }
  if (ea >= kExEventObjectTypeVirtualAddress &&
      ea < kExEventObjectTypeVirtualAddress + 0x100u) {
    set_diag(diagnostic, "ExEventObjectType",
             kExEventObjectTypeVirtualAddress, ea);
    return VirtualAccessStatus::UnknownField;
  }
  if (ea >= kTitleProcessVirtualAddress &&
      ea < kTitleProcessVirtualAddress + 0x100u) {
    set_diag(diagnostic, "KPROCESS(title)", kTitleProcessVirtualAddress, ea);
    return ea == kTitleProcessVirtualAddress + kProcessDefaultStackSizeOffset &&
                   width == 4
               ? VirtualAccessStatus::ReadOnly
               : VirtualAccessStatus::UnknownField;
  }
  set_diag(diagnostic, "kernel-token-page", kKernelVirtualPageBase, ea);
  return VirtualAccessStatus::UnknownField;
}

const VirtualFieldProvider kKernelVariableProvider{
    kKernelVirtualPageBase, kKernelVirtualPageSize, "kernel-token-page",
    &kernel_variable_read, &kernel_variable_write};

} // namespace

Status
parse_kernel_compatibility_profile_from_xex(const uint8_t *xex, size_t xex_size,
                                            KernelCompatibilityProfile *out,
                                            FILE *log) {
  if (!xex || !out)
    return Status::InvalidArgument;
  std::vector<XexHeaderField> fields;
  Status status = validate_xex_header(xex, xex_size, &fields);
  if (status != Status::Ok)
    return status;
  const XexHeaderField *imports = nullptr;
  for (const auto &field : fields) {
    if (field.key == kHeaderImportLibraries) {
      imports = &field;
      break;
    }
  }
  if (!imports || uint64_t(imports->offset) + imports->size > xex_size ||
      imports->size < 12)
    return Status::NotFound;

  const uint8_t *hdr = xex + imports->offset;
  const uint32_t total = be32(hdr);
  const uint32_t strtab_len = be32(hdr + 4);
  const uint32_t library_count = be32(hdr + 8);
  if (total != imports->size || uint64_t(12) + strtab_len > total)
    return Status::InvalidArgument;

  std::vector<std::string> names;
  for (size_t off = 0; off < strtab_len && names.size() < library_count;) {
    const char *string = reinterpret_cast<const char *>(hdr + 12 + off);
    size_t len = 0;
    while (off + len < strtab_len && string[len] != '\0')
      ++len;
    if (off + len == strtab_len)
      return Status::InvalidArgument;
    names.emplace_back(string, len);
    off += (len + 1 + 3) & ~size_t(3);
  }
  if (names.size() != library_count)
    return Status::InvalidArgument;

  VersionClaim kernel;
  VersionClaim xam;
  size_t off = 12 + strtab_len;
  for (uint32_t i = 0; i < library_count; ++i) {
    if (off + kImportLibraryHeader > total)
      return Status::InvalidArgument;
    const uint8_t *lib = hdr + off;
    const uint32_t lib_size = be32(lib);
    const uint16_t name_index = be16(lib + 0x24);
    const uint16_t import_count = be16(lib + 0x26);
    if (name_index >= names.size() ||
        lib_size < kImportLibraryHeader + 4u * import_count ||
        uint64_t(off) + lib_size > total)
      return Status::InvalidArgument;
    VersionClaim claim{true, be32(lib + 0x1C), be32(lib + 0x20)};
    VersionClaim *target = nullptr;
    if (ascii_ieq(names[name_index], "xboxkrnl.exe"))
      target = &kernel;
    else if (ascii_ieq(names[name_index], "xam.xex"))
      target = &xam;
    if (target) {
      if (target->present)
        return Status::Conflict;
      *target = claim;
      if (claim.version < claim.minimum) {
        if (log)
          fprintf(log,
                  "RCOMP-KERNEL profile_conflict module=%s version=0x%08X "
                  "below min=0x%08X\n",
                  names[name_index].c_str(), claim.version, claim.minimum);
        return Status::Conflict;
      }
    }
    off += lib_size;
  }
  if (off != total)
    return Status::InvalidArgument;
  if (!kernel.present)
    return Status::NotFound;
  // XamGetSystemVersion reports the same profile: XAM and the kernel ship together in one system version, so
  // a title cannot have been built against a newer XAM than its kernel.
  if (xam.present && xam.version > kernel.version) {
    if (log)
      fprintf(log, "RCOMP-KERNEL profile_conflict xboxkrnl_version=0x%08X below xam_version=0x%08X\n",
              kernel.version, xam.version);
    return Status::Conflict;
  }

  *out = unpack_profile(kernel.version);
  out->minimum_packed = kernel.minimum;
  if (log)
    fprintf(log,
            "RCOMP-KERNEL profile_source=title_import module=xboxkrnl.exe "
            "version=0x%08X "
            "min=0x%08X compatibility=%u.%u.%u.%u\n",
            kernel.version, kernel.minimum, unsigned(out->major),
            unsigned(out->minor), unsigned(out->build), unsigned(out->qfe));
  return Status::Ok;
}

Status presented_kernel_profile(const KernelCompatibilityProfile &claimed,
                                KernelCompatibilityProfile *presented) {
  if (!presented)
    return Status::InvalidArgument;
  if (profile_is_supported(claimed.packed)) {
    *presented = claimed;
    return Status::Ok;
  }
  // Built with a newer XDK than any retail kernel: it runs on a console whose
  // kernel is at least its minimum, so present the final retail kernel.
  const uint32_t major = (claimed.packed >> 28) & 0xFu, minor = (claimed.packed >> 24) & 0xFu;
  const uint32_t build = (claimed.packed >> 8) & 0xFFFFu;
  if (major == 2 && minor == 0 && build > kMaxKernelBuild && claimed.minimum_packed <= claimed.packed &&
      profile_is_supported(claimed.minimum_packed)) {
    *presented = unpack_profile(kFinalRetailKernelPacked);
    presented->minimum_packed = claimed.minimum_packed;
    return Status::Ok;
  }
  return Status::Unsupported;
}

Status register_xboxkrnl_kernel_variables_from_xex(const uint8_t *xex,
                                                   size_t xex_size, FILE *log) {
  Runtime *owner = runtime();
  if (!owner)
    return Status::NotInitialized;
  KernelCompatibilityProfile claimed, profile;
  Status status =
      parse_kernel_compatibility_profile_from_xex(xex, xex_size, &claimed, log);
  if (status != Status::Ok)
    return status;
  if (presented_kernel_profile(claimed, &profile) != Status::Ok) {
    if (log)
      fprintf(log,
              "RCOMP-KERNEL profile_unsupported packed=0x%08X min=0x%08X (minimum not an Xbox 360 kernel "
              "2.0.%u-2.0.%u) scope=title_compatibility\n",
              claimed.packed, claimed.minimum_packed, unsigned(kMinKernelBuild), unsigned(kMaxKernelBuild));
    return Status::Unsupported;
  }
  if (log && profile.packed != claimed.packed)
    fprintf(log,
            "RCOMP-KERNEL profile_capped built=0x%08X min=0x%08X presented=0x%08X (final retail kernel)\n",
            claimed.packed, claimed.minimum_packed, profile.packed);

  {
    std::lock_guard<std::mutex> lock(g_kernel_variable_mu);
    if (g_kernel_variable_state.active) {
      if (g_kernel_variable_state.generation != owner->generation ||
          g_kernel_variable_state.profile.packed != profile.packed)
        return Status::Conflict;
      status = register_virtual_field_provider(kKernelVariableProvider);
      if (status != Status::Ok)
        return status;
      status = register_variable_import(
          kModuleXboxkrnl, 0x0158,
          kXboxKrnlVersionVirtualAddress, "XboxKrnlVersion");
      if (status != Status::Ok)
        return status;
      status = register_variable_import(
          kModuleXboxkrnl, 0x00AD,
          kKeTimeStampBundleVirtualAddress, "KeTimeStampBundle");
      if (status != Status::Ok)
        return status;
      return register_variable_import(kModuleXboxkrnl, 0x0156,
                                      kXboxHardwareInfoVirtualAddress,
                                      "XboxHardwareInfo");
    }
  }

  // clock_gettime may fail fatally; sample it before taking runtime state
  // locks.
  const uint64_t origin_ns = monotonic_ns();
  status = register_virtual_field_provider(kKernelVariableProvider);
  if (status != Status::Ok)
    return status;
  status = register_variable_import(kModuleXboxkrnl, 0x0158,
                                    kXboxKrnlVersionVirtualAddress,
                                    "XboxKrnlVersion");
  if (status != Status::Ok) {
    unregister_virtual_field_provider(kKernelVariableProvider);
    return status;
  }
  status = register_variable_import(kModuleXboxkrnl, 0x00AD,
                                    kKeTimeStampBundleVirtualAddress,
                                    "KeTimeStampBundle");
  if (status != Status::Ok) {
    unregister_variable_import(kModuleXboxkrnl, kXboxKrnlVersionOrdinal);
    unregister_virtual_field_provider(kKernelVariableProvider);
    return status;
  }
  status = register_variable_import(kModuleXboxkrnl, 0x0156,
                                    kXboxHardwareInfoVirtualAddress,
                                    "XboxHardwareInfo");
  if (status != Status::Ok) {
    unregister_variable_import(kModuleXboxkrnl, kKeTimeStampBundleOrdinal);
    unregister_variable_import(kModuleXboxkrnl, kXboxKrnlVersionOrdinal);
    unregister_virtual_field_provider(kKernelVariableProvider);
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(g_kernel_variable_mu);
    if (g_kernel_variable_state.active)
      return Status::Conflict;
    g_kernel_variable_state.active = true;
    g_kernel_variable_state.generation = owner->generation;
    g_kernel_variable_state.origin_ns = origin_ns;
    g_kernel_variable_state.profile = profile;
  }
  return Status::Ok;
}

Status xboxkrnl_kernel_compatibility_profile(KernelCompatibilityProfile *out) {
  if (!out)
    return Status::InvalidArgument;
  std::lock_guard<std::mutex> lock(g_kernel_variable_mu);
  if (!g_kernel_variable_state.active)
    return Status::NotInitialized;
  *out = g_kernel_variable_state.profile;
  return Status::Ok;
}

Status xboxkrnl_uptime_millis(uint32_t *out) {
  if (!out)
    return Status::InvalidArgument;
  uint64_t origin_ns = 0;
  {
    std::lock_guard<std::mutex> lock(g_kernel_variable_mu);
    if (!g_kernel_variable_state.active)
      return Status::NotInitialized;
    origin_ns = g_kernel_variable_state.origin_ns;
  }
  const uint64_t now_ns = monotonic_ns();
  if (now_ns < origin_ns)
    return Status::Conflict;
  *out = uint32_t((now_ns - origin_ns) / 1000000ull);
  return Status::Ok;
}

void xboxkrnl_kernel_variables_prepare_shutdown() {
  {
    std::lock_guard<std::mutex> lock(g_kernel_variable_mu);
    g_kernel_variable_state = {};
  }
  unregister_variable_import(kModuleXboxkrnl, kXboxKrnlVersionOrdinal);
  unregister_variable_import(kModuleXboxkrnl, kKeTimeStampBundleOrdinal);
  unregister_variable_import(kModuleXboxkrnl, kXboxHardwareInfoOrdinal);
  unregister_virtual_field_provider(kKernelVariableProvider);
}

} // namespace rcomp::rt
