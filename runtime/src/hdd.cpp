// The Xbox 360 hard drive presented to a title (owner: Agent 3, runtime/).
// Contract: include/rcomp/runtime/hdd.h; design, layout and sources: runtime/docs/HDD.md.
#include "rcomp/runtime/hdd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#if defined(__linux__) || defined(__APPLE__) || defined(__CYGWIN__)
#include <sys/statvfs.h>
#define RCOMP_HDD_HAS_STATVFS 1
#else
#define RCOMP_HDD_HAS_STATVFS 0
#endif

#include "rcomp/diag.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/vfs.h"

namespace rcomp::rt {
namespace {

constexpr uint64_t kChunkBytes = 0x10000;  // raw storage granule (one host file per chunk written)
constexpr uint32_t kFatxHeaderBytes = 0x1000;  // FATX superblock area; the FAT starts after it
constexpr uint32_t kFatxDirEntryBytes = 0x40;
constexpr uint32_t kFatxMagic = 0x58544146u;  // "XTAF" read big-endian

Status errno_to_status(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR: return Status::NotFound;
    case EACCES: case EPERM: return Status::AccessDenied;
    default: return Status::IoError;
    }
}

bool make_dirs(const std::string& path) {
    if (path.empty()) return false;
    std::string current;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        current = path.substr(0, j);
        if (!current.empty() && ::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
            struct stat st;
            if (stat(current.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
        }
        i = j + 1;
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Sparse storage of a byte range: chunk i lives in <dir>/<16 hex digits of i>.bin, created on its
// first write; bytes never written (no file, or past a short file) read as zero.
class RawStore {
public:
    explicit RawStore(std::string dir) : dir_(std::move(dir)) {}

    Status read(uint64_t offset, uint8_t* dst, uint32_t len) {
        std::lock_guard<std::mutex> lock(mu_);
        while (len) {
            const uint64_t index = offset / kChunkBytes, within = offset % kChunkBytes;
            const uint32_t n = (uint32_t)std::min<uint64_t>(len, kChunkBytes - within);
            memset(dst, 0, n);
            const int fd = ::open(chunk_path(index).c_str(), O_RDONLY);
            if (fd < 0) {
                if (errno != ENOENT) return report("open", index, errno);
            } else {
                uint32_t done = 0;
                while (done < n) {
                    const ssize_t got = ::pread(fd, dst + done, n - done, (off_t)(within + done));
                    if (got < 0) {
                        if (errno == EINTR) continue;
                        const int e = errno;
                        ::close(fd);
                        return report("pread", index, e);
                    }
                    if (got == 0) break;  // short chunk: the rest was never written
                    done += (uint32_t)got;
                }
                ::close(fd);
            }
            offset += n; dst += n; len -= n;
        }
        return Status::Ok;
    }

    Status write(uint64_t offset, const uint8_t* src, uint32_t len) {
        std::lock_guard<std::mutex> lock(mu_);
        while (len) {
            const uint64_t index = offset / kChunkBytes, within = offset % kChunkBytes;
            const uint32_t n = (uint32_t)std::min<uint64_t>(len, kChunkBytes - within);
            const int fd = ::open(chunk_path(index).c_str(), O_RDWR | O_CREAT, 0600);
            if (fd < 0) return report("open", index, errno);
            uint32_t done = 0;
            while (done < n) {
                const ssize_t put = ::pwrite(fd, src + done, n - done, (off_t)(within + done));
                if (put < 0) {
                    if (errno == EINTR) continue;
                    const int e = errno;
                    ::close(fd);
                    return report("pwrite", index, e);
                }
                if (put == 0) { ::close(fd); return report("pwrite", index, EIO); }
                done += (uint32_t)put;
            }
            ::close(fd);
            dirty_.insert(index);
            offset += n; src += n; len -= n;
        }
        return Status::Ok;
    }

    // fsync of every chunk written since the last flush.
    Status flush() {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = dirty_.begin(); it != dirty_.end();) {
            const int fd = ::open(chunk_path(*it).c_str(), O_RDWR);
            if (fd < 0) return report("open", *it, errno);
            while (::fsync(fd) != 0) {
                if (errno == EINTR) continue;
                const int e = errno;
                ::close(fd);
                return report("fsync", *it, e);
            }
            ::close(fd);
            it = dirty_.erase(it);
        }
        return Status::Ok;
    }

private:
    std::string chunk_path(uint64_t index) const {
        char name[32];
        snprintf(name, sizeof(name), "/%016llx.bin", (unsigned long long)index);
        return dir_ + name;
    }
    Status report(const char* what, uint64_t index, int e) const {
        fprintf(stderr, "RCOMP-HDD %s %s failed errno=%d\n", what, chunk_path(index).c_str(), e);
        return Status::IoError;  // the device's backing store failed: a device I/O error
    }
    std::mutex mu_;
    std::string dir_;
    std::set<uint64_t> dirty_;
};

uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

struct Superblock {
    uint32_t serial = 0;
    uint32_t sectors_per_cluster = 0;
};

// FATX superblock (Free60 FATX table; the XDK formatter in the titles writes exactly these
// fields): "XTAF", serial, sectors per cluster (a power of two, 4 to 64 KiB clusters on retail
// drives; 512 B to 64 KiB accepted), root directory first cluster (nonzero).
bool parse_superblock(const uint8_t raw[16], Superblock* out) {
    if (be32(raw) != kFatxMagic) return false;
    const uint32_t spc = be32(raw + 8);
    if (!spc || (spc & (spc - 1)) || spc > 128 || !be32(raw + 12)) return false;
    out->serial = be32(raw + 4);
    out->sectors_per_cluster = spc;
    return true;
}

// Removes everything inside `dir` (not `dir` itself), without following links: a FATX format
// leaves an empty root directory.
Status empty_directory(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return errno_to_status(errno);
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        names.emplace_back(e->d_name);
    }
    closedir(d);
    for (const std::string& n : names) {
        const std::string path = dir + "/" + n;
        if (::unlink(path.c_str()) == 0) continue;
        if (errno != EISDIR && errno != EPERM && errno != EACCES) return errno_to_status(errno);
        const Status s = empty_directory(path);
        if (s != Status::Ok) return s;
        if (::rmdir(path.c_str()) != 0) return errno_to_status(errno);
    }
    return Status::Ok;
}

// FATX clusters used by a directory tree: every file its data rounded up to clusters, every
// directory (the root included) its 64-byte entries rounded up, at least one cluster.
uint64_t used_clusters(const std::string& dir, uint64_t cluster) {
    DIR* d = opendir(dir.c_str());
    if (!d) return 1;
    uint64_t entries = 0, total = 0;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        names.emplace_back(e->d_name);
    }
    closedir(d);
    for (const std::string& n : names) {
        const std::string path = dir + "/" + n;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        ++entries;
        if (S_ISDIR(st.st_mode)) total += used_clusters(path, cluster);
        else if (S_ISREG(st.st_mode) && st.st_size > 0) total += ((uint64_t)st.st_size + cluster - 1) / cluster;
    }
    total += std::max<uint64_t>(1, (entries * kFatxDirEntryBytes + cluster - 1) / cluster);
    return total;
}

// One utility partition: its raw bytes and the host directory that holds its file system.
class UtilityPartition {
public:
    UtilityPartition(unsigned index, uint64_t offset, uint64_t length, std::string raw_dir, std::string fs_dir)
        : index_(index), offset_(offset), length_(length), store_(std::move(raw_dir)), fs_dir_(std::move(fs_dir)) {}

    unsigned index() const { return index_; }
    uint64_t offset() const { return offset_; }
    uint64_t length() const { return length_; }
    const std::string& fs_dir() const { return fs_dir_; }

    Status read(uint64_t offset, void* dst, uint32_t len) { return store_.read(offset, (uint8_t*)dst, len); }

    // A write that turns the superblock into a new valid FATX superblock is a format: the file
    // system it replaces is gone, so the host directory is emptied (the FAT and root directory the
    // formatter writes are kept as raw bytes; the files live in the directory).
    Status write(uint64_t offset, const void* src, uint32_t len) {
        std::lock_guard<std::mutex> lock(format_mu_);
        const bool touches_superblock = offset < 16;
        uint8_t before[16] = {}, after[16] = {};
        if (touches_superblock) {
            const Status s = store_.read(0, before, 16);
            if (s != Status::Ok) return s;
        }
        Status s = store_.write(offset, (const uint8_t*)src, len);
        if (s != Status::Ok || !touches_superblock) return s;
        s = store_.read(0, after, 16);
        if (s != Status::Ok) return s;
        Superblock sb;
        if (memcmp(before, after, 16) != 0 && parse_superblock(after, &sb)) {
            s = empty_directory(fs_dir_);
            fprintf(stderr, "RCOMP-HDD format cache%u: FATX serial=0x%08X cluster=%u, %s emptied: %s\n", index_,
                    sb.serial, sb.sectors_per_cluster * (uint32_t)kHddSectorBytes, fs_dir_.c_str(), status_name(s));
            if (s != Status::Ok) return s;
        }
        return Status::Ok;
    }

    Status flush() { return store_.flush(); }

    bool superblock(Superblock* out) {
        uint8_t raw[16];
        return store_.read(0, raw, 16) == Status::Ok && parse_superblock(raw, out);
    }

    // FileFsSizeInformation / FileFsVolumeInformation of the FATX volume (layout of the XDK
    // formatter: 4 KiB superblock, FAT16 below 65520 clusters else FAT32, FAT rounded to 4 KiB,
    // then clusters). Free clusters: those the directory tree does not use, never more than the
    // host directory can still take.
    Status fatx_volume(VolumeMetadata* out) {
        Superblock sb;
        if (!superblock(&sb)) return Status::UnrecognizedVolume;
        const uint64_t cluster = (uint64_t)sb.sectors_per_cluster * kHddSectorBytes;
        const uint64_t entries = length_ / cluster + 1;
        const uint64_t fat = ((entries * (entries < 65520 ? 2 : 4)) + kFatxHeaderBytes - 1) & ~uint64_t(kFatxHeaderBytes - 1);
        if (length_ <= kFatxHeaderBytes + fat) return Status::UnrecognizedVolume;
        const uint64_t total = (length_ - kFatxHeaderBytes - fat) / cluster;
        const uint64_t used = used_clusters(fs_dir_, cluster);
        uint64_t available = total > used ? total - used : 0;
#if RCOMP_HDD_HAS_STATVFS
        struct statvfs fs;
        if (statvfs(fs_dir_.c_str(), &fs) == 0) {
            const uint64_t unit = fs.f_frsize ? (uint64_t)fs.f_frsize : (uint64_t)fs.f_bsize;
            available = std::min<uint64_t>(available, (uint64_t)fs.f_bavail * unit / cluster);
        }
#endif
        VolumeMetadata v;
        v.has_identity = true;
        v.creation_time = 0;  // a FATX superblock records no creation time
        v.serial_number = sb.serial;
        v.has_size = true;
        v.total_allocation_units = total;
        v.available_allocation_units = available;
        v.sectors_per_allocation_unit = sb.sectors_per_cluster;
        v.bytes_per_sector = (uint32_t)kHddSectorBytes;
        v.attributes = 0;
        v.component_name_max_length = (int32_t)kFatxMaxName;
        v.filesystem_name = "FATX";
        *out = std::move(v);
        return Status::Ok;
    }

private:
    unsigned index_;
    uint64_t offset_, length_;
    RawStore store_;
    std::string fs_dir_;
    std::mutex format_mu_;
};

// The partition's file system as reached by "\Device\Harddisk0\CacheN\..." and "\Device\cacheN\...".
class FatxVolumeSource : public VolumeSource {
public:
    explicit FatxVolumeSource(std::shared_ptr<UtilityPartition> p) : p_(std::move(p)) {}
    Status check_mounted() const override {
        Superblock sb;
        return p_->superblock(&sb) ? Status::Ok : Status::UnrecognizedVolume;
    }
    Status volume(VolumeMetadata* out) const override { return p_->fatx_volume(out); }

private:
    std::shared_ptr<UtilityPartition> p_;
};

// "\Device\Harddisk0\CacheN" opened as a whole.
class PartitionDevice : public GuestBlockDevice {
public:
    explicit PartitionDevice(std::shared_ptr<UtilityPartition> p) : p_(std::move(p)) {}
    uint64_t size() const override { return p_->length(); }
    uint64_t starting_offset() const override { return p_->offset(); }
    uint32_t bytes_per_sector() const override { return (uint32_t)kHddSectorBytes; }
    Status read(uint64_t offset, void* dst, uint32_t len) override { return p_->read(offset, dst, len); }
    Status write(uint64_t offset, const void* src, uint32_t len) override { return p_->write(offset, src, len); }
    Status flush() override { return p_->flush(); }
    // A volume handle answers for the file system mounted on the partition; an unformatted
    // partition has none (STATUS_UNRECOGNIZED_VOLUME).
    Status volume(VolumeMetadata* out) const override { return p_->fatx_volume(out); }

private:
    std::shared_ptr<UtilityPartition> p_;
};

// "\Device\Harddisk0\Partition0": the whole disk. R-comp stores the system area before the first
// partition and the two utility partitions (the same bytes as their own devices); the rest of the
// disk (SysExt, compatibility and data partitions) is not provided and a transfer there traps.
class DiskDevice : public GuestBlockDevice {
public:
    DiskDevice(std::shared_ptr<RawStore> system, std::shared_ptr<UtilityPartition> c0,
               std::shared_ptr<UtilityPartition> c1)
        : system_(std::move(system)), c0_(std::move(c0)), c1_(std::move(c1)) {}
    uint64_t size() const override { return kHddDiskBytes; }
    uint64_t starting_offset() const override { return 0; }
    uint32_t bytes_per_sector() const override { return (uint32_t)kHddSectorBytes; }
    Status read(uint64_t offset, void* dst, uint32_t len) override { return transfer(offset, (uint8_t*)dst, nullptr, len); }
    Status write(uint64_t offset, const void* src, uint32_t len) override {
        return transfer(offset, nullptr, (const uint8_t*)src, len);
    }
    Status flush() override {
        Status s = system_->flush();
        if (s == Status::Ok) s = c0_->flush();
        if (s == Status::Ok) s = c1_->flush();
        return s;
    }
    Status volume(VolumeMetadata*) const override { return Status::Unsupported; }  // no title asks

private:
    Status transfer(uint64_t offset, uint8_t* dst, const uint8_t* src, uint32_t len) {
        while (len) {
            uint64_t end = 0;
            Status s = Status::Ok;
            if (offset < kHddSystemAreaBytes) {
                end = kHddSystemAreaBytes;
                const uint32_t n = (uint32_t)std::min<uint64_t>(len, end - offset);
                s = dst ? system_->read(offset, dst, n) : system_->write(offset, src, n);
                if (s != Status::Ok) return s;
                advance(n, &offset, &dst, &src, &len);
                continue;
            }
            UtilityPartition* p = nullptr;
            if (offset >= c0_->offset() && offset - c0_->offset() < c0_->length()) p = c0_.get();
            else if (offset >= c1_->offset() && offset - c1_->offset() < c1_->length()) p = c1_.get();
            if (!p)
                rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                            "\\Device\\Harddisk0\\Partition0 %s at 0x%llX: R-comp provides the system area and the "
                            "utility partitions only (runtime/docs/HDD.md)",
                            dst ? "read" : "write", (unsigned long long)offset);
            const uint64_t within = offset - p->offset();
            const uint32_t n = (uint32_t)std::min<uint64_t>(len, p->length() - within);
            s = dst ? p->read(within, dst, n) : p->write(within, src, n);
            if (s != Status::Ok) return s;
            advance(n, &offset, &dst, &src, &len);
        }
        return Status::Ok;
    }
    static void advance(uint32_t n, uint64_t* offset, uint8_t** dst, const uint8_t** src, uint32_t* len) {
        *offset += n;
        if (*dst) *dst += n;
        if (*src) *src += n;
        *len -= n;
    }
    std::shared_ptr<RawStore> system_;
    std::shared_ptr<UtilityPartition> c0_, c1_;
};

struct HddState {
    uint64_t generation = 0;
    std::string root;
    std::shared_ptr<UtilityPartition> cache[2];
    std::shared_ptr<FatxVolumeSource> volume[2];
};

std::mutex g_mu;
std::shared_ptr<HddState> g_hdd;  // of the runtime lifetime g_hdd->generation

std::shared_ptr<HddState> current_hdd(Runtime** r) {
    *r = runtime();
    std::lock_guard<std::mutex> lock(g_mu);
    if (!*r || !g_hdd || g_hdd->generation != (*r)->generation) return nullptr;
    return g_hdd;
}

int title_device_index(const std::string& canonical) {
    if (canonical == "\\device\\cache0") return 0;
    if (canonical == "\\device\\cache1") return 1;
    return -1;
}

}  // namespace

Status runtime_configure_hdd(const std::string& host_dir) {
    Runtime* r = runtime();
    if (!r) return Status::NotInitialized;
    std::string root = host_dir;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    if (root.empty()) return Status::InvalidArgument;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_hdd && g_hdd->generation == r->generation) return Status::AlreadyExists;
    }
    for (const char* sub : {"/raw/system", "/raw/cache0", "/raw/cache1", "/cache0", "/cache1"})
        if (!make_dirs(root + sub)) return Status::NotFound;

    auto state = std::make_shared<HddState>();
    state->generation = r->generation;
    state->root = root;
    auto system = std::make_shared<RawStore>(root + "/raw/system");
    state->cache[0] = std::make_shared<UtilityPartition>(0, kHddCache0Offset, kHddCache0Bytes, root + "/raw/cache0",
                                                         root + "/cache0");
    state->cache[1] = std::make_shared<UtilityPartition>(1, kHddCache1Offset, kHddCache1Bytes, root + "/raw/cache1",
                                                         root + "/cache1");
    for (int i = 0; i < 2; ++i) state->volume[i] = std::make_shared<FatxVolumeSource>(state->cache[i]);

    Status s = r->vfs.attach_raw_device("\\Device\\Harddisk0\\Partition0",
                                        std::make_shared<DiskDevice>(system, state->cache[0], state->cache[1]));
    for (int i = 0; i < 2 && s == Status::Ok; ++i) {
        const std::string name = "\\Device\\Harddisk0\\Cache" + std::to_string(i);
        s = r->vfs.attach_raw_device(name, std::make_shared<PartitionDevice>(state->cache[i]));
        if (s == Status::Ok)
            s = r->vfs.mount_object(name, state->cache[i]->fs_dir(), MountAccess::ReadWrite, state->volume[i],
                                    kFatxMaxName);
    }
    if (s != Status::Ok) return s;
    std::lock_guard<std::mutex> lock(g_mu);
    g_hdd = std::move(state);
    return Status::Ok;
}

std::string hdd_root() {
    Runtime* r = nullptr;
    const auto hdd = current_hdd(&r);
    return hdd ? hdd->root : std::string();
}

void hdd_title_device_created(const std::string& canonical) {
    const int index = title_device_index(canonical);
    if (index < 0) return;
    Runtime* r = nullptr;
    const auto hdd = current_hdd(&r);
    if (!hdd) return;
    const Status s = r->vfs.mount_object(canonical, hdd->cache[index]->fs_dir(), MountAccess::ReadWrite,
                                         hdd->volume[index], kFatxMaxName);
    if (s != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "HDD: title device %s -> utility partition %d: %s", canonical.c_str(),
                    index, status_name(s));
    fprintf(stderr, "RCOMP-HDD title device %s -> utility partition %d (%s)\n", canonical.c_str(), index,
            hdd->cache[index]->fs_dir().c_str());
}

void hdd_title_device_deleted(const std::string& canonical) {
    if (title_device_index(canonical) < 0) return;
    Runtime* r = nullptr;
    if (!current_hdd(&r)) return;
    r->vfs.unmount_object(canonical);  // NotFound: the device was created before the drive existed
}

}  // namespace rcomp::rt
