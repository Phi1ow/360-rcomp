// NetDll Winsock HLE over real host loopback sockets only. No external network,
// XNet identity, QoS or authentication service is involved.
#include <string.h>

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
PPC_EXTERN_FUNC(__imp__NetDll_WSASetLastError);
PPC_EXTERN_FUNC(__imp__NetDll_socket);
PPC_EXTERN_FUNC(__imp__NetDll_closesocket);
PPC_EXTERN_FUNC(__imp__NetDll_shutdown);
PPC_EXTERN_FUNC(__imp__NetDll_ioctlsocket);
PPC_EXTERN_FUNC(__imp__NetDll_setsockopt);
PPC_EXTERN_FUNC(__imp__NetDll_getsockopt);
PPC_EXTERN_FUNC(__imp__NetDll_getsockname);
PPC_EXTERN_FUNC(__imp__NetDll_getpeername);
PPC_EXTERN_FUNC(__imp__NetDll_bind);
PPC_EXTERN_FUNC(__imp__NetDll_connect);
PPC_EXTERN_FUNC(__imp__NetDll_listen);
PPC_EXTERN_FUNC(__imp__NetDll_accept);
PPC_EXTERN_FUNC(__imp__NetDll_select);
PPC_EXTERN_FUNC(__imp__NetDll_recv);
PPC_EXTERN_FUNC(__imp__NetDll_recvfrom);
PPC_EXTERN_FUNC(__imp__NetDll_send);
PPC_EXTERN_FUNC(__imp__NetDll_sendto);
PPC_EXTERN_FUNC(__imp__NetDll_inet_addr);

using namespace rcomp;
using namespace rcomp::rt;

namespace {

uint8_t* g_base = nullptr;
uint32_t g_buf = 0;

void wr16(uint32_t address, uint16_t value) {
    const uint16_t be = __builtin_bswap16(value);
    memcpy(g_base + address, &be, sizeof(be));
}

uint16_t rd16(uint32_t address) {
    uint16_t value = 0;
    CHECK(guest_read_be16(address, &value));
    return value;
}

void wr32(uint32_t address, uint32_t value) { CHECK(guest_write_be32(address, value)); }

uint32_t rd32(uint32_t address) {
    uint32_t value = 0;
    CHECK(guest_read_be32(address, &value));
    return value;
}

uint32_t call(PPCFunc* fn, uint32_t r3 = 0, uint32_t r4 = 0, uint32_t r5 = 0,
              uint32_t r6 = 0, uint32_t r7 = 0, uint32_t r8 = 0, uint32_t r9 = 0,
              uint32_t r10 = 0) {
    alignas(64) PPCContext ctx{};
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

void sockaddr4(uint32_t address, uint32_t ipv4, uint16_t port) {
    memset(g_base + address, 0, 16);
    wr16(address, 2);
    wr16(address + 2, port);
    wr32(address + 4, ipv4);
}

uint32_t socket_tcp() {
    const uint32_t s = call(__imp__NetDll_socket, 0, 2, 1, 6);
    CHECK(s != 0xFFFFFFFFu);
    return s;
}

uint32_t socket_udp() {
    const uint32_t s = call(__imp__NetDll_socket, 0, 2, 2, 17);
    CHECK(s != 0xFFFFFFFFu);
    return s;
}

uint32_t last_error() { return call(__imp__NetDll_WSAGetLastError); }

void fdset(uint32_t address, uint32_t socket) {
    wr32(address, 1);
    wr32(address + 4, socket);
}

void timeval_ms(uint32_t address, uint32_t millis) {
    wr32(address, millis / 1000);
    wr32(address + 4, (millis % 1000) * 1000);
}

}  // namespace

int main() {
    GuestMemory mem;
    if (mem.reserve() != MemStatus::Ok) return 2;
    g_base = mem.base();
    CHECK_ST(runtime_init(&mem), Status::Ok);
    clear_imports();
    CHECK_ST(register_xam_net_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x10000, 16, true, &g_buf), Status::Ok);

    constexpr uint32_t kWsadata = 0x000;
    constexpr uint32_t kAddr = 0x400;
    constexpr uint32_t kAddr2 = 0x420;
    constexpr uint32_t kAddrLen = 0x440;
    constexpr uint32_t kOpt = 0x450;
    constexpr uint32_t kOptLen = 0x454;
    constexpr uint32_t kFdRead = 0x500;
    constexpr uint32_t kTimeout = 0x720;
    constexpr uint32_t kData = 0x800;
    constexpr uint32_t kData2 = 0x900;
    constexpr uint32_t kString = 0xA00;

    // Version negotiation is direct, then the successful calls maintain a
    // real startup reference count.
    CHECK_EQ(call(__imp__NetDll_WSAStartup, 0, 0x0300, g_buf + kWsadata), 10092u);
    CHECK_EQ(call(__imp__NetDll_WSAStartup, 0, 0x0202, g_buf + kWsadata), 0u);
    CHECK_EQ(rd16(g_buf + kWsadata + 0), 0x0202u);
    CHECK_EQ(rd16(g_buf + kWsadata + 2), 0x0202u);
    CHECK_EQ(g_base[g_buf + kWsadata + 4], 0u);
    CHECK_EQ(call(__imp__NetDll_WSAStartup, 0, 0x0202, g_buf + kWsadata), 0u);
    CHECK_EQ(call(__imp__NetDll_WSACleanup, 0), 0u);  // one reference remains

    // WSA last error is host-thread local, matching the guest thread contract.
    call(__imp__NetDll_WSASetLastError, 0x1234);
    CHECK_EQ(last_error(), 0x1234u);
    uint32_t child_error = 0;
    std::thread child([&] {
        CHECK_EQ(call(__imp__NetDll_WSAGetLastError), 0u);
        call(__imp__NetDll_WSASetLastError, 0x5678);
        child_error = call(__imp__NetDll_WSAGetLastError);
    });
    child.join();
    CHECK_EQ(child_error, 0x5678u);
    CHECK_EQ(last_error(), 0x1234u);

    // inet_addr returns a guest/network-order IPv4 word and keeps the Xbox 360
    // empty-string quirk.
    strcpy(reinterpret_cast<char*>(g_base + g_buf + kString), "127.0.0.1");
    CHECK_EQ(call(__imp__NetDll_inet_addr, g_buf + kString), 0x7F000001u);
    strcpy(reinterpret_cast<char*>(g_base + g_buf + kString), "127.1");
    CHECK_EQ(call(__imp__NetDll_inet_addr, g_buf + kString), 0x7F000001u);
    strcpy(reinterpret_cast<char*>(g_base + g_buf + kString), "0x7f000001");
    CHECK_EQ(call(__imp__NetDll_inet_addr, g_buf + kString), 0x7F000001u);
    g_base[g_buf + kString] = 0;
    CHECK_EQ(call(__imp__NetDll_inet_addr, g_buf + kString), 0u);
    strcpy(reinterpret_cast<char*>(g_base + g_buf + kString), "bad.ip");
    CHECK_EQ(call(__imp__NetDll_inet_addr, g_buf + kString), 0xFFFFFFFFu);

    // TCP listener on loopback/ephemeral port.
    const uint32_t listener = socket_tcp();
    wr32(g_buf + kOpt, 1);
    CHECK_EQ(call(__imp__NetDll_setsockopt, 0, listener, 0xFFFF, 0x0004,
                  g_buf + kOpt, 4), 0u);
    sockaddr4(g_buf + kAddr, 0x7F000001u, 0);
    CHECK_EQ(call(__imp__NetDll_bind, 0, listener, g_buf + kAddr, 16), 0u);
    CHECK_EQ(call(__imp__NetDll_listen, 0, listener, 4), 0u);
    wr32(g_buf + kAddrLen, 16);
    CHECK_EQ(call(__imp__NetDll_getsockname, 0, listener, g_buf + kAddr, g_buf + kAddrLen), 0u);
    CHECK_EQ(rd16(g_buf + kAddr), 2u);
    CHECK_EQ(rd32(g_buf + kAddr + 4), 0x7F000001u);
    const uint16_t tcp_port = rd16(g_buf + kAddr + 2);
    CHECK(tcp_port != 0);

    const uint32_t client = socket_tcp();
    sockaddr4(g_buf + kAddr2, 0x7F000001u, tcp_port);
    CHECK_EQ(call(__imp__NetDll_connect, 0, client, g_buf + kAddr2, 16), 0u);

    fdset(g_buf + kFdRead, listener);
    timeval_ms(g_buf + kTimeout, 1000);
    CHECK_EQ(call(__imp__NetDll_select, 0, 0, g_buf + kFdRead, 0, 0, g_buf + kTimeout), 1u);
    CHECK_EQ(rd32(g_buf + kFdRead), 1u);
    CHECK_EQ(rd32(g_buf + kFdRead + 4), listener);

    wr32(g_buf + kAddrLen, 16);
    const uint32_t accepted = call(__imp__NetDll_accept, 0, listener, g_buf + kAddr2, g_buf + kAddrLen);
    CHECK(accepted != 0xFFFFFFFFu);
    CHECK_EQ(rd16(g_buf + kAddr2), 2u);

    // getsockopt uses WinSock/Xbox 4-byte integer layout, independent of host
    // sockopt structure sizes.
    wr32(g_buf + kOptLen, 4);
    CHECK_EQ(call(__imp__NetDll_getsockopt, 0, client, 0xFFFF, 0x1008,
                  g_buf + kOpt, g_buf + kOptLen), 0u);
    CHECK_EQ(rd32(g_buf + kOpt), 1u);
    CHECK_EQ(rd32(g_buf + kOptLen), 4u);

    wr32(g_buf + kAddrLen, 16);
    CHECK_EQ(call(__imp__NetDll_getpeername, 0, client, g_buf + kAddr2, g_buf + kAddrLen), 0u);
    CHECK_EQ(rd16(g_buf + kAddr2 + 2), tcp_port);

    memcpy(g_base + g_buf + kData, "ping", 4);
    CHECK_EQ(call(__imp__NetDll_send, 0, client, g_buf + kData, 4, 0), 4u);
    fdset(g_buf + kFdRead, accepted);
    timeval_ms(g_buf + kTimeout, 1000);
    CHECK_EQ(call(__imp__NetDll_select, 0, 0, g_buf + kFdRead, 0, 0, g_buf + kTimeout), 1u);

    // FIONREAD is translated by the host backend and the in/out DWORD remains
    // guest-big-endian.
    wr32(g_buf + kOpt, 0);
    CHECK_EQ(call(__imp__NetDll_ioctlsocket, 0, accepted, 0x4004667F, g_buf + kOpt), 0u);
    CHECK(rd32(g_buf + kOpt) >= 4u);
    CHECK_EQ(call(__imp__NetDll_recv, 0, accepted, g_buf + kData2, 16, 0), 4u);
    CHECK(memcmp(g_base + g_buf + kData2, "ping", 4) == 0);

    memcpy(g_base + g_buf + kData, "pong", 4);
    CHECK_EQ(call(__imp__NetDll_send, 0, accepted, g_buf + kData, 4, 0), 4u);
    CHECK_EQ(call(__imp__NetDll_recv, 0, client, g_buf + kData2, 16, 0), 4u);
    CHECK(memcmp(g_base + g_buf + kData2, "pong", 4) == 0);

    // FIONBIO switches to nonblocking. Empty recv reports WSAEWOULDBLOCK in
    // this thread without altering another thread's WSA error.
    wr32(g_buf + kOpt, 1);
    CHECK_EQ(call(__imp__NetDll_ioctlsocket, 0, client, 0x8004667E, g_buf + kOpt), 0u);
    CHECK_EQ(call(__imp__NetDll_recv, 0, client, g_buf + kData2, 1, 0), 0xFFFFFFFFu);
    CHECK_EQ(last_error(), 10035u);
    wr32(g_buf + kOpt, 0);
    CHECK_EQ(call(__imp__NetDll_ioctlsocket, 0, client, 0x8004667E, g_buf + kOpt), 0u);

    // UDP exercises sendto/recvfrom and address marshalling.
    const uint32_t udp_server = socket_udp();
    sockaddr4(g_buf + kAddr, 0x7F000001u, 0);
    CHECK_EQ(call(__imp__NetDll_bind, 0, udp_server, g_buf + kAddr, 16), 0u);
    wr32(g_buf + kAddrLen, 16);
    CHECK_EQ(call(__imp__NetDll_getsockname, 0, udp_server, g_buf + kAddr, g_buf + kAddrLen), 0u);
    const uint16_t udp_port = rd16(g_buf + kAddr + 2);
    CHECK(udp_port != 0);
    const uint32_t udp_client = socket_udp();
    sockaddr4(g_buf + kAddr2, 0x7F000001u, udp_port);
    memcpy(g_base + g_buf + kData, "udp!", 4);
    CHECK_EQ(call(__imp__NetDll_sendto, 0, udp_client, g_buf + kData, 4, 0,
                  g_buf + kAddr2, 16), 4u);
    fdset(g_buf + kFdRead, udp_server);
    timeval_ms(g_buf + kTimeout, 1000);
    CHECK_EQ(call(__imp__NetDll_select, 0, 0, g_buf + kFdRead, 0, 0, g_buf + kTimeout), 1u);
    wr32(g_buf + kAddrLen, 16);
    CHECK_EQ(call(__imp__NetDll_recvfrom, 0, udp_server, g_buf + kData2, 16, 0,
                  g_buf + kAddr2, g_buf + kAddrLen), 4u);
    CHECK(memcmp(g_base + g_buf + kData2, "udp!", 4) == 0);
    CHECK_EQ(rd32(g_buf + kAddr2 + 4), 0x7F000001u);

    // Stale handles never become aliases after native descriptor reuse.
    const uint32_t stale = socket_tcp();
    CHECK_EQ(call(__imp__NetDll_closesocket, 0, stale), 0u);
    const uint32_t replacement = socket_tcp();
    CHECK(replacement != stale);
    CHECK_EQ(call(__imp__NetDll_closesocket, 0, stale), 0xFFFFFFFFu);
    CHECK_EQ(last_error(), 10038u);
    CHECK_EQ(call(__imp__NetDll_closesocket, 0, replacement), 0u);

    CHECK_EQ(call(__imp__NetDll_shutdown, 0, client, 2), 0u);
    CHECK_EQ(call(__imp__NetDll_closesocket, 0, accepted), 0u);
    CHECK_EQ(call(__imp__NetDll_closesocket, 0, client), 0u);
    CHECK_EQ(call(__imp__NetDll_closesocket, 0, listener), 0u);
    CHECK_EQ(call(__imp__NetDll_closesocket, 0, udp_client), 0u);
    CHECK_EQ(call(__imp__NetDll_closesocket, 0, udp_server), 0u);

    CHECK_EQ(call(__imp__NetDll_WSACleanup, 0), 0u);
    CHECK_EQ(call(__imp__NetDll_socket, 0, 2, 1, 6), 0xFFFFFFFFu);
    CHECK_EQ(last_error(), 10093u);
    CHECK_EQ(call(__imp__NetDll_WSACleanup, 0), 0xFFFFFFFFu);
    CHECK_EQ(last_error(), 10093u);

    shutdown_xam_net_hle();
    runtime_shutdown();
    clear_imports();
    mem.release();
    return test_result("rt_xam_net");
}
