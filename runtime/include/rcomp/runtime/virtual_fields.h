// Virtual guest fields backed by runtime state instead of committed guest
// pages. Providers are registered during title bootstrap and removed only
// after guest execution is quiescent.
#pragma once

#include <stdint.h>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// The existing opaque runtime reservation. The first 64 KiB page is reserved
// for kernel variable/type tokens; thread Body tokens begin on the next page.
// None of these addresses is backed by a committed guest page.
constexpr uint32_t kOpaqueRuntimeArenaBase = 0x70000000u;
constexpr uint32_t kOpaqueRuntimeArenaSize = 0x01000000u;
constexpr uint32_t kKernelVirtualPageBase = kOpaqueRuntimeArenaBase;
constexpr uint32_t kKernelVirtualPageSize = 0x00010000u;
constexpr uint32_t kThreadVirtualTokenBase =
    kKernelVirtualPageBase + kKernelVirtualPageSize;
constexpr uint32_t kThreadVirtualTokenStride = 0x00001000u;
constexpr uint32_t kOpaqueRuntimeArenaEnd =
    kOpaqueRuntimeArenaBase + kOpaqueRuntimeArenaSize;

enum class VirtualAccessStatus : uint8_t {
  NotVirtual = 0, // no provider covers this effective address
  Handled,        // value consumed/produced by the provider
  UnknownField,   // provider owns the address but the field has no contract
  InvalidWidth,   // field exists but this access width is unsupported
  ReadOnly,       // a write targeted a published read-only field
  Stale, // token/range belongs to the provider but no live object owns it
};

struct VirtualAccessDiagnostic {
  const char *provider = nullptr; // static provider/field name
  uint32_t region_base = 0;       // object/variable base, refined by provider
  uint32_t offset = 0;
  uint32_t lr = 0;
  uint8_t width = 0; // bytes
  bool write = false;
};

using VirtualReadCallback =
    VirtualAccessStatus (*)(uint32_t ea, uint8_t width, uint64_t *value,
                            VirtualAccessDiagnostic *diagnostic);
using VirtualWriteCallback =
    VirtualAccessStatus (*)(uint32_t ea, uint8_t width, uint64_t value,
                            VirtualAccessDiagnostic *diagnostic);

struct VirtualFieldProvider {
  uint32_t base = 0;
  uint32_t size = 0;
  const char *name = nullptr; // static storage
  VirtualReadCallback read = nullptr;
  VirtualWriteCallback write = nullptr;
};

// Registration is a bootstrap/shutdown operation. Provider ranges may not
// overlap. Dispatch copies the provider descriptor while holding the registry
// mutex and calls it only after releasing that mutex.
Status register_virtual_field_provider(const VirtualFieldProvider &provider);
Status unregister_virtual_field_provider(const VirtualFieldProvider &provider);
void virtual_fields_reset();

// Values use guest architectural scalar semantics: a BE32 field containing
// bytes 12 34 56 78 returns 0x12345678. The CPU/prelude hook therefore does
// not byte-swap a Handled result again. No function here calls rcomp_fatal for
// an unimplemented/stale field; the caller owns the final diagnostic after all
// provider locks/object references have been released.
VirtualAccessStatus
runtime_virtual_read(uint32_t ea, uint8_t width, uint32_t lr, uint64_t *value,
                     VirtualAccessDiagnostic *diagnostic = nullptr);
VirtualAccessStatus
runtime_virtual_write(uint32_t ea, uint8_t width, uint64_t value, uint32_t lr,
                      VirtualAccessDiagnostic *diagnostic = nullptr);

const char *virtual_access_status_name(VirtualAccessStatus status);

} // namespace rcomp::rt
