// xam.xex NetDll: overlapped WSA I/O, WSA events, WSAEventSelect and the XNet key/DNS/random services
// (owner: Agent 3, runtime/). Socket state, startup state and the WSA last error stay in
// src/hle_xam_net.cpp; this unit reaches them only through src/xam_net_internal.h.
//
// ABI sources. Argument registers come from the XDK import wrappers recompiled in Gears of War 2
// (4D53082D, ppc_recomp.156.cpp: each wrapper shifts the title's arguments up one register and puts
// XNCALLER_TITLE = 1 in r3 where the export takes a caller), cross-checked with Xenia 95a5c3e and
// Xenia Canary (src/xenia/kernel/xam/xam_net.cc; BSD-3, read only, no code copied):
//   WSARecv(caller, s, LPWSABUF, count, LPDWORD bytes, LPDWORD flags, LPWSAOVERLAPPED, completion)
//   WSARecvFrom(caller, s, LPWSABUF, count, LPDWORD bytes, LPDWORD flags, from, LPINT fromlen,
//               [stack 8] LPWSAOVERLAPPED, [stack 9] completion)
//   WSASend(caller, s, LPWSABUF, count, LPDWORD bytes, DWORD flags, LPWSAOVERLAPPED, completion)
//   WSASendTo(caller, s, LPWSABUF, count, LPDWORD bytes, DWORD flags, to, tolen,
//             [stack 8] LPWSAOVERLAPPED, [stack 9] completion)
//   WSAGetOverlappedResult(caller, s, LPWSAOVERLAPPED, LPDWORD bytes, BOOL wait, LPDWORD flags) -> BOOL
//   WSACancelOverlappedIO(caller, s) -> INT;  WSAEventSelect(caller, s, WSAEVENT, LONG events) -> INT
//   WSACreateEvent() -> WSAEVENT; WSACloseEvent/WSASetEvent/WSAResetEvent(WSAEVENT) -> BOOL
//   WSAWaitForMultipleEvents(count, const WSAEVENT*, BOOL wait_all, DWORD ms, BOOL alertable) -> DWORD
//   __WSAFDIsSet(SOCKET, fd_set*) -> INT;  XNetRandom(caller, BYTE*, UINT) -> INT
//   XNetRegisterKey(caller, const XNKID*, const XNKEY*) / XNetUnregisterKey(caller, const XNKID*) -> INT
//   XNetInAddrToString(caller, IN_ADDR (by value), char*, INT) -> INT
//   XNetDnsLookup(caller, const char*, WSAEVENT, XNDNS**) / XNetDnsRelease(caller, XNDNS*) -> INT
// The wrappers of WSAWaitForMultipleEvents, the WSA event calls and __WSAFDIsSet pass no caller.
// Layouts: WSABUF {u32 len, u32 buf}; WSAOVERLAPPED {+0 Internal, +4 InternalHigh, +8 Offset,
// +0xC OffsetHigh, +0x10 hEvent} (0x14 bytes); fd_set {u32 count, SOCKET array[64]}; XNDNS {+0 INT
// iStatus, +4 UINT cina, +8 IN_ADDR aina[8]} (0x28 bytes); XNKID 8 bytes, XNKEY 16 bytes.
//
// WSAEVENT is an ordinary kernel event handle on the Xbox 360 (Xenia creates a manual-reset XEvent and
// waits with NtWaitForMultipleObjectsEx): the WSA event calls go through the registered xboxkrnl
// NtCreateEvent / NtSetEvent / NtClearEvent / NtClose / NtWaitForMultipleObjectsEx, so a WSA event is
// the same dispatcher object a title can also wait on with KeWait* / NtWait*.
//
// Asynchronous model (runtime/docs/NETDLL.md, "Overlapped I/O and WSAEventSelect"):
//   * An overlapped request first checks readiness with a zero-timeout poll. If the transfer can run
//     it runs now and completes immediately (return 0, overlapped and event completed). Otherwise it is
//     queued with a socket lease, the overlapped reads STATUS_PENDING and the call fails with
//     WSA_IO_PENDING. One host worker thread polls every queued socket and performs the transfer when
//     the socket is ready; the overlapped and its event complete then. Cancellation (WSACancel-
//     OverlappedIO, closesocket, WSACleanup, title teardown) completes queued requests with
//     WSA_OPERATION_ABORTED.
//   * WSAEventSelect keeps a per-socket record with WinSock's re-enabling rules (FD_READ re-enabled by
//     a receive, FD_WRITE by a send that failed with WSAEWOULDBLOCK, FD_ACCEPT by accept, FD_OOB by an
//     MSG_OOB receive; FD_CONNECT and FD_CLOSE once). The same worker polls the sockets whose events
//     are armed and sets the event when one is recorded; selecting and re-enabling also evaluate the
//     socket at once, so conditions that already hold signal without waiting for the worker. A poll
//     observation older than the association's last re-enable is dropped (the accept/recv that
//     re-enabled it may have consumed the condition), and the end of stream is told from data by a
//     one-byte MSG_PEEK, never by an empty receive queue. The worker
//     sleeps in the platform poll for at most kWorkerSliceUs: a change of the watched set made while it
//     sleeps (new request, re-enabled event) is seen within that bound.
#include <stdio.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#if !defined(__PROSPERO__)
#include <sys/random.h>
#endif

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/handle_table.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/io_event.h"
#include "rcomp/runtime/runtime.h"
#include "host_random.h"
#include "xam_net_internal.h"

namespace rcomp::rt {
namespace {

using netdll::Lease;
using netdll::SocketInfo;

constexpr uint32_t kSocketError = 0xFFFFFFFFu;
constexpr uint32_t kStatusPending = 0x00000103u;  // WSAOVERLAPPED.Internal while the request runs
constexpr uint32_t kOverlappedBytes = 0x14;
constexpr uint32_t kOverlappedEvent = 0x10;
constexpr uint32_t kMaxWsaBuffers = 1024;
constexpr uint32_t kMaxTransfer = 16u << 20;  // larger gathers/scatters fail with WSAENOBUFS
constexpr uint32_t kMaxFdSetCount = 64;        // FD_SETSIZE
constexpr uint32_t kMaxWaitEvents = 64;        // WSA_MAXIMUM_WAIT_EVENTS
constexpr uint32_t kWaitFailed = 0xFFFFFFFFu;  // WSA_WAIT_FAILED
constexpr uint32_t kWaitInfinite = 0xFFFFFFFFu;
constexpr uint32_t kErrorInvalidHandle = 6;     // WSA_INVALID_HANDLE
constexpr uint32_t kErrorInvalidParameter = 87; // WSA_INVALID_PARAMETER
constexpr int64_t kWorkerSliceUs = 5000;
constexpr uint32_t kMaxPollEntries = 128;  // below both backends' 192-entry poll limit

// xboxkrnl ordinals of the kernel services the WSA event calls use.
constexpr uint32_t kOrdinalNtClearEvent = 0x00CE, kOrdinalNtClose = 0x00CF, kOrdinalNtCreateEvent = 0x00D1,
                   kOrdinalNtSetEvent = 0x00F6, kOrdinalNtWaitForMultipleObjectsEx = 0x00FE,
                   kOrdinalRtlNtStatusToDosError = 0x0135;

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s before runtime_init", fn);
    return *r;
}

bool writable(uint32_t address, uint32_t size) {
    uint8_t* unused = nullptr;
    return address && netdll::guest_span(address, size, true, &unused);
}
bool readable(uint32_t address, uint32_t size) {
    uint8_t* unused = nullptr;
    return address && netdll::guest_span(address, size, false, &unused);
}

void fail(PPCContext& ctx, int error) {
    netdll::set_error(static_cast<uint32_t>(error));
    ctx.r3.u64 = kSocketError;
}

// 9th+ arguments: r1 + 0x54 + 8*(n-8) in the caller frame (runtime/docs/RUNTIME.md).
uint32_t stack_arg(PPCContext& ctx, int n, const char* fn) {
    const uint64_t address = uint64_t(ctx.r1.u32) + 0x54 + 8u * uint32_t(n - 8);
    uint32_t value = 0;
    if (address > UINT32_MAX - 3 || !readable(uint32_t(address), 4) || !guest_read_be32(uint32_t(address), &value))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xam.xex!%s stack argument %d at 0x%08X not readable lr=0x%08X", fn, n,
                    uint32_t(address), uint32_t(ctx.lr));
    return value;
}

// ---- kernel services ---------------------------------------------------------------------------------
PPCFunc* kernel(uint32_t ordinal, const char* fn) {
    PPCFunc* f = find_import(kModuleXboxkrnl, ordinal);
    if (!f) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s needs xboxkrnl.exe ordinal 0x%04X registered", fn, ordinal);
    return f;
}

// Out-parameters of those kernel calls live in a frame this export builds below the caller's stack
// pointer, as the real XAM export would: past the 288-byte area the PPC ABI reserves under r1.
struct KernelFrame {
    alignas(64) PPCContext ctx;
    uint32_t scratch = 0;
};
void kernel_frame(PPCContext& caller, const char* fn, KernelFrame* out) {
    const uint32_t sp = caller.r1.u32;
    const uint32_t scratch = (sp - 0x180u) & ~0xFu;
    if (sp < 0x1000u || !writable(scratch, 0x40))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xam.xex!%s guest stack r1=0x%08X has no room for a frame lr=0x%08X", fn,
                    sp, uint32_t(caller.lr));
    out->ctx = caller;
    out->ctx.r1.u64 = scratch - 0x100u;
    out->scratch = scratch;
}

uint32_t dos_error(PPCContext& caller, uint8_t* base, uint32_t status, const char* fn) {
    KernelFrame f;
    kernel_frame(caller, fn, &f);
    f.ctx.r3.u64 = status;
    kernel(kOrdinalRtlNtStatusToDosError, fn)(f.ctx, base);
    return f.ctx.r3.u32;
}

// NTSTATUS of a one-handle kernel call (NtSetEvent with no previous state, NtClearEvent, NtClose).
uint32_t kernel_handle_call(PPCContext& caller, uint8_t* base, uint32_t ordinal, uint32_t handle, const char* fn) {
    KernelFrame f;
    kernel_frame(caller, fn, &f);
    f.ctx.r3.u64 = handle;
    f.ctx.r4.u64 = 0;
    kernel(ordinal, fn)(f.ctx, base);
    return f.ctx.r3.u32;
}

// ---- random bytes ------------------------------------------------------------------------------------
// The host source lives in src/host_random.h (shared with XNetCreateKey and XeCryptRandom).
bool random_bytes(uint8_t* out, uint32_t size) { return host_random_bytes(out, size); }

// ---- WSABUF transfers --------------------------------------------------------------------------------
enum class Kind : uint8_t { Recv, RecvFrom, Send, SendTo };
bool receives(Kind k) { return k == Kind::Recv || k == Kind::RecvFrom; }

struct Buffer {
    uint32_t address = 0, length = 0;
};

struct Request {
    Kind kind = Kind::Recv;
    uint32_t handle = 0;
    std::vector<Buffer> buffers;
    uint32_t total = 0;
    uint32_t flags = 0;
    uint32_t from = 0, fromlen = 0;  // RecvFrom outputs (optional)
    rcomp_net_addr4 to{};             // SendTo destination
    bool stream = false;              // the socket is a stream socket
    uint32_t done = 0;                // bytes of an overlapped stream send already transferred
};

int read_buffers(uint32_t array, uint32_t count, bool receive, Request* r) {
    if (count > kMaxWsaBuffers) return netdll::kWsaEnobufs;
    if (count && !readable(array, count * 8)) return netdll::kWsaEfault;
    uint64_t total = 0;
    r->buffers.clear();
    r->buffers.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Buffer b;
        guest_read_be32(array + 8 * i, &b.length);
        guest_read_be32(array + 8 * i + 4, &b.address);
        uint8_t* unused = nullptr;
        if (b.length && !netdll::guest_span(b.address, b.length, receive, &unused)) return netdll::kWsaEfault;
        total += b.length;
        if (total > kMaxTransfer) return netdll::kWsaEnobufs;
        r->buffers.push_back(b);
    }
    r->total = static_cast<uint32_t>(total);
    return 0;
}

// One transfer attempt on `native` with the socket's own blocking mode. Buffers are re-validated: an
// overlapped request reads or fills them when it runs, which can be after the call returned. A send
// starts after the r.done bytes earlier attempts transferred; *transferred counts this attempt only.
int perform(const Request& r, rcomp_net_socket native, int32_t* transferred) {
    *transferred = 0;
    const bool receive = receives(r.kind);
    uint8_t* direct = nullptr;
    std::vector<uint8_t> staging;
    if (r.buffers.size() == 1) {
        if (!netdll::guest_span(r.buffers[0].address, r.buffers[0].length, receive, &direct) && r.buffers[0].length)
            return netdll::kWsaEfault;
    } else {
        staging.resize(r.total);
        if (!receive) {
            uint32_t at = 0;
            for (const Buffer& b : r.buffers) {
                uint8_t* p = nullptr;
                if (b.length && !netdll::guest_span(b.address, b.length, false, &p)) return netdll::kWsaEfault;
                if (b.length) std::memcpy(staging.data() + at, p, b.length);
                at += b.length;
            }
        }
        direct = staging.data();
    }
    int e = 0;
    rcomp_net_addr4 from{};
    const uint32_t skip = receives(r.kind) ? 0 : std::min(r.done, r.total);
    uint8_t* const at = direct ? direct + skip : direct;
    switch (r.kind) {
    case Kind::Recv: e = rcomp_net_socket_recv(native, direct, r.total, r.flags, transferred); break;
    case Kind::RecvFrom:
        e = rcomp_net_socket_recvfrom(native, direct, r.total, r.flags, r.from ? &from : nullptr, transferred);
        break;
    case Kind::Send: e = rcomp_net_socket_send(native, at, r.total - skip, r.flags, transferred); break;
    case Kind::SendTo: e = rcomp_net_socket_sendto(native, at, r.total - skip, r.flags, &r.to, transferred); break;
    }
    if (e) return e;
    if (receive && r.buffers.size() != 1) {
        uint32_t left = static_cast<uint32_t>(*transferred), at = 0;
        for (const Buffer& b : r.buffers) {
            if (!left) break;
            const uint32_t n = std::min(left, b.length);
            uint8_t* p = nullptr;
            if (!netdll::guest_span(b.address, n, true, &p)) return netdll::kWsaEfault;
            std::memcpy(p, staging.data() + at, n);
            at += n;
            left -= n;
        }
    }
    if (r.kind == Kind::RecvFrom && r.from &&
        (!netdll::write_sockaddr(r.from, from) || !guest_write_be32(r.fromlen, 16)))
        return netdll::kWsaEfault;
    return 0;
}

// ---- shared state ------------------------------------------------------------------------------------
struct PendingOp {
    Request request;
    Lease lease;
    uint32_t overlapped = 0;
    std::shared_ptr<HandleObject> event;
    bool executing = false;  // owned by one thread that is running or completing it
};

struct Association {
    std::shared_ptr<HandleObject> event;
    uint32_t mask = 0;
    uint32_t enabled = 0;   // events that may be recorded now
    uint32_t recorded = 0;  // recorded since the association was made (no WSAEnumNetworkEvents export)
    uint64_t generation = 0;
    // Bumped by every re-enabling call. A poll observation made before a re-enable (the accept/recv that
    // consumed the condition may run between the poll and its interpretation) must not record anything.
    uint64_t epoch = 0;
    bool parked = false;    // readable with nothing to record: read side ignored until re-enabled
};

struct XnKey {
    uint8_t id[8];
    uint8_t key[16];
};

std::mutex g_mu;
std::condition_variable g_cv;  // worker wake-ups and overlapped completions
std::map<uint64_t, std::unique_ptr<PendingOp>> g_ops;
uint64_t g_next_op = 1;
std::map<uint32_t, Association> g_assoc;
std::set<uint32_t> g_connecting;
uint64_t g_next_generation = 1;
std::thread g_worker;
bool g_worker_running = false;
bool g_stop = false;
uint64_t g_round = 0;
std::vector<XnKey> g_keys;
std::set<uint32_t> g_dns_blocks;

bool has_work_locked() {
    for (const auto& item : g_ops)
        if (!item.second->executing) return true;
    for (const auto& item : g_assoc) {
        uint32_t armed = item.second.mask & item.second.enabled;
        // A parked read side waits for a receive to re-enable it (which wakes the worker).
        if (item.second.parked) armed &= ~(netdll::kFdRead | netdll::kFdClose);
        if (armed) return true;
    }
    return false;
}

void worker_main();

void ensure_worker_locked() {
    if (g_worker_running || g_stop) return;
    g_worker = std::thread(&worker_main);
    g_worker_running = true;
}

// Writes the result into the guest overlapped (Internal last: HasOverlappedIoCompleted readers never
// see a half-written completion), signals its event, then retires the request.
void finish_op(uint64_t id, int error, int32_t transferred) {
    PendingOp* op = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_ops.find(id);
        if (it == g_ops.end()) return;
        op = it->second.get();
    }
    if (writable(op->overlapped, kOverlappedBytes)) {
        guest_write_be32(op->overlapped + 4, error ? 0u : static_cast<uint32_t>(transferred));
        guest_write_be32(op->overlapped + 0, static_cast<uint32_t>(error));
    }
    if (op->event) set_io_event(op->event, true);
    std::unique_ptr<PendingOp> retired;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_ops.find(id);
        retired = std::move(it->second);
        g_ops.erase(it);
        g_cv.notify_all();
    }
    netdll::release(&retired->lease);
}

// Takes a queued request for this thread. False when it already runs or completed.
bool take_op(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_ops.find(id);
    if (it == g_ops.end() || it->second->executing) return false;
    it->second->executing = true;
    return true;
}

void abort_queued_ops(bool (*match)(const PendingOp&, uint32_t), uint32_t handle) {
    std::vector<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& item : g_ops)
            if (!item.second->executing && match(*item.second, handle)) {
                item.second->executing = true;
                ids.push_back(item.first);
            }
    }
    for (uint64_t id : ids) finish_op(id, netdll::kWsaOperationAborted, 0);
}
bool op_on_socket(const PendingOp& op, uint32_t handle) { return op.request.handle == handle; }
bool any_op(const PendingOp&, uint32_t) { return true; }

// ---- network-event evaluation ------------------------------------------------------------------------
struct AssocView {
    uint32_t handle = 0;
    uint64_t generation = 0;
    uint64_t epoch = 0;
    uint32_t armed = 0;
    bool parked = false;
    bool connecting = false;
};

struct Watch {
    AssocView view;
    Lease lease;
    SocketInfo info;
    bool connected = false;
    uint32_t events = 0;
    size_t slot = 0;
};

struct OpWatch {
    uint64_t id = 0;
    rcomp_net_socket native = RCOMP_NET_INVALID_SOCKET;
    uint32_t events = 0;
    size_t slot = 0;
};

size_t add_poll(std::vector<rcomp_net_pollfd>* fds, rcomp_net_socket native, uint32_t events) {
    for (size_t i = 0; i < fds->size(); ++i)
        if ((*fds)[i].socket == native) {
            (*fds)[i].events |= events;
            return i;
        }
    fds->push_back({native, events, 0});
    return fds->size() - 1;
}

void drop_association(uint32_t handle, uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_assoc.find(handle);
    if (it != g_assoc.end() && it->second.generation == generation) g_assoc.erase(it);
}

// Records `bits` (still enabled) and signals the association's event. An observation older than the
// association's last re-enable is dropped: the next evaluation polls again.
bool record_events(const AssocView& view, uint32_t bits, bool park, bool connect_done) {
    std::shared_ptr<HandleObject> event;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_assoc.find(view.handle);
        if (it == g_assoc.end() || it->second.generation != view.generation) {
            if (connect_done) g_connecting.erase(view.handle);
            return false;
        }
        Association& a = it->second;
        if (a.epoch != view.epoch) return false;
        if (connect_done) g_connecting.erase(view.handle);
        bits &= a.mask & a.enabled;
        if (park) a.parked = true;
        if (!bits) return park;
        a.recorded |= bits;
        a.enabled &= ~bits;
        event = a.event;
    }
    set_io_event(event, true);
    return true;
}

uint32_t watch_events(Watch* w) {
    const uint32_t armed = w->view.armed;
    uint32_t ev = 0;
    if (w->info.listening) {
        if (armed & netdll::kFdAccept) ev |= RCOMP_NET_POLL_READ | RCOMP_NET_POLL_EXCEPTION;
    } else if (w->info.type == RCOMP_NET_SOCK_STREAM) {
        rcomp_net_addr4 peer{};
        w->connected = rcomp_net_socket_getpeername(w->lease.native, &peer) == 0;
        if (w->connected) {
            if (!w->view.parked && (armed & (netdll::kFdRead | netdll::kFdClose)))
                ev |= RCOMP_NET_POLL_READ | RCOMP_NET_POLL_EXCEPTION;
            if (armed & netdll::kFdOob) ev |= RCOMP_NET_POLL_EXCEPTION;
            if (armed & netdll::kFdWrite) ev |= RCOMP_NET_POLL_WRITE;
        }
        if (w->view.connecting && (armed & netdll::kFdConnect)) ev |= RCOMP_NET_POLL_WRITE | RCOMP_NET_POLL_EXCEPTION;
    } else {
        if (!w->view.parked && (armed & netdll::kFdRead)) ev |= RCOMP_NET_POLL_READ | RCOMP_NET_POLL_EXCEPTION;
        if (armed & netdll::kFdWrite) ev |= RCOMP_NET_POLL_WRITE;
    }
    return ev;
}

// What a poll result means for one association. True when something was recorded or parked.
bool interpret(const Watch& w, uint32_t rev) {
    const uint32_t armed = w.view.armed;
    uint32_t bits = 0;
    bool park = false, connect_done = false;
    if (w.info.listening) {
        if (rev & (RCOMP_NET_POLL_READ | RCOMP_NET_POLL_EXCEPTION)) bits |= netdll::kFdAccept;
    } else if (w.info.type == RCOMP_NET_SOCK_STREAM) {
        if (w.view.connecting && (armed & netdll::kFdConnect) && (rev & (RCOMP_NET_POLL_WRITE | RCOMP_NET_POLL_EXCEPTION))) {
            bits |= netdll::kFdConnect;
            connect_done = true;
        }
        if (w.connected) {
            const bool read_side = (w.events & RCOMP_NET_POLL_READ) != 0;
            if (read_side && (rev & (RCOMP_NET_POLL_READ | RCOMP_NET_POLL_EXCEPTION))) {
                // A one-byte peek tells data from the end of stream. Event-selected sockets are always
                // nonblocking, so data consumed since the poll reads WSAEWOULDBLOCK (nothing to record),
                // never a false end of stream as an empty receive queue would suggest.
                uint8_t probe = 0;
                int32_t peeked = 0;
                const int e = rcomp_net_socket_recv(w.lease.native, &probe, 1, RCOMP_NET_MSG_PEEK, &peeked);
                if (e == netdll::kWsaEwouldblock) {
                    // consumed since the poll
                } else if (e || peeked == 0) {
                    // End of stream or a socket error: FD_CLOSE (once); the read side stays ready.
                    if (armed & netdll::kFdClose) bits |= netdll::kFdClose;
                    park = true;
                } else if (armed & netdll::kFdRead) {
                    bits |= netdll::kFdRead;
                } else {
                    park = true;
                }
            } else if ((rev & RCOMP_NET_POLL_EXCEPTION) && (armed & netdll::kFdOob)) {
                int32_t error = 0;
                if (rcomp_net_socket_get_int_option(w.lease.native, RCOMP_NET_OPT_ERROR, &error) == 0 && error == 0)
                    bits |= netdll::kFdOob;
                else if (armed & netdll::kFdClose)
                    bits |= netdll::kFdClose;
                else
                    park = true;
            }
            if ((rev & RCOMP_NET_POLL_WRITE) && (armed & netdll::kFdWrite)) bits |= netdll::kFdWrite;
        }
    } else {
        if ((rev & (RCOMP_NET_POLL_READ | RCOMP_NET_POLL_EXCEPTION)) && (armed & netdll::kFdRead)) bits |= netdll::kFdRead;
        if ((rev & RCOMP_NET_POLL_WRITE) && (armed & netdll::kFdWrite)) bits |= netdll::kFdWrite;
    }
    if (!bits && !park && !connect_done) return false;
    return record_events(w.view, bits, park, connect_done);
}

// A stream send attempt that left part of the request untransferred.
bool partial_stream_send(const Request& r, int32_t transferred) {
    return !receives(r.kind) && r.stream && transferred >= 0 && r.done + uint32_t(transferred) < r.total;
}

// Runs one queued request whose socket polled ready. A transfer that would still block stays queued.
bool run_op(uint64_t id) {
    if (!take_op(id)) return false;
    PendingOp* op = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        op = g_ops.at(id).get();
    }
    if (netdll::lease_error(op->lease)) {
        finish_op(id, netdll::kWsaOperationAborted, 0);
        return true;
    }
    int32_t transferred = 0;
    const int e = perform(op->request, op->lease.native, &transferred);
    if (e == netdll::kWsaEwouldblock) {
        std::lock_guard<std::mutex> lock(g_mu);
        op->executing = false;
        return false;
    }
    if (!e && partial_stream_send(op->request, transferred)) {
        // WinSock completes an overlapped stream send once every buffer is transferred: keep the rest
        // queued until the socket can take it.
        std::lock_guard<std::mutex> lock(g_mu);
        op->request.done += static_cast<uint32_t>(transferred);
        op->executing = false;
        return true;
    }
    finish_op(id, e, e ? 0 : static_cast<int32_t>(op->request.done) + transferred);
    return true;
}

// A request whose socket reported only an exception condition: a pending socket error (SO_ERROR, which
// reading clears) completes it with that error; urgent data alone does not make a transfer ready.
bool fail_on_socket_error(uint64_t id) {
    if (!take_op(id)) return false;
    rcomp_net_socket native = RCOMP_NET_INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        native = g_ops.at(id)->lease.native;
    }
    int32_t error = 0;
    if (rcomp_net_socket_get_int_option(native, RCOMP_NET_OPT_ERROR, &error) == 0 && error) {
        finish_op(id, error, 0);
        return true;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_ops.at(id)->executing = false;
    return false;
}

// One evaluation round over `ops` and `assocs`; `timeout_us` bounds the platform poll. Returns true
// when a request ran or an event was recorded.
bool run_round(const std::vector<OpWatch>& op_list, const std::vector<AssocView>& assocs, int64_t timeout_us) {
    bool progress = false;
    std::vector<rcomp_net_pollfd> fds;
    std::vector<OpWatch> ops;
    for (const OpWatch& candidate : op_list) {
        if (fds.size() >= kMaxPollEntries) break;
        OpWatch o = candidate;
        o.slot = add_poll(&fds, o.native, o.events);
        ops.push_back(o);
    }
    std::vector<Watch> watches;
    watches.reserve(assocs.size());
    for (const AssocView& view : assocs) {
        if (fds.size() >= kMaxPollEntries) break;
        Watch w;
        w.view = view;
        if (netdll::acquire(view.handle, &w.lease, &w.info) != 0) {
            drop_association(view.handle, view.generation);  // the socket was closed or WinSock stopped
            continue;
        }
        w.events = watch_events(&w);
        if (!w.events) {
            netdll::release(&w.lease);
            continue;
        }
        w.slot = add_poll(&fds, w.lease.native, w.events);
        watches.push_back(w);
    }
    uint32_t ready = 0;
    int e = fds.empty() ? 0 : rcomp_net_poll(fds.data(), static_cast<uint32_t>(fds.size()), timeout_us, &ready);
    if (!e && ready) {
        for (const OpWatch& o : ops) {
            const uint32_t rev = fds[o.slot].revents & o.events;
            if (rev == RCOMP_NET_POLL_EXCEPTION) progress |= fail_on_socket_error(o.id);
            else if (rev) progress |= run_op(o.id);
        }
        for (Watch& w : watches) {
            const uint32_t rev = fds[w.slot].revents & w.events;
            if (rev) progress |= interpret(w, rev);
        }
    }
    for (Watch& w : watches) netdll::release(&w.lease);
    return progress;
}

void snapshot_locked(std::vector<OpWatch>* ops, std::vector<AssocView>* assocs) {
    std::vector<OpWatch> all_ops;
    for (const auto& item : g_ops) {
        if (item.second->executing) continue;
        const uint32_t ev = (receives(item.second->request.kind) ? RCOMP_NET_POLL_READ : RCOMP_NET_POLL_WRITE) |
                            RCOMP_NET_POLL_EXCEPTION;
        all_ops.push_back({item.first, item.second->lease.native, ev, 0});
    }
    std::vector<AssocView> all_assocs;
    for (const auto& item : g_assoc) {
        const uint32_t armed = item.second.mask & item.second.enabled;
        if (!armed) continue;
        all_assocs.push_back({item.first, item.second.generation, item.second.epoch, armed, item.second.parked,
                              g_connecting.count(item.first) != 0});
    }
    // More than one poll can watch: rotate the starting point so nothing waits forever.
    const size_t total = all_ops.size() + all_assocs.size();
    const size_t start = total > kMaxPollEntries ? size_t(g_round % total) : 0;
    for (size_t i = 0; i < total; ++i) {
        const size_t k = (start + i) % total;
        if (k < all_ops.size()) ops->push_back(all_ops[k]);
        else assocs->push_back(all_assocs[k - all_ops.size()]);
    }
    ++g_round;
}

// Requests whose socket can no longer be used (closed, WSACleanup, teardown) complete aborted.
void abort_unusable(const std::vector<OpWatch>& ops) {
    for (const OpWatch& o : ops) {
        Lease lease{};
        {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_ops.find(o.id);
            if (it == g_ops.end() || it->second->executing) continue;
            lease = it->second->lease;
        }
        if (netdll::lease_error(lease) && take_op(o.id)) finish_op(o.id, netdll::kWsaOperationAborted, 0);
    }
}

void worker_main() {
    apply_host_service_affinity("NET");
    std::unique_lock<std::mutex> lock(g_mu);
    for (;;) {
        g_cv.wait(lock, [] { return g_stop || has_work_locked(); });
        if (g_stop) break;
        std::vector<OpWatch> ops;
        std::vector<AssocView> assocs;
        snapshot_locked(&ops, &assocs);
        lock.unlock();
        abort_unusable(ops);
        const auto started = std::chrono::steady_clock::now();
        const bool progress = run_round(ops, assocs, kWorkerSliceUs);
        lock.lock();
        if (!progress && !g_stop) {
            // A poll that returned early without anything to do (an unrequested error/hang-up state,
            // or nothing to watch) must not spin: wait out the rest of the slice unless woken.
            const auto until = started + std::chrono::microseconds(kWorkerSliceUs);
            g_cv.wait_until(lock, until);
        }
    }
}

// Immediate evaluation of one association (selection, re-enabling): conditions that already hold are
// recorded before the call returns.
void evaluate_now(uint32_t handle) {
    std::vector<AssocView> views;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_assoc.find(handle);
        if (it == g_assoc.end()) return;
        const uint32_t armed = it->second.mask & it->second.enabled;
        if (!armed) return;
        views.push_back({handle, it->second.generation, it->second.epoch, armed, it->second.parked,
                         g_connecting.count(handle) != 0});
    }
    (void)run_round({}, views, 0);
}

// ---- overlapped requests -----------------------------------------------------------------------------
// Validates the WSAOVERLAPPED and references its event before anything is transferred.
int begin_overlapped(uint32_t overlapped, std::shared_ptr<HandleObject>* event) {
    if (!writable(overlapped, kOverlappedBytes)) return netdll::kWsaEfault;
    uint32_t handle = 0;
    guest_read_be32(overlapped + kOverlappedEvent, &handle);
    if (handle && reference_io_event(handle, event) != Status::Ok) return static_cast<int>(kErrorInvalidHandle);
    return 0;
}

void reenable_after(const Request& r, int error) {
    if (receives(r.kind))
        netdll::reenable_network_events(r.handle, (r.flags & RCOMP_NET_MSG_OOB) ? netdll::kFdOob : netdll::kFdRead);
    else if (error == netdll::kWsaEwouldblock)
        netdll::reenable_network_events(r.handle, netdll::kFdWrite);
}

// The common body of WSARecv/WSARecvFrom/WSASend/WSASendTo once arguments are marshalled.
void transfer(PPCContext& ctx, Request&& r, uint32_t bytes_out, uint32_t flags_out, uint32_t overlapped,
              uint32_t routine, const char* fn) {
    if (routine)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xam.xex!%s completion routine 0x%08X: no APC is ever delivered to guest threads lr=0x%08X", fn,
                    routine, uint32_t(ctx.lr));
    if (bytes_out && !writable(bytes_out, 4)) return fail(ctx, netdll::kWsaEfault);
    if (!overlapped && !bytes_out) return fail(ctx, netdll::kWsaEfault);
    std::shared_ptr<HandleObject> event;
    if (overlapped) {
        const int e = begin_overlapped(overlapped, &event);
        if (e) return fail(ctx, e);
    }
    Lease lease{};
    SocketInfo info{};
    int e = netdll::acquire(r.handle, &lease, &info);
    if (e) return fail(ctx, e);
    r.stream = info.type == RCOMP_NET_SOCK_STREAM;

    bool run_now = true;
    if (overlapped) {
        if (event) set_io_event(event, false);
        // Only a transfer that can run without waiting runs on the calling thread.
        const uint32_t probe_events =
            (receives(r.kind) ? RCOMP_NET_POLL_READ : RCOMP_NET_POLL_WRITE) | RCOMP_NET_POLL_EXCEPTION;
        rcomp_net_pollfd probe{lease.native, probe_events, 0};
        uint32_t ready = 0;
        e = rcomp_net_poll(&probe, 1, 0, &ready);
        if (e) {
            netdll::release(&lease);
            return fail(ctx, e);
        }
        run_now = ready != 0;
    }
    int32_t transferred = 0;
    if (run_now) {
        e = perform(r, lease.native, &transferred);
        const bool rest_pending = overlapped && !e && partial_stream_send(r, transferred);
        if (rest_pending) r.done = static_cast<uint32_t>(transferred);
        if (!(overlapped && e == netdll::kWsaEwouldblock) && !rest_pending) {
            netdll::release(&lease);
            reenable_after(r, e);
            if (e) return fail(ctx, e);
            if (bytes_out) guest_write_be32(bytes_out, static_cast<uint32_t>(transferred));
            if (flags_out) guest_write_be32(flags_out, 0);
            if (overlapped) {
                guest_write_be32(overlapped + 4, static_cast<uint32_t>(transferred));
                guest_write_be32(overlapped + 0, 0);
                if (event) set_io_event(event, true);
            }
            ctx.r3.u64 = 0;
            return;
        }
    }
    // Pending: the overlapped reads STATUS_PENDING before the worker can see the request.
    guest_write_be32(overlapped + 4, 0);
    guest_write_be32(overlapped + 0, kStatusPending);
    const Request copy_for_hooks{r.kind, r.handle, {}, 0, r.flags, 0, 0, {}};
    {
        auto op = std::make_unique<PendingOp>();
        op->request = std::move(r);
        op->lease = lease;
        op->overlapped = overlapped;
        op->event = std::move(event);
        std::lock_guard<std::mutex> lock(g_mu);
        g_ops.emplace(g_next_op++, std::move(op));
        ensure_worker_locked();
        g_cv.notify_all();
    }
    reenable_after(copy_for_hooks, 0);
    fail(ctx, netdll::kWsaIoPending);
}

// NetDll_WSARecv (0x0013).
void WSARecv(PPCContext& ctx, uint8_t*) {
    const char* fn = "NetDll_WSARecv";
    Request r;
    r.kind = Kind::Recv;
    r.handle = ctx.r4.u32;
    const uint32_t flags_ptr = ctx.r8.u32;
    if (!writable(flags_ptr, 4)) return fail(ctx, netdll::kWsaEfault);
    guest_read_be32(flags_ptr, &r.flags);
    if (!netdll::message_flags_valid(r.flags)) return fail(ctx, netdll::kWsaEinval);
    const int e = read_buffers(ctx.r5.u32, ctx.r6.u32, true, &r);
    if (e) return fail(ctx, e);
    transfer(ctx, std::move(r), ctx.r7.u32, flags_ptr, ctx.r9.u32, ctx.r10.u32, fn);
}

// NetDll_WSARecvFrom (0x0015).
void WSARecvFrom(PPCContext& ctx, uint8_t*) {
    const char* fn = "NetDll_WSARecvFrom";
    const uint32_t overlapped = stack_arg(ctx, 8, fn), routine = stack_arg(ctx, 9, fn);
    Request r;
    r.kind = Kind::RecvFrom;
    r.handle = ctx.r4.u32;
    const uint32_t flags_ptr = ctx.r8.u32;
    if (!writable(flags_ptr, 4)) return fail(ctx, netdll::kWsaEfault);
    guest_read_be32(flags_ptr, &r.flags);
    if (!netdll::message_flags_valid(r.flags)) return fail(ctx, netdll::kWsaEinval);
    r.from = ctx.r9.u32;
    r.fromlen = ctx.r10.u32;
    if (r.from || r.fromlen) {
        uint32_t length = 0;
        if (!r.from || !writable(r.fromlen, 4) || !guest_read_be32(r.fromlen, &length) ||
            netdll::check_sockaddr_output(r.from, length) != 0)
            return fail(ctx, netdll::kWsaEfault);
    }
    const int e = read_buffers(ctx.r5.u32, ctx.r6.u32, true, &r);
    if (e) return fail(ctx, e);
    transfer(ctx, std::move(r), ctx.r7.u32, flags_ptr, overlapped, routine, fn);
}

// NetDll_WSASend (0x0017).
void WSASend(PPCContext& ctx, uint8_t*) {
    Request r;
    r.kind = Kind::Send;
    r.handle = ctx.r4.u32;
    r.flags = ctx.r8.u32;
    if (!netdll::message_flags_valid(r.flags) || (r.flags & RCOMP_NET_MSG_PEEK)) return fail(ctx, netdll::kWsaEinval);
    const int e = read_buffers(ctx.r5.u32, ctx.r6.u32, false, &r);
    if (e) return fail(ctx, e);
    transfer(ctx, std::move(r), ctx.r7.u32, 0, ctx.r9.u32, ctx.r10.u32, "NetDll_WSASend");
}

// NetDll_WSASendTo (0x0019).
void WSASendTo(PPCContext& ctx, uint8_t*) {
    const char* fn = "NetDll_WSASendTo";
    const uint32_t overlapped = stack_arg(ctx, 8, fn), routine = stack_arg(ctx, 9, fn);
    Request r;
    r.kind = Kind::SendTo;
    r.handle = ctx.r4.u32;
    r.flags = ctx.r8.u32;
    if (!netdll::message_flags_valid(r.flags) || (r.flags & RCOMP_NET_MSG_PEEK)) return fail(ctx, netdll::kWsaEinval);
    int e = netdll::read_sockaddr(ctx.r9.u32, ctx.r10.u32, &r.to);
    if (!e) e = read_buffers(ctx.r5.u32, ctx.r6.u32, false, &r);
    if (e) return fail(ctx, e);
    transfer(ctx, std::move(r), ctx.r7.u32, 0, overlapped, routine, fn);
}

bool op_pending(uint32_t handle, uint32_t overlapped) {
    for (const auto& item : g_ops)
        if (item.second->overlapped == overlapped && item.second->request.handle == handle) return true;
    return false;
}

// NetDll_WSAGetOverlappedResult (0x0010): TRUE with the transfer size, or FALSE with the request's error
// (WSA_IO_INCOMPLETE while it runs and fWait is FALSE).
void WSAGetOverlappedResult(PPCContext& ctx, uint8_t*) {
    const uint32_t handle = ctx.r4.u32, overlapped = ctx.r5.u32, bytes = ctx.r6.u32, wait = ctx.r7.u32,
                   flags = ctx.r8.u32;
    if (!writable(overlapped, kOverlappedBytes) || !writable(bytes, 4) || !writable(flags, 4)) {
        netdll::set_error(netdll::kWsaEfault);
        ctx.r3.u64 = 0;
        return;
    }
    Lease lease{};
    const int e = netdll::acquire(handle, &lease);
    netdll::release(&lease);
    if (e) {
        netdll::set_error(static_cast<uint32_t>(e));
        ctx.r3.u64 = 0;
        return;
    }
    {
        std::unique_lock<std::mutex> lock(g_mu);
        if (wait) {
            g_cv.wait(lock, [&] { return g_stop || !op_pending(handle, overlapped); });
        } else if (op_pending(handle, overlapped)) {
            netdll::set_error(netdll::kWsaIoIncomplete);
            ctx.r3.u64 = 0;
            return;
        }
    }
    uint32_t internal = 0, transferred = 0;
    guest_read_be32(overlapped + 0, &internal);
    guest_read_be32(overlapped + 4, &transferred);
    if (internal == kStatusPending) {  // not a request of this socket that ran here
        netdll::set_error(netdll::kWsaIoIncomplete);
        ctx.r3.u64 = 0;
        return;
    }
    guest_write_be32(bytes, transferred);
    guest_write_be32(flags, 0);
    if (internal) {
        netdll::set_error(internal);
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = 1;
}

// NetDll_WSACancelOverlappedIO (0x0011): every queued request of the socket completes with
// WSA_OPERATION_ABORTED; one that is already transferring completes normally.
void WSACancelOverlappedIO(PPCContext& ctx, uint8_t*) {
    Lease lease{};
    const int e = netdll::acquire(ctx.r4.u32, &lease);
    netdll::release(&lease);
    if (e) return fail(ctx, e);
    abort_queued_ops(&op_on_socket, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// ---- WSA events --------------------------------------------------------------------------------------
// NetDll_WSACreateEvent (0x001D): a manual-reset, nonsignaled kernel event (NtCreateEvent
// NotificationEvent), or WSA_INVALID_EVENT with the converted NTSTATUS as the WSA error.
void WSACreateEvent(PPCContext& ctx, uint8_t* base) {
    const char* fn = "NetDll_WSACreateEvent";
    KernelFrame f;
    kernel_frame(ctx, fn, &f);
    guest_write_be32(f.scratch, 0);
    f.ctx.r3.u64 = f.scratch;
    f.ctx.r4.u64 = 0;  // no OBJECT_ATTRIBUTES
    f.ctx.r5.u64 = 0;  // NotificationEvent
    f.ctx.r6.u64 = 0;  // nonsignaled
    kernel(kOrdinalNtCreateEvent, fn)(f.ctx, base);
    const uint32_t status = f.ctx.r3.u32;
    if (status) {
        netdll::set_error(dos_error(ctx, base, status, fn));
        ctx.r3.u64 = 0;
        return;
    }
    uint32_t handle = 0;
    guest_read_be32(f.scratch, &handle);
    ctx.r3.u64 = handle;
}

// BOOL result of a kernel call on a WSA event handle; the WSA error is the converted NTSTATUS.
void event_result(PPCContext& ctx, uint8_t* base, uint32_t status, const char* fn) {
    if (status) {
        netdll::set_error(dos_error(ctx, base, status, fn));
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = 1;
}

// NetDll_WSACloseEvent (0x001E): closes an event handle. A handle of another kind is refused
// (WSA_INVALID_HANDLE) rather than closed: WSACloseEvent only owns events.
void WSACloseEvent(PPCContext& ctx, uint8_t* base) {
    std::shared_ptr<HandleObject> event;
    const uint32_t handle = ctx.r3.u32;
    if (reference_io_event(handle, &event) != Status::Ok) {
        netdll::set_error(kErrorInvalidHandle);
        ctx.r3.u64 = 0;
        return;
    }
    event.reset();
    event_result(ctx, base, kernel_handle_call(ctx, base, kOrdinalNtClose, handle, "NetDll_WSACloseEvent"),
                 "NetDll_WSACloseEvent");
}

// NetDll_WSASetEvent (0x001F) / NetDll_WSAResetEvent (0x0020): NtSetEvent / NtClearEvent.
void WSASetEvent(PPCContext& ctx, uint8_t* base) {
    const uint32_t handle = ctx.r3.u32;
    event_result(ctx, base, kernel_handle_call(ctx, base, kOrdinalNtSetEvent, handle, "NetDll_WSASetEvent"),
                 "NetDll_WSASetEvent");
}
void WSAResetEvent(PPCContext& ctx, uint8_t* base) {
    const uint32_t handle = ctx.r3.u32;
    event_result(ctx, base, kernel_handle_call(ctx, base, kOrdinalNtClearEvent, handle, "NetDll_WSAResetEvent"),
                 "NetDll_WSAResetEvent");
}

// NetDll_WSAWaitForMultipleEvents (0x0021): NtWaitForMultipleObjectsEx(WaitAll/WaitAny, UserMode).
// Returns WSA_WAIT_EVENT_0 + i, WSA_WAIT_TIMEOUT (0x102), WSA_WAIT_IO_COMPLETION (0xC0) or
// WSA_WAIT_FAILED with the converted NTSTATUS as the WSA error.
void WSAWaitForMultipleEvents(PPCContext& ctx, uint8_t* base) {
    const char* fn = "NetDll_WSAWaitForMultipleEvents";
    const uint32_t count = ctx.r3.u32, events = ctx.r4.u32, wait_all = ctx.r5.u32, timeout = ctx.r6.u32,
                   alertable = ctx.r7.u32;
    if (count == 0 || count > kMaxWaitEvents) {
        netdll::set_error(kErrorInvalidParameter);
        ctx.r3.u64 = kWaitFailed;
        return;
    }
    KernelFrame f;
    kernel_frame(ctx, fn, &f);
    uint32_t timeout_ptr = 0;
    if (timeout != kWaitInfinite) {
        // Relative LARGE_INTEGER in 100 ns units; 0 polls.
        timeout_ptr = f.scratch;
        guest_write_be64(timeout_ptr, uint64_t(0) - uint64_t(timeout) * 10000u);
    }
    f.ctx.r3.u64 = count;
    f.ctx.r4.u64 = events;
    f.ctx.r5.u64 = wait_all ? 0 : 1;  // WAIT_TYPE: WaitAll = 0, WaitAny = 1
    f.ctx.r6.u64 = 1;                 // UserMode
    f.ctx.r7.u64 = alertable ? 1 : 0;
    f.ctx.r8.u64 = timeout_ptr;
    kernel(kOrdinalNtWaitForMultipleObjectsEx, fn)(f.ctx, base);
    const uint32_t status = f.ctx.r3.u32;
    if (status & 0x80000000u) {
        netdll::set_error(dos_error(ctx, base, status, fn));
        ctx.r3.u64 = kWaitFailed;
        return;
    }
    ctx.r3.u64 = status;
}

// NetDll_WSAEventSelect (0x0023).
void WSAEventSelect(PPCContext& ctx, uint8_t*) {
    const uint32_t handle = ctx.r4.u32, event_handle = ctx.r5.u32, mask = ctx.r6.u32;
    if (mask & ~netdll::kFdAll) return fail(ctx, netdll::kWsaEinval);
    Lease lease{};
    int e = netdll::acquire(handle, &lease);
    if (e) return fail(ctx, e);
    std::shared_ptr<HandleObject> event;
    if (mask && reference_io_event(event_handle, &event) != Status::Ok) e = netdll::kWsaEinval;
    // WinSock: WSAEventSelect puts the socket in nonblocking mode whatever lNetworkEvents is.
    uint32_t nonblocking = 1;
    if (!e) e = rcomp_net_socket_ioctl(lease.native, RCOMP_NET_IOCTL_NONBLOCK, &nonblocking);
    netdll::release(&lease);
    if (e) return fail(ctx, e);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_connecting.erase(handle);
        if (!mask) {
            g_assoc.erase(handle);
        } else {
            Association& a = g_assoc[handle];
            a = Association{};
            a.event = std::move(event);
            a.mask = mask;
            a.enabled = mask;
            a.generation = g_next_generation++;
            ensure_worker_locked();
        }
        g_cv.notify_all();
    }
    if (mask) evaluate_now(handle);
    ctx.r3.u64 = 0;
}

// NetDll___WSAFDIsSet (0x0022): whether `s` is one of the first fd_count (at most FD_SETSIZE) entries.
void WSAFDIsSet(PPCContext& ctx, uint8_t*) {
    const uint32_t socket = ctx.r3.u32, set = ctx.r4.u32;
    uint32_t count = 0;
    if (!readable(set, 4) || !guest_read_be32(set, &count))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xam.xex!NetDll___WSAFDIsSet fd_set 0x%08X not readable lr=0x%08X", set,
                    uint32_t(ctx.lr));
    count = std::min(count, kMaxFdSetCount);
    if (count && !readable(set + 4, count * 4))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xam.xex!NetDll___WSAFDIsSet fd_array 0x%08X+%u not readable lr=0x%08X",
                    set + 4, count * 4, uint32_t(ctx.lr));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = 0;
        guest_read_be32(set + 4 + 4 * i, &entry);
        if (entry == socket) {
            ctx.r3.u64 = 1;
            return;
        }
    }
    ctx.r3.u64 = 0;
}

// ---- XNet services -----------------------------------------------------------------------------------
// NetDll_XNetRandom (0x0035): cb bytes from the hardware/OS random source.
void XNetRandom(PPCContext& ctx, uint8_t*) {
    const uint32_t buffer = ctx.r4.u32, size = ctx.r5.u32;
    if (!size) {
        ctx.r3.u64 = 0;
        return;
    }
    uint8_t* out = nullptr;
    if (!netdll::guest_span(buffer, size, true, &out)) {
        ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaEfault);
        return;
    }
    if (!random_bytes(out, size))
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "xam.xex!NetDll_XNetRandom: no random source (RDRAND unavailable or failing)");
    ctx.r3.u64 = 0;
}

bool read_bytes(uint32_t address, uint32_t size, uint8_t* out) {
    uint8_t* p = nullptr;
    if (!netdll::guest_span(address, size, false, &p)) return false;
    std::memcpy(out, p, size);
    return true;
}

// NetDll_XNetRegisterKey (0x0037): adds an XNKID/XNKEY pair to the XNet key table (system link and
// LIVE sessions register their key before peers can connect). 0, WSAEALREADY for an XNKID already
// registered, WSAENOBUFS when cfgKeyRegMax pairs are registered, WSANOTINITIALISED before XNetStartup.
void XNetRegisterKey(PPCContext& ctx, uint8_t*) {
    XnKey entry{};
    if (!read_bytes(ctx.r4.u32, 8, entry.id) || !read_bytes(ctx.r5.u32, 16, entry.key)) {
        ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaEfault);
        return;
    }
    uint32_t capacity = 0;
    if (!netdll::xnet_started(&capacity)) {
        ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaNotInitialized);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    for (const XnKey& k : g_keys)
        if (std::memcmp(k.id, entry.id, sizeof(entry.id)) == 0) {
            ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaEalready);
            return;
        }
    if (g_keys.size() >= capacity) {
        ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaEnobufs);
        return;
    }
    g_keys.push_back(entry);
    ctx.r3.u64 = 0;
}

// NetDll_XNetUnregisterKey (0x0038): 0, or WSAEINVAL for an XNKID that is not registered.
void XNetUnregisterKey(PPCContext& ctx, uint8_t*) {
    uint8_t id[8];
    if (!read_bytes(ctx.r4.u32, 8, id)) {
        ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaEfault);
        return;
    }
    if (!netdll::xnet_started(nullptr)) {
        ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaNotInitialized);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    for (size_t i = 0; i < g_keys.size(); ++i)
        if (std::memcmp(g_keys[i].id, id, sizeof(id)) == 0) {
            g_keys.erase(g_keys.begin() + static_cast<std::ptrdiff_t>(i));
            ctx.r3.u64 = 0;
            return;
        }
    ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaEinval);
}

// NetDll_XNetInAddrToString (0x003E): dotted decimal of the IN_ADDR (network order, so its big-endian
// word is the host-order address). WSAEFAULT when the buffer cannot hold the text and its NUL.
void XNetInAddrToString(PPCContext& ctx, uint8_t*) {
    const uint32_t address = ctx.r4.u32, buffer = ctx.r5.u32;
    const int32_t capacity = static_cast<int32_t>(ctx.r6.u32);
    char text[16];
    const int length = snprintf(text, sizeof(text), "%u.%u.%u.%u", address >> 24, (address >> 16) & 0xFF,
                                (address >> 8) & 0xFF, address & 0xFF);
    uint8_t* out = nullptr;
    if (capacity <= length || !netdll::guest_span(buffer, uint32_t(length) + 1, true, &out)) {
        ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaEfault);
        return;
    }
    std::memcpy(out, text, size_t(length) + 1);
    ctx.r3.u64 = 0;
}

// NetDll_XNetDnsLookup (0x0043). R-comp has no network link (XNetGetEthernetLinkStatus = 0,
// XNetGetTitleXnAddr = XNET_GET_XNADDR_NONE, so no DNS server): the request is accepted and completes
// at once with a failed XNDNS (iStatus = WSAENETDOWN, cina = 0), as an unplugged console's lookup
// fails; the event is signalled. No host name ever resolves.
constexpr uint32_t kXnDnsBytes = 0x28;
constexpr uint32_t kMaxHostName = 256;
void XNetDnsLookup(PPCContext& ctx, uint8_t*) {
    const char* fn = "NetDll_XNetDnsLookup";
    Runtime& r = current(fn);
    const uint32_t host = ctx.r4.u32, event_handle = ctx.r5.u32, output = ctx.r6.u32;
    if (!writable(output, 4)) {
        ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaEfault);
        return;
    }
    char name[kMaxHostName + 1] = {};
    uint32_t length = 0;
    for (;; ++length) {
        uint8_t* p = nullptr;
        if (length > kMaxHostName || !netdll::guest_span(host + length, 1, false, &p)) {
            ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaEfault);
            return;
        }
        name[length] = static_cast<char>(*p);
        if (!*p) break;
    }
    std::shared_ptr<HandleObject> event;
    if (event_handle && reference_io_event(event_handle, &event) != Status::Ok) {
        ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaEinval);
        return;
    }
    if (!netdll::network_started()) {
        ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaNotInitialized);
        return;
    }
    uint32_t block = 0;
    if (r.heap.alloc(kXnDnsBytes, 16, true, &block) != Status::Ok) {
        ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaEnobufs);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_dns_blocks.insert(block);
    }
    guest_write_be32(block + 4, 0);  // cina
    guest_write_be32(block + 0, static_cast<uint32_t>(netdll::kWsaEnetdown));
    guest_write_be32(output, block);
    fprintf(stderr, "RCOMP-XNET DnsLookup \"%.64s\" -> iStatus=%d (no network link)\n", name, netdll::kWsaEnetdown);
    if (event) set_io_event(event, true);
    ctx.r3.u64 = 0;
}

// NetDll_XNetDnsRelease (0x0044): frees an XNDNS returned by XNetDnsLookup; WSAEINVAL for anything else.
void XNetDnsRelease(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("NetDll_XNetDnsRelease");
    const uint32_t block = ctx.r4.u32;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_dns_blocks.erase(block)) {
            ctx.r3.u64 = static_cast<uint32_t>(netdll::kWsaEinval);
            return;
        }
    }
    if (r.heap.free(block) != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!NetDll_XNetDnsRelease: XNDNS 0x%08X not a heap block", block);
    ctx.r3.u64 = 0;
}

struct Entry {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Entry kEntries[] = {
    {0x0010, "NetDll_WSAGetOverlappedResult", &WSAGetOverlappedResult},
    {0x0011, "NetDll_WSACancelOverlappedIO", &WSACancelOverlappedIO},
    {0x0013, "NetDll_WSARecv", &WSARecv},
    {0x0015, "NetDll_WSARecvFrom", &WSARecvFrom},
    {0x0017, "NetDll_WSASend", &WSASend},
    {0x0019, "NetDll_WSASendTo", &WSASendTo},
    {0x001D, "NetDll_WSACreateEvent", &WSACreateEvent},
    {0x001E, "NetDll_WSACloseEvent", &WSACloseEvent},
    {0x001F, "NetDll_WSASetEvent", &WSASetEvent},
    {0x0020, "NetDll_WSAResetEvent", &WSAResetEvent},
    {0x0021, "NetDll_WSAWaitForMultipleEvents", &WSAWaitForMultipleEvents},
    {0x0022, "NetDll___WSAFDIsSet", &WSAFDIsSet},
    {0x0023, "NetDll_WSAEventSelect", &WSAEventSelect},
    {0x0035, "NetDll_XNetRandom", &XNetRandom},
    {0x0037, "NetDll_XNetRegisterKey", &XNetRegisterKey},
    {0x0038, "NetDll_XNetUnregisterKey", &XNetUnregisterKey},
    {0x003E, "NetDll_XNetInAddrToString", &XNetInAddrToString},
    {0x0043, "NetDll_XNetDnsLookup", &XNetDnsLookup},
    {0x0044, "NetDll_XNetDnsRelease", &XNetDnsRelease},
};

}  // namespace

Status register_xam_net_wsa_hle() {
    for (const Entry& entry : kEntries) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXam, entry.name, &ordinal) || ordinal != entry.ordinal) return Status::Conflict;
        const Status status = register_import(kModuleXam, entry.ordinal, entry.function, entry.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

namespace netdll {

void reenable_network_events(uint32_t handle, uint32_t events) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_assoc.find(handle);
        if (it == g_assoc.end()) return;
        it->second.enabled |= events & it->second.mask;
        it->second.parked = false;
        ++it->second.epoch;
        g_cv.notify_all();
    }
    evaluate_now(handle);
}

void connect_started(uint32_t handle) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_assoc.count(handle)) return;
        g_connecting.insert(handle);
        g_cv.notify_all();
    }
    evaluate_now(handle);
}

void socket_closed(uint32_t handle) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_assoc.erase(handle);
        g_connecting.erase(handle);
    }
    abort_queued_ops(&op_on_socket, handle);
}

bool event_select_active(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_assoc.find(handle);
    return it != g_assoc.end() && it->second.mask != 0;
}

void xnet_stopped() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_keys.clear();
}

void begin_shutdown_wsa() {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_assoc.clear();
        g_connecting.clear();
        g_cv.notify_all();
    }
    abort_queued_ops(&any_op, 0);
}

void shutdown_wsa() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_stop = true;
        g_cv.notify_all();
        worker = std::move(g_worker);
    }
    if (worker.joinable()) worker.join();
    abort_queued_ops(&any_op, 0);
    std::set<uint32_t> blocks;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_assoc.clear();
        g_connecting.clear();
        g_keys.clear();
        blocks.swap(g_dns_blocks);
        g_worker_running = false;
        g_stop = false;
    }
    if (Runtime* r = runtime())
        for (uint32_t block : blocks) (void)r->heap.free(block);
}

}  // namespace netdll
}  // namespace rcomp::rt
