// Original R-comp presentation policy. SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <memory>
#include <utility>
namespace rcomp::xenos {
// CPU-only lifetime tag, unique for one VulkanOutput/presenter lifetime.
// Keeping a strong reference prevents allocator-address reuse (ABA).
struct GuestOutputPresentationEpoch {};
class GuestOutputPresentationCursor {
public:
    // The owner serializes access through the presenter's consumer lock.
    // true means visitor completed a real presentation, never a cache hit.
    template <typename Visitor>
    bool PresentIfChanged(const std::shared_ptr<const GuestOutputPresentationEpoch>& epoch,
                          uint64_t refresh_serial, Visitor&& visitor) {
        if (!epoch || (epoch_ == epoch && refresh_serial_ == refresh_serial)) return false;
        if (!std::forward<Visitor>(visitor)()) return false;
        // Commit only after the visitor's presentation/fence success.
        epoch_ = epoch;
        refresh_serial_ = refresh_serial;
        return true;
    }
    // CPU presentation/recreation invalidates the visible output assumption.
    void Reset() { epoch_.reset(); refresh_serial_ = 0; }
private:
    std::shared_ptr<const GuestOutputPresentationEpoch> epoch_;
    uint64_t refresh_serial_ = 0;
};
}  // namespace rcomp::xenos
