// Real xam.xex HLE implementations (owner: Agent 3, runtime/). Same rules as
// hle_xboxkrnl.cpp: documented semantics, implemented subset, anything else
// is fatal or a documented error code, never a fake success.
//
// Controller state comes from the linked input backend (include/rcomp/input.h).
// Guest structures (big-endian):
//   XINPUT_GAMEPAD (12)      +0 u16 buttons, +2 u8 lt, +3 u8 rt, +4 s16 lx,
//                            +6 ly, +8 rx, +10 ry
//   XINPUT_STATE (16)        +0 u32 packet number, +4 XINPUT_GAMEPAD
//   XINPUT_CAPABILITIES (20) +0 u8 type, +1 u8 subtype, +2 u16 flags,
//                            +4 XINPUT_GAMEPAD (supported controls),
//                            +16 u16 left motor, +18 u16 right motor
//   XINPUT_VIBRATION (4)     +0 u16 left motor, +2 u16 right motor
#include <cmath>
#include "diagnostics.h"
#include <cstring>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <new>

#include "module_state.h"
#include "hle_more.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/input.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam.h"
#include "rcomp/runtime/xam_net.h"
#include "rcomp/runtime/notifications.h"
#include "rcomp/runtime/xam_enum.h"
#include "rcomp/runtime/xam_content.h"
#include "rcomp/runtime/xam_profile.h"
#include "rcomp/runtime/xam_msg.h"
#include "rcomp/runtime/xam_misc.h"
#include "rcomp/runtime/xam_live_dialogs.h"
#include "rcomp/runtime/process_lifecycle.h"
#include "xam_state.h"

namespace rcomp::rt {

namespace {

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorNotSupported = 0x32;
constexpr uint32_t kErrorBadArguments = 0xA0;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr uint32_t kHresultInvalidArgument = 0x80070057u;
constexpr uint32_t kHresultOutOfMemory = 0x8007000Eu;
constexpr uint32_t kUserIndexAny = 0xFF;
constexpr uint32_t kMaxUsers = 4;
constexpr uint32_t kObservedXamAllocFlags = 0x18000000u;

// User 0xFF (XUSER_INDEX_ANY) reads controller 0. Returns false for 4..0xFE.
bool user_slot(uint32_t user, uint32_t* slot) {
    if (user == kUserIndexAny) user = 0;
    if (user >= kMaxUsers) return false;
    *slot = user;
    return true;
}

bool write_gamepad(uint32_t addr, const rcomp_pad& p) {
    return guest_write_be32(addr, (uint32_t(p.buttons) << 16) | (uint32_t(p.left_trigger) << 8) | p.right_trigger) &&
           guest_write_be32(addr + 4, (uint32_t(uint16_t(p.thumb_lx)) << 16) | uint16_t(p.thumb_ly)) &&
           guest_write_be32(addr + 8, (uint32_t(uint16_t(p.thumb_rx)) << 16) | uint16_t(p.thumb_ry));
}

bool same(const rcomp_pad& a, const rcomp_pad& b) {
    return a.buttons == b.buttons && a.left_trigger == b.left_trigger && a.right_trigger == b.right_trigger &&
           a.thumb_lx == b.thumb_lx && a.thumb_ly == b.thumb_ly && a.thumb_rx == b.thumb_rx &&
           a.thumb_ry == b.thumb_ry;
}

[[noreturn]] void bad_buffer(const char* fn, PPCContext& ctx, uint32_t p, const char* access) {
    rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xam.xex!%s %s buffer 0x%08X not accessible lr=0x%08X", fn, access, p,
                (uint32_t)ctx.lr);
}

Runtime& owner(const char* fn) {
    Runtime* r = runtime();
    if (!r || !r->xam)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s called without initialized XAM runtime state", fn);
    return *r;
}

XamRuntimeConfig configured(const char* fn) {
    Runtime& r = owner(fn);
    bool ready = false;
    XamRuntimeConfig result{};
    {
        std::lock_guard<std::mutex> lock(r.xam->mutex);
        ready = r.xam->configured;
        if (ready) result = r.xam->config;
    }
    if (!ready)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xam.xex!%s requires explicit runtime language/region/AV/video configuration", fn);
    return result;
}


void XamAlloc(PPCContext& ctx, uint8_t*) {
    Runtime& r = owner("XamAlloc");
    const uint32_t flags = ctx.r3.u32, size = ctx.r4.u32, output = ctx.r5.u32;
    if (!output || !r.mem->is_accessible(output, 4, Protect::ReadWrite))
        bad_buffer("XamAlloc", ctx, output, "output");
    if (!guest_write_be32(output, 0)) bad_buffer("XamAlloc", ctx, output, "output");
    if (!size) {
        ctx.r3.u64 = kHresultInvalidArgument;
        return;
    }
    // GTA IV uses 0 and 0x18000000. No distinct heap/protection semantics for
    // the latter have been established, so both select the ordinary non-RWX
    // guest heap; any other class stays explicit instead of being guessed.
    if (flags != 0 && flags != kObservedXamAllocFlags)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!XamAlloc flags=0x%08X contract not established", flags);

    // Provision ownership metadata before changing guest state. std::set node
    // insertion below then cannot allocate after GuestHeap has succeeded.
    std::set<uint32_t> staged;
    std::set<uint32_t>::node_type ownership;
#if defined(__cpp_exceptions)
    try {
#endif
        const auto it = staged.insert(1u).first;
        ownership = staged.extract(it);
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        ctx.r3.u64 = kHresultOutOfMemory;
        return;
    }
#endif

    uint32_t address = 0;
    Status s = r.heap.alloc(size, 16, false, &address);
    if (s == Status::OutOfMemory) {
        ctx.r3.u64 = kHresultOutOfMemory;
        return;
    }
    if (s != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!XamAlloc heap allocation failed: %s", status_name(s));

    ownership.value() = address;
    bool inserted = false;
    Status duplicate_rollback = Status::Ok;
    {
        std::lock_guard<std::mutex> lock(r.xam->mutex);
        auto result = r.xam->allocations.insert(std::move(ownership));
        inserted = result.inserted;
        if (!inserted) duplicate_rollback = r.heap.free(address);
    }
    if (!inserted) {
        if (duplicate_rollback != Status::Ok)
            rcomp_fatal(RCOMP_FATAL_INTERNAL,
                        "xam.xex!XamAlloc duplicate ownership and rollback failed for 0x%08X: %s", address,
                        status_name(duplicate_rollback));
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!XamAlloc duplicate guest allocation 0x%08X", address);
    }
    if (!guest_write_be32(output, address)) {
        Status rollback = Status::Ok;
        {
            std::lock_guard<std::mutex> lock(r.xam->mutex);
            rollback = r.heap.free(address);
            if (rollback == Status::Ok) r.xam->allocations.erase(address);
        }
        if (rollback != Status::Ok)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!XamAlloc failed to roll back lost output buffer: %s",
                        status_name(rollback));
        bad_buffer("XamAlloc", ctx, output, "output");
    }
    ctx.r3.u64 = kErrorSuccess;
}

void XamFree(PPCContext& ctx, uint8_t*) {
    Runtime& r = owner("XamFree");
    const uint32_t address = ctx.r3.u32;
    if (!address)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xam.xex!XamFree null allocation lr=0x%08X", (uint32_t)ctx.lr);
    bool owned = false;
    Status s = Status::Ok;
    {
        std::lock_guard<std::mutex> lock(r.xam->mutex);
        owned = r.xam->allocations.count(address) != 0;
        if (owned) {
            s = r.heap.free(address);
            if (s == Status::Ok) r.xam->allocations.erase(address);
        }
    }
    if (!owned)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,
                    "xam.xex!XamFree address 0x%08X is not owned by this XAM runtime lifetime", address);
    if (s != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!XamFree heap free failed for 0x%08X: %s", address, status_name(s));
}

void XamGetExecutionId(PPCContext& ctx, uint8_t*) {
    Runtime& r = owner("XamGetExecutionId");
    const uint32_t output = ctx.r3.u32;
    if (!output || !r.mem->is_accessible(output, 4, Protect::ReadWrite))
        bad_buffer("XamGetExecutionId", ctx, output, "output");
    const uint32_t address = xam_execution_info_pointer("XamGetExecutionId");
    if (!guest_write_be32(output, address)) bad_buffer("XamGetExecutionId", ctx, output, "output");
    ctx.r3.u64 = kErrorSuccess;
}

void XGetAVPack(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = configured("XGetAVPack").av_pack; }
void XGetGameRegion(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = configured("XGetGameRegion").game_region; }
void XGetLanguage(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = configured("XGetLanguage").language; }

void XGetVideoMode(PPCContext& ctx, uint8_t*) {
    Runtime& r = owner("XGetVideoMode");
    const uint32_t output = ctx.r3.u32;
    if (!output || !r.mem->is_accessible(output, 24, Protect::ReadWrite))
        bad_buffer("XGetVideoMode", ctx, output, "output");
    const auto mode = configured("XGetVideoMode").video;
    uint32_t refresh = 0;
    static_assert(sizeof(refresh) == sizeof(mode.refresh_rate_hz));
    std::memcpy(&refresh, &mode.refresh_rate_hz, sizeof(refresh));
    if (!guest_write_be32(output + 0, mode.display_width) ||
        !guest_write_be32(output + 4, mode.display_height) ||
        !guest_write_be32(output + 8, mode.interlaced ? 1u : 0u) ||
        !guest_write_be32(output + 12, mode.widescreen ? 1u : 0u) ||
        !guest_write_be32(output + 16, mode.high_definition ? 1u : 0u) ||
        !guest_write_be32(output + 20, refresh))
        bad_buffer("XGetVideoMode", ctx, output, "output");
}

// XamInputGetState (0x0191): (DWORD UserIndex, DWORD Flags, PXINPUT_STATE).
// ERROR_SUCCESS with the state, ERROR_DEVICE_NOT_CONNECTED, or
// ERROR_BAD_ARGUMENTS for a user index above 3 (0xFF = any user reads
// controller 0). The packet number changes whenever the state changes.
// Flags only select device classes on the real system; every controller
// here is a gamepad, so they are not interpreted.
void XamInputGetState(PPCContext& ctx, uint8_t*) {
    Runtime& r = owner("XamInputGetState");
#if RCOMP_RUNTIME_DIAGNOSTICS
    {   // bring-up tracing: how the title polls
        static std::atomic<uint32_t> calls{0};
        const uint32_t n = calls.fetch_add(1);
        if (n < 6 || (n % 600) == 0)
            fprintf(stderr, "RCOMP-XAM GetState call#%u user=0x%X flags=0x%X lr=0x%08X\n", n, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr);
    }
#endif
    uint32_t slot;
    if (!user_slot(ctx.r3.u32, &slot)) {
        ctx.r3.u64 = kErrorBadArguments;
        return;
    }
    const uint32_t out = ctx.r5.u32;
    if (!out || !r.mem->is_accessible(out, 16, Protect::ReadWrite))
        bad_buffer("XamInputGetState", ctx, out, "output");
    rcomp_pad p{};
    if (rcomp_input_read(slot, &p) != RCOMP_INPUT_OK) {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    uint32_t packet;
    {
        std::lock_guard<std::mutex> lk(r.xam->mutex);
        if (!same(p, r.xam->last_pad[slot])) {
            r.xam->last_pad[slot] = p;
            ++r.xam->packet[slot];
        }
        packet = r.xam->packet[slot];
    }
    if (!guest_write_be32(out, packet) || !write_gamepad(out + 4, p)) bad_buffer("XamInputGetState", ctx, out, "output");
#if RCOMP_RUNTIME_DIAGNOSTICS
    {   // bring-up tracing: first reports of each distinct non-empty button state
        static std::atomic<uint32_t> last_shown{0};
        static std::atomic<int> shown{0};
        if (last_shown.exchange(p.buttons) != p.buttons && shown.fetch_add(1) < 80)
            fprintf(stderr, "RCOMP-XAM GetState user=%u buttons=0x%04X packet=%u lr=0x%08X\n", ctx.r3.u32, p.buttons, packet, (uint32_t)ctx.lr);
    }
#endif
    ctx.r3.u64 = kErrorSuccess;
}

// XamInputGetCapabilities (0x0190): (DWORD UserIndex, DWORD Flags,
// PXINPUT_CAPABILITIES). Reports a gamepad (type 1, subtype 1) with every
// button this runtime can produce, 8-bit triggers, 8-bit stick resolution
// (0xFF00) and no vibration (motors 0), since no backend drives the motors yet.
void XamInputGetCapabilities(PPCContext& ctx, uint8_t*) {
    Runtime& r = owner("XamInputGetCapabilities");
#if RCOMP_RUNTIME_DIAGNOSTICS
    {
        static std::atomic<uint32_t> calls{0};
        const uint32_t n = calls.fetch_add(1);
        if (n < 4 || (n % 600) == 0)
            fprintf(stderr, "RCOMP-XAM GetCapabilities call#%u user=0x%X flags=0x%X lr=0x%08X\n", n, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr);
    }
#endif
    uint32_t slot;
    if (!user_slot(ctx.r3.u32, &slot)) {
        ctx.r3.u64 = kErrorBadArguments;
        return;
    }
    const uint32_t out = ctx.r5.u32;
    if (!out || !r.mem->is_accessible(out, 20, Protect::ReadWrite))
        bad_buffer("XamInputGetCapabilities", ctx, out, "output");
    rcomp_pad probe{};
    if (rcomp_input_read(slot, &probe) != RCOMP_INPUT_OK) {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    rcomp_pad caps{};
    caps.buttons = 0xF3FF;  // everything but the reserved 0x0400/0x0800 bits
    caps.left_trigger = caps.right_trigger = 0xFF;
    caps.thumb_lx = caps.thumb_ly = caps.thumb_rx = caps.thumb_ry = int16_t(0xFF00);
    if (!guest_write_be32(out, 0x01010000u) || !write_gamepad(out + 4, caps) || !guest_write_be32(out + 16, 0))
        bad_buffer("XamInputGetCapabilities", ctx, out, "output");
    ctx.r3.u64 = kErrorSuccess;
}

// XamInputSetState (0x0192): (DWORD UserIndex, DWORD Unused,
// PXINPUT_VIBRATION). ERROR_SUCCESS only when the backend drove the motors;
// ERROR_NOT_SUPPORTED when it cannot (reported, not pretended),
// ERROR_DEVICE_NOT_CONNECTED, ERROR_BAD_ARGUMENTS.
void XamInputSetState(PPCContext& ctx, uint8_t*) {
    Runtime& r = owner("XamInputSetState");
    uint32_t slot, v = 0;
    if (!user_slot(ctx.r3.u32, &slot)) {
        ctx.r3.u64 = kErrorBadArguments;
        return;
    }
    if (!ctx.r5.u32 || !r.mem->is_accessible(ctx.r5.u32, 4, Protect::Read) || !guest_read_be32(ctx.r5.u32, &v))
        bad_buffer("XamInputSetState", ctx, ctx.r5.u32, "input");
    switch (rcomp_input_set_vibration(slot, uint16_t(v >> 16), uint16_t(v))) {
    case RCOMP_INPUT_OK: ctx.r3.u64 = kErrorSuccess; break;
    case RCOMP_INPUT_NOT_CONNECTED: ctx.r3.u64 = kErrorDeviceNotConnected; break;
    default: ctx.r3.u64 = kErrorNotSupported; break;
    }
}

// XamInputGetKeystrokeEx (0x0198): (PDWORD UserIndex in/out, DWORD Flags,
// PXINPUT_KEYSTROKE). Flags bit 0 selects gamepad keystrokes, bit 1 keyboard
// keystrokes, 0x40000000 is the any-user modifier the title passes. R-comp has
// no keyboard, so a keyboard-only request reports ERROR_DEVICE_NOT_CONNECTED
// (the actual GTA IV call uses 0x40000002). Gamepad keystroke generation
// (button transitions with repeat) is not implemented and stops explicitly.
void XamInputGetKeystrokeEx(PPCContext& ctx, uint8_t*) {
    Runtime& r = owner("XamInputGetKeystrokeEx");
    const uint32_t user_ptr = ctx.r3.u32, flags = ctx.r4.u32, out = ctx.r5.u32;
    uint32_t user = 0;
    if (!user_ptr || !r.mem->is_accessible(user_ptr, 4, Protect::ReadWrite) || !guest_read_be32(user_ptr, &user))
        bad_buffer("XamInputGetKeystrokeEx", ctx, user_ptr, "user index");
    if (!out || !r.mem->is_accessible(out, 8, Protect::ReadWrite))
        bad_buffer("XamInputGetKeystrokeEx", ctx, out, "keystroke");
    if (user >= 4 && user != 0xFF) { ctx.r3.u64 = kErrorBadArguments; return; }
    if ((flags & ~0x40000003u) || (flags & 1))
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!XamInputGetKeystrokeEx flags=0x%08X lr=0x%08X", flags,
                    (uint32_t)ctx.lr);
    ctx.r3.u64 = kErrorDeviceNotConnected;
}

struct Impl {
    uint32_t ordinal;
    const char* name;
    PPCFunc* fn;
};

const Impl kImpls[] = {
    {0x0190, "XamInputGetCapabilities", &XamInputGetCapabilities},
    {0x0191, "XamInputGetState", &XamInputGetState},
    {0x0192, "XamInputSetState", &XamInputSetState},
    {0x0198, "XamInputGetKeystrokeEx", &XamInputGetKeystrokeEx},
    {0x01EA, "XamAlloc", &XamAlloc},
    {0x01EC, "XamFree", &XamFree},
    {0x0280, "XamGetExecutionId", &XamGetExecutionId},
    {0x03CB, "XGetAVPack", &XGetAVPack},
    {0x03CC, "XGetGameRegion", &XGetGameRegion},
    {0x03CD, "XGetLanguage", &XGetLanguage},
    {0x03D1, "XGetVideoMode", &XGetVideoMode},
};

}  // namespace

Status runtime_configure_xam(const XamRuntimeConfig& config) {
    Runtime* r = runtime();
    if (!r || !r->xam) return Status::NotInitialized;
    if (!config.video.display_width || !config.video.display_height ||
        !std::isfinite(config.video.refresh_rate_hz) || config.video.refresh_rate_hz <= 0.0f)
        return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(r->xam->mutex);
    if (r->xam->configured) return Status::Conflict;
    r->xam->config = config;
    r->xam->configured = true;
    return Status::Ok;
}

Status register_xam_hle() {
    Runtime* r = runtime();
    if (!r) return Status::NotInitialized;
    if (!r->xam) {
#if defined(__cpp_exceptions)
        try {
#endif
            r->xam = std::make_unique<XamState>();
#if defined(__cpp_exceptions)
        } catch (const std::bad_alloc&) {
            return Status::OutOfMemory;
        }
#endif
    }
    for (const Impl& i : kImpls) {
        uint32_t ord = 0;
        if (!export_ordinal(kModuleXam, i.name, &ord) || ord != i.ordinal)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam HLE %s: ordinal 0x%04X not in export table", i.name, i.ordinal);
        Status s = register_import(kModuleXam, i.ordinal, i.fn, i.name);
        if (s != Status::Ok) return s;
    }
    Status status = register_xam_net_hle();
    if (status != Status::Ok) return status;
    status = register_xam_loader_hle();
    if (status != Status::Ok) return status;
    status = register_xam_notifications_hle();
    if (status != Status::Ok) return status;
    status = register_xam_enum_hle();
    if (status != Status::Ok) return status;
    status = register_xam_msg_hle();
    if (status != Status::Ok) return status;
    status = register_xam_content_hle();
    if (status != Status::Ok) return status;
    status = register_xam_profile_hle();
    if (status != Status::Ok) return status;
    status = register_xam_misc_hle();
    if (status != Status::Ok) return status;
    status = register_xam_ui_more_hle();
    if (status != Status::Ok) return status;
    status = register_xam_live_dialogs_hle();
    if (status != Status::Ok) return status;
    status = register_xam_more_hle();  // src/hle_xam_more.cpp
    return status != Status::Ok ? status : register_xam_system_hle();
}

}  // namespace rcomp::rt
