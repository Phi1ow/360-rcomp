#include "rcomp/runtime/status.h"

namespace rcomp::rt {

const char* status_name(Status s) {
    switch (s) {
    case Status::Ok: return "Ok";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::OutOfMemory: return "OutOfMemory";
    case Status::NotInitialized: return "NotInitialized";
    case Status::InvalidHandle: return "InvalidHandle";
    case Status::WrongHandleKind: return "WrongHandleKind";
    case Status::TableFull: return "TableFull";
    case Status::NotAllocated: return "NotAllocated";
    case Status::DoubleFree: return "DoubleFree";
    case Status::Conflict: return "Conflict";
    case Status::PathRejected: return "PathRejected";
    case Status::NoSuchDevice: return "NoSuchDevice";
    case Status::NotFound: return "NotFound";
    case Status::AccessDenied: return "AccessDenied";
    case Status::IsDirectory: return "IsDirectory";
    case Status::EndOfFile: return "EndOfFile";
    case Status::IoError: return "IoError";
    case Status::GuestFault: return "GuestFault";
    case Status::Timeout: return "Timeout";
    case Status::AlreadyExists: return "AlreadyExists";
    case Status::Unsupported: return "Unsupported";
    case Status::UnrecognizedVolume: return "UnrecognizedVolume";
    }
    return "?";
}

uint32_t to_ntstatus(Status s) {
    switch (s) {
    case Status::Ok: return nt::kSuccess;
    case Status::InvalidArgument: return nt::kInvalidParameter;
    case Status::OutOfMemory: return nt::kNoMemory;
    case Status::NotInitialized: return nt::kUnsuccessful;
    case Status::InvalidHandle: return nt::kInvalidHandle;
    case Status::WrongHandleKind: return nt::kObjectTypeMismatch;
    case Status::TableFull: return nt::kInsufficientResources;
    case Status::NotAllocated: return nt::kMemoryNotAllocated;
    case Status::DoubleFree: return nt::kMemoryNotAllocated;
    case Status::Conflict: return nt::kConflictingAddresses;
    case Status::PathRejected: return nt::kObjectNameInvalid;
    case Status::NoSuchDevice: return nt::kObjectPathNotFound;
    case Status::NotFound: return nt::kObjectNameNotFound;
    case Status::AccessDenied: return nt::kAccessDenied;
    case Status::IsDirectory: return nt::kFileIsADirectory;
    case Status::EndOfFile: return nt::kEndOfFile;
    case Status::IoError: return nt::kIoDeviceError;
    case Status::GuestFault: return nt::kAccessViolation;
    case Status::Timeout: return nt::kTimeout;
    case Status::AlreadyExists: return nt::kObjectNameCollision;
    case Status::Unsupported: return 0xC00000BBu;  // STATUS_NOT_SUPPORTED
    case Status::UnrecognizedVolume: return 0xC000014Fu;  // STATUS_UNRECOGNIZED_VOLUME
    }
    return nt::kUnsuccessful;
}

}  // namespace rcomp::rt
