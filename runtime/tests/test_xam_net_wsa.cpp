// NetDll overlapped WSA I/O, WSA events, WSAEventSelect, __WSAFDIsSet and the XNet random/key/
// address/DNS services (src/hle_xam_net_wsa.cpp) over real host loopback sockets only. No external
// network, XNet identity or LIVE service is involved. The WSA event calls go through the registered
// xboxkrnl event services (register_xboxkrnl_hle), as on the console.
#include <string.h>

#include <chrono>
#include <initializer_list>
#include <thread>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam_net.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__NetDll_WSAStartup);
PPC_EXTERN_FUNC(__imp__NetDll_WSACleanup);
PPC_EXTERN_FUNC(__imp__NetDll_WSAGetLastError);
PPC_EXTERN_FUNC(__imp__NetDll_socket);
PPC_EXTERN_FUNC(__imp__NetDll_closesocket);
PPC_EXTERN_FUNC(__imp__NetDll_ioctlsocket);
PPC_EXTERN_FUNC(__imp__NetDll_setsockopt);
PPC_EXTERN_FUNC(__imp__NetDll_getsockname);
PPC_EXTERN_FUNC(__imp__NetDll_bind);
PPC_EXTERN_FUNC(__imp__NetDll_connect);
PPC_EXTERN_FUNC(__imp__NetDll_listen);
PPC_EXTERN_FUNC(__imp__NetDll_accept);
PPC_EXTERN_FUNC(__imp__NetDll_select);
PPC_EXTERN_FUNC(__imp__NetDll_recv);
PPC_EXTERN_FUNC(__imp__NetDll_send);
PPC_EXTERN_FUNC(__imp__NetDll_sendto);
PPC_EXTERN_FUNC(__imp__NetDll_XNetStartup);
PPC_EXTERN_FUNC(__imp__NetDll_XNetCleanup);
// Imports under test (src/hle_xam_net_wsa.cpp).
PPC_EXTERN_FUNC(__imp__NetDll_WSAGetOverlappedResult);
PPC_EXTERN_FUNC(__imp__NetDll_WSACancelOverlappedIO);
PPC_EXTERN_FUNC(__imp__NetDll_WSARecv);
PPC_EXTERN_FUNC(__imp__NetDll_WSARecvFrom);
PPC_EXTERN_FUNC(__imp__NetDll_WSASend);
PPC_EXTERN_FUNC(__imp__NetDll_WSASendTo);
PPC_EXTERN_FUNC(__imp__NetDll_WSACreateEvent);
PPC_EXTERN_FUNC(__imp__NetDll_WSACloseEvent);
PPC_EXTERN_FUNC(__imp__NetDll_WSASetEvent);
PPC_EXTERN_FUNC(__imp__NetDll_WSAResetEvent);
PPC_EXTERN_FUNC(__imp__NetDll_WSAWaitForMultipleEvents);
PPC_EXTERN_FUNC(__imp__NetDll___WSAFDIsSet);
PPC_EXTERN_FUNC(__imp__NetDll_WSAEventSelect);
PPC_EXTERN_FUNC(__imp__NetDll_XNetRandom);
PPC_EXTERN_FUNC(__imp__NetDll_XNetRegisterKey);
PPC_EXTERN_FUNC(__imp__NetDll_XNetUnregisterKey);
PPC_EXTERN_FUNC(__imp__NetDll_XNetInAddrToString);
PPC_EXTERN_FUNC(__imp__NetDll_XNetDnsLookup);
PPC_EXTERN_FUNC(__imp__NetDll_XNetDnsRelease);

using namespace rcomp;
using namespace rcomp::rt;

namespace {

// ---- guest layout ------------------------------------------------------------------------------------
constexpr uint32_t S = 0x30000000;          // committed read/write scratch (0x40000 bytes)
constexpr uint32_t kBad = 0x30040000;       // committed, then made inaccessible
constexpr uint32_t kStackMain = 0x30037000;  // guest r1 of the main thread
constexpr uint32_t kStackChild = 0x3003F000; // guest r1 of helper threads
constexpr uint32_t kWsadata = S + 0x000;
constexpr uint32_t kEvArr = S + 0x200;
constexpr uint32_t kFdSet = S + 0x300;     // up to 4 + 100 * 4 bytes
constexpr uint32_t kSelSet = S + 0x500;
constexpr uint32_t kTimeval = S + 0x5F0;
constexpr uint32_t kAddr = S + 0x600, kAddr2 = S + 0x620, kAddrLen = S + 0x640, kOpt = S + 0x650;
constexpr uint32_t kBytes = S + 0x660, kFlags = S + 0x664, kFromLen = S + 0x668, kFrom = S + 0x680;
constexpr uint32_t kOv1 = S + 0x700, kOv2 = S + 0x720, kOv3 = S + 0x740;
constexpr uint32_t kBytes2 = S + 0x760, kFlags2 = S + 0x764;
constexpr uint32_t kWsabuf = S + 0x800, kWsabuf2 = S + 0x840, kWsabuf3 = S + 0x880;
constexpr uint32_t kKey = S + 0x900, kKeyId = S + 0x920, kDnsOut = S + 0x980, kHost = S + 0xA00;
constexpr uint32_t kText = S + 0xB00;
constexpr uint32_t kData = S + 0x1000, kData2 = S + 0x1800;
constexpr uint32_t kRecv = S + 0x2000, kRecv2 = S + 0x2800, kRecv3 = S + 0x3000;
constexpr uint32_t kRandom = S + 0x4000;
constexpr uint32_t kBig = S + 0x10000;   // 0x10000 bytes of send data
constexpr uint32_t kSink = S + 0x20000;  // 0x8000 bytes of drain buffer

constexpr uint32_t kSocketError = 0xFFFFFFFFu;
constexpr uint32_t kWaitTimeout = 0x102, kWaitFailed = 0xFFFFFFFFu, kInfinite = 0xFFFFFFFFu;
constexpr uint32_t kStatusPending = 0x103;
constexpr uint32_t kWsaEfault = 10014, kWsaEinval = 10022, kWsaEwouldblock = 10035, kWsaEalready = 10037,
                   kWsaEnotsock = 10038, kWsaEafnosupport = 10047, kWsaEnetdown = 10050, kWsaEnobufs = 10055,
                   kWsaNotInitialised = 10093;
constexpr uint32_t kAborted = 995, kIoIncomplete = 996, kIoPending = 997;
constexpr uint32_t kErrorInvalidHandle = 6, kErrorInvalidParameter = 87;
constexpr uint32_t kFdRead = 0x01, kFdWrite = 0x02, kFdAccept = 0x08, kFdConnect = 0x10, kFdClose = 0x20;
constexpr uint32_t kFionbio = 0x8004667E;
constexpr uint32_t kBogusHandle = 0xDEAD0000u;

GuestMemory g_mem;
uint8_t* g_base = nullptr;
thread_local uint32_t t_sp = kStackMain;

uint32_t rd32(uint32_t address) {
    uint32_t value = 0;
    CHECK(guest_read_be32(address, &value));
    return value;
}
uint16_t rd16(uint32_t address) {
    uint16_t value = 0;
    CHECK(guest_read_be16(address, &value));
    return value;
}
void wr32(uint32_t address, uint32_t value) { CHECK(guest_write_be32(address, value)); }
void wr16(uint32_t address, uint16_t value) {
    const uint16_t be = __builtin_bswap16(value);
    memcpy(g_base + address, &be, sizeof(be));
}
void put(uint32_t address, const char* text) { memcpy(g_base + address, text, strlen(text) + 1); }
bool same(uint32_t address, const char* text) { return memcmp(g_base + address, text, strlen(text)) == 0; }

// Calls an import with r1 on this thread's guest stack; s8/s9 are the 9th/10th arguments, stored in
// the caller frame at r1 + 0x54 / r1 + 0x5C.
uint32_t call(PPCFunc* fn, uint32_t r3 = 0, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0, uint32_t r7 = 0,
              uint32_t r8 = 0, uint32_t r9 = 0, uint32_t r10 = 0, uint32_t s8 = 0, uint32_t s9 = 0) {
    alignas(64) PPCContext ctx{};
    ctx.r1.u64 = t_sp;
    wr32(t_sp + 0x54, s8);
    wr32(t_sp + 0x5C, s9);
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    ctx.r7.u64 = r7;
    ctx.r8.u64 = r8;
    ctx.r9.u64 = r9;
    ctx.r10.u64 = r10;
    fn(ctx, g_base);
    return ctx.r3.u32;
}

uint32_t last_error() { return call(__imp__NetDll_WSAGetLastError); }

// ---- sockets -----------------------------------------------------------------------------------------
uint32_t tcp() {
    const uint32_t s = call(__imp__NetDll_socket, 0, 2, 1, 6);
    CHECK(s != kSocketError);
    return s;
}
uint32_t udp() {
    const uint32_t s = call(__imp__NetDll_socket, 0, 2, 2, 17);
    CHECK(s != kSocketError);
    return s;
}
void sockaddr4(uint32_t address, uint32_t ipv4, uint16_t port, uint16_t family = 2) {
    memset(g_base + address, 0, 16);
    wr16(address, family);
    wr16(address + 2, port);
    wr32(address + 4, ipv4);
}
uint16_t bind_loopback(uint32_t s) {
    sockaddr4(kAddr, 0x7F000001u, 0);
    CHECK_EQ(call(__imp__NetDll_bind, 0, s, kAddr, 16), 0u);
    wr32(kAddrLen, 16);
    CHECK_EQ(call(__imp__NetDll_getsockname, 0, s, kAddr, kAddrLen), 0u);
    const uint16_t port = rd16(kAddr + 2);
    CHECK(port != 0);
    return port;
}
uint32_t connect_to(uint32_t s, uint16_t port) {
    sockaddr4(kAddr2, 0x7F000001u, port);
    return call(__imp__NetDll_connect, 0, s, kAddr2, 16);
}
uint32_t accept_one(uint32_t listener) { return call(__imp__NetDll_accept, 0, listener, 0, 0); }
void set_nonblocking(uint32_t s, uint32_t on) {
    wr32(kOpt, on);
    CHECK_EQ(call(__imp__NetDll_ioctlsocket, 0, s, kFionbio, kOpt), 0u);
}
void set_int_option(uint32_t s, uint32_t name, uint32_t value) {
    wr32(kOpt, value);
    CHECK_EQ(call(__imp__NetDll_setsockopt, 0, s, 0xFFFF, name, kOpt, 4), 0u);
}
bool readable(uint32_t s, uint32_t ms) {
    wr32(kSelSet, 1);
    wr32(kSelSet + 4, s);
    wr32(kTimeval, ms / 1000);
    wr32(kTimeval + 4, (ms % 1000) * 1000);
    return call(__imp__NetDll_select, 0, 0, kSelSet, 0, 0, kTimeval) == 1;
}
uint32_t send_plain(uint32_t s, uint32_t data, uint32_t size) {
    return call(__imp__NetDll_send, 0, s, data, size, 0);
}
uint32_t recv_plain(uint32_t s, uint32_t data, uint32_t size) {
    return call(__imp__NetDll_recv, 0, s, data, size, 0);
}
// Sends on a nonblocking stream socket until even one byte would block; returns the bytes sent.
uint32_t fill_send_buffer(uint32_t s) {
    uint32_t total = 0;
    for (uint32_t chunk : {0x8000u, 1000u, 1u}) {
        for (int i = 0; i < 200000; ++i) {
            const uint32_t n = send_plain(s, kBig, chunk);
            if (n == kSocketError) {
                CHECK_EQ(last_error(), kWsaEwouldblock);
                break;
            }
            total += n;
        }
    }
    return total;
}
// Reads everything that arrives within `idle_ms` of silence.
uint32_t drain(uint32_t s, uint32_t idle_ms) {
    uint32_t total = 0;
    while (readable(s, idle_ms)) {
        const uint32_t n = recv_plain(s, kSink, 0x8000);
        if (n == kSocketError || n == 0) break;
        total += n;
    }
    return total;
}

// ---- WSA calls ---------------------------------------------------------------------------------------
void wsabuf(uint32_t array, uint32_t index, uint32_t length, uint32_t buffer) {
    wr32(array + 8 * index, length);
    wr32(array + 8 * index + 4, buffer);
}
void overlapped(uint32_t ov, uint32_t event) {
    memset(g_base + ov, 0xCC, 0x14);
    wr32(ov + 0x10, event);
}
uint32_t wsa_recv(uint32_t s, uint32_t bufs, uint32_t count, uint32_t bytes, uint32_t flags, uint32_t ov,
                  uint32_t routine = 0) {
    return call(__imp__NetDll_WSARecv, 1, s, bufs, count, bytes, flags, ov, routine);
}
uint32_t wsa_send(uint32_t s, uint32_t bufs, uint32_t count, uint32_t bytes, uint32_t flags, uint32_t ov,
                  uint32_t routine = 0) {
    return call(__imp__NetDll_WSASend, 1, s, bufs, count, bytes, flags, ov, routine);
}
uint32_t wsa_recvfrom(uint32_t s, uint32_t bufs, uint32_t count, uint32_t bytes, uint32_t flags, uint32_t from,
                      uint32_t fromlen, uint32_t ov, uint32_t routine = 0) {
    return call(__imp__NetDll_WSARecvFrom, 1, s, bufs, count, bytes, flags, from, fromlen, ov, routine);
}
uint32_t wsa_sendto(uint32_t s, uint32_t bufs, uint32_t count, uint32_t bytes, uint32_t flags, uint32_t to,
                    uint32_t tolen, uint32_t ov, uint32_t routine = 0) {
    return call(__imp__NetDll_WSASendTo, 1, s, bufs, count, bytes, flags, to, tolen, ov, routine);
}
uint32_t get_result(uint32_t s, uint32_t ov, uint32_t bytes, uint32_t wait, uint32_t flags) {
    return call(__imp__NetDll_WSAGetOverlappedResult, 1, s, ov, bytes, wait, flags);
}
uint32_t create_event() {
    const uint32_t e = call(__imp__NetDll_WSACreateEvent);
    CHECK(e != 0);
    return e;
}
uint32_t wait_events(std::initializer_list<uint32_t> events, bool all, uint32_t ms) {
    uint32_t i = 0;
    for (uint32_t e : events) wr32(kEvArr + 4 * i++, e);
    return call(__imp__NetDll_WSAWaitForMultipleEvents, i, kEvArr, all ? 1 : 0, ms, 0);
}
uint32_t wait1(uint32_t event, uint32_t ms) { return wait_events({event}, false, ms); }
void reset(uint32_t event) { CHECK_EQ(call(__imp__NetDll_WSAResetEvent, event), 1u); }
uint32_t event_select(uint32_t s, uint32_t event, uint32_t mask) {
    return call(__imp__NetDll_WSAEventSelect, 1, s, event, mask);
}

void expect_pending(uint32_t result, uint32_t ov) {
    CHECK_EQ(result, kSocketError);
    CHECK_EQ(last_error(), kIoPending);
    CHECK_EQ(rd32(ov + 0), kStatusPending);
}
void expect_aborted(uint32_t ov, uint32_t event) {
    CHECK_EQ(rd32(ov + 0), kAborted);
    CHECK_EQ(rd32(ov + 4), 0u);
    if (event) CHECK_EQ(wait1(event, 0), 0u);
}

struct Expected {
    uint32_t ordinal;
    const char* name;
    PPCFunc* thunk;
};

}  // namespace

int main() {
    if (g_mem.reserve() != MemStatus::Ok) return 2;
    g_base = g_mem.base();
    CHECK(g_mem.commit(S, 0x50000, Protect::ReadWrite) == MemStatus::Ok);
    CHECK(g_mem.protect(kBad, 0x10000, Protect::None) == MemStatus::Ok);
    CHECK_ST(runtime_init(&g_mem), Status::Ok);
    clear_imports();
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(register_xam_net_hle(), Status::Ok);

    // ---- registration: every import of the unit at its xam_table.inc ordinal ------------------------
    const Expected expected[] = {
        {0x0010, "NetDll_WSAGetOverlappedResult", __imp__NetDll_WSAGetOverlappedResult},
        {0x0011, "NetDll_WSACancelOverlappedIO", __imp__NetDll_WSACancelOverlappedIO},
        {0x0013, "NetDll_WSARecv", __imp__NetDll_WSARecv},
        {0x0015, "NetDll_WSARecvFrom", __imp__NetDll_WSARecvFrom},
        {0x0017, "NetDll_WSASend", __imp__NetDll_WSASend},
        {0x0019, "NetDll_WSASendTo", __imp__NetDll_WSASendTo},
        {0x001D, "NetDll_WSACreateEvent", __imp__NetDll_WSACreateEvent},
        {0x001E, "NetDll_WSACloseEvent", __imp__NetDll_WSACloseEvent},
        {0x001F, "NetDll_WSASetEvent", __imp__NetDll_WSASetEvent},
        {0x0020, "NetDll_WSAResetEvent", __imp__NetDll_WSAResetEvent},
        {0x0021, "NetDll_WSAWaitForMultipleEvents", __imp__NetDll_WSAWaitForMultipleEvents},
        {0x0022, "NetDll___WSAFDIsSet", __imp__NetDll___WSAFDIsSet},
        {0x0023, "NetDll_WSAEventSelect", __imp__NetDll_WSAEventSelect},
        {0x0035, "NetDll_XNetRandom", __imp__NetDll_XNetRandom},
        {0x0037, "NetDll_XNetRegisterKey", __imp__NetDll_XNetRegisterKey},
        {0x0038, "NetDll_XNetUnregisterKey", __imp__NetDll_XNetUnregisterKey},
        {0x003E, "NetDll_XNetInAddrToString", __imp__NetDll_XNetInAddrToString},
        {0x0043, "NetDll_XNetDnsLookup", __imp__NetDll_XNetDnsLookup},
        {0x0044, "NetDll_XNetDnsRelease", __imp__NetDll_XNetDnsRelease},
    };
    for (const Expected& e : expected) {
        uint32_t ordinal = 0;
        CHECK(export_ordinal(kModuleXam, e.name, &ordinal));
        CHECK_EQ(ordinal, e.ordinal);
        CHECK(export_kind(kModuleXam, e.ordinal) == ExportKind::Function);
        CHECK(find_import(kModuleXam, e.ordinal) != nullptr);
        CHECK(import_thunk(kModuleXam, e.ordinal) == e.thunk);
        const char* registered = import_registry_name(kModuleXam, e.ordinal);
        CHECK(registered && strcmp(registered, e.name) == 0);
    }
    // A second registration of the same implementations conflicts with nothing.
    CHECK_ST(register_xam_net_hle(), Status::Ok);

    // ---- before WSAStartup/XNetStartup: WSANOTINITIALISED ---------------------------------------------
    wsabuf(kWsabuf, 0, 4, kRecv);
    wr32(kFlags, 0);
    CHECK_EQ(wsa_recv(0x12345678u, kWsabuf, 1, kBytes, kFlags, 0), kSocketError);
    CHECK_EQ(last_error(), kWsaNotInitialised);
    CHECK_EQ(event_select(0x12345678u, 0, 0), kSocketError);
    CHECK_EQ(last_error(), kWsaNotInitialised);
    CHECK_EQ(call(__imp__NetDll_WSACancelOverlappedIO, 1, 0x12345678u), kSocketError);
    CHECK_EQ(last_error(), kWsaNotInitialised);
    put(kHost, "example.com");
    CHECK_EQ(call(__imp__NetDll_XNetDnsLookup, 1, kHost, 0, kDnsOut), kWsaNotInitialised);
    memset(g_base + kKeyId, 0x11, 8);
    memset(g_base + kKey, 0x22, 16);
    CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kKeyId, kKey), kWsaNotInitialised);
    CHECK_EQ(call(__imp__NetDll_XNetUnregisterKey, 1, kKeyId), kWsaNotInitialised);

    // ---- WSA events: manual-reset kernel events ------------------------------------------------------
    {
        const uint32_t a = create_event(), b = create_event();
        CHECK(a != b);
        CHECK_EQ(wait1(a, 0), kWaitTimeout);  // created nonsignaled
        CHECK_EQ(call(__imp__NetDll_WSASetEvent, a), 1u);
        CHECK_EQ(wait1(a, 0), 0u);
        CHECK_EQ(wait1(a, 0), 0u);  // manual reset: a wait does not consume it
        reset(a);
        CHECK_EQ(wait1(a, 0), kWaitTimeout);

        CHECK_EQ(call(__imp__NetDll_WSASetEvent, b), 1u);
        CHECK_EQ(wait_events({a, b}, false, 0), 1u);  // WSA_WAIT_EVENT_0 + 1
        CHECK_EQ(wait_events({a, b}, true, 0), kWaitTimeout);
        CHECK_EQ(call(__imp__NetDll_WSASetEvent, a), 1u);
        CHECK_EQ(wait_events({a, b}, true, 0), 0u);
        reset(a);
        reset(b);

        // A finite timeout really waits.
        auto started = std::chrono::steady_clock::now();
        CHECK_EQ(wait1(a, 60), kWaitTimeout);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        CHECK(elapsed.count() >= 50);

        // An infinite wait ends when another thread sets the event.
        std::thread setter([a] {
            t_sp = kStackChild;
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            CHECK_EQ(call(__imp__NetDll_WSASetEvent, a), 1u);
        });
        started = std::chrono::steady_clock::now();
        CHECK_EQ(wait1(a, kInfinite), 0u);
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        CHECK(elapsed.count() >= 30);
        setter.join();

        // Invalid parameters and handles: WSA_WAIT_FAILED / FALSE with the converted error.
        CHECK_EQ(call(__imp__NetDll_WSAWaitForMultipleEvents, 0, kEvArr, 0, 0, 0), kWaitFailed);
        CHECK_EQ(last_error(), kErrorInvalidParameter);
        CHECK_EQ(call(__imp__NetDll_WSAWaitForMultipleEvents, 65, kEvArr, 0, 0, 0), kWaitFailed);
        CHECK_EQ(last_error(), kErrorInvalidParameter);
        CHECK_EQ(wait_events({a, kBogusHandle}, false, 0), kWaitFailed);
        CHECK_EQ(last_error(), kErrorInvalidHandle);
        CHECK_EQ(call(__imp__NetDll_WSASetEvent, kBogusHandle), 0u);
        CHECK_EQ(last_error(), kErrorInvalidHandle);
        CHECK_EQ(call(__imp__NetDll_WSAResetEvent, kBogusHandle), 0u);
        CHECK_EQ(last_error(), kErrorInvalidHandle);
        CHECK_EQ(call(__imp__NetDll_WSACloseEvent, kBogusHandle), 0u);
        CHECK_EQ(last_error(), kErrorInvalidHandle);

        CHECK_EQ(call(__imp__NetDll_WSACloseEvent, a), 1u);
        CHECK_EQ(call(__imp__NetDll_WSASetEvent, a), 0u);  // closed handle
        CHECK_EQ(last_error(), kErrorInvalidHandle);
        CHECK_EQ(call(__imp__NetDll_WSACloseEvent, a), 0u);
        CHECK_EQ(last_error(), kErrorInvalidHandle);
        CHECK_EQ(call(__imp__NetDll_WSACloseEvent, b), 1u);
    }

    // ---- __WSAFDIsSet --------------------------------------------------------------------------------
    {
        wr32(kFdSet, 3);
        wr32(kFdSet + 4, 0x100);
        wr32(kFdSet + 8, 0x200);
        wr32(kFdSet + 12, 0x300);
        wr32(kFdSet + 16, 0x400);  // beyond fd_count
        CHECK_EQ(call(__imp__NetDll___WSAFDIsSet, 0x200, kFdSet), 1u);
        CHECK_EQ(call(__imp__NetDll___WSAFDIsSet, 0x300, kFdSet), 1u);
        CHECK_EQ(call(__imp__NetDll___WSAFDIsSet, 0x400, kFdSet), 0u);
        CHECK_EQ(call(__imp__NetDll___WSAFDIsSet, 0x500, kFdSet), 0u);
        wr32(kFdSet, 0);
        CHECK_EQ(call(__imp__NetDll___WSAFDIsSet, 0x100, kFdSet), 0u);
        // fd_count above FD_SETSIZE is bounded to the 64 entries of the structure.
        for (uint32_t i = 0; i < 100; ++i) wr32(kFdSet + 4 + 4 * i, 0x1000 + i);
        wr32(kFdSet, 100);
        CHECK_EQ(call(__imp__NetDll___WSAFDIsSet, 0x1000 + 63, kFdSet), 1u);
        CHECK_EQ(call(__imp__NetDll___WSAFDIsSet, 0x1000 + 64, kFdSet), 0u);
        CHECK_EQ(call(__imp__NetDll___WSAFDIsSet, 0x1000 + 99, kFdSet), 0u);
        bool fatal = false;
        CAPTURE_FATAL(call(__imp__NetDll___WSAFDIsSet, 0x100, kBad), fatal);
        CHECK(fatal);
        CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_GUEST_ACCESS);
    }

    // ---- XNetRandom ----------------------------------------------------------------------------------
    {
        memset(g_base + kRandom, 0, 0x100);
        CHECK_EQ(call(__imp__NetDll_XNetRandom, 1, kRandom, 64), 0u);
        CHECK_EQ(call(__imp__NetDll_XNetRandom, 1, kRandom + 64, 64), 0u);
        CHECK(memcmp(g_base + kRandom, g_base + kRandom + 64, 64) != 0);
        bool all_zero = true, all_same = true;
        for (uint32_t i = 0; i < 128; ++i) {
            all_zero &= g_base[kRandom + i] == 0;
            all_same &= g_base[kRandom + i] == g_base[kRandom];
        }
        CHECK(!all_zero);
        CHECK(!all_same);
        // Exactly cb bytes are written.
        memset(g_base + kRandom + 0x80, 0xA5, 0x20);
        CHECK_EQ(call(__imp__NetDll_XNetRandom, 1, kRandom + 0x80, 5), 0u);
        for (uint32_t i = 5; i < 0x20; ++i) CHECK_EQ(g_base[kRandom + 0x80 + i], 0xA5u);
        CHECK_EQ(call(__imp__NetDll_XNetRandom, 1, 0, 0), 0u);
        CHECK_EQ(call(__imp__NetDll_XNetRandom, 1, kBad, 16), kWsaEfault);
        CHECK_EQ(call(__imp__NetDll_XNetRandom, 1, 0, 16), kWsaEfault);
    }

    // ---- XNetInAddrToString --------------------------------------------------------------------------
    {
        memset(g_base + kText, 0x7E, 32);
        CHECK_EQ(call(__imp__NetDll_XNetInAddrToString, 1, 0x7F000001u, kText, 16), 0u);
        CHECK(strcmp(reinterpret_cast<char*>(g_base + kText), "127.0.0.1") == 0);
        CHECK_EQ(call(__imp__NetDll_XNetInAddrToString, 1, 0x7F000001u, kText, 10), 0u);
        CHECK_EQ(call(__imp__NetDll_XNetInAddrToString, 1, 0x7F000001u, kText, 9), kWsaEfault);
        CHECK_EQ(call(__imp__NetDll_XNetInAddrToString, 1, 0xC0A80164u, kText, 14), 0u);
        CHECK(strcmp(reinterpret_cast<char*>(g_base + kText), "192.168.1.100") == 0);
        CHECK_EQ(call(__imp__NetDll_XNetInAddrToString, 1, 0xC0A80164u, kText, 13), kWsaEfault);
        CHECK_EQ(call(__imp__NetDll_XNetInAddrToString, 1, 0x7F000001u, kBad, 16), kWsaEfault);
        CHECK_EQ(call(__imp__NetDll_XNetInAddrToString, 1, 0x7F000001u, kText, 0xFFFFFFFFu), kWsaEfault);
    }

    // ---- WSAStartup ----------------------------------------------------------------------------------
    CHECK_EQ(call(__imp__NetDll_WSAStartup, 0, 0x0202, kWsadata), 0u);

    // ---- XNet keys: the table follows XNetStartup's cfgKeyRegMax and is forgotten by XNetCleanup -----
    {
        auto key_id = [](uint32_t at, uint8_t fill) { memset(g_base + at, fill, 8); };
        // WSAStartup alone is not XNetStartup.
        key_id(kKeyId, 1);
        CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kKeyId, kKey), kWsaNotInitialised);
        CHECK_EQ(call(__imp__NetDll_XNetStartup, 1, 0), 0u);
        CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kKeyId, kKey), 0u);
        CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kKeyId, kKey), kWsaEalready);
        for (uint8_t id = 2; id <= 4; ++id) {
            key_id(kKeyId, id);
            CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kKeyId, kKey), 0u);
        }
        key_id(kKeyId, 5);
        CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kKeyId, kKey), kWsaEnobufs);  // default 4 pairs
        CHECK_EQ(call(__imp__NetDll_XNetUnregisterKey, 1, kKeyId), kWsaEinval);
        key_id(kKeyId, 1);
        CHECK_EQ(call(__imp__NetDll_XNetUnregisterKey, 1, kKeyId), 0u);
        CHECK_EQ(call(__imp__NetDll_XNetUnregisterKey, 1, kKeyId), kWsaEinval);
        key_id(kKeyId, 5);
        CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kKeyId, kKey), 0u);
        CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kBad, kKey), kWsaEfault);
        CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kKeyId, kBad), kWsaEfault);
        CHECK_EQ(call(__imp__NetDll_XNetUnregisterKey, 1, 0), kWsaEfault);
        CHECK_EQ(call(__imp__NetDll_XNetCleanup, 1), 0u);

        // XNetStartupParams {cfgSizeOfStruct = 13, ..., cfgKeyRegMax (byte 6) = 2}.
        memset(g_base + kText, 0, 13);
        g_base[kText + 0] = 13;
        g_base[kText + 6] = 2;
        CHECK_EQ(call(__imp__NetDll_XNetStartup, 1, kText), 0u);
        key_id(kKeyId, 1);  // registered before XNetCleanup: forgotten since
        CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kKeyId, kKey), 0u);
        key_id(kKeyId, 2);
        CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kKeyId, kKey), 0u);
        key_id(kKeyId, 3);
        CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kKeyId, kKey), kWsaEnobufs);
        CHECK_EQ(call(__imp__NetDll_XNetCleanup, 1), 0u);
        CHECK_EQ(call(__imp__NetDll_XNetRegisterKey, 1, kKeyId, kKey), kWsaNotInitialised);
    }

    // ---- XNetDnsLookup / XNetDnsRelease: no link, the lookup fails with WSAENETDOWN ------------------
    uint32_t leaked_dns = 0;
    {
        const uint32_t ev = create_event();
        put(kHost, "example.com");
        wr32(kDnsOut, 0);
        CHECK_EQ(call(__imp__NetDll_XNetDnsLookup, 1, kHost, ev, kDnsOut), 0u);
        const uint32_t block = rd32(kDnsOut);
        CHECK(block != 0);
        CHECK_EQ(rd32(block + 0), kWsaEnetdown);  // iStatus
        CHECK_EQ(rd32(block + 4), 0u);            // cina
        CHECK_EQ(wait1(ev, 0), 0u);
        CHECK_EQ(call(__imp__NetDll_XNetDnsRelease, 1, block), 0u);
        CHECK_EQ(call(__imp__NetDll_XNetDnsRelease, 1, block), kWsaEinval);
        CHECK_EQ(call(__imp__NetDll_XNetDnsRelease, 1, kText), kWsaEinval);
        // No event is fine; this block is left for title teardown to free.
        CHECK_EQ(call(__imp__NetDll_XNetDnsLookup, 1, kHost, 0, kDnsOut), 0u);
        leaked_dns = rd32(kDnsOut);
        CHECK(leaked_dns != 0);  // may reuse the released block
        CHECK_EQ(rd32(leaked_dns + 0), kWsaEnetdown);
        CHECK_EQ(call(__imp__NetDll_XNetDnsLookup, 1, kHost, kBogusHandle, kDnsOut), kWsaEinval);
        CHECK_EQ(call(__imp__NetDll_XNetDnsLookup, 1, kHost, ev, kBad), kWsaEfault);
        CHECK_EQ(call(__imp__NetDll_XNetDnsLookup, 1, kBad, ev, kDnsOut), kWsaEfault);
        CHECK_EQ(call(__imp__NetDll_XNetDnsLookup, 1, 0, ev, kDnsOut), kWsaEfault);
        CHECK_EQ(call(__imp__NetDll_WSACloseEvent, ev), 1u);
    }

    // ---- TCP loopback pair -----------------------------------------------------------------------------
    const uint32_t listener = tcp();
    const uint16_t tcp_port = bind_loopback(listener);
    CHECK_EQ(call(__imp__NetDll_listen, 0, listener, 8), 0u);
    const uint32_t client = tcp();
    CHECK_EQ(connect_to(client, tcp_port), 0u);
    const uint32_t acc = accept_one(listener);
    CHECK(acc != kSocketError);
    const uint32_t ev1 = create_event(), ev2 = create_event(), ev3 = create_event();

    // Immediate overlapped gather send and scatter receive: return 0, overlapped and event completed.
    {
        memcpy(g_base + kData, "hel", 3);
        memcpy(g_base + kData2, "lo!", 3);
        wsabuf(kWsabuf, 0, 3, kData);
        wsabuf(kWsabuf, 1, 3, kData2);
        overlapped(kOv1, ev1);
        wr32(kBytes, 0xCCCCCCCC);
        CHECK_EQ(wsa_send(client, kWsabuf, 2, kBytes, 0, kOv1), 0u);
        CHECK_EQ(rd32(kBytes), 6u);
        CHECK_EQ(rd32(kOv1 + 0), 0u);
        CHECK_EQ(rd32(kOv1 + 4), 6u);
        CHECK_EQ(wait1(ev1, 0), 0u);

        CHECK(readable(acc, 2000));
        memset(g_base + kRecv, 0, 16);
        memset(g_base + kRecv2, 0, 16);
        wsabuf(kWsabuf2, 0, 4, kRecv);
        wsabuf(kWsabuf2, 1, 4, kRecv2);
        overlapped(kOv2, ev2);
        wr32(kFlags, 0);
        wr32(kBytes, 0xCCCCCCCC);
        CHECK_EQ(wsa_recv(acc, kWsabuf2, 2, kBytes, kFlags, kOv2), 0u);
        CHECK_EQ(rd32(kBytes), 6u);
        CHECK_EQ(rd32(kFlags), 0u);
        CHECK_EQ(rd32(kOv2 + 0), 0u);
        CHECK_EQ(rd32(kOv2 + 4), 6u);
        CHECK(same(kRecv, "hell"));
        CHECK(same(kRecv2, "o!"));
        CHECK_EQ(wait1(ev2, 0), 0u);
        wr32(kBytes2, 0);
        CHECK_EQ(get_result(acc, kOv2, kBytes2, 0, kFlags2), 1u);
        CHECK_EQ(rd32(kBytes2), 6u);
    }

    // Pending overlapped receive: WSA_IO_PENDING, WSA_IO_INCOMPLETE, then completion by the worker.
    {
        wsabuf(kWsabuf2, 0, 16, kRecv);
        overlapped(kOv1, ev1);
        CHECK_EQ(call(__imp__NetDll_WSASetEvent, ev1), 1u);  // the request resets its event
        wr32(kFlags, 0);
        expect_pending(wsa_recv(acc, kWsabuf2, 1, kBytes, kFlags, kOv1), kOv1);
        CHECK_EQ(wait1(ev1, 0), kWaitTimeout);
        CHECK_EQ(get_result(acc, kOv1, kBytes2, 0, kFlags2), 0u);
        CHECK_EQ(last_error(), kIoIncomplete);
        memcpy(g_base + kData, "world", 5);
        CHECK_EQ(send_plain(client, kData, 5), 5u);
        CHECK_EQ(wait1(ev1, 2000), 0u);
        wr32(kBytes2, 0);
        wr32(kFlags2, 0xFFFF);
        CHECK_EQ(get_result(acc, kOv1, kBytes2, 0, kFlags2), 1u);
        CHECK_EQ(rd32(kBytes2), 5u);
        CHECK_EQ(rd32(kFlags2), 0u);
        CHECK_EQ(rd32(kOv1 + 0), 0u);
        CHECK_EQ(rd32(kOv1 + 4), 5u);
        CHECK(same(kRecv, "world"));
    }

    // WSAGetOverlappedResult(fWait = TRUE) blocks until another thread's send completes the request;
    // a request without an event is allowed.
    {
        wsabuf(kWsabuf2, 0, 16, kRecv3);
        overlapped(kOv2, 0);
        wr32(kFlags, 0);
        expect_pending(wsa_recv(acc, kWsabuf2, 1, kBytes, kFlags, kOv2), kOv2);
        memcpy(g_base + kData2, "late", 4);
        std::thread sender([client] {
            t_sp = kStackChild;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CHECK_EQ(send_plain(client, kData2, 4), 4u);
        });
        wr32(kBytes2, 0);
        CHECK_EQ(get_result(acc, kOv2, kBytes2, 1, kFlags2), 1u);
        CHECK_EQ(rd32(kBytes2), 4u);
        CHECK(same(kRecv3, "late"));
        sender.join();
    }

    // WSACancelOverlappedIO: every queued request of the socket completes with WSA_OPERATION_ABORTED and
    // consumes no data.
    {
        wsabuf(kWsabuf2, 0, 16, kRecv);
        wsabuf(kWsabuf3, 0, 16, kRecv2);
        overlapped(kOv1, ev1);
        overlapped(kOv2, ev2);
        wr32(kFlags, 0);
        wr32(kFlags2, 0);
        expect_pending(wsa_recv(acc, kWsabuf2, 1, kBytes, kFlags, kOv1), kOv1);
        expect_pending(wsa_recv(acc, kWsabuf3, 1, kBytes2, kFlags2, kOv2), kOv2);
        CHECK_EQ(call(__imp__NetDll_WSACancelOverlappedIO, 1, acc), 0u);
        expect_aborted(kOv1, ev1);
        expect_aborted(kOv2, ev2);
        CHECK_EQ(get_result(acc, kOv1, kBytes2, 0, kFlags2), 0u);
        CHECK_EQ(last_error(), kAborted);
        CHECK_EQ(get_result(acc, kOv2, kBytes2, 1, kFlags2), 0u);
        CHECK_EQ(last_error(), kAborted);
        // Nothing to cancel is still a success.
        CHECK_EQ(call(__imp__NetDll_WSACancelOverlappedIO, 1, acc), 0u);
        CHECK_EQ(call(__imp__NetDll_WSACancelOverlappedIO, 1, 0x12345678u), kSocketError);
        CHECK_EQ(last_error(), kWsaEnotsock);
        memcpy(g_base + kData, "x", 1);
        CHECK_EQ(send_plain(client, kData, 1), 1u);
        CHECK_EQ(recv_plain(acc, kRecv, 16), 1u);
        CHECK(same(kRecv, "x"));
    }

    // Error paths of the transfer calls.
    {
        wsabuf(kWsabuf2, 0, 16, kRecv);
        wr32(kFlags, 0);
        overlapped(kOv3, ev3);
        CHECK_EQ(wsa_recv(0x12345678u, kWsabuf2, 1, kBytes, kFlags, 0), kSocketError);
        CHECK_EQ(last_error(), kWsaEnotsock);
        CHECK_EQ(wsa_send(0x12345678u, kWsabuf2, 1, kBytes, 0, kOv3), kSocketError);
        CHECK_EQ(last_error(), kWsaEnotsock);
        CHECK_EQ(wsa_recv(acc, kWsabuf2, 1, 0, kFlags, 0), kSocketError);  // neither bytes nor overlapped
        CHECK_EQ(last_error(), kWsaEfault);
        CHECK_EQ(wsa_recv(acc, kWsabuf2, 1, kBytes, 0, kOv3), kSocketError);  // lpFlags NULL
        CHECK_EQ(last_error(), kWsaEfault);
        CHECK_EQ(wsa_recv(acc, kBad, 1, kBytes, kFlags, kOv3), kSocketError);  // WSABUF array
        CHECK_EQ(last_error(), kWsaEfault);
        CHECK_EQ(wsa_recv(acc, 0, 1, kBytes, kFlags, kOv3), kSocketError);
        CHECK_EQ(last_error(), kWsaEfault);
        wsabuf(kWsabuf3, 0, 16, kBad);
        CHECK_EQ(wsa_recv(acc, kWsabuf3, 1, kBytes, kFlags, kOv3), kSocketError);  // buffer
        CHECK_EQ(last_error(), kWsaEfault);
        CHECK_EQ(wsa_send(client, kWsabuf3, 1, kBytes, 0, kOv3), kSocketError);
        CHECK_EQ(last_error(), kWsaEfault);
        CHECK_EQ(wsa_recv(acc, kWsabuf2, 1, kBytes, kFlags, kBad), kSocketError);  // overlapped
        CHECK_EQ(last_error(), kWsaEfault);
        CHECK_EQ(wsa_recv(acc, kWsabuf2, 1, kBad, kFlags, kOv3), kSocketError);  // bytes
        CHECK_EQ(last_error(), kWsaEfault);
        overlapped(kOv3, kBogusHandle);
        CHECK_EQ(wsa_recv(acc, kWsabuf2, 1, kBytes, kFlags, kOv3), kSocketError);
        CHECK_EQ(last_error(), kErrorInvalidHandle);
        overlapped(kOv3, ev3);
        wr32(kFlags, 0x8000);
        CHECK_EQ(wsa_recv(acc, kWsabuf2, 1, kBytes, kFlags, kOv3), kSocketError);
        CHECK_EQ(last_error(), kWsaEinval);
        wr32(kFlags, 0);
        CHECK_EQ(wsa_send(client, kWsabuf2, 1, kBytes, 2 /* MSG_PEEK */, kOv3), kSocketError);
        CHECK_EQ(last_error(), kWsaEinval);
        CHECK_EQ(wsa_recv(acc, kWsabuf2, 1025, kBytes, kFlags, kOv3), kSocketError);
        CHECK_EQ(last_error(), kWsaEnobufs);
        // Completion routines need APC delivery, which no guest thread has: a fatal, never a success.
        bool fatal = false;
        CAPTURE_FATAL(wsa_recv(acc, kWsabuf2, 0, kBytes, kFlags, kOv3, 0x82001000u), fatal);
        CHECK(fatal);
        CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_UNIMPLEMENTED);
        // WSAGetOverlappedResult.
        CHECK_EQ(get_result(acc, kBad, kBytes2, 0, kFlags2), 0u);
        CHECK_EQ(last_error(), kWsaEfault);
        CHECK_EQ(get_result(acc, kOv1, 0, 0, kFlags2), 0u);
        CHECK_EQ(last_error(), kWsaEfault);
        CHECK_EQ(get_result(acc, kOv1, kBytes2, 0, 0), 0u);
        CHECK_EQ(last_error(), kWsaEfault);
        CHECK_EQ(get_result(0x12345678u, kOv1, kBytes2, 0, kFlags2), 0u);
        CHECK_EQ(last_error(), kWsaEnotsock);
        wr32(kOv3 + 0, kStatusPending);  // never submitted on this socket
        CHECK_EQ(get_result(acc, kOv3, kBytes2, 0, kFlags2), 0u);
        CHECK_EQ(last_error(), kIoIncomplete);
        // Non-overlapped WSARecv/WSASend behave as recv/send.
        memcpy(g_base + kData, "sync", 4);
        wsabuf(kWsabuf, 0, 4, kData);
        CHECK_EQ(wsa_send(client, kWsabuf, 1, kBytes, 0, 0), 0u);
        CHECK_EQ(rd32(kBytes), 4u);
        wr32(kFlags, 0);
        CHECK_EQ(wsa_recv(acc, kWsabuf2, 1, kBytes, kFlags, 0), 0u);
        CHECK_EQ(rd32(kBytes), 4u);
        CHECK(same(kRecv, "sync"));
    }

    // A pending overlapped stream send completes only when the whole buffer is transferred.
    {
        set_int_option(client, 0x1001, 8192);  // SO_SNDBUF
        set_int_option(acc, 0x1002, 8192);     // SO_RCVBUF
        set_nonblocking(client, 1);
        const uint32_t filled = fill_send_buffer(client);
        CHECK(filled > 0);
        for (uint32_t i = 0; i < 0x8000; ++i) g_base[kBig + i] = static_cast<uint8_t>(i * 7);
        wsabuf(kWsabuf, 0, 0x8000, kBig);
        overlapped(kOv3, ev3);
        expect_pending(wsa_send(client, kWsabuf, 1, kBytes, 0, kOv3), kOv3);
        CHECK_EQ(get_result(client, kOv3, kBytes2, 0, kFlags2), 0u);
        CHECK_EQ(last_error(), kIoIncomplete);
        uint32_t received = 0, sent = 0;
        bool complete = false;
        for (int i = 0; i < 100000; ++i) {
            if (!complete && get_result(client, kOv3, kBytes2, 0, kFlags2) == 1) {
                complete = true;
                sent = rd32(kBytes2);
            }
            if (complete && received >= filled + sent) break;
            if (!readable(acc, 1000)) break;
            const uint32_t n = recv_plain(acc, kSink, 0x8000);
            if (n == kSocketError || n == 0) break;
            received += n;
        }
        // The peer can read the last bytes before the worker publishes the completion.
        if (!complete && get_result(client, kOv3, kBytes2, 1, kFlags2) == 1) {
            complete = true;
            sent = rd32(kBytes2);
            received += drain(acc, 300);
        }
        CHECK(complete);
        CHECK_EQ(sent, 0x8000u);
        CHECK_EQ(rd32(kOv3 + 4), 0x8000u);
        CHECK_EQ(received, filled + sent);
        CHECK_EQ(wait1(ev3, 0), 0u);
        set_nonblocking(client, 0);
    }

    // ---- UDP: overlapped WSARecvFrom / WSASendTo (stack arguments) --------------------------------------
    const uint32_t us = udp(), uc = udp();
    const uint16_t us_port = bind_loopback(us);
    const uint16_t uc_port = bind_loopback(uc);
    {
        wsabuf(kWsabuf2, 0, 32, kRecv);
        overlapped(kOv1, ev1);
        wr32(kFlags, 0);
        memset(g_base + kFrom, 0, 16);
        wr32(kFromLen, 16);
        expect_pending(wsa_recvfrom(us, kWsabuf2, 1, kBytes, kFlags, kFrom, kFromLen, kOv1), kOv1);
        CHECK_EQ(wait1(ev1, 0), kWaitTimeout);

        memcpy(g_base + kData, "datagram", 8);
        wsabuf(kWsabuf, 0, 8, kData);
        sockaddr4(kAddr2, 0x7F000001u, us_port);
        overlapped(kOv2, ev2);
        wr32(kBytes2, 0);
        CHECK_EQ(wsa_sendto(uc, kWsabuf, 1, kBytes2, 0, kAddr2, 16, kOv2), 0u);
        CHECK_EQ(rd32(kBytes2), 8u);
        CHECK_EQ(rd32(kOv2 + 0), 0u);
        CHECK_EQ(rd32(kOv2 + 4), 8u);
        CHECK_EQ(wait1(ev2, 0), 0u);

        CHECK_EQ(wait1(ev1, 2000), 0u);
        CHECK_EQ(get_result(us, kOv1, kBytes2, 0, kFlags2), 1u);
        CHECK_EQ(rd32(kBytes2), 8u);
        CHECK(same(kRecv, "datagram"));
        CHECK_EQ(rd16(kFrom), 2u);
        CHECK_EQ(rd16(kFrom + 2), uc_port);
        CHECK_EQ(rd32(kFrom + 4), 0x7F000001u);
        CHECK_EQ(rd32(kFromLen), 16u);

        // Non-overlapped scatter receive of one datagram into two buffers.
        memcpy(g_base + kData, "0123456789", 10);
        wsabuf(kWsabuf, 0, 10, kData);
        CHECK_EQ(wsa_sendto(uc, kWsabuf, 1, kBytes2, 0, kAddr2, 16, 0), 0u);
        CHECK(readable(us, 2000));
        memset(g_base + kRecv, 0, 16);
        memset(g_base + kRecv2, 0, 16);
        wsabuf(kWsabuf3, 0, 4, kRecv);
        wsabuf(kWsabuf3, 1, 12, kRecv2);
        wr32(kFromLen, 16);
        CHECK_EQ(wsa_recvfrom(us, kWsabuf3, 2, kBytes, kFlags, kFrom, kFromLen, 0), 0u);
        CHECK_EQ(rd32(kBytes), 10u);
        CHECK(same(kRecv, "0123"));
        CHECK(same(kRecv2, "456789"));
        CHECK_EQ(rd16(kFrom + 2), uc_port);

        // Address and argument errors.
        CHECK_EQ(wsa_sendto(uc, kWsabuf, 1, kBytes2, 0, kAddr2, 8, 0), kSocketError);
        CHECK_EQ(last_error(), kWsaEfault);
        CHECK_EQ(wsa_sendto(uc, kWsabuf, 1, kBytes2, 0, kBad, 16, 0), kSocketError);
        CHECK_EQ(last_error(), kWsaEfault);
        sockaddr4(kAddr, 0x7F000001u, us_port, 23);
        CHECK_EQ(wsa_sendto(uc, kWsabuf, 1, kBytes2, 0, kAddr, 16, 0), kSocketError);
        CHECK_EQ(last_error(), kWsaEafnosupport);
        CHECK_EQ(wsa_recvfrom(us, kWsabuf3, 2, kBytes, kFlags, kFrom, 0, 0), kSocketError);
        CHECK_EQ(last_error(), kWsaEfault);
        wr32(kFromLen, 8);
        CHECK_EQ(wsa_recvfrom(us, kWsabuf3, 2, kBytes, kFlags, kFrom, kFromLen, 0), kSocketError);
        CHECK_EQ(last_error(), kWsaEfault);
        CHECK_EQ(wsa_recvfrom(0x12345678u, kWsabuf3, 2, kBytes, kFlags, 0, 0, kOv1), kSocketError);
        CHECK_EQ(last_error(), kWsaEnotsock);
        bool fatal = false;
        CAPTURE_FATAL(wsa_sendto(uc, kWsabuf, 0, kBytes2, 0, kAddr2, 16, kOv2, 0x82001000u), fatal);
        CHECK(fatal);
        CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_UNIMPLEMENTED);
    }

    // ---- WSAEventSelect ----------------------------------------------------------------------------------
    const uint32_t evA = create_event(), evR = create_event(), evW = create_event(), evC = create_event(),
                   evK = create_event(), evU = create_event();
    const uint32_t l2 = tcp();
    const uint16_t l2_port = bind_loopback(l2);
    CHECK_EQ(call(__imp__NetDll_listen, 0, l2, 8), 0u);
    uint32_t c1 = 0, a1 = 0;
    {
        // FD_ACCEPT: recorded once, re-enabled by accept().
        CHECK_EQ(event_select(l2, evA, kFdAccept), 0u);
        CHECK_EQ(wait1(evA, 0), kWaitTimeout);
        wr32(kOpt, 0);  // the socket stays nonblocking while events are selected
        CHECK_EQ(call(__imp__NetDll_ioctlsocket, 0, l2, kFionbio, kOpt), kSocketError);
        CHECK_EQ(last_error(), kWsaEinval);
        c1 = tcp();
        CHECK_EQ(connect_to(c1, l2_port), 0u);
        CHECK_EQ(wait1(evA, 2000), 0u);
        reset(evA);
        const uint32_t c2 = tcp();
        CHECK_EQ(connect_to(c2, l2_port), 0u);
        CHECK_EQ(wait1(evA, 150), kWaitTimeout);  // not re-enabled yet
        a1 = accept_one(l2);
        CHECK(a1 != kSocketError);
        CHECK_EQ(wait1(evA, 1000), 0u);  // accept re-enabled it and c2 is pending
        reset(evA);
        const uint32_t a2 = accept_one(l2);
        CHECK(a2 != kSocketError);
        CHECK_EQ(wait1(evA, 150), kWaitTimeout);
        CHECK_EQ(accept_one(l2), kSocketError);  // nonblocking listener, nothing pending
        CHECK_EQ(last_error(), kWsaEwouldblock);
        // Clearing the selection allows blocking mode again.
        CHECK_EQ(event_select(l2, 0, 0), 0u);
        set_nonblocking(l2, 0);
        CHECK_EQ(call(__imp__NetDll_closesocket, 0, a2), 0u);
        CHECK_EQ(call(__imp__NetDll_closesocket, 0, c2), 0u);
    }
    {
        // FD_READ: recorded once, re-enabled by a receive; data still queued re-records at once.
        CHECK_EQ(event_select(a1, evR, kFdRead), 0u);
        CHECK_EQ(wait1(evR, 0), kWaitTimeout);
        CHECK_EQ(recv_plain(a1, kRecv, 16), kSocketError);  // WSAEventSelect made it nonblocking
        CHECK_EQ(last_error(), kWsaEwouldblock);
        memcpy(g_base + kData, "abcdef", 6);
        CHECK_EQ(send_plain(c1, kData, 3), 3u);
        CHECK_EQ(wait1(evR, 2000), 0u);
        reset(evR);
        CHECK_EQ(send_plain(c1, kData + 3, 3), 3u);
        CHECK_EQ(wait1(evR, 150), kWaitTimeout);
        CHECK_EQ(recv_plain(a1, kRecv, 2), 2u);
        CHECK_EQ(wait1(evR, 1000), 0u);
        reset(evR);
        CHECK_EQ(recv_plain(a1, kRecv + 2, 16), 4u);
        CHECK(same(kRecv, "abcdef"));
        CHECK_EQ(wait1(evR, 150), kWaitTimeout);
    }
    {
        // FD_WRITE: recorded at selection on a connected socket, then only after a send that failed
        // with WSAEWOULDBLOCK once the socket can send again.
        CHECK_EQ(event_select(c1, evW, kFdWrite), 0u);
        CHECK_EQ(wait1(evW, 0), 0u);
        reset(evW);
        CHECK_EQ(wait1(evW, 150), kWaitTimeout);
        set_int_option(c1, 0x1001, 8192);
        const uint32_t filled = fill_send_buffer(c1);
        CHECK(filled > 0);
        // Plain writability did not re-record FD_WRITE above; the WSAEWOULDBLOCK sends re-enabled it, so
        // it is recorded once the peer drains (or already was, if space reappeared during the fill).
        CHECK_EQ(drain(a1, 300), filled);
        CHECK_EQ(wait1(evW, 2000), 0u);
        reset(evW);
        CHECK_EQ(wait1(evW, 150), kWaitTimeout);
    }
    {
        // FD_CLOSE: recorded once when the peer closes; the end of stream does not re-record it.
        reset(evR);  // the drain above re-enabled and re-recorded FD_READ on the previous association
        CHECK_EQ(event_select(a1, evC, kFdRead | kFdClose), 0u);
        CHECK_EQ(wait1(evC, 0), kWaitTimeout);
        CHECK_EQ(call(__imp__NetDll_closesocket, 0, c1), 0u);
        CHECK_EQ(wait1(evC, 2000), 0u);
        reset(evC);
        CHECK_EQ(wait1(evC, 150), kWaitTimeout);
        CHECK_EQ(recv_plain(a1, kRecv, 16), 0u);  // end of stream
        CHECK_EQ(wait1(evC, 150), kWaitTimeout);
        CHECK_EQ(wait1(evR, 0), kWaitTimeout);  // the replaced association no longer signals evR
    }
    {
        // FD_CONNECT: a nonblocking connect reports WSAEWOULDBLOCK, then FD_CONNECT is recorded once.
        const uint32_t c3 = tcp();
        CHECK_EQ(event_select(c3, evK, kFdConnect), 0u);
        CHECK_EQ(wait1(evK, 0), kWaitTimeout);
        const uint32_t r = connect_to(c3, l2_port);
        if (r == kSocketError) CHECK_EQ(last_error(), kWsaEwouldblock);
        else CHECK_EQ(r, 0u);
        CHECK_EQ(wait1(evK, 2000), 0u);
        reset(evK);
        CHECK_EQ(wait1(evK, 150), kWaitTimeout);
        const uint32_t a3 = accept_one(l2);
        CHECK(a3 != kSocketError);
        CHECK_EQ(call(__imp__NetDll_closesocket, 0, a3), 0u);
        CHECK_EQ(call(__imp__NetDll_closesocket, 0, c3), 0u);
    }
    {
        // Datagram FD_READ and FD_WRITE.
        CHECK_EQ(event_select(us, evU, kFdRead), 0u);
        CHECK_EQ(wait1(evU, 0), kWaitTimeout);
        memcpy(g_base + kData, "ping", 4);
        sockaddr4(kAddr2, 0x7F000001u, us_port);
        CHECK_EQ(call(__imp__NetDll_sendto, 0, uc, kData, 4, 0, kAddr2, 16), 4u);
        CHECK_EQ(wait1(evU, 2000), 0u);
        reset(evU);
        CHECK_EQ(recv_plain(us, kRecv, 16), 4u);
        CHECK_EQ(wait1(evU, 150), kWaitTimeout);
        CHECK_EQ(event_select(uc, evU, kFdWrite), 0u);
        CHECK_EQ(wait1(evU, 0), 0u);
        CHECK_EQ(event_select(uc, 0, 0), 0u);
        reset(evU);
    }
    {
        // Selection errors.
        CHECK_EQ(event_select(0x12345678u, evU, kFdRead), kSocketError);
        CHECK_EQ(last_error(), kWsaEnotsock);
        CHECK_EQ(event_select(us, evU, 0x40), kSocketError);
        CHECK_EQ(last_error(), kWsaEinval);
        CHECK_EQ(event_select(us, kBogusHandle, kFdRead), kSocketError);
        CHECK_EQ(last_error(), kWsaEinval);
    }

    // ---- closesocket with queued requests: they complete aborted, the handle is gone -----------------
    {
        wsabuf(kWsabuf2, 0, 16, kRecv);
        overlapped(kOv1, ev1);
        wr32(kFlags, 0);
        expect_pending(wsa_recv(acc, kWsabuf2, 1, kBytes, kFlags, kOv1), kOv1);
        CHECK_EQ(call(__imp__NetDll_closesocket, 0, acc), 0u);
        expect_aborted(kOv1, ev1);
        CHECK_EQ(wsa_recv(acc, kWsabuf2, 1, kBytes, kFlags, kOv1), kSocketError);
        CHECK_EQ(last_error(), kWsaEnotsock);
        CHECK_EQ(get_result(acc, kOv1, kBytes2, 0, kFlags2), 0u);
        CHECK_EQ(last_error(), kWsaEnotsock);
        CHECK_EQ(call(__imp__NetDll_WSACancelOverlappedIO, 1, acc), kSocketError);
        CHECK_EQ(last_error(), kWsaEnotsock);
        // The peer sees the close.
        CHECK(readable(client, 2000));
        CHECK_EQ(recv_plain(client, kRecv, 16), 0u);
    }

    // ---- WSACleanup with a queued request and an active selection: no hang, aborted -------------------
    {
        const uint32_t u3 = udp();
        bind_loopback(u3);
        CHECK_EQ(event_select(u3, evU, kFdRead), 0u);
        const uint32_t u4 = udp();
        bind_loopback(u4);
        wsabuf(kWsabuf2, 0, 16, kRecv);
        overlapped(kOv2, ev2);
        wr32(kFlags, 0);
        expect_pending(wsa_recv(u4, kWsabuf2, 1, kBytes, kFlags, kOv2), kOv2);
        const auto started = std::chrono::steady_clock::now();
        CHECK_EQ(call(__imp__NetDll_WSACleanup, 0), 0u);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        CHECK(elapsed.count() < 5000);
        expect_aborted(kOv2, ev2);
        CHECK_EQ(wsa_recv(u4, kWsabuf2, 1, kBytes, kFlags, kOv2), kSocketError);
        CHECK_EQ(last_error(), kWsaNotInitialised);
        CHECK_EQ(get_result(u4, kOv2, kBytes2, 0, kFlags2), 0u);
        CHECK_EQ(last_error(), kWsaNotInitialised);
        CHECK_EQ(event_select(u3, evU, kFdRead), kSocketError);
        CHECK_EQ(last_error(), kWsaNotInitialised);
        CHECK_EQ(call(__imp__NetDll_WSACancelOverlappedIO, 1, u4), kSocketError);
        CHECK_EQ(last_error(), kWsaNotInitialised);
        CHECK_EQ(call(__imp__NetDll_WSACleanup, 0), kSocketError);
        CHECK_EQ(last_error(), kWsaNotInitialised);
        CHECK_EQ(call(__imp__NetDll_XNetDnsLookup, 1, kHost, 0, kDnsOut), kWsaNotInitialised);
    }

    // ---- title teardown with a queued request: completes aborted, the worker is joined --------------
    {
        CHECK_EQ(call(__imp__NetDll_WSAStartup, 0, 0x0202, kWsadata), 0u);
        const uint32_t u5 = udp();
        bind_loopback(u5);
        CHECK_EQ(event_select(u5, evU, kFdRead), 0u);
        wsabuf(kWsabuf2, 0, 16, kRecv);
        overlapped(kOv3, ev3);
        wr32(kFlags, 0);
        expect_pending(wsa_recv(u5, kWsabuf2, 1, kBytes, kFlags, kOv3), kOv3);
        CHECK_ST(shutdown_xam_net_hle(), Status::Ok);
        expect_aborted(kOv3, ev3);
        // A second teardown finds nothing left.
        CHECK_ST(shutdown_xam_net_hle(), Status::Ok);
    }
    (void)leaked_dns;  // freed by the first teardown

    for (uint32_t e : {ev1, ev2, ev3, evA, evR, evW, evC, evK, evU})
        CHECK_EQ(call(__imp__NetDll_WSACloseEvent, e), 1u);
    runtime_shutdown();
    clear_imports();
    g_mem.release();
    return test_result("rt_xam_net_wsa");
}
