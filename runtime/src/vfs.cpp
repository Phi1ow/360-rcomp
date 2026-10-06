#include "rcomp/runtime/vfs.h"

#include <algorithm>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__linux__) || defined(__APPLE__) || defined(__CYGWIN__)
#include <dirent.h>
#include <sys/statvfs.h>
#define RCOMP_VFS_HAS_DIRENT 1
#define RCOMP_VFS_HAS_STATVFS 1
#else
#define RCOMP_VFS_HAS_DIRENT 0
#define RCOMP_VFS_HAS_STATVFS 0
#endif

// NtQueryDirectoryFile enumeration only needs opendir/readdir, independent of the
// platform gate above (which selects the confinement strategy).
#if __has_include(<dirent.h>)
#include <dirent.h>
#define RCOMP_VFS_CAN_ENUMERATE 1
#else
#define RCOMP_VFS_CAN_ENUMERATE 0
#endif

// The Xbox 360 file systems are case-insensitive and the titles spell names as they please (the Episodes from Liberty City executable asks for
// ep1_intro_music.rpf and AMERICAN.GXT where the disc has EP1_INTRO_MUSIC.rpf and american.gxt): the lookup that matches a component in its directory
// ignoring ASCII case only needs opendir/readdir, so every platform that can enumerate directories (the PS5's FreeBSD included) has it.
#if RCOMP_VFS_HAS_DIRENT || RCOMP_VFS_CAN_ENUMERATE
#define RCOMP_VFS_CASEFOLD 1
#else
#define RCOMP_VFS_CASEFOLD 0
#endif

namespace rcomp::rt {

namespace {

constexpr size_t kMaxGuestPath = 255;
constexpr uint32_t kFileAttributeReadOnly = 0x00000001u;
constexpr uint32_t kFileAttributeDirectory = 0x00000010u;
constexpr uint32_t kFileAttributeNormal = 0x00000080u;
constexpr uint64_t kFiletimeUnixEpoch = 116444736000000000ull;
constexpr uint64_t kFiletimeTicksPerSecond = 10000000ull;
constexpr uint64_t kAllocationUnit = 0x200;

bool valid_device_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return r;
}

#if RCOMP_VFS_CASEFOLD
bool ascii_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ac = (unsigned char)a[i], bc = (unsigned char)b[i];
        if (ac >= 'A' && ac <= 'Z') ac = (unsigned char)(ac + 32);
        if (bc >= 'A' && bc <= 'Z') bc = (unsigned char)(bc + 32);
        if (ac != bc) return false;
    }
    return true;
}
#endif

bool valid_component(std::string_view c) {
    if (c == "." || c == "..") return false;
    for (unsigned char ch : c) {
        if (ch < 0x20 || ch == 0x7F) return false;
        switch (ch) {
        case ':': case '*': case '?': case '"': case '<': case '>': case '|': return false;
        default: break;
        }
    }
    return true;
}

// Lexical normalisation of an absolute path: collapses "//" and ".", refuses
// "..". Used only when realpath() is unavailable (see canonical()).
bool lexical_canonical(const std::string& in, std::string* out) {
    if (in.empty() || in[0] != '/') return false;
    std::string r;
    size_t i = 0;
    while (i < in.size()) {
        while (i < in.size() && in[i] == '/') ++i;
        size_t j = in.find('/', i);
        if (j == std::string::npos) j = in.size();
        std::string comp = in.substr(i, j - i);
        i = j;
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") return false;
        r += "/" + comp;
    }
    *out = r.empty() ? "/" : r;
    return true;
}

bool force_lexical() {
    const char* e = getenv("RCOMP_VFS_FORCE_LEXICAL");
    return e && e[0] == '1';
}

bool canonical(const std::string& in, std::string* out, int* err) {
    char buf[PATH_MAX];
    if (!force_lexical() && realpath(in.c_str(), buf)) {
        *out = buf;
        return true;
    }
    int e = force_lexical() ? ENOSYS : errno;
    // Some title environments refuse the metadata calls used by realpath().
    // For absolute paths, fall back to lexical containment: guest paths never
    // contain ".." (checked before) and open() keeps O_NOFOLLOW.
    if ((e == EPERM || e == ENOSYS) && lexical_canonical(in, out)) {
        // Without lstat/openat, reject symlinked directories by opening every
        // proper prefix with O_NOFOLLOW (each prefix is then the final
        // component of its own open call). The last component is opened with
        // O_NOFOLLOW by the caller.
        for (size_t pos = out->find('/', 1); pos != std::string::npos; pos = out->find('/', pos + 1)) {
            std::string prefix = out->substr(0, pos);
            int fd = ::open(prefix.c_str(), O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
            if (fd < 0) {
                *err = (errno == ENOTDIR || errno == EMLINK) ? ELOOP : errno;
                return false;
            }
            ::close(fd);
        }
        vfs_lexical_fallback_used = true;
        return true;
    }
    *err = e;
    return false;
}

Status errno_status(int e) {
    switch (e) {
    case ENOENT:
    case ENOTDIR: return Status::NotFound;
    case EACCES:
    case EPERM:
#ifdef EROFS
    case EROFS:
#endif
        return Status::AccessDenied;
    case ELOOP: return Status::PathRejected;
    default: return Status::IoError;
    }
}

bool contained(const std::string& root, const std::string& path) {
    const std::string prefix = root == "/" ? root : root + "/";
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0;
}

uint64_t unix_seconds_to_filetime(time_t seconds) {
    const __int128 ticks = (__int128)seconds * kFiletimeTicksPerSecond +
                           (__int128)kFiletimeUnixEpoch;
    if (ticks <= 0) return 0;
    if (ticks > (__int128)UINT64_MAX) return UINT64_MAX;
    return (uint64_t)ticks;
}

uint64_t round_allocation(uint64_t size) {
    if (!size) return 0;
    if (size > UINT64_MAX - (kAllocationUnit - 1)) return UINT64_MAX;
    return (size + kAllocationUnit - 1) & ~(kAllocationUnit - 1);
}

Status stat_metadata(const struct stat& st, bool read_only, FileMetadata* out) {
    if (!out) return Status::InvalidArgument;
    const bool dir = S_ISDIR(st.st_mode);
    if (!dir && !S_ISREG(st.st_mode)) return Status::AccessDenied;
    if (st.st_size < 0) return Status::IoError;
    FileMetadata m;
#ifdef st_birthtime
    m.creation_time = unix_seconds_to_filetime(st.st_birthtime);
#else
    m.creation_time = unix_seconds_to_filetime(st.st_ctime);
#endif
    m.last_access_time = unix_seconds_to_filetime(st.st_atime);
    m.last_write_time = unix_seconds_to_filetime(st.st_mtime);
    // Xbox change time has no direct portable POSIX equivalent. Match the
    // pinned host-path reference by using the write timestamp.
    m.change_time = m.last_write_time;
    m.end_of_file = dir ? 0 : (uint64_t)st.st_size;
    m.allocation_size = dir ? 0 : round_allocation(m.end_of_file);
    m.file_id = (uint64_t)st.st_ino;
    const uint64_t links = (uint64_t)st.st_nlink;
    m.number_of_links = links > UINT32_MAX ? UINT32_MAX : (uint32_t)links;
    m.directory = dir;
    m.attributes = dir ? kFileAttributeDirectory : kFileAttributeNormal;
    if (!dir && (read_only || !(st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH))))
        m.attributes |= kFileAttributeReadOnly;
    *out = m;
    return Status::Ok;
}

Status filetime_to_timeval(uint64_t filetime, timeval* out) {
    if (!out || filetime < kFiletimeUnixEpoch) return Status::InvalidArgument;
    const uint64_t ticks = filetime - kFiletimeUnixEpoch;
    const uint64_t seconds = ticks / kFiletimeTicksPerSecond;
    const uint64_t usec = (ticks % kFiletimeTicksPerSecond) / 10;
    const time_t converted = (time_t)seconds;
    if ((uint64_t)converted != seconds) return Status::InvalidArgument;
    out->tv_sec = converted;
    out->tv_usec = (suseconds_t)usec;
    return Status::Ok;
}

#if RCOMP_VFS_CASEFOLD
Status casefold_existing(const std::string& root, const std::string& mapped, std::string* out) {
    if (!out || mapped.size() <= root.size() || mapped.compare(0, root.size(), root) != 0)
        return Status::InvalidArgument;
    std::string current = root;
    size_t i = root.size();
    while (i < mapped.size()) {
        while (i < mapped.size() && mapped[i] == '/') ++i;
        if (i == mapped.size()) break;
        size_t j = mapped.find('/', i);
        if (j == std::string::npos) j = mapped.size();
        const std::string_view wanted(mapped.data() + i, j - i);

        std::string canon_current;
        int err = 0;
        if (!canonical(current, &canon_current, &err)) return errno_status(err);
        if (canon_current != root && !contained(root, canon_current)) return Status::PathRejected;
        DIR* dir = opendir(canon_current.c_str());
        if (!dir) return errno_status(errno);
        std::string match;
        bool ambiguous = false;
        errno = 0;
        while (dirent* ent = readdir(dir)) {
            const std::string_view name(ent->d_name);
            if (name == "." || name == ".." || !ascii_iequals(name, wanted)) continue;
            if (match.empty()) match.assign(name.data(), name.size());
            else if (match != name) ambiguous = true;
        }
        const int read_err = errno;
        closedir(dir);
        if (read_err) return errno_status(read_err);
        if (ambiguous) return Status::PathRejected;
        if (match.empty()) return Status::NotFound;
        current = canon_current + "/" + match;
        i = j;
    }
    *out = current;
    return Status::Ok;
}
#endif

Status canonical_target(const std::string& mapped, const std::string& root, std::string* out) {
    int err = 0;
    bool resolved = canonical(mapped, out, &err);
#if RCOMP_VFS_CASEFOLD
    if (resolved && vfs_lexical_fallback_used) {
        // realpath() is refused in some title environments (EPERM on the PS5): the lexical fallback does not look at the file system, so a path whose
        // spelling differs in case would be taken as resolved and fail later in open(): look for the target here and fold the case when it is missing.
        int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        const int probe = ::open(out->c_str(), flags);
        if (probe >= 0) {
            ::close(probe);
        } else if (errno == ENOENT || errno == ENOTDIR) {
            resolved = false;
            err = errno;
        }
    }
#endif
    if (!resolved) {
#if RCOMP_VFS_CASEFOLD
        if (err == ENOENT || err == ENOTDIR) {
            std::string folded;
            Status s = casefold_existing(root, mapped, &folded);
            if (s != Status::Ok) return s;
            if (!canonical(folded, out, &err)) return errno_status(err);
        } else {
            return errno_status(err);
        }
#else
        return errno_status(err);
#endif
    }
    if (!contained(root, *out)) return Status::PathRejected;
    return Status::Ok;
}

Status resolve_create_target(const std::string& mapped, const std::string& root,
                             std::string* target, bool* exists) {
    if (!target || !exists || mapped.size() <= root.size() ||
        mapped.compare(0, root.size(), root) != 0)
        return Status::InvalidArgument;
    const size_t slash = mapped.find_last_of('/');
    if (slash == std::string::npos || slash < root.size() || slash + 1 >= mapped.size())
        return Status::PathRejected;

    const std::string parent_mapped = mapped.substr(0, slash);
    std::string parent;
    Status status = Status::Ok;
    if (parent_mapped == root) parent = root;
    else {
        status = canonical_target(parent_mapped, root, &parent);
        if (status != Status::Ok) return status;
    }
    int parent_flags = O_RDONLY;
#ifdef O_CLOEXEC
    parent_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    parent_flags |= O_NOFOLLOW;
#endif
#ifdef O_DIRECTORY
    parent_flags |= O_DIRECTORY;
#endif
    int parent_fd = ::open(parent.c_str(), parent_flags);
    if (parent_fd < 0)
        return (errno == ELOOP || errno == ENOTDIR) ? Status::PathRejected
                                                    : errno_status(errno);
    struct stat parent_st{};
    if (fstat(parent_fd, &parent_st) != 0) {
        const int error = errno;
        ::close(parent_fd);
        return errno_status(error);
    }
    ::close(parent_fd);
    if (!S_ISDIR(parent_st.st_mode)) return Status::PathRejected;

    std::string leaf = mapped.substr(slash + 1);
    std::string candidate = parent + "/" + leaf;
    *exists = false;
#if RCOMP_VFS_CASEFOLD
    DIR* dir = opendir(parent.c_str());
    if (!dir) return errno_status(errno);
    std::string match;
    bool ambiguous = false;
    errno = 0;
    while (dirent* ent = readdir(dir)) {
        std::string_view name(ent->d_name);
        if (name == "." || name == ".." || !ascii_iequals(name, leaf)) continue;
        if (match.empty()) match.assign(name.data(), name.size());
        else if (match != name) ambiguous = true;
    }
    const int read_error = errno;
    closedir(dir);
    if (read_error) return errno_status(read_error);
    if (ambiguous) return Status::PathRejected;
    if (!match.empty()) {
        candidate = parent + "/" + match;
        *exists = true;
    }
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int probe = ::open(candidate.c_str(), flags);
    if (probe >= 0) {
        *exists = true;
        ::close(probe);
    } else if (errno == ELOOP) {
        return Status::PathRejected;
    } else if (errno != ENOENT) {
        return errno_status(errno);
    }
#endif

    if (*exists) {
        std::string canonical_existing;
        status = canonical_target(candidate, root, &canonical_existing);
        if (status != Status::Ok) return status;
        candidate = std::move(canonical_existing);
    }
    *target = std::move(candidate);
    return Status::Ok;
}

}  // namespace

// ---------------------------------------------------------------- GuestFile

namespace {
std::atomic<uint64_t> g_fsync_count{0};

// fsync() that survives EINTR; every call is counted (vfs_fsync_count).
Status sync_descriptor(int fd) {
    g_fsync_count.fetch_add(1, std::memory_order_relaxed);
    while (::fsync(fd) != 0) {
        if (errno != EINTR) return errno_status(errno);
    }
    return Status::Ok;
}
}  // namespace

uint64_t vfs_fsync_count() { return g_fsync_count.load(std::memory_order_relaxed); }

GuestFile::~GuestFile() {
    if (fd_ >= 0) ::close(fd_);
}

void GuestFile::handle_opened() {
    std::lock_guard<std::mutex> lock(mu_);
    ++open_handle_count_;
}

Status GuestFile::delete_now_locked() {
    if (!writable_ || host_path_.empty()) return Status::AccessDenied;
    const int rc = directory_ ? ::rmdir(host_path_.c_str()) : ::unlink(host_path_.c_str());
    if (rc == 0 || errno == ENOENT) {
        delete_on_close_ = false;
        return Status::Ok;
    }
    return errno_status(errno);
}

void GuestFile::handle_closed() {
    std::lock_guard<std::mutex> lock(mu_);
    if (open_handle_count_) --open_handle_count_;
    if (open_handle_count_ == 0 && delete_on_close_) {
        const Status status = delete_now_locked();
        if (status != Status::Ok) {
            fprintf(stderr, "rcomp-rt: delete-on-close(%s) failed: %s\n",
                    guest_path_.c_str(), status_name(status));
        }
    }
}

uint64_t GuestFile::position() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pos_;
}

Status GuestFile::metadata(FileMetadata* out) const {
    if (!out) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    if (device_) {
        // A device opened as a whole: its size is the end of "file"; no times, no file id.
        FileMetadata m;
        m.end_of_file = m.allocation_size = size_;
        m.number_of_links = 1;
        m.attributes = kFileAttributeNormal;
        *out = m;
        return Status::Ok;
    }
    struct stat st;
    if (fstat(fd_, &st) != 0) return errno_status(errno);
    Status status = stat_metadata(st, read_only_media_, out);
    if (status == Status::Ok && mutable_metadata_) {
        std::lock_guard<std::mutex> metadata_lock(mutable_metadata_->mutex);
        if (mutable_metadata_->has_attributes)
            out->attributes = mutable_metadata_->attributes;
    }
    return status;
}

namespace {
// DOS-style wildcard match: '*' any run, '?' any one character, ASCII
// case-insensitive. The Xbox filesystem is case-insensitive.
bool wildcard_match(const char* pattern, const char* name) {
    if (!*pattern) return !*name;
    if (*pattern == '*') {
        while (*pattern == '*') ++pattern;
        if (!*pattern) return true;
        for (; *name; ++name)
            if (wildcard_match(pattern, name)) return true;
        return false;
    }
    if (!*name) return false;
    auto lower = [](char ch) { return (ch >= 'A' && ch <= 'Z') ? char(ch - 'A' + 'a') : ch; };
    if (*pattern != '?' && lower(*pattern) != lower(*name)) return false;
    return wildcard_match(pattern + 1, name + 1);
}
}  // namespace

Status GuestFile::query_directory(const std::string& pattern, bool restart, DirectoryEntry* out) {
    if (!out) return Status::InvalidArgument;
    if (!directory_) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    if (restart || !dir_loaded_) {
#if !RCOMP_VFS_CAN_ENUMERATE
        return Status::Unsupported;
#else
        dir_names_.clear();
        DIR* dir = opendir(host_path_.c_str());
        if (!dir) return errno_status(errno);
        while (dirent* entry = readdir(dir)) {
            const char* n = entry->d_name;
            if (!strcmp(n, ".") || !strcmp(n, "..")) continue;
            dir_names_.emplace_back(n);
        }
        closedir(dir);
        std::sort(dir_names_.begin(), dir_names_.end());
        dir_pattern_ = pattern.empty() ? std::string("*") : pattern;
        dir_index_ = 0;
        dir_yielded_ = false;
        dir_loaded_ = true;
#endif
    }
    while (dir_index_ < dir_names_.size()) {
        const std::string& name = dir_names_[dir_index_++];
        if (!wildcard_match(dir_pattern_.c_str(), name.c_str())) continue;
        struct stat st;
        const std::string path = host_path_ + "/" + name;
        if (stat(path.c_str(), &st) != 0) continue;  // vanished or special: not an Xbox entry
        FileMetadata metadata;
        if (stat_metadata(st, read_only_media_, &metadata) != Status::Ok) continue;
        out->name = name;
        out->metadata = metadata;
        dir_yielded_ = true;
        return Status::Ok;
    }
    return dir_yielded_ ? Status::EndOfFile : Status::NotFound;
}

Status GuestFile::volume_metadata(VolumeMetadata* out) const {
    if (!out) return Status::InvalidArgument;
    if (device_) return device_->volume(out);
    if (volume_source_) return volume_source_->volume(out);
    VolumeMetadata volume = volume_;
#if RCOMP_VFS_HAS_STATVFS
    struct statvfs fs;
    if (fstatvfs(fd_, &fs) == 0) {
        const uint64_t unit = fs.f_frsize ? (uint64_t)fs.f_frsize : (uint64_t)fs.f_bsize;
        if (unit >= volume.bytes_per_sector &&
            unit % volume.bytes_per_sector == 0 &&
            unit / volume.bytes_per_sector <= UINT32_MAX) {
            volume.total_allocation_units = (uint64_t)fs.f_blocks;
            volume.available_allocation_units = (uint64_t)fs.f_bavail;
            volume.sectors_per_allocation_unit =
                (uint32_t)(unit / volume.bytes_per_sector);
            volume.has_size = true;
        }
    } else {
        switch (errno) {
        case ENOSYS:
        case EPERM:
        case EACCES:
#ifdef EOPNOTSUPP
        case EOPNOTSUPP:
#endif
            break;
        default:
            return errno_status(errno);
        }
    }
#endif
    *out = std::move(volume);
    return Status::Ok;
}

Status GuestFile::flush() {
    std::lock_guard<std::mutex> lock(mu_);
    // Read-only handles can have no dirty runtime-managed file data. Completing
    // a flush is therefore real work (handle validation + empty dirty set), not
    // a success stub.
    if (!writable_) return Status::Ok;
    if (device_) return device_->flush();
    return sync_descriptor(fd_);
}

Status GuestFile::read_locked(uint64_t offset, void* dst, uint32_t len, uint32_t* got) {
    *got = 0;
    if (len == 0) return Status::Ok;
    if (!dst) return Status::InvalidArgument;
    if (directory_) return Status::IsDirectory;
    if (offset > (uint64_t)INT64_MAX ||
        (uint64_t)len > (uint64_t)INT64_MAX - offset)
        return Status::InvalidArgument;
    // FILE_NO_INTERMEDIATE_BUFFERING: sector-aligned offset and length. The buffer address is not checked:
    // the console reads into unaligned buffers on such handles (Halo 3 reads its map headers that way).
    if (direct_io_) {
        constexpr uint64_t kSector = 0x200;
        if ((offset & (kSector - 1)) || (len & (kSector - 1))) return Status::InvalidArgument;
    }
    if (device_) {
        // A device ends at its size: a start there or beyond is EndOfFile, a transfer that
        // crosses it stops there.
        if (offset >= size_) return Status::EndOfFile;
        const uint32_t n = (uint32_t)std::min<uint64_t>(len, size_ - offset);
        const Status s = device_->read(offset, dst, n);
        if (s != Status::Ok) return s;
        *got = n;
        pos_ = offset + n;
        return Status::Ok;
    }
    uint32_t total = 0;
    while (total < len) {
        ssize_t n = ::pread(fd_, (uint8_t*)dst + total, len - total, (off_t)(offset + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "rcomp-rt: pread(%s) failed errno=%d\n", guest_path_.c_str(), errno);
            return Status::IoError;
        }
        if (n == 0) break;
        total += (uint32_t)n;
    }
    if (total == 0) return Status::EndOfFile;
    *got = total;
    pos_ = offset + total;
    return Status::Ok;
}

Status GuestFile::read(void* dst, uint32_t len, uint32_t* got) {
    if (!got) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    return read_locked(pos_, dst, len, got);
}

Status GuestFile::read_at(uint64_t offset, void* dst, uint32_t len, uint32_t* got) {
    if (!got) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    return read_locked(offset, dst, len, got);
}

Status GuestFile::write_locked(uint64_t offset, const void* src, uint32_t len,
                               uint32_t* written) {
    *written = 0;
    if (!writable_) return Status::AccessDenied;
    if (directory_) return Status::IsDirectory;
    if (len == 0) return Status::Ok;
    if (!src) return Status::InvalidArgument;
    if (offset > (uint64_t)INT64_MAX ||
        (uint64_t)len > (uint64_t)INT64_MAX - offset)
        return Status::InvalidArgument;
    // FILE_NO_INTERMEDIATE_BUFFERING: sector-aligned offset and length. The buffer address is not checked:
    // the console reads into unaligned buffers on such handles (Halo 3 reads its map headers that way).
    if (direct_io_) {
        constexpr uint64_t kSector = 0x200;
        if ((offset & (kSector - 1)) || (len & (kSector - 1))) return Status::InvalidArgument;
    }
    if (device_) {
        // A device does not grow: a write that would cross its end writes nothing
        // (NT: STATUS_INVALID_PARAMETER past the end of a volume or a disk).
        if (offset >= size_ || len > size_ - offset) return Status::InvalidArgument;
        Status s = device_->write(offset, src, len);
        if (s != Status::Ok) return s;
        if (write_through_) {
            s = device_->flush();
            if (s != Status::Ok) return s;
        }
        *written = len;
        pos_ = offset + len;
        return Status::Ok;
    }

    uint32_t total = 0;
    while (total < len) {
        ssize_t n = ::pwrite(fd_, (const uint8_t*)src + total, len - total,
                             (off_t)(offset + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            *written = total;
            if (total) pos_ = offset + total;
            return errno_status(errno);
        }
        if (n == 0) {
            *written = total;
            if (total) pos_ = offset + total;
            return Status::IoError;
        }
        total += (uint32_t)n;
    }
    pos_ = offset + total;
    if (pos_ > size_) size_ = pos_;
    *written = total;
    if (write_through_) {
        const Status synced = sync_descriptor(fd_);
        if (synced != Status::Ok) return synced;
    }
    return Status::Ok;
}

Status GuestFile::write(const void* src, uint32_t len, uint32_t* written) {
    if (!written) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    return write_locked(pos_, src, len, written);
}

Status GuestFile::write_at(uint64_t offset, const void* src, uint32_t len,
                           uint32_t* written) {
    if (!written) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    return write_locked(offset, src, len, written);
}

Status GuestFile::write_to_end(const void* src, uint32_t len, uint32_t* written) {
    if (!written) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    if (device_) return write_locked(size_, src, len, written);  // a device cannot be extended
    struct stat st{};
    if (fstat(fd_, &st) != 0) return errno_status(errno);
    if (st.st_size < 0) return Status::IoError;
    return write_locked((uint64_t)st.st_size, src, len, written);
}

Status GuestFile::seek(int64_t offset, SeekOrigin origin, uint64_t* new_pos) {
    std::lock_guard<std::mutex> lock(mu_);
    int64_t basis = 0;
    switch (origin) {
    case SeekOrigin::Begin: basis = 0; break;
    case SeekOrigin::Current:
        if (pos_ > (uint64_t)INT64_MAX) return Status::InvalidArgument;
        basis = (int64_t)pos_;
        break;
    case SeekOrigin::End: {
        if (device_) {
            basis = (int64_t)size_;
            break;
        }
        struct stat st;
        if (fstat(fd_, &st) != 0) return errno_status(errno);
        if (st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)INT64_MAX) return Status::IoError;
        size_ = (uint64_t)st.st_size;
        basis = (int64_t)size_;
        break;
    }
    }
    const __int128 np = (__int128)basis + (__int128)offset;
    if (np < 0 || np > INT64_MAX) return Status::InvalidArgument;
    pos_ = (uint64_t)np;
    if (new_pos) *new_pos = pos_;
    return Status::Ok;
}

Status GuestFile::set_position(uint64_t position) {
    if (position > (uint64_t)INT64_MAX) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    pos_ = position;
    return Status::Ok;
}

Status GuestFile::truncate(uint64_t length) {
    if (!writable_) return Status::AccessDenied;
    if (directory_) return Status::IsDirectory;
    if (length > (uint64_t)INT64_MAX) return Status::InvalidArgument;
    if (device_) return Status::Unsupported;  // a disk or partition has a fixed size
    std::lock_guard<std::mutex> lock(mu_);
    while (::ftruncate(fd_, (off_t)length) != 0) {
        if (errno != EINTR) return errno_status(errno);
    }
    size_ = length;
    if (write_through_) {
        const Status synced = sync_descriptor(fd_);
        if (synced != Status::Ok) return synced;
    }
    return Status::Ok;
}

Status GuestFile::set_delete_on_close(bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!writable_ || device_) return Status::AccessDenied;
    delete_on_close_ = enabled;
    return Status::Ok;
}

Status GuestFile::set_basic(const FileBasicUpdate& update) {
    if (!writable_) return Status::AccessDenied;
    if (device_) return Status::Unsupported;  // no times or attributes on a raw device
    if (update.creation_time || update.change_time) return Status::Unsupported;
    std::lock_guard<std::mutex> lock(mu_);

    if (update.last_access_time || update.last_write_time) {
        struct stat st{};
        if (fstat(fd_, &st) != 0) return errno_status(errno);
        timeval times[2]{};
        if (update.last_access_time) {
            Status status = filetime_to_timeval(update.last_access_time, &times[0]);
            if (status != Status::Ok) return status;
        } else {
            times[0].tv_sec = st.st_atime;
        }
        if (update.last_write_time) {
            Status status = filetime_to_timeval(update.last_write_time, &times[1]);
            if (status != Status::Ok) return status;
        } else {
            times[1].tv_sec = st.st_mtime;
        }
        while (::futimes(fd_, times) != 0) {
            if (errno != EINTR) return errno_status(errno);
        }
    }

    if (update.attributes) {
        const uint32_t attributes = update.attributes;
        if (directory_ && !(attributes & kFileAttributeDirectory))
            return Status::InvalidArgument;
        if (!directory_ && (attributes & kFileAttributeDirectory))
            return Status::InvalidArgument;
        struct stat st{};
        if (fstat(fd_, &st) != 0) return errno_status(errno);
        mode_t mode = st.st_mode;
        if (attributes & kFileAttributeReadOnly)
            mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
        else
            mode |= S_IWUSR;
        while (::fchmod(fd_, mode) != 0) {
            if (errno != EINTR) return errno_status(errno);
        }
        if (!mutable_metadata_) return Status::IoError;
        std::lock_guard<std::mutex> metadata_lock(mutable_metadata_->mutex);
        mutable_metadata_->has_attributes = true;
        mutable_metadata_->attributes = attributes;
    }
    if (write_through_) {
        const Status synced = sync_descriptor(fd_);
        if (synced != Status::Ok) return synced;
    }
    return Status::Ok;
}

// ---------------------------------------------------------------- Vfs

bool vfs_lexical_fallback_used = false;

Status Vfs::mount(std::string_view device, const std::string& host_root, MountAccess access) {
    if (!device.empty() && device.back() == ':') device.remove_suffix(1);
    if (device.empty()) return Status::InvalidArgument;
    for (char c : device)
        if (!valid_device_char(c)) return Status::InvalidArgument;
    std::string root;
    int err = 0;
    if (!canonical(host_root, &root, &err)) return errno_status(err);
    struct stat st;
    if (stat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return Status::NotFound;
    std::lock_guard<std::mutex> lock(mu_);
    std::string dev = lower(device);
    // TitleRuntime exposes the packaged game root through both game: and d:.
    // Neither alias may become writable.
    if ((dev == "game" || dev == "d") && access != MountAccess::ReadOnly)
        return Status::AccessDenied;
    for (auto& m : mounts_)
        if (m.device == dev) return Status::AlreadyExists;
    if (root.size() > 1 && root.back() == '/') root.pop_back();
    Mount m;
    m.device = dev;
    m.host_root = root;
    m.access = access;
    mounts_.push_back(std::move(m));
    return Status::Ok;
}

Status Vfs::unmount(std::string_view device) {
    if (!device.empty() && device.back() == ':') device.remove_suffix(1);
    const std::string dev = lower(device);
    if (dev == "game" || dev == "d") return Status::AccessDenied;
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = mounts_.begin(); it != mounts_.end(); ++it) {
        if (it->device != dev) continue;
        mounts_.erase(it);
        return Status::Ok;
    }
    return Status::NotFound;
}

void Vfs::unmount_all() {
    std::lock_guard<std::mutex> lock(mu_);
    mounts_.clear();
    mutable_metadata_.clear();
}

std::shared_ptr<FileMutableMetadata> Vfs::metadata_state_locked(
    const std::string& host_path, bool reset) const {
    auto it = mutable_metadata_.find(host_path);
    if (reset || it == mutable_metadata_.end()) {
        auto state = std::make_shared<FileMutableMetadata>();
        mutable_metadata_[host_path] = state;
        return state;
    }
    return it->second;
}

const Vfs::Mount* Vfs::find_mount(std::string_view device) const {
    std::string dev = lower(device);
    for (auto& m : mounts_)
        if (m.device == dev) return &m;
    return nullptr;
}

Status Vfs::resolve(std::string_view p, std::string* host_path) const {
    return resolve_impl(p, host_path, nullptr, nullptr, nullptr);
}

namespace {
std::atomic<DriveLinkResolver> g_drive_link_resolver{nullptr};

// "\Device\..." (any object-manager name that is not a DOS-devices "\??\" path).
bool is_object_path(std::string_view p) {
    return !p.empty() && p[0] == '\\' && !(p.size() >= 4 && p.substr(0, 4) == "\\??\\");
}

// A lower-case "\device\a\b" name: no empty, "." or ".." component, no trailing separator.
bool valid_object_name(const std::string& name) {
    if (name.size() <= 8 || name.compare(0, 8, "\\device\\") != 0 || name.back() == '\\') return false;
    size_t i = 1;
    while (i < name.size()) {
        size_t j = name.find('\\', i);
        if (j == std::string::npos) j = name.size();
        const std::string_view comp(name.data() + i, j - i);
        if (comp.empty() || !valid_component(comp)) return false;
        i = j + 1;
    }
    return true;
}
}  // namespace

void vfs_set_drive_link_resolver(DriveLinkResolver resolver) { g_drive_link_resolver.store(resolver); }

Status Vfs::mount_object(std::string_view object_name, const std::string& host_root, MountAccess access,
                         std::shared_ptr<const VolumeSource> source, size_t component_max) {
    const std::string name = lower(object_name);
    if (!source || !component_max || !valid_object_name(name)) return Status::InvalidArgument;
    std::string root;
    int err = 0;
    if (!canonical(host_root, &root, &err)) return errno_status(err);
    struct stat st;
    if (stat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return Status::NotFound;
    if (root.size() > 1 && root.back() == '/') root.pop_back();
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& m : object_mounts_)
        if (m.device == name) return Status::AlreadyExists;
    Mount m;
    m.device = name;
    m.host_root = root;
    m.access = access;
    m.source = std::move(source);
    m.component_max = component_max;
    object_mounts_.push_back(std::move(m));
    return Status::Ok;
}

Status Vfs::unmount_object(std::string_view object_name) {
    const std::string name = lower(object_name);
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = object_mounts_.begin(); it != object_mounts_.end(); ++it) {
        if (it->device != name) continue;
        object_mounts_.erase(it);
        return Status::Ok;
    }
    return Status::NotFound;
}

Status Vfs::attach_raw_device(std::string_view object_name, std::shared_ptr<GuestBlockDevice> device) {
    const std::string name = lower(object_name);
    if (!device || !valid_object_name(name)) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& d : raw_devices_)
        if (d.first == name) return Status::AlreadyExists;
    raw_devices_.emplace_back(name, std::move(device));
    return Status::Ok;
}

Status Vfs::resolve_impl(std::string_view p, std::string* host_path, std::string* root,
                         MountAccess* access, VolumeMetadata* volume) const {
    if (!host_path) return Status::InvalidArgument;
    Resolved r;
    const Status s = resolve_full(p, &r);
    if (s != Status::Ok) return s;
    if (r.raw) return Status::Unsupported;  // a raw device has no host path
    *host_path = r.host_path;
    if (root) *root = r.root;
    if (access) *access = r.access;
    if (volume) *volume = r.volume;
    return Status::Ok;
}

Status Vfs::resolve_full(std::string_view p, Resolved* out) const {
    Status s = resolve_one(p, out);
    // The root of a drive that is neither mounted nor linked stays rejected as an invalid name, the
    // answer the VFS always gave for "x:\" alone; a linked drive's root follows its link (a volume
    // root, "cache1:\" -> "\Device\cache1\").
    bool root_only = false;
    if (s == Status::NoSuchDevice) {
        std::string_view rest = p;
        if (rest.size() >= 4 && rest.substr(0, 4) == "\\??\\") rest.remove_prefix(4);
        const size_t colon = rest.find(':');
        root_only = colon != std::string_view::npos &&
                    rest.find_first_not_of("\\/", colon + 1) == std::string_view::npos;
    }
    // A drive the title linked to another path: follow the links (bounded against loops).
    std::string current;
    for (int depth = 0; s == Status::NoSuchDevice && depth < 8; ++depth) {
        const DriveLinkResolver resolver = g_drive_link_resolver.load();
        std::string rewritten;
        if (!resolver || resolver(depth ? std::string_view(current) : p, &rewritten) != Status::Ok)
            return root_only ? Status::PathRejected : Status::NoSuchDevice;
        current = std::move(rewritten);
        s = resolve_one(current, out);
        // Only an object-namespace volume has an openable root; a drive linked to a host-directory
        // path keeps the old answer for its root.
        if (root_only && s != Status::NoSuchDevice && !is_object_path(current)) return Status::PathRejected;
    }
    return s;
}

Status Vfs::resolve_one(std::string_view p, Resolved* out) const {
    *out = Resolved{};
    if (is_object_path(p)) return resolve_object(p, out);
    return resolve_mounted(p, &out->host_path, &out->root, &out->access, &out->volume);
}

// "\Device\..." names: a raw device by its exact name, or a path on an object-namespace volume
// ("<volume>\<path>", "<volume>\" = its root directory). With neither configured, or for a name
// that is neither, the path is rejected as the VFS always rejected NT device paths.
Status Vfs::resolve_object(std::string_view p, Resolved* out) const {
    if (p.size() > kMaxGuestPath) return Status::PathRejected;
    const std::string lp = lower(p);
    std::shared_ptr<const VolumeSource> source;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& d : raw_devices_) {
            if (d.first != lp) continue;
            out->raw = d.second;
            return Status::Ok;
        }
        const Mount* best = nullptr;
        for (const auto& m : object_mounts_) {
            if (lp.size() > m.device.size() && lp.compare(0, m.device.size(), m.device) == 0 &&
                lp[m.device.size()] == '\\' && (!best || m.device.size() > best->device.size()))
                best = &m;
        }
        if (!best) return Status::PathRejected;
        const std::string_view rest = p.substr(best->device.size() + 1);
        std::string rel;
        size_t i = 0;
        while (i <= rest.size()) {
            size_t j = rest.find_first_of("\\/", i);
            if (j == std::string_view::npos) j = rest.size();
            const std::string_view comp = rest.substr(i, j - i);
            if (!comp.empty()) {
                if (!valid_component(comp) || comp.size() > best->component_max) return Status::PathRejected;
                rel += '/';
                rel.append(comp.data(), comp.size());
            }
            i = j + 1;
        }
        out->host_path = best->host_root + rel;
        out->root = best->host_root;
        out->access = best->access;
        out->volume = best->volume;
        out->source = best->source;
        out->is_root = rel.empty();
        source = best->source;
    }
    // Asked without the VFS lock: the volume reads its partition.
    return source->check_mounted();
}

Status Vfs::resolve_mounted(std::string_view p, std::string* host_path, std::string* root,
                            MountAccess* access, VolumeMetadata* volume) const {
    if (!host_path) return Status::InvalidArgument;
    if (p.empty() || p.size() > kMaxGuestPath) return Status::PathRejected;
    // NT object-manager DOS-devices prefix.
    if (p.size() >= 4 && p.substr(0, 4) == "\\??\\") p.remove_prefix(4);
    size_t colon = p.find(':');
    if (colon == std::string_view::npos || colon == 0) return Status::PathRejected;
    std::string_view device = p.substr(0, colon);
    for (char c : device)
        if (!valid_device_char(c)) return Status::PathRejected;  // "/x:..", "\Device\..:"
    std::string_view rest = p.substr(colon + 1);

    std::string rel;
    size_t i = 0;
    while (i <= rest.size()) {
        size_t j = rest.find_first_of("\\/", i);
        if (j == std::string_view::npos) j = rest.size();
        std::string_view comp = rest.substr(i, j - i);
        if (!comp.empty()) {
            if (!valid_component(comp)) return Status::PathRejected;
            rel += '/';
            rel.append(comp.data(), comp.size());
        }
        i = j + 1;
    }
    std::lock_guard<std::mutex> lock(mu_);
    const Mount* m = find_mount(device);
    // An unmounted drive may be a title link (its root "x:\" included, resolve_full).
    if (!m) return Status::NoSuchDevice;
    if (rel.empty()) return Status::PathRejected;  // root of a host-directory drive itself
    *host_path = m->host_root + rel;
    if (root) *root = m->host_root;
    if (access) *access = m->access;
    if (volume) *volume = m->volume;
    return Status::Ok;
}

Status Vfs::open(HandleTable& handles, std::string_view guest_path, bool write_access,
                 uint32_t* handle) {
    OpenRequest request;
    request.write_access = write_access;
    return open(handles, guest_path, request, handle, nullptr);
}

Status Vfs::open(HandleTable& handles, std::string_view guest_path,
                 const OpenRequest& request, uint32_t* handle,
                 OpenAction* action) {
    if (!handle) return Status::InvalidArgument;
    Resolved resolved;
    Status s = resolve_full(guest_path, &resolved);
    if (s != Status::Ok) return s;
    if (resolved.raw)
        return open_raw(handles, guest_path, request, std::move(resolved.raw), handle, action);
    const std::string host = resolved.host_path, root = resolved.root;
    const MountAccess access = resolved.access;
    VolumeMetadata volume = resolved.volume;
    const bool mutating_disposition = request.disposition != OpenDisposition::Open;
    if ((request.write_access || mutating_disposition) &&
        access != MountAccess::ReadWrite)
        return Status::AccessDenied;

    std::string target;
    bool exists = true;
    if (resolved.is_root) {
        // The root directory of an object-namespace volume ("\Device\...\"): it always exists and
        // is never created, replaced or deleted.
        if (request.disposition == OpenDisposition::Create) return Status::AlreadyExists;
        if (request.disposition == OpenDisposition::Overwrite ||
            request.disposition == OpenDisposition::OverwriteIf)
            return Status::InvalidArgument;
        if (!request.directory) return Status::IsDirectory;
        target = root;
    } else if (request.disposition == OpenDisposition::Open) {
        s = canonical_target(host, root, &target);
        if (s != Status::Ok) return s;
    } else {
        s = resolve_create_target(host, root, &target, &exists);
        if (s != Status::Ok) return s;
    }

    OpenAction resulting_action = OpenAction::Opened;
    switch (request.disposition) {
    case OpenDisposition::Open:
        resulting_action = OpenAction::Opened;
        break;
    case OpenDisposition::Create:
        if (exists) return Status::AlreadyExists;
        resulting_action = OpenAction::Created;
        break;
    case OpenDisposition::OpenIf:
        resulting_action = exists ? OpenAction::Opened : OpenAction::Created;
        break;
    case OpenDisposition::Overwrite:
        if (!exists) return Status::NotFound;
        resulting_action = OpenAction::Overwritten;
        break;
    case OpenDisposition::OverwriteIf:
        resulting_action = exists ? OpenAction::Overwritten : OpenAction::Created;
        break;
    default:
        return Status::InvalidArgument;
    }

    if (request.directory) {
        if (request.disposition == OpenDisposition::Overwrite ||
            request.disposition == OpenDisposition::OverwriteIf)
            return Status::InvalidArgument;
        if (!exists) {
            for (;;) {
                if (::mkdir(target.c_str(), 0700) == 0) break;
                if (errno != EINTR) return errno_status(errno);
            }
            exists = true;
        }
    }

    int flags = request.directory ? O_RDONLY :
                ((request.write_access || mutating_disposition) ? O_RDWR : O_RDONLY);
    if (!request.directory) {
        if (request.disposition == OpenDisposition::Create)
            flags |= O_CREAT | O_EXCL;
        else if (request.disposition == OpenDisposition::OpenIf && !exists)
            flags |= O_CREAT;
        else if (request.disposition == OpenDisposition::Overwrite)
            flags |= O_TRUNC;
        else if (request.disposition == OpenDisposition::OverwriteIf)
            flags |= exists ? O_TRUNC : O_CREAT;
    }
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_DIRECTORY
    if (request.directory) flags |= O_DIRECTORY;
#endif
    int fd = ::open(target.c_str(), flags, 0600);
    if (fd < 0) return errno_status(errno);
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        ::close(fd);
        return errno_status(e);
    }
    const bool is_directory = S_ISDIR(st.st_mode);
    if (is_directory && !request.directory) {
        ::close(fd);
        return Status::IsDirectory;
    }
    if (request.directory && !is_directory) {
        ::close(fd);
        return Status::InvalidArgument;
    }
    if (!is_directory && !S_ISREG(st.st_mode)) {
        ::close(fd);
        return Status::AccessDenied;
    }
    std::shared_ptr<FileMutableMetadata> mutable_metadata;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const bool reset_metadata =
            resulting_action == OpenAction::Created ||
            resulting_action == OpenAction::Overwritten;
        mutable_metadata = metadata_state_locked(target, reset_metadata);
    }
    auto file = std::make_shared<GuestFile>(fd, (uint64_t)st.st_size,
                                            access != MountAccess::ReadWrite,
                                            // The volume root is never deleted or renamed.
                                            request.write_access && !resolved.is_root, is_directory,
                                            request.write_through, request.no_buffering,
                                            std::string(guest_path), target, root,
                                            std::move(volume),
                                            std::move(mutable_metadata));
    file->synchronous_io_ = request.synchronous_io;
    file->volume_source_ = std::move(resolved.source);
    s = handles.insert(std::move(file), handle);  // on failure the file closes fd
    if (s == Status::Ok && action) *action = resulting_action;
    return s;
}

// A raw device opened as a whole: it exists (Open/OpenIf open it, Create collides), is not a
// directory and cannot be replaced.
Status Vfs::open_raw(HandleTable& handles, std::string_view guest_path, const OpenRequest& request,
                     std::shared_ptr<GuestBlockDevice> device, uint32_t* handle, OpenAction* action) {
    if (request.directory) return Status::InvalidArgument;
    switch (request.disposition) {
    case OpenDisposition::Open:
    case OpenDisposition::OpenIf: break;
    case OpenDisposition::Create: return Status::AlreadyExists;
    default: return Status::InvalidArgument;
    }
    auto file = std::make_shared<GuestFile>(-1, device->size(), false, request.write_access, false,
                                            request.write_through, request.no_buffering,
                                            std::string(guest_path), std::string(), std::string(),
                                            VolumeMetadata{}, nullptr);
    file->device_ = std::move(device);
    file->synchronous_io_ = request.synchronous_io;
    const Status s = handles.insert(std::move(file), handle);
    if (s == Status::Ok && action) *action = OpenAction::Opened;
    return s;
}

Status GuestFile::set_completion_port(std::shared_ptr<HandleObject> port, uint32_t key) {
    if (!port) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(mu_);
    if (synchronous_io_) return Status::InvalidArgument;
    if (completion_port_) return Status::AlreadyExists;
    completion_port_ = std::move(port);
    completion_key_ = key;
    return Status::Ok;
}

bool GuestFile::completion_port(std::shared_ptr<HandleObject>* port, uint32_t* key) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!completion_port_) return false;
    if (port) *port = completion_port_;
    if (key) *key = completion_key_;
    return true;
}

Status Vfs::rename(GuestFile& file, std::string_view target_guest_path,
                   bool replace_existing) {
    std::string mapped, root;
    MountAccess access = MountAccess::ReadOnly;
    Status status = resolve_impl(target_guest_path, &mapped, &root, &access, nullptr);
    if (status != Status::Ok) return status;
    if (access != MountAccess::ReadWrite) return Status::AccessDenied;

    std::lock_guard<std::mutex> file_lock(file.mu_);
    if (!file.writable_) return Status::AccessDenied;
    if (root != file.mount_root_) return Status::InvalidArgument;

    std::string target;
    bool exists = false;
    status = resolve_create_target(mapped, root, &target, &exists);
    if (status != Status::Ok) return status;
    if (target == file.host_path_) {
        file.guest_path_ = std::string(target_guest_path);
        return Status::Ok;
    }
    if (exists && !replace_existing) return Status::AlreadyExists;

    struct stat target_st{};
    if (exists) {
        if (stat(target.c_str(), &target_st) != 0) return errno_status(errno);
        if (S_ISDIR(target_st.st_mode) != file.directory_) return Status::InvalidArgument;
    }
    const std::string old_host_path = file.host_path_;
    for (;;) {
        if (::rename(old_host_path.c_str(), target.c_str()) == 0) break;
        if (errno != EINTR) return errno_status(errno);
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (exists) mutable_metadata_.erase(target);
        auto it = mutable_metadata_.find(old_host_path);
        if (it != mutable_metadata_.end()) {
            mutable_metadata_[target] = it->second;
            mutable_metadata_.erase(it);
        } else if (file.mutable_metadata_) {
            mutable_metadata_[target] = file.mutable_metadata_;
        }
    }
    file.host_path_ = std::move(target);
    file.guest_path_ = std::string(target_guest_path);
    return Status::Ok;
}

Status Vfs::query(std::string_view guest_path, FileMetadata* out) const {
    if (!out) return Status::InvalidArgument;
    Resolved resolved;
    Status s = resolve_full(guest_path, &resolved);
    if (s != Status::Ok) return s;
    if (resolved.raw) return Status::Unsupported;  // a raw device has no file attributes
    const MountAccess access = resolved.access;
    std::string canon;
    if (resolved.is_root) {
        canon = resolved.root;
    } else {
        s = canonical_target(resolved.host_path, resolved.root, &canon);
        if (s != Status::Ok) return s;
    }
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = ::open(canon.c_str(), flags);
    if (fd < 0) return errno_status(errno);
    struct stat st;
    if (fstat(fd, &st) != 0) {
        const int e = errno;
        ::close(fd);
        return errno_status(e);
    }
    ::close(fd);
    s = stat_metadata(st, access != MountAccess::ReadWrite, out);
    if (s != Status::Ok) return s;
    std::shared_ptr<FileMutableMetadata> metadata;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = mutable_metadata_.find(canon);
        if (it != mutable_metadata_.end()) metadata = it->second;
    }
    if (metadata) {
        std::lock_guard<std::mutex> metadata_lock(metadata->mutex);
        if (metadata->has_attributes) out->attributes = metadata->attributes;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
