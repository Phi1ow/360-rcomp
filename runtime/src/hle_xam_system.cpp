#include "rcomp/runtime/xam.h"
#include "rcomp/runtime/xam_profile.h"
#include <atomic>
#include "rcomp/diag.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/kernel_variables.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/runtime.h"

namespace rcomp::rt {
namespace {
void XamGetSystemVersion(PPCContext& ctx,uint8_t*) {
    KernelCompatibilityProfile profile;
    const Status status=xboxkrnl_kernel_compatibility_profile(&profile);
    if (status!=Status::Ok)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "XamGetSystemVersion requires the title-derived kernel compatibility profile: %s",
                    status_name(status));
    // Public XAM ABI returns the packed version word. Use the same selected
    // profile as XboxKrnlVersion, never the host firmware or a guessed value.
    ctx.r3.u64=profile.packed;
}
// ---- user/profile queries: one local, offline profile (rcomp/runtime/xam_profile.h) ----------
// Slot 0 holds the local player, signed in locally (state 1) with an offline XUID and the gamertag
// "Player"; slots 1-3 are empty and report X_E_NO_SUCH_USER as a console does. No profile is signed in to
// Xbox LIVE, so an online XUID does not exist and every LIVE privilege is denied. Codes: public
// rexglue-sdk c94f5eb xam_user.cpp; layouts: X_USER_SIGNIN_INFO (40 bytes) xuid +0, info flags +8,
// state +0x0C, guest number +0x10, sponsor index +0x14, name[16] +0x18.
constexpr uint32_t kEInvalidArg = 0x80070057u;
constexpr uint32_t kENoSuchUser = 0x80070525u;
constexpr uint32_t kAnyUser = 0xFFu;  // "any signed-in user" index
constexpr uint32_t kXuidOffline = 1;  // XUserGetXUID type mask: offline XUID (2 and 4 ask for online ones)
constexpr uint32_t kSigninInfoOnlineXuidOnly = 2;

bool writable(uint32_t address, uint32_t size) {
    Runtime* r = runtime();
    return r && r->mem && address && r->mem->is_accessible(address, size, Protect::ReadWrite);
}
void zero_guest(uint32_t address, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        uint8_t* p = runtime()->mem->translate(address + i, 1);
        if (p) *p = 0;
    }
}
void write_be64(uint32_t address, uint64_t value) {
    guest_write_be32(address, uint32_t(value >> 32));
    guest_write_be32(address + 4, uint32_t(value));
}
// Slot of a query: the local player for 0 and "any user"; NoSuchUser for the empty slots 1-3.
bool local_slot(uint32_t index) { return is_local_user(index) || index == kAnyUser; }
uint64_t slot_result(uint32_t index) { return index < 4 ? kENoSuchUser : kEInvalidArg; }

// XamUserGetSigninState(DWORD UserIndex).
void XamUserGetSigninState(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = local_slot(ctx.r3.u32) ? kSigninStateSignedInLocally : kSigninStateNotSignedIn;
}

// XamUserGetXUID(DWORD UserIndex, DWORD TypeMask, PQWORD Xuid).
void XamUserGetXUID(PPCContext& ctx, uint8_t*) {
    const uint32_t index = ctx.r3.u32, mask = ctx.r4.u32, xuid = ctx.r5.u32;
    if (!xuid || !writable(xuid, 8)) { ctx.r3.u64 = kEInvalidArg; return; }
    zero_guest(xuid, 8);
    if (!local_slot(index)) { ctx.r3.u64 = slot_result(index); return; }
    // A local profile has only an offline XUID.
    if (!(mask & kXuidOffline)) { ctx.r3.u64 = kENoSuchUser; return; }
    write_be64(xuid, kLocalUserXuid);
    ctx.r3.u64 = 0;
}

// XamUserGetSigninInfo(DWORD UserIndex, DWORD Flags, PX_USER_SIGNIN_INFO).
void XamUserGetSigninInfo(PPCContext& ctx, uint8_t*) {
    const uint32_t index = ctx.r3.u32, flags = ctx.r4.u32, info = ctx.r5.u32;
    if (!info || !writable(info, 40)) { ctx.r3.u64 = kEInvalidArg; return; }
    zero_guest(info, 40);
    if (!local_slot(index)) { ctx.r3.u64 = slot_result(index); return; }
    if (!(flags & kSigninInfoOnlineXuidOnly)) write_be64(info, kLocalUserXuid);
    guest_write_be32(info + 0x0C, kSigninStateSignedInLocally);
    for (uint32_t i = 0; kLocalUserName[i] && i < 15; ++i)
        *runtime()->mem->translate(info + 0x18 + i, 1) = uint8_t(kLocalUserName[i]);
    ctx.r3.u64 = 0;
}

// XamUserGetName(DWORD UserIndex, PCHAR Buffer, DWORD Length): NUL-terminated, truncated to the buffer.
void XamUserGetName(PPCContext& ctx, uint8_t*) {
    const uint32_t index = ctx.r3.u32, buffer = ctx.r4.u32, length = ctx.r5.u32;
    if (!local_slot(index)) { ctx.r3.u64 = slot_result(index); return; }
    if (!length || !writable(buffer, length)) { ctx.r3.u64 = kEInvalidArg; return; }
    uint32_t i = 0;
    for (; kLocalUserName[i] && i + 1 < length && i < 15; ++i)
        *runtime()->mem->translate(buffer + i, 1) = uint8_t(kLocalUserName[i]);
    *runtime()->mem->translate(buffer + i, 1) = 0;
    ctx.r3.u64 = 0;
}

// XamUserCheckPrivilege(DWORD UserIndex, DWORD Privilege, PBOOL Result): the privileges are Xbox LIVE
// ones (multiplayer, communications, content...), which a profile not signed in to LIVE does not hold.
void XamUserCheckPrivilege(PPCContext& ctx, uint8_t*) {
    const uint32_t index = ctx.r3.u32, result = ctx.r5.u32;
    if (!result || !writable(result, 4)) { ctx.r3.u64 = kEInvalidArg; return; }
    zero_guest(result, 4);
    ctx.r3.u64 = local_slot(index) ? 0 : slot_result(index);
}

// ---- inactivity ---------------------------------------------------------
// XamResetInactivity() restarts the console's idle clock (the dashboard's screensaver / auto power-off countdown the title keeps feeding while the player is
// active) and XamEnableInactivityProcessing(a, b) switches that processing on or off; both return ERROR_SUCCESS. The idle policy of this platform is the PS5's own
// (a title in the foreground is kept awake by its controller input) and there is no XAM idle UI to drive, so the calls are only recorded, as XNotifyPositionUI
// is. No public documentation says which argument of XamEnableInactivityProcessing carries the flag: both are kept raw instead of guessing.
std::atomic<uint64_t> g_inactivity_resets{0};
std::atomic<uint64_t> g_inactivity_enable_requests{0};
std::atomic<uint32_t> g_inactivity_enable_args[2]={};
void XamResetInactivity(PPCContext& ctx,uint8_t*) {
    g_inactivity_resets.fetch_add(1,std::memory_order_relaxed);
    ctx.r3.u64=0;
}
void XamEnableInactivityProcessing(PPCContext& ctx,uint8_t*) {
    g_inactivity_enable_args[0].store(ctx.r3.u32,std::memory_order_relaxed);
    g_inactivity_enable_args[1].store(ctx.r4.u32,std::memory_order_relaxed);
    g_inactivity_enable_requests.fetch_add(1,std::memory_order_relaxed);
    ctx.r3.u64=0;
}

struct Impl {uint32_t ordinal;const char* name;PPCFunc* function;};
const Impl entries[]={
    {0x01A0,"XamEnableInactivityProcessing",&XamEnableInactivityProcessing},
    {0x01A1,"XamResetInactivity",&XamResetInactivity},
    {0x0282,"XamGetSystemVersion",&XamGetSystemVersion},
    {0x020A,"XamUserGetXUID",&XamUserGetXUID},
    {0x020E,"XamUserGetName",&XamUserGetName},
    {0x0210,"XamUserGetSigninState",&XamUserGetSigninState},
    {0x0212,"XamUserCheckPrivilege",&XamUserCheckPrivilege},
    {0x0227,"XamUserGetSigninInfo",&XamUserGetSigninInfo},
};
}
XamInactivityStats xam_inactivity_stats() {
    XamInactivityStats stats;
    stats.resets=g_inactivity_resets.load(std::memory_order_relaxed);
    stats.enable_requests=g_inactivity_enable_requests.load(std::memory_order_relaxed);
    stats.last_enable_args[0]=g_inactivity_enable_args[0].load(std::memory_order_relaxed);
    stats.last_enable_args[1]=g_inactivity_enable_args[1].load(std::memory_order_relaxed);
    return stats;
}
Status register_xam_system_hle() {
    g_inactivity_resets.store(0,std::memory_order_relaxed);
    g_inactivity_enable_requests.store(0,std::memory_order_relaxed);
    g_inactivity_enable_args[0].store(0,std::memory_order_relaxed);
    g_inactivity_enable_args[1].store(0,std::memory_order_relaxed);
    for (const auto& entry:entries) {
        uint32_t ordinal=0;
        if (!export_ordinal(kModuleXam,entry.name,&ordinal)||ordinal!=entry.ordinal) return Status::Conflict;
        const Status status=register_import(kModuleXam,entry.ordinal,entry.function,entry.name);
        if (status!=Status::Ok) return status;
    }
    return Status::Ok;
}
}
