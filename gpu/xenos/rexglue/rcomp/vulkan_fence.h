#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>
namespace rcomp::xenos {
// Submission completion only: this does not establish completion of a
// presentation engine's semaphore wait. Propagate failure before reuse.
inline VkResult WaitAndResetFence(VkDevice device, VkFence fence,
                                 PFN_vkWaitForFences wait, PFN_vkResetFences reset,
                                 uint64_t timeout = UINT64_MAX) {
    const VkResult result = wait(device, 1, &fence, VK_TRUE, timeout);
    if (result != VK_SUCCESS) return result;
    return reset(device, 1, &fence);
}
}
