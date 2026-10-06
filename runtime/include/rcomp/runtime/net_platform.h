// Portable socket backend used by the xam.xex NetDll HLE.
//
// This interface deliberately speaks Xbox/WinSock errors (100xx) while all
// addresses and option values are host-order plain integers.  Platform files
// translate those normalized values to the native socket API.  That keeps the
// PPC HLE independent of POSIX, FreeBSD and SceNet numeric constants.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t rcomp_net_socket;

enum {
    RCOMP_NET_INVALID_SOCKET = -1,
    RCOMP_NET_AF_INET = 2,
    RCOMP_NET_SOCK_STREAM = 1,
    RCOMP_NET_SOCK_DGRAM = 2,
    RCOMP_NET_IPPROTO_IP = 0,
    RCOMP_NET_IPPROTO_TCP = 6,
    RCOMP_NET_IPPROTO_UDP = 17,
};

// Internal message flags. Values intentionally match WinSock/Xbox so the HLE
// can reject unknown guest bits before reaching a platform backend.
enum rcomp_net_message_flag {
    RCOMP_NET_MSG_OOB = 0x0001,
    RCOMP_NET_MSG_PEEK = 0x0002,
    RCOMP_NET_MSG_DONTROUTE = 0x0004,
    RCOMP_NET_MSG_WAITALL = 0x0008,
};

// Normalized option IDs.  Xbox SOL_SOCKET is 0xFFFF and cannot be passed
// directly to POSIX/SceNet, so the HLE maps guest options to this enum.
enum rcomp_net_option {
    RCOMP_NET_OPT_REUSEADDR = 1,
    RCOMP_NET_OPT_KEEPALIVE,
    RCOMP_NET_OPT_BROADCAST,
    RCOMP_NET_OPT_SNDBUF,
    RCOMP_NET_OPT_RCVBUF,
    RCOMP_NET_OPT_SNDTIMEO,
    RCOMP_NET_OPT_RCVTIMEO,
    RCOMP_NET_OPT_ERROR,
    RCOMP_NET_OPT_TYPE,
    RCOMP_NET_OPT_TCP_NODELAY,
    RCOMP_NET_OPT_LINGER,
};

enum rcomp_net_ioctl_op {
    RCOMP_NET_IOCTL_NONBLOCK = 1,
    RCOMP_NET_IOCTL_BYTES_AVAILABLE = 2,
};

enum rcomp_net_poll_event {
    RCOMP_NET_POLL_READ = 1u << 0,
    RCOMP_NET_POLL_WRITE = 1u << 1,
    RCOMP_NET_POLL_EXCEPTION = 1u << 2,
};

typedef struct rcomp_net_addr4 {
    // Host-order IPv4 value, e.g. 127.0.0.1 == 0x7F000001.
    uint32_t address;
    // Host-order TCP/UDP port.
    uint16_t port;
    uint16_t reserved;
} rcomp_net_addr4;

typedef struct rcomp_net_linger {
    int32_t enabled;
    int32_t seconds;
} rcomp_net_linger;

typedef struct rcomp_net_pollfd {
    rcomp_net_socket socket;
    uint32_t events;
    uint32_t revents;
} rcomp_net_pollfd;

// Return 0 on success, otherwise a WinSock WSA* error number.  startup/cleanup
// are called once when the HLE's WSA reference count crosses 0 <-> 1.
int rcomp_net_platform_startup(void);
void rcomp_net_platform_cleanup(void);

int rcomp_net_socket_open(int32_t family, int32_t type, int32_t protocol,
                          rcomp_net_socket* out_socket);
int rcomp_net_socket_close(rcomp_net_socket socket);
// Interrupt a blocking native operation so a generation-invalidated guest
// handle can finish closing. This is lifecycle-only; guest shutdown() uses the
// separate shutdown entry below.
int rcomp_net_socket_cancel(rcomp_net_socket socket);
int rcomp_net_socket_shutdown(rcomp_net_socket socket, int32_t how);
int rcomp_net_socket_bind(rcomp_net_socket socket, const rcomp_net_addr4* addr);
int rcomp_net_socket_connect(rcomp_net_socket socket, const rcomp_net_addr4* addr);
int rcomp_net_socket_listen(rcomp_net_socket socket, int32_t backlog);
int rcomp_net_socket_accept(rcomp_net_socket socket, rcomp_net_socket* out_socket,
                            rcomp_net_addr4* out_addr);
int rcomp_net_socket_getsockname(rcomp_net_socket socket, rcomp_net_addr4* out_addr);
int rcomp_net_socket_getpeername(rcomp_net_socket socket, rcomp_net_addr4* out_addr);

int rcomp_net_socket_send(rcomp_net_socket socket, const void* data, uint32_t size,
                          uint32_t flags, int32_t* out_size);
int rcomp_net_socket_recv(rcomp_net_socket socket, void* data, uint32_t size,
                          uint32_t flags, int32_t* out_size);
int rcomp_net_socket_sendto(rcomp_net_socket socket, const void* data, uint32_t size,
                            uint32_t flags, const rcomp_net_addr4* addr,
                            int32_t* out_size);
int rcomp_net_socket_recvfrom(rcomp_net_socket socket, void* data, uint32_t size,
                              uint32_t flags, rcomp_net_addr4* out_addr,
                              int32_t* out_size);

int rcomp_net_socket_set_int_option(rcomp_net_socket socket, enum rcomp_net_option option,
                                    int32_t value);
int rcomp_net_socket_get_int_option(rcomp_net_socket socket, enum rcomp_net_option option,
                                    int32_t* out_value);
int rcomp_net_socket_set_linger(rcomp_net_socket socket, const rcomp_net_linger* value);
int rcomp_net_socket_get_linger(rcomp_net_socket socket, rcomp_net_linger* out_value);
int rcomp_net_socket_ioctl(rcomp_net_socket socket, enum rcomp_net_ioctl_op op,
                           uint32_t* inout_value);

// timeout_us: -1 = infinite, >= 0 = finite.  revents are cleared by the
// backend. out_ready counts unique entries with at least one requested event.
int rcomp_net_poll(rcomp_net_pollfd* fds, uint32_t count, int64_t timeout_us,
                   uint32_t* out_ready);

#ifdef __cplusplus
}
#endif
