// Strong host ownership for an existing runtime event used by synchronous I/O.
// The event implementation remains private to hle_xboxkrnl_threads.cpp; file
// services can acquire/reset/signal it without duplicating dispatcher state.
#pragma once

#include <stdint.h>
#include <memory>

#include "rcomp/runtime/handle_table.h"
#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Acquires a strong reference to an Event handle. Closing the guest handle
// after this succeeds does not invalidate `out`. Wrong kind is reported as
// WrongHandleKind; a stale/closed/unknown handle as InvalidHandle.
Status reference_io_event(uint32_t handle, std::shared_ptr<HandleObject>* out);

// Changes a reference returned by reference_io_event. `signalled=false`
// resets it; `signalled=true` sets it and wakes dispatcher waiters.
void set_io_event(const std::shared_ptr<HandleObject>& event, bool signalled);

// I/O completion ports (hle_xboxkrnl_threads.cpp). reference_io_completion
// acquires a strong reference to an IoCompletion handle (WrongHandleKind /
// InvalidHandle as above). post_io_completion appends a packet {Key,
// ApcContext, Status, Information} and wakes the port's waiters.
Status reference_io_completion(uint32_t handle, std::shared_ptr<HandleObject>* out);
Status post_io_completion(const std::shared_ptr<HandleObject>& port, uint32_t key, uint32_t apc_context,
                          uint32_t status, uint32_t information);

// APC routine of a file transfer (NtReadFile, NtWriteFile, NtReadFileScatter,
// NtWriteFileGather; xapi's ReadFileEx/WriteFileEx). xapi passes its completion
// thunk with the low bit set (Halo 3: 0x8259F900 | 1); the routine run is the
// 4-byte-aligned address. file_apc_routine validates it before any I/O (a
// misaligned/NULL routine or one without a recompiled function traps) and
// returns the routine address. queue_file_apc, after the transfer, queues it as a
// user APC to the calling guest thread, NT's I/O completion APC:
// Routine(ApcContext, IoStatusBlock, 0), run by that thread's next user-mode
// alertable wait (SleepEx / Wait*Ex with Alertable). A request that failed
// (NT_ERROR) queues nothing, as on NT for a request that never went pending.
uint32_t file_apc_routine(const char* fn, uint32_t apc, uint32_t lr);
void queue_file_apc(const char* fn, uint32_t apc, uint32_t apc_context, uint32_t piosb, uint32_t status, uint32_t lr);

class GuestFile;
// Completion of a file I/O call (NtReadFile, NtWriteFile, NtReadFileScatter,
// NtWriteFileGather), NT's rule: when the file object is associated with a
// completion port, the call passed a non-NULL ApcContext and the request
// completed with a success status, one packet {Key, ApcContext, Status,
// Information} is queued. A request that fails returns its error and queues
// nothing (Win32: "no completion packet is queued" when ReadFile fails).
void post_file_completion(const GuestFile& file, uint32_t apc_context, uint32_t status, uint32_t information);
// True when the file object is associated with a completion port: an APC
// routine is then refused with STATUS_INVALID_PARAMETER (NT).
bool file_has_completion_port(const GuestFile& file);
// A file transfer that failed (any error status except STATUS_END_OF_FILE at or
// beyond the end of the file) is reported on stderr as one "RCOMP-IO" line with
// the guest path, offset, length, status and caller, for the first 64 failures
// of the process: a title that only checks a hash or a byte count afterwards
// would otherwise fail silently.
void report_file_transfer_failure(const char* fn, const GuestFile& file, uint64_t offset, uint32_t length,
                                  uint32_t status, uint32_t transferred, uint32_t lr);

}  // namespace rcomp::rt
