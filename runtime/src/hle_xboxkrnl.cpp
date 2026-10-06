#include <cstdio>
#include "physical_window.h"
#include "diagnostics.h"
#include "kernel_internal.h"
// Real xboxkrnl.exe HLE implementations (owner: Agent 3, runtime/).
//
// Each export documents the Xbox 360 semantics and what is implemented.
// Parameter combinations outside the implemented subset end in
// rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, ...) -- never in a fake success.
// Exports not registered here trap through hle_missing_import().
//
// Guest calling convention: integer args r3..r10, 9th+ args on the guest
// stack at r1 + 0x54 + 8*(n-8) (32-bit value in the low word of each 8-byte
// slot, big-endian); result in r3.
#include <string.h>

#include <mutex>
#include <new>
#include <set>
#include <atomic>
#include <chrono>
#include <string>

#include "rcomp/guest_write_tracking.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/clock_sync.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/io_event.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xma.h"
#include "rcomp/runtime/xaudio.h"
#include "rcomp/runtime/modules.h"
#include "rcomp/runtime/objects.h"
#include "rcomp/runtime/xboxkrnl_io.h"
#include "rcomp/runtime/xboxkrnl_devices.h"
#include "rcomp/runtime/format.h"
#include "rcomp/runtime/process_lifecycle.h"
#include "rcomp/runtime/query_memory.h"
#include "rcomp/runtime/xconfig.h"

namespace rcomp::rt {

namespace {

// ---- constants (NT / Xbox SDK values) -------------------------------------
constexpr uint32_t FILE_OPEN = 1;
constexpr uint32_t FILE_OPENED = 1;  // IO_STATUS_BLOCK.Information
constexpr uint32_t FILE_DIRECTORY_FILE = 0x00000001;
constexpr uint32_t kWriteAccessMask = 0x00000002 /*FILE_WRITE_DATA*/ | 0x00000004 /*APPEND*/ |
                                      0x00000010 /*WRITE_EA*/ | 0x00000100 /*WRITE_ATTRIBUTES*/ |
                                      0x00010000 /*DELETE*/ | 0x00040000 /*WRITE_DAC*/ |
                                      0x00080000 /*WRITE_OWNER*/ | 0x10000000 /*GENERIC_ALL*/ |
                                      0x40000000 /*GENERIC_WRITE*/;
constexpr uint32_t kObDosDevices = 0xFFFFFFFD;
constexpr uint64_t FILE_USE_FILE_POINTER_POSITION = 0xFFFFFFFFFFFFFFFEull;

Runtime& rt_or_die(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s called before rcomp::rt::runtime_init", fn);
    return *r;
}

[[noreturn]] void unimplemented(const char* fn, PPCContext& ctx, const char* what, uint32_t v) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xboxkrnl.exe!%s %s=0x%08X lr=0x%08X (not implemented)",
                fn, what, v, (uint32_t)ctx.lr);
}

void ret(PPCContext& ctx, uint32_t ntstatus) { ctx.r3.u64 = ntstatus; }

uint32_t stack_arg(PPCContext& ctx, int n, const char* fn) {
    uint32_t v = 0;
    const uint64_t address = uint64_t(ctx.r1.u32) + 0x54 + 8u * (uint32_t)(n - 8);
    Runtime& r = rt_or_die(fn);
    if (address > UINT32_MAX || !r.mem->is_accessible(address, 4, Protect::Read) ||
        !guest_read_be32(uint32_t(address), &v))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s: stack argument %d at 0x%08X not readable", fn,
                    n, uint32_t(address));
    return v;
}

// ---- NtCreateFile (0x00D2) / NtOpenFile (0x00DF) ---------------------------
// Xbox 360: NtCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
//   PIO_STATUS_BLOCK, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
//   ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions);
//   NtOpenFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
//   ULONG OpenOptions) == create with FILE_OPEN. OBJECT_ATTRIBUTES is
//   {HANDLE RootDirectory; PANSI_STRING ObjectName; ULONG Attributes} and
//   ANSI_STRING {USHORT Length; USHORT MaximumLength; PCHAR Buffer}.
//   Opens/creates files and directories on any mounted device, share modes,
//   async handles.
// RootDirectory 0 or ObDosDevices selects a configured VFS mount. Mutable
// dispositions are restricted by VFS to explicit save mounts; game:/d: remain
// read-only. Directory/flush/alignment options are passed to the real backend.
// All I/O currently completes synchronously. Relative handle roots and options
// with unimplemented effects remain explicit diagnostics.
void open_common(PPCContext& ctx, const char* fn, uint32_t phandle, uint32_t access,
                 uint32_t pattrs, uint32_t piosb, uint32_t disposition, uint32_t options) {
    Runtime& r = rt_or_die(fn);
    OpenRequest request;
    request.write_access = (access & kWriteAccessMask) != 0;
    switch (disposition) {
    case 1: request.disposition = OpenDisposition::Open; break;
    case 2: request.disposition = OpenDisposition::Create; break;
    case 3: request.disposition = OpenDisposition::OpenIf; break;
    case 4: request.disposition = OpenDisposition::Overwrite; break;
    case 5: request.disposition = OpenDisposition::OverwriteIf; break;
    default: unimplemented(fn, ctx, "create_disposition", disposition);
    }
    // 0x4 FILE_SEQUENTIAL_ONLY and 0x800 FILE_RANDOM_ACCESS are access-pattern hints to the cache manager
    // (no effect on the bytes read or written): accepted as hints. Halo 3 opens its maps with 0x848.
    // 0x10 FILE_SYNCHRONOUS_IO_ALERT: synchronous I/O like 0x20 (every R-comp transfer completes before
    // the call returns, so no wait exists that an alert could end); the XDK FATX formatter opens the raw
    // partition with 0x18. 0x800000 FILE_OPEN_FOR_FREE_SPACE_QUERY: the open serves a volume-size query;
    // it changes nothing else (the XDK utility-drive mount opens "\Device\Harddisk0\CacheN\" with 0x800021).
    constexpr uint32_t supported_options =
        0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x40u | 0x800u | 0x4000u | 0x800000u;
    if (options & ~supported_options) unimplemented(fn, ctx, "create_options", options);
    if ((options & (FILE_DIRECTORY_FILE | 0x40u)) == (FILE_DIRECTORY_FILE | 0x40u))
        return ret(ctx, nt::kInvalidParameter);
    if ((options & 0x30u) == 0x30u) return ret(ctx, nt::kInvalidParameter);  // both synchronous modes (NT)
    request.directory = (options & FILE_DIRECTORY_FILE) != 0;
    request.synchronous_io = (options & 0x30u) != 0;  // FILE_SYNCHRONOUS_IO_ALERT / _NONALERT
    request.write_through = (options & 2u) != 0;
    request.no_buffering = (options & 8u) != 0;
    if(!phandle || !r.mem->is_accessible(phandle,4,Protect::ReadWrite) ||
       (piosb && !r.mem->is_accessible(piosb,8,Protect::ReadWrite))) return ret(ctx,nt::kAccessViolation);
    uint32_t root = 0, name_ptr = 0, buf = 0;
    uint16_t len = 0;
    if (!pattrs) return ret(ctx, nt::kInvalidParameter);
    if (!r.mem->is_accessible(pattrs,12,Protect::Read) || !guest_read_be32(pattrs + 0, &root) || !guest_read_be32(pattrs + 4, &name_ptr))
        return ret(ctx, nt::kAccessViolation);
    if (root != 0 && root != kObDosDevices) unimplemented(fn, ctx, "root_directory", root);
    if (!name_ptr) return ret(ctx, nt::kObjectNameInvalid);
    if (!r.mem->is_accessible(name_ptr,8,Protect::Read) || !guest_read_be16(name_ptr, &len) || !guest_read_be32(name_ptr + 4, &buf))
        return ret(ctx, nt::kAccessViolation);
    const uint8_t* chars = len && r.mem->is_accessible(buf,len,Protect::Read) ? r.mem->translate(buf, len) : nullptr;
    if (len && !chars) return ret(ctx, nt::kAccessViolation);
    std::string path(reinterpret_cast<const char*>(chars ? chars : (const uint8_t*)""), len);

    uint32_t handle = 0;
    OpenAction action = OpenAction::Opened;
    Status s = r.vfs.open(r.handles, path, request, &handle, &action);
    uint32_t st = to_ntstatus(s);
    if (s == Status::Ok) guest_write_be32(phandle,handle);
#if RCOMP_RUNTIME_DIAGNOSTICS
    {   // bring-up tracing: which files the title opens
        static std::atomic<uint32_t> opens{0};
        if (opens.fetch_add(1) < 4000) fprintf(stderr, "RCOMP-OPEN %s st=0x%08X handle=0x%08X\n", path.c_str(), st, handle);
    }
#endif
    if (piosb) {
        guest_write_be32(piosb, st);
        guest_write_be32(piosb + 4, s == Status::Ok ? uint32_t(action) : 0);
    }
    ret(ctx, st);
}

void NtCreateFile(PPCContext& ctx, uint8_t*) {
    uint32_t options = stack_arg(ctx, 8, "NtCreateFile");
    open_common(ctx, "NtCreateFile", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32,
                options);
}

void NtOpenFile(PPCContext& ctx, uint8_t*) {
    open_common(ctx, "NtOpenFile", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, FILE_OPEN,
                ctx.r7.u32);
}

// ---- NtReadFile (0x00F0) ---------------------------------------------------
// Xbox 360: NtReadFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID
//   ApcContext, PIO_STATUS_BLOCK, PVOID Buffer, ULONG Length, PLARGE_INTEGER
//   ByteOffset). Sync or async read; signals Event / queues APC on completion.
// Implemented: synchronous completion, optional real Event and no APC. The
// event is reset only after all arguments and handle types have been checked,
// then signalled after the buffer and IO_STATUS_BLOCK have been written.
// ByteOffset NULL or
//   FILE_USE_FILE_POINTER_POSITION -> current position; otherwise absolute.
//   Position advances by the bytes read. Start at/after EOF ->
//   STATUS_END_OF_FILE (Information 0). Wrong handle kind ->
//   STATUS_OBJECT_TYPE_MISMATCH, closed/unknown -> STATUS_INVALID_HANDLE.
// Completion port: a file associated with one (FileCompletionInformation)
//   receives {Key, ApcContext, Status, Information} after a successful read
//   with a non-NULL ApcContext (post_file_completion); an APC routine on such
//   a file is STATUS_INVALID_PARAMETER.
// APC routine: queued to the calling thread after the transfer (io_event.h
//   queue_file_apc), the I/O completion APC of ReadFileEx.
void NtReadFile(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtReadFile";
    Runtime& r = rt_or_die(fn);
    uint32_t h = ctx.r3.u32, ev = ctx.r4.u32, apc = ctx.r5.u32, apc_context = ctx.r6.u32, piosb = ctx.r7.u32;
    uint32_t buf = ctx.r8.u32, len = ctx.r9.u32, poff = ctx.r10.u32;
    if (apc) {
        std::shared_ptr<GuestFile> associated;
        if (r.handles.lookup_as<GuestFile>(h, &associated) == Status::Ok && file_has_completion_port(*associated))
            return ret(ctx, nt::kInvalidParameter);  // NT: no APC routine on a port-associated file
        file_apc_routine(fn, apc, uint32_t(ctx.lr));
    }
    // Preflight every memory range before advancing the file position or
    // changing event state. translate() alone does not check access rights.
    if ((piosb && !r.mem->is_accessible(piosb, 8, Protect::ReadWrite)) ||
        (len && !r.mem->is_accessible(buf, len, Protect::ReadWrite)) ||
        (poff && !r.mem->is_accessible(poff, 8, Protect::Read)))
        return ret(ctx, nt::kAccessViolation);
    uint64_t off = FILE_USE_FILE_POINTER_POSITION;
    if (poff && !guest_read_be64(poff, &off)) return ret(ctx, nt::kAccessViolation);
    if (off != FILE_USE_FILE_POINTER_POSITION && (off >> 63))
        return ret(ctx, nt::kInvalidParameter);
    std::shared_ptr<GuestFile> file;
    Status s = r.handles.lookup_as<GuestFile>(h, &file);
    std::shared_ptr<HandleObject> event;
    if (s == Status::Ok && ev) s = reference_io_event(ev, &event);
    uint32_t got = 0;
    if (s == Status::Ok) {
        uint8_t* dst = len ? r.mem->host(buf) : nullptr;
        if (event) set_io_event(event, false);
        if (off == FILE_USE_FILE_POINTER_POSITION) s = file->read(dst, len, &got);
        else s = file->read_at(off, dst, len, &got);
        // Recorded for the GPU caches whatever the outcome: a failed read
        // may still have stored part of the buffer.
        note_title_write(buf, len);
    }
#if RCOMP_RUNTIME_DIAGNOSTICS
    {   // bring-up tracing: read volume
        static std::atomic<uint64_t> reads{0}, bytes{0};
        bytes += got;
        const uint64_t n = reads.fetch_add(1) + 1;
        if (n % 500 == 0) fprintf(stderr, "RCOMP-READ reads=%llu bytes=%llu\n", (unsigned long long)n, (unsigned long long)bytes.load());
        const uint32_t st = to_ntstatus(s);
        if (st != 0 || got != len) {
            static std::atomic<uint32_t> odd{0};
            if (odd.fetch_add(1) < 200) fprintf(stderr, "RCOMP-READ-ODD handle=0x%08X st=0x%08X len=%u got=%u off=0x%llX\n", h, st, len, got, (unsigned long long)off);
        }
        if (n > 7300 && n <= 7500) fprintf(stderr, "RCOMP-READ-LATE t=%llums #%llu handle=0x%08X len=%u off=0x%llX thread=%u\n", (unsigned long long)(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % 100000000ull), (unsigned long long)n, h, len, (unsigned long long)off, current_guest_thread() ? current_guest_thread()->thread_id : 0u);
    }
#endif
    uint32_t st = to_ntstatus(s);
    if (piosb) {
        guest_write_be32(piosb, st);
        guest_write_be32(piosb + 4, got);
    }
    if (event) set_io_event(event, true);
    if (file) {
        report_file_transfer_failure(fn, *file, off, len, st, got, uint32_t(ctx.lr));
        post_file_completion(*file, apc_context, st, got);
    }
    queue_file_apc(fn, apc, apc_context, piosb, st, uint32_t(ctx.lr));
    ret(ctx, st);
}

// ---- NtClose (0x00CF) ------------------------------------------------------
// Xbox 360: closes any object handle. Implemented for every handle in the
// runtime handle table; unknown/closed -> STATUS_INVALID_HANDLE.
void NtClose(PPCContext& ctx, uint8_t*) {
    Runtime& r = rt_or_die("NtClose");
    Status s = r.handles.close(ctx.r3.u32);
    ret(ctx, to_ntstatus(s));
}

// ---- KeQuerySystemTime (0x0084) --------------------------------------------
// Xbox 360: VOID KeQuerySystemTime(PLARGE_INTEGER CurrentTime): system time
// as FILETIME (100 ns since 1601-01-01 UTC). Implemented from the host
// real-time clock (CLOCK_REALTIME); an unwritable pointer is a guest fault.
void KeQuerySystemTime(PPCContext& ctx, uint8_t*) {
    Runtime& r=rt_or_die("KeQuerySystemTime");
    uint32_t p = ctx.r3.u32;
    if (!r.mem->is_accessible(p,8,Protect::ReadWrite) || !guest_write_be64(p, system_filetime()))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "KeQuerySystemTime: out pointer 0x%08X lr=0x%08X", p,
                    (uint32_t)ctx.lr);
}

// ---- ExTerminateThread (0x0019) --------------------------------------------
// Xbox 360: VOID ExTerminateThread(DWORD ExitStatus): ends the calling thread
// (thread object signalled, TLS/notification callbacks run, handle kept until
// closed). Implemented: unwinds to run_guest_thread() of the calling host
// thread with ExitStatus as exit code. No thread object / callbacks exist yet.
void ExTerminateThread(PPCContext& ctx, uint8_t*) {
    exit_current_guest_thread(ctx.r3.u32);
}

struct Impl {
    uint32_t ordinal;
    const char* name;
    PPCFunc* fn;
};

const Impl kImpls[] = {
    {0x0019, "ExTerminateThread", &ExTerminateThread},
    {0x0084, "KeQuerySystemTime", &KeQuerySystemTime},
    {0x00CF, "NtClose", &NtClose},
    {0x00D2, "NtCreateFile", &NtCreateFile},
    {0x00DF, "NtOpenFile", &NtOpenFile},
    {0x00F0, "NtReadFile", &NtReadFile},
};

}  // namespace

Status register_xboxkrnl_hle() {
    for (const Impl& i : kImpls) {
        // Cross-check our ordinal against the export table the generator uses.
        uint32_t ord = 0;
        if (!export_ordinal(kModuleXboxkrnl, i.name, &ord) || ord != i.ordinal)
            rcomp_fatal(RCOMP_FATAL_INTERNAL, "xboxkrnl HLE %s: ordinal 0x%04X not in export table",
                        i.name, i.ordinal);
        Status s = register_import(kModuleXboxkrnl, i.ordinal, i.fn, i.name);
        if (s != Status::Ok) return s;
    }
    Status s = register_xboxkrnl_threading_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_memory_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_pool_rtl_hle();
    if(s!=Status::Ok)return s;
    s=register_xboxkrnl_module_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_time_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_sched_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_io_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_object_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_device_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_strings_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_format_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_xconfig_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_process_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_xma_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_xaudio_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_seh_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_more_hle();
    if (s != Status::Ok) return s;
    s = register_xboxkrnl_dpc_hle();
    return s != Status::Ok ? s : register_xboxkrnl_query_memory_hle();
}

}  // namespace rcomp::rt
