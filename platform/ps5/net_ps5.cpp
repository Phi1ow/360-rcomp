// PS5 implementation of the NetDll socket backend using libSceNet only.
// Symbols used here are present in ps5-payload-sdk v0.42 libSceNet.so.  The
// socket/epoll ABI mirrors the public SceNet user API; no BSD socket syscall is
// used by this backend.
#include "rcomp/runtime/net_platform.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>

namespace {

struct SceNetInAddr {
    uint32_t s_addr;
};

struct SceNetSockaddrIn {
    uint8_t sin_len;
    uint8_t sin_family;
    uint16_t sin_port;
    SceNetInAddr sin_addr;
    uint16_t sin_vport;
    char sin_zero[6];
};
static_assert(sizeof(SceNetSockaddrIn) == 16);

struct SceNetLinger {
    int32_t l_onoff;
    int32_t l_linger;
};
static_assert(sizeof(SceNetLinger) == 8);

struct SceNetSockInfo {
    char name[32];
    int32_t pid;
    int32_t socket;
    int8_t socket_type;
    int8_t policy;
    int8_t priority;
    int8_t reserved8;
    int32_t recv_queue_length;
    int32_t send_queue_length;
    uint32_t local_addr;
    uint32_t remote_addr;
    uint16_t local_port;
    uint16_t remote_port;
    uint16_t local_vport;
    uint16_t remote_vport;
    int32_t state;
    int32_t flags;
    int32_t tx_bps;
    int32_t rx_bps;
    int32_t max_tx_bps;
    int32_t max_rx_bps;
    int32_t tx_vbps;
    int32_t rx_vbps;
    int32_t recv_buffer_size;
    int32_t send_buffer_size;
    int32_t reserved6[8];
    int32_t tx_drops;
    int32_t rx_drops;
    int32_t tx_wait;
    int32_t reserved[2];
};
static_assert(sizeof(SceNetSockInfo) == 0xA0);

union SceNetEpollData {
    void* ptr;
    int32_t fd;
    uint32_t u32;
    uint64_t u64;
};

struct SceNetEpollEvent {
    uint32_t events;
    uint32_t reserved;
    uint64_t ident;
    SceNetEpollData data;
};
static_assert(sizeof(SceNetEpollEvent) == 0x18);

extern "C" {
int sceNetInit(void);
int sceNetTerm(void);
int* sceNetErrnoLoc(void);
int sceNetSocket(const char* name, int domain, int type, int protocol);
int sceNetSocketClose(int s);
int sceNetSocketAbort(int s, int flags);
int sceNetShutdown(int s, int how);
int sceNetBind(int s, const void* addr, uint32_t addrlen);
int sceNetConnect(int s, const void* addr, uint32_t addrlen);
int sceNetListen(int s, int backlog);
int sceNetAccept(int s, void* addr, uint32_t* addrlen);
int sceNetGetsockname(int s, void* name, uint32_t* namelen);
int sceNetGetpeername(int s, void* name, uint32_t* namelen);
int sceNetSend(int s, const void* msg, size_t len, int flags);
int sceNetRecv(int s, void* buf, size_t len, int flags);
int sceNetSendto(int s, const void* msg, size_t len, int flags, const void* to, uint32_t tolen);
int sceNetRecvfrom(int s, void* buf, size_t len, int flags, void* from, uint32_t* fromlen);
int sceNetSetsockopt(int s, int level, int optname, const void* optval, uint32_t optlen);
int sceNetGetsockopt(int s, int level, int optname, void* optval, uint32_t* optlen);
int sceNetGetSockInfo(int s, SceNetSockInfo* info, int n, int flags);
int sceNetEpollCreate(const char* name, int flags);
int sceNetEpollControl(int eid, int op, int id, SceNetEpollEvent* event);
int sceNetEpollWait(int eid, SceNetEpollEvent* events, int maxevents, int timeout);
int sceNetEpollDestroy(int eid);
}

constexpr int kAfInet = 2;
constexpr int kSolSocket = 0xFFFF;
constexpr int kIpProtoTcp = 6;
constexpr int kSoReuseaddr = 0x0004;
constexpr int kSoKeepalive = 0x0008;
constexpr int kSoBroadcast = 0x0020;
constexpr int kSoLinger = 0x0080;
constexpr int kSoSndbuf = 0x1001;
constexpr int kSoRcvbuf = 0x1002;
constexpr int kSoSndtimeo = 0x1105;
constexpr int kSoRcvtimeo = 0x1106;
constexpr int kSoError = 0x1007;
constexpr int kSoType = 0x1008;
constexpr int kSoNbio = 0x1200;
constexpr int kTcpNodelay = 1;

constexpr int kMsgPeek = 0x0002;
constexpr int kMsgWaitall = 0x0040;

constexpr uint32_t kEpollIn = 0x0001;
constexpr uint32_t kEpollOut = 0x0002;
constexpr uint32_t kEpollErr = 0x0008;
constexpr uint32_t kEpollHup = 0x0010;
constexpr int kEpollCtlAdd = 1;

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

int last_error() {
    int* p = sceNetErrnoLoc();
    return p ? wsa_from_errno(*p) : kWsaSyscallFailure;
}

uint16_t swap16(uint16_t v) { return __builtin_bswap16(v); }
uint32_t swap32(uint32_t v) { return __builtin_bswap32(v); }

SceNetSockaddrIn to_native(const rcomp_net_addr4& a) {
    SceNetSockaddrIn out{};
    out.sin_len = sizeof(out);
    out.sin_family = kAfInet;
    out.sin_port = swap16(a.port);
    out.sin_addr.s_addr = swap32(a.address);
    return out;
}

rcomp_net_addr4 from_native(const SceNetSockaddrIn& a) {
    rcomp_net_addr4 out{};
    out.address = swap32(a.sin_addr.s_addr);
    out.port = swap16(a.sin_port);
    return out;
}

int socket_id(rcomp_net_socket socket, int* out) {
    if (socket < 0 || socket > INT_MAX) return kWsaEnotsock;
    *out = static_cast<int>(socket);
    return 0;
}

int message_flags(uint32_t flags, int* out) {
    // Public SceNet defines PEEK and WAITALL.  OOB and DONTROUTE have no
    // documented SceNet equivalents, so reject rather than passing Xbox bits.
    if (flags & (RCOMP_NET_MSG_OOB | RCOMP_NET_MSG_DONTROUTE)) return kWsaEopnotsupp;
    int native = 0;
    if (flags & RCOMP_NET_MSG_PEEK) native |= kMsgPeek;
    if (flags & RCOMP_NET_MSG_WAITALL) native |= kMsgWaitall;
    *out = native;
    return 0;
}

int option_native(enum rcomp_net_option option, int* level, int* name) {
    *level = kSolSocket;
    switch (option) {
    case RCOMP_NET_OPT_REUSEADDR: *name = kSoReuseaddr; return 0;
    case RCOMP_NET_OPT_KEEPALIVE: *name = kSoKeepalive; return 0;
    case RCOMP_NET_OPT_BROADCAST: *name = kSoBroadcast; return 0;
    case RCOMP_NET_OPT_SNDBUF: *name = kSoSndbuf; return 0;
    case RCOMP_NET_OPT_RCVBUF: *name = kSoRcvbuf; return 0;
    case RCOMP_NET_OPT_SNDTIMEO: *name = kSoSndtimeo; return 0;
    case RCOMP_NET_OPT_RCVTIMEO: *name = kSoRcvtimeo; return 0;
    case RCOMP_NET_OPT_ERROR: *name = kSoError; return 0;
    case RCOMP_NET_OPT_TYPE: *name = kSoType; return 0;
    case RCOMP_NET_OPT_TCP_NODELAY: *level = kIpProtoTcp; *name = kTcpNodelay; return 0;
    default: return kWsaEnoprotoopt;
    }
}

}  // namespace

extern "C" int rcomp_net_platform_startup(void) {
    return sceNetInit() == 0 ? 0 : last_error();
}

extern "C" void rcomp_net_platform_cleanup(void) { (void)sceNetTerm(); }

extern "C" int rcomp_net_socket_open(int32_t family, int32_t type, int32_t protocol,
                                      rcomp_net_socket* out_socket) {
    if (!out_socket) return kWsaEfault;
    if (family != RCOMP_NET_AF_INET) return kWsaEafnosupport;
    if (type != RCOMP_NET_SOCK_STREAM && type != RCOMP_NET_SOCK_DGRAM) return kWsaEsocktnosupport;
    if (protocol != 0 && protocol != RCOMP_NET_IPPROTO_TCP && protocol != RCOMP_NET_IPPROTO_UDP)
        return kWsaEprotonosupport;
    const int s = sceNetSocket("rcomp", family, type, protocol);
    if (s < 0) return last_error();
    *out_socket = s;
    return 0;
}

extern "C" int rcomp_net_socket_close(rcomp_net_socket socket) {
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    return sceNetSocketClose(s) == 0 ? 0 : last_error();
}

extern "C" int rcomp_net_socket_cancel(rcomp_net_socket socket) {
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    return sceNetSocketAbort(s, 0) == 0 ? 0 : last_error();
}

extern "C" int rcomp_net_socket_shutdown(rcomp_net_socket socket, int32_t how) {
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    if (how < 0 || how > 2) return kWsaEinval;
    return sceNetShutdown(s, how) == 0 ? 0 : last_error();
}

extern "C" int rcomp_net_socket_bind(rcomp_net_socket socket, const rcomp_net_addr4* addr) {
    if (!addr) return kWsaEfault;
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    SceNetSockaddrIn a = to_native(*addr);
    return sceNetBind(s, &a, sizeof(a)) == 0 ? 0 : last_error();
}

extern "C" int rcomp_net_socket_connect(rcomp_net_socket socket, const rcomp_net_addr4* addr) {
    if (!addr) return kWsaEfault;
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    SceNetSockaddrIn a = to_native(*addr);
    return sceNetConnect(s, &a, sizeof(a)) == 0 ? 0 : last_error();
}

extern "C" int rcomp_net_socket_listen(rcomp_net_socket socket, int32_t backlog) {
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    return sceNetListen(s, backlog) == 0 ? 0 : last_error();
}

extern "C" int rcomp_net_socket_accept(rcomp_net_socket socket, rcomp_net_socket* out_socket,
                                        rcomp_net_addr4* out_addr) {
    if (!out_socket) return kWsaEfault;
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    SceNetSockaddrIn a{};
    uint32_t len = sizeof(a);
    const int accepted = sceNetAccept(s, out_addr ? &a : nullptr, out_addr ? &len : nullptr);
    if (accepted < 0) return last_error();
    *out_socket = accepted;
    if (out_addr) *out_addr = from_native(a);
    return 0;
}

extern "C" int rcomp_net_socket_getsockname(rcomp_net_socket socket, rcomp_net_addr4* out_addr) {
    if (!out_addr) return kWsaEfault;
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    SceNetSockaddrIn a{};
    uint32_t len = sizeof(a);
    if (sceNetGetsockname(s, &a, &len) != 0) return last_error();
    if (a.sin_family != kAfInet) return kWsaEafnosupport;
    *out_addr = from_native(a);
    return 0;
}

extern "C" int rcomp_net_socket_getpeername(rcomp_net_socket socket, rcomp_net_addr4* out_addr) {
    if (!out_addr) return kWsaEfault;
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    SceNetSockaddrIn a{};
    uint32_t len = sizeof(a);
    if (sceNetGetpeername(s, &a, &len) != 0) return last_error();
    if (a.sin_family != kAfInet) return kWsaEafnosupport;
    *out_addr = from_native(a);
    return 0;
}

extern "C" int rcomp_net_socket_send(rcomp_net_socket socket, const void* data, uint32_t size,
                                      uint32_t flags, int32_t* out_size) {
    if (!out_size || (size && !data)) return kWsaEfault;
    int s, native_flags;
    int e = socket_id(socket, &s);
    if (e) return e;
    e = message_flags(flags, &native_flags);
    if (e) return e;
    const int n = sceNetSend(s, data, size, native_flags);
    if (n < 0) return last_error();
    *out_size = n;
    return 0;
}

extern "C" int rcomp_net_socket_recv(rcomp_net_socket socket, void* data, uint32_t size,
                                      uint32_t flags, int32_t* out_size) {
    if (!out_size || (size && !data)) return kWsaEfault;
    int s, native_flags;
    int e = socket_id(socket, &s);
    if (e) return e;
    e = message_flags(flags, &native_flags);
    if (e) return e;
    const int n = sceNetRecv(s, data, size, native_flags);
    if (n < 0) return last_error();
    *out_size = n;
    return 0;
}

extern "C" int rcomp_net_socket_sendto(rcomp_net_socket socket, const void* data, uint32_t size,
                                        uint32_t flags, const rcomp_net_addr4* addr,
                                        int32_t* out_size) {
    if (!out_size || !addr || (size && !data)) return kWsaEfault;
    int s, native_flags;
    int e = socket_id(socket, &s);
    if (e) return e;
    e = message_flags(flags, &native_flags);
    if (e) return e;
    SceNetSockaddrIn a = to_native(*addr);
    const int n = sceNetSendto(s, data, size, native_flags, &a, sizeof(a));
    if (n < 0) return last_error();
    *out_size = n;
    return 0;
}

extern "C" int rcomp_net_socket_recvfrom(rcomp_net_socket socket, void* data, uint32_t size,
                                          uint32_t flags, rcomp_net_addr4* out_addr,
                                          int32_t* out_size) {
    if (!out_size || (size && !data)) return kWsaEfault;
    int s, native_flags;
    int e = socket_id(socket, &s);
    if (e) return e;
    e = message_flags(flags, &native_flags);
    if (e) return e;
    SceNetSockaddrIn a{};
    uint32_t len = sizeof(a);
    const int n = sceNetRecvfrom(s, data, size, native_flags, out_addr ? &a : nullptr,
                                 out_addr ? &len : nullptr);
    if (n < 0) return last_error();
    if (out_addr) *out_addr = from_native(a);
    *out_size = n;
    return 0;
}

extern "C" int rcomp_net_socket_set_int_option(rcomp_net_socket socket,
                                                enum rcomp_net_option option, int32_t value) {
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    int level, name;
    e = option_native(option, &level, &name);
    if (e) return e;
    if (option == RCOMP_NET_OPT_ERROR || option == RCOMP_NET_OPT_TYPE) return kWsaEnoprotoopt;
    int native_value = value;
    if (option == RCOMP_NET_OPT_SNDTIMEO || option == RCOMP_NET_OPT_RCVTIMEO) {
        if (value < 0) return kWsaEinval;
        const int64_t us = int64_t(value) * 1000;
        native_value = us > INT_MAX ? INT_MAX : static_cast<int>(us);
    }
    return sceNetSetsockopt(s, level, name, &native_value, sizeof(native_value)) == 0 ? 0
                                                                                      : last_error();
}

extern "C" int rcomp_net_socket_get_int_option(rcomp_net_socket socket,
                                                enum rcomp_net_option option,
                                                int32_t* out_value) {
    if (!out_value) return kWsaEfault;
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    int level, name;
    e = option_native(option, &level, &name);
    if (e) return e;
    int native_value = 0;
    uint32_t len = sizeof(native_value);
    if (sceNetGetsockopt(s, level, name, &native_value, &len) != 0) return last_error();
    if (option == RCOMP_NET_OPT_ERROR) native_value = wsa_from_errno(native_value);
    if (option == RCOMP_NET_OPT_SNDTIMEO || option == RCOMP_NET_OPT_RCVTIMEO)
        native_value = (native_value + 999) / 1000;
    *out_value = native_value;
    return 0;
}

extern "C" int rcomp_net_socket_set_linger(rcomp_net_socket socket,
                                             const rcomp_net_linger* value) {
    if (!value) return kWsaEfault;
    if (value->seconds < 0) return kWsaEinval;
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    SceNetLinger l{value->enabled != 0, value->seconds};
    return sceNetSetsockopt(s, kSolSocket, kSoLinger, &l, sizeof(l)) == 0 ? 0 : last_error();
}

extern "C" int rcomp_net_socket_get_linger(rcomp_net_socket socket,
                                             rcomp_net_linger* out_value) {
    if (!out_value) return kWsaEfault;
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    SceNetLinger l{};
    uint32_t len = sizeof(l);
    if (sceNetGetsockopt(s, kSolSocket, kSoLinger, &l, &len) != 0) return last_error();
    out_value->enabled = l.l_onoff != 0;
    out_value->seconds = l.l_linger;
    return 0;
}

extern "C" int rcomp_net_socket_ioctl(rcomp_net_socket socket, enum rcomp_net_ioctl_op op,
                                       uint32_t* inout_value) {
    if (!inout_value) return kWsaEfault;
    int s;
    int e = socket_id(socket, &s);
    if (e) return e;
    if (op == RCOMP_NET_IOCTL_NONBLOCK) {
        const int enabled = *inout_value != 0;
        return sceNetSetsockopt(s, kSolSocket, kSoNbio, &enabled, sizeof(enabled)) == 0 ? 0
                                                                                        : last_error();
    }
    if (op == RCOMP_NET_IOCTL_BYTES_AVAILABLE) {
        SceNetSockInfo info{};
        const int rc = sceNetGetSockInfo(s, &info, 1, 0);
        if (rc < 0) return last_error();
        *inout_value = info.recv_queue_length < 0 ? 0u : static_cast<uint32_t>(info.recv_queue_length);
        return 0;
    }
    return kWsaEopnotsupp;
}

extern "C" int rcomp_net_poll(rcomp_net_pollfd* fds, uint32_t count, int64_t timeout_us,
                               uint32_t* out_ready) {
    constexpr uint32_t kMaxPoll = 192;
    if (!out_ready || (count && !fds)) return kWsaEfault;
    if (count > kMaxPoll || timeout_us < -1) return kWsaEinval;
    const int eid = sceNetEpollCreate("rcomp-select", 0);
    if (eid < 0) return last_error();

    int first_error = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int s;
        first_error = socket_id(fds[i].socket, &s);
        if (first_error) break;
        SceNetEpollEvent ev{};
        if (fds[i].events & RCOMP_NET_POLL_READ) ev.events |= kEpollIn;
        if (fds[i].events & RCOMP_NET_POLL_WRITE) ev.events |= kEpollOut;
        // ERR/HUP are reported independently by SceNet epoll.
        ev.data.u32 = i;
        fds[i].revents = 0;
        if (sceNetEpollControl(eid, kEpollCtlAdd, s, &ev) != 0) {
            first_error = last_error();
            break;
        }
    }

    uint32_t ready = 0;
    if (!first_error) {
        SceNetEpollEvent events[kMaxPoll ? kMaxPoll : 1]{};
        int timeout = -1;
        if (timeout_us >= 0) timeout = timeout_us > INT_MAX ? INT_MAX : static_cast<int>(timeout_us);
        const int n = sceNetEpollWait(eid, events, count ? static_cast<int>(count) : 1, timeout);
        if (n < 0) {
            first_error = last_error();
        } else {
            for (int j = 0; j < n; ++j) {
                const uint32_t i = events[j].data.u32;
                if (i >= count) {
                    first_error = kWsaSyscallFailure;
                    break;
                }
                uint32_t revents = 0;
                if (events[j].events & (kEpollIn | kEpollHup)) revents |= RCOMP_NET_POLL_READ;
                if (events[j].events & kEpollOut) revents |= RCOMP_NET_POLL_WRITE;
                if (events[j].events & (kEpollErr | kEpollHup)) revents |= RCOMP_NET_POLL_EXCEPTION;
                fds[i].revents |= revents & fds[i].events;
            }
            if (!first_error)
                for (uint32_t i = 0; i < count; ++i)
                    if (fds[i].revents) ++ready;
        }
    }

    if (sceNetEpollDestroy(eid) != 0 && !first_error) first_error = last_error();
    if (first_error) return first_error;
    *out_ready = ready;
    return 0;
}
