// R-comp game shell (owner: PRIME, app/): everything a recompiled title needs
// around its generated code, in one place:
//   guest memory -> runtime + kernel/XAM/video HLE + file mounts -> XEX image
//   -> function table -> Xenos GPU (rexglue command processor + Vulkan backend)
//   -> screen output (VulkanDisplay, refreshed at the display rate)
//   -> entry point on a guest thread.
// The title's own main() provides the XEX bytes and the function table of its
// generated code (PPCFuncMappings), links an input backend
// (include/rcomp/input.h) and the Vulkan driver (host loader / PS5_Vulkan).
#pragma once

#include <stdint.h>
#include <stdio.h>

#include <memory>
#include <string>
#include <vector>

#include "rcomp/func_table.h"
#include "rcomp/runtime/xam.h"
#include "rcomp/runtime/xconfig.h"

namespace rcomp::xenos {
class VulkanOutput;
class VulkanDisplay;
}  // namespace rcomp::xenos

namespace rcomp::app {

struct TitleConfig {
    std::vector<uint8_t> xex;           // the (decrypted, decompressed) XEX file
    std::vector<FuncEntry> functions;   // generated code: guest address -> host function
    uint32_t display_width = 1280;      // what the title is told (VdQueryVideoMode)
    uint32_t display_height = 720;
    uint32_t refresh_hz = 60;           // vblank interrupts and screen refresh
    uint32_t draw_resolution_scale_x = 1, draw_resolution_scale_y = 1;
    bool screen = true;                 // put frames on a display (else headless render only)
    bool direct_guest_presentation = false;  // consume the Vulkan image without CPU readback
    // Frame-rate counter, once per second: an "RCOMP-FPS" line on stderr and, when
    // a screen exists, the rate in the top-left corner (VulkanDisplay::SetOverlayText).
    // Counts guest swaps (VdSwap), the same counter as the "RCOMP-VD swap" lines.
    // On by default: every game run through the tool shows its rate; an entry point opts out explicitly.
    bool fps_counter = true;
    // Launch options menu (rcomp/app/launch_menu.h): with a screen, the player picks the internal resolution
    // before the GPU starts; draw_resolution_scale_x/y are the defaults and are replaced by the choice. The
    // choice is kept in launch_options_path (empty: not remembered). launch_menu_countdown seconds without
    // input start the game with the shown choice (0: wait for the player).
    bool launch_menu = false;
    std::string launch_menu_title;
    std::string launch_options_path;
    uint32_t launch_menu_countdown = 15;
    // Console diagnostic: run the save-service self-test (rcomp/app/content_selftest.h) before the guest starts.
    bool content_selftest = false;
    // Console layout of the physical windows (rcomp::rt::RuntimeConfig::physical_4k_window_offset): must match the
    // generated code's RCOMP_PHYSICAL_4K_WINDOW_OFFSET (app/m6 option RCOMP_M6_PHYSICAL_4K_WINDOW_OFFSET sets both).
    bool physical_4k_window_offset = false;
    uint32_t main_stack_size = 0x100000;
    FILE* log = stderr;
    // Empty for synthetic fixtures; otherwise an existing read-only extracted
    // game directory, mounted as both game: and d: before guest execution.
    std::string game_root;
    // Empty: no HDD cache partitions. Otherwise a writable directory under which the cache: and cache1: devices are mounted (cache/ and cache1/ are created
    // when missing): the Episodes from Liberty City executable checks cache:\valid.txt and cache1:\valid.txt before it builds its device registry.
    std::string cache_root;
    // Mount the cache devices read-only (their valid.txt markers are created at start-up). The Episodes from Liberty City executable installs several GB of its IMG
    // archives into cache:/cache1: in the background, and on the console that install, whatever its sync policy, drags the title from 45 fps (cache read-only) to 24 fps (its write
    // stalls everything that touches memory at random points, the command processor included); without a writable cache it streams from the disc.
    bool cache_read_only = false;
    // Empty: no storage device (content and profile-setting calls answer as a console without one). Otherwise a
    // writable directory that backs the hard drive the title saves to (rcomp/runtime/xam_content.h); created
    // when missing. On the PS5 it is inside the title's own app folder, which R-comp Installer never deletes.
    std::string save_root;
    // Empty: no hard drive (opening "\Device\Harddisk0\..." fails as before). Otherwise a writable directory
    // that backs the Xbox 360 hard drive: raw Partition0/Cache0/Cache1 and the utility-partition file systems
    // (rcomp/runtime/hdd.h, runtime/docs/HDD.md); created when missing.
    std::string hdd_root;
    // Guest-visible identity, independent of the host data mount. The command
    // line is supplied verbatim (empty means no bytes before the terminator);
    // the bootstrap never invents arguments, quoting or a host filesystem path.
    std::string guest_path = "game:\\default.xex";
    std::string command_line;
    // Explicit guest profile. Fixtures which do not call configuration getters
    // may leave this disabled; real titles supply and record their profile.
    bool configure_xam = false;
    rt::XamRuntimeConfig xam_profile;
    // Kernel values are a title compatibility profile, read from this XEX's
    // versioned import libraries. They never describe the host PS5 firmware.
    bool configure_kernel_variables = false;
    bool configure_xconfig = false;
    rt::XConfigProfile xconfig_profile;
    // Production PS5 entry points must opt in. Hosts retain the explicit
    // headless test path; a required display never silently falls back to it.
    bool require_display_plane = false;
};

class Title {
public:
    // Sets everything up; nullptr and *error on the first failure.
    static std::unique_ptr<Title> Create(TitleConfig cfg, std::string* error);
    ~Title();

    // Runs the XEX entry point with r3 = arg on a guest thread of the calling
    // host thread; returns false (and *error) if it could not be started.
    bool RunEntry(uint32_t arg, uint32_t* exit_code, std::string* error);

    uint32_t entry_point() const { return entry_; }
    xenos::VulkanOutput& output() { return *output_; }
    // Last presented guest frame as R8G8B8X8 rows (see VulkanOutput::Capture); false before the first swap.
    bool CaptureFrame(std::vector<uint8_t>* rgbx, uint32_t* width, uint32_t* height) const;
    // nullptr when running without a screen (or none could be created).
    xenos::VulkanDisplay* display() { return display_.get(); }
    // Display refreshes submitted, NOT distinct guest frames or proof of boot.
    uint64_t frames_shown() const;
    // Vblank ticks issued by the Xenos bridge, independent of guest swaps.
    uint64_t vblanks_issued() const;
    std::string screen_status() const { return screen_status_; }

private:
    Title() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<xenos::VulkanOutput> output_;
    std::unique_ptr<xenos::VulkanDisplay> display_;
    uint32_t entry_ = 0;
    std::string screen_status_;
};

}  // namespace rcomp::app
