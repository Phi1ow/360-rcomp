// xam.xex NetDll Winsock subset used by statically recompiled titles.
//
// Guest-visible structures are marshalled explicitly: the PPC guest is
// big-endian while every supported host is little-endian. Native socket IDs
// never escape to the guest; generation-tagged handles prevent stale guest
// handles from becoming valid after a host descriptor is reused.
#include "rcomp/runtime/xam_net.h"

#include <pthread.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/net_platform.h"
#include "rcomp/runtime/runtime.h"
#include "hle_more.h"
#include "xam_net_internal.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kSocketError = 0xFFFFFFFFu;
constexpr uint32_t kInvalidSocket = 0xFFFFFFFFu;
constexpr int kWsaEfault = 10014;
constexpr int kWsaEinval = 10022;
constexpr int kWsaEmfile = 10024;
constexpr int kWsaEnoprotoopt = 10042;
constexpr int kWsaEopnotsupp = 10045;
constexpr int kWsaEnotsock = 10038;
constexpr int kWsaEafnosupport = 10047;
constexpr int kWsaEnobufs = 10055;
constexpr int kWsaEverNotSupported = 10092;
constexpr int kWsaNotInitialized = 10093;

constexpr uint32_t kSolSocket = 0xFFFFu;
constexpr uint32_t kIpProtoTcp = 6u;
constexpr uint32_t kSoReuseaddr = 0x0004u;
constexpr uint32_t kSoKeepalive = 0x0008u;
constexpr uint32_t kSoBroadcast = 0x0020u;
constexpr uint32_t kSoLinger = 0x0080u;
constexpr uint32_t kSoSndbuf = 0x1001u;
constexpr uint32_t kSoRcvbuf = 0x1002u;
constexpr uint32_t kSoSndtimeo = 0x1005u;
constexpr uint32_t kSoRcvtimeo = 0x1006u;
constexpr uint32_t kSoError = 0x1007u;
constexpr uint32_t kSoType = 0x1008u;
constexpr uint32_t kTcpNodelay = 1u;

// Xbox/WinSock ioctlsocket command values.
constexpr uint32_t kFionbio = 0x8004667Eu;
constexpr uint32_t kFionread = 0x4004667Fu;

constexpr uint32_t kHandleTag = 0xE1000000u;
constexpr uint32_t kHandleTagMask = 0xFF000000u;
constexpr uint32_t kMaxSockets = 256;
constexpr uint32_t kMaxFdSet = 64;
constexpr uint32_t kWsadataSize = 400;

// Public Xbox/XNet references agree on this compact byte-only startup layout.
// GTA IV constructs exactly 13 bytes, with size=13 and flags=1.
constexpr uint8_t kXnetStartupParamsSize = 13;
constexpr uint8_t kXnetStartupBypassSecurity = 0x01;
constexpr uint8_t kXnetStartupAllocateMaxDgram = 0x02;
constexpr uint8_t kXnetStartupAllocateMaxStream = 0x04;
constexpr uint8_t kXnetStartupDisablePeerEncryption = 0x08;
constexpr uint8_t kXnetStartupKnownFlags = kXnetStartupBypassSecurity |
                                           kXnetStartupAllocateMaxDgram |
                                           kXnetStartupAllocateMaxStream |
                                           kXnetStartupDisablePeerEncryption;
constexpr uint8_t kXnetDefaultMaxDgramSockets = 8;
constexpr uint8_t kXnetDefaultMaxStreamSockets = 32;
constexpr uint8_t kXnetDefaultRecvBufferKiB = 16;
constexpr uint8_t kXnetDefaultSendBufferKiB = 16;

struct XNetStartupConfig {
    std::array<uint8_t, kXnetStartupParamsSize> raw{};
    uint16_t max_dgram_sockets = 0;
    uint16_t max_stream_sockets = 0;
    uint32_t recv_buffer_bytes = 0;
    uint32_t send_buffer_bytes = 0;
};

pthread_once_t g_error_once = PTHREAD_ONCE_INIT;
pthread_key_t g_error_key{};
bool g_error_key_ok = false;
std::mutex g_guest_error_mu;
std::map<std::pair<uintptr_t, uint32_t>, uint32_t> g_guest_errors;

void make_error_key() { g_error_key_ok = pthread_key_create(&g_error_key, nullptr) == 0; }

void ensure_error_key() {
    pthread_once(&g_error_once, &make_error_key);
    // There is no process-global fallback: that would break WSA's required
    // per-thread last-error semantics and race guest threads.
    if (!g_error_key_ok)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "NetDll: pthread_key_create failed for WSA last-error");
}

void set_last_error(uint32_t error) {
    if (GuestThread* thread = current_guest_thread()) {
        const auto key = std::make_pair(reinterpret_cast<uintptr_t>(runtime()), thread->thread_id);
        std::lock_guard<std::mutex> lock(g_guest_error_mu);
        if (error) g_guest_errors[key] = error;
        else g_guest_errors.erase(key);
        return;
    }
    ensure_error_key();
    if (pthread_setspecific(g_error_key, reinterpret_cast<void*>(static_cast<uintptr_t>(error))) != 0)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "NetDll: pthread_setspecific failed for WSA last-error");
}

uint32_t get_last_error() {
    if (GuestThread* thread = current_guest_thread()) {
        const auto key = std::make_pair(reinterpret_cast<uintptr_t>(runtime()), thread->thread_id);
        std::lock_guard<std::mutex> lock(g_guest_error_mu);
        auto it = g_guest_errors.find(key);
        return it == g_guest_errors.end() ? 0u : it->second;
    }
    ensure_error_key();
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_getspecific(g_error_key)));
}

struct SocketSlot {
    rcomp_net_socket native = RCOMP_NET_INVALID_SOCKET;
    uint16_t generation = 1;
    uint16_t in_flight = 0;
    uint8_t type = 0;
    bool open = false;
    bool closing = false;
    bool close_in_progress = false;
    bool retired = false;
    bool listening = false;  // listen() succeeded (WSAEventSelect: FD_ACCEPT, not FD_READ/FD_CLOSE)
};

using SocketLease = netdll::Lease;

std::mutex g_net_mu;
std::condition_variable g_net_cv;
std::array<SocketSlot, kMaxSockets> g_sockets{};
uint32_t g_wsa_startup_refs = 0;
uint32_t g_xnet_startup_refs = 0;
XNetStartupConfig g_xnet_config{};
bool g_platform_started = false;
bool g_cleanup_active = false;
bool g_teardown_requested = false;

uint32_t make_handle(uint8_t index, uint16_t generation) {
    return kHandleTag | (uint32_t(generation) << 8) | index;
}

bool decode_handle(uint32_t handle, uint8_t* index, uint16_t* generation) {
    if ((handle & kHandleTagMask) != kHandleTag) return false;
    *index = static_cast<uint8_t>(handle);
    *generation = static_cast<uint16_t>((handle >> 8) & 0xFFFFu);
    return *generation != 0;
}

void invalidate_generation(SocketSlot& slot) {
    if (slot.generation == 0xFFFFu) {
        slot.retired = true;
    } else {
        ++slot.generation;
    }
}

bool winsock_ready_locked() {
    return g_platform_started && g_wsa_startup_refs != 0 && !g_cleanup_active && !g_teardown_requested;
}

uint32_t socket_count_locked(uint8_t type) {
    uint32_t count = 0;
    for (const SocketSlot& slot : g_sockets)
        if (slot.native != RCOMP_NET_INVALID_SOCKET && slot.type == type) ++count;
    return count;
}

int xnet_socket_quota_locked(uint8_t type) {
    if (g_xnet_startup_refs == 0) return 0;
    uint32_t limit = kMaxSockets;
    if (type == RCOMP_NET_SOCK_DGRAM) limit = g_xnet_config.max_dgram_sockets;
    if (type == RCOMP_NET_SOCK_STREAM) limit = g_xnet_config.max_stream_sockets;
    return socket_count_locked(type) >= limit ? kWsaEmfile : 0;
}

int apply_xnet_socket_defaults_locked(rcomp_net_socket native) {
    if (g_xnet_startup_refs == 0) return 0;
    int e = rcomp_net_socket_set_int_option(native, RCOMP_NET_OPT_RCVBUF,
                                            static_cast<int32_t>(g_xnet_config.recv_buffer_bytes));
    if (!e)
        e = rcomp_net_socket_set_int_option(native, RCOMP_NET_OPT_SNDBUF,
                                            static_cast<int32_t>(g_xnet_config.send_buffer_bytes));
    return e;
}

int insert_socket(rcomp_net_socket native, uint8_t type, uint32_t* out_handle) {
    std::lock_guard<std::mutex> lock(g_net_mu);
    if (!winsock_ready_locked()) return kWsaNotInitialized;
    int e = xnet_socket_quota_locked(type);
    if (e) return e;
    e = apply_xnet_socket_defaults_locked(native);
    if (e) return e;
    for (uint32_t i = 0; i < g_sockets.size(); ++i) {
        SocketSlot& slot = g_sockets[i];
        if (slot.retired || slot.open || slot.closing || slot.close_in_progress ||
            slot.native != RCOMP_NET_INVALID_SOCKET)
            continue;
        slot.native = native;
        slot.type = type;
        slot.listening = false;
        slot.open = true;
        *out_handle = make_handle(static_cast<uint8_t>(i), slot.generation);
        return 0;
    }
    return kWsaEmfile;
}

int open_guest_socket(int32_t family, int32_t type, int32_t protocol, uint32_t* out_handle) {
    std::lock_guard<std::mutex> lock(g_net_mu);
    if (!winsock_ready_locked()) return kWsaNotInitialized;
    int e = xnet_socket_quota_locked(static_cast<uint8_t>(type));
    if (e) return e;
    rcomp_net_socket native = RCOMP_NET_INVALID_SOCKET;
    e = rcomp_net_socket_open(family, type, protocol, &native);
    if (e) return e;
    e = apply_xnet_socket_defaults_locked(native);
    if (e) {
        (void)rcomp_net_socket_close(native);
        return e;
    }
    for (uint32_t i = 0; i < g_sockets.size(); ++i) {
        SocketSlot& slot = g_sockets[i];
        if (slot.retired || slot.open || slot.closing || slot.close_in_progress ||
            slot.native != RCOMP_NET_INVALID_SOCKET)
            continue;
        slot.native = native;
        slot.type = static_cast<uint8_t>(type);
        slot.listening = false;
        slot.open = true;
        *out_handle = make_handle(static_cast<uint8_t>(i), slot.generation);
        return 0;
    }
    (void)rcomp_net_socket_close(native);
    return kWsaEmfile;
}

int acquire_socket(uint32_t handle, SocketLease* out) {
    std::lock_guard<std::mutex> lock(g_net_mu);
    if (!winsock_ready_locked()) return kWsaNotInitialized;
    uint8_t index = 0;
    uint16_t generation = 0;
    if (!decode_handle(handle, &index, &generation)) return kWsaEnotsock;
    SocketSlot& slot = g_sockets[index];
    if (!slot.open || slot.closing || slot.generation != generation ||
        slot.native == RCOMP_NET_INVALID_SOCKET)
        return kWsaEnotsock;
    if (slot.in_flight == UINT16_MAX) return kWsaEmfile;
    ++slot.in_flight;
    *out = {index, slot.native, true};
    return 0;
}

void finish_native_close(uint8_t index, rcomp_net_socket native) {
    (void)rcomp_net_socket_close(native);
    std::lock_guard<std::mutex> lock(g_net_mu);
    SocketSlot& slot = g_sockets[index];
    if (slot.native == native) {
        slot.native = RCOMP_NET_INVALID_SOCKET;
        slot.type = 0;
        slot.closing = false;
        slot.close_in_progress = false;
    }
    g_net_cv.notify_all();
}

void release_socket(SocketLease* lease) {
    if (!lease || !lease->valid) return;
    rcomp_net_socket close_native = RCOMP_NET_INVALID_SOCKET;
    uint8_t close_index = lease->index;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        SocketSlot& slot = g_sockets[lease->index];
        if (slot.in_flight) --slot.in_flight;
        if (slot.in_flight == 0 && slot.closing && !slot.close_in_progress && !g_cleanup_active &&
            !g_teardown_requested &&
            slot.native != RCOMP_NET_INVALID_SOCKET) {
            slot.close_in_progress = true;
            close_native = slot.native;
        }
        g_net_cv.notify_all();
    }
    lease->valid = false;
    if (close_native != RCOMP_NET_INVALID_SOCKET) finish_native_close(close_index, close_native);
}

int close_guest_socket(uint32_t handle) {
    rcomp_net_socket native = RCOMP_NET_INVALID_SOCKET;
    uint8_t index = 0;
    bool close_now = false;
    bool interrupt = false;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        if (!winsock_ready_locked()) return kWsaNotInitialized;
        uint16_t generation = 0;
        if (!decode_handle(handle, &index, &generation)) return kWsaEnotsock;
        SocketSlot& slot = g_sockets[index];
        if (!slot.open || slot.closing || slot.generation != generation ||
            slot.native == RCOMP_NET_INVALID_SOCKET)
            return kWsaEnotsock;
        native = slot.native;
        slot.open = false;
        slot.closing = true;
        invalidate_generation(slot);
        if (slot.in_flight == 0) {
            slot.close_in_progress = true;
            close_now = true;
        } else {
            interrupt = true;
        }
    }
    if (interrupt) {
        // Wake a blocking recv/accept. The final lease closes the native ID.
        (void)rcomp_net_socket_cancel(native);
        return 0;
    }
    const int e = rcomp_net_socket_close(native);
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        SocketSlot& slot = g_sockets[index];
        if (slot.native == native) {
            slot.native = RCOMP_NET_INVALID_SOCKET;
            slot.type = 0;
            slot.closing = false;
            slot.close_in_progress = false;
            if (e) slot.retired = true;
        }
        g_net_cv.notify_all();
    }
    (void)close_now;
    return e;
}

void cleanup_winsock_sockets() {
    std::vector<rcomp_net_socket> interrupt;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        for (SocketSlot& slot : g_sockets) {
            if (slot.open) {
                slot.open = false;
                slot.closing = true;
                invalidate_generation(slot);
            }
            if (slot.in_flight && slot.native != RCOMP_NET_INVALID_SOCKET) interrupt.push_back(slot.native);
        }
    }
    for (rcomp_net_socket native : interrupt) (void)rcomp_net_socket_cancel(native);

    std::vector<std::pair<uint8_t, rcomp_net_socket>> to_close;
    {
        std::unique_lock<std::mutex> lock(g_net_mu);
        g_net_cv.wait(lock, [] {
            if (g_teardown_requested) return true;
            for (const SocketSlot& slot : g_sockets)
                if (slot.in_flight || slot.close_in_progress) return false;
            return true;
        });
        if (g_teardown_requested) {
            // Title teardown owns the final descriptor close and platform term.
            // Do not make a guest WSACleanup race that phase or hold up quiesce.
            g_cleanup_active = false;
            g_net_cv.notify_all();
            return;
        }
        for (uint32_t i = 0; i < g_sockets.size(); ++i) {
            SocketSlot& slot = g_sockets[i];
            if (slot.native == RCOMP_NET_INVALID_SOCKET) continue;
            slot.close_in_progress = true;
            to_close.emplace_back(static_cast<uint8_t>(i), slot.native);
        }
    }
    for (const auto& item : to_close) finish_native_close(item.first, item.second);

    bool stop_platform = false;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        stop_platform = g_platform_started && g_xnet_startup_refs == 0;
    }
    if (stop_platform) rcomp_net_platform_cleanup();
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        if (stop_platform) g_platform_started = false;
        g_cleanup_active = false;
        g_net_cv.notify_all();
    }
}

int start_winsock() {
    std::unique_lock<std::mutex> lock(g_net_mu);
    g_net_cv.wait(lock, [] { return !g_cleanup_active || g_teardown_requested; });
    if (g_teardown_requested) return kWsaNotInitialized;
    if (g_wsa_startup_refs == UINT32_MAX) return kWsaEnobufs;
    if (!g_platform_started) {
        const int e = rcomp_net_platform_startup();
        if (e) return e;
        g_platform_started = true;
    }
    ++g_wsa_startup_refs;
    return 0;
}

int cleanup_winsock() {
    {
        std::unique_lock<std::mutex> lock(g_net_mu);
        g_net_cv.wait(lock, [] { return !g_cleanup_active || g_teardown_requested; });
        if (g_teardown_requested) return kWsaNotInitialized;
        if (!g_platform_started || g_wsa_startup_refs == 0) return kWsaNotInitialized;
        if (--g_wsa_startup_refs != 0) return 0;
        g_cleanup_active = true;
    }
    cleanup_winsock_sockets();
    return 0;
}

bool parse_xnet_startup(const uint8_t* raw, XNetStartupConfig* out) {
    XNetStartupConfig config{};
    if (raw) {
        std::memcpy(config.raw.data(), raw, kXnetStartupParamsSize);
        if (raw[0] != kXnetStartupParamsSize || (raw[1] & ~kXnetStartupKnownFlags) != 0) return false;
    }
    const uint8_t flags = raw ? raw[1] : 0;
    // A zero count/size selects the documented default; the allocate flags
    // request the documented maximum instead of a smaller title-selected value.
    uint32_t dgram = raw && raw[2] ? raw[2] : kXnetDefaultMaxDgramSockets;
    uint32_t stream = raw && raw[3] ? raw[3] : kXnetDefaultMaxStreamSockets;
    if (flags & kXnetStartupAllocateMaxDgram) dgram = 255;
    if (flags & kXnetStartupAllocateMaxStream) stream = 255;
    config.max_dgram_sockets = static_cast<uint16_t>(dgram);
    config.max_stream_sockets = static_cast<uint16_t>(stream);
    config.recv_buffer_bytes = 1024u * (raw && raw[4] ? raw[4] : kXnetDefaultRecvBufferKiB);
    config.send_buffer_bytes = 1024u * (raw && raw[5] ? raw[5] : kXnetDefaultSendBufferKiB);
    *out = config;
    return true;
}

int start_xnet(const XNetStartupConfig& config) {
    std::unique_lock<std::mutex> lock(g_net_mu);
    g_net_cv.wait(lock, [] { return !g_cleanup_active || g_teardown_requested; });
    if (g_teardown_requested) return kWsaNotInitialized;
    if (g_xnet_startup_refs == UINT32_MAX) return kWsaEnobufs;
    if (!g_platform_started) {
        const int e = rcomp_net_platform_startup();
        if (e) return e;
        g_platform_started = true;
    }
    if (g_xnet_startup_refs == 0) g_xnet_config = config;
    ++g_xnet_startup_refs;
    return 0;
}

int cleanup_xnet() {
    bool stop_platform = false;
    bool last_reference = false;
    {
        std::unique_lock<std::mutex> lock(g_net_mu);
        g_net_cv.wait(lock, [] { return !g_cleanup_active || g_teardown_requested; });
        if (g_teardown_requested || g_xnet_startup_refs == 0) return kWsaNotInitialized;
        last_reference = --g_xnet_startup_refs == 0;
        if (last_reference && g_wsa_startup_refs == 0 && g_platform_started) {
            g_cleanup_active = true;
            stop_platform = true;
        }
    }
    // XNetCleanup unregisters every key the title registered (XNetRegisterKey).
    if (last_reference) netdll::xnet_stopped();
    if (stop_platform) {
        cleanup_winsock_sockets();
    }
    return 0;
}

Runtime* rt() { return runtime(); }

bool guest_range(uint32_t address, uint32_t size, Protect access, uint8_t** out) {
    if (size == 0) {
        *out = nullptr;
        return true;
    }
    Runtime* r = rt();
    if (!r || !r->mem || !address || !r->mem->is_accessible(address, size, access)) return false;
    *out = r->mem->translate(address, size);
    return *out != nullptr;
}

bool write_be16(uint32_t address, uint16_t value) {
    uint8_t* p = nullptr;
    if (!guest_range(address, 2, Protect::ReadWrite, &p)) return false;
    value = __builtin_bswap16(value);
    memcpy(p, &value, sizeof(value));
    return true;
}

int read_sockaddr(uint32_t address, uint32_t length, rcomp_net_addr4* out) {
    if (!out || !address || length < 16) return kWsaEfault;
    uint8_t* unused = nullptr;
    if (!guest_range(address, 16, Protect::Read, &unused)) return kWsaEfault;
    uint16_t family = 0, port = 0;
    uint32_t ipv4 = 0;
    if (!guest_read_be16(address, &family) || !guest_read_be16(address + 2, &port) ||
        !guest_read_be32(address + 4, &ipv4))
        return kWsaEfault;
    if (family != RCOMP_NET_AF_INET) return kWsaEafnosupport;
    out->address = ipv4;
    out->port = port;
    out->reserved = 0;
    return 0;
}

int validate_sockaddr_output(uint32_t address, uint32_t length) {
    if (!address || length < 16) return kWsaEfault;
    uint8_t* unused = nullptr;
    return guest_range(address, 16, Protect::ReadWrite, &unused) ? 0 : kWsaEfault;
}

bool write_sockaddr(uint32_t address, const rcomp_net_addr4& value) {
    uint8_t* p = nullptr;
    if (!guest_range(address, 16, Protect::ReadWrite, &p)) return false;
    memset(p, 0, 16);
    return write_be16(address, RCOMP_NET_AF_INET) && write_be16(address + 2, value.port) &&
           guest_write_be32(address + 4, value.address);
}

void fail_socket(PPCContext& ctx, int error) {
    set_last_error(static_cast<uint32_t>(error));
    ctx.r3.u64 = kSocketError;
}

bool validate_message_flags(uint32_t flags) {
    constexpr uint32_t allowed = RCOMP_NET_MSG_OOB | RCOMP_NET_MSG_PEEK | RCOMP_NET_MSG_DONTROUTE |
                                 RCOMP_NET_MSG_WAITALL;
    return (flags & ~allowed) == 0;
}

int option_from_guest(uint32_t level, uint32_t name, rcomp_net_option* out) {
    if (level == kIpProtoTcp && name == kTcpNodelay) {
        *out = RCOMP_NET_OPT_TCP_NODELAY;
        return 0;
    }
    if (level != kSolSocket) return kWsaEnoprotoopt;
    switch (name) {
    case kSoReuseaddr: *out = RCOMP_NET_OPT_REUSEADDR; return 0;
    case kSoKeepalive: *out = RCOMP_NET_OPT_KEEPALIVE; return 0;
    case kSoBroadcast: *out = RCOMP_NET_OPT_BROADCAST; return 0;
    case kSoSndbuf: *out = RCOMP_NET_OPT_SNDBUF; return 0;
    case kSoRcvbuf: *out = RCOMP_NET_OPT_RCVBUF; return 0;
    case kSoSndtimeo: *out = RCOMP_NET_OPT_SNDTIMEO; return 0;
    case kSoRcvtimeo: *out = RCOMP_NET_OPT_RCVTIMEO; return 0;
    case kSoError: *out = RCOMP_NET_OPT_ERROR; return 0;
    case kSoType: *out = RCOMP_NET_OPT_TYPE; return 0;
    case kSoLinger: *out = RCOMP_NET_OPT_LINGER; return 0;
    default: return kWsaEnoprotoopt;
    }
}

struct GuestFdSet {
    uint32_t address = 0;
    std::vector<uint32_t> handles;
};

int read_fd_set(uint32_t address, GuestFdSet* out) {
    out->address = address;
    out->handles.clear();
    if (!address) return 0;
    uint32_t count = 0;
    if (!guest_read_be32(address, &count) || count > kMaxFdSet) return kWsaEfault;
    uint8_t* unused = nullptr;
    const uint32_t bytes = 4 + count * 4;
    if (!guest_range(address, bytes, Protect::ReadWrite, &unused)) return kWsaEfault;
    out->handles.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t handle = 0;
        if (!guest_read_be32(address + 4 + i * 4, &handle)) return kWsaEfault;
        out->handles.push_back(handle);
    }
    return 0;
}

bool write_fd_set(const GuestFdSet& set, const std::vector<rcomp_net_pollfd>& poll,
                  const std::vector<uint32_t>& poll_handles, uint32_t event) {
    if (!set.address) return true;
    uint32_t out_count = 0;
    for (uint32_t handle : set.handles) {
        for (size_t i = 0; i < poll.size(); ++i) {
            if (poll_handles[i] == handle && (poll[i].revents & event)) {
                if (!guest_write_be32(set.address + 4 + out_count * 4, handle)) return false;
                ++out_count;
                break;
            }
        }
    }
    return guest_write_be32(set.address, out_count);
}

int add_poll_handles(const GuestFdSet& set, uint32_t event, std::vector<rcomp_net_pollfd>* poll,
                     std::vector<uint32_t>* handles, std::vector<SocketLease>* leases) {
    for (uint32_t handle : set.handles) {
        size_t existing = handles->size();
        for (size_t i = 0; i < handles->size(); ++i) {
            if ((*handles)[i] == handle) {
                existing = i;
                break;
            }
        }
        if (existing < handles->size()) {
            (*poll)[existing].events |= event;
            continue;
        }
        SocketLease lease{};
        int e = acquire_socket(handle, &lease);
        if (e) return e;
        handles->push_back(handle);
        poll->push_back({lease.native, event, 0});
        leases->push_back(lease);
    }
    return 0;
}

void release_all(std::vector<SocketLease>* leases) {
    for (SocketLease& lease : *leases) release_socket(&lease);
    leases->clear();
}

int poll_cancellation_error(const std::vector<SocketLease>& leases) {
    std::lock_guard<std::mutex> lock(g_net_mu);
    if (g_teardown_requested || g_cleanup_active || !g_platform_started || g_wsa_startup_refs == 0)
        return kWsaNotInitialized;
    for (const SocketLease& lease : leases) {
        if (!lease.valid) continue;
        const SocketSlot& slot = g_sockets[lease.index];
        if (slot.closing || !slot.open || slot.native != lease.native) return kWsaEnotsock;
    }
    return 0;
}

int64_t read_timeout_us(uint32_t address, int* error) {
    *error = 0;
    if (!address) return -1;
    uint32_t sec_u = 0, usec_u = 0;
    if (!guest_read_be32(address, &sec_u) || !guest_read_be32(address + 4, &usec_u)) {
        *error = kWsaEfault;
        return 0;
    }
    const int32_t sec = static_cast<int32_t>(sec_u);
    const int32_t usec = static_cast<int32_t>(usec_u);
    if (sec < 0 || usec < 0 || usec >= 1000000) {
        *error = kWsaEinval;
        return 0;
    }
    return int64_t(sec) * 1000000 + usec;
}

bool write_wsadata(uint32_t address, uint16_t negotiated) {
    uint8_t* p = nullptr;
    if (!guest_range(address, kWsadataSize, Protect::ReadWrite, &p)) return false;
    // Preserve padding only; all defined output fields are deterministic.
    if (!write_be16(address + 0, negotiated) || !write_be16(address + 2, 0x0202)) return false;
    p[4] = 0;       // szDescription[0]
    p[261] = 0;     // szSystemStatus[0]
    if (!write_be16(address + 390, 0) || !write_be16(address + 392, 0) ||
        !guest_write_be32(address + 396, 0))
        return false;
    return true;
}

bool negotiate_version(uint16_t requested, uint16_t* negotiated) {
    const uint8_t major = static_cast<uint8_t>(requested & 0xFFu);
    const uint8_t minor = static_cast<uint8_t>(requested >> 8);
    if ((major == 1 && minor <= 1) || (major == 2 && minor <= 2)) {
        *negotiated = requested;
        return true;
    }
    return false;
}

bool read_guest_cstr(uint32_t address, char* out, size_t capacity) {
    if (!address || !out || capacity < 2) return false;
    Runtime* r = rt();
    if (!r || !r->mem) return false;
    for (size_t i = 0; i + 1 < capacity; ++i) {
        if (!r->mem->is_accessible(uint64_t(address) + i, 1, Protect::Read)) return false;
        uint8_t* p = r->mem->translate(address + static_cast<uint32_t>(i), 1);
        if (!p) return false;
        out[i] = static_cast<char>(*p);
        if (out[i] == 0) return true;
    }
    out[capacity - 1] = 0;
    return false;
}

bool parse_legacy_ipv4(const char* text, uint32_t* out) {
    // inet_addr is older than inet_pton: WinSock accepts the historical
    // a.b.c.d / a.b.c / a.b / a forms and C integer bases. Preserve that
    // behavior explicitly instead of inheriting host libc parser differences.
    uint64_t parts[4]{};
    uint32_t count = 0;
    const char* p = text;
    while (*p) {
        if (count == 4 || *p == '-' || *p == '+') return false;
        errno = 0;
        char* end = nullptr;
        const unsigned long value = strtoul(p, &end, 0);
        if (end == p || errno == ERANGE || static_cast<uint64_t>(value) > 0xFFFFFFFFull) return false;
        parts[count++] = value;
        if (*end == 0) break;
        if (*end != '.' || end[1] == 0) return false;
        p = end + 1;
    }
    if (count == 0) return false;
    uint64_t address = 0;
    switch (count) {
    case 1:
        address = parts[0];
        break;
    case 2:
        if (parts[0] > 0xFF || parts[1] > 0xFFFFFF) return false;
        address = (parts[0] << 24) | parts[1];
        break;
    case 3:
        if (parts[0] > 0xFF || parts[1] > 0xFF || parts[2] > 0xFFFF) return false;
        address = (parts[0] << 24) | (parts[1] << 16) | parts[2];
        break;
    case 4:
        for (uint64_t part : parts)
            if (part > 0xFF) return false;
        address = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
        break;
    default:
        return false;
    }
    *out = static_cast<uint32_t>(address);
    return true;
}

// NetDll_WSAStartup(caller, version, WSADATA*).
void WSAStartup(PPCContext& ctx, uint8_t*) {
    const uint16_t requested = static_cast<uint16_t>(ctx.r4.u32);
    uint16_t negotiated = 0;
    if (!negotiate_version(requested, &negotiated)) {
        ctx.r3.u64 = kWsaEverNotSupported;
        return;
    }
    if (!ctx.r5.u32 || !write_wsadata(ctx.r5.u32, negotiated)) {
        ctx.r3.u64 = kWsaEfault;
        return;
    }
    const int e = start_winsock();
    ctx.r3.u64 = static_cast<uint32_t>(e);
}

void WSACleanup(PPCContext& ctx, uint8_t*) {
    const int e = cleanup_winsock();
    if (e) {
        fail_socket(ctx, e);
        return;
    }
    ctx.r3.u64 = 0;
}

void WSAGetLastError(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = get_last_error(); }

void WSASetLastError(PPCContext& ctx, uint8_t*) { set_last_error(ctx.r3.u32); }

void socket_impl(PPCContext& ctx, uint8_t*) {
    uint32_t handle = 0;
    const int e = open_guest_socket(static_cast<int32_t>(ctx.r4.u32),
                                    static_cast<int32_t>(ctx.r5.u32),
                                    static_cast<int32_t>(ctx.r6.u32), &handle);
    if (e) {
        fail_socket(ctx, e);
        return;
    }
    ctx.r3.u64 = handle;
}

void closesocket(PPCContext& ctx, uint8_t*) {
    const int e = close_guest_socket(ctx.r4.u32);
    if (e) {
        fail_socket(ctx, e);
        return;
    }
    // Closing cancels the WSAEventSelect association and the socket's queued overlapped requests.
    netdll::socket_closed(ctx.r4.u32);
    ctx.r3.u64 = 0;
}

void shutdown_impl(PPCContext& ctx, uint8_t*) {
    SocketLease lease{};
    int e = acquire_socket(ctx.r4.u32, &lease);
    if (!e) e = rcomp_net_socket_shutdown(lease.native, static_cast<int32_t>(ctx.r5.u32));
    release_socket(&lease);
    if (e) fail_socket(ctx, e); else ctx.r3.u64 = 0;
}

void bind_impl(PPCContext& ctx, uint8_t*) {
    rcomp_net_addr4 addr{};
    int e = read_sockaddr(ctx.r5.u32, ctx.r6.u32, &addr);
    SocketLease lease{};
    if (!e) e = acquire_socket(ctx.r4.u32, &lease);
    if (!e) e = rcomp_net_socket_bind(lease.native, &addr);
    release_socket(&lease);
    if (e) fail_socket(ctx, e); else ctx.r3.u64 = 0;
}

void connect_impl(PPCContext& ctx, uint8_t*) {
    rcomp_net_addr4 addr{};
    int e = read_sockaddr(ctx.r5.u32, ctx.r6.u32, &addr);
    SocketLease lease{};
    if (!e) e = acquire_socket(ctx.r4.u32, &lease);
    if (!e) e = rcomp_net_socket_connect(lease.native, &addr);
    release_socket(&lease);
    // WinSock: a nonblocking connect that cannot complete at once fails with WSAEWOULDBLOCK (the
    // backends report the POSIX EINPROGRESS as WSAEINPROGRESS, which WinSock reserves for blocking calls).
    if (e == netdll::kWsaEinprogress) e = netdll::kWsaEwouldblock;
    if (e == 0 || e == netdll::kWsaEwouldblock || e == netdll::kWsaEinprogress)
        netdll::connect_started(ctx.r4.u32);
    if (e) fail_socket(ctx, e); else ctx.r3.u64 = 0;
}

void listen_impl(PPCContext& ctx, uint8_t*) {
    SocketLease lease{};
    int e = acquire_socket(ctx.r4.u32, &lease);
    if (!e) e = rcomp_net_socket_listen(lease.native, static_cast<int32_t>(ctx.r5.u32));
    if (!e) {
        std::lock_guard<std::mutex> lock(g_net_mu);
        SocketSlot& slot = g_sockets[lease.index];
        if (slot.native == lease.native) slot.listening = true;
    }
    release_socket(&lease);
    if (e) fail_socket(ctx, e); else ctx.r3.u64 = 0;
}

void accept_impl(PPCContext& ctx, uint8_t*) {
    const uint32_t addr = ctx.r5.u32;
    const uint32_t addrlen_ptr = ctx.r6.u32;
    uint32_t addrlen = 0;
    int e = 0;
    if (addr || addrlen_ptr) {
        if (!addr || !addrlen_ptr || !guest_read_be32(addrlen_ptr, &addrlen) ||
            validate_sockaddr_output(addr, addrlen) != 0) {
            fail_socket(ctx, kWsaEfault);
            return;
        }
        uint8_t* out = nullptr;
        if (!guest_range(addrlen_ptr, 4, Protect::ReadWrite, &out)) {
            fail_socket(ctx, kWsaEfault);
            return;
        }
    }
    SocketLease lease{};
    e = acquire_socket(ctx.r4.u32, &lease);
    rcomp_net_socket accepted = RCOMP_NET_INVALID_SOCKET;
    rcomp_net_addr4 peer{};
    if (!e) e = rcomp_net_socket_accept(lease.native, &accepted, addr ? &peer : nullptr);
    if (lease.valid) netdll::reenable_network_events(ctx.r4.u32, netdll::kFdAccept);
    if (e) {
        release_socket(&lease);
        fail_socket(ctx, e);
        return;
    }
    uint32_t handle = 0;
    e = insert_socket(accepted, RCOMP_NET_SOCK_STREAM, &handle);
    if (e) {
        (void)rcomp_net_socket_close(accepted);
        release_socket(&lease);
        fail_socket(ctx, e);
        return;
    }
    release_socket(&lease);
    if (addr && (!write_sockaddr(addr, peer) || !guest_write_be32(addrlen_ptr, 16))) {
        (void)close_guest_socket(handle);
        fail_socket(ctx, kWsaEfault);
        return;
    }
    ctx.r3.u64 = handle;
}

void name_impl(PPCContext& ctx, bool peer) {
    uint32_t addrlen = 0;
    uint8_t* out = nullptr;
    if (!ctx.r5.u32 || !ctx.r6.u32 || !guest_read_be32(ctx.r6.u32, &addrlen) ||
        validate_sockaddr_output(ctx.r5.u32, addrlen) != 0 ||
        !guest_range(ctx.r6.u32, 4, Protect::ReadWrite, &out)) {
        fail_socket(ctx, kWsaEfault);
        return;
    }
    SocketLease lease{};
    int e = acquire_socket(ctx.r4.u32, &lease);
    rcomp_net_addr4 addr{};
    if (!e) e = peer ? rcomp_net_socket_getpeername(lease.native, &addr)
                      : rcomp_net_socket_getsockname(lease.native, &addr);
    release_socket(&lease);
    if (e) {
        fail_socket(ctx, e);
        return;
    }
    if (!write_sockaddr(ctx.r5.u32, addr) || !guest_write_be32(ctx.r6.u32, 16)) {
        fail_socket(ctx, kWsaEfault);
        return;
    }
    ctx.r3.u64 = 0;
}

void getsockname(PPCContext& ctx, uint8_t*) { name_impl(ctx, false); }
void getpeername(PPCContext& ctx, uint8_t*) { name_impl(ctx, true); }

void setsockopt_impl(PPCContext& ctx, uint8_t*) {
    rcomp_net_option option{};
    int e = option_from_guest(ctx.r5.u32, ctx.r6.u32, &option);
    SocketLease lease{};
    if (!e) e = acquire_socket(ctx.r4.u32, &lease);
    if (!e) {
        if (option == RCOMP_NET_OPT_LINGER) {
            uint16_t enabled = 0, seconds = 0;
            if (ctx.r8.u32 < 4 || !guest_read_be16(ctx.r7.u32, &enabled) ||
                !guest_read_be16(ctx.r7.u32 + 2, &seconds)) {
                e = kWsaEfault;
            } else {
                rcomp_net_linger value{enabled != 0, seconds};
                e = rcomp_net_socket_set_linger(lease.native, &value);
            }
        } else {
            uint32_t value = 0;
            if (ctx.r8.u32 < 4 || !guest_read_be32(ctx.r7.u32, &value)) e = kWsaEfault;
            else e = rcomp_net_socket_set_int_option(lease.native, option, static_cast<int32_t>(value));
        }
    }
    release_socket(&lease);
    if (e) fail_socket(ctx, e); else ctx.r3.u64 = 0;
}

void getsockopt_impl(PPCContext& ctx, uint8_t*) {
    uint32_t optlen = 0;
    uint8_t* optlen_out = nullptr;
    if (!ctx.r7.u32 || !ctx.r8.u32 || !guest_read_be32(ctx.r8.u32, &optlen) ||
        !guest_range(ctx.r8.u32, 4, Protect::ReadWrite, &optlen_out)) {
        fail_socket(ctx, kWsaEfault);
        return;
    }
    rcomp_net_option option{};
    int e = option_from_guest(ctx.r5.u32, ctx.r6.u32, &option);
    SocketLease lease{};
    if (!e) e = acquire_socket(ctx.r4.u32, &lease);
    if (!e) {
        if (option == RCOMP_NET_OPT_LINGER) {
            if (optlen < 4) {
                e = kWsaEfault;
            } else {
                uint8_t* out = nullptr;
                if (!guest_range(ctx.r7.u32, 4, Protect::ReadWrite, &out)) {
                    e = kWsaEfault;
                } else {
                    rcomp_net_linger value{};
                    e = rcomp_net_socket_get_linger(lease.native, &value);
                    if (!e && (!write_be16(ctx.r7.u32, value.enabled ? 1 : 0) ||
                               !write_be16(ctx.r7.u32 + 2, static_cast<uint16_t>(value.seconds)) ||
                               !guest_write_be32(ctx.r8.u32, 4)))
                        e = kWsaEfault;
                }
            }
        } else {
            if (optlen < 4) {
                e = kWsaEfault;
            } else {
                uint8_t* out = nullptr;
                if (!guest_range(ctx.r7.u32, 4, Protect::ReadWrite, &out)) {
                    e = kWsaEfault;
                } else {
                    int32_t value = 0;
                    e = rcomp_net_socket_get_int_option(lease.native, option, &value);
                    if (!e && (!guest_write_be32(ctx.r7.u32, static_cast<uint32_t>(value)) ||
                               !guest_write_be32(ctx.r8.u32, 4)))
                        e = kWsaEfault;
                }
            }
        }
    }
    release_socket(&lease);
    if (e) fail_socket(ctx, e); else ctx.r3.u64 = 0;
}

void ioctlsocket_impl(PPCContext& ctx, uint8_t*) {
    if (!ctx.r6.u32) {
        fail_socket(ctx, kWsaEfault);
        return;
    }
    uint32_t value = 0;
    if (!guest_read_be32(ctx.r6.u32, &value)) {
        fail_socket(ctx, kWsaEfault);
        return;
    }
    rcomp_net_ioctl_op op{};
    // WSAEventSelect keeps the socket nonblocking while it selects network events.
    if (ctx.r5.u32 == kFionbio && value == 0 && netdll::event_select_active(ctx.r4.u32)) {
        fail_socket(ctx, kWsaEinval);
        return;
    }
    if (ctx.r5.u32 == kFionbio) op = RCOMP_NET_IOCTL_NONBLOCK;
    else if (ctx.r5.u32 == kFionread) op = RCOMP_NET_IOCTL_BYTES_AVAILABLE;
    else {
        fail_socket(ctx, kWsaEinval);
        return;
    }
    SocketLease lease{};
    int e = acquire_socket(ctx.r4.u32, &lease);
    if (!e) e = rcomp_net_socket_ioctl(lease.native, op, &value);
    release_socket(&lease);
    if (!e && op == RCOMP_NET_IOCTL_BYTES_AVAILABLE && !guest_write_be32(ctx.r6.u32, value)) e = kWsaEfault;
    if (e) fail_socket(ctx, e); else ctx.r3.u64 = 0;
}

void send_impl(PPCContext& ctx, uint8_t*) {
    if (!validate_message_flags(ctx.r7.u32)) {
        fail_socket(ctx, kWsaEinval);
        return;
    }
    uint8_t* data = nullptr;
    if (!guest_range(ctx.r5.u32, ctx.r6.u32, Protect::Read, &data)) {
        fail_socket(ctx, kWsaEfault);
        return;
    }
    SocketLease lease{};
    int e = acquire_socket(ctx.r4.u32, &lease);
    int32_t sent = 0;
    if (!e) e = rcomp_net_socket_send(lease.native, data, ctx.r6.u32, ctx.r7.u32, &sent);
    release_socket(&lease);
    if (e == netdll::kWsaEwouldblock) netdll::reenable_network_events(ctx.r4.u32, netdll::kFdWrite);
    if (e) fail_socket(ctx, e); else ctx.r3.u64 = static_cast<uint32_t>(sent);
}

void recv_impl(PPCContext& ctx, uint8_t*) {
    if (!validate_message_flags(ctx.r7.u32)) {
        fail_socket(ctx, kWsaEinval);
        return;
    }
    uint8_t* data = nullptr;
    if (!guest_range(ctx.r5.u32, ctx.r6.u32, Protect::ReadWrite, &data)) {
        fail_socket(ctx, kWsaEfault);
        return;
    }
    SocketLease lease{};
    int e = acquire_socket(ctx.r4.u32, &lease);
    int32_t received = 0;
    if (!e) e = rcomp_net_socket_recv(lease.native, data, ctx.r6.u32, ctx.r7.u32, &received);
    const bool attempted = lease.valid;
    release_socket(&lease);
    if (attempted)
        netdll::reenable_network_events(ctx.r4.u32, (ctx.r7.u32 & RCOMP_NET_MSG_OOB) ? netdll::kFdOob : netdll::kFdRead);
    if (e) fail_socket(ctx, e); else ctx.r3.u64 = static_cast<uint32_t>(received);
}

void sendto_impl(PPCContext& ctx, uint8_t*) {
    if (!validate_message_flags(ctx.r7.u32)) {
        fail_socket(ctx, kWsaEinval);
        return;
    }
    uint8_t* data = nullptr;
    rcomp_net_addr4 to{};
    int e = guest_range(ctx.r5.u32, ctx.r6.u32, Protect::Read, &data) ? 0 : kWsaEfault;
    if (!e) e = read_sockaddr(ctx.r8.u32, ctx.r9.u32, &to);
    SocketLease lease{};
    if (!e) e = acquire_socket(ctx.r4.u32, &lease);
    int32_t sent = 0;
    if (!e) e = rcomp_net_socket_sendto(lease.native, data, ctx.r6.u32, ctx.r7.u32, &to, &sent);
    release_socket(&lease);
    if (e == netdll::kWsaEwouldblock) netdll::reenable_network_events(ctx.r4.u32, netdll::kFdWrite);
    if (e) fail_socket(ctx, e); else ctx.r3.u64 = static_cast<uint32_t>(sent);
}

void recvfrom_impl(PPCContext& ctx, uint8_t*) {
    if (!validate_message_flags(ctx.r7.u32)) {
        fail_socket(ctx, kWsaEinval);
        return;
    }
    uint8_t* data = nullptr;
    int e = guest_range(ctx.r5.u32, ctx.r6.u32, Protect::ReadWrite, &data) ? 0 : kWsaEfault;
    uint32_t from_len = 0;
    if (!e && (ctx.r8.u32 || ctx.r9.u32)) {
        uint8_t* len_out = nullptr;
        if (!ctx.r8.u32 || !ctx.r9.u32 || !guest_read_be32(ctx.r9.u32, &from_len) ||
            validate_sockaddr_output(ctx.r8.u32, from_len) != 0 ||
            !guest_range(ctx.r9.u32, 4, Protect::ReadWrite, &len_out))
            e = kWsaEfault;
    }
    SocketLease lease{};
    if (!e) e = acquire_socket(ctx.r4.u32, &lease);
    rcomp_net_addr4 from{};
    int32_t received = 0;
    if (!e)
        e = rcomp_net_socket_recvfrom(lease.native, data, ctx.r6.u32, ctx.r7.u32,
                                      ctx.r8.u32 ? &from : nullptr, &received);
    const bool attempted = lease.valid;
    release_socket(&lease);
    if (attempted)
        netdll::reenable_network_events(ctx.r4.u32, (ctx.r7.u32 & RCOMP_NET_MSG_OOB) ? netdll::kFdOob : netdll::kFdRead);
    if (!e && ctx.r8.u32 && (!write_sockaddr(ctx.r8.u32, from) || !guest_write_be32(ctx.r9.u32, 16)))
        e = kWsaEfault;
    if (e) fail_socket(ctx, e); else ctx.r3.u64 = static_cast<uint32_t>(received);
}

void select_impl(PPCContext& ctx, uint8_t*) {
    GuestFdSet read_set{}, write_set{}, except_set{};
    int e = 0;
    if (!ctx.r5.u32 && !ctx.r6.u32 && !ctx.r7.u32) e = kWsaEinval;
    if (!e) e = read_fd_set(ctx.r5.u32, &read_set);
    if (!e) e = read_fd_set(ctx.r6.u32, &write_set);
    if (!e) e = read_fd_set(ctx.r7.u32, &except_set);
    int timeout_error = 0;
    const int64_t timeout_us = !e ? read_timeout_us(ctx.r8.u32, &timeout_error) : 0;
    if (!e) e = timeout_error;

    std::vector<rcomp_net_pollfd> poll;
    std::vector<uint32_t> handles;
    std::vector<SocketLease> leases;
    if (!e) e = add_poll_handles(read_set, RCOMP_NET_POLL_READ, &poll, &handles, &leases);
    if (!e) e = add_poll_handles(write_set, RCOMP_NET_POLL_WRITE, &poll, &handles, &leases);
    if (!e) e = add_poll_handles(except_set, RCOMP_NET_POLL_EXCEPTION, &poll, &handles, &leases);
    uint32_t ready = 0;
    if (!e && poll.empty()) e = kWsaEinval;
    if (!e) {
        constexpr int64_t kPollSliceUs = 50000;
        const bool infinite = timeout_us < 0;
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = infinite
                                  ? std::chrono::steady_clock::time_point::max()
                                  : started + std::chrono::microseconds(timeout_us);
        for (;;) {
            e = poll_cancellation_error(leases);
            if (e) break;

            int64_t slice_us = kPollSliceUs;
            if (!infinite) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) slice_us = 0;
                else {
                    const auto remaining =
                        std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
                    if (remaining < slice_us) slice_us = remaining;
                }
            }

            uint32_t iteration_ready = 0;
            e = rcomp_net_poll(poll.data(), static_cast<uint32_t>(poll.size()), slice_us,
                               &iteration_ready);
            if (e) break;
            e = poll_cancellation_error(leases);
            if (e) break;
            if (iteration_ready) {
                ready = iteration_ready;
                break;
            }
            if (!infinite && std::chrono::steady_clock::now() >= deadline) break;
        }
    }
    if (!e && (!write_fd_set(read_set, poll, handles, RCOMP_NET_POLL_READ) ||
               !write_fd_set(write_set, poll, handles, RCOMP_NET_POLL_WRITE) ||
               !write_fd_set(except_set, poll, handles, RCOMP_NET_POLL_EXCEPTION)))
        e = kWsaEfault;
    release_all(&leases);
    if (e) fail_socket(ctx, e); else ctx.r3.u64 = ready;
}

void inet_addr_impl(PPCContext& ctx, uint8_t*) {
    char text[64]{};
    if (!read_guest_cstr(ctx.r3.u32, text, sizeof(text))) {
        ctx.r3.u64 = kInvalidSocket;
        return;
    }
    if (text[0] == 0) {
        ctx.r3.u64 = 0;  // observed Xbox 360 legacy behavior
        return;
    }
    uint32_t address = 0;
    ctx.r3.u64 = parse_legacy_ipv4(text, &address) ? address : kInvalidSocket;
}

void XNetStartup(PPCContext& ctx, uint8_t*) {
    uint8_t raw[kXnetStartupParamsSize]{};
    const uint32_t params = ctx.r4.u32;
    XNetStartupConfig config{};
    if (params) {
        uint8_t* in = nullptr;
        if (!guest_range(params, kXnetStartupParamsSize, Protect::Read, &in)) {
            ctx.r3.u64 = static_cast<uint32_t>(kWsaEfault);
            return;
        }
        std::memcpy(raw, in, sizeof(raw));
    }
    if (!parse_xnet_startup(params ? raw : nullptr, &config)) {
        ctx.r3.u64 = static_cast<uint32_t>(kWsaEinval);
        return;
    }
    ctx.r3.u64 = static_cast<uint32_t>(start_xnet(config));
}

void XNetCleanup(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = static_cast<uint32_t>(cleanup_xnet());
}

// ---- XNet identity/link services: this console has no XNet link ---------
// R-comp exposes no Xbox Live/system-link network. The link is down and the
// title has no XNADDR, which is the honest result of these queries; the
// address-mapping calls therefore have nothing to map. Values: public XDK
// XNet constants (XNET_GET_XNADDR_NONE = 1, link status 0 = no link).
constexpr uint32_t kXnetGetXnAddrNone = 0x00000001u;
constexpr uint32_t kXnetConnectStatusIdle = 0x00000000u;
constexpr uint32_t kXnAddrSize = 36;

// NetDll_XNetGetEthernetLinkStatus(XNCALLER_TYPE): 0 = no active link.
void XNetGetEthernetLinkStatus(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }

// NetDll_XNetGetTitleXnAddr(XNCALLER_TYPE, XNADDR*): zeroed XNADDR, NONE.
void XNetGetTitleXnAddr(PPCContext& ctx, uint8_t*) {
    const uint32_t xnaddr = ctx.r4.u32;
    uint8_t* out = nullptr;
    if (!xnaddr || !guest_range(xnaddr, kXnAddrSize, Protect::ReadWrite, &out)) {
        ctx.r3.u64 = kXnetGetXnAddrNone;
        return;
    }
    std::memset(out, 0, kXnAddrSize);
    ctx.r3.u64 = kXnetGetXnAddrNone;
}

// NetDll_XNetGetConnectStatus(XNCALLER_TYPE, const IN_ADDR): no connection
// was ever requested, so the address is idle.
void XNetGetConnectStatus(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kXnetConnectStatusIdle; }

// NetDll_XNetXnAddrToInAddr / XNetServerToInAddr / XNetUnregisterInAddr map or
// release addresses in the XNet security table. With no XNet link nothing is
// ever registered: the request fails with WSAEINVAL.
void XNetNoRegisteredAddress(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = static_cast<uint32_t>(kWsaEinval); }

struct Impl {
    uint32_t ordinal;
    const char* name;
    PPCFunc* fn;
};

const Impl kImpls[] = {
    {0x0001, "NetDll_WSAStartup", &WSAStartup},
    {0x0002, "NetDll_WSACleanup", &WSACleanup},
    {0x0003, "NetDll_socket", &socket_impl},
    {0x0004, "NetDll_closesocket", &closesocket},
    {0x0005, "NetDll_shutdown", &shutdown_impl},
    {0x0006, "NetDll_ioctlsocket", &ioctlsocket_impl},
    {0x0007, "NetDll_setsockopt", &setsockopt_impl},
    {0x0008, "NetDll_getsockopt", &getsockopt_impl},
    {0x0009, "NetDll_getsockname", &getsockname},
    {0x000A, "NetDll_getpeername", &getpeername},
    {0x000B, "NetDll_bind", &bind_impl},
    {0x000C, "NetDll_connect", &connect_impl},
    {0x000D, "NetDll_listen", &listen_impl},
    {0x000E, "NetDll_accept", &accept_impl},
    {0x000F, "NetDll_select", &select_impl},
    {0x0012, "NetDll_recv", &recv_impl},
    {0x0014, "NetDll_recvfrom", &recvfrom_impl},
    {0x0016, "NetDll_send", &send_impl},
    {0x0018, "NetDll_sendto", &sendto_impl},
    {0x001A, "NetDll_inet_addr", &inet_addr_impl},
    {0x001B, "NetDll_WSAGetLastError", &WSAGetLastError},
    {0x001C, "NetDll_WSASetLastError", &WSASetLastError},
    {0x0033, "NetDll_XNetStartup", &XNetStartup},
    {0x0034, "NetDll_XNetCleanup", &XNetCleanup},
    {0x0039, "NetDll_XNetXnAddrToInAddr", &XNetNoRegisteredAddress},
    {0x003A, "NetDll_XNetServerToInAddr", &XNetNoRegisteredAddress},
    {0x003F, "NetDll_XNetUnregisterInAddr", &XNetNoRegisteredAddress},
    {0x0042, "NetDll_XNetGetConnectStatus", &XNetGetConnectStatus},
    {0x0049, "NetDll_XNetGetTitleXnAddr", &XNetGetTitleXnAddr},
    {0x004B, "NetDll_XNetGetEthernetLinkStatus", &XNetGetEthernetLinkStatus},
};

}  // namespace

Status register_xam_net_hle() {
    for (const Impl& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXam, impl.name, &ordinal) || ordinal != impl.ordinal) return Status::Conflict;
        const Status s = register_import(kModuleXam, impl.ordinal, impl.fn, impl.name);
        if (s != Status::Ok) return s;
    }
    const Status wsa = register_xam_net_wsa_hle();
    // XNet keys, connect, address and QoS queries: src/hle_xam_net_more.cpp.
    return wsa != Status::Ok ? wsa : register_xam_net_more_hle();
}

Status begin_shutdown_xam_net_hle() {
    std::vector<rcomp_net_socket> cancel;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        if (g_teardown_requested) return Status::Ok;
        g_teardown_requested = true;
        g_wsa_startup_refs = 0;
        g_xnet_startup_refs = 0;
        for (SocketSlot& slot : g_sockets) {
            if (slot.open) {
                slot.open = false;
                slot.closing = true;
                invalidate_generation(slot);
            }
            if (slot.in_flight && slot.native != RCOMP_NET_INVALID_SOCKET)
                cancel.push_back(slot.native);
        }
        g_net_cv.notify_all();
    }
    // Queued overlapped requests complete aborted and the WSA worker drops its leases.
    netdll::begin_shutdown_wsa();
    Status result = Status::Ok;
    for (rcomp_net_socket native : cancel)
        if (rcomp_net_socket_cancel(native) != 0) result = Status::IoError;
    return result;
}

Status shutdown_xam_net_hle() {
    const Status begin_status = begin_shutdown_xam_net_hle();
    // The WSA worker is joined first: it may hold socket leases until it observes the teardown.
    netdll::shutdown_wsa();
    std::vector<std::pair<uint8_t, rcomp_net_socket>> to_close;
    bool platform_started = false;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        if (g_cleanup_active) return Status::Conflict;
        for (const SocketSlot& slot : g_sockets)
            if (slot.in_flight || slot.close_in_progress) return Status::Conflict;
        for (uint32_t i = 0; i < g_sockets.size(); ++i) {
            SocketSlot& slot = g_sockets[i];
            if (slot.native == RCOMP_NET_INVALID_SOCKET) continue;
            slot.close_in_progress = true;
            to_close.emplace_back(static_cast<uint8_t>(i), slot.native);
        }
        platform_started = g_platform_started;
    }

    Status result = begin_status;
    for (const auto& item : to_close) {
        const int e = rcomp_net_socket_close(item.second);
        if (e && result == Status::Ok) result = Status::IoError;
        std::lock_guard<std::mutex> lock(g_net_mu);
        SocketSlot& slot = g_sockets[item.first];
        if (slot.native == item.second) {
            slot.native = RCOMP_NET_INVALID_SOCKET;
            slot.closing = false;
            slot.close_in_progress = false;
            if (e) slot.retired = true;
        }
    }
    if (platform_started) rcomp_net_platform_cleanup();

    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        g_platform_started = false;
        g_cleanup_active = false;
        g_teardown_requested = false;
        g_wsa_startup_refs = 0;
        g_xnet_startup_refs = 0;
        g_net_cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(g_guest_error_mu);
        g_guest_errors.clear();
    }
    set_last_error(0);
    return result;
}

// ---- contract with src/hle_xam_net_wsa.cpp (src/xam_net_internal.h) ----------------------------
namespace netdll {

int acquire(uint32_t handle, Lease* out, SocketInfo* info) {
    const int e = acquire_socket(handle, out);
    if (e || !info) return e;
    std::lock_guard<std::mutex> lock(g_net_mu);
    const SocketSlot& slot = g_sockets[out->index];
    info->type = slot.type;
    info->listening = slot.listening;
    return 0;
}

void release(Lease* lease) { release_socket(lease); }

int lease_error(const Lease& lease) {
    std::vector<SocketLease> one{lease};
    return poll_cancellation_error(one);
}

void set_error(uint32_t error) { set_last_error(error); }

bool guest_span(uint32_t address, uint32_t size, bool writable, uint8_t** out) {
    return guest_range(address, size, writable ? Protect::ReadWrite : Protect::Read, out);
}

int read_sockaddr(uint32_t address, uint32_t length, rcomp_net_addr4* out) {
    return rcomp::rt::read_sockaddr(address, length, out);
}

int check_sockaddr_output(uint32_t address, uint32_t length) {
    return validate_sockaddr_output(address, length);
}

bool write_sockaddr(uint32_t address, const rcomp_net_addr4& value) {
    return rcomp::rt::write_sockaddr(address, value);
}

bool message_flags_valid(uint32_t flags) { return validate_message_flags(flags); }

bool network_started() {
    std::lock_guard<std::mutex> lock(g_net_mu);
    return g_platform_started && (g_wsa_startup_refs || g_xnet_startup_refs) && !g_cleanup_active &&
           !g_teardown_requested;
}

bool xnet_started(uint32_t* key_capacity) {
    std::lock_guard<std::mutex> lock(g_net_mu);
    if (!g_platform_started || !g_xnet_startup_refs || g_cleanup_active || g_teardown_requested) return false;
    // XNetStartupParams.cfgKeyRegMax (byte 6); 0 selects the XDK default of 4 key pairs.
    if (key_capacity) *key_capacity = g_xnet_config.raw[6] ? g_xnet_config.raw[6] : 4u;
    return true;
}

}  // namespace netdll

}  // namespace rcomp::rt
