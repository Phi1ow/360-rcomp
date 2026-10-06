// Internal contract between the two NetDll units (owner: Agent 3, runtime/).
//
// src/hle_xam_net.cpp owns the guest socket table, WinSock/XNet startup state and the per-thread WSA
// error. src/hle_xam_net_wsa.cpp (overlapped WSA I/O, WSA events, WSAEventSelect, XNet keys/DNS/random)
// reaches that state only through this header, and gives back the hooks the plain socket calls need
// (network-event re-enabling, connect tracking, teardown). Not a public interface.
#pragma once

#include <stdint.h>

#include "rcomp/runtime/net_platform.h"
#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Registers the imports of src/hle_xam_net_wsa.cpp; called by register_xam_net_hle().
Status register_xam_net_wsa_hle();

namespace netdll {

// WinSock / Win32 error numbers shared by both units.
constexpr int kWsaEfault = 10014;
constexpr int kWsaEinval = 10022;
constexpr int kWsaEwouldblock = 10035;
constexpr int kWsaEinprogress = 10036;
constexpr int kWsaEalready = 10037;
constexpr int kWsaEnotsock = 10038;
constexpr int kWsaEnobufs = 10055;
constexpr int kWsaEnetdown = 10050;
constexpr int kWsaNotInitialized = 10093;
constexpr int kWsaOperationAborted = 995;  // ERROR_OPERATION_ABORTED
constexpr int kWsaIoIncomplete = 996;      // ERROR_IO_INCOMPLETE
constexpr int kWsaIoPending = 997;         // ERROR_IO_PENDING

// WSAEventSelect network events (winsockx.h FD_* bits).
constexpr uint32_t kFdRead = 0x01, kFdWrite = 0x02, kFdOob = 0x04, kFdAccept = 0x08, kFdConnect = 0x10,
                   kFdClose = 0x20;
constexpr uint32_t kFdAll = kFdRead | kFdWrite | kFdOob | kFdAccept | kFdConnect | kFdClose;

// A counted use of a guest socket: while held, the native descriptor stays open (closesocket defers
// the native close to the last release and interrupts blocked native calls instead).
struct Lease {
    uint8_t index = 0;
    rcomp_net_socket native = RCOMP_NET_INVALID_SOCKET;
    bool valid = false;
};

struct SocketInfo {
    uint8_t type = 0;        // RCOMP_NET_SOCK_STREAM / RCOMP_NET_SOCK_DGRAM
    bool listening = false;  // a successful listen() was made on it
};

// 0 or a WinSock error (WSAENOTSOCK, WSANOTINITIALISED, ...). `info` may be null.
int acquire(uint32_t handle, Lease* out, SocketInfo* info = nullptr);
void release(Lease* lease);
// 0 while the leased handle is still open and WinSock is usable; otherwise the error a call on it
// must now report (the handle was closed, WSACleanup or title teardown started).
int lease_error(const Lease& lease);

// The calling thread's WSA last error (WSAGetLastError).
void set_error(uint32_t error);

// Guest memory helpers (big-endian guest, explicit marshalling).
bool guest_span(uint32_t address, uint32_t size, bool writable, uint8_t** out);
int read_sockaddr(uint32_t address, uint32_t length, rcomp_net_addr4* out);
int check_sockaddr_output(uint32_t address, uint32_t length);
bool write_sockaddr(uint32_t address, const rcomp_net_addr4& value);
bool message_flags_valid(uint32_t flags);

// WSAStartup or XNetStartup holds a reference and no cleanup/teardown is running.
bool network_started();
// XNetStartup holds a reference; `key_capacity` receives cfgKeyRegMax (or its default).
bool xnet_started(uint32_t* key_capacity);

// ---- hooks implemented by src/hle_xam_net_wsa.cpp -----------------------------------------------
// A call that re-enables recording of `events` for an event-selected socket (recv: FD_READ, a send
// that failed with WSAEWOULDBLOCK: FD_WRITE, accept: FD_ACCEPT, MSG_OOB recv: FD_OOB).
void reenable_network_events(uint32_t handle, uint32_t events);
// connect() started (or completed) on `handle`: FD_CONNECT is recorded when it completes.
void connect_started(uint32_t handle);
// closesocket succeeded: drops the WSAEventSelect association and aborts queued overlapped requests.
void socket_closed(uint32_t handle);
// ioctlsocket(FIONBIO, 0) is refused while WSAEventSelect has events selected on the socket.
bool event_select_active(uint32_t handle);
// The last XNetCleanup reference went away: registered keys are forgotten.
void xnet_stopped();
// Title teardown phase 1 (wake the worker; queued requests complete aborted) and phase 2 (join the
// worker, release every lease it holds, forget all state). Phase 2 runs before descriptors close.
void begin_shutdown_wsa();
void shutdown_wsa();

}  // namespace netdll
}  // namespace rcomp::rt
