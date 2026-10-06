// POSIX implementation of the NetDll socket backend.  Guest ABI and WinSock
// constants stay in runtime/src/hle_xam_net.cpp; this file only translates the
// normalized runtime contract to native sockets.
#include "rcomp/runtime/net_platform.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr int kWsaEintr = 10004;
constexpr int kWsaEbadf = 10009;
constexpr int kWsaEacces = 10013;
constexpr int kWsaEfault = 10014;
constexpr int kWsaEinval = 10022;
constexpr int kWsaEmfile = 10024;
constexpr int kWsaEwouldblock = 10035;
constexpr int kWsaEinprogress = 10036;
constexpr int kWsaEalready = 10037;
constexpr int kWsaEnotsock = 10038;
constexpr int kWsaEdestaddrreq = 10039;
constexpr int kWsaEmsgsize = 10040;
constexpr int kWsaEprototype = 10041;
constexpr int kWsaEnoprotoopt = 10042;
constexpr int kWsaEprotonosupport = 10043;
constexpr int kWsaEsocktnosupport = 10044;
constexpr int kWsaEopnotsupp = 10045;
constexpr int kWsaEpfnosupport = 10046;
constexpr int kWsaEafnosupport = 10047;
constexpr int kWsaEaddrinuse = 10048;
constexpr int kWsaEaddrnotavail = 10049;
constexpr int kWsaEnetdown = 10050;
constexpr int kWsaEnetunreach = 10051;
constexpr int kWsaEnetreset = 10052;
constexpr int kWsaEconnaborted = 10053;
constexpr int kWsaEconnreset = 10054;
constexpr int kWsaEnobufs = 10055;
constexpr int kWsaEisconn = 10056;
constexpr int kWsaEnotconn = 10057;
constexpr int kWsaEshutdown = 10058;
constexpr int kWsaEtimedout = 10060;
constexpr int kWsaEconnrefused = 10061;
constexpr int kWsaEhostdown = 10064;
constexpr int kWsaEhostunreach = 10065;
constexpr int kWsaSyscallFailure = 10107;

int wsa_from_errno(int e) {
    if (e == 0) return 0;
    if (e == EINTR) return kWsaEintr;
    if (e == EBADF) return kWsaEbadf;
    if (e == EACCES || e == EPERM) return kWsaEacces;
    if (e == EFAULT) return kWsaEfault;
    if (e == EINVAL) return kWsaEinval;
    if (e == EMFILE || e == ENFILE) return kWsaEmfile;
    if (e == EAGAIN || e == EWOULDBLOCK) return kWsaEwouldblock;
    if (e == EINPROGRESS) return kWsaEinprogress;
    if (e == EALREADY) return kWsaEalready;
    if (e == ENOTSOCK) return kWsaEnotsock;
    if (e == EDESTADDRREQ) return kWsaEdestaddrreq;
    if (e == EMSGSIZE) return kWsaEmsgsize;
    if (e == EPROTOTYPE) return kWsaEprototype;
    if (e == ENOPROTOOPT) return kWsaEnoprotoopt;
    if (e == EPROTONOSUPPORT) return kWsaEprotonosupport;
#ifdef ESOCKTNOSUPPORT
    if (e == ESOCKTNOSUPPORT) return kWsaEsocktnosupport;
#endif
    if (e == EOPNOTSUPP) return kWsaEopnotsupp;
#ifdef EPFNOSUPPORT
    if (e == EPFNOSUPPORT) return kWsaEpfnosupport;
#endif
    if (e == EAFNOSUPPORT) return kWsaEafnosupport;
    if (e == EADDRINUSE) return kWsaEaddrinuse;
    if (e == EADDRNOTAVAIL) return kWsaEaddrnotavail;
    if (e == ENETDOWN) return kWsaEnetdown;
    if (e == ENETUNREACH) return kWsaEnetunreach;
    if (e == ENETRESET) return kWsaEnetreset;
    if (e == ECONNABORTED) return kWsaEconnaborted;
    if (e == ECONNRESET) return kWsaEconnreset;
    if (e == ENOBUFS || e == ENOMEM) return kWsaEnobufs;
    if (e == EISCONN) return kWsaEisconn;
    if (e == ENOTCONN) return kWsaEnotconn;
#ifdef ESHUTDOWN
    if (e == ESHUTDOWN) return kWsaEshutdown;
#endif
    if (e == ETIMEDOUT) return kWsaEtimedout;
    if (e == ECONNREFUSED) return kWsaEconnrefused;
#ifdef EHOSTDOWN
    if (e == EHOSTDOWN) return kWsaEhostdown;
#endif
    if (e == EHOSTUNREACH) return kWsaEhostunreach;
    return kWsaSyscallFailure;
}

int native_socket(rcomp_net_socket s, int* out) {
    if (s < 0 || s > INT_MAX) return kWsaEnotsock;
    *out = static_cast<int>(s);
    return 0;
}

sockaddr_in to_native(const rcomp_net_addr4& a) {
    sockaddr_in out{};
    out.sin_family = AF_INET;
    out.sin_port = htons(a.port);
    out.sin_addr.s_addr = htonl(a.address);
    return out;
}

rcomp_net_addr4 from_native(const sockaddr_in& a) {
    rcomp_net_addr4 out{};
    out.address = ntohl(a.sin_addr.s_addr);
    out.port = ntohs(a.sin_port);
    return out;
}

int native_flags(uint32_t flags, bool receiving) {
    int out = 0;
    if (flags & RCOMP_NET_MSG_OOB) out |= MSG_OOB;
    if (flags & RCOMP_NET_MSG_PEEK) out |= MSG_PEEK;
    if (flags & RCOMP_NET_MSG_DONTROUTE) out |= MSG_DONTROUTE;
#ifdef MSG_WAITALL
    if (receiving && (flags & RCOMP_NET_MSG_WAITALL)) out |= MSG_WAITALL;
#else
    (void)receiving;
#endif
    return out;
}

int option_native(enum rcomp_net_option option, int* level, int* name) {
    *level = SOL_SOCKET;
    switch (option) {
    case RCOMP_NET_OPT_REUSEADDR: *name = SO_REUSEADDR; return 0;
    case RCOMP_NET_OPT_KEEPALIVE: *name = SO_KEEPALIVE; return 0;
    case RCOMP_NET_OPT_BROADCAST: *name = SO_BROADCAST; return 0;
    case RCOMP_NET_OPT_SNDBUF: *name = SO_SNDBUF; return 0;
    case RCOMP_NET_OPT_RCVBUF: *name = SO_RCVBUF; return 0;
    case RCOMP_NET_OPT_SNDTIMEO: *name = SO_SNDTIMEO; return 0;
    case RCOMP_NET_OPT_RCVTIMEO: *name = SO_RCVTIMEO; return 0;
    case RCOMP_NET_OPT_ERROR: *name = SO_ERROR; return 0;
    case RCOMP_NET_OPT_TYPE: *name = SO_TYPE; return 0;
    case RCOMP_NET_OPT_TCP_NODELAY: *level = IPPROTO_TCP; *name = TCP_NODELAY; return 0;
    default: return kWsaEnoprotoopt;
    }
}

}  // namespace

extern "C" int rcomp_net_platform_startup(void) { return 0; }
extern "C" void rcomp_net_platform_cleanup(void) {}

extern "C" int rcomp_net_socket_open(int32_t family, int32_t type, int32_t protocol,
                                      rcomp_net_socket* out_socket) {
    if (!out_socket) return kWsaEfault;
    if (family != RCOMP_NET_AF_INET) return kWsaEafnosupport;
    if (type != RCOMP_NET_SOCK_STREAM && type != RCOMP_NET_SOCK_DGRAM) return kWsaEsocktnosupport;
    if (protocol != 0 && protocol != RCOMP_NET_IPPROTO_TCP && protocol != RCOMP_NET_IPPROTO_UDP)
        return kWsaEprotonosupport;
    const int s = ::socket(AF_INET, type == RCOMP_NET_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM, protocol);
    if (s < 0) return wsa_from_errno(errno);
    *out_socket = s;
    return 0;
}

extern "C" int rcomp_net_socket_close(rcomp_net_socket socket) {
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    return ::close(s) == 0 ? 0 : wsa_from_errno(errno);
}

extern "C" int rcomp_net_socket_cancel(rcomp_net_socket socket) {
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    if (::shutdown(s, SHUT_RDWR) == 0) return 0;
    // ENOTCONN/EINVAL are common for listeners and datagram sockets. Closing
    // is still deferred until leases drain, so cancellation is best-effort.
    if (errno == ENOTCONN || errno == EINVAL) return 0;
    return wsa_from_errno(errno);
}

extern "C" int rcomp_net_socket_shutdown(rcomp_net_socket socket, int32_t how) {
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    if (how < 0 || how > 2) return kWsaEinval;
    return ::shutdown(s, how) == 0 ? 0 : wsa_from_errno(errno);
}

extern "C" int rcomp_net_socket_bind(rcomp_net_socket socket, const rcomp_net_addr4* addr) {
    if (!addr) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    sockaddr_in a = to_native(*addr);
    return ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 ? 0 : wsa_from_errno(errno);
}

extern "C" int rcomp_net_socket_connect(rcomp_net_socket socket, const rcomp_net_addr4* addr) {
    if (!addr) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    sockaddr_in a = to_native(*addr);
    return ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 ? 0 : wsa_from_errno(errno);
}

extern "C" int rcomp_net_socket_listen(rcomp_net_socket socket, int32_t backlog) {
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    return ::listen(s, backlog) == 0 ? 0 : wsa_from_errno(errno);
}

extern "C" int rcomp_net_socket_accept(rcomp_net_socket socket, rcomp_net_socket* out_socket,
                                        rcomp_net_addr4* out_addr) {
    if (!out_socket) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    sockaddr_in a{};
    socklen_t n = sizeof(a);
    const int accepted = ::accept(s, reinterpret_cast<sockaddr*>(&a), &n);
    if (accepted < 0) return wsa_from_errno(errno);
    *out_socket = accepted;
    if (out_addr) *out_addr = from_native(a);
    return 0;
}

extern "C" int rcomp_net_socket_getsockname(rcomp_net_socket socket, rcomp_net_addr4* out_addr) {
    if (!out_addr) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    sockaddr_in a{};
    socklen_t n = sizeof(a);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&a), &n) != 0) return wsa_from_errno(errno);
    if (a.sin_family != AF_INET) return kWsaEafnosupport;
    *out_addr = from_native(a);
    return 0;
}

extern "C" int rcomp_net_socket_getpeername(rcomp_net_socket socket, rcomp_net_addr4* out_addr) {
    if (!out_addr) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    sockaddr_in a{};
    socklen_t n = sizeof(a);
    if (::getpeername(s, reinterpret_cast<sockaddr*>(&a), &n) != 0) return wsa_from_errno(errno);
    if (a.sin_family != AF_INET) return kWsaEafnosupport;
    *out_addr = from_native(a);
    return 0;
}

extern "C" int rcomp_net_socket_send(rcomp_net_socket socket, const void* data, uint32_t size,
                                      uint32_t flags, int32_t* out_size) {
    if (!out_size || (size && !data)) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    const ssize_t n = ::send(s, data, size, native_flags(flags, false));
    if (n < 0) return wsa_from_errno(errno);
    *out_size = static_cast<int32_t>(n);
    return 0;
}

extern "C" int rcomp_net_socket_recv(rcomp_net_socket socket, void* data, uint32_t size,
                                      uint32_t flags, int32_t* out_size) {
    if (!out_size || (size && !data)) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    const ssize_t n = ::recv(s, data, size, native_flags(flags, true));
    if (n < 0) return wsa_from_errno(errno);
    *out_size = static_cast<int32_t>(n);
    return 0;
}

extern "C" int rcomp_net_socket_sendto(rcomp_net_socket socket, const void* data, uint32_t size,
                                        uint32_t flags, const rcomp_net_addr4* addr,
                                        int32_t* out_size) {
    if (!out_size || !addr || (size && !data)) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    sockaddr_in a = to_native(*addr);
    const ssize_t n = ::sendto(s, data, size, native_flags(flags, false),
                               reinterpret_cast<sockaddr*>(&a), sizeof(a));
    if (n < 0) return wsa_from_errno(errno);
    *out_size = static_cast<int32_t>(n);
    return 0;
}

extern "C" int rcomp_net_socket_recvfrom(rcomp_net_socket socket, void* data, uint32_t size,
                                          uint32_t flags, rcomp_net_addr4* out_addr,
                                          int32_t* out_size) {
    if (!out_size || (size && !data)) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    sockaddr_in a{};
    socklen_t naddr = sizeof(a);
    const ssize_t n = ::recvfrom(s, data, size, native_flags(flags, true),
                                 out_addr ? reinterpret_cast<sockaddr*>(&a) : nullptr,
                                 out_addr ? &naddr : nullptr);
    if (n < 0) return wsa_from_errno(errno);
    if (out_addr) *out_addr = from_native(a);
    *out_size = static_cast<int32_t>(n);
    return 0;
}

extern "C" int rcomp_net_socket_set_int_option(rcomp_net_socket socket,
                                                enum rcomp_net_option option, int32_t value) {
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    int level, name;
    e = option_native(option, &level, &name);
    if (e) return e;
    if (option == RCOMP_NET_OPT_ERROR || option == RCOMP_NET_OPT_TYPE) return kWsaEnoprotoopt;
    if (option == RCOMP_NET_OPT_SNDTIMEO || option == RCOMP_NET_OPT_RCVTIMEO) {
        if (value < 0) return kWsaEinval;
        timeval tv{};
        tv.tv_sec = value / 1000;
        tv.tv_usec = (value % 1000) * 1000;
        return ::setsockopt(s, level, name, &tv, sizeof(tv)) == 0 ? 0 : wsa_from_errno(errno);
    }
    const int native_value = value;
    return ::setsockopt(s, level, name, &native_value, sizeof(native_value)) == 0 ? 0
                                                                                 : wsa_from_errno(errno);
}

extern "C" int rcomp_net_socket_get_int_option(rcomp_net_socket socket,
                                                enum rcomp_net_option option,
                                                int32_t* out_value) {
    if (!out_value) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    int level, name;
    e = option_native(option, &level, &name);
    if (e) return e;
    if (option == RCOMP_NET_OPT_SNDTIMEO || option == RCOMP_NET_OPT_RCVTIMEO) {
        timeval tv{};
        socklen_t n = sizeof(tv);
        if (::getsockopt(s, level, name, &tv, &n) != 0) return wsa_from_errno(errno);
        int64_t ms = int64_t(tv.tv_sec) * 1000 + (tv.tv_usec + 999) / 1000;
        if (ms > INT32_MAX) ms = INT32_MAX;
        *out_value = static_cast<int32_t>(ms);
        return 0;
    }
    int native_value = 0;
    socklen_t n = sizeof(native_value);
    if (::getsockopt(s, level, name, &native_value, &n) != 0) return wsa_from_errno(errno);
    *out_value = option == RCOMP_NET_OPT_ERROR ? wsa_from_errno(native_value) : native_value;
    return 0;
}

extern "C" int rcomp_net_socket_set_linger(rcomp_net_socket socket,
                                             const rcomp_net_linger* value) {
    if (!value) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    if (value->seconds < 0 || value->seconds > USHRT_MAX) return kWsaEinval;
    linger l{};
    l.l_onoff = value->enabled != 0;
    l.l_linger = value->seconds;
    return ::setsockopt(s, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) == 0 ? 0 : wsa_from_errno(errno);
}

extern "C" int rcomp_net_socket_get_linger(rcomp_net_socket socket,
                                             rcomp_net_linger* out_value) {
    if (!out_value) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    linger l{};
    socklen_t n = sizeof(l);
    if (::getsockopt(s, SOL_SOCKET, SO_LINGER, &l, &n) != 0) return wsa_from_errno(errno);
    out_value->enabled = l.l_onoff != 0;
    out_value->seconds = l.l_linger;
    return 0;
}

extern "C" int rcomp_net_socket_ioctl(rcomp_net_socket socket, enum rcomp_net_ioctl_op op,
                                       uint32_t* inout_value) {
    if (!inout_value) return kWsaEfault;
    int s;
    int e = native_socket(socket, &s);
    if (e) return e;
    if (op == RCOMP_NET_IOCTL_NONBLOCK) {
        const int old_flags = fcntl(s, F_GETFL, 0);
        if (old_flags < 0) return wsa_from_errno(errno);
        const int new_flags = *inout_value ? (old_flags | O_NONBLOCK) : (old_flags & ~O_NONBLOCK);
        return fcntl(s, F_SETFL, new_flags) == 0 ? 0 : wsa_from_errno(errno);
    }
    if (op == RCOMP_NET_IOCTL_BYTES_AVAILABLE) {
        int bytes = 0;
        if (::ioctl(s, FIONREAD, &bytes) != 0) return wsa_from_errno(errno);
        *inout_value = bytes < 0 ? 0u : static_cast<uint32_t>(bytes);
        return 0;
    }
    return kWsaEinval;
}

extern "C" int rcomp_net_poll(rcomp_net_pollfd* fds, uint32_t count, int64_t timeout_us,
                               uint32_t* out_ready) {
    constexpr uint32_t kMaxPoll = 192;
    if (!out_ready || (count && !fds)) return kWsaEfault;
    if (count > kMaxPoll || timeout_us < -1) return kWsaEinval;
    pollfd native[kMaxPoll]{};
    for (uint32_t i = 0; i < count; ++i) {
        int s;
        int e = native_socket(fds[i].socket, &s);
        if (e) return e;
        native[i].fd = s;
        if (fds[i].events & RCOMP_NET_POLL_READ) native[i].events |= POLLIN;
        if (fds[i].events & RCOMP_NET_POLL_WRITE) native[i].events |= POLLOUT;
        if (fds[i].events & RCOMP_NET_POLL_EXCEPTION) native[i].events |= POLLPRI;
        fds[i].revents = 0;
    }
    int timeout_ms = -1;
    if (timeout_us >= 0) {
        int64_t ms = (timeout_us + 999) / 1000;
        if (ms > INT_MAX) ms = INT_MAX;
        timeout_ms = static_cast<int>(ms);
    }
    const int n = ::poll(native, count, timeout_ms);
    if (n < 0) return wsa_from_errno(errno);
    uint32_t ready = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (native[i].revents & POLLNVAL) return kWsaEnotsock;
        if (native[i].revents & (POLLIN | POLLHUP)) fds[i].revents |= RCOMP_NET_POLL_READ;
        if (native[i].revents & POLLOUT) fds[i].revents |= RCOMP_NET_POLL_WRITE;
        if (native[i].revents & (POLLPRI | POLLERR)) fds[i].revents |= RCOMP_NET_POLL_EXCEPTION;
        fds[i].revents &= fds[i].events;
        if (fds[i].revents) ++ready;
    }
    *out_ready = ready;
    return 0;
}
