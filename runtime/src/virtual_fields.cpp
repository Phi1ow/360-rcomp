#include "rcomp/runtime/virtual_fields.h"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace rcomp::rt {
namespace {

constexpr size_t kMaxProviders = 8;
std::mutex g_virtual_mu;
std::array<VirtualFieldProvider, kMaxProviders> g_providers{};
size_t g_provider_count = 0;

// Read side without a lock: guest code reaches providers on hot paths (for
// example a title spinning on its own thread object while it waits for the
// GPU). Every change under g_virtual_mu publishes a new immutable snapshot;
// readers load one pointer. Superseded snapshots are never freed because a
// reader may still hold one; changes happen only when a runtime starts or
// stops, so this is a few hundred bytes per runtime lifetime.
struct Snapshot {
  size_t count = 0;
  std::array<VirtualFieldProvider, kMaxProviders> providers{};
};
std::atomic<const Snapshot *> g_snapshot{nullptr};

void publish_locked() {
  auto *next = new Snapshot;
  next->count = g_provider_count;
  next->providers = g_providers;
  g_snapshot.store(next, std::memory_order_release);
}

bool same_provider(const VirtualFieldProvider &a,
                   const VirtualFieldProvider &b) {
  return a.base == b.base && a.size == b.size && a.read == b.read &&
         a.write == b.write &&
         ((!a.name && !b.name) ||
          (a.name && b.name && std::strcmp(a.name, b.name) == 0));
}

bool overlaps(const VirtualFieldProvider &a, const VirtualFieldProvider &b) {
  const uint64_t a_end = uint64_t(a.base) + a.size;
  const uint64_t b_end = uint64_t(b.base) + b.size;
  return uint64_t(a.base) < b_end && uint64_t(b.base) < a_end;
}

bool supported_width(uint8_t width) {
  return width == 1 || width == 2 || width == 4 || width == 8;
}

bool find_provider(uint32_t ea, VirtualFieldProvider *out) {
  if (!out)
    return false;
  const Snapshot *snapshot = g_snapshot.load(std::memory_order_acquire);
  if (!snapshot)
    return false;
  for (size_t i = 0; i < snapshot->count; ++i) {
    const auto &provider = snapshot->providers[i];
    const uint64_t provider_end = uint64_t(provider.base) + provider.size;
    if (ea >= provider.base && uint64_t(ea) < provider_end) {
      *out = provider;
      return true;
    }
  }
  return false;
}

void seed_diagnostic(const VirtualFieldProvider &provider, uint32_t ea,
                     uint8_t width, uint32_t lr, bool write,
                     VirtualAccessDiagnostic *diagnostic) {
  if (!diagnostic)
    return;
  diagnostic->provider = provider.name;
  diagnostic->region_base = provider.base;
  diagnostic->offset = ea - provider.base;
  diagnostic->lr = lr;
  diagnostic->width = width;
  diagnostic->write = write;
}

} // namespace

Status register_virtual_field_provider(const VirtualFieldProvider &provider) {
  if (!provider.size || !provider.name || !provider.read ||
      uint64_t(provider.base) + provider.size > 0x100000000ull)
    return Status::InvalidArgument;
  std::lock_guard<std::mutex> lock(g_virtual_mu);
  for (size_t i = 0; i < g_provider_count; ++i) {
    if (same_provider(g_providers[i], provider))
      return Status::Ok;
    if (overlaps(g_providers[i], provider))
      return Status::Conflict;
  }
  if (g_provider_count == g_providers.size())
    return Status::TableFull;
  g_providers[g_provider_count++] = provider;
  publish_locked();
  return Status::Ok;
}

Status unregister_virtual_field_provider(const VirtualFieldProvider &provider) {
  std::lock_guard<std::mutex> lock(g_virtual_mu);
  for (size_t i = 0; i < g_provider_count; ++i) {
    if (!same_provider(g_providers[i], provider))
      continue;
    for (size_t j = i + 1; j < g_provider_count; ++j)
      g_providers[j - 1] = g_providers[j];
    g_providers[--g_provider_count] = {};
    publish_locked();
    return Status::Ok;
  }
  return Status::NotFound;
}

void virtual_fields_reset() {
  std::lock_guard<std::mutex> lock(g_virtual_mu);
  for (auto &provider : g_providers)
    provider = {};
  g_provider_count = 0;
  publish_locked();
}

VirtualAccessStatus runtime_virtual_read(uint32_t ea, uint8_t width,
                                         uint32_t lr, uint64_t *value,
                                         VirtualAccessDiagnostic *diagnostic) {
  VirtualFieldProvider provider;
  if (!find_provider(ea, &provider))
    return VirtualAccessStatus::NotVirtual;
  seed_diagnostic(provider, ea, width, lr, false, diagnostic);
  const uint64_t end = uint64_t(ea) + width;
  if (!value || !supported_width(width) ||
      end > uint64_t(provider.base) + provider.size)
    return VirtualAccessStatus::InvalidWidth;
  return provider.read ? provider.read(ea, width, value, diagnostic)
                       : VirtualAccessStatus::UnknownField;
}

VirtualAccessStatus runtime_virtual_write(uint32_t ea, uint8_t width,
                                          uint64_t value, uint32_t lr,
                                          VirtualAccessDiagnostic *diagnostic) {
  VirtualFieldProvider provider;
  if (!find_provider(ea, &provider))
    return VirtualAccessStatus::NotVirtual;
  seed_diagnostic(provider, ea, width, lr, true, diagnostic);
  const uint64_t end = uint64_t(ea) + width;
  if (!supported_width(width) || end > uint64_t(provider.base) + provider.size)
    return VirtualAccessStatus::InvalidWidth;
  return provider.write ? provider.write(ea, width, value, diagnostic)
                        : VirtualAccessStatus::ReadOnly;
}

const char *virtual_access_status_name(VirtualAccessStatus status) {
  switch (status) {
  case VirtualAccessStatus::NotVirtual:
    return "not_virtual";
  case VirtualAccessStatus::Handled:
    return "handled";
  case VirtualAccessStatus::UnknownField:
    return "unknown_field";
  case VirtualAccessStatus::InvalidWidth:
    return "invalid_width";
  case VirtualAccessStatus::ReadOnly:
    return "read_only";
  case VirtualAccessStatus::Stale:
    return "stale";
  }
  return "invalid";
}

} // namespace rcomp::rt
