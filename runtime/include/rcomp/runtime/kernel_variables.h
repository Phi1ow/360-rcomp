// Runtime-backed xboxkrnl variable imports whose values come from title
// metadata and live R-comp state.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "rcomp/runtime/status.h"
#include "rcomp/runtime/virtual_fields.h"

namespace rcomp::rt {

// Fixed identities inside the first, permanently uncommitted page of the
// opaque runtime arena. The import slots point at these addresses; generated
// loads are served by the virtual-field hook.
constexpr uint32_t kExThreadObjectTypeVirtualAddress =
    kKernelVirtualPageBase + 0x000u;
constexpr uint32_t kXboxKrnlVersionVirtualAddress =
    kKernelVirtualPageBase + 0x100u;
constexpr uint32_t kKeTimeStampBundleVirtualAddress =
    kKernelVirtualPageBase + 0x200u;
constexpr uint32_t kXboxHardwareInfoVirtualAddress =
    kKernelVirtualPageBase + 0x300u;
// ExEventObjectType (ordinal 0x000E): identity-only type token, registered with
// ExThreadObjectType by register_thread_object_type_variable().
constexpr uint32_t kExEventObjectTypeVirtualAddress =
    kKernelVirtualPageBase + 0x400u;
// The title process (X_KPROCESS, 0x60 bytes), the value of every R-comp
// KTHREAD's +0x84 Process pointer. Only +0x1C (BE32 default thread stack
// size, read by xapi's CreateFiber(0, ...)) is published; it equals the
// runtime's real ExCreateThread default, kDefaultGuestThreadStackSize.
// runtime/docs/KERNEL_FIELDS.md, runtime/docs/THREAD_OBJECTS.md ("Guest fibers").
constexpr uint32_t kTitleProcessVirtualAddress =
    kKernelVirtualPageBase + 0x500u;
constexpr uint32_t kTitleProcessSize = 0x60u;
constexpr uint32_t kProcessDefaultStackSizeOffset = 0x1Cu;
// XboxHardwareInfo (ordinal 0x0156): only +0 (BE32 Flags) and +4 (u8 number
// of hardware threads) are published; +5..+0xF stay UnknownField. Values and
// sources: runtime/docs/KERNEL_FIELDS.md.
constexpr uint32_t kXboxHardwareInfoFlags = 0x00000020u;
constexpr uint8_t kXboxHardwareInfoProcessorCount = 6;

struct KernelCompatibilityProfile {
  uint16_t major = 0;
  uint16_t minor = 0;
  uint16_t build = 0;
  uint16_t qfe = 0;
  uint32_t packed = 0;
  // Minimum kernel version the title's xboxkrnl.exe import library declares
  // (0 when the profile was not parsed from a title).
  uint32_t minimum_packed = 0;
};

// Kernel version policy (owner decision 3 Oct 2026, runtime/docs/KERNEL_FIELDS.md).
// The Xbox 360 kernel family is 2.0.1888 to 2.0.17559 (the final retail
// dashboard). A title runs when its declared MINIMUM version is inside that
// family; the version it was built with (its XDK) may be newer than any retail
// kernel. The presented kernel is the title's build version, capped at the
// final retail kernel 2.0.17559.0. Returns Unsupported outside the family.
constexpr uint32_t kFinalRetailKernelPacked = 0x20449700u;  // 2.0.17559.0
Status presented_kernel_profile(const KernelCompatibilityProfile &claimed,
                                KernelCompatibilityProfile *presented);

// Reads the version/min-version claims from the actual XEX import-library
// metadata. For the GTA IV title both xboxkrnl.exe and xam.xex claim
// 0x201A1B00 = 2.0.6683.0; the Episodes from Liberty City executable claims
// 0x20213200 = 2.0.8498.0 (both are accepted, nothing else). Conflicting
// version/min-version claims are rejected; this is a title compatibility
// profile, not a physical console firmware claim.
Status
parse_kernel_compatibility_profile_from_xex(const uint8_t *xex, size_t xex_size,
                                            KernelCompatibilityProfile *out,
                                            FILE *log = stderr);

// Current supported title-derived profile. Registers XboxKrnlVersion (0x0158),
// KeTimeStampBundle (0x00AD) and XboxHardwareInfo (0x0156) before XEX
// relocation. Version bytes are four
// BE16 fields. Timestamp publishes only the observed +0x10 BE32 uptime-ms
// field.
Status register_xboxkrnl_kernel_variables_from_xex(const uint8_t *xex,
                                                   size_t xex_size,
                                                   FILE *log = stderr);
Status xboxkrnl_kernel_compatibility_profile(KernelCompatibilityProfile *out);
// Shared monotonic-origin helper intended for KeQueryTickCount as well as the
// timestamp variable. The current runtime has no KeQueryTickCount export yet.
Status xboxkrnl_uptime_millis(uint32_t *out);
void xboxkrnl_kernel_variables_prepare_shutdown();

// KeTimeStampBundle: only the GTA IV-observed +0x10 BE32 uptime-milliseconds
// field is published. Registration is a title-bootstrap step before XEX import
// binding, not part of generic register_xboxkrnl_hle().

} // namespace rcomp::rt
