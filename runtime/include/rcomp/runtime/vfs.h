// Sandboxed guest file access (owner: Agent 3, runtime/).
//
// Guest paths look like "game:\data\x.bin", "d:\x.bin", "\??\game:\x.bin"
// (also '/' as separator). The device ("game:", "d:", ...) must have been
// mounted onto a host root directory; the rest is mapped component by
// component under that root.
//
// Rejected with Status::PathRejected (never opened):
//   * any ".." component, "." components, empty path after the device;
//   * paths without a device prefix: host absolute paths ("/etc/passwd"),
//     UNC/NT device paths ("\Device\Harddisk0\..."), relative paths;
//   * components containing ':' (second drive / NTFS streams), control
//     characters, or NUL; paths longer than 255 bytes (Xbox limit is 255).
// An unmounted device -> Status::NoSuchDevice.
// Exception to the NT-path rule: object-namespace volumes and raw devices registered with
// mount_object()/attach_raw_device() (the hard drive, runtime/docs/HDD.md) resolve their
// "\Device\..." names; any other NT path stays rejected.
// After mapping, the host path is canonicalised with realpath() and must stay
// under the canonical root (defends against host symlinks pointing outside);
// this check is done at open time (TOCTOU window documented, acceptable for a
// read-only game directory).
//
// Case: device names are case-insensitive. Existing path components are also
// matched case-insensitively wherever directories can be listed, the PS5
// included (the Xbox file systems ignore case and titles spell names as they
// please); exact spelling remains the fast path. Where realpath() is refused (the
// PS5 sandbox: lexical confinement) the target is probed before the lexical
// result is trusted, so a name that differs only in case still folds. Without
// directory enumeration the host lookup stays exact.
//
// Access: mounts are read-only unless explicitly declared ReadWrite. The
// canonical game device is always read-only. Opening with write intent on a
// read-only mount -> Status::AccessDenied.
// Files are opened through the HandleTable (HandleKind::File).
#pragma once

#include <stdint.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rcomp/runtime/handle_table.h"
#include "rcomp/runtime/status.h"

namespace rcomp::rt {

enum class SeekOrigin { Begin, Current, End };
enum class MountAccess { ReadOnly, ReadWrite };
enum class OpenDisposition : uint32_t {
    Open = 1,
    Create = 2,
    OpenIf = 3,
    Overwrite = 4,
    OverwriteIf = 5,
};
enum class OpenAction : uint32_t {
    Opened = 1,
    Created = 2,
    Overwritten = 3,
};

struct OpenRequest {
    bool write_access = false;
    bool directory = false;
    bool write_through = false;
    bool no_buffering = false;
    // FILE_SYNCHRONOUS_IO_NONALERT: the file object does synchronous I/O (NT
    // FO_SYNCHRONOUS_IO); it cannot be associated with a completion port.
    bool synchronous_io = false;
    OpenDisposition disposition = OpenDisposition::Open;
};

// Xbox-facing metadata obtained from the opened host descriptor (or from a
// confined path for NtQueryFullAttributesFile). Integer fields are native host
// values here; HLE code serializes the Xbox structures in big-endian order.
struct FileMetadata {
    uint64_t creation_time = 0;
    uint64_t last_access_time = 0;
    uint64_t last_write_time = 0;
    uint64_t change_time = 0;
    uint64_t allocation_size = 0;
    uint64_t end_of_file = 0;
    uint64_t file_id = 0;
    uint32_t attributes = 0;
    uint32_t number_of_links = 0;
    bool directory = false;
};

struct FileBasicUpdate {
    uint64_t creation_time = 0;
    uint64_t last_access_time = 0;
    uint64_t last_write_time = 0;
    uint64_t change_time = 0;
    uint32_t attributes = 0;
};

struct FileMutableMetadata {
    mutable std::mutex mutex;
    bool has_attributes = false;
    uint32_t attributes = 0;
};

struct VolumeMetadata {
    // An arbitrary host-path mount has no Xbox/package volume identity.
    bool has_identity = false;
    uint64_t creation_time = 0;
    uint32_t serial_number = 0;
    // Capacity is populated from fstatvfs() on an opened descriptor.
    bool has_size = false;
    uint64_t total_allocation_units = 0;
    uint64_t available_allocation_units = 0;
    uint32_t sectors_per_allocation_unit = 1;
    uint32_t bytes_per_sector = 0x200;
    uint32_t attributes = 0;
    int32_t component_name_max_length = 255;
    std::string filesystem_name = "RCOMP";
};

// One directory entry produced by GuestFile::query_directory().
struct DirectoryEntry {
    std::string name;
    FileMetadata metadata;
};

// A raw device opened as a whole through its NT object name (a disk or a partition:
// "\Device\Harddisk0\Partition0"). The GuestFile that opens it has no host descriptor; reads and
// writes go to the device. Implemented by the hard drive (src/hdd.cpp, runtime/docs/HDD.md).
class GuestBlockDevice {
public:
    virtual ~GuestBlockDevice() = default;
    virtual uint64_t size() const = 0;             // bytes
    virtual uint64_t starting_offset() const = 0;  // byte offset of the device on its disk
    virtual uint32_t bytes_per_sector() const = 0;
    // Transfers of exactly `len` bytes inside [0, size()) (the caller clamps at the end).
    virtual Status read(uint64_t offset, void* dst, uint32_t len) = 0;
    virtual Status write(uint64_t offset, const void* src, uint32_t len) = 0;
    virtual Status flush() = 0;
    // NtQueryVolumeInformationFile on the device handle.
    virtual Status volume(VolumeMetadata* out) const = 0;
};

// Volume information of a file system the runtime keeps in a host directory and that is reached
// through an NT object name (Vfs::mount_object; the HDD utility partitions).
class VolumeSource {
public:
    virtual ~VolumeSource() = default;
    // Ok while the partition carries the file system; UnrecognizedVolume otherwise (nothing opens).
    virtual Status check_mounted() const = 0;
    virtual Status volume(VolumeMetadata* out) const = 0;
};

class GuestFile : public HandleObject {
public:
    static constexpr HandleKind kKind = HandleKind::File;
    GuestFile(int fd, uint64_t size, bool read_only_media, bool writable,
              bool directory, bool write_through, bool direct_io,
              std::string guest_path, std::string host_path,
              std::string mount_root, VolumeMetadata volume,
              std::shared_ptr<FileMutableMetadata> mutable_metadata)
        : fd_(fd), size_(size), read_only_media_(read_only_media), writable_(writable),
          directory_(directory), write_through_(write_through), direct_io_(direct_io),
          guest_path_(std::move(guest_path)), host_path_(std::move(host_path)),
          mount_root_(std::move(mount_root)), volume_(std::move(volume)),
          mutable_metadata_(std::move(mutable_metadata)) {}
    ~GuestFile() override;
    HandleKind kind() const override { return kKind; }
    void handle_opened() override;
    void handle_closed() override;

    // Reads at the current position and advances it. Reading at/after EOF with
    // len > 0 returns EndOfFile and *got = 0; a short read near EOF is Ok.
    Status read(void* dst, uint32_t len, uint32_t* got);
    Status read_at(uint64_t offset, void* dst, uint32_t len, uint32_t* got);  // also moves position
    Status write(const void* src, uint32_t len, uint32_t* written);
    Status write_at(uint64_t offset, const void* src, uint32_t len, uint32_t* written);
    Status write_to_end(const void* src, uint32_t len, uint32_t* written);
    Status seek(int64_t offset, SeekOrigin origin, uint64_t* new_pos);
    Status set_position(uint64_t position);
    Status truncate(uint64_t length);
    Status set_delete_on_close(bool enabled);
    Status set_basic(const FileBasicUpdate& update);
    Status metadata(FileMetadata* out) const;
    Status volume_metadata(VolumeMetadata* out) const;
    Status flush();
    // Directory enumeration (NtQueryDirectoryFile). The first call, or one with
    // restart, snapshots the directory's entries (sorted by name, "." and ".."
    // omitted) and applies `pattern` (DOS wildcards * and ?, case-insensitive;
    // empty = all). Later calls continue the scan and ignore `pattern`. Returns
    // NotFound when nothing matched on the first call of a scan and EndOfFile
    // once a scan is exhausted. InvalidArgument for a non-directory.
    Status query_directory(const std::string& pattern, bool restart, DirectoryEntry* out);
    uint64_t size() const { return size_; }
    uint64_t position() const;
    bool writable() const { return writable_; }
    bool directory() const { return directory_; }
    const std::string& guest_path() const { return guest_path_; }
    const std::string& host_path() const { return host_path_; }

    // I/O completion port of this file object (NtSetInformationFile
    // FileCompletionInformation). The association is per file object (every
    // handle to it shares it) and permanent, as in NT. InvalidArgument for a
    // synchronous-I/O file object, AlreadyExists when already associated.
    bool synchronous_io() const { return synchronous_io_; }
    Status set_completion_port(std::shared_ptr<HandleObject> port, uint32_t key);
    bool completion_port(std::shared_ptr<HandleObject>* port, uint32_t* key) const;

    // The raw device this handle opened as a whole (Vfs::attach_raw_device); nullptr for a file or a
    // directory.
    const std::shared_ptr<GuestBlockDevice>& block_device() const { return device_; }

private:
    friend class Vfs;
    Status read_locked(uint64_t offset, void* dst, uint32_t len, uint32_t* got);
    Status write_locked(uint64_t offset, const void* src, uint32_t len, uint32_t* written);
    Status delete_now_locked();
    mutable std::mutex mu_;
    int fd_;
    uint64_t size_;
    uint64_t pos_ = 0;
    bool read_only_media_ = true;
    bool writable_ = false;
    bool directory_ = false;
    bool write_through_ = false;
    bool direct_io_ = false;
    bool delete_on_close_ = false;
    bool synchronous_io_ = false;
    std::shared_ptr<HandleObject> completion_port_;  // guarded by mu_
    uint32_t completion_key_ = 0;
    uint32_t open_handle_count_ = 0;
    std::string guest_path_;
    std::string host_path_;
    std::string mount_root_;
    VolumeMetadata volume_;
    std::shared_ptr<FileMutableMetadata> mutable_metadata_;
    bool dir_loaded_ = false;
    bool dir_yielded_ = false;
    size_t dir_index_ = 0;
    std::string dir_pattern_;
    std::vector<std::string> dir_names_;
    std::shared_ptr<GuestBlockDevice> device_;            // raw device open (fd_ == -1)
    std::shared_ptr<const VolumeSource> volume_source_;   // volume of an object-namespace mount
};

// True once a path was confined lexically because realpath() failed with
// EPERM/ENOSYS (PS5: lstat refused). Reported by test runners.
extern bool vfs_lexical_fallback_used;

// Number of fsync() calls the VFS has made (diagnostic counter; the frame-rate counter line prints its per-second delta). A FILE_WRITE_THROUGH handle syncs after
// every mutation, an explicit NtFlushBuffersFile once.
uint64_t vfs_fsync_count();

// Drive names the title itself links (ObCreateSymbolicLink "\??\x:" -> "y:\dir"). When a path's drive is not
// mounted, the VFS asks this resolver for a rewritten path (one level per call; at most 8 levels are
// followed) before reporting NoSuchDevice. It is installed by the kernel object services; nullptr = none.
// The resolver is called without the VFS lock held.
using DriveLinkResolver = Status (*)(std::string_view guest_path, std::string* rewritten);
void vfs_set_drive_link_resolver(DriveLinkResolver resolver);

class Vfs {
public:
    // device: "game", "game:", "d:" ... (case-insensitive). host_root must be
    // an existing directory.
    Status mount(std::string_view device, const std::string& host_root,
                 MountAccess access = MountAccess::ReadOnly);
    void unmount_all();
    // Removes one device (content packages closed by XamContentClose). Handles already open on it stay
    // valid (they own their host descriptors). NotFound when the device is not mounted; AccessDenied for
    // the game devices ("game", "d"), which stay for the title's lifetime.
    Status unmount(std::string_view device);

    // Pure mapping, no host access: guest path -> host path under the root.
    Status resolve(std::string_view guest_path, std::string* host_path) const;

    // Opens an existing regular file for reading; *handle is a File handle in
    // `handles`.
    Status open(HandleTable& handles, std::string_view guest_path, bool write_access,
                uint32_t* handle);
    // Full open/create path used by NtCreateFile/NtOpenFile integration.
    Status open(HandleTable& handles, std::string_view guest_path,
                const OpenRequest& request, uint32_t* handle,
                OpenAction* action = nullptr);

    // Renames an already-open writable file within the same mounted root.
    Status rename(GuestFile& file, std::string_view target_guest_path,
                  bool replace_existing);

    // Metadata query without publishing a handle. The target must exist and
    // remain confined to its mount. Directories are allowed here.
    Status query(std::string_view guest_path, FileMetadata* out) const;

    // NT object namespace (runtime/docs/HDD.md). Names are "\Device\..." object names, compared
    // ignoring ASCII case. Without any of these, a "\Device\..." path is rejected as before
    // (PathRejected).
    //
    // A file system kept in a host directory: "<object_name>\<path>" maps to host_root and the root
    // itself, "<object_name>\", opens as a directory. Every resolution first asks `source` whether the
    // volume is mounted (UnrecognizedVolume otherwise); `component_max` is the longest name the volume
    // accepts (PathRejected beyond). AlreadyExists for a name already mounted.
    Status mount_object(std::string_view object_name, const std::string& host_root, MountAccess access,
                        std::shared_ptr<const VolumeSource> source, size_t component_max);
    // Handles already open on it stay valid. NotFound when not mounted.
    Status unmount_object(std::string_view object_name);
    // A raw device opened by exactly this name (no trailing component). AlreadyExists when taken.
    Status attach_raw_device(std::string_view object_name, std::shared_ptr<GuestBlockDevice> device);

private:
    struct Mount {
        std::string device;     // lower-case, without ':' (object mounts: the lower-case object name)
        std::string host_root;  // canonical, no trailing '/'
        MountAccess access = MountAccess::ReadOnly;
        VolumeMetadata volume;
        std::shared_ptr<const VolumeSource> source;  // object mounts only
        size_t component_max = 255;
    };
    struct Resolved {
        std::string host_path;
        std::string root;
        MountAccess access = MountAccess::ReadOnly;
        VolumeMetadata volume;
        std::shared_ptr<const VolumeSource> source;
        std::shared_ptr<GuestBlockDevice> raw;  // raw device open: host_path/root empty
        bool is_root = false;                   // the volume root directory itself
    };
    const Mount* find_mount(std::string_view device) const;
    Status resolve_impl(std::string_view guest_path, std::string* host_path,
                        std::string* root, MountAccess* access = nullptr,
                        VolumeMetadata* volume = nullptr) const;
    Status resolve_full(std::string_view guest_path, Resolved* out) const;
    Status resolve_one(std::string_view guest_path, Resolved* out) const;
    Status resolve_object(std::string_view guest_path, Resolved* out) const;
    Status resolve_mounted(std::string_view guest_path, std::string* host_path,
                           std::string* root, MountAccess* access,
                           VolumeMetadata* volume) const;
    Status open_raw(HandleTable& handles, std::string_view guest_path, const OpenRequest& request,
                    std::shared_ptr<GuestBlockDevice> device, uint32_t* handle, OpenAction* action);
    std::shared_ptr<FileMutableMetadata> metadata_state_locked(
        const std::string& host_path, bool reset) const;
    mutable std::mutex mu_;
    std::vector<Mount> mounts_;
    std::vector<Mount> object_mounts_;
    std::vector<std::pair<std::string, std::shared_ptr<GuestBlockDevice>>> raw_devices_;
    mutable std::unordered_map<std::string, std::shared_ptr<FileMutableMetadata>>
        mutable_metadata_;
};

}  // namespace rcomp::rt
