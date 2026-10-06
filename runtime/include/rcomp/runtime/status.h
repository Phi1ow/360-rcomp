// Runtime-internal status codes (owner: Agent 3, runtime/).
//
// Every runtime service returns an explicit Status. Ok is only returned when
// the requested work was done. HLE exports translate Status into the NTSTATUS
// value the guest expects with to_ntstatus().
#pragma once

#include <stdint.h>

namespace rcomp::rt {

enum class Status : int32_t {
    Ok = 0,
    InvalidArgument,   // bad size/alignment/flags/pointer value
    OutOfMemory,       // heap region exhausted or commit refused
    NotInitialized,    // service used before init()
    InvalidHandle,     // unknown, closed or stale handle
    WrongHandleKind,   // handle exists but refers to another object kind
    TableFull,         // handle table has no free slot
    NotAllocated,      // free() of an address the heap never returned
    DoubleFree,        // free() of an address already freed
    Conflict,          // overlaps a runtime-reserved guest range
    PathRejected,      // traversal, absolute host path, illegal characters
    NoSuchDevice,      // guest path names an unmounted device ("x:")
    NotFound,          // file does not exist under the mounted root
    AccessDenied,      // e.g. write access requested on the read-only VFS
    IsDirectory,       // file operation on a directory
    EndOfFile,         // read starting at or beyond end of file
    IoError,           // host I/O failure (errno logged)
    GuestFault,        // guest pointer not committed
    Timeout,           // wait timed out
    AlreadyExists,     // duplicate registration
    Unsupported,       // valid input this runtime does not handle (e.g. encrypted XEX)
    UnrecognizedVolume,  // the partition carries no file system (runtime/docs/HDD.md)
};

const char* status_name(Status s);

// NTSTATUS values (Xbox 360 kernel uses the Windows NT codes).
namespace nt {
constexpr uint32_t kSuccess = 0x00000000u;
constexpr uint32_t kTimeout = 0x00000102u;
constexpr uint32_t kUnsuccessful = 0xC0000001u;
constexpr uint32_t kNotImplemented = 0xC0000002u;
constexpr uint32_t kAccessViolation = 0xC0000005u;
constexpr uint32_t kInvalidHandle = 0xC0000008u;
constexpr uint32_t kInvalidParameter = 0xC000000Du;
constexpr uint32_t kEndOfFile = 0xC0000011u;
constexpr uint32_t kNoMemory = 0xC0000017u;
constexpr uint32_t kConflictingAddresses = 0xC0000018u;
constexpr uint32_t kAccessDenied = 0xC0000022u;
constexpr uint32_t kObjectTypeMismatch = 0xC0000024u;
constexpr uint32_t kObjectNameInvalid = 0xC0000033u;
constexpr uint32_t kObjectNameNotFound = 0xC0000034u;
constexpr uint32_t kObjectNameCollision = 0xC0000035u;
constexpr uint32_t kObjectPathNotFound = 0xC000003Au;
constexpr uint32_t kMemoryNotAllocated = 0xC00000A0u;
constexpr uint32_t kFileIsADirectory = 0xC00000BAu;
constexpr uint32_t kInsufficientResources = 0xC000009Au;
constexpr uint32_t kIoDeviceError = 0xC0000185u;
}  // namespace nt

uint32_t to_ntstatus(Status s);

}  // namespace rcomp::rt
