// R-comp: puts the guest's presented frames on a real screen (owner:
// gpu/xenos). R-comp has no window system, so rexglue's presenter (built
// around a Window) never paints; this owns a swapchain on:
//   * a display plane (VK_KHR_display): the PS5's screen under PS5_Vulkan;
//   * a headless surface (VK_EXT_headless_surface): host tests (lavapipe).
// Each frame is the presenter's guest output (VulkanOutput::Capture) scaled
// into the swapchain image with its aspect ratio kept (black bars), then
// presented. The frame travels through host memory once per present: simple
// and correct, not the fastest path (a GPU-side copy from the guest output
// image is the later optimisation).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/ui/vulkan/api.h>

#include "guest_output_presentation_cursor.h"

namespace rex::ui::vulkan {
class VulkanProvider;
class VulkanDevice;
}

namespace rcomp::xenos {

class VulkanDisplay {
public:
    enum class Kind { kDisplayPlane, kHeadless };

    // Prefers a display plane with a connected display; falls back to a
    // headless surface of guest_width x guest_height when the instance has
    // one. nullptr + *error otherwise. The provider must have been created
    // with presentation (VulkanOutput::Create(with_display = true)).
    static std::unique_ptr<VulkanDisplay> Create(rex::ui::vulkan::VulkanProvider& provider, uint32_t guest_width,
                                                 uint32_t guest_height, std::string* error);
    ~VulkanDisplay();

    Kind kind() const { return kind_; }
    uint32_t width() const { return extent_.width; }
    uint32_t height() const { return extent_.height; }
    VkFormat format() const { return format_; }

    // Scales `rgbx` (R8G8B8X8 rows, w x h) into the next swapchain image and
    // presents it. `readback` (optional): the presented image, R8G8B8A8
    // rows of width() x height(), read back before presentation (tests).
    bool Present(const uint8_t* rgbx, uint32_t w, uint32_t h, std::vector<uint8_t>* readback, std::string* error);

    // Borrowed presenter image on this same device/queue. Completes its blit
    // and restores source_layout before returning, including present errors.
    bool PresentGuestImage(const rex::ui::vulkan::VulkanDevice* source_device,
                           VkImage source, VkExtent2D source_extent, VkFormat source_format,
                           VkImageLayout source_layout, std::string* error);

    // Text in the top-left corner of every frame presented from now on (the
    // frame-rate counter). Empty text removes it; nothing is drawn unless this
    // is called. Call it from the thread that presents. The text is copied into
    // the swapchain image after the frame blit, so the guest image is never
    // modified. False with *error if the text cannot be drawn (too long, or
    // larger than the screen); the previous text stays.
    bool SetOverlayText(const char* text, std::string* error);

private:
    friend class VulkanOutput;
    GuestOutputPresentationCursor guest_output_presentation_cursor_;
    VulkanDisplay() = default;
    bool Init(uint32_t guest_width, uint32_t guest_height, std::string* error);
    bool EnsureStaging(uint32_t w, uint32_t h, std::string* error);
    // Records the barrier and the copy of the overlay into `target` (in
    // TRANSFER_DST_OPTIMAL, after the blit) on cmd_; nothing when no text is set.
    void RecordOverlay(VkImage target);
    void DestroyOverlay();
    // Self-check: read the drawn rectangle back from the swapchain image and compare it with the
    // uploaded texels (RecordOverlayReadback in the present command buffer, VerifyOverlay after it).
    void RecordOverlayReadback(VkImage target);
    void VerifyOverlay();
    void DestroyOverlayCheck();
    // Black-frame probe (RCOMP_LUMA_PROBE=1): sixteen 8x8 tiles of the guest image are copied into a host buffer by the present command buffer (RecordLumaProbe, the image in
    // TRANSFER_SRC_OPTIMAL) and judged after its fence (VerifyLumaProbe).
    void RecordLumaProbe(VkImage source, uint32_t w, uint32_t h, VkFormat source_format);
    void VerifyLumaProbe();
    void DestroyLumaProbe();
    void Destroy();

    rex::ui::vulkan::VulkanProvider* provider_ = nullptr;
    Kind kind_ = Kind::kHeadless;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    bool can_read_back_ = false;
    std::vector<VkImage> images_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore rendered_ = VK_NULL_HANDLE;
    // Reuse only after acquiring this swapchain image again. A graphics
    // submission fence alone does not cover the presentation semaphore wait.
    std::vector<VkSemaphore> direct_rendered_;
    // Actual presentation completion, not just graphics queue completion.
    std::vector<VkFence> direct_presented_;
    std::vector<bool> direct_present_pending_;
    // Staging: the frame as an image of its own size, fed from a buffer.
    uint32_t staging_w_ = 0, staging_h_ = 0;
    VkBuffer upload_ = VK_NULL_HANDLE;
    VkDeviceMemory upload_mem_ = VK_NULL_HANDLE;
    VkDeviceSize upload_size_ = 0;
    uint32_t upload_type_ = 0;
    VkImage frame_ = VK_NULL_HANDLE;
    VkDeviceMemory frame_mem_ = VK_NULL_HANDLE;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_mem_ = VK_NULL_HANDLE;
    VkDeviceSize readback_size_ = 0;
    PFN_vkCmdBlitImage cmd_blit_ = nullptr;
    // Overlay text (SetOverlayText): a host-visible buffer holding the bitmap.
    // Every present waits for its own submission, so the buffer is never read
    // by the GPU while the presenting thread rewrites it.
    std::string overlay_text_;
    bool overlay_visible_ = false;
    uint32_t overlay_width_ = 0, overlay_height_ = 0, overlay_x_ = 0, overlay_y_ = 0;
    VkBuffer overlay_ = VK_NULL_HANDLE;
    VkDeviceMemory overlay_mem_ = VK_NULL_HANDLE;
    VkDeviceSize overlay_size_ = 0;
    uint32_t overlay_type_ = 0;
    std::vector<uint32_t> overlay_pixels_;   // the texels that were uploaded
    VkBuffer overlay_check_ = VK_NULL_HANDLE;
    VkDeviceMemory overlay_check_mem_ = VK_NULL_HANDLE;
    VkDeviceSize overlay_check_size_ = 0;
    bool overlay_check_pending_ = false;     // new text: read the rectangle back with the next present
    bool overlay_check_recorded_ = false;    // the command buffer being submitted holds a readback
    bool overlay_check_reported_ = false;    // the first result has been written to stderr
    bool luma_probe_failed_ = false;         // the probe buffer could not be created
    bool luma_recorded_ = false;             // the command buffer being submitted holds the probe copy
    VkFormat luma_format_ = VK_FORMAT_UNDEFINED;
    VkBuffer luma_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory luma_mem_ = VK_NULL_HANDLE;
    VkDeviceSize luma_size_ = 0;
    uint64_t luma_presents_ = 0, luma_black_ = 0, luma_dark_ = 0, luma_logged_ = 0;
    double luma_last_logged_mean_ = -1.0;
    uint64_t luma_first_present_ns_ = 0;
};

}  // namespace rcomp::xenos
