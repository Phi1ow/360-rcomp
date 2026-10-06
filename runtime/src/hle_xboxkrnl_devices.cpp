// xboxkrnl device objects, share access, title symbolic links, L2 locking,
// console-key signatures and the remaining file-handle I/O exports
// (owner: Agent 3, runtime/).
//
// Every export documents the Xbox 360 contract it implements and its subset.
// Outside the subset a call ends in rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED or
// RCOMP_FATAL_GUEST_ACCESS); a request R-comp cannot perform returns an
// honest NTSTATUS error and prints one RCOMP-DECLINED line per export. No
// export reports success for work it did not do.
//
// ABI sources. Ordinals: XenonRecomp's copy of Xenia's export table. Argument
// order and the layouts used below were read from the GTA IV / Episodes from
// Liberty City call sites in the generated code (an AOT translation of the
// XDK library code linked into those titles), cross-checked with public
// references: Xenia / rexglue-sdk sources (signatures only, no code copied),
// the Free60 "Console Security Certificate" table, and the NT DDK contracts
// of the same-named Windows NT routines (SHARE_ACCESS algorithm, generic
// access mapping). Facts taken from the title code are noted "(title)".
//
// IoCompleteRequest and IoInvalidDeviceRequest are registered by
// src/hle_xboxkrnl_more.cpp as explicit diagnostics: R-comp has no I/O manager
// that builds or dispatches IRPs (IoAllocateIrp, IoBuild*Request and
// IoCallDriver are not implemented and NtCreateFile/NtReadFile never route to
// a guest driver), so no IRP they could complete exists. RtlCaptureContext,
// RtlUnwind and __C_specific_handler live in src/hle_xboxkrnl_seh.cpp
// (runtime/docs/SEH.md).
#include "physical_window.h"
#include "rcomp/runtime/xboxkrnl_devices.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/guest_write_tracking.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/hdd.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/io_event.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/vfs.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kStatusInvalidDeviceRequest = 0xC0000010u;
constexpr uint32_t kStatusSharingViolation = 0xC0000043u;
constexpr uint32_t kStatusNotSupported = 0xC00000BBu;
constexpr uint64_t kUseFilePointerPosition = 0xFFFFFFFFFFFFFFFEull;

Runtime& rt_or_die(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s before runtime_init", fn);
    return *r;
}

[[noreturn]] void unimplemented(const char* fn, PPCContext& ctx, const char* what, uint32_t value) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xboxkrnl.exe!%s %s=0x%08X lr=0x%08X (not implemented)", fn,
                what, value, (uint32_t)ctx.lr);
}

[[noreturn]] void misuse(const char* fn, PPCContext& ctx, const char* what, uint32_t value) {
    rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s %s=0x%08X lr=0x%08X", fn, what, value,
                (uint32_t)ctx.lr);
}

void ret(PPCContext& ctx, uint32_t value) { ctx.r3.u64 = value; }

// One line per export the first time R-comp declines a request with an error.
void declined_once(std::atomic<bool>& flag, const char* fn, const char* why, uint32_t a, uint32_t b,
                   uint32_t lr) {
    if (!flag.exchange(true))
        fprintf(stderr, "RCOMP-DECLINED xboxkrnl.exe!%s %s (0x%08X 0x%08X) lr=0x%08X\n", fn, why, a, b, lr);
}

void store_bytes(Runtime& r, uint32_t address, const void* data, uint32_t size) {
    memcpy(r.mem->host(address), data, size);
    note_title_write(address, size);
}

void put_be16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// ANSI_STRING {USHORT Length; USHORT MaximumLength; PCHAR Buffer}.
Status read_ansi(Runtime& r, uint32_t descriptor, std::string* out) {
    if (!descriptor) return Status::InvalidArgument;
    if (!r.mem->is_accessible(descriptor, 8, Protect::Read)) return Status::GuestFault;
    uint16_t length = 0, maximum = 0;
    uint32_t buffer = 0;
    if (!guest_read_be16(descriptor, &length) || !guest_read_be16(descriptor + 2, &maximum) ||
        !guest_read_be32(descriptor + 4, &buffer))
        return Status::GuestFault;
    if (maximum < length) return Status::InvalidArgument;
    if (length && (!buffer || !r.mem->is_accessible(buffer, length, Protect::Read))) return Status::GuestFault;
    out->assign(length ? reinterpret_cast<const char*>(r.mem->host(buffer)) : "", length);
    return Status::Ok;
}

std::string ascii_lower(std::string_view in) {
    std::string out(in);
    for (char& ch : out)
        if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
    return out;
}

// ---- object namespace ------------------------------------------------------
// R-comp's object namespace holds the root directory "\", "\??" (DOS devices;
// "\DosDevices" is its alias) and "\Device". Names are case-insensitive.
// Returns 0 and the canonical (lower-case, "\??\"-prefixed) name, or the
// NTSTATUS for an invalid name / a directory R-comp does not have.
uint32_t canonical_object_name(std::string_view name, std::string* out) {
    if (name.empty() || name[0] != '\\') return nt::kObjectNameInvalid;
    std::string lower = ascii_lower(name);
    if (lower.compare(0, 12, "\\dosdevices\\") == 0) lower = "\\??\\" + lower.substr(12);
    const size_t slash = lower.find('\\', 1);
    std::string component;
    if (slash == std::string::npos) {
        component = lower.substr(1);
    } else {
        const std::string directory = lower.substr(0, slash);
        if (directory != "\\??" && directory != "\\device") return nt::kObjectPathNotFound;
        component = lower.substr(slash + 1);
        if (component.find('\\') != std::string::npos) return nt::kObjectPathNotFound;
    }
    if (component.empty()) return nt::kObjectNameInvalid;
    for (char ch : component)
        if ((unsigned char)ch < 0x20) return nt::kObjectNameInvalid;
    *out = std::move(lower);
    return nt::kSuccess;
}

// "\??\x:" names a drive the runtime configured as a VFS mount. Called
// without objects().mu held: the VFS takes its own lock, and a VFS that
// resolves title links calls back into this file.
bool names_configured_mount(Runtime& r, const std::string& canonical) {
    if (canonical.compare(0, 4, "\\??\\") != 0 || canonical.size() < 6 || canonical.back() != ':') return false;
    std::string host;
    return r.vfs.resolve(canonical.substr(4) + "\\x", &host) == Status::Ok;
}

// Size R-comp allocates for a DEVICE_OBJECT before its extension. The fields
// written below end at +0x28; the original Xbox structure is 0x50 bytes and
// the Xbox 360 one is not published, so the rest of 0x60 stays zero.
constexpr uint32_t kDeviceObjectSize = 0x60;
constexpr uint16_t kIoTypeDevice = 3;
constexpr uint32_t kDoDeviceInitializing = 0x10;  // (title) cleared at the end of its own setup

struct DeviceRecord {
    uint32_t size = 0;          // allocation (object + extension)
    uint32_t driver = 0;
    uint32_t fifth_argument = 0;  // r7 of IoCreateDevice; meaning not established
    std::string name;           // canonical, empty for an unnamed device
    uint32_t references = 0;    // creation reference + ObReferenceObject
    bool deleted = false;
};

struct FileAccess {
    bool read = false, write = false, del = false;
    bool shared_read = false, shared_write = false, shared_delete = false;
};

struct KernelObjects {
    std::mutex mu;
    uint64_t generation = 0;
    std::map<uint32_t, DeviceRecord> devices;          // by DEVICE_OBJECT address
    std::map<std::string, std::string> links;          // canonical link name -> target as given
    std::unordered_map<uint32_t, FileAccess> file_access;  // by FILE_OBJECT address
};

KernelObjects& objects() {
    static KernelObjects* state = new KernelObjects();  // never destroyed: used at exit by late threads
    return *state;
}

// Caller holds objects().mu. A new runtime lifetime starts with an empty
// namespace: guest memory of the previous one is gone.
KernelObjects& sync_locked(Runtime& r) {
    KernelObjects& s = objects();
    if (s.generation != r.generation) {
        s.devices.clear();
        s.links.clear();
        s.file_access.clear();
        s.generation = r.generation;
    }
    return s;
}

// `mounted` = names_configured_mount(), evaluated before taking the lock.
bool name_in_use_locked(KernelObjects& s, const std::string& canonical, bool mounted) {
    if (mounted || s.links.count(canonical)) return true;
    for (const auto& d : s.devices)
        if (!d.second.deleted && d.second.name == canonical) return true;
    return false;
}

// ---- SHA-1 / HMAC-SHA-1 (FIPS 180-4, RFC 2104) -----------------------------
class Sha1 {
public:
    void update(const void* data, size_t size) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total_ += size;
        while (size) {
            const size_t take = std::min(size, sizeof(block_) - used_);
            memcpy(block_ + used_, p, take);
            used_ += take; p += take; size -= take;
            if (used_ == sizeof(block_)) { compress(); used_ = 0; }
        }
    }
    void final(uint8_t out[20]) {
        const uint64_t bits = total_ * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (used_ != 56) update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; ++i) length[i] = uint8_t(bits >> (56 - 8 * i));
        update(length, 8);
        for (int i = 0; i < 5; ++i) put_be32(out + 4 * i, h_[i]);
    }

private:
    static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
    void compress() {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(block_[4 * i]) << 24 | uint32_t(block_[4 * i + 1]) << 16 |
                   uint32_t(block_[4 * i + 2]) << 8 | block_[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
    }
    uint32_t h_[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t block_[64] = {};
    size_t used_ = 0;
    uint64_t total_ = 0;
};

struct Part { const void* data; size_t size; };

void hmac_sha1(const uint8_t* key, size_t key_size, const Part* parts, size_t count, uint8_t out[20]) {
    uint8_t k[64] = {};
    if (key_size > 64) { Sha1 h; h.update(key, key_size); h.final(k); }
    else memcpy(k, key, key_size);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = uint8_t(k[i] ^ 0x36); opad[i] = uint8_t(k[i] ^ 0x5C); }
    uint8_t inner[20];
    Sha1 a; a.update(ipad, 64);
    for (size_t i = 0; i < count; ++i) a.update(parts[i].data, parts[i].size);
    a.final(inner);
    Sha1 b; b.update(opad, 64); b.update(inner, 20); b.final(out);
}

// ---- IoCheckShareAccess (0x0034) / IoSetShareAccess (0x0047) /
//      IoRemoveShareAccess (0x0045) ----------------------------------------
// Xbox 360: NTSTATUS IoCheckShareAccess(ACCESS_MASK DesiredAccess,
//   ULONG DesiredShareAccess, PFILE_OBJECT, PSHARE_ACCESS, BOOLEAN Update);
//   VOID IoSetShareAccess(ACCESS_MASK, ULONG, PFILE_OBJECT, PSHARE_ACCESS);
//   VOID IoRemoveShareAccess(PFILE_OBJECT, PSHARE_ACCESS) (title: argument
//   registers at the file-system driver's create/cleanup call sites).
// SHARE_ACCESS is seven UCHARs {OpenCount, Readers, Writers, Deleters,
//   SharedRead, SharedWrite, SharedDelete} (original Xbox layout; (title) the
//   driver tests OpenCount as the byte at offset 0 to choose Set or Check).
// Algorithm: the NT one. Read = FILE_READ_DATA|FILE_EXECUTE, write =
//   FILE_WRITE_DATA|FILE_APPEND_DATA, delete = DELETE (GENERIC_* bits mapped
//   with the file generic mapping first). Without any of them the open does
//   not take part in share accounting. Conflict -> STATUS_SHARING_VIOLATION
//   and nothing changes; Update counts the open.
// The per-open access flags NT keeps in FILE_OBJECT bit fields are kept by
//   the runtime, keyed by the FILE_OBJECT address, because the Xbox 360 bit
//   layout is not established ((title) the byte at +2 holds FO_* flags; the
//   driver never reads the access bits itself). Set/Check record them,
//   Remove consumes them; a FILE_OBJECT never recorded has no access, as a
//   freshly zeroed one has on NT. A UCHAR counter that would overflow or
//   underflow stops with a diagnostic.
constexpr uint32_t kFileReadData = 0x1, kFileWriteData = 0x2, kFileAppendData = 0x4, kFileExecute = 0x20;
constexpr uint32_t kDelete = 0x10000;
constexpr uint32_t kGenericRead = 0x80000000u, kGenericWrite = 0x40000000u, kGenericExecute = 0x20000000u,
                   kGenericAll = 0x10000000u;

FileAccess access_for(uint32_t desired, uint32_t share) {
    if (desired & kGenericAll) desired |= kFileReadData | kFileWriteData | kFileAppendData | kFileExecute | kDelete;
    if (desired & kGenericRead) desired |= kFileReadData;
    if (desired & kGenericWrite) desired |= kFileWriteData | kFileAppendData;
    if (desired & kGenericExecute) desired |= kFileExecute;
    FileAccess a;
    a.read = (desired & (kFileReadData | kFileExecute)) != 0;
    a.write = (desired & (kFileWriteData | kFileAppendData)) != 0;
    a.del = (desired & kDelete) != 0;
    a.shared_read = (share & 1u) != 0;
    a.shared_write = (share & 2u) != 0;
    a.shared_delete = (share & 4u) != 0;
    return a;
}

uint8_t* share_access_bytes(Runtime& r, const char* fn, PPCContext& ctx, uint32_t address) {
    if (!address || !r.mem->is_accessible(address, 7, Protect::ReadWrite))
        misuse(fn, ctx, "share_access_unwritable", address);
    return r.mem->host(address);
}

void check_file_object(Runtime& r, const char* fn, PPCContext& ctx, uint32_t file_object) {
    if (!file_object || !r.mem->is_accessible(file_object, 4, Protect::Read))
        misuse(fn, ctx, "file_object_unreadable", file_object);
}

enum { kOpen = 0, kReaders, kWriters, kDeleters, kSharedRead, kSharedWrite, kSharedDelete };

// Adds (+1) or removes (-1) one open's contribution; false on UCHAR overflow/underflow.
bool apply_open(uint8_t counts[7], const FileAccess& a, int delta) {
    const bool member[7] = {true, a.read, a.write, a.del, a.shared_read, a.shared_write, a.shared_delete};
    for (int i = 0; i < 7; ++i) {
        const int v = int(counts[i]) + (member[i] ? delta : 0);
        if (v < 0 || v > 0xFF) return false;
    }
    for (int i = 0; i < 7; ++i)
        if (member[i]) counts[i] = uint8_t(int(counts[i]) + delta);
    return true;
}

void IoCheckShareAccess(PPCContext& ctx, uint8_t*) {
    const char* fn = "IoCheckShareAccess";
    Runtime& r = rt_or_die(fn);
    const uint32_t file_object = ctx.r5.u32, share_address = ctx.r6.u32;
    const bool update = (ctx.r7.u32 & 0xFF) != 0;
    check_file_object(r, fn, ctx, file_object);
    uint8_t* bytes = share_access_bytes(r, fn, ctx, share_address);
    const FileAccess a = access_for(ctx.r3.u32, ctx.r4.u32);
    {
        std::lock_guard<std::mutex> lock(objects().mu);
        sync_locked(r).file_access[file_object] = a;
    }
    if (!a.read && !a.write && !a.del) return ret(ctx, nt::kSuccess);
    uint8_t counts[7];
    memcpy(counts, bytes, 7);
    if ((a.read && counts[kSharedRead] < counts[kOpen]) || (a.write && counts[kSharedWrite] < counts[kOpen]) ||
        (a.del && counts[kSharedDelete] < counts[kOpen]) || (counts[kReaders] && !a.shared_read) ||
        (counts[kWriters] && !a.shared_write) || (counts[kDeleters] && !a.shared_delete))
        return ret(ctx, kStatusSharingViolation);
    if (update) {
        if (!apply_open(counts, a, +1)) unimplemented(fn, ctx, "share_access_counter_overflow", share_address);
        store_bytes(r, share_address, counts, 7);
    }
    ret(ctx, nt::kSuccess);
}

void IoSetShareAccess(PPCContext& ctx, uint8_t*) {
    const char* fn = "IoSetShareAccess";
    Runtime& r = rt_or_die(fn);
    const uint32_t file_object = ctx.r5.u32, share_address = ctx.r6.u32;
    check_file_object(r, fn, ctx, file_object);
    share_access_bytes(r, fn, ctx, share_address);
    const FileAccess a = access_for(ctx.r3.u32, ctx.r4.u32);
    {
        std::lock_guard<std::mutex> lock(objects().mu);
        sync_locked(r).file_access[file_object] = a;
    }
    uint8_t counts[7] = {};
    if (a.read || a.write || a.del) apply_open(counts, a, +1);
    store_bytes(r, share_address, counts, 7);
}

void IoRemoveShareAccess(PPCContext& ctx, uint8_t*) {
    const char* fn = "IoRemoveShareAccess";
    Runtime& r = rt_or_die(fn);
    const uint32_t file_object = ctx.r3.u32, share_address = ctx.r4.u32;
    check_file_object(r, fn, ctx, file_object);
    uint8_t* bytes = share_access_bytes(r, fn, ctx, share_address);
    FileAccess a;
    {
        std::lock_guard<std::mutex> lock(objects().mu);
        KernelObjects& s = sync_locked(r);
        auto it = s.file_access.find(file_object);
        if (it != s.file_access.end()) { a = it->second; s.file_access.erase(it); }
    }
    if (!a.read && !a.write && !a.del) return;
    uint8_t counts[7];
    memcpy(counts, bytes, 7);
    if (!apply_open(counts, a, -1)) misuse(fn, ctx, "share_access_counter_underflow", share_address);
    store_bytes(r, share_address, counts, 7);
}

// ---- IoCreateDevice (0x0037) -----------------------------------------------
// Xbox 360: NTSTATUS IoCreateDevice(PDRIVER_OBJECT, ULONG DeviceExtensionSize,
//   POBJECT_STRING DeviceName OPTIONAL, DEVICE_TYPE, <r7>, PDEVICE_OBJECT*)
//   (title: six argument registers; original Xbox order). r7 (the original
//   Xbox "Exclusive"; the title passes 0x1000 or 0x2000) has no established
//   meaning: it is recorded, not interpreted.
// Implemented: a zeroed guest allocation of kDeviceObjectSize + the extension
//   (16-byte aligned) with Type=IO_TYPE_DEVICE (+0x00, USHORT),
//   DriverObject (+0x08), Flags=DO_DEVICE_INITIALIZING (+0x14),
//   DeviceExtension (+0x18, the zeroed extension right after the object),
//   DeviceType (+0x1C, UCHAR), StackSize=1 (+0x1E). (title) the driver reads
//   +0x18 and +0x24 (AlignmentRequirement, 0 = byte alignment) and writes
//   +0x0C, +0x14, +0x20, +0x24, matching the original Xbox DEVICE_OBJECT.
// The name joins R-comp's object namespace (collision ->
//   STATUS_OBJECT_NAME_COLLISION; a directory other than \, \?? or \Device ->
//   STATUS_OBJECT_PATH_NOT_FOUND). The object holds one creation reference,
//   released by IoDeleteDevice; ObReferenceObject/ObDereferenceObject count
//   further references and the memory is freed when the last one goes.
// Limits: no I/O ever reaches the device (R-comp dispatches no IRP to guest
//   drivers), so files on it cannot be opened; DeviceType above 0xFF traps.
//   Exception, with a hard drive configured: "\Device\cache0"/"\Device\cache1"
//   are the XDK secure file cache of a utility partition, and the paths under
//   them reach that partition's file system (rcomp/runtime/hdd.h,
//   runtime/docs/HDD.md) until IoDeleteDevice.
void IoCreateDevice(PPCContext& ctx, uint8_t*) {
    const char* fn = "IoCreateDevice";
    Runtime& r = rt_or_die(fn);
    const uint32_t driver = ctx.r3.u32, extension = ctx.r4.u32, name_ptr = ctx.r5.u32;
    const uint32_t type = ctx.r6.u32, fifth = ctx.r7.u32, out = ctx.r8.u32;
    if (type > 0xFF) unimplemented(fn, ctx, "device_type", type);
    if (!out || (out & 3) || !r.mem->is_accessible(out, 4, Protect::ReadWrite)) return ret(ctx, nt::kAccessViolation);
    std::string canonical;
    if (name_ptr) {
        std::string name;
        const Status s = read_ansi(r, name_ptr, &name);
        if (s == Status::GuestFault) return ret(ctx, nt::kAccessViolation);
        if (s != Status::Ok) return ret(ctx, nt::kObjectNameInvalid);
        const uint32_t st = canonical_object_name(name, &canonical);
        if (st != nt::kSuccess) return ret(ctx, st);
    }
    if (extension > 0x7FFF0000u) return ret(ctx, nt::kInsufficientResources);
    const uint32_t size = kDeviceObjectSize + ((extension + 15u) & ~15u);
    const bool mounted = !canonical.empty() && names_configured_mount(r, canonical);

    uint32_t address = 0;
    {
        std::lock_guard<std::mutex> lock(objects().mu);
        KernelObjects& s = sync_locked(r);
        if (!canonical.empty() && name_in_use_locked(s, canonical, mounted))
            return ret(ctx, nt::kObjectNameCollision);
        if (r.heap.alloc(size, 16, true, &address) != Status::Ok) return ret(ctx, nt::kInsufficientResources);
        DeviceRecord record;
        record.size = size;
        record.driver = driver;
        record.fifth_argument = fifth;
        record.name = canonical;
        record.references = 1;
        s.devices[address] = std::move(record);
    }
    uint8_t object[0x20] = {};
    put_be16(object + 0x00, kIoTypeDevice);
    put_be32(object + 0x08, driver);
    put_be32(object + 0x14, kDoDeviceInitializing);
    put_be32(object + 0x18, address + kDeviceObjectSize);
    object[0x1C] = uint8_t(type);
    object[0x1E] = 1;
    store_bytes(r, address, object, sizeof(object));
    note_title_write(address, size);  // zeroed by the heap
    guest_write_be32(out, address);
    // "\Device\cache0/1": the XDK secure file cache of a utility partition (rcomp/runtime/hdd.h).
    if (!canonical.empty()) hdd_title_device_created(canonical);
    ret(ctx, nt::kSuccess);
}

// ---- IoDeleteDevice (0x0039) -----------------------------------------------
// Xbox 360: VOID IoDeleteDevice(PDEVICE_OBJECT). Removes the name from the
// namespace and releases the creation reference; the memory goes with the
// last reference. A pointer that is not a live device from IoCreateDevice,
// or a second delete, is guest misuse.
void IoDeleteDevice(PPCContext& ctx, uint8_t*) {
    const char* fn = "IoDeleteDevice";
    Runtime& r = rt_or_die(fn);
    const uint32_t device = ctx.r3.u32;
    bool known = false, free_now = false;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(objects().mu);
        KernelObjects& s = sync_locked(r);
        auto it = s.devices.find(device);
        if (it != s.devices.end() && !it->second.deleted) {
            known = true;
            it->second.deleted = true;
            name.swap(it->second.name);
            if (--it->second.references == 0) { s.devices.erase(it); free_now = true; }
        }
    }
    if (!known) misuse(fn, ctx, "unknown_or_deleted_device", device);
    if (!name.empty()) hdd_title_device_deleted(name);
    if (free_now && r.heap.free(device) != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s could not free device 0x%08X", fn, device);
}

// ---- IoDismountVolume (0x003B) / IoDismountVolumeByFileHandle (0x003C) ----
// Xbox 360: NTSTATUS IoDismountVolume(PDEVICE_OBJECT);
//   NTSTATUS IoDismountVolumeByFileHandle(HANDLE) (title: one argument each).
//   They tear down the file system mounted on a device; its open files fail
//   afterwards and the next open mounts it again.
// R-comp cannot do that: a device from IoCreateDevice has no file system
//   instance (its I/O is never dispatched), and a file handle lives on a
//   host-directory mount that is runtime configuration whose other open
//   handles cannot be invalidated. Both answer STATUS_NOT_SUPPORTED (the GTA
//   IV / TBoGT callers ignore the status). A bad handle gives
//   STATUS_INVALID_HANDLE / STATUS_OBJECT_TYPE_MISMATCH; a device pointer that
//   R-comp did not create traps.
std::atomic<bool> g_declined_dismount{false}, g_declined_dismount_handle{false};

void IoDismountVolume(PPCContext& ctx, uint8_t*) {
    const char* fn = "IoDismountVolume";
    Runtime& r = rt_or_die(fn);
    const uint32_t device = ctx.r3.u32;
    bool known = false;
    {
        std::lock_guard<std::mutex> lock(objects().mu);
        KernelObjects& s = sync_locked(r);
        auto it = s.devices.find(device);
        known = it != s.devices.end();
    }
    if (!known) unimplemented(fn, ctx, "device_not_created_by_IoCreateDevice", device);
    declined_once(g_declined_dismount, fn, "no file system instance on a title device", device, 0,
                  (uint32_t)ctx.lr);
    ret(ctx, kStatusNotSupported);
}

void IoDismountVolumeByFileHandle(PPCContext& ctx, uint8_t*) {
    const char* fn = "IoDismountVolumeByFileHandle";
    Runtime& r = rt_or_die(fn);
    std::shared_ptr<GuestFile> file;
    const Status s = r.handles.lookup_as<GuestFile>(ctx.r3.u32, &file);
    if (s != Status::Ok) return ret(ctx, to_ntstatus(s));
    declined_once(g_declined_dismount_handle, fn, "host-backed VFS volumes cannot be dismounted", ctx.r3.u32, 0,
                  (uint32_t)ctx.lr);
    ret(ctx, kStatusNotSupported);
}

// ---- KeLockL2 (0x006B) / KeUnlockL2 (0x006C) -------------------------------
// Xbox 360: KeLockL2(index, address, size, ...) reserves L2 cache ways for an
// address range (title: index 0, 0x7F100000, 128/256 KiB, then the caller
// zeroes the range itself with dcbz128); KeUnlockL2(index) releases them.
// Cache locking changes timing only, never memory contents, and the PS5 has
// no equivalent: both are no-ops. KeLockL2 returns 0, the value Xenia
// returns; the GTA IV / TBoGT caller ignores it.
void KeLockL2(PPCContext& ctx, uint8_t*) { ret(ctx, 0); }
void KeUnlockL2(PPCContext&, uint8_t*) {}

// ---- NtDeviceIoControlFile (0x00D9) ----------------------------------------
// Xbox 360: NtDeviceIoControlFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE,
//   PVOID ApcContext, PIO_STATUS_BLOCK, ULONG IoControlCode, PVOID InputBuffer,
//   ULONG InputLength, PVOID OutputBuffer, ULONG OutputLength) (title: the
//   9th/10th arguments on the stack, r1+0x54 and r1+0x5C). The device behind
//   the handle answers.
// A raw hard-drive device ("\Device\Harddisk0\Partition0", "...\Cache0/1",
//   runtime/docs/HDD.md) answers the two disk requests the XDK FATX formatter
//   sends (Halo 3, GTA IV, TBoGT, GoW2: sub at the "XTAF" writer):
//   IOCTL_DISK_GET_DRIVE_GEOMETRY 0x70000, output 8 bytes {ULONG Sectors;
//     ULONG BytesPerSector} ((title) length 8, BytesPerSector read at +4; +0
//     is the device's sector count, the layout Xenia's xboxkrnl_io.cc answers
//     too), Information 8;
//   IOCTL_DISK_GET_PARTITION_INFO 0x74004, output 16 bytes {LONGLONG
//     StartingOffset; LONGLONG PartitionLength} (NT PARTITION_INFORMATION's
//     first two fields; (title) length 16, PartitionLength read at +8),
//     Information 16.
//   No input is used. A shorter output -> STATUS_BUFFER_TOO_SMALL, an
//   unwritable one -> STATUS_ACCESS_VIOLATION; any other code on a raw device
//   traps (no title sends one). Completion as NtReadFile: synchronous,
//   IO_STATUS_BLOCK written, event reset then signalled, APC queued.
// Files and directories opened by the VFS have no device that answers an
//   IOCTL: every code is STATUS_INVALID_DEVICE_REQUEST, the status a file
//   system gives for a control request it does not handle.
// Unknown handle -> STATUS_INVALID_HANDLE; other kind or an event handle that
//   is not an event -> as NtReadFile. As on NT, a request that fails
//   synchronously writes no IO_STATUS_BLOCK and signals no event.
std::atomic<bool> g_declined_ioctl{false};
constexpr uint32_t kIoctlDiskGetDriveGeometry = 0x70000;
constexpr uint32_t kIoctlDiskGetPartitionInfo = 0x74004;
constexpr uint32_t kStatusBufferTooSmall = 0xC0000023u;

uint32_t stack_argument(Runtime& r, PPCContext& ctx, int n, const char* fn) {
    const uint64_t address = uint64_t(ctx.r1.u32) + 0x54 + 8u * uint32_t(n - 8);
    uint32_t v = 0;
    if (address > UINT32_MAX || !r.mem->is_accessible(uint32_t(address), 4, Protect::Read) ||
        !guest_read_be32(uint32_t(address), &v))
        misuse(fn, ctx, "stack_argument_unreadable", uint32_t(address));
    return v;
}

void put_be64(uint8_t* p, uint64_t v) {
    put_be32(p, uint32_t(v >> 32));
    put_be32(p + 4, uint32_t(v));
}

void NtDeviceIoControlFile(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtDeviceIoControlFile";
    Runtime& r = rt_or_die(fn);
    const uint32_t handle = ctx.r3.u32, event_handle = ctx.r4.u32, apc = ctx.r5.u32, apc_context = ctx.r6.u32;
    const uint32_t piosb = ctx.r7.u32, code = ctx.r8.u32;
    std::shared_ptr<GuestFile> file;
    Status s = r.handles.lookup_as<GuestFile>(handle, &file);
    std::shared_ptr<HandleObject> event;
    if (s == Status::Ok && event_handle) s = reference_io_event(event_handle, &event);
    if (s != Status::Ok) return ret(ctx, to_ntstatus(s));
    const std::shared_ptr<GuestBlockDevice>& device = file->block_device();
    if (!device) {
        declined_once(g_declined_ioctl, fn, "no device behind a VFS file handle", code, handle, (uint32_t)ctx.lr);
        return ret(ctx, kStatusInvalidDeviceRequest);
    }
    uint32_t needed = 0;
    if (code == kIoctlDiskGetDriveGeometry) needed = 8;
    else if (code == kIoctlDiskGetPartitionInfo) needed = 16;
    else unimplemented(fn, ctx, "disk_ioctl_code", code);
    if (apc) {
        if (file_has_completion_port(*file)) return ret(ctx, nt::kInvalidParameter);
        file_apc_routine(fn, apc, uint32_t(ctx.lr));
    }
    const uint32_t out = stack_argument(r, ctx, 8, fn), out_length = stack_argument(r, ctx, 9, fn);
    if (piosb && !r.mem->is_accessible(piosb, 8, Protect::ReadWrite)) return ret(ctx, nt::kAccessViolation);
    if (out_length < needed) return ret(ctx, kStatusBufferTooSmall);
    if (!out || !r.mem->is_accessible(out, needed, Protect::ReadWrite)) return ret(ctx, nt::kAccessViolation);
    if (event) set_io_event(event, false);
    uint8_t answer[16] = {};
    if (code == kIoctlDiskGetDriveGeometry) {
        const uint64_t sectors = device->size() / device->bytes_per_sector();
        if (sectors > UINT32_MAX) unimplemented(fn, ctx, "geometry_sector_count_above_32_bits", code);
        put_be32(answer, uint32_t(sectors));
        put_be32(answer + 4, device->bytes_per_sector());
    } else {
        put_be64(answer, device->starting_offset());
        put_be64(answer + 8, device->size());
    }
    store_bytes(r, out, answer, needed);
    if (piosb) {
        guest_write_be32(piosb, nt::kSuccess);
        guest_write_be32(piosb + 4, needed);
    }
    if (event) set_io_event(event, true);
    post_file_completion(*file, apc_context, nt::kSuccess, needed);
    queue_file_apc(fn, apc, apc_context, piosb, nt::kSuccess, uint32_t(ctx.lr));
    ret(ctx, nt::kSuccess);
}

// ---- NtReadFileScatter (0x00F1) --------------------------------------------
// Xbox 360: NtReadFileScatter(HANDLE, HANDLE Event, PIO_APC_ROUTINE,
//   PVOID ApcContext, PIO_STATUS_BLOCK, PFILE_SEGMENT_ELEMENT SegmentArray,
//   ULONG Length, PLARGE_INTEGER ByteOffset): NtReadFile into a list of pages.
//   SegmentArray holds one 32-bit big-endian buffer address per 4 KiB page
//   (the element stride Xenia's working implementation uses); page i
//   receives file bytes [offset + 4096*i, +4096), the last page the rest.
// Completion rules are NtReadFile's: synchronous, optional event (reset after
//   every argument and handle is validated, signalled after the pages and the
//   IO_STATUS_BLOCK are written), ByteOffset NULL or
//   FILE_USE_FILE_POINTER_POSITION reads at the current position, which
//   advances by the bytes read. A start at or after EOF -> STATUS_END_OF_FILE
//   with Information 0; the transfer stops at EOF with STATUS_SUCCESS and the
//   bytes read. Every page is checked writable before anything is read.
// APC routine: queued to the calling thread after the transfer (io_event.h queue_file_apc).
void NtReadFileScatter(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtReadFileScatter";
    Runtime& r = rt_or_die(fn);
    const uint32_t handle = ctx.r3.u32, event_handle = ctx.r4.u32, apc = ctx.r5.u32, apc_context = ctx.r6.u32;
    const uint32_t piosb = ctx.r7.u32, segments = ctx.r8.u32, length = ctx.r9.u32, poffset = ctx.r10.u32;
    constexpr uint32_t kPage = 0x1000;
    if (apc) {
        std::shared_ptr<GuestFile> associated;
        if (r.handles.lookup_as<GuestFile>(handle, &associated) == Status::Ok && file_has_completion_port(*associated))
            return ret(ctx, nt::kInvalidParameter);
        file_apc_routine(fn, apc, uint32_t(ctx.lr));
    }
    const uint32_t pages = length / kPage + ((length % kPage) ? 1u : 0u);
    if ((piosb && !r.mem->is_accessible(piosb, 8, Protect::ReadWrite)) ||
        (poffset && !r.mem->is_accessible(poffset, 8, Protect::Read)) ||
        (pages && (!segments || !r.mem->is_accessible(segments, uint64_t(pages) * 4, Protect::Read))))
        return ret(ctx, nt::kAccessViolation);
    for (uint32_t i = 0; i < pages; ++i) {
        uint32_t page = 0;
        guest_read_be32(segments + 4 * i, &page);
        const uint32_t n = std::min(kPage, length - i * kPage);
        if (!page || !r.mem->is_accessible(page, n, Protect::ReadWrite)) return ret(ctx, nt::kAccessViolation);
    }
    uint64_t offset = kUseFilePointerPosition;
    if (poffset && !guest_read_be64(poffset, &offset)) return ret(ctx, nt::kAccessViolation);
    if (offset != kUseFilePointerPosition && (offset >> 63)) return ret(ctx, nt::kInvalidParameter);

    std::shared_ptr<GuestFile> file;
    Status s = r.handles.lookup_as<GuestFile>(handle, &file);
    std::shared_ptr<HandleObject> event;
    if (s == Status::Ok && event_handle) s = reference_io_event(event_handle, &event);
    uint32_t total = 0;
    if (s == Status::Ok) {
        if (event) set_io_event(event, false);
        if (!length) {
            uint32_t got = 0;
            s = offset == kUseFilePointerPosition ? file->read(nullptr, 0, &got)
                                                  : file->read_at(offset, nullptr, 0, &got);
        }
        for (uint32_t i = 0; i < pages; ++i) {
            uint32_t page = 0;
            guest_read_be32(segments + 4 * i, &page);
            const uint32_t n = std::min(kPage, length - i * kPage);
            uint32_t got = 0;
            Status part = offset == kUseFilePointerPosition
                              ? file->read(r.mem->host(page), n, &got)
                              : file->read_at(offset + total, r.mem->host(page), n, &got);
            note_title_write(page, n);
            if (part == Status::EndOfFile) {
                if (!total) s = Status::EndOfFile;
                break;
            }
            if (part != Status::Ok) { s = part; break; }
            total += got;
            if (got < n) break;
        }
    }
    const uint32_t st = to_ntstatus(s);
    if (piosb) {
        guest_write_be32(piosb, st);
        guest_write_be32(piosb + 4, s == Status::Ok ? total : 0);
    }
    if (event) set_io_event(event, true);
    if (file) {
        report_file_transfer_failure(fn, *file, offset, length, st, total, uint32_t(ctx.lr));
        post_file_completion(*file, apc_context, st, s == Status::Ok ? total : 0);
    }
    queue_file_apc(fn, apc, apc_context, piosb, st, uint32_t(ctx.lr));
    ret(ctx, st);
}

// ---- NtWriteFileGather (0x0100) ---------------------------------------------
// Xbox 360: NtWriteFileGather(HANDLE, HANDLE Event, PIO_APC_ROUTINE,
//   PVOID ApcContext, PIO_STATUS_BLOCK, PFILE_SEGMENT_ELEMENT SegmentArray,
//   ULONG Length, PLARGE_INTEGER ByteOffset): NtWriteFile from a list of pages,
//   the mirror of NtReadFileScatter (same segment format: one 32-bit
//   big-endian page address per 4 KiB; page i supplies file bytes
//   [offset + 4096*i, +4096), the last page the rest).
// Completion rules are NtWriteFile's (src/hle_xboxkrnl_io.cpp): synchronous,
//   optional event (reset after every argument and handle is validated,
//   signalled after the IO_STATUS_BLOCK is written), ByteOffset NULL or
//   FILE_USE_FILE_POINTER_POSITION writes at the current position (which
//   advances), FILE_WRITE_TO_END_OF_FILE appends; a handle opened without write
//   access -> STATUS_ACCESS_DENIED. Every page is checked readable first.
// APC routine: queued to the calling thread after the transfer (io_event.h queue_file_apc).
void NtWriteFileGather(PPCContext& ctx, uint8_t*) {
    const char* fn = "NtWriteFileGather";
    Runtime& r = rt_or_die(fn);
    const uint32_t handle = ctx.r3.u32, event_handle = ctx.r4.u32, apc = ctx.r5.u32, apc_context = ctx.r6.u32;
    const uint32_t piosb = ctx.r7.u32, segments = ctx.r8.u32, length = ctx.r9.u32, poffset = ctx.r10.u32;
    constexpr uint32_t kPage = 0x1000;
    constexpr uint64_t kWriteToEndOfFile = 0xFFFFFFFFFFFFFFFFull;
    if (apc) {
        std::shared_ptr<GuestFile> associated;
        if (r.handles.lookup_as<GuestFile>(handle, &associated) == Status::Ok && file_has_completion_port(*associated))
            return ret(ctx, nt::kInvalidParameter);
        file_apc_routine(fn, apc, uint32_t(ctx.lr));
    }
    const uint32_t pages = length / kPage + ((length % kPage) ? 1u : 0u);
    if ((piosb && !r.mem->is_accessible(piosb, 8, Protect::ReadWrite)) ||
        (poffset && !r.mem->is_accessible(poffset, 8, Protect::Read)) ||
        (pages && (!segments || !r.mem->is_accessible(segments, uint64_t(pages) * 4, Protect::Read))))
        return ret(ctx, nt::kAccessViolation);
    for (uint32_t i = 0; i < pages; ++i) {
        uint32_t page = 0;
        guest_read_be32(segments + 4 * i, &page);
        const uint32_t n = std::min(kPage, length - i * kPage);
        if (!page || !r.mem->is_accessible(page, n, Protect::Read)) return ret(ctx, nt::kAccessViolation);
    }
    uint64_t offset = kUseFilePointerPosition;
    if (poffset && !guest_read_be64(poffset, &offset)) return ret(ctx, nt::kAccessViolation);
    if (offset != kUseFilePointerPosition && offset != kWriteToEndOfFile && (offset >> 63))
        return ret(ctx, nt::kInvalidParameter);

    std::shared_ptr<GuestFile> file;
    Status s = r.handles.lookup_as<GuestFile>(handle, &file);
    if (s == Status::Ok && !file->writable()) s = Status::AccessDenied;
    std::shared_ptr<HandleObject> event;
    if (s == Status::Ok && event_handle) s = reference_io_event(event_handle, &event);
    uint32_t total = 0;
    if (s == Status::Ok) {
        if (event) set_io_event(event, false);
        if (!length) {
            uint32_t written = 0;
            s = offset == kUseFilePointerPosition ? file->write(nullptr, 0, &written)
                : offset == kWriteToEndOfFile    ? file->write_to_end(nullptr, 0, &written)
                                                 : file->write_at(offset, nullptr, 0, &written);
        }
        for (uint32_t i = 0; i < pages; ++i) {
            uint32_t page = 0;
            guest_read_be32(segments + 4 * i, &page);
            const uint32_t n = std::min(kPage, length - i * kPage);
            const void* src = r.mem->host(page);
            uint32_t written = 0;
            // Appending: the first page goes to the end, the rest follow it.
            Status part = offset == kUseFilePointerPosition ? file->write(src, n, &written)
                          : offset == kWriteToEndOfFile    ? (i == 0 ? file->write_to_end(src, n, &written)
                                                                     : file->write(src, n, &written))
                                                           : file->write_at(offset + total, src, n, &written);
            if (part != Status::Ok) { s = part; break; }
            total += written;
            if (written < n) break;
        }
    }
    const uint32_t st = to_ntstatus(s);
    if (piosb) {
        guest_write_be32(piosb, st);
        guest_write_be32(piosb + 4, s == Status::Ok ? total : 0);
    }
    if (event) set_io_event(event, true);
    if (file) {
        report_file_transfer_failure(fn, *file, offset, length, st, total, uint32_t(ctx.lr));
        post_file_completion(*file, apc_context, st, s == Status::Ok ? total : 0);
    }
    queue_file_apc(fn, apc, apc_context, piosb, st, uint32_t(ctx.lr));
    ret(ctx, st);
}

// ---- ObCreateSymbolicLink (0x0103) / ObDeleteSymbolicLink (0x0104) ---------
// Xbox 360: NTSTATUS ObCreateSymbolicLink(PANSI_STRING LinkName,
//   PANSI_STRING TargetName); NTSTATUS ObDeleteSymbolicLink(PANSI_STRING)
//   (title: "\??\<drive>:" links built with RtlInitAnsiString; it deletes
//   only the links it recorded as created).
// Implemented: a real link table in R-comp's object namespace (\, \?? with
//   its \DosDevices alias, \Device; case-insensitive). Creating an existing
//   name - another link, a device, or a drive the runtime mounted ("\??\game:")
//   - is STATUS_OBJECT_NAME_COLLISION; as on the console the target is not
//   checked when the link is made. Deleting an unknown link is
//   STATUS_OBJECT_NAME_NOT_FOUND, a device name STATUS_OBJECT_TYPE_MISMATCH;
//   deleting a runtime-mounted drive traps (the VFS mount is configuration).
// Path resolution: find_title_drive_link() / rewrite_path_through_title_links()
//   expose the table; registration installs rewrite_path_through_title_links() as the VFS
//   drive-link resolver, so a path on a linked drive opens through the link target.
void ObCreateSymbolicLink(PPCContext& ctx, uint8_t*) {
    const char* fn = "ObCreateSymbolicLink";
    Runtime& r = rt_or_die(fn);
    std::string link, target, canonical;
    Status s = read_ansi(r, ctx.r3.u32, &link);
    if (s == Status::GuestFault) return ret(ctx, nt::kAccessViolation);
    if (s != Status::Ok) return ret(ctx, nt::kObjectNameInvalid);
    s = read_ansi(r, ctx.r4.u32, &target);
    if (s == Status::GuestFault) return ret(ctx, nt::kAccessViolation);
    if (s != Status::Ok || target.empty()) return ret(ctx, nt::kInvalidParameter);
    const uint32_t st = canonical_object_name(link, &canonical);
    if (st != nt::kSuccess) return ret(ctx, st);
    const bool mounted = names_configured_mount(r, canonical);
    std::lock_guard<std::mutex> lock(objects().mu);
    KernelObjects& state = sync_locked(r);
    if (name_in_use_locked(state, canonical, mounted)) return ret(ctx, nt::kObjectNameCollision);
    state.links.emplace(canonical, target);
    ret(ctx, nt::kSuccess);
}

void ObDeleteSymbolicLink(PPCContext& ctx, uint8_t*) {
    const char* fn = "ObDeleteSymbolicLink";
    Runtime& r = rt_or_die(fn);
    std::string link, canonical;
    const Status s = read_ansi(r, ctx.r3.u32, &link);
    if (s == Status::GuestFault) return ret(ctx, nt::kAccessViolation);
    if (s != Status::Ok) return ret(ctx, nt::kObjectNameInvalid);
    const uint32_t st = canonical_object_name(link, &canonical);
    if (st != nt::kSuccess) return ret(ctx, st);
    const bool configured = names_configured_mount(r, canonical);
    bool mounted = false;
    uint32_t result = nt::kObjectNameNotFound;
    {
        std::lock_guard<std::mutex> lock(objects().mu);
        KernelObjects& state = sync_locked(r);
        if (state.links.erase(canonical)) {
            result = nt::kSuccess;
        } else if (configured) {
            mounted = true;
        } else {
            for (const auto& d : state.devices)
                if (!d.second.deleted && d.second.name == canonical) result = nt::kObjectTypeMismatch;
        }
    }
    if (mounted) unimplemented(fn, ctx, "delete_runtime_mounted_drive", ctx.r3.u32);
    ret(ctx, result);
}

// ---- ObIsTitleObject (0x0109) ----------------------------------------------
// Xbox 360: BOOLEAN ObIsTitleObject(PVOID Object): TRUE when the object
//   belongs to the title rather than the system (title: the result picks the
//   pool for a driver allocation). Every object R-comp creates is created on
//   behalf of the title: a thread Body (ExCreateThread, the entry thread) and
//   a device from IoCreateDevice answer TRUE. Any other pointer is an object
//   R-comp did not create (for example a FILE_OBJECT of an IRP, which R-comp
//   never builds): trap.
void ObIsTitleObject(PPCContext& ctx, uint8_t*) {
    const char* fn = "ObIsTitleObject";
    Runtime& r = rt_or_die(fn);
    const uint32_t body = ctx.r3.u32;
    bool device = false;
    {
        std::lock_guard<std::mutex> lock(objects().mu);
        KernelObjects& s = sync_locked(r);
        device = s.devices.count(body) != 0;
    }
    std::shared_ptr<ThreadObjectIdentity> thread;
    if (device || find_thread_object(body, &thread) == Status::Ok) return ret(ctx, 1);
    unimplemented(fn, ctx, "object_not_created_by_rcomp", body);
}

// ---- StfsCreateDevice (0x0259) / StfsControlDevice (0x025A) ----------------
// Xbox 360: the kernel's STFS (content package) file system: create a device
//   for a package, then send it control requests. R-comp has no STFS
//   implementation and mounts no package, so both answer
//   STATUS_NOT_SUPPORTED without touching guest memory (no output is
//   produced, so none is written). GTA IV / TBoGT have no direct call site.
std::atomic<bool> g_declined_stfs_create{false}, g_declined_stfs_control{false};

void StfsCreateDevice(PPCContext& ctx, uint8_t*) {
    declined_once(g_declined_stfs_create, "StfsCreateDevice", "R-comp has no STFS package support", ctx.r3.u32,
                  ctx.r4.u32, (uint32_t)ctx.lr);
    ret(ctx, kStatusNotSupported);
}

void StfsControlDevice(PPCContext& ctx, uint8_t*) {
    declined_once(g_declined_stfs_control, "StfsControlDevice", "R-comp has no STFS package support",
                  ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr);
    ret(ctx, kStatusNotSupported);
}

// ---- XeKeysConsolePrivateKeySign (0x0256) /
//      XeKeysConsoleSignatureVerification (0x0257) -------------------------
// Xbox 360: BOOL XeKeysConsolePrivateKeySign(const BYTE Hash[20],
//   XE_CONSOLE_SIGNATURE*); BOOL XeKeysConsoleSignatureVerification(
//   const BYTE Hash[20], const XE_CONSOLE_SIGNATURE*, LONG* Result) (title:
//   verification succeeds only when both the return value and *Result are
//   nonzero; the signature sits at +4 of a saved block whose SHA-1 is the
//   hash). XE_CONSOLE_SIGNATURE is 0x228 bytes (Free60 table; (title) the
//   0x228-byte gap between the signature and the hashed data): certificate
//   {u16 size 0x1A8, ConsoleId[5], PartNumber[11], Reserved[4],
//   u16 Privileges, u32 ConsoleType, Date[8], PublicExponent[4],
//   Modulus[0x80], CertificateSignature[0x100]} then Signature[0x80].
// There is no console key. R-comp signs with a fixed, public local key
//   (integrity between R-comp runs, no security): the certificate carries
//   size 0x1A8, a console id derived from the key, part number "RCOMP-LOCAL",
//   type 0 and zero date, exponent, modulus and certificate signature (no
//   RSA key or Microsoft signature exists); Signature = the first 0x80 bytes
//   of HMAC-SHA-1(key, i || certificate || hash) for i = 1..7.
//   Verification recomputes it over the given certificate and hash and
//   compares all 0x80 bytes: *Result = 1 when they match, 0 otherwise. The
//   return value is TRUE only for an R-comp certificate (size, id and part
//   number), so a real console's signature - which R-comp cannot verify -
//   returns FALSE with *Result 0. Unreadable/unwritable pointers trap.
constexpr uint32_t kCertificateSize = 0x1A8;
constexpr uint32_t kSignatureSize = 0x228;
constexpr char kPartNumber[11] = {'R', 'C', 'O', 'M', 'P', '-', 'L', 'O', 'C', 'A', 'L'};
const uint8_t kLocalKey[] = "R-comp local console signing key v1 (public; integrity only, not security)";

void console_id(uint8_t out[5]) {
    static const char label[] = "console-id";
    const Part parts[] = {{label, sizeof(label) - 1}};
    uint8_t mac[20];
    hmac_sha1(kLocalKey, sizeof(kLocalKey) - 1, parts, 1, mac);
    memcpy(out, mac, 5);
}

void local_certificate(uint8_t cert[kCertificateSize]) {
    memset(cert, 0, kCertificateSize);
    put_be16(cert, uint16_t(kCertificateSize));
    console_id(cert + 0x02);
    memcpy(cert + 0x07, kPartNumber, sizeof(kPartNumber));
}

void local_signature(const uint8_t* cert, const uint8_t* hash, uint8_t out[0x80]) {
    for (uint8_t i = 1; i <= 7; ++i) {
        const Part parts[] = {{&i, 1}, {cert, kCertificateSize}, {hash, 20}};
        uint8_t mac[20];
        hmac_sha1(kLocalKey, sizeof(kLocalKey) - 1, parts, 3, mac);
        const uint32_t at = uint32_t(i - 1) * 20;
        memcpy(out + at, mac, std::min<uint32_t>(20, 0x80 - at));
    }
}

void XeKeysConsolePrivateKeySign(PPCContext& ctx, uint8_t*) {
    const char* fn = "XeKeysConsolePrivateKeySign";
    Runtime& r = rt_or_die(fn);
    const uint32_t hash = ctx.r3.u32, signature = ctx.r4.u32;
    if (!hash || !r.mem->is_accessible(hash, 20, Protect::Read)) misuse(fn, ctx, "hash_unreadable", hash);
    if (!signature || !r.mem->is_accessible(signature, kSignatureSize, Protect::ReadWrite))
        misuse(fn, ctx, "signature_unwritable", signature);
    uint8_t block[kSignatureSize];
    local_certificate(block);
    uint8_t digest[20];
    memcpy(digest, r.mem->host(hash), 20);
    local_signature(block, digest, block + kCertificateSize);
    store_bytes(r, signature, block, kSignatureSize);
    ret(ctx, 1);
}

void XeKeysConsoleSignatureVerification(PPCContext& ctx, uint8_t*) {
    const char* fn = "XeKeysConsoleSignatureVerification";
    Runtime& r = rt_or_die(fn);
    const uint32_t hash = ctx.r3.u32, signature = ctx.r4.u32, result = ctx.r5.u32;
    if (!hash || !r.mem->is_accessible(hash, 20, Protect::Read)) misuse(fn, ctx, "hash_unreadable", hash);
    if (!signature || !r.mem->is_accessible(signature, kSignatureSize, Protect::Read))
        misuse(fn, ctx, "signature_unreadable", signature);
    if (result && ((result & 3) || !r.mem->is_accessible(result, 4, Protect::ReadWrite)))
        misuse(fn, ctx, "result_unwritable", result);
    uint8_t block[kSignatureSize], digest[20];
    memcpy(block, r.mem->host(signature), kSignatureSize);
    memcpy(digest, r.mem->host(hash), 20);
    uint8_t expected_cert[kCertificateSize];
    local_certificate(expected_cert);
    const bool ours = memcmp(block, expected_cert, 0x12) == 0;  // size, console id, part number
    uint8_t expected[0x80];
    local_signature(block, digest, expected);
    uint8_t diff = 0;
    for (uint32_t i = 0; i < 0x80; ++i) diff = uint8_t(diff | (expected[i] ^ block[kCertificateSize + i]));
    const bool valid = ours && diff == 0;
    if (result) guest_write_be32(result, valid ? 1u : 0u);
    ret(ctx, ours ? 1u : 0u);
}

struct Impl { uint32_t ordinal; const char* name; PPCFunc* function; };
constexpr Impl kImpls[] = {
    {0x0034, "IoCheckShareAccess", &IoCheckShareAccess},
    {0x0037, "IoCreateDevice", &IoCreateDevice},
    {0x0039, "IoDeleteDevice", &IoDeleteDevice},
    {0x003B, "IoDismountVolume", &IoDismountVolume},
    {0x003C, "IoDismountVolumeByFileHandle", &IoDismountVolumeByFileHandle},
    {0x0045, "IoRemoveShareAccess", &IoRemoveShareAccess},
    {0x0047, "IoSetShareAccess", &IoSetShareAccess},
    {0x006B, "KeLockL2", &KeLockL2},
    {0x006C, "KeUnlockL2", &KeUnlockL2},
    {0x00D9, "NtDeviceIoControlFile", &NtDeviceIoControlFile},
    {0x00F1, "NtReadFileScatter", &NtReadFileScatter},
    {0x0100, "NtWriteFileGather", &NtWriteFileGather},
    {0x0103, "ObCreateSymbolicLink", &ObCreateSymbolicLink},
    {0x0104, "ObDeleteSymbolicLink", &ObDeleteSymbolicLink},
    {0x0109, "ObIsTitleObject", &ObIsTitleObject},
    {0x0256, "XeKeysConsolePrivateKeySign", &XeKeysConsolePrivateKeySign},
    {0x0257, "XeKeysConsoleSignatureVerification", &XeKeysConsoleSignatureVerification},
    {0x0259, "StfsCreateDevice", &StfsCreateDevice},
    {0x025A, "StfsControlDevice", &StfsControlDevice},
};

}  // namespace

Status reference_device_object(uint32_t body) {
    Runtime* r = runtime();
    if (!r) return Status::NotInitialized;
    std::lock_guard<std::mutex> lock(objects().mu);
    KernelObjects& s = sync_locked(*r);
    auto it = s.devices.find(body);
    if (it == s.devices.end()) return Status::NotFound;
    if (it->second.references == UINT32_MAX) return Status::Conflict;
    ++it->second.references;
    return Status::Ok;
}

Status dereference_device_object(uint32_t body) {
    Runtime* r = runtime();
    if (!r) return Status::NotInitialized;
    bool free_now = false;
    {
        std::lock_guard<std::mutex> lock(objects().mu);
        KernelObjects& s = sync_locked(*r);
        auto it = s.devices.find(body);
        if (it == s.devices.end()) return Status::NotFound;
        // The creation reference belongs to IoDeleteDevice, not to ObDereferenceObject.
        if (it->second.references <= (it->second.deleted ? 0u : 1u)) return Status::Conflict;
        if (--it->second.references == 0) { s.devices.erase(it); free_now = true; }
    }
    if (free_now && r->heap.free(body) != Status::Ok) return Status::Conflict;
    return Status::Ok;
}

Status find_title_drive_link(std::string_view drive, std::string* target) {
    Runtime* r = runtime();
    if (!r || !target) return Status::NotInitialized;
    std::string name = ascii_lower(drive);
    if (name.compare(0, 4, "\\??\\") == 0) name = name.substr(4);
    if (name.empty()) return Status::NotFound;
    if (name.back() != ':') name += ':';
    std::lock_guard<std::mutex> lock(objects().mu);
    KernelObjects& s = sync_locked(*r);
    auto it = s.links.find("\\??\\" + name);
    if (it == s.links.end()) return Status::NotFound;
    *target = it->second;
    return Status::Ok;
}

Status rewrite_path_through_title_links(std::string_view guest_path, std::string* rewritten) {
    if (!rewritten) return Status::InvalidArgument;
    std::string_view path = guest_path;
    if (path.size() >= 4 && path.compare(0, 4, "\\??\\") == 0) path.remove_prefix(4);
    const size_t colon = path.find(':');
    if (colon == std::string_view::npos || colon == 0 || path.substr(0, colon).find_first_of("\\/") != std::string_view::npos)
        return Status::PathRejected;
    std::string target;
    const Status s = find_title_drive_link(path.substr(0, colon + 1), &target);
    if (s != Status::Ok) return s;
    std::string_view rest = path.substr(colon + 1);
    if (!target.empty() && (target.back() == '\\' || target.back() == '/') && !rest.empty() &&
        (rest.front() == '\\' || rest.front() == '/'))
        rest.remove_prefix(1);
    *rewritten = target + std::string(rest);
    return Status::Ok;
}

Status register_xboxkrnl_device_hle() {
    vfs_set_drive_link_resolver(&rewrite_path_through_title_links);
    for (const auto& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            return Status::InvalidArgument;
        const Status s = register_import(kModuleXboxkrnl, impl.ordinal, impl.function, impl.name);
        if (s != Status::Ok) return s;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
