# NetDll Winsock HLE

`runtime/src/hle_xam_net.cpp` implements the socket subset of `xam.xex`
needed by recompiled titles. It is intentionally separate from XNet identity,
authentication and QoS services: those imports remain unresolved until their
service model is implemented.

## Implemented guest exports

The HLE registers the following original XAM ordinals after checking them
against XenonRecomp's pinned export table:

| Ordinal | Export |
| --- | --- |
| `0x0001` | `NetDll_WSAStartup` |
| `0x0002` | `NetDll_WSACleanup` |
| `0x0003` | `NetDll_socket` |
| `0x0004` | `NetDll_closesocket` |
| `0x0005` | `NetDll_shutdown` |
| `0x0006` | `NetDll_ioctlsocket` |
| `0x0007` | `NetDll_setsockopt` |
| `0x0008` | `NetDll_getsockopt` |
| `0x0009` | `NetDll_getsockname` |
| `0x000A` | `NetDll_getpeername` |
| `0x000B` | `NetDll_bind` |
| `0x000C` | `NetDll_connect` |
| `0x000D` | `NetDll_listen` |
| `0x000E` | `NetDll_accept` |
| `0x000F` | `NetDll_select` |
| `0x0012` | `NetDll_recv` |
| `0x0014` | `NetDll_recvfrom` |
| `0x0016` | `NetDll_send` |
| `0x0018` | `NetDll_sendto` |
| `0x001A` | `NetDll_inet_addr` |
| `0x001B` | `NetDll_WSAGetLastError` |
| `0x001C` | `NetDll_WSASetLastError` |

All wrappers honor the leading `caller` PPC parameter where the Xbox ABI has
one. Guest `sockaddr_in`, `fd_set`, `timeval`, option integers and `linger`
values are marshalled explicitly from big endian storage. Native descriptors
are hidden behind generation-tagged guest socket handles, so a closed handle
cannot alias a later descriptor reuse. WSA last-error state uses a real
`pthread_key_t`, the same TLS mechanism already used by guest-thread runtime
state.

## Platform backend contract

`runtime/include/rcomp/runtime/net_platform.h` is the only interface between
the HLE and native sockets. A backend returns `0` or a normalized Winsock
`WSA*` error number; native `errno` and platform option values never cross the
interface.

Exactly one backend is linked into a title or host test:

- `platform/posix/net_posix.cpp` uses host POSIX sockets and `poll`.
- `platform/ps5/net_ps5.cpp` uses `libSceNet` exports only. `select` is built
  from `sceNetEpollCreate`, `sceNetEpollControl`, `sceNetEpollWait` and
  `sceNetEpollDestroy`, because the pinned PS5 SDK does not export
  `sceNetSelect`.

On PS5, `FIONBIO` uses the SceNet nonblocking socket option and `FIONREAD` reads
the verified `recv_queue_length` from `sceNetGetSockInfo`; no BSD ioctl command
number is assumed. Socket cancellation during handle teardown uses
`sceNetSocketAbort`, so a blocked receive or accept can unwind before the native
descriptor is finally closed.

## Runtime integration

The shared build/runtime owner should:

1. Compile `src/hle_xam_net.cpp` into `rcomp_runtime`.
2. Link `platform/posix/net_posix.cpp` for host tests or
   `platform/ps5/net_ps5.cpp` for PS5 titles. PS5 titles also link `libSceNet`.
3. Call `register_xam_net_hle()` after the normal XAM import registry is ready.
4. After all guest threads and external guest-code producers are quiesced,
   call `shutdown_xam_net_hle()` before final runtime/platform teardown. This
   closes native sockets and releases the last SceNet startup reference.
5. Build `tests/test_xam_net.cpp` with the POSIX backend for the host loopback
   suite.

`NetDll` XNet, overlapped WSA, WSA event and HTTP imports are not registered by
this module and therefore remain visible as missing imports rather than fake
successes.

## Validation scope

The host suite uses loopback only and exercises the guest import thunks for
startup reference counting, thread-local last error, TCP and UDP, bind/listen/
accept/connect, send/recv, `select`, socket options, `FIONBIO`, `FIONREAD`,
address byte order, legacy `inet_addr` forms and stale-handle rejection.

The PS5 backend and HLE compile for `x86_64-sie-ps5` with the pinned SDK and
all referenced `sceNet*` names are present in its `libSceNet.so`. That is a
compile/link-contract check only; it is not a PS5 execution result.

## XNet link state (no link)

Found by the actual GTA IV PS5 entry (`NetDll_XNetGetEthernetLinkStatus`).
R-comp exposes no Xbox Live / system-link network, so these report that state:
`XNetGetEthernetLinkStatus` (0x4B) returns 0 (no active link);
`XNetGetTitleXnAddr` (0x49) zeroes the 36-byte XNADDR and returns
`XNET_GET_XNADDR_NONE` (1); `XNetGetConnectStatus` (0x42) returns idle (0);
`XNetXnAddrToInAddr` (0x39), `XNetServerToInAddr` (0x3A) and
`XNetUnregisterInAddr` (0x3F) fail with WSAEINVAL because nothing is ever
registered. QoS (`XNetQosListen` / `XNetQosLookup` / `XNetQosRelease`,
`src/hle_xam_misc.cpp`) fail with WSAEINVAL for the same reason. A real network
provider would replace this contract.

## XNet keys, connect, address and QoS queries

`src/hle_xam_net_more.cpp` (Halo 3; wrappers pass XNCALLER_TITLE in r3):

- `XNetCreateKey` (0x36): a real local key pair: 8 random XNKID bytes with the
  flag bits cleared (`XNET_XNKID_SYSTEM_LINK`), 16 random XNKEY bytes from the host
  random source; WSAEFAULT for an unwritable output, WSANOTINITIALISED before
  `XNetStartup`. The pair is not registered (`XNetRegisterKey` accepts it).
- `XNetConnect` (0x41), `XNetInAddrToXnAddr` (0x3C): nothing is in the security
  table, WSAEINVAL, outputs untouched.
- `XNetXnAddrToMachineId` (0x40): an XNADDR without LIVE data (`inaOnline` and
  `abOnline` zero; every XNADDR R-comp makes) has no machine id: WSAEINVAL
  (WSAEFAULT for inaccessible pointers). One carrying LIVE data cannot come from
  this console and its decoding is not established: explicit fatal.
- `XNetQosServiceLookup` (0x47): no LIVE service association: `*ppxnqos = NULL`,
  WSAEINVAL, event not signalled. Halo 3 skips the result on any non-zero return.
- `XNetQosGetListenStats` (0x4D): no listener exists, WSAEINVAL.
- `XnpLogonGetStatus` (0x70): registered as an explicit fatal that prints r3..r6;
  no call site or reference establishes its arguments or result encoding.

The host random source is shared with `XNetRandom` and `XeCryptRandom`
(`src/host_random.h`). Host evidence: `tests/test_xam_net_more.cpp`. PS5: NOT
TESTED.

## Overlapped I/O and WSAEventSelect

`src/hle_xam_net_wsa.cpp` (added for Gears of War 2, 3 Oct 2026) implements the
asynchronous WinSock surface on top of the socket table of `src/hle_xam_net.cpp`,
through the internal contract `src/xam_net_internal.h`:

- `WSARecv`, `WSARecvFrom`, `WSASend`, `WSASendTo` with a WSAOVERLAPPED: a request
  that can run at once completes immediately; otherwise it is queued with a socket
  lease, the overlapped reads STATUS_PENDING and the call fails with WSA_IO_PENDING.
  One host worker polls the queued sockets and completes the overlapped and its
  event. A TCP send completes only when every byte is sent, as on WinSock.
  Completion routines are not supported (explicit fatal).
- `WSAGetOverlappedResult` (WSA_IO_INCOMPLETE or wait), `WSACancelOverlappedIO`;
  closesocket, WSACleanup and title teardown complete queued requests with
  WSA_OPERATION_ABORTED.
- WSA events are ordinary kernel events (`NtCreateEvent` manual reset, set, clear,
  close, `NtWaitForMultipleObjectsEx` for `WSAWaitForMultipleEvents`).
- `WSAEventSelect` follows WinSock's re-enabling rules (FD_READ by a receive,
  FD_WRITE by a send that failed with WSAEWOULDBLOCK, FD_ACCEPT by accept, FD_OOB by
  an MSG_OOB receive, FD_CONNECT and FD_CLOSE once). A poll observation older than
  the association's last re-enable is dropped; the end of stream is detected with a
  one-byte MSG_PEEK.
- `__WSAFDIsSet`, `XNetRandom` (host random source), `XNetRegisterKey` /
  `XNetUnregisterKey` (bounded key table), `XNetInAddrToString`, `XNetDnsLookup` /
  `XNetDnsRelease`.
- A nonblocking `connect` reports WSAEWOULDBLOCK, as WinSock does, not
  WSAEINPROGRESS.

Host evidence: `tests/test_xam_net_wsa.cpp` (loopback TCP and UDP, events,
EventSelect, error codes, teardown) PASS, repeated 25 times and 8 in parallel.
PS5 status: NOT TESTED. Known limit: an overlapped send on a blocking socket runs
the blocking send on the single worker and can delay other queued requests until
the peer reads (NOT TESTED).
