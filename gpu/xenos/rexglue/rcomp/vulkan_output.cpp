// R-comp: Vulkan rendering backend of the Xenos GPU (owner: gpu/xenos).
#include "vulkan_output.h"

#include <string.h>
#include <cstdio>

#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

#include <stdlib.h>

#include <rex/cvar.h>

#include "rcomp/diag.h"

namespace rcomp::xenos {

namespace {
// Host GPU loss during presentation cannot be recovered from in a title.
void host_gpu_lost(bool is_responsible, bool) {
    rcomp_fatal(RCOMP_FATAL_PLATFORM, "xenos: host GPU lost (presenter%s)", is_responsible ? ", responsible" : "");
}
}  // namespace

bool VulkanOutput::ConfigureDrawResolutionScale(uint32_t scale_x, uint32_t scale_y, std::string* error) {
    constexpr uint32_t max_scale = rex::graphics::TextureCache::kMaxDrawResolutionScaleAlongAxis;
    if (!scale_x || !scale_y || scale_x > max_scale || scale_y > max_scale) {
        if (error) *error = "draw resolution scale must be within 1.." + std::to_string(max_scale);
        return false;
    }
    if (!rex::cvar::SetFlagByName("resolution_scale", "1") ||
        !rex::cvar::SetFlagByName("draw_resolution_scale_x", std::to_string(scale_x)) ||
        !rex::cvar::SetFlagByName("draw_resolution_scale_y", std::to_string(scale_y))) {
        if (error) *error = "cannot configure draw resolution scale";
        return false;
    }
    uint32_t effective_x = 0, effective_y = 0;
    if (!rex::graphics::TextureCache::GetConfigDrawResolutionScale(effective_x, effective_y) ||
        effective_x != scale_x || effective_y != scale_y) {
        if (error) *error = "draw resolution scale was clamped or overridden";
        return false;
    }
    std::fprintf(stderr, "RCOMP-RESOLUTION requested=%ux%u effective=%ux%u source=title_config\n",
                 scale_x, scale_y, effective_x, effective_y);
    return true;
}

bool VulkanOutput::SetDrawResolutionScale(uint32_t scale_x, uint32_t scale_y, std::string* error) {
    if (!ConfigureDrawResolutionScale(scale_x, scale_y, error)) return false;
    draw_resolution_scale_x_ = scale_x;
    draw_resolution_scale_y_ = scale_y;
    return true;
}

std::unique_ptr<VulkanOutput> VulkanOutput::Create(bool with_display, std::string* error,
                                                   uint32_t scale_x, uint32_t scale_y) {
    if (!ConfigureDrawResolutionScale(scale_x, scale_y, error)) return nullptr;
    const uint32_t effective_x = scale_x, effective_y = scale_y;
    // Async placeholders currently discard draws. No environment override may
    // select that path while R-comp requires every submitted draw to execute.
    if (!rex::cvar::SetFlagByName("async_shader_compilation", "false")) {
        if (error) *error = "cannot disable async_shader_compilation";
        return nullptr;
    }
    // PS5_Vulkan has neither geometryShader nor fillModeNonSolid; the backend
    // has fallbacks for both (primitive expansion in the vertex shader, solid
    // fill for line/point polygon modes), so they are not required.
    rex::cvar::SetFlagByName("vulkan_require_geometry_shader", "false");
    rex::cvar::SetFlagByName("vulkan_require_fill_mode_non_solid", "false");
    // Sample with the guest's own anisotropy (fetch constants) rather than a
    // forced host enhancement: exact Xenos filtering, less texture traffic at
    // scaled resolutions.
    if (!rex::cvar::SetFlagByName("anisotropic_override", "-1")) {
        if (error) *error = "cannot select guest anisotropic filtering";
        return nullptr;
    }
    // One queue submission per guest frame (at the swap) instead of one more
    // per primary ring-buffer segment. Each PS5 submission is followed by an
    // AGC suspend point that throttles on GPU progress (gpu/vulkan
    // driver-reports); guest fences are written by the command processor and
    // do not depend on host submission timing.
    if (!rex::cvar::SetFlagByName("vulkan_submit_on_primary_buffer_end", "false")) {
        if (error) *error = "cannot configure Vulkan submission points";
        return nullptr;
    }
#if RCOMP_XENOS_FSI
    // EDRAM emulated in a storage buffer with fragment shader interlock: no
    // host render target ownership transfers.
    if (!rex::cvar::SetFlagByName("render_target_path_vulkan", "fsi")) {
        if (error) *error = "cannot select the FSI render target path";
        return nullptr;
    }
#endif
    std::unique_ptr<VulkanOutput> out(new VulkanOutput());
    out->draw_resolution_scale_x_ = effective_x;
    out->draw_resolution_scale_y_ = effective_y;
    auto provider = rex::ui::vulkan::VulkanProvider::Create(/*with_gpu_emulation=*/true, with_display);
    if (!provider) {
        if (error) *error = "no Vulkan device usable for GPU emulation (see the log for the missing feature)";
        return nullptr;
    }
    out->presenter_ = provider->CreatePresenter(host_gpu_lost);
    if (!out->presenter_) {
        if (error) *error = "Vulkan presenter creation failed";
        return nullptr;
    }
    out->provider_ = std::move(provider);
    return out;
}

VulkanOutput::~VulkanOutput() {
    presenter_.reset();
    provider_.reset();
}

void VulkanOutput::Attach(HostConfig& cfg) const {
    cfg.provider = provider_.get();
    cfg.presenter = presenter_.get();
    std::fprintf(stderr, "RCOMP-RESOLUTION source_display=%ux%u draw_scale=%ux%u\n",
                 cfg.display_width, cfg.display_height,
                 draw_resolution_scale_x_, draw_resolution_scale_y_);
}

BackendFactory VulkanOutput::backend_factory() const {
    return [](rex::graphics::GpuHost* host) -> std::unique_ptr<rex::graphics::CommandProcessor> {
        return std::make_unique<rex::graphics::vulkan::VulkanCommandProcessor>(host);
    };
}

bool VulkanOutput::Capture(std::vector<uint8_t>* rgbx, uint32_t* width, uint32_t* height) const {
    rex::ui::RawImage image;
    if (!presenter_ || !presenter_->CaptureGuestOutput(image)) return false;
    rgbx->resize(size_t(image.width) * image.height * 4);
    for (uint32_t y = 0; y < image.height; ++y)
        memcpy(rgbx->data() + size_t(y) * image.width * 4, image.data.data() + y * image.stride, size_t(image.width) * 4);
    *width = image.width;
    *height = image.height;
    return true;
}

bool VulkanOutput::PresentGuestFrame(VulkanDisplay& display, std::string* error) const {
    std::string local_error;
    if (!error) error = &local_error;
    error->clear();
    if (!presenter_) return false;
    auto* presenter = static_cast<rex::ui::vulkan::VulkanPresenter*>(presenter_.get());
    return presenter->VisitGuestOutputImage(
        [&](const rex::ui::vulkan::VulkanDevice* device, VkImage image, VkExtent2D extent,
            VkFormat format, VkImageLayout layout, uint64_t refresh_serial) {
            // ConsumeGuestOutput still pins the source for this entire callback.
            // The cursor is per display, so a recreated/second display paints once.
            return display.guest_output_presentation_cursor_.PresentIfChanged(
                presentation_epoch_, refresh_serial, [&] {
                    return display.PresentGuestImage(device, image, extent, format, layout, error);
                });
        });
}

std::unique_ptr<VulkanDisplay> VulkanOutput::CreateDisplay(uint32_t guest_width, uint32_t guest_height,
                                                           std::string* error) const {
    auto* p = static_cast<rex::ui::vulkan::VulkanProvider*>(provider_.get());
    if (!p) {
        if (error) *error = "no Vulkan provider";
        return nullptr;
    }
    std::string e;
    auto d = VulkanDisplay::Create(*p, guest_width, guest_height, &e);
    if (!d && error) *error = e;
    return d;
}

std::string VulkanOutput::device_name() const {
    auto* p = static_cast<rex::ui::vulkan::VulkanProvider*>(provider_.get());
    return p && p->vulkan_device() ? std::string(p->vulkan_device()->properties().deviceName) : std::string();
}

}  // namespace rcomp::xenos
