// The Xbox 360 hard drive (src/hdd.cpp, runtime/docs/HDD.md): raw Partition0 / Cache0 / Cache1
// devices, the disk IOCTLs, the XDK FATX format sequence, the utility-partition file systems
// under "\Device\Harddisk0\CacheN\" and under a title device "\Device\cacheN", persistence across
// runtime lifetimes, and the traps outside the provided range.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/hdd.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__NtCreateFile);
PPC_EXTERN_FUNC(__imp__NtReadFile);
PPC_EXTERN_FUNC(__imp__NtWriteFile);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__NtFlushBuffersFile);
PPC_EXTERN_FUNC(__imp__NtDeviceIoControlFile);
PPC_EXTERN_FUNC(__imp__NtQueryVolumeInformationFile);
PPC_EXTERN_FUNC(__imp__NtQueryInformationFile);
PPC_EXTERN_FUNC(__imp__IoCreateDevice);
PPC_EXTERN_FUNC(__imp__IoDeleteDevice);
PPC_EXTERN_FUNC(__imp__ObCreateSymbolicLink);

using namespace rcomp;
using namespace rcomp::rt;

namespace {

constexpr uint32_t kUnrecognizedVolume = 0xC000014Fu;
constexpr uint32_t kObjectNameInvalid = 0xC0000033u;
constexpr uint32_t kInvalidDeviceRequest = 0xC0000010u;
constexpr uint32_t kBufferTooSmall = 0xC0000023u;
constexpr uint32_t kRw = 0xC0100000u;  // GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE
constexpr uint32_t kCluster = 0x10000;

uint8_t* g_base;
uint32_t g_scratch;  // 0x4000 bytes

enum : uint32_t {
    kHandleCell = 0x00,
    kIosb = 0x08,
    kOffset = 0x10,
    kAttrs = 0x20,     // OBJECT_ATTRIBUTES
    kName = 0x30,      // ANSI_STRING
    kNameText = 0x40,  // 0x100 bytes
    kAnsiB = 0x140,
    kTextB = 0x150,
    kOut = 0x200,      // device object cell
    kInfo = 0x240,     // 0x40 bytes of query output
    kStack = 0x300,    // fake guest stack frame (arguments at +0x54)
    kBuffer = 0x1000,  // 0x2000 bytes of transfer buffer
};

void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
uint32_t rd32(uint32_t a) { uint32_t v = 0; CHECK(guest_read_be32(a, &v)); return v; }
uint64_t rd64(uint32_t a) { uint64_t v = 0; CHECK(guest_read_be64(a, &v)); return v; }

uint32_t ansi(uint32_t descriptor, uint32_t text, const std::string& s) {
    memcpy(g_base + g_scratch + text, s.data(), s.size());
    const uint32_t d = g_scratch + descriptor;
    g_base[d] = uint8_t(s.size() >> 8); g_base[d + 1] = uint8_t(s.size());
    g_base[d + 2] = uint8_t((s.size() + 1) >> 8); g_base[d + 3] = uint8_t(s.size() + 1);
    CHECK(guest_write_be32(d + 4, g_scratch + text));
    return d;
}

uint32_t nt_create(const std::string& path, uint32_t access, uint32_t disposition, uint32_t options,
                   uint32_t* handle) {
    ansi(kName, kNameText, path);
    CHECK(guest_write_be32(g_scratch + kAttrs, 0));
    CHECK(guest_write_be32(g_scratch + kAttrs + 4, g_scratch + kName));
    CHECK(guest_write_be32(g_scratch + kAttrs + 8, 0x40));
    CHECK(guest_write_be32(g_scratch + kHandleCell, 0));
    CHECK(guest_write_be32(g_scratch + kStack + 0x54, options));
    PPCContext c{};
    c.r1.u64 = g_scratch + kStack;
    c.r3.u64 = g_scratch + kHandleCell; c.r4.u64 = access; c.r5.u64 = g_scratch + kAttrs;
    c.r6.u64 = g_scratch + kIosb; c.r7.u64 = 0; c.r8.u64 = 0x80; c.r9.u64 = 0; c.r10.u64 = disposition;
    __imp__NtCreateFile(c, g_base);
    *handle = rd32(g_scratch + kHandleCell);
    return c.r3.u32;
}

uint32_t nt_close(uint32_t handle) {
    PPCContext c{};
    c.r3.u64 = handle;
    __imp__NtClose(c, g_base);
    return c.r3.u32;
}

uint32_t nt_transfer(PPCFunc* fn, uint32_t handle, uint64_t offset, uint32_t length) {
    CHECK(guest_write_be64(g_scratch + kOffset, offset));
    PPCContext c{};
    c.r3.u64 = handle; c.r7.u64 = g_scratch + kIosb; c.r8.u64 = g_scratch + kBuffer;
    c.r9.u64 = length; c.r10.u64 = g_scratch + kOffset;
    fn(c, g_base);
    return c.r3.u32;
}

uint32_t nt_ioctl(uint32_t handle, uint32_t code, uint32_t out_length) {
    memset(g_base + g_scratch + kInfo, 0xEE, 0x20);
    CHECK(guest_write_be32(g_scratch + kStack + 0x54, g_scratch + kInfo));
    CHECK(guest_write_be32(g_scratch + kStack + 0x5C, out_length));
    PPCContext c{};
    c.r1.u64 = g_scratch + kStack;
    c.r3.u64 = handle; c.r7.u64 = g_scratch + kIosb; c.r8.u64 = code;
    c.lr = 0x82001234;
    __imp__NtDeviceIoControlFile(c, g_base);
    return c.r3.u32;
}

uint32_t nt_volume_size(uint32_t handle) {
    PPCContext c{};
    c.r3.u64 = handle; c.r4.u64 = g_scratch + kIosb; c.r5.u64 = g_scratch + kInfo; c.r6.u64 = 24; c.r7.u64 = 3;
    __imp__NtQueryVolumeInformationFile(c, g_base);
    return c.r3.u32;
}

bool host_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string read_host(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string s;
    int ch;
    while ((ch = fgetc(f)) != EOF) s.push_back(char(ch));
    fclose(f);
    return s;
}

// The XDK FATX formatter's write sequence (Halo 3 sub_82582DF8): zero header page, FAT
// (entries 0/1 = 0xFFF8/0xFFFF on FAT16), root directory cluster of 0xFF, then the superblock.
void xdk_format(uint32_t handle, uint64_t length, uint32_t serial) {
    const uint64_t entries = length / kCluster + 1;
    const uint64_t fat = (entries * 2 + 0xFFF) & ~uint64_t(0xFFF);
    uint8_t* buf = g_base + g_scratch + kBuffer;
    memset(buf, 0, 0x1000);
    CHECK_EQ(nt_transfer(__imp__NtWriteFile, handle, 0, 0x1000), nt::kSuccess);
    for (uint64_t at = 0; at < fat; at += 0x1000) {
        memset(buf, 0, 0x1000);
        if (!at) { buf[0] = 0xFF; buf[1] = 0xF8; buf[2] = 0xFF; buf[3] = 0xFF; }
        CHECK_EQ(nt_transfer(__imp__NtWriteFile, handle, 0x1000 + at, 0x1000), nt::kSuccess);
    }
    memset(buf, 0xFF, 0x1000);
    for (uint64_t at = 0; at < kCluster; at += 0x1000)
        CHECK_EQ(nt_transfer(__imp__NtWriteFile, handle, 0x1000 + fat + at, 0x1000), nt::kSuccess);
    memset(buf, 0xFF, 0x1000);
    put_be32(buf, 0x58544146u);  // "XTAF"
    put_be32(buf + 4, serial);
    put_be32(buf + 8, kCluster / 512);
    put_be32(buf + 12, 1);
    buf[16] = buf[17] = 0;
    CHECK_EQ(nt_transfer(__imp__NtWriteFile, handle, 0, 0x1000), nt::kSuccess);
}

uint64_t fatx_total_clusters(uint64_t length) {
    const uint64_t entries = length / kCluster + 1;
    const uint64_t fat = (entries * (entries < 65520 ? 2 : 4) + 0xFFF) & ~uint64_t(0xFFF);
    return (length - 0x1000 - fat) / kCluster;
}

uint32_t create_device(const std::string& name, uint32_t* device) {
    PPCContext c{};
    c.r3.u64 = 0x82B0EC40u; c.r4.u64 = 0x100; c.r5.u64 = ansi(kName, kNameText, name); c.r6.u64 = 61;
    c.r7.u64 = 0x2000; c.r8.u64 = g_scratch + kOut;
    __imp__IoCreateDevice(c, g_base);
    *device = rd32(g_scratch + kOut);
    return c.r3.u32;
}

uint32_t create_link(const std::string& link, const std::string& target) {
    PPCContext c{};
    c.r3.u64 = ansi(kName, kNameText, link);
    c.r4.u64 = ansi(kAnsiB, kTextB, target);
    __imp__ObCreateSymbolicLink(c, g_base);
    return c.r3.u32;
}

void start_runtime(GuestMemory& mem) {
    CHECK_ST(runtime_init(&mem), Status::Ok);
    clear_imports();
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x4000, 16, true, &g_scratch), Status::Ok);
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/rcomp_rt_hdd_XXXXXX";
    const std::string top = mkdtemp(tmpl) ? tmpl : "";
    if (top.empty()) return 2;
    const std::string hdd = top + "/hdd";

    GuestMemory mem;
    if (mem.reserve() != MemStatus::Ok) return 2;
    g_base = mem.base();
    start_runtime(mem);
    Runtime* r = runtime();
    uint32_t h = 0, h2 = 0;

    // ---- no drive: NT device paths are refused as before --------------------------------------
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\partition0", kRw, 1, 0x22, &h), kObjectNameInvalid);
    CHECK(hdd_root() == std::string());
    // The root of a drive neither mounted nor linked stays an invalid name (as before).
    CHECK_EQ(nt_create("nodrive:\\", 0x100001, 1, 0x21, &h), kObjectNameInvalid);
    // FILE_SYNCHRONOUS_IO_ALERT and _NONALERT together are contradictory (NT).
    CHECK_EQ(nt_create("nodrive:\\x", 0x100001, 1, 0x30, &h), nt::kInvalidParameter);

    // ---- configuration --------------------------------------------------------------------------
    CHECK_ST(runtime_configure_hdd(hdd), Status::Ok);
    CHECK_ST(runtime_configure_hdd(hdd), Status::AlreadyExists);
    CHECK(hdd_root() == hdd);
    for (const char* sub : {"/raw/system", "/raw/cache0", "/raw/cache1", "/cache0", "/cache1"})
        CHECK(host_exists(hdd + sub));

    // ---- Partition0: system area (Halo 3's utility-drive table at 2048) --------------------------
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\partition0", kRw, 1, 0x22, &h), nt::kSuccess);
    memset(g_base + g_scratch + kBuffer, 0x5A, 1024);
    CHECK_EQ(nt_transfer(__imp__NtReadFile, h, 2048, 1024), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 1024u);
    CHECK_EQ(rd32(g_scratch + kBuffer), 0u);  // a new drive reads zero
    CHECK_EQ(rd32(g_scratch + kBuffer + 1020), 0u);
    for (uint32_t i = 0; i < 1024; ++i) g_base[g_scratch + kBuffer + i] = uint8_t(i * 13 + 1);
    CHECK_EQ(nt_transfer(__imp__NtWriteFile, h, 2048, 1024), nt::kSuccess);
    {
        PPCContext c{};
        c.r3.u64 = h; c.r4.u64 = g_scratch + kIosb;
        __imp__NtFlushBuffersFile(c, g_base);
        CHECK_EQ(c.r3.u32, nt::kSuccess);
    }
    CHECK(host_exists(hdd + "/raw/system/0000000000000000.bin"));
    // Geometry and partition information of the whole disk.
    CHECK_EQ(nt_ioctl(h, 0x70000, 8), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kInfo), uint32_t(kHddDiskBytes / 512));
    CHECK_EQ(rd32(g_scratch + kInfo + 4), 512u);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 8u);
    CHECK_EQ(nt_ioctl(h, 0x74004, 16), nt::kSuccess);
    CHECK_EQ(rd64(g_scratch + kInfo), 0ull);
    CHECK_EQ(rd64(g_scratch + kInfo + 8), kHddDiskBytes);
    CHECK_EQ(nt_ioctl(h, 0x74004, 15), kBufferTooSmall);
    {
        bool fatal = false;
        CAPTURE_FATAL(nt_ioctl(h, 0x2D1080, 0x20), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED && g_fatal_msg.find("disk_ioctl_code") != std::string::npos);
        // The SysExt / compatibility / data partitions are not provided. Called on the device itself:
        // the trap must not leave a file lock behind in this process.
        std::shared_ptr<GuestFile> file;
        CHECK_ST(r->handles.lookup_as<GuestFile>(h, &file), Status::Ok);
        CHECK(file && file->block_device());
        uint8_t sector[512];
        if (file && file->block_device())
            CAPTURE_FATAL(file->block_device()->read(0x130EB0000ull, sector, 512), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED && g_fatal_msg.find("Partition0") != std::string::npos);
    }
    // FileAlignmentInformation of a device handle (the XDK secure cache asks for it).
    {
        PPCContext c{};
        c.r3.u64 = h; c.r4.u64 = g_scratch + kIosb; c.r5.u64 = g_scratch + kInfo; c.r6.u64 = 4; c.r7.u64 = 17;
        __imp__NtQueryInformationFile(c, g_base);
        CHECK_EQ(c.r3.u32, nt::kSuccess);
    }
    CHECK_EQ(nt_close(h), nt::kSuccess);
    // A device does not grow: a write across its end is refused.
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0", kRw, 1, 0x28, &h), nt::kSuccess);
    CHECK_EQ(nt_transfer(__imp__NtWriteFile, h, kHddCache0Bytes - 512, 1024), nt::kInvalidParameter);
    CHECK_EQ(nt_transfer(__imp__NtReadFile, h, kHddCache0Bytes, 512), nt::kEndOfFile);
    CHECK_EQ(nt_close(h), nt::kSuccess);
    // A raw device is not a directory and cannot be created.
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0", kRw, 2, 0x20, &h), nt::kObjectNameCollision);

    // ---- utility partition 0: unformatted, then the XDK format ----------------------------------
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0\\", 0x100001, 1, 0x800021, &h), kUnrecognizedVolume);
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0\\x.bin", kRw, 5, 0x60, &h), kUnrecognizedVolume);
    {
        FILE* f = fopen((hdd + "/cache0/stale.bin").c_str(), "wb");  // left over from an older file system
        CHECK(f != nullptr);
        if (f) { fputs("old", f); fclose(f); }
    }
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0", 0x100003, 1, 0x18, &h), nt::kSuccess);
    CHECK_EQ(nt_ioctl(h, 0x70000, 8), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kInfo), uint32_t(kHddCache0Bytes / 512));
    CHECK_EQ(rd32(g_scratch + kInfo + 4), 512u);
    CHECK_EQ(nt_ioctl(h, 0x74004, 16), nt::kSuccess);
    CHECK_EQ(rd64(g_scratch + kInfo), kHddCache0Offset);
    CHECK_EQ(rd64(g_scratch + kInfo + 8), kHddCache0Bytes);
    CHECK_EQ(nt_volume_size(h), kUnrecognizedVolume);  // no file system yet
    xdk_format(h, kHddCache0Bytes, 0x01D2C0DEu);
    CHECK(!host_exists(hdd + "/cache0/stale.bin"));  // the format emptied the file system
    CHECK_EQ(nt_close(h), nt::kSuccess);
    // The same bytes through Partition0.
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Partition0", 0x80000000u, 1, 0x20, &h), nt::kSuccess);
    CHECK_EQ(nt_transfer(__imp__NtReadFile, h, kHddCache0Offset, 512), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kBuffer), 0x58544146u);
    CHECK_EQ(rd32(g_scratch + kBuffer + 4), 0x01D2C0DEu);
    CHECK_EQ(nt_close(h), nt::kSuccess);

    // ---- the FATX view: root directory, volume size, files ---------------------------------------
    const uint64_t total0 = fatx_total_clusters(kHddCache0Bytes);
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0\\", 0x100001, 1, 0x800021, &h), nt::kSuccess);
    CHECK_EQ(nt_volume_size(h), nt::kSuccess);
    CHECK_EQ(rd64(g_scratch + kInfo), total0);
    CHECK(rd64(g_scratch + kInfo + 8) <= total0 - 1);  // the root directory takes one cluster
    CHECK_EQ(rd32(g_scratch + kInfo + 16), kCluster / 512);
    CHECK_EQ(rd32(g_scratch + kInfo + 20), 512u);
    CHECK_EQ(nt_close(h), nt::kSuccess);
    // A raw volume handle answers for the file system on it.
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0", kRw, 1, 0x28, &h), nt::kSuccess);
    CHECK_EQ(nt_volume_size(h), nt::kSuccess);
    CHECK_EQ(rd64(g_scratch + kInfo), total0);
    CHECK_EQ(nt_close(h), nt::kSuccess);
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0\\test.dat", 0x40000000u, 5, 0x60, &h), nt::kSuccess);
    memcpy(g_base + g_scratch + kBuffer, "hello", 5);
    CHECK_EQ(nt_transfer(__imp__NtWriteFile, h, 0, 5), nt::kSuccess);
    CHECK_EQ(nt_close(h), nt::kSuccess);
    CHECK(read_host(hdd + "/cache0/test.dat") == std::string("hello"));
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0\\" + std::string(43, 'n'), kRw, 5, 0x60, &h), kObjectNameInvalid);
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0\\", kRw, 2, 0x21, &h), nt::kObjectNameCollision);
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0\\", 0x100001, 1, 0x60, &h), nt::kFileIsADirectory);

    // ---- a title device "\Device\cache1": the XDK secure cache of utility partition 1 ------------
    uint32_t device = 0;
    CHECK_EQ(create_device("\\Device\\cache1", &device), nt::kSuccess);
    CHECK_EQ(create_link("\\??\\cache1:", "\\Device\\cache1"), nt::kSuccess);
    CHECK_EQ(nt_create("cache1:\\upload_queue", kRw, 5, 0x60, &h), kUnrecognizedVolume);  // not formatted
    // Formatted through Partition0 this time: the format is seen whatever the device used.
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Partition0", kRw, 1, 0x20, &h), nt::kSuccess);
    {
        uint8_t* buf = g_base + g_scratch + kBuffer;
        memset(buf, 0xFF, 0x1000);
        put_be32(buf, 0x58544146u); put_be32(buf + 4, 7); put_be32(buf + 8, kCluster / 512); put_be32(buf + 12, 1);
        CHECK_EQ(nt_transfer(__imp__NtWriteFile, h, kHddCache1Offset, 0x1000), nt::kSuccess);
    }
    CHECK_EQ(nt_close(h), nt::kSuccess);
    CHECK_EQ(nt_create("cache1:\\", 0x100001, 1, 0x800021, &h), nt::kSuccess);
    CHECK_EQ(nt_volume_size(h), nt::kSuccess);
    CHECK_EQ(rd64(g_scratch + kInfo), fatx_total_clusters(kHddCache1Bytes));
    CHECK_EQ(nt_close(h), nt::kSuccess);
    CHECK_EQ(nt_create("cache1:\\upload_queue", kRw, 5, 0x60, &h), nt::kSuccess);
    memcpy(g_base + g_scratch + kBuffer, "queue", 5);
    CHECK_EQ(nt_transfer(__imp__NtWriteFile, h, 0, 5), nt::kSuccess);
    CHECK_EQ(nt_close(h), nt::kSuccess);
    CHECK(read_host(hdd + "/cache1/upload_queue") == std::string("queue"));
    // Only the XDK's names are utility partitions.
    uint32_t other = 0;
    CHECK_EQ(create_device("\\Device\\cache2", &other), nt::kSuccess);
    CHECK_EQ(nt_create("\\Device\\cache2\\x", kRw, 5, 0x60, &h), kObjectNameInvalid);
    CHECK_EQ(nt_create("\\Device\\cache0\\x", kRw, 5, 0x60, &h), kObjectNameInvalid);  // no such device yet
    {
        PPCContext c{};
        c.r3.u64 = device;
        __imp__IoDeleteDevice(c, g_base);
    }
    CHECK_EQ(nt_create("cache1:\\upload_queue", kRw, 1, 0x60, &h), kObjectNameInvalid);  // device gone
    // Unformatting (superblock destroyed): the file system no longer opens; the files stay.
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache1", kRw, 1, 0x28, &h2), nt::kSuccess);
    memset(g_base + g_scratch + kBuffer, 0, 0x1000);
    CHECK_EQ(nt_transfer(__imp__NtWriteFile, h2, 0, 0x1000), nt::kSuccess);
    CHECK_EQ(nt_close(h2), nt::kSuccess);
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache1\\upload_queue", kRw, 1, 0x60, &h), kUnrecognizedVolume);
    CHECK(host_exists(hdd + "/cache1/upload_queue"));

    // A regular file has no device behind it.
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0\\test.dat", 0x80000000u, 1, 0x60, &h), nt::kSuccess);
    CHECK_EQ(nt_ioctl(h, 0x70000, 8), kInvalidDeviceRequest);
    CHECK_EQ(nt_close(h), nt::kSuccess);

    // ---- persistence: a new runtime lifetime on the same directory ----------------------------
    runtime_shutdown();
    start_runtime(mem);
    r = runtime();
    (void)r;
    CHECK(hdd_root() == std::string());  // the drive belongs to a runtime lifetime
    CHECK_ST(runtime_configure_hdd(hdd), Status::Ok);
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\partition0", kRw, 1, 0x22, &h), nt::kSuccess);
    CHECK_EQ(nt_transfer(__imp__NtReadFile, h, 2048, 1024), nt::kSuccess);
    bool same = true;
    for (uint32_t i = 0; i < 1024; ++i) same = same && g_base[g_scratch + kBuffer + i] == uint8_t(i * 13 + 1);
    CHECK(same);
    CHECK_EQ(nt_close(h), nt::kSuccess);
    CHECK_EQ(nt_create("\\Device\\Harddisk0\\Cache0\\test.dat", 0x80000000u, 1, 0x60, &h), nt::kSuccess);
    CHECK_EQ(nt_transfer(__imp__NtReadFile, h, 0, 5), nt::kSuccess);
    CHECK(memcmp(g_base + g_scratch + kBuffer, "hello", 5) == 0);
    CHECK_EQ(nt_close(h), nt::kSuccess);

    runtime_shutdown();
    clear_imports();
    return test_result("rt_hdd");
}
