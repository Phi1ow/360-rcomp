// R-comp: guest frames on a display plane or a headless surface
// (owner: gpu/xenos). See vulkan_display.h.
#include "vulkan_display.h"
#include "fps_overlay.h"
#include "vulkan_fence.h"
#include "rcomp/diag.h"
#include <rcomp_xenos/diagnostics.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>

namespace rcomp::xenos {

namespace {
namespace vk = rex::ui::vulkan;

// The overlay self-check (RecordOverlayReadback) transitions the 4K swapchain image to and from
// TRANSFER_SRC, which costs GPU time on the queue the command processor shares: measured at
// about -0.5 % of the GPU-limited frame rate when run once a second. It is evidence for the
// console, not part of normal runs: RCOMP_FPS_OVERLAY_CHECK=1 (RCOMP_M6_TUNING_ENV) turns it on.
bool overlay_check_requested() {
    static const bool requested = [] {
        const char* value = getenv("RCOMP_FPS_OVERLAY_CHECK");
        return value && value[0] == '1' && !value[1];
    }();
    return requested;
}

// RCOMP_LUMA_PROBE=1 (RCOMP_M6_TUNING_ENV): the black-frame probe of PresentGuestImage.
bool luma_probe_requested() {
    static const bool requested = [] {
        const char* value = getenv("RCOMP_LUMA_PROBE");
        return value && value[0] == '1' && !value[1];
    }();
    return requested;
}

std::string vk_error(const char* what, VkResult r) { return std::string(what) + " failed (VkResult " + std::to_string(int(r)) + ")"; }

template <typename T>
T instance_fn(const vk::VulkanInstance& inst, const char* name) {
    return reinterpret_cast<T>(inst.functions().vkGetInstanceProcAddr(inst.instance(), name));
}

VkImageMemoryBarrier barrier(VkImage image, VkAccessFlags src, VkAccessFlags dst, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}
}  // namespace

std::unique_ptr<VulkanDisplay> VulkanDisplay::Create(vk::VulkanProvider& provider, uint32_t guest_width,
                                                     uint32_t guest_height, std::string* error) {
    std::unique_ptr<VulkanDisplay> d(new VulkanDisplay());
    d->provider_ = &provider;
    if (!d->Init(guest_width, guest_height, error)) return nullptr;
    return d;
}

VulkanDisplay::~VulkanDisplay() { Destroy(); }

bool VulkanDisplay::Init(uint32_t guest_width, uint32_t guest_height, std::string* error) {
    const vk::VulkanDevice& dev = *provider_->vulkan_device();
    const vk::VulkanInstance& inst = *provider_->vulkan_instance();
    const auto& ifn = inst.functions();
    const auto& dfn = dev.functions();
    const VkPhysicalDevice pd = dev.physical_device();
    const uint32_t qf = dev.queue_family_graphics_compute();
    if (!inst.extensions().ext_KHR_surface) return *error = "instance without VK_KHR_surface", false;
    if (!dev.extensions().ext_KHR_swapchain) return *error = "device without VK_KHR_swapchain", false;

    // 1. Surface: display plane if a display is connected, else headless.
    std::string plane_reason = "instance without VK_KHR_display";
    if (inst.extensions().ext_KHR_display) {
        auto get_displays = instance_fn<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(inst, "vkGetPhysicalDeviceDisplayPropertiesKHR");
        auto get_modes = instance_fn<PFN_vkGetDisplayModePropertiesKHR>(inst, "vkGetDisplayModePropertiesKHR");
        auto get_planes = instance_fn<PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR>(inst, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR");
        auto plane_displays = instance_fn<PFN_vkGetDisplayPlaneSupportedDisplaysKHR>(inst, "vkGetDisplayPlaneSupportedDisplaysKHR");
        auto create_plane = instance_fn<PFN_vkCreateDisplayPlaneSurfaceKHR>(inst, "vkCreateDisplayPlaneSurfaceKHR");
        uint32_t nd = 0;
        if (!get_displays || !get_modes || !get_planes || !plane_displays || !create_plane) {
            plane_reason = "VK_KHR_display commands not resolved";
        } else if (get_displays(pd, &nd, nullptr) != VK_SUCCESS || !nd) {
            plane_reason = "no display";
        } else {
            std::vector<VkDisplayPropertiesKHR> displays(nd);
            get_displays(pd, &nd, displays.data());
            const VkDisplayKHR display = displays[0].display;
            uint32_t nm = 0;
            get_modes(pd, display, &nm, nullptr);
            std::vector<VkDisplayModePropertiesKHR> modes(nm);
            if (nm) get_modes(pd, display, &nm, modes.data());
            uint32_t np = 0;
            get_planes(pd, &np, nullptr);
            uint32_t plane = UINT32_MAX;
            for (uint32_t p = 0; p < np && plane == UINT32_MAX; ++p) {
                uint32_t ns = 0;
                if (plane_displays(pd, p, &ns, nullptr) != VK_SUCCESS || !ns) continue;
                std::vector<VkDisplayKHR> supported(ns);
                plane_displays(pd, p, &ns, supported.data());
                if (std::find(supported.begin(), supported.end(), display) != supported.end()) plane = p;
            }
            if (!nm) {
                plane_reason = "display has no mode";
            } else if (plane == UINT32_MAX) {
                plane_reason = "no plane supports the display";
            } else {
                VkDisplaySurfaceCreateInfoKHR sci = {};
                sci.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
                sci.displayMode = modes[0].displayMode;
                sci.planeIndex = plane;
                sci.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
                sci.globalAlpha = 1.0f;
                sci.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
                sci.imageExtent = modes[0].parameters.visibleRegion;
                const VkResult r = create_plane(inst.instance(), &sci, nullptr, &surface_);
                if (r == VK_SUCCESS) {
                    kind_ = Kind::kDisplayPlane;
                    extent_ = sci.imageExtent;
                } else {
                    plane_reason = vk_error("vkCreateDisplayPlaneSurfaceKHR", r);
                }
            }
        }
    }
    if (surface_ == VK_NULL_HANDLE) {
        if (!inst.extensions().ext_EXT_headless_surface)
            return *error = "no display plane (" + plane_reason + ") and no headless surface", false;
        auto create_headless = instance_fn<PFN_vkCreateHeadlessSurfaceEXT>(inst, "vkCreateHeadlessSurfaceEXT");
        VkHeadlessSurfaceCreateInfoEXT hci = {};
        hci.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
        const VkResult r = create_headless ? create_headless(inst.instance(), &hci, nullptr, &surface_)
                                           : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (r != VK_SUCCESS) return *error = vk_error("vkCreateHeadlessSurfaceEXT", r), false;
        kind_ = Kind::kHeadless;
        extent_ = {guest_width, guest_height};
    }
    VkBool32 can_present = VK_FALSE;
    if (ifn.vkGetPhysicalDeviceSurfaceSupportKHR(pd, qf, surface_, &can_present) != VK_SUCCESS || !can_present)
        return *error = "the graphics queue family cannot present to the surface", false;

    // 2. Swapchain: a UNORM 8-bit format (the frame's values pass through),
    // blit destination, readable back when the surface allows it.
    VkSurfaceCapabilitiesKHR caps;
    if (ifn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface_, &caps) != VK_SUCCESS)
        return *error = "surface capabilities query failed", false;
    if (caps.currentExtent.width != UINT32_MAX) extent_ = caps.currentExtent;
    extent_.width = std::clamp(extent_.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent_.height = std::clamp(extent_.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    uint32_t nf = 0;
    ifn.vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface_, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nf);
    if (nf) ifn.vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface_, &nf, formats.data());
    for (const VkSurfaceFormatKHR& f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            format_ = f.format;
            break;
        }
    if (format_ == VK_FORMAT_UNDEFINED) return *error = "surface offers no 8-bit UNORM format", false;
    VkFormatProperties fp;
    ifn.vkGetPhysicalDeviceFormatProperties(pd, format_, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return *error = "swapchain format is not a blit destination", false;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return *error = "swapchain images cannot be transfer destinations", false;
    can_read_back_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkSwapchainCreateInfoKHR sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = format_;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (can_read_back_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                           ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                           : caps.currentTransform;
    sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                             ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                             : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always supported; paced by the display
    sci.clipped = VK_TRUE;
    const VkDevice device = dev.device();
    VkResult r = dfn.vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain_);
    if (r != VK_SUCCESS) return *error = vk_error("vkCreateSwapchainKHR", r), false;
    uint32_t ni = 0;
    dfn.vkGetSwapchainImagesKHR(device, swapchain_, &ni, nullptr);
    images_.resize(ni);
    dfn.vkGetSwapchainImagesKHR(device, swapchain_, &ni, images_.data());

    // 3. Command buffer, fence, semaphore.
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qf};
    if (dfn.vkCreateCommandPool(device, &pci, nullptr, &pool_) != VK_SUCCESS)
        return *error = "vkCreateCommandPool failed", false;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (dfn.vkAllocateCommandBuffers(device, &cai, &cmd_) != VK_SUCCESS)
        return *error = "vkAllocateCommandBuffers failed", false;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
    VkSemaphoreCreateInfo semci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
    if (dfn.vkCreateFence(device, &fci, nullptr, &fence_) != VK_SUCCESS ||
        dfn.vkCreateSemaphore(device, &semci, nullptr, &rendered_) != VK_SUCCESS)
        return *error = "fence/semaphore creation failed", false;
    cmd_blit_ = reinterpret_cast<PFN_vkCmdBlitImage>(ifn.vkGetDeviceProcAddr(device, "vkCmdBlitImage"));
    if (!cmd_blit_) return *error = "vkCmdBlitImage not resolved", false;
    return true;
}

bool VulkanDisplay::EnsureStaging(uint32_t w, uint32_t h, std::string* error) {
    const vk::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDevice device = dev->device();
    if (w == staging_w_ && h == staging_h_ && frame_ != VK_NULL_HANDLE) return true;
    if (frame_) dfn.vkDestroyImage(device, frame_, nullptr), frame_ = VK_NULL_HANDLE;
    if (frame_mem_) dfn.vkFreeMemory(device, frame_mem_, nullptr), frame_mem_ = VK_NULL_HANDLE;
    if (upload_) dfn.vkDestroyBuffer(device, upload_, nullptr), upload_ = VK_NULL_HANDLE;
    if (upload_mem_) dfn.vkFreeMemory(device, upload_mem_, nullptr), upload_mem_ = VK_NULL_HANDLE;
    if (!vk::util::CreateDedicatedAllocationBuffer(dev, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                   vk::util::MemoryPurpose::kUpload, upload_, upload_mem_,
                                                   &upload_type_, &upload_size_))
        return *error = "upload buffer allocation failed", false;
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!vk::util::CreateDedicatedAllocationImage(dev, ici, vk::util::MemoryPurpose::kDeviceLocal, frame_, frame_mem_))
        return *error = "frame image allocation failed", false;
    staging_w_ = w;
    staging_h_ = h;
    return true;
}

bool VulkanDisplay::SetOverlayText(const char* text, std::string* error) {
    std::string local_error;
    if (!error) error = &local_error;
    const std::string next = text ? text : "";
    if (next == overlay_text_) return true;
    if (next.empty()) {
        overlay_text_.clear();
        overlay_visible_ = false;
        guest_output_presentation_cursor_.Reset();  // repaint the last frame without the text
        return true;
    }
    const vk::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDevice device = dev->device();
    // Legible on a TV at 3840x2160 (scale 4) and still small at 1280x720 (scale 2).
    const uint32_t scale = std::max(2u, extent_.height / 540u);
    OverlaySize size;
    if (!overlay_size(next.size(), scale, &size)) return *error = "overlay text is empty or too long", false;
    // Clear of the screen edge, which a TV may overscan.
    const uint32_t margin = std::max(8u, extent_.height / 40u);
    if (size.width + margin > extent_.width || size.height + margin > extent_.height)
        return *error = "overlay text does not fit the screen", false;
    const VkDeviceSize bytes = VkDeviceSize(size.width) * size.height * 4;
    if (bytes > overlay_size_) {
        DestroyOverlay();
        if (!vk::util::CreateDedicatedAllocationBuffer(dev, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                       vk::util::MemoryPurpose::kUpload, overlay_, overlay_mem_,
                                                       &overlay_type_, &overlay_size_))
            return *error = "overlay buffer allocation failed", false;
    }
    void* mapped = nullptr;
    if (dfn.vkMapMemory(device, overlay_mem_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        return *error = "vkMapMemory failed", false;
    OverlaySize written;
    const bool rendered = overlay_render(next.data(), next.size(), scale, static_cast<uint32_t*>(mapped),
                                         size_t(overlay_size_ / 4), &written);
    if (rendered)  // kept for the self-check of what reaches the swapchain image
        overlay_pixels_.assign(static_cast<const uint32_t*>(mapped),
                               static_cast<const uint32_t*>(mapped) + size_t(written.width) * written.height);
    vk::util::FlushMappedMemoryRange(dev, overlay_mem_, overlay_type_, 0, overlay_size_, overlay_size_);
    dfn.vkUnmapMemory(device, overlay_mem_);
    if (!rendered) return *error = "overlay text could not be rendered", false;
    overlay_text_ = next;
    overlay_width_ = written.width;
    overlay_height_ = written.height;
    overlay_x_ = overlay_y_ = margin;
    overlay_visible_ = true;
    overlay_check_pending_ = overlay_check_requested();
    guest_output_presentation_cursor_.Reset();  // repaint the last frame with the new text
    return true;
}

void VulkanDisplay::RecordOverlay(VkImage target) {
    if (!overlay_visible_) return;
    const auto& dfn = provider_->vulkan_device()->functions();
    // The blit and the overlay write the same image: order them.
    VkImageMemoryBarrier order = barrier(target, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    dfn.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &order);
    VkBufferImageCopy copy = {};
    copy.bufferRowLength = overlay_width_;
    copy.bufferImageHeight = overlay_height_;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {int32_t(overlay_x_), int32_t(overlay_y_), 0};
    copy.imageExtent = {overlay_width_, overlay_height_, 1};
    dfn.vkCmdCopyBufferToImage(cmd_, overlay_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    overlay_check_recorded_ = false;
    if (overlay_check_pending_) RecordOverlayReadback(target);
}

void VulkanDisplay::RecordOverlayReadback(VkImage target) {
    overlay_check_pending_ = false;
    if (overlay_pixels_.empty()) return;
    if (!can_read_back_) {
        if (!overlay_check_reported_) {
            overlay_check_reported_ = true;
            fprintf(stderr, "RCOMP-FPS overlay readback unavailable: the swapchain images cannot be read back\n");
        }
        return;
    }
    const vk::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDeviceSize bytes = VkDeviceSize(overlay_width_) * overlay_height_ * 4;
    if (bytes > overlay_check_size_) {
        DestroyOverlayCheck();
        if (!vk::util::CreateDedicatedAllocationBuffer(dev, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                       vk::util::MemoryPurpose::kReadback, overlay_check_,
                                                       overlay_check_mem_, nullptr, &overlay_check_size_))
            return;
    }
    // The copy above wrote the image: make it readable, read the rectangle back, restore the layout.
    VkImageMemoryBarrier to_source = barrier(target, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    dfn.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_source);
    VkBufferImageCopy back = {};
    back.bufferRowLength = overlay_width_;
    back.bufferImageHeight = overlay_height_;
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    back.imageOffset = {int32_t(overlay_x_), int32_t(overlay_y_), 0};
    back.imageExtent = {overlay_width_, overlay_height_, 1};
    dfn.vkCmdCopyImageToBuffer(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, overlay_check_, 1, &back);
    VkImageMemoryBarrier to_target = barrier(target, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    dfn.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_target);
    overlay_check_recorded_ = true;
}

void VulkanDisplay::VerifyOverlay() {
    overlay_check_recorded_ = false;
    const vk::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDevice device = dev->device();
    void* mapped = nullptr;
    if (dfn.vkMapMemory(device, overlay_check_mem_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return;
    VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, overlay_check_mem_, 0, VK_WHOLE_SIZE};
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
    const uint32_t* got = static_cast<const uint32_t*>(mapped);
    const size_t count = std::min(size_t(overlay_width_) * overlay_height_, overlay_pixels_.size());
    size_t different = 0, white = 0;
    for (size_t i = 0; i < count; ++i) {
        different += got[i] != overlay_pixels_[i];
        white += got[i] == kOverlayWhite;
    }
    dfn.vkUnmapMemory(device, overlay_check_mem_);
    if (!overlay_check_reported_ || different) {
        overlay_check_reported_ = true;
        fprintf(stderr, "RCOMP-FPS overlay readback %ux%u at (%u,%u): %zu of %zu texels differ from the upload, %zu white\n",
                overlay_width_, overlay_height_, overlay_x_, overlay_y_, different, count, white);
    }
}

constexpr uint32_t kLumaGrid = 4, kLumaTile = 8, kLumaTiles = kLumaGrid * kLumaGrid;
constexpr VkDeviceSize kLumaBytes = VkDeviceSize(kLumaTiles) * kLumaTile * kLumaTile * 4;

void VulkanDisplay::RecordLumaProbe(VkImage source, uint32_t w, uint32_t h, VkFormat source_format) {
    luma_recorded_ = false;
    if (!luma_probe_requested() || luma_probe_failed_ || w < kLumaTile * kLumaGrid || h < kLumaTile * kLumaGrid) return;
    if (source_format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 && source_format != VK_FORMAT_R8G8B8A8_UNORM &&
        source_format != VK_FORMAT_B8G8R8A8_UNORM)
        return;
    const vk::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    if (!luma_buffer_) {
        if (!vk::util::CreateDedicatedAllocationBuffer(dev, kLumaBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                       vk::util::MemoryPurpose::kReadback, luma_buffer_, luma_mem_,
                                                       nullptr, &luma_size_)) {
            luma_probe_failed_ = true;
            fprintf(stderr, "RCOMP-LUMA probe unavailable: the buffer could not be created\n");
            return;
        }
    }
    VkBufferImageCopy regions[kLumaTiles];
    for (uint32_t j = 0; j < kLumaGrid; ++j) {
        for (uint32_t i = 0; i < kLumaGrid; ++i) {
            VkBufferImageCopy& region = regions[j * kLumaGrid + i];
            region = {};
            region.bufferOffset = VkDeviceSize(j * kLumaGrid + i) * kLumaTile * kLumaTile * 4;
            region.bufferRowLength = kLumaTile;
            region.bufferImageHeight = kLumaTile;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            const int32_t x = int32_t(uint64_t(w) * (2 * i + 1) / (2 * kLumaGrid)) - int32_t(kLumaTile / 2);
            const int32_t y = int32_t(uint64_t(h) * (2 * j + 1) / (2 * kLumaGrid)) - int32_t(kLumaTile / 2);
            region.imageOffset = {std::clamp(x, 0, int32_t(w - kLumaTile)), std::clamp(y, 0, int32_t(h - kLumaTile)), 0};
            region.imageExtent = {kLumaTile, kLumaTile, 1};
        }
    }
    // The guest image is a blit source here: it is in TRANSFER_SRC_OPTIMAL (the barrier before the blit).
    dfn.vkCmdCopyImageToBuffer(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, luma_buffer_, kLumaTiles, regions);
    luma_format_ = source_format;
    luma_recorded_ = true;
}

void VulkanDisplay::VerifyLumaProbe() {
    luma_recorded_ = false;
    const vk::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDevice device = dev->device();
    void* mapped = nullptr;
    if (dfn.vkMapMemory(device, luma_mem_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return;
    VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, luma_mem_, 0, VK_WHOLE_SIZE};
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
    const uint32_t* texels = static_cast<const uint32_t*>(mapped);
    double sum = 0.0, maximum = 0.0;
    const size_t count = size_t(kLumaTiles) * kLumaTile * kLumaTile;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t texel = texels[i];
        double r, g, b;
        if (luma_format_ == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
            r = double(texel & 1023u) / 1023.0, g = double((texel >> 10) & 1023u) / 1023.0, b = double((texel >> 20) & 1023u) / 1023.0;
        } else if (luma_format_ == VK_FORMAT_B8G8R8A8_UNORM) {
            b = double(texel & 255u) / 255.0, g = double((texel >> 8) & 255u) / 255.0, r = double((texel >> 16) & 255u) / 255.0;
        } else {
            r = double(texel & 255u) / 255.0, g = double((texel >> 8) & 255u) / 255.0, b = double((texel >> 16) & 255u) / 255.0;
        }
        const double luma = 255.0 * (0.2126 * r + 0.7152 * g + 0.0722 * b);
        sum += luma;
        maximum = std::max(maximum, luma);
    }
    dfn.vkUnmapMemory(device, luma_mem_);
    const double mean = sum / double(count);
    const uint64_t now_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (!luma_presents_) luma_first_present_ns_ = now_ns;
    ++luma_presents_;
    const bool black = mean < 3.0 && maximum < 12.0;
    luma_black_ += black;
    luma_dark_ += mean < 12.0;
    const bool moved = luma_last_logged_mean_ < 0.0 || std::abs(mean - luma_last_logged_mean_) > std::max(6.0, 0.25 * luma_last_logged_mean_);
    if ((moved || black) && luma_logged_ < 4000) {
        ++luma_logged_;
        luma_last_logged_mean_ = mean;
        fprintf(stderr, "RCOMP-LUMA f=%llu t=%llu m=%.1f x=%.0f%s\n", (unsigned long long)luma_presents_,
                (unsigned long long)((now_ns - luma_first_present_ns_) / 1000000ull), mean, maximum, black ? " BLACK" : "");
    }
    if (luma_presents_ % 1200 == 0)
        fprintf(stderr, "RCOMP-LUMA-STATS presents=%llu black=%llu dark=%llu logged=%llu\n", (unsigned long long)luma_presents_,
                (unsigned long long)luma_black_, (unsigned long long)luma_dark_, (unsigned long long)luma_logged_);
}

void VulkanDisplay::DestroyLumaProbe() {
    if (!provider_) return;
    const vk::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDevice device = dev->device();
    if (luma_buffer_) dfn.vkDestroyBuffer(device, luma_buffer_, nullptr);
    if (luma_mem_) dfn.vkFreeMemory(device, luma_mem_, nullptr);
    luma_buffer_ = VK_NULL_HANDLE;
    luma_mem_ = VK_NULL_HANDLE;
}

void VulkanDisplay::DestroyOverlayCheck() {
    if (!provider_) return;
    const vk::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDevice device = dev->device();
    if (overlay_check_) dfn.vkDestroyBuffer(device, overlay_check_, nullptr);
    if (overlay_check_mem_) dfn.vkFreeMemory(device, overlay_check_mem_, nullptr);
    overlay_check_ = VK_NULL_HANDLE;
    overlay_check_mem_ = VK_NULL_HANDLE;
    overlay_check_size_ = 0;
}

void VulkanDisplay::DestroyOverlay() {
    if (!provider_) return;
    DestroyOverlayCheck();
    const vk::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDevice device = dev->device();
    if (overlay_) dfn.vkDestroyBuffer(device, overlay_, nullptr);
    if (overlay_mem_) dfn.vkFreeMemory(device, overlay_mem_, nullptr);
    overlay_ = VK_NULL_HANDLE;
    overlay_mem_ = VK_NULL_HANDLE;
    overlay_size_ = 0;
    overlay_visible_ = false;
    overlay_text_.clear();
}

bool VulkanDisplay::Present(const uint8_t* rgbx, uint32_t w, uint32_t h, std::vector<uint8_t>* readback,
                            std::string* error) {
    // A CPU presentation may replace the image kept by the direct path.
    guest_output_presentation_cursor_.Reset();
    const vk::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDevice device = dev->device();
    if (!w || !h) return *error = "empty frame", false;
    if (readback && !can_read_back_) return *error = "swapchain images cannot be read back", false;
    if (!EnsureStaging(w, h, error)) return false;
    if (readback && !readback_) {
        if (!vk::util::CreateDedicatedAllocationBuffer(dev, VkDeviceSize(extent_.width) * extent_.height * 4,
                                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                       vk::util::MemoryPurpose::kReadback, readback_, readback_mem_,
                                                       nullptr, &readback_size_))
            return *error = "readback buffer allocation failed", false;
    }
    // Frame -> upload buffer (alpha forced opaque).
    void* mapped = nullptr;
    if (dfn.vkMapMemory(device, upload_mem_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        return *error = "vkMapMemory failed", false;
    uint8_t* dst = static_cast<uint8_t*>(mapped);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        memcpy(dst + 4 * i, rgbx + 4 * i, 3);
        dst[4 * i + 3] = 0xFF;
    }
    vk::util::FlushMappedMemoryRange(dev, upload_mem_, upload_type_, 0, upload_size_, upload_size_);
    dfn.vkUnmapMemory(device, upload_mem_);

    uint32_t index = 0;
    VkResult r = dfn.vkAcquireNextImageKHR(device, swapchain_, UINT64_MAX, VK_NULL_HANDLE, fence_, &index);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return *error = vk_error("vkAcquireNextImageKHR", r), false;
    r = WaitAndResetFence(device, fence_, dfn.vkWaitForFences, dfn.vkResetFences);
    if (r != VK_SUCCESS) return *error = vk_error("wait/reset display fence", r), false;
    const VkImage target = images_[index];

    // Letterbox rectangle keeping the frame's aspect ratio.
    uint32_t dw = extent_.width, dh = uint32_t(uint64_t(extent_.width) * h / w);
    if (dh > extent_.height) dh = extent_.height, dw = uint32_t(uint64_t(extent_.height) * w / h);
    const int32_t dx = int32_t(extent_.width - dw) / 2, dy = int32_t(extent_.height - dh) / 2;

    // (vkBeginCommandBuffer resets it: pool created with RESET_COMMAND_BUFFER.)
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    dfn.vkBeginCommandBuffer(cmd_, &bi);
    VkImageMemoryBarrier pre[2] = {
        barrier(frame_, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        barrier(target, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
    dfn.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 2, pre);
    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    dfn.vkCmdCopyBufferToImage(cmd_, upload_, frame_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    const VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dfn.vkCmdClearColorImage(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    VkImageMemoryBarrier mid[2] = {
        barrier(frame_, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        barrier(target, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
    dfn.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 2, mid);
    VkImageBlit blit = {};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
    blit.dstOffsets[0] = {dx, dy, 0};
    blit.dstOffsets[1] = {dx + int32_t(dw), dy + int32_t(dh), 1};
    // Same size: exact copy; scaled: linear filtering.
    cmd_blit_(cmd_, frame_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
              (dw == w && dh == h) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    RecordOverlay(target);
    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (readback) {
        VkImageMemoryBarrier rb = barrier(target, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, layout,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        dfn.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &rb);
        layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkBufferImageCopy back = {};
        back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        back.imageExtent = {extent_.width, extent_.height, 1};
        dfn.vkCmdCopyImageToBuffer(cmd_, target, layout, readback_, 1, &back);
    }
    VkImageMemoryBarrier post = barrier(target, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0, layout,
                                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    dfn.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &post);
    dfn.vkEndCommandBuffer(cmd_);

    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_;
    VkPresentInfoKHR pi = {};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    VkResult pr;
    {
        auto queue = dev->AcquireQueue(dev->queue_family_graphics_compute(), 0);
        r = dfn.vkQueueSubmit(queue.queue(), 1, &si, fence_);
        if (r != VK_SUCCESS) return *error = vk_error("vkQueueSubmit", r), false;
        pr = dfn.vkQueuePresentKHR(queue.queue(), &pi);
    }
    r = WaitAndResetFence(device, fence_, dfn.vkWaitForFences, dfn.vkResetFences);
    if (r != VK_SUCCESS) return *error = vk_error("wait/reset display fence", r), false;
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) return *error = vk_error("vkQueuePresentKHR", pr), false;
    if (overlay_check_recorded_) VerifyOverlay();
    if (readback) {
        void* m = nullptr;
        if (dfn.vkMapMemory(device, readback_mem_, 0, VK_WHOLE_SIZE, 0, &m) != VK_SUCCESS)
            return *error = "readback map failed", false;
        VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, readback_mem_, 0, VK_WHOLE_SIZE};
        dfn.vkInvalidateMappedMemoryRanges(device, 1, &mr);
        readback->resize(size_t(extent_.width) * extent_.height * 4);
        memcpy(readback->data(), m, readback->size());
        dfn.vkUnmapMemory(device, readback_mem_);
        if (format_ == VK_FORMAT_B8G8R8A8_UNORM)  // report R8G8B8A8
            for (size_t i = 0; i < readback->size(); i += 4) std::swap((*readback)[i], (*readback)[i + 2]);
    }
    return true;
}

bool VulkanDisplay::PresentGuestImage(const vk::VulkanDevice* source_device,
                                       VkImage source, VkExtent2D source_extent,
                                       VkFormat source_format, VkImageLayout source_layout,
                                       std::string* error) {
    std::string local_error;
    if (!error) error = &local_error;
    ::rcomp::xenos::ProfScope rcomp_prof_display(30);
    const vk::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDevice device = dev->device();
    const uint32_t w = source_extent.width, h = source_extent.height;
    if (source_device != dev) return *error = "guest image belongs to a different Vulkan device", false;
    if (!dev->properties().swapchainMaintenance1)
        return *error = "direct presentation requires enabled swapchainMaintenance1 for safe shutdown", false;
    if (source == VK_NULL_HANDLE || !w || !h) return *error = "empty guest image", false;
    VkFormatProperties source_properties = {};
    dev->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
        dev->physical_device(), source_format, &source_properties);
    if (!(source_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
        return *error = "guest image format is not a blit source", false;
    uint32_t dw = extent_.width, dh = uint32_t(uint64_t(extent_.width) * h / w);
    if (dh > extent_.height) dh = extent_.height, dw = uint32_t(uint64_t(extent_.height) * w / h);
    if (!dw || !dh) return *error = "empty scaled guest image", false;
    const int32_t dx = int32_t(extent_.width - dw) / 2, dy = int32_t(extent_.height - dh) / 2;
    const bool scaled = dw != w || dh != h;
    const VkFilter filter = scaled &&
        (source_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
            ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

    uint32_t index = 0;
    VkResult r = dfn.vkAcquireNextImageKHR(device, swapchain_, UINT64_MAX, VK_NULL_HANDLE, fence_, &index);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return *error = vk_error("vkAcquireNextImageKHR", r), false;
    r = WaitAndResetFence(device, fence_, dfn.vkWaitForFences, dfn.vkResetFences);
    if (r != VK_SUCCESS) return *error = vk_error("wait/reset acquisition fence", r), false;
    if (direct_rendered_.empty()) direct_rendered_.resize(images_.size(), VK_NULL_HANDLE);
    if (direct_presented_.empty()) {
        direct_presented_.resize(images_.size(), VK_NULL_HANDLE);
        direct_present_pending_.resize(images_.size(), false);
    }
    VkFence& presented = direct_presented_[index];
    if (direct_present_pending_[index]) {
        r = WaitAndResetFence(device, presented, dfn.vkWaitForFences, dfn.vkResetFences);
        if (r != VK_SUCCESS)
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "display: presentation fence reuse failed (VkResult %d)", int(r));
        direct_present_pending_[index] = false;
    }
    if (presented == VK_NULL_HANDLE) {
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
        r = dfn.vkCreateFence(device, &fci, nullptr, &presented);
        if (r != VK_SUCCESS) return *error = vk_error("vkCreateFence(present)", r), false;
    }
    VkSemaphore& rendered = direct_rendered_[index];
    if (rendered == VK_NULL_HANDLE) {
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
        r = dfn.vkCreateSemaphore(device, &sci, nullptr, &rendered);
        if (r != VK_SUCCESS) return *error = vk_error("vkCreateSemaphore", r), false;
    }
    const VkImage target = images_[index];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    r = dfn.vkBeginCommandBuffer(cmd_, &bi);
    if (r != VK_SUCCESS) return *error = vk_error("vkBeginCommandBuffer", r), false;
    VkImageMemoryBarrier pre[2] = {
        barrier(source, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, source_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        barrier(target, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
    dfn.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, pre);
    // A full-surface blit writes every pixel; only letterbox borders need black.
    const bool covers_target = dx == 0 && dy == 0 && dw == extent_.width && dh == extent_.height;
    if (!covers_target) {
        const VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dfn.vkCmdClearColorImage(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        VkImageMemoryBarrier clear_order = barrier(target, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        dfn.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &clear_order);
    }
    VkImageBlit blit = {};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
    blit.dstOffsets[0] = {dx, dy, 0};
    blit.dstOffsets[1] = {dx + int32_t(dw), dy + int32_t(dh), 1};
    cmd_blit_(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
    RecordLumaProbe(source, w, h, source_format);
    RecordOverlay(target);
    VkImageMemoryBarrier post[2] = {
        barrier(source, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source_layout),
        barrier(target, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)};
    dfn.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 2, post);
    r = dfn.vkEndCommandBuffer(cmd_);
    if (r != VK_SUCCESS) return *error = vk_error("vkEndCommandBuffer", r), false;
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered;
    VkPresentInfoKHR pi = {};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    VkSwapchainPresentFenceInfoEXT present_fence_info = {};
    present_fence_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
    present_fence_info.swapchainCount = 1;
    present_fence_info.pFences = &presented;
    pi.pNext = &present_fence_info;
    VkResult pr;
    {
        ::rcomp::xenos::ProfScope rcomp_prof_display_queue(28);
        auto queue = dev->AcquireQueue(dev->queue_family_graphics_compute(), 0);
        r = dfn.vkQueueSubmit(queue.queue(), 1, &si, fence_);
        if (r != VK_SUCCESS)
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "display: guest blit submission failed (VkResult %d)", int(r));
        pr = dfn.vkQueuePresentKHR(queue.queue(), &pi);
    }
    // Retain the borrowed guest image and mailbox lock until every read and
    // layout restoration is complete, including the present-failure path.
    r = WaitAndResetFence(device, fence_, dfn.vkWaitForFences, dfn.vkResetFences);
    if (r != VK_SUCCESS)
        // Never release the visitor's consumer lock/image while GPU completion
        // or layout restoration is unknown. This terminates inside the lease.
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "display: guest blit completion failed (VkResult %d)", int(r));
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR)
        // An errored presentation may leave semaphore/fence state uncertain.
        // Do not permit retry or teardown through a normal visitor return.
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "display: guest presentation failed (VkResult %d)", int(pr));
    direct_present_pending_[index] = true;
    if (overlay_check_recorded_) VerifyOverlay();
    if (luma_recorded_) VerifyLumaProbe();
    return true;
}


void VulkanDisplay::Destroy() {
    if (!provider_) return;
    const vk::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDevice device = dev->device();
    // A graphics QueueWaitIdle doesn't prove a WSI semaphore wait is over.
    // Present fences supplied by swapchainMaintenance1 cover those waits.
    for (size_t i = 0; i < direct_presented_.size(); ++i) {
        if (direct_present_pending_[i]) {
            const VkResult completed = dfn.vkWaitForFences(device, 1, &direct_presented_[i], VK_TRUE, UINT64_MAX);
            if (completed != VK_SUCCESS)
                rcomp_fatal(RCOMP_FATAL_PLATFORM, "display: presentation shutdown failed (VkResult %d)", int(completed));
        }
        if (direct_presented_[i] != VK_NULL_HANDLE)
            dfn.vkDestroyFence(device, direct_presented_[i], nullptr);
    }
    direct_presented_.clear();
    direct_present_pending_.clear();
    for (VkSemaphore semaphore : direct_rendered_)
        if (semaphore != VK_NULL_HANDLE) dfn.vkDestroySemaphore(device, semaphore, nullptr);
    direct_rendered_.clear();
    // Every Present waited for its own submission: nothing is in flight.
    DestroyOverlay();
    DestroyLumaProbe();
    if (readback_) dfn.vkDestroyBuffer(device, readback_, nullptr);
    if (readback_mem_) dfn.vkFreeMemory(device, readback_mem_, nullptr);
    if (upload_) dfn.vkDestroyBuffer(device, upload_, nullptr);
    if (upload_mem_) dfn.vkFreeMemory(device, upload_mem_, nullptr);
    if (frame_) dfn.vkDestroyImage(device, frame_, nullptr);
    if (frame_mem_) dfn.vkFreeMemory(device, frame_mem_, nullptr);
    if (rendered_) dfn.vkDestroySemaphore(device, rendered_, nullptr);
    if (fence_) dfn.vkDestroyFence(device, fence_, nullptr);
    if (pool_) dfn.vkDestroyCommandPool(device, pool_, nullptr);
    if (swapchain_) dfn.vkDestroySwapchainKHR(device, swapchain_, nullptr);
    if (surface_) provider_->vulkan_instance()->functions().vkDestroySurfaceKHR(provider_->vulkan_instance()->instance(), surface_, nullptr);
    guest_output_presentation_cursor_.Reset();
    provider_ = nullptr;
}

}  // namespace rcomp::xenos
