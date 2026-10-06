// XAudio ducker settings (owner: Agent 3, runtime/). Contract: runtime/docs/XAUDIO.md, "Ducker".
//
// The console's audio ducker lowers title audio while voice chat is heard. R-comp has no voice
// device (XamVoiceCreate fails with ERROR_DEVICE_NOT_CONNECTED, XamVoiceIsActiveProcess is FALSE), so
// nothing ever triggers it; what remains is its configuration, kept here per Runtime generation:
// the enabled flag (XAudioEnableDucker / XAudioIsDuckerEnabled) and the level, threshold, attack, hold
// and release values the getters report. The setters are not provided: their value argument is FLOAT
// for some of them (passed in f1, not r3) and no call site establishes which, so a title that sets a
// value stops at the missing import instead of having it misread.
//
// ABI. XAudioEnableDucker(DWORD) -> HRESULT is the one-argument form of Xenia 95a5c3e / rexglue-sdk
// c94f5eb xboxkrnl_audio (read only). The getters XAudioGetDucker{Level,Threshold,AttackTime,
// HoldTime,ReleaseTime}(PVOID out) -> HRESULT follow the kernel XAudio query convention
// (XAudioGetSpeakerConfig(PDWORD), XAudioGetVoiceCategoryVolume(DWORD, float*)); no title call site
// was available to confirm them (NARUTO STORM 3 is not in the local catalog), so an output pointer
// that is not writable guest memory is reported as an ABI fatal rather than answered. The console's
// initial values are not established: R-comp starts every value at the all-zero word (0 and 0.0f) and
// the ducker disabled.
#include <stdio.h>

#include <mutex>

#include "hle_more.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"

namespace rcomp::rt {
namespace {

enum Parameter { kLevel, kThreshold, kAttackTime, kHoldTime, kReleaseTime, kParameterCount };

struct DuckerState {
    std::mutex mutex;
    uint64_t generation = 0;
    bool enabled = false;
    uint32_t values[kParameterCount] = {};
};
DuckerState g_ducker;

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r || !r->mem) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xboxkrnl.exe!%s before runtime_init", fn);
    return *r;
}

// Caller holds g_ducker.mutex: a new Runtime starts from the initial settings.
void sync_locked(const Runtime& r) {
    if (g_ducker.generation == r.generation) return;
    g_ducker.generation = r.generation;
    g_ducker.enabled = false;
    for (auto& v : g_ducker.values) v = 0;
}

// XAudioEnableDucker (0x034D): (BOOL Enable) -> S_OK.
void XAudioEnableDucker(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XAudioEnableDucker");
    std::lock_guard<std::mutex> lock(g_ducker.mutex);
    sync_locked(r);
    g_ducker.enabled = ctx.r3.u32 != 0;
    ctx.r3.u64 = 0;
}

// XAudioIsDuckerEnabled (0x034F): (void) -> BOOL, the last XAudioEnableDucker setting.
void XAudioIsDuckerEnabled(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XAudioIsDuckerEnabled");
    std::lock_guard<std::mutex> lock(g_ducker.mutex);
    sync_locked(r);
    ctx.r3.u64 = g_ducker.enabled ? 1 : 0;
}

void get_parameter(PPCContext& ctx, const char* fn, Parameter parameter) {
    Runtime& r = current(fn);
    const uint32_t out = ctx.r3.u32;
    if (!out || (out & 3) || !r.mem->is_accessible(out, 4, Protect::ReadWrite))
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xboxkrnl.exe!%s r3=0x%08X is not a writable output pointer: the getter ABI (PVOID out) is "
                    "not the title's lr=0x%08X",
                    fn, out, uint32_t(ctx.lr));
    uint32_t value = 0;
    {
        std::lock_guard<std::mutex> lock(g_ducker.mutex);
        sync_locked(r);
        value = g_ducker.values[parameter];
    }
    guest_write_be32(out, value);
    ctx.r3.u64 = 0;
}

void XAudioGetDuckerLevel(PPCContext& ctx, uint8_t*) { get_parameter(ctx, "XAudioGetDuckerLevel", kLevel); }
void XAudioGetDuckerThreshold(PPCContext& ctx, uint8_t*) {
    get_parameter(ctx, "XAudioGetDuckerThreshold", kThreshold);
}
void XAudioGetDuckerAttackTime(PPCContext& ctx, uint8_t*) {
    get_parameter(ctx, "XAudioGetDuckerAttackTime", kAttackTime);
}
void XAudioGetDuckerHoldTime(PPCContext& ctx, uint8_t*) { get_parameter(ctx, "XAudioGetDuckerHoldTime", kHoldTime); }
void XAudioGetDuckerReleaseTime(PPCContext& ctx, uint8_t*) {
    get_parameter(ctx, "XAudioGetDuckerReleaseTime", kReleaseTime);
}

struct Impl {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Impl kImpls[] = {
    {0x034D, "XAudioEnableDucker", &XAudioEnableDucker},
    {0x034F, "XAudioIsDuckerEnabled", &XAudioIsDuckerEnabled},
    {0x0350, "XAudioGetDuckerLevel", &XAudioGetDuckerLevel},
    {0x0351, "XAudioGetDuckerThreshold", &XAudioGetDuckerThreshold},
    {0x0353, "XAudioGetDuckerAttackTime", &XAudioGetDuckerAttackTime},
    {0x0355, "XAudioGetDuckerReleaseTime", &XAudioGetDuckerReleaseTime},
    {0x0357, "XAudioGetDuckerHoldTime", &XAudioGetDuckerHoldTime},
};

}  // namespace

Status register_xboxkrnl_audio_ducker_hle() {
    for (const Impl& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            return Status::InvalidArgument;
        const Status status = register_import(kModuleXboxkrnl, impl.ordinal, impl.function, impl.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
