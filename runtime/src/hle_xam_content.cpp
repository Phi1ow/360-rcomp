// Content packages on the hard drive: XamContentCreateEx (0x259), XamContentClose (0x25A),
// XamContentGetDeviceData (0x25E), XamContentSetThumbnail (0x260), XamContentGetCreator (0x262) and
// XamShowDeviceSelectorUI (0x2CB). See rcomp/runtime/xam_content.h for the storage layout.
//
// ABI (public XDK names; argument order and structure layouts checked against Xenia's xam_content.cc,
// xam_content_device.cc, xam_ui.cc and content_manager.h):
//   XCONTENT_DATA (0x134): DeviceID +0, ContentType +4, DisplayName WCHAR[128] +8, FileName CHAR[42] +0x108.
//   XDEVICE_DATA (0x50): DeviceID +0, DeviceType +4, ullTotalBytes +8, ullFreeBytes +0x10, wszName[28] +0x18.
//   Results are Win32 codes; with an XOVERLAPPED the call completes at once (xam_complete_overlapped) and
//   returns ERROR_IO_PENDING, as the other XAM services here do.
// Nothing is pretended: a package is a real directory mounted read-write under the title's root name, the
// device sizes come from statvfs()/fstatvfs() of the save root, and without a configured save root no storage device
// exists (ERROR_DEVICE_NOT_CONNECTED). The device selector cannot be shown on the PS5; by owner decision a
// dialog completes with the default the console would offer, here the only device, and is logged.
#include "rcomp/runtime/xam_content.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/vfs.h"
#include "rcomp/runtime/xam.h"
#include "rcomp/runtime/xam_enum.h"
#include "rcomp/runtime/xam_profile.h"
#include "xam_storage.h"

namespace rcomp::rt {
namespace {
using namespace storage;
constexpr uint32_t kErrorSuccess = 0, kErrorFileNotFound = 2, kErrorPathNotFound = 3, kErrorAccessDenied = 5,
                   kErrorDiskFull = 0x70, kErrorInvalidParameter = 0x57, kErrorAlreadyExists = 0xB7,
                   kErrorDeviceNotConnected = 0x48F, kErrorCancelled = 0x4C7, kErrorNoSuchUser = 0x525,
                   kErrorIoPending = 0x3E5, kErrorFunctionFailed = 0x65B, kErrorSharingViolation = 0x20;
constexpr uint32_t kOverlappedBytes = 0x1C, kDeviceDataBytes = 0x50, kMaxThumbnailBytes = 0x4000;
constexpr uint32_t kUserInternal = 0xFE, kUserAny = 0xFF;
constexpr uint32_t kDeviceTypeHdd = 1;
constexpr uint32_t kFileNameBytes = 42, kDisplayNameBytes = 256;
// Disposition (low nibble of the flags) and the value written back.
// 4 = OPEN_ALWAYS needs no test of its own: it opens an existing package and creates a missing one.
constexpr uint32_t kCreateNew = 1, kCreateAlways = 2, kOpenExisting = 3, kTruncateExisting = 5;
constexpr uint32_t kDispositionCreated = 1, kDispositionOpened = 2;
constexpr char kMetaMagic[4] = {'R', 'C', 'X', 'C'};
constexpr uint32_t kMetaVersion = 1, kMetaBytes = 4 + 4 + 4 + 8 + kDisplayNameBytes;

std::mutex g_mutex;
std::string g_root;                              // host save root, no trailing '/'
std::map<std::string, std::string> g_open;       // lower-case root name -> package host directory

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s before runtime_init", fn);
    return *r;
}

bool readable(Runtime& r, uint32_t address, uint32_t size) {
    return address && r.mem->is_accessible(address, size, Protect::Read);
}
bool writable(Runtime& r, uint32_t address, uint32_t size) {
    return address && r.mem->is_accessible(address, size, Protect::ReadWrite);
}
void copy_from_guest(Runtime& r, uint32_t address, void* dst, uint32_t size) {
    const uint8_t* p = r.mem->translate(address, size);
    if (!p) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex! validated guest range 0x%08X became unreadable", address);
    memcpy(dst, p, size);
}
void copy_to_guest(Runtime& r, uint32_t address, const void* src, uint32_t size) {
    uint8_t* p = r.mem->translate(address, size);
    if (!p) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex! validated guest range 0x%08X became unwritable", address);
    memcpy(p, src, size);
}
std::string lower(std::string s) {
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}

// A guest ANSI string of at most `max` bytes (NUL-terminated before that), or false.
bool read_ansi(Runtime& r, uint32_t address, uint32_t max, std::string* out) {
    out->clear();
    for (uint32_t i = 0; i < max; ++i) {
        if (!readable(r, address + i, 1)) return false;
        const char c = char(*r.mem->translate(address + i, 1));
        if (!c) return true;
        out->push_back(c);
    }
    return false;
}

// Root names are device names: letters, digits, '_' and '-' (the VFS device alphabet), 1..32 bytes.
bool valid_root_name(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s)
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
    return true;
}

// Package file names become one host directory name: printable ASCII without separators, wildcards or
// drive colons, not "." / "..", no leading or trailing blank and no trailing dot.
bool valid_file_name(const std::string& s) {
    if (s.empty() || s.size() >= kFileNameBytes || s == "." || s == "..") return false;
    if (s.front() == ' ' || s.back() == ' ' || s.back() == '.') return false;
    for (char c : s) {
        if (c < 0x20 || c > 0x7E) return false;
        if (strchr("/\\:*?\"<>|", c)) return false;
    }
    return true;
}

struct ContentData {
    uint32_t device_id = 0, content_type = 0;
    uint8_t display_name[kDisplayNameBytes] = {};
    std::string file_name;
};

// Reads and validates a guest XCONTENT_DATA. Returns a Win32 code.
uint32_t read_content_data(Runtime& r, uint32_t address, ContentData* out) {
    if (!readable(r, address, kXContentDataBytes)) return kErrorInvalidParameter;
    uint8_t raw[kXContentDataBytes];
    copy_from_guest(r, address, raw, sizeof raw);
    out->device_id = be32(raw);
    out->content_type = be32(raw + 4);
    memcpy(out->display_name, raw + 8, kDisplayNameBytes);
    const char* name = reinterpret_cast<const char*>(raw + 0x108);
    const size_t len = strnlen(name, kFileNameBytes);
    if (len == kFileNameBytes) return kErrorInvalidParameter;
    out->file_name.assign(name, len);
    if (!valid_file_name(out->file_name) || out->content_type == 0) return kErrorInvalidParameter;
    if (out->device_id != kContentDeviceHdd) return kErrorDeviceNotConnected;
    return kErrorSuccess;
}

// Owner of a package: saved games belong to the local profile, other content is shared by the console.
uint32_t owner_xuid(uint32_t user_index, uint32_t content_type, uint64_t* xuid) {
    if (content_type == kContentTypeSavedGame) {
        if (!is_local_user(user_index) && user_index != kUserAny) return kErrorNoSuchUser;
        *xuid = kLocalUserXuid;
        return kErrorSuccess;
    }
    if (user_index >= 4 && user_index != kUserInternal && user_index != kUserAny) return kErrorInvalidParameter;
    *xuid = 0;
    return kErrorSuccess;
}

// Removes everything inside `dir` (not `dir` itself). Symbolic links are removed, never followed.
// The PS5 sandbox refuses lstat() (EPERM, see vfs.h): the entry type comes from readdir(), else from stat().
// A symbolic link cannot exist in a package (the title has no way to create one), so stat() is enough there.
bool clear_dir(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool ok = true;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        const std::string path = dir + "/" + e->d_name;
        bool is_dir = false;
#if defined(DT_DIR) && defined(DT_UNKNOWN)
        if (e->d_type != DT_UNKNOWN) {
            is_dir = e->d_type == DT_DIR;
        } else
#endif
        {
            struct stat st;
            if (stat(path.c_str(), &st) != 0) { ok = false; continue; }
            is_dir = S_ISDIR(st.st_mode);
        }
        if (is_dir) {
            ok = clear_dir(path) && ok;
            if (rmdir(path.c_str()) != 0) ok = false;
        } else if (unlink(path.c_str()) != 0) {
            ok = false;
        }
    }
    closedir(d);
    return ok;
}

// Total and free bytes of the file system holding `dir`. A sandboxed PS5 title cannot measure them: the libc
// answers ENOSYS for statvfs() and fstatvfs(), and statfs() is exported only to system processes (calling it
// from a title jumps to address 0; both measured on the console, 3 Oct 2026). There the value comes from
// `dir`/.rcomp-capacity, which R-comp Installer's chmod payload (un-sandboxed) measures with statfs(2) at
// every install: "total=<bytes>\nfree=<bytes>\nmeasured=<unix time>\n".
struct Capacity {
    uint64_t total = 0, free = 0;
};
bool dir_capacity(const std::string& dir, Capacity* out) {
    struct statvfs vfs;
    if (statvfs(dir.c_str(), &vfs) == 0) {
        out->total = uint64_t(vfs.f_blocks) * vfs.f_frsize;
        out->free = uint64_t(vfs.f_bavail) * vfs.f_frsize;
        return true;
    }
    const int vfs_errno = errno;
    std::vector<uint8_t> text;
    unsigned long long total = 0, free_bytes = 0;
    long long measured = 0;
    if (read_file(dir + "/.rcomp-capacity", &text, 256)) {
        text.push_back(0);
        if (sscanf(reinterpret_cast<const char*>(text.data()), "total=%llu free=%llu measured=%lld", &total,
                   &free_bytes, &measured) == 3 && total && free_bytes <= total) {
            out->total = total;
            out->free = free_bytes;
            static bool logged = false;
            if (!logged) {
                logged = true;
                fprintf(stderr, "RCOMP-XAM-CONTENT capacity of %s from the install-time measurement (%lld): "
                        "total=%llu free=%llu\n", dir.c_str(), measured, total, free_bytes);
            }
            return true;
        }
    }
    static bool reported = false;
    if (!reported) {
        reported = true;
        fprintf(stderr, "RCOMP-XAM-CONTENT capacity of %s unavailable: statvfs %s and no install-time measurement\n",
                dir.c_str(), strerror(vfs_errno));
    }
    return false;
}

std::string type_dir(uint64_t xuid, uint32_t title_id, uint32_t content_type) {
    return g_root + "/" + hex(xuid, 16) + "/" + hex(title_id, 8) + "/" + hex(content_type, 8);
}

// The existing package directory whose name matches case-insensitively (the Xbox file systems ignore
// case), or "" when none exists. Caller holds g_mutex.
std::string find_package(const std::string& dir, const std::string& file_name) {
    DIR* d = opendir(dir.c_str());
    if (!d) return {};
    std::string found;
    const std::string want = lower(file_name);
    while (dirent* e = readdir(d)) {
        if (lower(e->d_name) != want) continue;
        const std::string path = dir + "/" + e->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            found = e->d_name;
            break;
        }
    }
    closedir(d);
    return found;
}

bool write_metadata(const std::string& path, uint32_t content_type, uint64_t creator, const uint8_t* display_name) {
    uint8_t meta[kMetaBytes] = {};
    memcpy(meta, kMetaMagic, 4);
    put_be32(meta + 4, kMetaVersion);
    put_be32(meta + 8, content_type);
    put_be64(meta + 12, creator);
    memcpy(meta + 20, display_name, kDisplayNameBytes);
    return write_file_atomic(path, meta, sizeof meta);
}

bool read_metadata(const std::string& path, uint64_t* creator, uint8_t* display_name) {
    std::vector<uint8_t> meta;
    if (!read_file(path, &meta, kMetaBytes) || meta.size() != kMetaBytes || memcmp(meta.data(), kMetaMagic, 4) ||
        be32(meta.data() + 4) != kMetaVersion)
        return false;
    *creator = be64(meta.data() + 12);
    memcpy(display_name, meta.data() + 20, kDisplayNameBytes);
    return true;
}

uint32_t finish(PPCContext& ctx, uint32_t overlapped, uint32_t result) {
    if (overlapped) {
        xam_complete_overlapped(overlapped, result, 0);
        ctx.r3.u64 = kErrorIoPending;
    } else {
        ctx.r3.u64 = result;
    }
    return result;
}

// Validates an optional overlapped up front: an unwritable one is a caller error reported synchronously.
bool overlapped_ok(Runtime& r, uint32_t overlapped) {
    return !overlapped || writable(r, overlapped, kOverlappedBytes);
}

uint32_t stack_arg(Runtime& r, PPCContext& ctx, int n, const char* fn) {
    // Arguments past r10 live at r1 + 0x54 + 8 * (n - 8), low word of each 8-byte slot.
    const uint64_t address = uint64_t(ctx.r1.u32) + 0x54 + 8u * uint32_t(n - 8);
    uint32_t v = 0;
    if (address > UINT32_MAX || !r.mem->is_accessible(uint32_t(address), 4, Protect::Read) ||
        !guest_read_be32(uint32_t(address), &v))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xam.xex!%s: stack argument %d at 0x%08X not readable", fn, n,
                    uint32_t(address));
    return v;
}

// Locates a package for a guest XCONTENT_DATA: *dir is its type directory, *name the existing directory
// name ("" when absent). Returns a Win32 code.
uint32_t locate(Runtime& r, uint32_t user_index, uint32_t content_data, const char* fn, ContentData* data,
                uint64_t* xuid, std::string* dir, std::string* name) {
    uint32_t result = read_content_data(r, content_data, data);
    if (result != kErrorSuccess) return result;
    result = owner_xuid(user_index, data->content_type, xuid);
    if (result != kErrorSuccess) return result;
    if (g_root.empty()) return kErrorDeviceNotConnected;
    *dir = type_dir(*xuid, xam_main_title_id(fn), data->content_type);
    *name = find_package(*dir, data->file_name);
    return kErrorSuccess;
}

// XamContentCreateEx(user_index, root_name, content_data, flags, disposition*, license_mask*, cache_size,
// content_size (64-bit, r10), overlapped (stack)).
void XamContentCreateEx(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamContentCreateEx");
    const uint32_t user_index = ctx.r3.u32, root_address = ctx.r4.u32, content_data = ctx.r5.u32,
                   flags = ctx.r6.u32, disposition_out = ctx.r7.u32, license_out = ctx.r8.u32;
    const uint64_t content_size = ctx.r10.u64;
    const uint32_t overlapped = stack_arg(r, ctx, 8, "XamContentCreateEx");
    if (!overlapped_ok(r, overlapped) || (disposition_out && !writable(r, disposition_out, 4)) ||
        (license_out && !writable(r, license_out, 4))) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    std::string root;
    if (!read_ansi(r, root_address, 33, &root) || !valid_root_name(root))
        return void(finish(ctx, overlapped, kErrorInvalidParameter));
    const uint32_t disposition = flags & 0xF;
    if (disposition < kCreateNew || disposition > kTruncateExisting)
        return void(finish(ctx, overlapped, kErrorInvalidParameter));

    std::lock_guard<std::mutex> lock(g_mutex);
    ContentData data;
    uint64_t xuid = 0;
    std::string dir, name;
    uint32_t result = locate(r, user_index, content_data, "XamContentCreateEx", &data, &xuid, &dir, &name);
    if (result != kErrorSuccess) return void(finish(ctx, overlapped, result));
    if (g_open.count(lower(root))) return void(finish(ctx, overlapped, kErrorAlreadyExists));
    const bool exists = !name.empty();
    if (disposition == kCreateNew && exists) return void(finish(ctx, overlapped, kErrorAlreadyExists));
    if ((disposition == kOpenExisting || disposition == kTruncateExisting) && !exists)
        return void(finish(ctx, overlapped, kErrorPathNotFound));
    const bool create = !exists;
    const bool reset = exists && (disposition == kCreateAlways || disposition == kTruncateExisting);
    if (create || reset) {
        Capacity capacity;
        if (content_size && dir_capacity(g_root, &capacity) && capacity.free < content_size)
            return void(finish(ctx, overlapped, kErrorDiskFull));
    }
    if (create) name = data.file_name;
    const std::string package = dir + "/" + name;
    if (create && !mkdir_p(package)) return void(finish(ctx, overlapped, kErrorAccessDenied));
    if (reset && !clear_dir(package)) return void(finish(ctx, overlapped, kErrorAccessDenied));
    if (create || reset) {
        if (!write_metadata(package + ".xcontent", data.content_type, xuid,
                            data.display_name))
            return void(finish(ctx, overlapped, kErrorAccessDenied));
    }
    const Status mounted = r.vfs.mount(root, package, MountAccess::ReadWrite);
    if (mounted == Status::AlreadyExists || mounted == Status::AccessDenied)
        return void(finish(ctx, overlapped, kErrorAlreadyExists));
    if (mounted != Status::Ok) {
        fprintf(stderr, "RCOMP-XAM-CONTENT mount %s: %s\n", root.c_str(), status_name(mounted));
        return void(finish(ctx, overlapped, kErrorFunctionFailed));
    }
    g_open[lower(root)] = package;
    if (disposition_out) guest_write_be32(disposition_out, create || reset ? kDispositionCreated : kDispositionOpened);
    // No marketplace licence exists for content the title creates itself.
    if (license_out) guest_write_be32(license_out, 0);
    fprintf(stderr, "RCOMP-XAM-CONTENT open root=%s type=0x%08X name=%s %s\n", root.c_str(), data.content_type,
            name.c_str(), create ? "created" : reset ? "recreated" : "opened");
    finish(ctx, overlapped, kErrorSuccess);
}

// XamContentClose(root_name, overlapped).
void XamContentClose(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamContentClose");
    const uint32_t overlapped = ctx.r4.u32;
    if (!overlapped_ok(r, overlapped)) { ctx.r3.u64 = kErrorInvalidParameter; return; }
    std::string root;
    if (!read_ansi(r, ctx.r3.u32, 33, &root) || !valid_root_name(root))
        return void(finish(ctx, overlapped, kErrorInvalidParameter));
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_open.find(lower(root));
    if (it == g_open.end()) return void(finish(ctx, overlapped, kErrorFileNotFound));
    const Status s = r.vfs.unmount(root);
    if (s != Status::Ok && s != Status::NotFound)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!XamContentClose unmount %s: %s", root.c_str(), status_name(s));
    g_open.erase(it);
    fprintf(stderr, "RCOMP-XAM-CONTENT close root=%s\n", root.c_str());
    finish(ctx, overlapped, kErrorSuccess);
}

// XamContentGetCreator(user_index, content_data, is_creator*, creator_xuid*, overlapped).
void XamContentGetCreator(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamContentGetCreator");
    const uint32_t user_index = ctx.r3.u32, content_data = ctx.r4.u32, is_creator_out = ctx.r5.u32,
                   xuid_out = ctx.r6.u32, overlapped = ctx.r7.u32;
    if (!overlapped_ok(r, overlapped) || !writable(r, is_creator_out, 4) || (xuid_out && !writable(r, xuid_out, 8))) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ContentData data;
    uint64_t xuid = 0;
    std::string dir, name;
    const uint32_t result = locate(r, user_index, content_data, "XamContentGetCreator", &data, &xuid, &dir, &name);
    if (result != kErrorSuccess) return void(finish(ctx, overlapped, result));
    if (name.empty()) return void(finish(ctx, overlapped, kErrorPathNotFound));
    uint64_t creator = 0;
    uint8_t display[kDisplayNameBytes];
    if (!read_metadata(dir + "/" + name + ".xcontent", &creator, display)) creator = 0;
    const bool is_creator = creator != 0 && creator == kLocalUserXuid && is_local_user(user_index);
    guest_write_be32(is_creator_out, is_creator ? 1 : 0);
    if (xuid_out) {
        uint8_t raw[8];
        put_be64(raw, creator);
        copy_to_guest(r, xuid_out, raw, 8);
    }
    finish(ctx, overlapped, kErrorSuccess);
}

// XamContentSetThumbnail(user_index, content_data, buffer, size, overlapped): stored beside the package.
void XamContentSetThumbnail(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamContentSetThumbnail");
    const uint32_t user_index = ctx.r3.u32, content_data = ctx.r4.u32, buffer = ctx.r5.u32, size = ctx.r6.u32,
                   overlapped = ctx.r7.u32;
    if (!overlapped_ok(r, overlapped) || !size || size > kMaxThumbnailBytes || !readable(r, buffer, size)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ContentData data;
    uint64_t xuid = 0;
    std::string dir, name;
    const uint32_t result = locate(r, user_index, content_data, "XamContentSetThumbnail", &data, &xuid, &dir, &name);
    if (result != kErrorSuccess) return void(finish(ctx, overlapped, result));
    if (name.empty()) return void(finish(ctx, overlapped, kErrorPathNotFound));
    std::vector<uint8_t> png(size);
    copy_from_guest(r, buffer, png.data(), size);
    finish(ctx, overlapped,
           write_file_atomic(dir + "/" + name + ".thumbnail", png.data(), png.size()) ? kErrorSuccess
                                                                                     : kErrorAccessDenied);
}

// XamContentGetDeviceData(device_id, XDEVICE_DATA*).
void XamContentGetDeviceData(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamContentGetDeviceData");
    const uint32_t device_id = ctx.r3.u32, output = ctx.r4.u32;
    if (!writable(r, output, kDeviceDataBytes)) { ctx.r3.u64 = kErrorInvalidParameter; return; }
    std::lock_guard<std::mutex> lock(g_mutex);
    Capacity capacity;
    if (device_id != kContentDeviceHdd || g_root.empty() || !dir_capacity(g_root, &capacity)) {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    uint8_t raw[kDeviceDataBytes] = {};
    put_be32(raw, kContentDeviceHdd);
    put_be32(raw + 4, kDeviceTypeHdd);
    put_be64(raw + 8, capacity.total);
    put_be64(raw + 0x10, capacity.free);
    const char* label = "Hard Drive";
    for (uint32_t i = 0; label[i] && i < 27; ++i) raw[0x18 + 2 * i + 1] = uint8_t(label[i]);
    copy_to_guest(r, output, raw, sizeof raw);
    ctx.r3.u64 = kErrorSuccess;
}

// XamShowDeviceSelectorUI(user_index, content_type, content_flags, total_requested (64-bit), device_id*,
// overlapped). No system UI exists on the PS5: the selection completes with the only device, as the console
// does when one storage device is present, and is logged.
void XamShowDeviceSelectorUI(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamShowDeviceSelectorUI");
    const uint32_t user_index = ctx.r3.u32, content_type = ctx.r4.u32, device_out = ctx.r7.u32,
                   overlapped = ctx.r8.u32;
    const uint64_t requested = ctx.r6.u64;
    if (!overlapped_ok(r, overlapped) || !writable(r, device_out, 4)) { ctx.r3.u64 = kErrorInvalidParameter; return; }
    uint32_t result = kErrorSuccess;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!is_local_user(user_index) && user_index != kUserAny) result = kErrorNoSuchUser;
        else if (g_root.empty()) result = kErrorCancelled;  // no storage device: the player can only cancel
    }
    if (result == kErrorSuccess) guest_write_be32(device_out, kContentDeviceHdd);
    fprintf(stderr, "RCOMP-XAM-UI DeviceSelector user=%u type=0x%08X requested=%llu -> %s\n", user_index,
            content_type, (unsigned long long)requested, result == kErrorSuccess ? "hard drive" : "none");
    finish(ctx, overlapped, result);
}

// fsync()s every regular file under `dir` (directories are walked with readdir, see clear_dir).
bool sync_tree(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool ok = true;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        const std::string path = dir + "/" + e->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) { ok = false; continue; }
        if (S_ISDIR(st.st_mode)) {
            ok = sync_tree(path) && ok;
        } else if (S_ISREG(st.st_mode)) {
            // Windows hosts flush only through a writable handle; FreeBSD (PS5) accepts either.
            int fd = open(path.c_str(), O_RDWR);
            if (fd < 0) fd = open(path.c_str(), O_RDONLY);
            if (fd < 0 || fsync(fd) != 0) ok = false;
            if (fd >= 0) close(fd);
        }
    }
    closedir(d);
    return ok;
}

// XamContentDelete(user_index, content_data, overlapped): removes the package, its metadata and thumbnail.
// A package mounted under a root name is in use and is not deleted (ERROR_SHARING_VIOLATION).
void XamContentDelete(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamContentDelete");
    const uint32_t user_index = ctx.r3.u32, content_data = ctx.r4.u32, overlapped = ctx.r5.u32;
    if (!overlapped_ok(r, overlapped)) { ctx.r3.u64 = kErrorInvalidParameter; return; }
    std::lock_guard<std::mutex> lock(g_mutex);
    ContentData data;
    uint64_t xuid = 0;
    std::string dir, name;
    const uint32_t result = locate(r, user_index, content_data, "XamContentDelete", &data, &xuid, &dir, &name);
    if (result != kErrorSuccess) return void(finish(ctx, overlapped, result));
    if (name.empty()) return void(finish(ctx, overlapped, kErrorFileNotFound));
    const std::string package = dir + "/" + name;
    for (const auto& [root, host] : g_open)
        if (host == package) return void(finish(ctx, overlapped, kErrorSharingViolation));
    const bool removed = clear_dir(package) && rmdir(package.c_str()) == 0;
    unlink((package + ".xcontent").c_str());
    unlink((package + ".thumbnail").c_str());
    fprintf(stderr, "RCOMP-XAM-CONTENT delete type=0x%08X name=%s %s\n", data.content_type, name.c_str(),
            removed ? "removed" : "FAILED");
    finish(ctx, overlapped, removed ? kErrorSuccess : kErrorAccessDenied);
}

// XamContentFlush(root_name, overlapped): commits the open package's files to storage (fsync of each file).
void XamContentFlush(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamContentFlush");
    const uint32_t overlapped = ctx.r4.u32;
    if (!overlapped_ok(r, overlapped)) { ctx.r3.u64 = kErrorInvalidParameter; return; }
    std::string root;
    if (!read_ansi(r, ctx.r3.u32, 33, &root) || !valid_root_name(root))
        return void(finish(ctx, overlapped, kErrorInvalidParameter));
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_open.find(lower(root));
    if (it == g_open.end()) return void(finish(ctx, overlapped, kErrorFileNotFound));
    finish(ctx, overlapped, sync_tree(it->second) ? kErrorSuccess : kErrorAccessDenied);
}

// XamContentGetDeviceState(device_id, overlapped): the hard drive is present while a save root exists.
void XamContentGetDeviceState(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamContentGetDeviceState");
    const uint32_t device_id = ctx.r3.u32, overlapped = ctx.r4.u32;
    if (!overlapped_ok(r, overlapped)) { ctx.r3.u64 = kErrorInvalidParameter; return; }
    bool present;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        present = device_id == kContentDeviceHdd && !g_root.empty();
    }
    finish(ctx, overlapped, present ? kErrorSuccess : kErrorDeviceNotConnected);
}

struct Entry {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Entry entries[] = {
    {0x025B, "XamContentDelete", &XamContentDelete},
    {0x0265, "XamContentGetDeviceState", &XamContentGetDeviceState},
    {0x0267, "XamContentFlush", &XamContentFlush},
    {0x0259, "XamContentCreateEx", &XamContentCreateEx},
    {0x025A, "XamContentClose", &XamContentClose},
    {0x025E, "XamContentGetDeviceData", &XamContentGetDeviceData},
    {0x0260, "XamContentSetThumbnail", &XamContentSetThumbnail},
    {0x0262, "XamContentGetCreator", &XamContentGetCreator},
    {0x02CB, "XamShowDeviceSelectorUI", &XamShowDeviceSelectorUI},
};

// Appends the packages of one type directory.
void list_type(const std::string& dir, uint32_t content_type, std::vector<std::vector<uint8_t>>* items) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        struct stat st;
        if (n == "." || n == ".." || !valid_file_name(n) || stat((dir + "/" + n).c_str(), &st) != 0 ||
            !S_ISDIR(st.st_mode))
            continue;
        names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        std::vector<uint8_t> item(kXContentDataBytes, 0);
        put_be32(item.data(), kContentDeviceHdd);
        put_be32(item.data() + 4, content_type);
        uint64_t creator = 0;
        if (!read_metadata(dir + "/" + n + ".xcontent", &creator, item.data() + 8)) {
            // No metadata (written by an older tool): show the file name as the display name.
            for (size_t i = 0; i < n.size() && i < 127; ++i) item[8 + 2 * i + 1] = uint8_t(n[i]);
        }
        memcpy(item.data() + 0x108, n.data(), n.size());
        items->push_back(std::move(item));
    }
}
}  // namespace

Status runtime_configure_save_root(const std::string& host_dir) {
    std::string root = host_dir;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    if (root.empty()) return Status::InvalidArgument;
    if (!mkdir_p(root)) return Status::NotFound;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_root.empty() && g_root != root) return Status::Conflict;
    g_root = root;
    return Status::Ok;
}

std::string save_root() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_root;
}

Status list_content(uint32_t user_index, uint32_t device_id, uint32_t content_type,
                    std::vector<std::vector<uint8_t>>* items) {
    items->clear();
    if (device_id != 0 && device_id != kContentDeviceHdd) return Status::Ok;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_root.empty()) return Status::Ok;
    const uint32_t title_id = xam_main_title_id("XamContentCreateEnumerator");
    std::vector<std::pair<uint64_t, uint32_t>> sources;  // (owner xuid, content type)
    auto add_type = [&](uint32_t type) {
        if (type == kContentTypeSavedGame) {
            if (is_local_user(user_index) || user_index == kUserAny) sources.push_back({kLocalUserXuid, type});
        } else {
            sources.push_back({0, type});
        }
    };
    if (content_type) {
        add_type(content_type);
    } else {
        // Every type present on disk for this title, saved games of the local user included.
        for (uint64_t owner : {kLocalUserXuid, uint64_t(0)}) {
            const std::string title_dir = g_root + "/" + hex(owner, 16) + "/" + hex(title_id, 8);
            DIR* d = opendir(title_dir.c_str());
            if (!d) continue;
            while (dirent* e = readdir(d)) {
                char* end = nullptr;
                const unsigned long type = strtoul(e->d_name, &end, 16);
                if (strlen(e->d_name) != 8 || !end || *end || !type) continue;
                if ((owner != 0) == (type == kContentTypeSavedGame)) add_type(uint32_t(type));
            }
            closedir(d);
        }
    }
    for (const auto& [owner, type] : sources) list_type(type_dir(owner, title_id, type), type, items);
    return Status::Ok;
}

void reset_xam_content() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_open.clear();
    g_root.clear();
}

Status register_xam_content_hle() {
    for (const auto& entry : entries) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXam, entry.name, &ordinal) || ordinal != entry.ordinal) return Status::Conflict;
        const Status status = register_import(kModuleXam, entry.ordinal, entry.function, entry.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}
}  // namespace rcomp::rt
