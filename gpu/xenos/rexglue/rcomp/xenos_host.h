// R-comp host of rexglue's Xenos command processor (owner: gpu/xenos).
// Implements rex::graphics::GpuHost (patch 0003) over R-comp's guest memory,
// runs the register-window bridge and the vblank timer, and serves the
// runtime-facing interface include/rcomp/xenos_gpu.h.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/gpu_host.h>
#include <rex/graphics/register_file.h>
#include <rex/system/xmemory.h>
#include <rcomp_xenos/host_thread.h>

#include "rcomp/guest_memory.h"
#include "rcomp/xenos_gpu.h"

namespace rcomp::xenos {

struct HostConfig {
    uint32_t display_width = 1280;   // what the display registers report
    uint32_t display_height = 720;
    uint32_t vblank_hz = 60;         // 0: no vblank timer (tests)
    float refresh_hz = 60.0f;        // reported refresh rate (VdQueryVideoMode)
    // Rendering output (not owned; VulkanOutput::Attach): nullptr for
    // backends that render nowhere (tests).
    rex::ui::GraphicsProvider* provider = nullptr;
    rex::ui::Presenter* presenter = nullptr;
    uint32_t bridge_poll_us = 100;   // register-window poll interval
};

// Builds the rendering backend (a CommandProcessor subclass) for the host.
using BackendFactory = std::function<std::unique_ptr<rex::graphics::CommandProcessor>(rex::graphics::GpuHost*)>;

class XenosHost final : public rex::graphics::GpuHost {
public:
    // `guest_base`: GuestMemory::base(). The register window must be committed
    // (gpu_start does it through GuestMemory::reserve_runtime_range).
    XenosHost(uint8_t* guest_base, const HostConfig& cfg, InterruptDispatcher dispatcher);
    ~XenosHost() override;

    bool Start(const BackendFactory& backend);
    void Stop();

    rex::memory::Memory* memory() const override { return memory_.get(); }
    rex::graphics::RegisterFile* register_file() override { return &register_file_; }
    void DispatchInterruptCallback(uint32_t source, uint32_t cpu) override;
    rex::ui::GraphicsProvider* provider() const override { return cfg_.provider; }
    rex::ui::Presenter* presenter() const override { return cfg_.presenter; }
    void GetDisplayMode(uint32_t& width, uint32_t& height) const override {
        width = cfg_.display_width;
        height = cfg_.display_height;
    }
    // A lost host GPU cannot be recovered in a title: fatal.
    void OnHostGpuLossFromAnyThread(bool is_responsible) override;

    rex::graphics::CommandProcessor* command_processor() { return cp_.get(); }
    void SetInterruptCallback(uint32_t callback, uint32_t user_data);
    uint32_t vblank_count() const { return vblanks_.load(); }
    const HostConfig& config() const { return cfg_; }

private:
    void BridgeMain(uint32_t initial_wptr);
    void PublishReadRegisters();

    uint8_t* guest_base_;
    HostConfig cfg_;
    InterruptDispatcher dispatcher_;
    std::unique_ptr<rex::memory::Memory> memory_;
    rex::graphics::RegisterFile register_file_;
    std::unique_ptr<rex::graphics::CommandProcessor> cp_;
    std::atomic<uint32_t> interrupt_callback_{0}, interrupt_user_data_{0};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> vblanks_{0};
    rcomp::xenos::HostThread bridge_;
};

// Process-wide instance used by the runtime's video exports. Commits the
// register window as a runtime range of `mem`.
bool gpu_start(rcomp::GuestMemory& mem, const HostConfig& cfg, InterruptDispatcher dispatcher,
               const BackendFactory& backend);
void gpu_stop();
XenosHost* gpu_host();

}  // namespace rcomp::xenos
