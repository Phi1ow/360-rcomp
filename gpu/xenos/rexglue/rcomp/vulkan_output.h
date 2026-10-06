// R-comp: the Vulkan rendering backend of the Xenos GPU (rexglue's
// VulkanCommandProcessor, patch 0009) and where its frames go
// (owner: gpu/xenos). Vulkan is linked into the binary: the system loader on
// the host (lavapipe in tests), PS5_Vulkan in a PS5 title.
//
// Frames: VdSwap -> XE_SWAP -> the command processor refreshes the
// presenter's guest output image. Without a surface the presenter only keeps
// that image (host tests read it back with Capture); with one it also shows
// it.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "guest_output_presentation_cursor.h"
#include "vulkan_display.h"
#include "xenos_host.h"

namespace rex::ui {
class GraphicsProvider;
class Presenter;
}  // namespace rex::ui

namespace rcomp::xenos {

class VulkanOutput {
public:
    // nullptr and *error set if Vulkan (instance, device with the features
    // the backend needs, presenter) is not available. with_display: the
    // instance must support a presentation surface (a title); without it the
    // output is headless (host tests capture it).
    // Scales internal Xenos rendering; display-mode registers retain guest size.
    // Explicit title configuration overrides resolution cvars in the environment.
    static std::unique_ptr<VulkanOutput> Create(bool with_display, std::string* error,
                                               uint32_t scale_x = 1, uint32_t scale_y = 1);
    ~VulkanOutput();
    // Changes the draw resolution scale chosen at Create (the launch options menu). Valid only before
    // gpu_start: the command processor reads the scale when it starts.
    bool SetDrawResolutionScale(uint32_t scale_x, uint32_t scale_y, std::string* error);

    // Fills the provider/presenter fields of a HostConfig for gpu_start.
    void Attach(HostConfig& cfg) const;
    // Command processor factory for gpu_start.
    BackendFactory backend_factory() const;

    // Last presented guest frame, R8G8B8X8 rows (stride = 4 * width).
    bool Capture(std::vector<uint8_t>* rgbx, uint32_t* width, uint32_t* height) const;
    // false with an empty error means no new guest output needs presentation.
    // Repeated polls of one published output perform no blit, submit or WSI.
    // false with an error means that presentation failed.
    bool PresentGuestFrame(VulkanDisplay& display, std::string* error) const;
    std::string device_name() const;
    // Screen output for the presented frames (Create(with_display = true)).
    std::unique_ptr<VulkanDisplay> CreateDisplay(uint32_t guest_width, uint32_t guest_height, std::string* error) const;

private:
    VulkanOutput() = default;
    static bool ConfigureDrawResolutionScale(uint32_t scale_x, uint32_t scale_y, std::string* error);
    uint32_t draw_resolution_scale_x_ = 1, draw_resolution_scale_y_ = 1;
    // Unique CPU identity for the immutable presenter_ lifetime, not a VkImage.
    std::shared_ptr<const GuestOutputPresentationEpoch> presentation_epoch_ =
        std::make_shared<GuestOutputPresentationEpoch>();
    std::unique_ptr<rex::ui::GraphicsProvider> provider_;
    std::unique_ptr<rex::ui::Presenter> presenter_;
};

}  // namespace rcomp::xenos
