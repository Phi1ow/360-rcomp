/*
 * R-comp - Agent 4 (PS5_Vulkan integration).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Command loading shared by the probe and the draw test.
 *
 * Every Vulkan command is resolved through vkGetInstanceProcAddr /
 * vkGetDeviceProcAddr, never linked by name. That is the only shape that works
 * on both targets:
 *   host: vkGetInstanceProcAddr comes from the Khronos loader (-lvulkan);
 *   PS5 : PS5_Vulkan is linked statically (--whole-archive, see
 *         LINK_CONTRACT.md) and its only global Vulkan symbol is
 *         vkGetInstanceProcAddr (driver/ps5vk_instance.c); every other command
 *         is prefixed ps5vk_ and reachable only through it. No loader exists.
 */
#ifndef RCVK_LOADER_H
#define RCVK_LOADER_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The one symbol linked by name, on both targets. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char *pName);

/* Global (instance == NULL) commands. vkEnumerateInstanceVersion is absent on
 * a Vulkan 1.0 implementation: NULL there means "1.0", not an error. */
#define RCVK_GLOBAL_COMMANDS(X)                                                                    \
   X(vkEnumerateInstanceExtensionProperties)                                                       \
   X(vkEnumerateInstanceLayerProperties)                                                           \
   X(vkCreateInstance)

#define RCVK_INSTANCE_COMMANDS(X)                                                                  \
   X(vkDestroyInstance)                                                                            \
   X(vkEnumeratePhysicalDevices)                                                                   \
   X(vkGetPhysicalDeviceProperties)                                                                \
   X(vkGetPhysicalDeviceFeatures)                                                                  \
   X(vkGetPhysicalDeviceMemoryProperties)                                                          \
   X(vkGetPhysicalDeviceQueueFamilyProperties)                                                     \
   X(vkGetPhysicalDeviceFormatProperties)                                                          \
   X(vkEnumerateDeviceExtensionProperties)                                                         \
   X(vkCreateDevice)                                                                               \
   X(vkGetDeviceProcAddr)

/* Resolved only when the matching version / extension is enabled. */
#define RCVK_OPTIONAL_INSTANCE_COMMANDS(X)                                                         \
   X(vkGetPhysicalDeviceFeatures2)                                                                 \
   X(vkDestroySurfaceKHR)                                                                          \
   X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                         \
   X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                    \
   X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                         \
   X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                                    \
   X(vkGetPhysicalDeviceDisplayPropertiesKHR)                                                      \
   X(vkGetPhysicalDeviceDisplayPlanePropertiesKHR)                                                 \
   X(vkGetDisplayPlaneSupportedDisplaysKHR)                                                        \
   X(vkGetDisplayModePropertiesKHR)                                                                \
   X(vkGetDisplayPlaneCapabilitiesKHR)                                                             \
   X(vkCreateDisplayPlaneSurfaceKHR)

#define RCVK_DEVICE_COMMANDS(X)                                                                    \
   X(vkDestroyDevice)                                                                              \
   X(vkGetDeviceQueue)                                                                             \
   X(vkDeviceWaitIdle)                                                                             \
   X(vkQueueSubmit)                                                                                \
   X(vkQueueWaitIdle)                                                                              \
   X(vkCreateBuffer)                                                                               \
   X(vkDestroyBuffer)                                                                              \
   X(vkGetBufferMemoryRequirements)                                                                \
   X(vkBindBufferMemory)                                                                           \
   X(vkCreateImage)                                                                                \
   X(vkDestroyImage)                                                                               \
   X(vkGetImageMemoryRequirements)                                                                 \
   X(vkBindImageMemory)                                                                            \
   X(vkCreateImageView)                                                                            \
   X(vkDestroyImageView)                                                                           \
   X(vkAllocateMemory)                                                                             \
   X(vkFreeMemory)                                                                                 \
   X(vkMapMemory)                                                                                  \
   X(vkUnmapMemory)                                                                                \
   X(vkFlushMappedMemoryRanges)                                                                    \
   X(vkInvalidateMappedMemoryRanges)                                                               \
   X(vkCreateShaderModule)                                                                         \
   X(vkDestroyShaderModule)                                                                        \
   X(vkCreateDescriptorSetLayout)                                                                  \
   X(vkDestroyDescriptorSetLayout)                                                                 \
   X(vkCreateDescriptorPool)                                                                       \
   X(vkDestroyDescriptorPool)                                                                      \
   X(vkAllocateDescriptorSets)                                                                     \
   X(vkUpdateDescriptorSets)                                                                       \
   X(vkCreatePipelineLayout)                                                                       \
   X(vkDestroyPipelineLayout)                                                                      \
   X(vkCreateRenderPass)                                                                           \
   X(vkDestroyRenderPass)                                                                          \
   X(vkCreateFramebuffer)                                                                          \
   X(vkDestroyFramebuffer)                                                                         \
   X(vkCreateGraphicsPipelines)                                                                    \
   X(vkDestroyPipeline)                                                                            \
   X(vkCreateCommandPool)                                                                          \
   X(vkDestroyCommandPool)                                                                         \
   X(vkAllocateCommandBuffers)                                                                     \
   X(vkBeginCommandBuffer)                                                                         \
   X(vkEndCommandBuffer)                                                                           \
   X(vkCmdPipelineBarrier)                                                                         \
   X(vkCmdBeginRenderPass)                                                                         \
   X(vkCmdEndRenderPass)                                                                           \
   X(vkCmdBindPipeline)                                                                            \
   X(vkCmdBindDescriptorSets)                                                                      \
   X(vkCmdBindVertexBuffers)                                                                       \
   X(vkCmdBindIndexBuffer)                                                                         \
   X(vkCmdSetViewport)                                                                             \
   X(vkCmdSetScissor)                                                                              \
   X(vkCmdDrawIndexed)                                                                             \
   X(vkCmdDraw)                                                                             \
   X(vkResetCommandBuffer)                                                                             \
   X(vkCmdCopyImageToBuffer)                                                                       \
   X(vkCmdCopyImage)                                                                               \
   X(vkCmdClearColorImage)                                                                         \
   X(vkCreateFence)                                                                                \
   X(vkDestroyFence)                                                                               \
   X(vkWaitForFences)                                                                              \
   X(vkResetFences)                                                                                \
   X(vkCreateSemaphore)                                                                            \
   X(vkDestroySemaphore)

/* Resolved only when VK_KHR_swapchain is enabled on the device. */
#define RCVK_SWAPCHAIN_COMMANDS(X)                                                                 \
   X(vkCreateSwapchainKHR)                                                                         \
   X(vkDestroySwapchainKHR)                                                                        \
   X(vkGetSwapchainImagesKHR)                                                                      \
   X(vkAcquireNextImageKHR)                                                                        \
   X(vkQueuePresentKHR)

#define RCVK_DECLARE(name) extern PFN_##name rc_##name;
RCVK_GLOBAL_COMMANDS(RCVK_DECLARE)
RCVK_INSTANCE_COMMANDS(RCVK_DECLARE)
RCVK_OPTIONAL_INSTANCE_COMMANDS(RCVK_DECLARE)
RCVK_DEVICE_COMMANDS(RCVK_DECLARE)
RCVK_SWAPCHAIN_COMMANDS(RCVK_DECLARE)
#undef RCVK_DECLARE
extern PFN_vkEnumerateInstanceVersion rc_vkEnumerateInstanceVersion;

/* 0 on success; otherwise names the first missing command on stderr. */
int rcvk_load_global(void);
int rcvk_load_instance(VkInstance instance);
/* Optional commands: NULL when unavailable; never an error. */
void rcvk_load_instance_optional(VkInstance instance);
int rcvk_load_device(VkDevice device);
int rcvk_load_swapchain(VkDevice device);

const char *rcvk_result_name(VkResult r);

#ifdef __cplusplus
}
#endif
#endif
