// XNet keys, connect, address and QoS queries (src/hle_xam_net_more.cpp) on a console with no XNet link
// and no Xbox LIVE logon. Arguments as the XDK wrappers pass them: XNCALLER_TITLE (1) in r3.
#include <initializer_list>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam_net.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__NetDll_XNetStartup);
PPC_EXTERN_FUNC(__imp__NetDll_XNetCleanup);
PPC_EXTERN_FUNC(__imp__NetDll_XNetCreateKey);
PPC_EXTERN_FUNC(__imp__NetDll_XNetRegisterKey);
PPC_EXTERN_FUNC(__imp__NetDll_XNetConnect);
PPC_EXTERN_FUNC(__imp__NetDll_XNetInAddrToXnAddr);
PPC_EXTERN_FUNC(__imp__NetDll_XNetXnAddrToMachineId);
PPC_EXTERN_FUNC(__imp__NetDll_XNetQosServiceLookup);
PPC_EXTERN_FUNC(__imp__NetDll_XNetQosGetListenStats);
PPC_EXTERN_FUNC(__imp__NetDll_XnpLogonGetStatus);

using namespace rcomp;
using namespace rcomp::rt;

namespace {
GuestMemory g_mem;
uint32_t g_buf;
constexpr uint32_t kCaller = 1, kWsaEfault = 10014, kWsaEinval = 10022, kWsaNotInitialised = 10093;
enum : uint32_t { kKid = 0x00, kKey = 0x10, kKid2 = 0x20, kKey2 = 0x30, kXnAddr = 0x40, kOut = 0x80, kStats = 0xA0 };

uint32_t call(PPCFunc* f, std::initializer_list<uint64_t> args) {
    alignas(64) PPCContext ctx{};
    PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8};
    size_t i = 0;
    for (uint64_t v : args) regs[i++]->u64 = v;
    f(ctx, g_mem.base());
    return ctx.r3.u32;
}
uint8_t* at(uint32_t offset) { return g_mem.base() + g_buf + offset; }
}  // namespace

int main() {
    if (g_mem.reserve() != MemStatus::Ok) return 2;
    CHECK_ST(runtime_init(&g_mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(register_xam_net_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x200, 16, true, &g_buf), Status::Ok);

    // Key creation needs XNetStartup.
    memset(at(kKid), 0xEE, 0x50);
    CHECK_EQ(call(__imp__NetDll_XNetCreateKey, {kCaller, g_buf + kKid, g_buf + kKey}), kWsaNotInitialised);
    CHECK_EQ(at(kKid)[0], 0xEE);
    CHECK_EQ(call(__imp__NetDll_XNetStartup, {kCaller, 0}), 0u);
    CHECK_EQ(call(__imp__NetDll_XNetCreateKey, {kCaller, g_buf + kKid, g_buf + kKey}), 0u);
    CHECK_EQ(call(__imp__NetDll_XNetCreateKey, {kCaller, g_buf + kKid2, g_buf + kKey2}), 0u);
    CHECK_EQ(at(kKid)[0] & 0xF0, 0);  // XNET_XNKID_SYSTEM_LINK
    CHECK_EQ(at(kKid2)[0] & 0xF0, 0);
    CHECK(memcmp(at(kKid), at(kKid2), 8) != 0);
    CHECK(memcmp(at(kKey), at(kKey2), 16) != 0);
    CHECK_EQ(at(kKey2 + 16)[0], 0xEE);  // exactly 16 key bytes
    // The created pair is a usable XNet key: it registers.
    CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, {kCaller, g_buf + kKid, g_buf + kKey}), 0u);
    CHECK_EQ(call(__imp__NetDll_XNetCreateKey, {kCaller, 0, g_buf + kKey}), kWsaEfault);
    CHECK_EQ(call(__imp__NetDll_XNetCreateKey, {kCaller, g_buf + kKid, 0x00001000}), kWsaEfault);

    // Nothing is ever in the XNet security table: connect and reverse lookups fail.
    CHECK_EQ(call(__imp__NetDll_XNetConnect, {kCaller, 0x0A000001}), kWsaEinval);
    memset(at(kXnAddr), 0xEE, 36);
    CHECK_EQ(call(__imp__NetDll_XNetInAddrToXnAddr, {kCaller, 0x0A000001, g_buf + kXnAddr, g_buf + kKid2}),
             kWsaEinval);
    CHECK_EQ(at(kXnAddr)[0], 0xEE);

    // Machine id: an XNADDR without LIVE data (every XNADDR R-comp makes) has none.
    memset(at(kXnAddr), 0, 36);
    at(kXnAddr)[3] = 7;    // ina (local) set
    at(kXnAddr)[0xA] = 1;  // abEnet set
    memset(at(kOut), 0xEE, 8);
    CHECK_EQ(call(__imp__NetDll_XNetXnAddrToMachineId, {kCaller, g_buf + kXnAddr, g_buf + kOut}), kWsaEinval);
    CHECK_EQ(at(kOut)[0], 0xEE);
    CHECK_EQ(call(__imp__NetDll_XNetXnAddrToMachineId, {kCaller, 0, g_buf + kOut}), kWsaEfault);
    CHECK_EQ(call(__imp__NetDll_XNetXnAddrToMachineId, {kCaller, g_buf + kXnAddr, 0}), kWsaEfault);
    bool fatal = false;
    at(kXnAddr)[5] = 1;  // inaOnline
    CAPTURE_FATAL(call(__imp__NetDll_XNetXnAddrToMachineId, {kCaller, g_buf + kXnAddr, g_buf + kOut}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);

    // QoS: no LIVE service association, no listener.
    memset(at(kOut), 0xEE, 4);
    CHECK_EQ(call(__imp__NetDll_XNetQosServiceLookup, {kCaller, 0, 0, g_buf + kOut}), kWsaEinval);
    CHECK_EQ(at(kOut)[0] | at(kOut)[1] | at(kOut)[2] | at(kOut)[3], 0);  // *ppxnqos = NULL
    CHECK_EQ(call(__imp__NetDll_XNetQosServiceLookup, {kCaller, 0, 0, 0}), kWsaEinval);
    memset(at(kStats), 0xEE, 16);
    CHECK_EQ(call(__imp__NetDll_XNetQosGetListenStats, {kCaller, g_buf + kKid, g_buf + kStats}), kWsaEinval);
    CHECK_EQ(at(kStats)[0], 0xEE);

    // XnpLogonGetStatus: no contract, explicit fatal.
    CAPTURE_FATAL(call(__imp__NetDll_XnpLogonGetStatus, {kCaller, 0, 0}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);

    CHECK_EQ(call(__imp__NetDll_XNetCleanup, {kCaller}), 0u);
    runtime_shutdown();
    clear_imports();
    return test_result("rt_xam_net_more");
}
