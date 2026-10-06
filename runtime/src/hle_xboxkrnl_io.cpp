// Xbox 360 file metadata/query HLE.
// Public ABI sources and supported information classes are documented in
// runtime/docs/IO_METADATA.md.
#include <atomic>
#include <cstdio>
#include <set>
#include <mutex>
#include "rcomp/runtime/xboxkrnl_io.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/io_event.h"
#include "rcomp/runtime/runtime.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kObDosDevices = 0xFFFFFFFDu;
constexpr uint32_t kStatusInvalidInfoClass = 0xC0000003u;
constexpr uint32_t kStatusInfoLengthMismatch = 0xC0000004u;
constexpr uint32_t kStatusBufferOverflow = 0x80000005u;
constexpr uint32_t kStatusNotSupported = 0xC00000BBu;

constexpr uint32_t kFileBasicInformation = 4;
constexpr uint32_t kFileStandardInformation = 5;
constexpr uint32_t kFileInternalInformation = 6;
constexpr uint32_t kFileRenameInformation = 10;
constexpr uint32_t kFileDispositionInformation = 13;
constexpr uint32_t kFilePositionInformation = 14;
constexpr uint32_t kFileModeInformation = 16;
constexpr uint32_t kFileAlignmentInformation = 17;
constexpr uint32_t kFileAllocationInformation = 19;
constexpr uint32_t kFileEndOfFileInformation = 20;
// FileCompletionInformation: {HANDLE Port; PVOID Key} = 8 bytes on the 32-bit
// Xbox 360 (NT FILE_COMPLETION_INFORMATION; Halo 3's CreateIoCompletionPort
// builds exactly this at r1+88 and passes Length 8, class 30). Set-only in NT:
// the query side has no length entry and answers STATUS_INVALID_INFO_CLASS.
constexpr uint32_t kFileCompletionInformation = 30;
constexpr uint32_t kFileNetworkOpenInformation = 34;
constexpr uint32_t kFileMaximumInformation = 37;

constexpr uint32_t kFileFsVolumeInformation = 1;
constexpr uint32_t kFileFsSizeInformation = 3;
constexpr uint32_t kFileFsDeviceInformation = 4;
constexpr uint32_t kFileFsAttributeInformation = 5;
constexpr uint64_t kFileWriteToEndOfFile = UINT64_MAX;
constexpr uint64_t kFileUseFilePointerPosition = UINT64_MAX - 1;

Runtime& runtime_for(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s before runtime_init", fn);
    return *r;
}

void ret(PPCContext& c, uint32_t status) { c.r3.u64 = status; }

bool writable(Runtime& r, uint32_t p, uint32_t bytes) {
    return p && r.mem->is_accessible(p, bytes, Protect::ReadWrite);
}

void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void put64(uint8_t* p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (56 - i * 8));
}

void finish_iosb(Runtime& r, uint32_t piosb, uint32_t status, uint32_t information) {
    if (!piosb) return;
    uint8_t* p = r.mem->host(piosb);
    put32(p, status);
    put32(p + 4, information);
}

uint32_t file_info_size(uint32_t cls) {
    switch (cls) {
    case kFileBasicInformation: return 40;
    case kFileStandardInformation: return 24;
    case kFileInternalInformation: return 8;
    case kFilePositionInformation: return 8;
    case kFileAlignmentInformation: return 4;
    case kFileAllocationInformation: return 8;
    case kFileNetworkOpenInformation: return 56;
    default: return 0;
    }
}

// An information class the runtime does not serve is answered as the console answers an unknown class, and
// reported once per (function, class): a title's next missing file service is then visible in its log.
void report_info_class(const char* fn, uint32_t cls, uint32_t status, uint32_t lr) {
    static std::mutex mutex;
    static std::set<uint64_t> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen.insert((uint64_t(reinterpret_cast<uintptr_t>(fn)) << 8) ^ cls).second) return;
    std::fprintf(stderr, "RCOMP-IO %s class=%u not served: 0x%08X lr=0x%08X\n", fn, cls, status, lr);
    std::fflush(stderr);
}

uint32_t set_file_info_size(uint32_t cls) {
    switch (cls) {
    case kFileBasicInformation: return 40;
    case kFileRenameInformation: return 16;
    case kFileDispositionInformation: return 1;
    case kFilePositionInformation: return 8;
    case kFileModeInformation: return 4;
    case kFileAllocationInformation: return 8;
    case kFileEndOfFileInformation: return 8;
    case kFileCompletionInformation: return 8;
    default: return 0;
    }
}

bool known_but_unsupported_file_info(uint32_t cls) {
    if (cls == 0 || cls >= kFileMaximumInformation || cls == kFileCompletionInformation) return false;
    switch (cls) {
    case kFileBasicInformation:
    case kFileStandardInformation:
    case kFileInternalInformation:
    case kFilePositionInformation:
    case kFileAlignmentInformation:
    case kFileAllocationInformation:
    case kFileNetworkOpenInformation:
        return false;
    default:
        return true;
    }
}

void write_basic(uint8_t* p, const FileMetadata& m) {
    put64(p + 0, m.creation_time);
    put64(p + 8, m.last_access_time);
    put64(p + 16, m.last_write_time);
    put64(p + 24, m.change_time);
    put32(p + 32, m.attributes);
    put32(p + 36, 0);
}

void write_standard(uint8_t* p, const FileMetadata& m) {
    put64(p + 0, m.allocation_size);
    put64(p + 8, m.end_of_file);
    put32(p + 16, m.number_of_links);
    p[20] = 0;  // DeletePending.
    p[21] = m.directory ? 1 : 0;
    p[22] = p[23] = 0;
}

void write_network_open(uint8_t* p, const FileMetadata& m) {
    put64(p + 0, m.creation_time);
    put64(p + 8, m.last_access_time);
    put64(p + 16, m.last_write_time);
    put64(p + 24, m.change_time);
    put64(p + 32, m.allocation_size);
    put64(p + 40, m.end_of_file);
    put32(p + 48, m.attributes);
    put32(p + 52, 0);
}

Status read_object_path(Runtime& r, uint32_t pattrs, const char* fn, std::string* path) {
    if (!pattrs || !path) return Status::InvalidArgument;
    if (!r.mem->is_accessible(pattrs, 12, Protect::Read)) return Status::GuestFault;
    uint32_t root = 0, name_ptr = 0;
    if (!guest_read_be32(pattrs, &root) || !guest_read_be32(pattrs + 4, &name_ptr))
        return Status::GuestFault;
    if (root != 0 && root != kObDosDevices)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "%s relative RootDirectory=0x%08X is outside the VFS contract", fn, root);
    if (!name_ptr) return Status::InvalidArgument;
    if (!r.mem->is_accessible(name_ptr, 8, Protect::Read)) return Status::GuestFault;
    uint16_t length = 0, maximum = 0;
    uint32_t buffer = 0;
    if (!guest_read_be16(name_ptr, &length) || !guest_read_be16(name_ptr + 2, &maximum) ||
        !guest_read_be32(name_ptr + 4, &buffer))
        return Status::GuestFault;
    if (maximum < length) return Status::InvalidArgument;
    if (length && (!buffer || !r.mem->is_accessible(buffer, length, Protect::Read)))
        return Status::GuestFault;
    const uint8_t* chars = length ? r.mem->host(buffer) : nullptr;
    path->assign(reinterpret_cast<const char*>(chars ? chars : (const uint8_t*)""), length);
    return Status::Ok;
}

void NtQueryInformationFile(PPCContext& c, uint8_t*) {
    const char* fn = "NtQueryInformationFile";
    Runtime& r = runtime_for(fn);
    const uint32_t handle = c.r3.u32, piosb = c.r4.u32, out = c.r5.u32;
    const uint32_t length = c.r6.u32, cls = c.r7.u32;

    if (piosb && !writable(r, piosb, 8)) return ret(c, nt::kAccessViolation);
    const uint32_t needed = file_info_size(cls);
    if (!needed) {
        const uint32_t st = known_but_unsupported_file_info(cls) ?
                                kStatusNotSupported : kStatusInvalidInfoClass;
        report_info_class(fn, cls, st, uint32_t(c.lr));
        finish_iosb(r, piosb, st, 0);
        return ret(c, st);
    }
    if (length < needed) {
        finish_iosb(r, piosb, kStatusInfoLengthMismatch, 0);
        return ret(c, kStatusInfoLengthMismatch);
    }
    if (!writable(r, out, needed)) {
        finish_iosb(r, piosb, nt::kAccessViolation, 0);
        return ret(c, nt::kAccessViolation);
    }

    std::shared_ptr<GuestFile> file;
    Status s = r.handles.lookup_as<GuestFile>(handle, &file);
    if (s != Status::Ok) {
        const uint32_t st = to_ntstatus(s);
        finish_iosb(r, piosb, st, 0);
        return ret(c, st);
    }
    FileMetadata m;
    s = file->metadata(&m);
    if (s != Status::Ok) {
        const uint32_t st = to_ntstatus(s);
        finish_iosb(r, piosb, st, 0);
        return ret(c, st);
    }

    uint8_t* p = r.mem->host(out);
    memset(p, 0, needed);
    switch (cls) {
    case kFileBasicInformation:
        write_basic(p, m);
        break;
    case kFileStandardInformation:
        write_standard(p, m);
        break;
    case kFileInternalInformation:
        put64(p, m.file_id);
        break;
    case kFilePositionInformation:
        put64(p, file->position());
        break;
    case kFileAlignmentInformation:
        put32(p, 0);  // FILE_BYTE_ALIGNMENT.
        break;
    case kFileAllocationInformation:
        put64(p, m.allocation_size);
        break;
    case kFileNetworkOpenInformation:
        write_network_open(p, m);
        break;
    default:
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s class dispatch mismatch %u", fn, cls);
    }
    finish_iosb(r, piosb, nt::kSuccess, needed);
    ret(c, nt::kSuccess);
}

void NtQueryFullAttributesFile(PPCContext& c, uint8_t*) {
    const char* fn = "NtQueryFullAttributesFile";
    Runtime& r = runtime_for(fn);
    const uint32_t pattrs = c.r3.u32, out = c.r4.u32;
    if (!writable(r, out, 56)) return ret(c, nt::kAccessViolation);
    std::string path;
    Status s = read_object_path(r, pattrs, fn, &path);
    if (s != Status::Ok) return ret(c, to_ntstatus(s));
    FileMetadata m;
    s = r.vfs.query(path, &m);
    if (s != Status::Ok) return ret(c, to_ntstatus(s));
    uint8_t* p = r.mem->host(out);
    memset(p, 0, 56);
    write_network_open(p, m);
    ret(c, nt::kSuccess);
}

void NtQueryVolumeInformationFile(PPCContext& c, uint8_t*) {
    const char* fn = "NtQueryVolumeInformationFile";
    Runtime& r = runtime_for(fn);
    const uint32_t handle = c.r3.u32, piosb = c.r4.u32, out = c.r5.u32;
    const uint32_t length = c.r6.u32, cls = c.r7.u32;
    if (piosb && !writable(r, piosb, 8)) return ret(c, nt::kAccessViolation);

    uint32_t minimum = 0;
    switch (cls) {
    case kFileFsVolumeInformation: minimum = 0; break;
    case kFileFsSizeInformation: minimum = 24; break;
    case kFileFsAttributeInformation: minimum = 12; break;
    case kFileFsDeviceInformation:
        report_info_class(fn, cls, kStatusNotSupported, uint32_t(c.lr));
        finish_iosb(r, piosb, kStatusNotSupported, 0);
        return ret(c, kStatusNotSupported);
    default:
        report_info_class(fn, cls, kStatusInvalidInfoClass, uint32_t(c.lr));
        finish_iosb(r, piosb, kStatusInvalidInfoClass, 0);
        return ret(c, kStatusInvalidInfoClass);
    }
    if (minimum && length < minimum) {
        finish_iosb(r, piosb, kStatusInfoLengthMismatch, 0);
        return ret(c, kStatusInfoLengthMismatch);
    }

    std::shared_ptr<GuestFile> file;
    Status s = r.handles.lookup_as<GuestFile>(handle, &file);
    if (s != Status::Ok) {
        const uint32_t st = to_ntstatus(s);
        finish_iosb(r, piosb, st, 0);
        return ret(c, st);
    }
    VolumeMetadata v;
    s = file->volume_metadata(&v);
    if (s != Status::Ok) {
        const uint32_t st = to_ntstatus(s);
        finish_iosb(r, piosb, st, 0);
        return ret(c, st);
    }

    if (cls == kFileFsVolumeInformation && !v.has_identity) {
        finish_iosb(r, piosb, kStatusNotSupported, 0);
        return ret(c, kStatusNotSupported);
    }
    if (cls == kFileFsSizeInformation && !v.has_size) {
        finish_iosb(r, piosb, kStatusNotSupported, 0);
        return ret(c, kStatusNotSupported);
    }

    uint32_t write_size = minimum;
    if (cls == kFileFsVolumeInformation) write_size = 24;
    if (cls == kFileFsAttributeInformation)
        write_size = 12 + (uint32_t)v.filesystem_name.size();
    if (length < write_size) {
        if (cls == kFileFsAttributeInformation && length >= 12 &&
            writable(r, out, 12)) {
            uint8_t* header = r.mem->host(out);
            memset(header, 0, 12);
            put32(header + 0, v.attributes);
            put32(header + 4, (uint32_t)v.component_name_max_length);
            put32(header + 8, (uint32_t)v.filesystem_name.size());
            finish_iosb(r, piosb, kStatusBufferOverflow, 12);
            return ret(c, kStatusBufferOverflow);
        }
        finish_iosb(r, piosb, kStatusInfoLengthMismatch, 0);
        return ret(c, kStatusInfoLengthMismatch);
    }
    if (!writable(r, out, write_size)) {
        finish_iosb(r, piosb, nt::kAccessViolation, 0);
        return ret(c, nt::kAccessViolation);
    }

    uint8_t* p = r.mem->host(out);
    memset(p, 0, write_size);
    uint32_t information = 0;
    switch (cls) {
    case kFileFsVolumeInformation:
        put64(p + 0, v.creation_time);
        put32(p + 8, v.serial_number);
        put32(p + 12, 0);  // label length
        p[16] = 0;         // supports objects
        information = 17;  // offsetof(X_FILE_FS_VOLUME_INFORMATION, label)
        break;
    case kFileFsSizeInformation:
        put64(p + 0, v.total_allocation_units);
        put64(p + 8, v.available_allocation_units);
        put32(p + 16, v.sectors_per_allocation_unit);
        put32(p + 20, v.bytes_per_sector);
        information = 24;
        break;
    case kFileFsAttributeInformation:
        put32(p + 0, v.attributes);
        put32(p + 4, (uint32_t)v.component_name_max_length);
        put32(p + 8, (uint32_t)v.filesystem_name.size());
        memcpy(p + 12, v.filesystem_name.data(), v.filesystem_name.size());
        information = 12 + (uint32_t)v.filesystem_name.size();
        break;
    default:
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s class dispatch mismatch %u", fn, cls);
    }
    finish_iosb(r, piosb, nt::kSuccess, information);
    ret(c, nt::kSuccess);
}

void NtSetInformationFile(PPCContext& c, uint8_t*) {
    const char* fn = "NtSetInformationFile";
    Runtime& r = runtime_for(fn);
    const uint32_t handle = c.r3.u32, piosb = c.r4.u32, info = c.r5.u32;
    const uint32_t length = c.r6.u32, cls = c.r7.u32;
    if (piosb && !writable(r, piosb, 8)) return ret(c, nt::kAccessViolation);

    const uint32_t minimum = set_file_info_size(cls);
    if (!minimum) {
        report_info_class(fn, cls, kStatusInvalidInfoClass, uint32_t(c.lr));
        finish_iosb(r, piosb, kStatusInvalidInfoClass, 0);
        return ret(c, kStatusInvalidInfoClass);
    }
    if (length < minimum) {
        finish_iosb(r, piosb, kStatusInfoLengthMismatch, 0);
        return ret(c, kStatusInfoLengthMismatch);
    }
    if (!info || !r.mem->is_accessible(info, minimum, Protect::Read))
        return ret(c, nt::kAccessViolation);

    std::shared_ptr<GuestFile> file;
    Status status = r.handles.lookup_as<GuestFile>(handle, &file);
    if (status != Status::Ok) {
        const uint32_t st = to_ntstatus(status);
        finish_iosb(r, piosb, st, 0);
        return ret(c, st);
    }

    uint32_t information = 0;
    switch (cls) {
    case kFileBasicInformation: {
        FileBasicUpdate update;
        if (!guest_read_be64(info + 0, &update.creation_time) ||
            !guest_read_be64(info + 8, &update.last_access_time) ||
            !guest_read_be64(info + 16, &update.last_write_time) ||
            !guest_read_be64(info + 24, &update.change_time) ||
            !guest_read_be32(info + 32, &update.attributes))
            return ret(c, nt::kAccessViolation);
        status = file->set_basic(update);
        information = minimum;
        break;
    }
    case kFileRenameInformation: {
        uint32_t replace = 0, root = 0, name_buffer = 0;
        uint16_t name_length = 0, name_maximum = 0;
        if (!guest_read_be32(info + 0, &replace) ||
            !guest_read_be32(info + 4, &root) ||
            !guest_read_be16(info + 8, &name_length) ||
            !guest_read_be16(info + 10, &name_maximum) ||
            !guest_read_be32(info + 12, &name_buffer))
            return ret(c, nt::kAccessViolation);
        if (root != 0 && root != kObDosDevices)
            rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                        "%s relative RootDirectory=0x%08X is outside the VFS contract",
                        fn, root);
        if (name_maximum < name_length) {
            finish_iosb(r, piosb, nt::kInvalidParameter, 0);
            return ret(c, nt::kInvalidParameter);
        }
        if (!name_length) {
            finish_iosb(r, piosb, nt::kObjectNameInvalid, 0);
            return ret(c, nt::kObjectNameInvalid);
        }
        if (!name_buffer ||
            !r.mem->is_accessible(name_buffer, name_length, Protect::Read))
            return ret(c, nt::kAccessViolation);
        const char* bytes = reinterpret_cast<const char*>(r.mem->host(name_buffer));
        status = r.vfs.rename(*file, std::string_view(bytes, name_length), replace != 0);
        information = minimum;
        break;
    }
    case kFileDispositionInformation:
        status = file->set_delete_on_close(r.mem->base()[info] != 0);
        information = 0;
        break;
    case kFilePositionInformation: {
        uint64_t position = 0;
        if (!guest_read_be64(info, &position)) return ret(c, nt::kAccessViolation);
        status = file->set_position(position);
        information = minimum;
        break;
    }
    case kFileEndOfFileInformation: {
        uint64_t end = 0;
        if (!guest_read_be64(info, &end)) return ret(c, nt::kAccessViolation);
        status = file->truncate(end);
        information = minimum;
        break;
    }
    case kFileModeInformation:
    case kFileAllocationInformation:
        status = Status::Unsupported;
        information = 0;
        break;
    case kFileCompletionInformation: {
        // NT IopSetCompletion contract: the port handle must be an
        // IoCompletion object (STATUS_OBJECT_TYPE_MISMATCH / STATUS_INVALID_HANDLE);
        // a synchronous-I/O file object or one already associated is refused
        // with STATUS_INVALID_PARAMETER. The association holds a reference to
        // the port for the file object's lifetime.
        uint32_t port_handle = 0, key = 0;
        if (!guest_read_be32(info, &port_handle) || !guest_read_be32(info + 4, &key))
            return ret(c, nt::kAccessViolation);
        std::shared_ptr<HandleObject> port;
        status = reference_io_completion(port_handle, &port);
        if (status == Status::Ok) {
            status = file->set_completion_port(std::move(port), key);
            if (status == Status::AlreadyExists) status = Status::InvalidArgument;
        }
        information = 0;
        break;
    }
    default:
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s class dispatch mismatch %u", fn, cls);
    }

    const uint32_t st = to_ntstatus(status);
    finish_iosb(r, piosb, st, status == Status::Ok ? information : 0);
    ret(c, st);
}

void NtWriteFile(PPCContext& c, uint8_t*) {
    const char* fn = "NtWriteFile";
    Runtime& r = runtime_for(fn);
    const uint32_t handle = c.r3.u32, event_handle = c.r4.u32;
    const uint32_t apc = c.r5.u32, apc_context = c.r6.u32, piosb = c.r7.u32;
    const uint32_t buffer = c.r8.u32, length = c.r9.u32, poffset = c.r10.u32;

    if (apc) {
        std::shared_ptr<GuestFile> associated;
        if (r.handles.lookup_as<GuestFile>(handle, &associated) == Status::Ok &&
            file_has_completion_port(*associated))
            return ret(c, nt::kInvalidParameter);  // NT: no APC routine on a port-associated file
        file_apc_routine(fn, apc, uint32_t(c.lr));
    }
    if ((piosb && !writable(r, piosb, 8)) ||
        (length && !r.mem->is_accessible(buffer, length, Protect::Read)) ||
        (poffset && !r.mem->is_accessible(poffset, 8, Protect::Read)))
        return ret(c, nt::kAccessViolation);

    uint64_t offset = kFileUseFilePointerPosition;
    if (poffset && !guest_read_be64(poffset, &offset))
        return ret(c, nt::kAccessViolation);
    if (offset != kFileUseFilePointerPosition &&
        offset != kFileWriteToEndOfFile && (offset >> 63))
        return ret(c, nt::kInvalidParameter);

    std::shared_ptr<GuestFile> file;
    Status status = r.handles.lookup_as<GuestFile>(handle, &file);
    if (status == Status::Ok && !file->writable())
        status = Status::AccessDenied;
    std::shared_ptr<HandleObject> event;
    if (status == Status::Ok && event_handle)
        status = reference_io_event(event_handle, &event);

    uint32_t written = 0;
    if (status == Status::Ok) {
        const void* src = length ? r.mem->host(buffer) : nullptr;
        if (event) set_io_event(event, false);
        if (offset == kFileUseFilePointerPosition) {
            status = file->write(src, length, &written);
        } else if (offset == kFileWriteToEndOfFile) {
            status = file->write_to_end(src, length, &written);
        } else {
            status = file->write_at(offset, src, length, &written);
        }
    }

    const uint32_t st = to_ntstatus(status);
    finish_iosb(r, piosb, st, written);
    if (event) set_io_event(event, true);
    if (file) {
        report_file_transfer_failure(fn, *file, offset, length, st, written, uint32_t(c.lr));
        post_file_completion(*file, apc_context, st, written);
    }
    queue_file_apc(fn, apc, apc_context, piosb, st, uint32_t(c.lr));
    ret(c, st);
}

// ---- NtQueryDirectoryFile (0x00E4) -----------------------------------------
// Xbox 360: NtQueryDirectoryFile(HANDLE Directory, HANDLE Event, PIO_APC_ROUTINE,
//   PVOID ApcContext, PIO_STATUS_BLOCK, PVOID FileInformation, ULONG Length,
//   PANSI_STRING FileName, BOOLEAN RestartScan). There is no information-class
//   parameter: the output is always FileDirectoryInformation (confirmed by the
//   actual GTA IV call, where the eighth argument is a stack pointer).
// One entry per call, with the Xbox layout (ANSI names): NextEntryOffset(4)=0 FileIndex(4)=0 four FILETIMEs,
// EndOfFile(8), AllocationSize(8), FileAttributes(4), FileNameLength(4), name.
// Results: STATUS_SUCCESS, STATUS_NO_SUCH_FILE (nothing matched),
// STATUS_NO_MORE_FILES (scan exhausted). Layout: public Xenia
// 95a5c3ee `kernel/info/file.h`; behaviour cross-checked with the NT contract.
constexpr uint32_t kStatusNoSuchFile = 0xC000000Fu;
constexpr uint32_t kStatusNoMoreFiles = 0x80000006u;
constexpr uint32_t kDirectoryInfoHeader = 64;

uint32_t io_stack_arg(PPCContext& c, int n, const char* fn) {
    Runtime& r = runtime_for(fn);
    uint32_t v = 0;
    const uint64_t address = uint64_t(c.r1.u32) + 0x54 + 8u * (uint32_t)(n - 8);
    if (address > UINT32_MAX || !r.mem->is_accessible(address, 4, Protect::Read) ||
        !guest_read_be32(uint32_t(address), &v))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s: stack argument %d not readable", fn, n);
    return v;
}

void NtQueryDirectoryFile(PPCContext& c, uint8_t*) {
    const char* fn = "NtQueryDirectoryFile";
    Runtime& r = runtime_for(fn);
    const uint32_t handle = c.r3.u32, event = c.r4.u32, apc = c.r5.u32, piosb = c.r7.u32, out = c.r8.u32,
                   length = c.r9.u32, name_ptr = c.r10.u32;
    const bool restart = (io_stack_arg(c, 8, fn) & 0xFF) != 0;
    if (apc) rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "%s ApcRoutine=0x%08X lr=0x%08X", fn, apc, (uint32_t)c.lr);
    if (piosb && !writable(r, piosb, 8)) return ret(c, nt::kAccessViolation);
    std::string pattern;
    if (name_ptr) {  // X_ANSI_STRING { be16 Length, be16 MaximumLength, be32 Buffer }
        uint16_t len = 0;
        uint32_t buffer = 0;
        if (!r.mem->is_accessible(name_ptr, 8, Protect::Read) || !guest_read_be16(name_ptr, &len) ||
            !guest_read_be32(name_ptr + 4, &buffer) || (len && !r.mem->is_accessible(buffer, len, Protect::Read)))
            return ret(c, nt::kAccessViolation);
        pattern.assign(reinterpret_cast<const char*>(r.mem->host(buffer)), len);
    }
    if (length < kDirectoryInfoHeader) {
        finish_iosb(r, piosb, kStatusInfoLengthMismatch, 0);
        return ret(c, kStatusInfoLengthMismatch);
    }
    if (!writable(r, out, length)) return ret(c, nt::kAccessViolation);
    std::shared_ptr<GuestFile> file;
    Status s = r.handles.lookup_as<GuestFile>(handle, &file);
    std::shared_ptr<HandleObject> completion;
    if (s == Status::Ok && event) s = reference_io_event(event, &completion);
    if (completion) set_io_event(completion, false);
    DirectoryEntry entry;
    if (s == Status::Ok) s = file->query_directory(pattern, restart, &entry);
    if (s == Status::Ok && kDirectoryInfoHeader + entry.name.size() > length) {
        // The scan already advanced past this entry: report it rather than drop it.
        finish_iosb(r, piosb, kStatusBufferOverflow, 0);
        if (completion) set_io_event(completion, true);
        return ret(c, kStatusBufferOverflow);
    }
    uint32_t st = to_ntstatus(s);
    uint32_t information = 0;
    if (s == Status::NotFound) {
        st = kStatusNoSuchFile;
    } else if (s == Status::EndOfFile) {
        st = kStatusNoMoreFiles;
    } else if (s == Status::Ok) {
        uint8_t* p = r.mem->host(out);
        std::memset(p, 0, kDirectoryInfoHeader);
        put64(p + 8, entry.metadata.creation_time);
        put64(p + 16, entry.metadata.last_access_time);
        put64(p + 24, entry.metadata.last_write_time);
        put64(p + 32, entry.metadata.change_time);
        put64(p + 40, entry.metadata.end_of_file);
        put64(p + 48, entry.metadata.allocation_size);
        put32(p + 56, entry.metadata.attributes);
        put32(p + 60, (uint32_t)entry.name.size());
        std::memcpy(p + kDirectoryInfoHeader, entry.name.data(), entry.name.size());
        information = kDirectoryInfoHeader + (uint32_t)entry.name.size();
        st = nt::kSuccess;
    }
    finish_iosb(r, piosb, st, information);
    if (completion) set_io_event(completion, true);
    ret(c, st);
}

void NtFlushBuffersFile(PPCContext& c, uint8_t*) {
    const char* fn = "NtFlushBuffersFile";
    Runtime& r = runtime_for(fn);
    const uint32_t handle = c.r3.u32, piosb = c.r4.u32;
    if (piosb && !writable(r, piosb, 8)) return ret(c, nt::kAccessViolation);
    std::shared_ptr<GuestFile> file;
    Status s = r.handles.lookup_as<GuestFile>(handle, &file);
    if (s == Status::Ok) s = file->flush();
    const uint32_t st = to_ntstatus(s);
    finish_iosb(r, piosb, st, 0);
    ret(c, st);
}

// FscSetCacheElementCount (0x0021): NTSTATUS (ULONG Selector, ULONG Count).
// Sizes the kernel's file-system cache in 4 KiB elements: GoW2's XAPI
// XSetFileCacheSize(bytes) calls it with Selector 0 and Count =
// ceil(bytes / 4096) (sub_82AAA560; the title asks for 128 KiB = 32). The
// signature is Xenia 95a5c3e's (xboxkrnl_io.cc, a stub there, "unk_0 = 0").
// The cache only changes how often the console reads its media; R-comp's VFS
// reads the host file for every request and has no such cache, so there is
// nothing to resize. The requested count is recorded on the Runtime
// (fsc_cache_elements) and the call succeeds, as the console's does. A
// nonzero Selector has no established meaning and traps.
void FscSetCacheElementCount(PPCContext& c, uint8_t*) {
    const char* fn = "FscSetCacheElementCount";
    Runtime& r = runtime_for(fn);
    if (c.r3.u32 != 0)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xboxkrnl.exe!%s selector=0x%08X lr=0x%08X (only 0 is established)",
                    fn, c.r3.u32, uint32_t(c.lr));
    r.fsc_cache_elements.store(c.r4.u32, std::memory_order_relaxed);
    ret(c, nt::kSuccess);
}

}  // namespace

Status register_xboxkrnl_io_hle() {
    struct Impl {
        uint32_t ordinal;
        const char* name;
        PPCFunc* function;
    };
    const Impl entries[] = {
        {0x0021, "FscSetCacheElementCount", &FscSetCacheElementCount},
        {0x00DB, "NtFlushBuffersFile", &NtFlushBuffersFile},
        {0x00E4, "NtQueryDirectoryFile", &NtQueryDirectoryFile},
        {0x00E7, "NtQueryFullAttributesFile", &NtQueryFullAttributesFile},
        {0x00E8, "NtQueryInformationFile", &NtQueryInformationFile},
        {0x00EF, "NtQueryVolumeInformationFile", &NtQueryVolumeInformationFile},
        {0x00F7, "NtSetInformationFile", &NtSetInformationFile},
        {0x00FF, "NtWriteFile", &NtWriteFile},
    };
    for (const auto& i : entries) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, i.name, &ordinal) || ordinal != i.ordinal)
            return Status::InvalidArgument;
        const Status s =
            register_import(kModuleXboxkrnl, i.ordinal, i.function, i.name);
        if (s != Status::Ok) return s;
    }
    return Status::Ok;
}

bool file_has_completion_port(const GuestFile& file) { return file.completion_port(nullptr, nullptr); }

void post_file_completion(const GuestFile& file, uint32_t apc_context, uint32_t status, uint32_t information) {
    if (!apc_context || (status & 0x80000000u)) return;
    std::shared_ptr<HandleObject> port;
    uint32_t key = 0;
    if (!file.completion_port(&port, &key)) return;
    const Status posted = post_io_completion(port, key, apc_context, status, information);
    if (posted != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "I/O completion packet for %s could not be queued: %s",
                    file.guest_path().c_str(), status_name(posted));
}

void report_file_transfer_failure(const char* fn, const GuestFile& file, uint64_t offset, uint32_t length,
                                  uint32_t status, uint32_t transferred, uint32_t lr) {
    constexpr uint32_t kStatusEndOfFile = 0xC0000011u;
    if (!(status & 0x80000000u)) return;
    if (status == kStatusEndOfFile && offset != kFileUseFilePointerPosition && offset >= file.size()) return;
    if (status == kStatusEndOfFile && offset == kFileUseFilePointerPosition && file.position() >= file.size()) return;
    static std::atomic<uint32_t> reported{0};
    if (reported.fetch_add(1) >= 64) return;
    const unsigned long long at = offset == kFileUseFilePointerPosition ? (unsigned long long)file.position()
                                                                       : (unsigned long long)offset;
    std::fprintf(stderr, "RCOMP-IO %s failed path=%s offset=0x%llX%s length=0x%X status=0x%08X transferred=0x%X "
                         "size=0x%llX lr=0x%08X\n",
                 fn, file.guest_path().c_str(), at, offset == kFileUseFilePointerPosition ? "(current)" : "", length,
                 status, transferred, (unsigned long long)file.size(), lr);
    std::fflush(stderr);
}

}  // namespace rcomp::rt
