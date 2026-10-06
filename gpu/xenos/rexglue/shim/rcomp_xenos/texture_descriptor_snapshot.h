// Exact descriptor payload cache. Resource residency and lifetime are owned by
// the Xenos texture cache; reset this snapshot when a submission can reclaim
// resources. No hash or guest-memory dirty tracking is inferred here.
#pragma once

#include <cstddef>
#include <vector>
#include <rex/ui/vulkan/api.h>

namespace rcomp::xenos {
class TextureDescriptorSnapshot {
 public:
  bool Matches(VkDescriptorSetLayout layout, const VkDescriptorImageInfo* infos,
               size_t count) const {
    if (!valid_ || layout_ != layout || infos_.size() != count) return false;
    for (size_t i = 0; i < count; ++i) {
      if (infos_[i].sampler != infos[i].sampler ||
          infos_[i].imageView != infos[i].imageView ||
          infos_[i].imageLayout != infos[i].imageLayout) return false;
    }
    return true;
  }

  void Remember(VkDescriptorSetLayout layout, const VkDescriptorImageInfo* infos,
                size_t count) {
    if (count) infos_.assign(infos, infos + count);
    else infos_.clear();
    layout_ = layout;
    valid_ = true;
  }

  void Reset() { valid_ = false; }

 private:
  bool valid_ = false;
  VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorImageInfo> infos_;
};
}  // namespace rcomp::xenos
