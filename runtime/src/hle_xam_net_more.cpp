// xam.xex NetDll: XNet key creation, connect, address and QoS queries for a console without an XNet link
// or an Xbox LIVE logon (owner: Agent 3, runtime/). Contract: runtime/docs/NETDLL.md, "XNet keys,
// connect, address and QoS queries".
//
// ABI: the XDK import wrappers recompiled in Halo 3 (4D5307E6, sub_826099E8..sub_82609B50) shift the
// title's arguments up one register and put XNCALLER_TITLE = 1 in r3:
//   XNetCreateKey(caller, XNKID*, XNKEY*)                         XNetConnect(caller, IN_ADDR)
//   XNetInAddrToXnAddr(caller, IN_ADDR, XNADDR*, XNKID*)          XNetXnAddrToMachineId(caller, const XNADDR*, ULONGLONG*)
//   XNetQosServiceLookup(caller, DWORD flags, WSAEVENT, XNQOS**)  XNetQosGetListenStats(caller, const XNKID*, XNQOSLISTENSTATS*)
// all returning INT (0 or a WinSock error). XNADDR (36 bytes): +0 ina, +4 inaOnline, +8 wPortOnline,
// +0xA abEnet[6], +0x10 abOnline[20]. Halo 3 asks XNetXnAddrToMachineId only for an XNADDR whose
// inaOnline is non-zero (0x823A0FF8) and skips the QoS result on any non-zero return (0x8209428C).
//
// Contract shared with src/hle_xam_net.cpp and src/hle_xam_misc.cpp: the link is down, the title's own
// XNADDR is empty, nothing is ever registered in the XNet security table and no QoS listener exists, so
// every address or QoS query names something that does not exist: WSAEINVAL. Key creation is local work
// and is done for real.
#include <stdio.h>

#include <cstring>

#include "hle_more.h"
#include "host_random.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "xam_net_internal.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kXnKidBytes = 8, kXnKeyBytes = 16, kXnAddrBytes = 36;
constexpr uint8_t kXnKidFlagMask = 0xF0;  // XNET_XNKID_MASK; 0x00 in the top bits = XNET_XNKID_SYSTEM_LINK

uint32_t wsa(int error) { return static_cast<uint32_t>(error); }

// NetDll_XNetCreateKey (0x0036): a new system-link key pair: 8 random XNKID bytes whose flag bits mark a
// system-link session, 16 random XNKEY bytes. WSAEFAULT for an unwritable output, WSANOTINITIALISED
// before XNetStartup. The pair is not registered (XNetRegisterKey does that).
void XNetCreateKey(PPCContext& ctx, uint8_t*) {
    uint8_t* id = nullptr;
    uint8_t* key = nullptr;
    if (!netdll::guest_span(ctx.r4.u32, kXnKidBytes, true, &id) ||
        !netdll::guest_span(ctx.r5.u32, kXnKeyBytes, true, &key)) {
        ctx.r3.u64 = wsa(netdll::kWsaEfault);
        return;
    }
    if (!netdll::xnet_started(nullptr)) {
        ctx.r3.u64 = wsa(netdll::kWsaNotInitialized);
        return;
    }
    uint8_t bytes[kXnKidBytes + kXnKeyBytes];
    if (!host_random_bytes(bytes, sizeof(bytes)))
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "xam.xex!NetDll_XNetCreateKey: no host random source");
    bytes[0] &= uint8_t(~kXnKidFlagMask);
    std::memcpy(id, bytes, kXnKidBytes);
    std::memcpy(key, bytes + kXnKidBytes, kXnKeyBytes);
    ctx.r3.u64 = 0;
}

// NetDll_XNetConnect (0x0041): starts the key exchange with an address registered through
// XNetXnAddrToInAddr / XNetServerToInAddr; none can be: WSAEINVAL.
void XNetConnect(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = wsa(netdll::kWsaEinval); }

// NetDll_XNetInAddrToXnAddr (0x003C): the reverse lookup of the same table: WSAEINVAL, outputs untouched.
void XNetInAddrToXnAddr(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = wsa(netdll::kWsaEinval); }

// NetDll_XNetXnAddrToMachineId (0x0040): the machine id is carried by the online part of an XNADDR,
// which only an Xbox LIVE logon fills. An XNADDR without it (inaOnline 0, abOnline zero, which is every
// XNADDR R-comp produces) has no machine id: WSAEINVAL. WSAEFAULT when the XNADDR or the output is not
// accessible. An XNADDR carrying online data cannot come from this console; its decoding is not
// established and is an explicit fatal.
void XNetXnAddrToMachineId(PPCContext& ctx, uint8_t*) {
    uint8_t* address = nullptr;
    uint8_t* out = nullptr;
    if (!netdll::guest_span(ctx.r4.u32, kXnAddrBytes, false, &address) ||
        !netdll::guest_span(ctx.r5.u32, 8, true, &out)) {
        ctx.r3.u64 = wsa(netdll::kWsaEfault);
        return;
    }
    bool online = false;
    for (uint32_t i = 4; i < 8; ++i) online |= address[i] != 0;      // inaOnline
    for (uint32_t i = 0x10; i < kXnAddrBytes; ++i) online |= address[i] != 0;  // abOnline
    if (online)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xam.xex!NetDll_XNetXnAddrToMachineId: XNADDR 0x%08X carries Xbox LIVE data, which no R-comp "
                    "service produces (machine id decoding not established) lr=0x%08X",
                    ctx.r4.u32, uint32_t(ctx.lr));
    ctx.r3.u64 = wsa(netdll::kWsaEinval);
}

// NetDll_XNetQosServiceLookup (0x0047): probes the Xbox LIVE service, which needs the logon's security
// association; there is none (XNetQosLookup's answer in src/hle_xam_misc.cpp): *ppxnqos = NULL when
// writable, WSAEINVAL. The event is not signalled: no lookup was started.
void XNetQosServiceLookup(PPCContext& ctx, uint8_t*) {
    uint8_t* out = nullptr;
    if (netdll::guest_span(ctx.r6.u32, 4, true, &out)) guest_write_be32(ctx.r6.u32, 0);
    ctx.r3.u64 = wsa(netdll::kWsaEinval);
}

// NetDll_XNetQosGetListenStats (0x004D): statistics of a listener created by XNetQosListen, which never
// succeeds here: no listener has this XNKID, WSAEINVAL, statistics untouched.
void XNetQosGetListenStats(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = wsa(netdll::kWsaEinval); }

// NetDll_XnpLogonGetStatus (0x0070): an XNet-private logon query; no title call site or reference
// establishes its arguments or result encoding, so it stops with the registers a later contract needs.
void XnpLogonGetStatus(PPCContext& ctx, uint8_t*) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                "xam.xex!NetDll_XnpLogonGetStatus r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X lr=0x%08X: arguments "
                "and result encoding not established (R-comp is never logged on to Xbox LIVE)",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
}

struct Entry {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Entry kEntries[] = {
    {0x0036, "NetDll_XNetCreateKey", &XNetCreateKey},
    {0x003C, "NetDll_XNetInAddrToXnAddr", &XNetInAddrToXnAddr},
    {0x0040, "NetDll_XNetXnAddrToMachineId", &XNetXnAddrToMachineId},
    {0x0041, "NetDll_XNetConnect", &XNetConnect},
    {0x0047, "NetDll_XNetQosServiceLookup", &XNetQosServiceLookup},
    {0x004D, "NetDll_XNetQosGetListenStats", &XNetQosGetListenStats},
    {0x0070, "NetDll_XnpLogonGetStatus", &XnpLogonGetStatus},
};

}  // namespace

Status register_xam_net_more_hle() {
    for (const auto& entry : kEntries) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXam, entry.name, &ordinal) || ordinal != entry.ordinal) return Status::Conflict;
        const Status status = register_import(kModuleXam, entry.ordinal, entry.function, entry.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
