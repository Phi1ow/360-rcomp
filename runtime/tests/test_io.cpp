#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xboxkrnl_io.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__NtFlushBuffersFile);
PPC_EXTERN_FUNC(__imp__NtQueryFullAttributesFile);
PPC_EXTERN_FUNC(__imp__NtQueryInformationFile);
PPC_EXTERN_FUNC(__imp__NtQueryVolumeInformationFile);
PPC_EXTERN_FUNC(__imp__NtSetInformationFile);
PPC_EXTERN_FUNC(__imp__NtWriteFile);
PPC_EXTERN_FUNC(__imp__NtCreateFile);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtSetEvent);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);

using namespace rcomp;
using namespace rcomp::rt;

namespace {

constexpr uint32_t kInvalidInfoClass = 0xC0000003u;
constexpr uint32_t kInfoLengthMismatch = 0xC0000004u;
constexpr uint32_t kNotSupported = 0xC00000BBu;

uint8_t* g_base;
uint32_t g_scratch;

enum : uint32_t {
    kIosb = 0x00,
    kOut = 0x40,
    kAttrs = 0x100,
    kAnsi = 0x110,
    kName = 0x120,
    kEventCell = 0x180,
    kOffset = 0x190,
    kTimeout = 0x1A0,
    kSetInfo = 0x200,
    kHandleCell = 0x260,
    kStack = 0x400,
};

uint32_t g_data;

bool write_file(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

std::string read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[128];
    while (size_t n = fread(buf, 1, sizeof buf, f)) out.append(buf, n);
    fclose(f);
    return out;
}

uint32_t rd32(uint32_t address) {
    uint32_t v = 0;
    CHECK(guest_read_be32(address, &v));
    return v;
}

uint64_t rd64(uint32_t address) {
    uint64_t v = 0;
    CHECK(guest_read_be64(address, &v));
    return v;
}

void fill(uint32_t offset, uint8_t byte, uint32_t count) {
    memset(g_base + g_scratch + offset, byte, count);
}

void set_name(const std::string& name) {
    memcpy(g_base + g_scratch + kName, name.data(), name.size());
    const uint16_t len = __builtin_bswap16((uint16_t)name.size());
    memcpy(g_base + g_scratch + kAnsi, &len, 2);
    memcpy(g_base + g_scratch + kAnsi + 2, &len, 2);
    CHECK(guest_write_be32(g_scratch + kAnsi + 4, g_scratch + kName));
    CHECK(guest_write_be32(g_scratch + kAttrs + 0, 0xFFFFFFFDu));
    CHECK(guest_write_be32(g_scratch + kAttrs + 4, g_scratch + kAnsi));
    CHECK(guest_write_be32(g_scratch + kAttrs + 8, 0x40));
}

uint32_t query_info(PPCContext& c, uint32_t handle, uint32_t cls, uint32_t length) {
    c.r3.u64 = handle;
    c.r4.u64 = g_scratch + kIosb;
    c.r5.u64 = g_scratch + kOut;
    c.r6.u64 = length;
    c.r7.u64 = cls;
    __imp__NtQueryInformationFile(c, g_base);
    return c.r3.u32;
}

uint32_t query_volume(PPCContext& c, uint32_t handle, uint32_t cls, uint32_t length) {
    c.r3.u64 = handle;
    c.r4.u64 = g_scratch + kIosb;
    c.r5.u64 = g_scratch + kOut;
    c.r6.u64 = length;
    c.r7.u64 = cls;
    __imp__NtQueryVolumeInformationFile(c, g_base);
    return c.r3.u32;
}

uint32_t set_info(PPCContext& c, uint32_t handle, uint32_t cls, uint32_t length,
                  uint32_t info = 0) {
    c = {};
    c.r3.u64 = handle;
    c.r4.u64 = g_scratch + kIosb;
    c.r5.u64 = info ? info : g_scratch + kSetInfo;
    c.r6.u64 = length;
    c.r7.u64 = cls;
    __imp__NtSetInformationFile(c, g_base);
    return c.r3.u32;
}

uint32_t write_hle(PPCContext& c, uint32_t handle, uint32_t event,
                   uint32_t data, uint32_t length, uint32_t offset_ptr = 0,
                   uint32_t iosb = 0) {
    c = {};
    c.r3.u64 = handle;
    c.r4.u64 = event;
    c.r6.u64 = 0x12345678;
    c.r7.u64 = iosb ? iosb : g_scratch + kIosb;
    c.r8.u64 = data;
    c.r9.u64 = length;
    c.r10.u64 = offset_ptr;
    __imp__NtWriteFile(c, g_base);
    return c.r3.u32;
}

uint32_t poll_event(uint32_t event) {
    PPCContext c{};
    c.r3.u64 = event;
    c.r6.u64 = g_scratch + kTimeout;
    __imp__NtWaitForSingleObjectEx(c, g_base);
    return c.r3.u32;
}

void signal_event(uint32_t event) {
    PPCContext c{};
    c.r3.u64 = event;
    __imp__NtSetEvent(c, g_base);
    CHECK_EQ(c.r3.u32, nt::kSuccess);
}

uint32_t create_file_hle(PPCContext& c, const std::string& path,
                         uint32_t access, uint32_t disposition,
                         uint32_t options, uint32_t file_attributes = 0x80) {
    set_name(path);
    CHECK(guest_write_be32(g_scratch + kHandleCell, 0xA5A5A5A5u));
    fill(kIosb, 0xCC, 8);
    CHECK(guest_write_be32(g_scratch + kStack + 0x54, options));
    c = {};
    c.r1.u64 = g_scratch + kStack;
    c.r3.u64 = g_scratch + kHandleCell;
    c.r4.u64 = access;
    c.r5.u64 = g_scratch + kAttrs;
    c.r6.u64 = g_scratch + kIosb;
    c.r7.u64 = 0;
    c.r8.u64 = file_attributes;
    c.r9.u64 = 3;
    c.r10.u64 = disposition;
    __imp__NtCreateFile(c, g_base);
    return c.r3.u32;
}

struct TestObj : HandleObject {
    static constexpr HandleKind kKind = HandleKind::Test;
    HandleKind kind() const override { return kKind; }
};

}  // namespace

int main() {
    char tmpl[] = "/tmp/rcomp_rt_io_XXXXXX";
    std::string top = mkdtemp(tmpl) ? tmpl : "";
    if (top.empty()) return 2;
    const std::string game = top + "/game";
    const std::string mixed = game + "/Mixed";
    const std::string save = top + "/save";
    const std::string save_sub = save + "/Sub";
    const std::string outside_dir = top + "/outside";
    CHECK(mkdir(game.c_str(), 0755) == 0);
    CHECK(mkdir(mixed.c_str(), 0755) == 0);
    CHECK(mkdir(save.c_str(), 0755) == 0);
    CHECK(mkdir(save_sub.c_str(), 0755) == 0);
    CHECK(mkdir(outside_dir.c_str(), 0755) == 0);
    const std::string payload = "IO-METADATA-0123456789";
    const std::string asset = mixed + "/Asset.BIN";
    CHECK(write_file(asset, payload));
    CHECK(write_file(save + "/slot.dat", "SAVE"));
    CHECK(write_file(top + "/outside.bin", "OUTSIDE"));
    CHECK(symlink((top + "/outside.bin").c_str(), (mixed + "/Escape.BIN").c_str()) == 0);
    CHECK(symlink(outside_dir.c_str(), (save + "/LinkDir").c_str()) == 0);

    GuestMemory mem;
    if (mem.reserve() != MemStatus::Ok) return 2;
    g_base = mem.base();
    CHECK_ST(runtime_init(&mem), Status::Ok);
    Runtime& r = *runtime();
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(r.heap.alloc(0x1000, 16, true, &g_scratch), Status::Ok);
    CHECK_ST(r.heap.alloc(0x10000, 0x10000, true, &g_data), Status::Ok);
    CHECK(guest_write_be64(g_scratch + kTimeout, 0));

    // Explicit media policy: game is never writable, save must opt in.
    CHECK_ST(r.vfs.mount("game", game, MountAccess::ReadWrite), Status::AccessDenied);
    CHECK_ST(r.vfs.mount("d", game, MountAccess::ReadWrite), Status::AccessDenied);
    CHECK_ST(r.vfs.mount("game", game), Status::Ok);
    CHECK_ST(r.vfs.mount("save", save, MountAccess::ReadWrite), Status::Ok);

    uint32_t game_handle = 0, save_handle = 0;
    CHECK_ST(r.vfs.open(r.handles, "GAME:\\mixed\\asset.bin", false, &game_handle), Status::Ok);
    CHECK_ST(r.vfs.open(r.handles, "game:\\mixed\\asset.bin", true, &save_handle),
             Status::AccessDenied);
    CHECK_ST(r.vfs.open(r.handles, "save:\\SLOT.DAT", true, &save_handle), Status::Ok);
    CHECK_EQ(r.handles.live_count(), 2u);
    std::shared_ptr<GuestFile> save_file;
    CHECK_ST(r.handles.lookup_as<GuestFile>(save_handle, &save_file), Status::Ok);
    CHECK(save_file->writable());
    CHECK_ST(save_file->flush(), Status::Ok);
    CHECK(read_file(save + "/slot.dat") == "SAVE");
    CHECK_ST(r.handles.close(save_handle), Status::Ok);
    CHECK_EQ(r.handles.live_count(), 1u);

    // Full production open/create dispositions on an explicitly writable save
    // mount. Case-folded existing parents stay confined.
    OpenRequest create_request;
    create_request.write_access = true;
    create_request.disposition = OpenDisposition::Create;
    create_request.write_through = true;
    OpenAction action = OpenAction::Opened;
    uint32_t created_handle = 0;
    CHECK_ST(r.vfs.open(r.handles, "save:\\SUB\\created.dat", create_request,
                        &created_handle, &action),
             Status::Ok);
    CHECK(action == OpenAction::Created);
    std::shared_ptr<GuestFile> created_file;
    CHECK_ST(r.handles.lookup_as<GuestFile>(created_handle, &created_file), Status::Ok);
    uint32_t direct_written = 0;
    CHECK_ST(created_file->write("abc", 3, &direct_written), Status::Ok);
    CHECK_EQ(direct_written, 3u);
    CHECK(read_file(save_sub + "/created.dat") == "abc");
    // A write-through handle syncs after every mutation (and the VFS counts its fsync calls); closing it adds none.
    const uint64_t syncs_before = vfs_fsync_count();
    CHECK_ST(created_file->write("def", 3, &direct_written), Status::Ok);
    CHECK_ST(created_file->write("ghi", 3, &direct_written), Status::Ok);
    CHECK(read_file(save_sub + "/created.dat") == "abcdefghi");
    CHECK_EQ(vfs_fsync_count(), syncs_before + 2);
    CHECK_ST(r.handles.close(created_handle), Status::Ok);
    created_file.reset();
    CHECK_EQ(vfs_fsync_count(), syncs_before + 2);

    OpenRequest open_if = create_request;
    open_if.disposition = OpenDisposition::OpenIf;
    CHECK_ST(r.vfs.open(r.handles, "SAVE:\\sub\\CREATED.DAT", open_if,
                        &created_handle, &action),
             Status::Ok);
    CHECK(action == OpenAction::Opened);
    CHECK_ST(r.handles.close(created_handle), Status::Ok);

    OpenRequest overwrite_if = create_request;
    overwrite_if.disposition = OpenDisposition::OverwriteIf;
    CHECK_ST(r.vfs.open(r.handles, "save:\\Sub\\created.dat", overwrite_if,
                        &created_handle, &action),
             Status::Ok);
    CHECK(action == OpenAction::Overwritten);
    CHECK_ST(r.handles.lookup_as<GuestFile>(created_handle, &created_file), Status::Ok);
    FileMetadata created_meta{};
    CHECK_ST(created_file->metadata(&created_meta), Status::Ok);
    CHECK_EQ(created_meta.end_of_file, 0ull);
    // A write is one fsync on a write-through handle and an explicit flush is one more.
    const uint64_t flushes_before = vfs_fsync_count();
    CHECK_ST(created_file->write("x", 1, &direct_written), Status::Ok);
    CHECK_EQ(vfs_fsync_count(), flushes_before + 1);
    CHECK_ST(created_file->flush(), Status::Ok);
    CHECK_EQ(vfs_fsync_count(), flushes_before + 2);
    CHECK_ST(r.handles.close(created_handle), Status::Ok);
    created_file.reset();
    CHECK_EQ(vfs_fsync_count(), flushes_before + 2);

    CHECK_ST(r.vfs.open(r.handles, "game:\\new.bin", create_request,
                        &created_handle, &action),
             Status::AccessDenied);
    CHECK_ST(r.vfs.open(r.handles, "save:\\LinkDir\\escape.dat", create_request,
                        &created_handle, &action),
             Status::PathRejected);

    OpenRequest directory_create;
    directory_create.disposition = OpenDisposition::Create;
    directory_create.directory = true;
    CHECK_ST(r.vfs.open(r.handles, "save:\\NewDir", directory_create,
                        &created_handle, &action),
             Status::Ok);
    CHECK(action == OpenAction::Created);
    CHECK_ST(r.handles.lookup_as<GuestFile>(created_handle, &created_file), Status::Ok);
    CHECK(created_file->directory());
    CHECK_ST(r.handles.close(created_handle), Status::Ok);
    created_file.reset();

    // PRIME-owned NtCreateFile hook drives the same production VFS contract.
    // Exercise the exact dispositions/actions GTA uses instead of only testing
    // the C++ VFS overload directly.
    PPCContext c{};
    const std::string hle_created_guest = "save:\\Sub\\hle-created.dat";
    CHECK_EQ(create_file_hle(c, hle_created_guest, 0x40000000u, 2, 0x22),
             nt::kSuccess);
    const uint32_t hle_created_handle = rd32(g_scratch + kHandleCell);
    CHECK_EQ(rd32(g_scratch + kIosb), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 2u);  // FILE_CREATED
    memcpy(g_base + g_data, "HLE", 3);
    CHECK_EQ(write_hle(c, hle_created_handle, 0, g_data, 3), nt::kSuccess);
    CHECK(read_file(save_sub + "/hle-created.dat") == "HLE");
    CHECK_ST(r.handles.close(hle_created_handle), Status::Ok);

    CHECK_EQ(create_file_hle(c, hle_created_guest, 0x40000000u, 2, 0x22),
             nt::kObjectNameCollision);
    CHECK_EQ(rd32(g_scratch + kHandleCell), 0xA5A5A5A5u);
    CHECK_EQ(rd32(g_scratch + kIosb), nt::kObjectNameCollision);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 0u);

    CHECK_EQ(create_file_hle(c, hle_created_guest, 0x40000000u, 5, 0x20),
             nt::kSuccess);
    const uint32_t hle_overwrite_handle = rd32(g_scratch + kHandleCell);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 3u);  // FILE_OVERWRITTEN
    std::shared_ptr<GuestFile> hle_overwrite_file;
    CHECK_ST(r.handles.lookup_as<GuestFile>(hle_overwrite_handle, &hle_overwrite_file),
             Status::Ok);
    FileMetadata hle_overwrite_meta{};
    CHECK_ST(hle_overwrite_file->metadata(&hle_overwrite_meta), Status::Ok);
    CHECK_EQ(hle_overwrite_meta.end_of_file, 0ull);
    CHECK_ST(r.handles.close(hle_overwrite_handle), Status::Ok);
    hle_overwrite_file.reset();

    // GTA also opens regular files with NO_INTERMEDIATE_BUFFERING |
    // SYNCHRONOUS_IO_NONALERT (0x28). Host Cygwin enforces alignment only;
    // PS5 additionally receives O_DIRECT.
    CHECK_EQ(create_file_hle(c, hle_created_guest, 0x40000000u, 1, 0x28),
             nt::kSuccess);
    const uint32_t hle_direct_handle = rd32(g_scratch + kHandleCell);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 1u);  // FILE_OPENED
    CHECK_ST(r.handles.close(hle_direct_handle), Status::Ok);

    CHECK_EQ(create_file_hle(c, hle_created_guest, 0x40000000u, 1, 0x60),
             nt::kSuccess);
    const uint32_t hle_non_directory_handle = rd32(g_scratch + kHandleCell);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 1u);
    CHECK_ST(r.handles.close(hle_non_directory_handle), Status::Ok);

    CHECK_EQ(create_file_hle(c, hle_created_guest, 0x40000000u, 1, 0x41),
             nt::kInvalidParameter);
    CHECK_EQ(rd32(g_scratch + kHandleCell), 0xA5A5A5A5u);

    CHECK_EQ(create_file_hle(c, "save:\\HleDir", 0x00100001u, 2, 0x4021, 0x10),
             nt::kSuccess);
    const uint32_t hle_dir_handle = rd32(g_scratch + kHandleCell);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 2u);
    std::shared_ptr<GuestFile> hle_dir_file;
    CHECK_ST(r.handles.lookup_as<GuestFile>(hle_dir_handle, &hle_dir_file), Status::Ok);
    CHECK(hle_dir_file->directory());
    CHECK_ST(r.handles.close(hle_dir_handle), Status::Ok);
    hle_dir_file.reset();

    FileMetadata direct{};
    CHECK_ST(r.vfs.query("game:\\MIXED\\ASSET.bin", &direct), Status::Ok);
    CHECK_EQ(direct.end_of_file, (uint64_t)payload.size());
    CHECK_EQ(direct.allocation_size, 0x200ull);
    CHECK_EQ(direct.attributes, 0x81u);
    CHECK_ST(r.vfs.query("game:\\mixed\\escape.bin", &direct), Status::PathRejected);

    std::shared_ptr<GuestFile> game_file;
    CHECK_ST(r.handles.lookup_as<GuestFile>(game_handle, &game_file), Status::Ok);
    char prefix[5] = {};
    uint32_t got = 0;
    CHECK_ST(game_file->read(prefix, sizeof prefix, &got), Status::Ok);
    CHECK_EQ(got, 5u);
    CHECK(memcmp(prefix, payload.data(), 5) == 0);

    fill(kOut, 0xA5, 128);
    fill(kIosb, 0xCC, 8);
    CHECK_EQ(query_info(c, game_handle, 14, 8), nt::kSuccess);
    const uint8_t expected_position[8] = {0, 0, 0, 0, 0, 0, 0, 5};
    CHECK(memcmp(g_base + g_scratch + kOut, expected_position, 8) == 0);
    CHECK_EQ(rd32(g_scratch + kIosb), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 8u);

    fill(kOut, 0xA5, 128);
    CHECK_EQ(query_info(c, game_handle, 4, 40), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kOut + 32), 0x81u);
    CHECK_EQ(rd32(g_scratch + kOut + 36), 0u);

    fill(kOut, 0xA5, 128);
    CHECK_EQ(query_info(c, game_handle, 5, 24), nt::kSuccess);
    CHECK_EQ(rd64(g_scratch + kOut + 0), 0x200ull);
    CHECK_EQ(rd64(g_scratch + kOut + 8), (uint64_t)payload.size());
    CHECK_EQ(rd32(g_scratch + kOut + 16), 1u);
    CHECK_EQ(g_base[g_scratch + kOut + 20], 0u);
    CHECK_EQ(g_base[g_scratch + kOut + 21], 0u);

    fill(kOut, 0xA5, 128);
    CHECK_EQ(query_info(c, game_handle, 19, 64), nt::kSuccess);
    CHECK_EQ(rd64(g_scratch + kOut), 0x200ull);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 8u);
    CHECK_EQ(g_base[g_scratch + kOut + 8], 0xA5u);

    fill(kOut, 0xA5, 128);
    CHECK_EQ(query_info(c, game_handle, 34, 56), nt::kSuccess);
    CHECK_EQ(rd64(g_scratch + kOut + 32), 0x200ull);
    CHECK_EQ(rd64(g_scratch + kOut + 40), (uint64_t)payload.size());
    CHECK_EQ(rd32(g_scratch + kOut + 48), 0x81u);
    CHECK_EQ(rd32(g_scratch + kOut + 52), 0u);

    // Recognized but unsupported classes are explicit and leave data untouched.
    fill(kOut, 0xA5, 64);
    CHECK_EQ(query_info(c, game_handle, 9, 64), kNotSupported);
    CHECK_EQ(rd32(g_scratch + kIosb), kNotSupported);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 0u);
    for (unsigned i = 0; i < 64; ++i) CHECK_EQ(g_base[g_scratch + kOut + i], 0xA5u);
    fill(kOut, 0xA6, 64);
    CHECK_EQ(query_info(c, game_handle, 99, 64), kInvalidInfoClass);
    for (unsigned i = 0; i < 64; ++i) CHECK_EQ(g_base[g_scratch + kOut + i], 0xA6u);
    fill(kOut, 0xA7, 64);
    CHECK_EQ(query_info(c, game_handle, 34, 8), kInfoLengthMismatch);
    for (unsigned i = 0; i < 64; ++i) CHECK_EQ(g_base[g_scratch + kOut + i], 0xA7u);

    // Path metadata uses the same confined, case-insensitive resolver.
    set_name("GAME:\\MIXED\\asset.bin");
    fill(kOut, 0xA5, 56);
    c.r3.u64 = g_scratch + kAttrs;
    c.r4.u64 = g_scratch + kOut;
    __imp__NtQueryFullAttributesFile(c, g_base);
    CHECK_EQ(c.r3.u32, nt::kSuccess);
    CHECK_EQ(rd64(g_scratch + kOut + 32), 0x200ull);
    CHECK_EQ(rd64(g_scratch + kOut + 40), (uint64_t)payload.size());
    CHECK_EQ(rd32(g_scratch + kOut + 48), 0x81u);
    set_name("game:\\Mixed\\missing.bin");
    fill(kOut, 0xA8, 56);
    c.r3.u64 = g_scratch + kAttrs;
    c.r4.u64 = g_scratch + kOut;
    __imp__NtQueryFullAttributesFile(c, g_base);
    CHECK_EQ(c.r3.u32, nt::kObjectNameNotFound);
    for (unsigned i = 0; i < 56; ++i) CHECK_EQ(g_base[g_scratch + kOut + i], 0xA8u);

    // An arbitrary host directory has no truthful Xbox volume identity.
    fill(kOut, 0xA4, 24);
    CHECK_EQ(query_volume(c, game_handle, 1, 24), kNotSupported);
    for (unsigned i = 0; i < 24; ++i) CHECK_EQ(g_base[g_scratch + kOut + i], 0xA4u);

    // Capacity comes from the actual descriptor's host filesystem.
    fill(kOut, 0xA5, 64);
    CHECK_EQ(query_volume(c, game_handle, 3, 24), nt::kSuccess);
    CHECK(rd64(g_scratch + kOut + 0) > 0);
    CHECK(rd64(g_scratch + kOut + 8) <= rd64(g_scratch + kOut + 0));
    CHECK(rd32(g_scratch + kOut + 16) > 0);
    CHECK_EQ(rd32(g_scratch + kOut + 20), 0x200u);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 24u);

    fill(kOut, 0xA5, 64);
    CHECK_EQ(query_volume(c, game_handle, 5, 17), nt::kSuccess);
    const uint8_t expected_attr[17] = {
        0,0,0,0, 0,0,0,255, 0,0,0,5, 'R','C','O','M','P'
    };
    CHECK(memcmp(g_base + g_scratch + kOut, expected_attr, sizeof expected_attr) == 0);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 17u);
    CHECK_EQ(g_base[g_scratch + kOut + 17], 0xA5u);

    fill(kOut, 0xAB, 16);
    CHECK_EQ(query_volume(c, game_handle, 5, 16), 0x80000005u);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 12u);
    CHECK_EQ(rd32(g_scratch + kOut + 8), 5u);
    for (unsigned i = 12; i < 16; ++i) CHECK_EQ(g_base[g_scratch + kOut + i], 0xABu);

    fill(kOut, 0xA9, 16);
    CHECK_EQ(query_volume(c, game_handle, 4, 16), kNotSupported);
    for (unsigned i = 0; i < 16; ++i) CHECK_EQ(g_base[g_scratch + kOut + i], 0xA9u);

    uint64_t seek_pos = 0;
    CHECK_ST(game_file->seek(INT64_MAX, SeekOrigin::Begin, &seek_pos), Status::Ok);
    CHECK_EQ(seek_pos, (uint64_t)INT64_MAX);
    CHECK_ST(game_file->seek(1, SeekOrigin::Current, &seek_pos), Status::InvalidArgument);
    CHECK_EQ(game_file->position(), (uint64_t)INT64_MAX);
    CHECK_ST(game_file->seek(5, SeekOrigin::Begin, &seek_pos), Status::Ok);

    // Production NtWriteFile: real bytes, IOSB, explicit offsets and real Event
    // completion on a writable save handle.
    OpenRequest writable_open;
    writable_open.write_access = true;
    uint32_t write_handle = 0;
    CHECK_ST(r.vfs.open(r.handles, "save:\\slot.dat", writable_open,
                        &write_handle, &action),
             Status::Ok);
    std::shared_ptr<GuestFile> write_file_object;
    CHECK_ST(r.handles.lookup_as<GuestFile>(write_handle, &write_file_object), Status::Ok);

    PPCContext event_context{};
    event_context.r3.u64 = g_scratch + kEventCell;
    event_context.r5.u64 = 1;
    __imp__NtCreateEvent(event_context, g_base);
    CHECK_EQ(event_context.r3.u32, nt::kSuccess);
    const uint32_t event_handle = rd32(g_scratch + kEventCell);
    CHECK_EQ(poll_event(event_handle), nt::kTimeout);

    memcpy(g_base + g_data, "HELLO", 5);
    CHECK(mem.protect(g_data, 0x10000, Protect::Read) == MemStatus::Ok);
    fill(kIosb, 0xCC, 8);
    CHECK_EQ(write_hle(c, write_handle, event_handle, g_data, 5), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kIosb), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 5u);
    CHECK(read_file(save + "/slot.dat") == "HELLO");
    CHECK_EQ(write_file_object->position(), 5ull);
    CHECK_EQ(poll_event(event_handle), nt::kSuccess);
    CHECK_EQ(poll_event(event_handle), nt::kTimeout);
    CHECK(mem.protect(g_data, 0x10000, Protect::ReadWrite) == MemStatus::Ok);

    // A guest fault cannot reset a pre-signalled Event or publish a partial IOSB.
    signal_event(event_handle);
    fill(kIosb, 0xA5, 8);
    CHECK(mem.protect(g_data, 0x10000, Protect::None) == MemStatus::Ok);
    CHECK_EQ(write_hle(c, write_handle, event_handle, g_data, 1), nt::kAccessViolation);
    for (unsigned i = 0; i < 8; ++i) CHECK_EQ(g_base[g_scratch + kIosb + i], 0xA5u);
    CHECK_EQ(poll_event(event_handle), nt::kSuccess);
    CHECK(read_file(save + "/slot.dat") == "HELLO");
    CHECK(mem.protect(g_data, 0x10000, Protect::ReadWrite) == MemStatus::Ok);

    g_base[g_data] = 'X';
    CHECK(guest_write_be64(g_scratch + kOffset, 2));
    CHECK_EQ(write_hle(c, write_handle, 0, g_data, 1, g_scratch + kOffset),
             nt::kSuccess);
    CHECK(read_file(save + "/slot.dat") == "HEXLO");
    CHECK_EQ(write_file_object->position(), 3ull);
    g_base[g_data] = '!';
    CHECK(guest_write_be64(g_scratch + kOffset, UINT64_MAX));
    CHECK_EQ(write_hle(c, write_handle, 0, g_data, 1, g_scratch + kOffset),
             nt::kSuccess);
    CHECK(read_file(save + "/slot.dat") == "HEXLO!");

    fill(kIosb, 0xA6, 8);
    CHECK(guest_write_be64(g_scratch + kOffset, UINT64_MAX - 2));
    CHECK_EQ(write_hle(c, write_handle, 0, g_data, 1, g_scratch + kOffset),
             nt::kInvalidParameter);
    for (unsigned i = 0; i < 8; ++i) CHECK_EQ(g_base[g_scratch + kIosb + i], 0xA6u);

    fill(kIosb, 0xCC, 8);
    CHECK_EQ(write_hle(c, write_handle, game_handle, g_data, 1),
             nt::kObjectTypeMismatch);
    CHECK_EQ(rd32(g_scratch + kIosb), nt::kObjectTypeMismatch);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 0u);
    CHECK(read_file(save + "/slot.dat") == "HEXLO!");

    fill(kIosb, 0xCC, 8);
    signal_event(event_handle);
    CHECK_EQ(write_hle(c, game_handle, event_handle, g_data, 1), nt::kAccessDenied);
    CHECK_EQ(rd32(g_scratch + kIosb), nt::kAccessDenied);
    CHECK(read_file(asset) == payload);
    CHECK_EQ(poll_event(event_handle), nt::kSuccess);
    CHECK_EQ(poll_event(event_handle), nt::kTimeout);

    // Set Position is handle state and works on read-only files.
    memset(g_base + g_scratch + kSetInfo, 0, 64);
    CHECK(guest_write_be64(g_scratch + kSetInfo, 2));
    CHECK_EQ(set_info(c, game_handle, 14, 8), nt::kSuccess);
    CHECK_EQ(game_file->position(), 2ull);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 8u);

    // EOF is a real ftruncate on writable handles; Allocation stays explicit
    // unsupported because PS5 has no backed allocation primitive.
    CHECK(guest_write_be64(g_scratch + kSetInfo, 3));
    CHECK_EQ(set_info(c, write_handle, 20, 8), nt::kSuccess);
    CHECK(read_file(save + "/slot.dat") == "HEX");
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 8u);
    CHECK(guest_write_be64(g_scratch + kSetInfo, 0x2000));
    CHECK_EQ(set_info(c, write_handle, 19, 8), kNotSupported);
    CHECK(read_file(save + "/slot.dat") == "HEX");
    CHECK_EQ(set_info(c, game_handle, 20, 8), nt::kAccessDenied);

    // GTA's observed BasicInformation use is backed: zero means unchanged,
    // LastWriteTime uses futimes, and attributes update real host write bits
    // while preserving the Xbox attribute value on the open file object.
    constexpr uint64_t kTestWriteTime = 116444736000000000ull + 12345ull * 10000000ull;
    memset(g_base + g_scratch + kSetInfo, 0, 40);
    CHECK(guest_write_be64(g_scratch + kSetInfo + 16, kTestWriteTime));
    CHECK_EQ(set_info(c, write_handle, 4, 40), nt::kSuccess);
    FileMetadata save_meta{};
    CHECK_ST(write_file_object->metadata(&save_meta), Status::Ok);
    CHECK_EQ(save_meta.last_write_time, kTestWriteTime);
    memset(g_base + g_scratch + kSetInfo, 0, 40);
    CHECK(guest_write_be32(g_scratch + kSetInfo + 32, 0x81));
    CHECK_EQ(set_info(c, write_handle, 4, 40), nt::kSuccess);
    CHECK_ST(write_file_object->metadata(&save_meta), Status::Ok);
    CHECK_EQ(save_meta.attributes, 0x81u);
    CHECK_ST(r.vfs.query("save:\\SLOT.DAT", &save_meta), Status::Ok);
    CHECK_EQ(save_meta.attributes, 0x81u);
    uint32_t second_save_handle = 0;
    CHECK_ST(r.vfs.open(r.handles, "save:\\slot.dat", false, &second_save_handle),
             Status::Ok);
    std::shared_ptr<GuestFile> second_save_file;
    CHECK_ST(r.handles.lookup_as<GuestFile>(second_save_handle, &second_save_file),
             Status::Ok);
    CHECK_ST(second_save_file->metadata(&save_meta), Status::Ok);
    CHECK_EQ(save_meta.attributes, 0x81u);
    CHECK_ST(r.handles.close(second_save_handle), Status::Ok);
    second_save_file.reset();
    CHECK(guest_write_be32(g_scratch + kSetInfo + 32, 0x80));
    CHECK_EQ(set_info(c, write_handle, 4, 40), nt::kSuccess);
    memset(g_base + g_scratch + kSetInfo, 0, 40);
    CHECK(guest_write_be64(g_scratch + kSetInfo + 0, kTestWriteTime));
    CHECK_EQ(set_info(c, write_handle, 4, 40), kNotSupported);

    // Rename uses the Xbox 16-byte structure and stays inside the same RW
    // mount. Collisions honor ReplaceIfExists.
    const std::string renamed_guest = "save:\\Sub\\renamed.dat";
    memcpy(g_base + g_data, renamed_guest.data(), renamed_guest.size());
    memset(g_base + g_scratch + kSetInfo, 0, 16);
    CHECK(guest_write_be32(g_scratch + kSetInfo + 4, 0xFFFFFFFDu));
    const uint16_t rename_len_be = __builtin_bswap16((uint16_t)renamed_guest.size());
    memcpy(g_base + g_scratch + kSetInfo + 8, &rename_len_be, 2);
    memcpy(g_base + g_scratch + kSetInfo + 10, &rename_len_be, 2);
    CHECK(guest_write_be32(g_scratch + kSetInfo + 12, g_data));
    CHECK_EQ(set_info(c, write_handle, 10, 16), nt::kSuccess);
    CHECK(read_file(save_sub + "/renamed.dat") == "HEX");
    CHECK(access((save + "/slot.dat").c_str(), F_OK) != 0);
    CHECK(write_file_object->guest_path() == renamed_guest);
    CHECK_ST(r.vfs.query("save:\\sub\\RENAMED.DAT", &save_meta), Status::Ok);
    CHECK_EQ(save_meta.attributes, 0x80u);

    CHECK(write_file(save_sub + "/collision.dat", "COLLIDE"));
    const std::string collision_guest = "save:\\Sub\\collision.dat";
    memcpy(g_base + g_data, collision_guest.data(), collision_guest.size());
    const uint16_t collision_len_be = __builtin_bswap16((uint16_t)collision_guest.size());
    memcpy(g_base + g_scratch + kSetInfo + 8, &collision_len_be, 2);
    memcpy(g_base + g_scratch + kSetInfo + 10, &collision_len_be, 2);
    CHECK_EQ(set_info(c, write_handle, 10, 16), nt::kObjectNameCollision);
    CHECK(read_file(save_sub + "/renamed.dat") == "HEX");
    CHECK(guest_write_be32(g_scratch + kSetInfo + 0, 1));
    CHECK_EQ(set_info(c, write_handle, 10, 16), nt::kSuccess);
    CHECK(read_file(save_sub + "/collision.dat") == "HEX");
    CHECK(access((save_sub + "/renamed.dat").c_str(), F_OK) != 0);

    // Delete-on-close is real and only available on writable file handles.
    memset(g_base + g_scratch + kSetInfo, 0, 1);
    g_base[g_scratch + kSetInfo] = 1;
    CHECK_EQ(set_info(c, game_handle, 13, 1), nt::kAccessDenied);
    CHECK(access(asset.c_str(), F_OK) == 0);
    CHECK_EQ(set_info(c, write_handle, 13, 1), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 0u);
    CHECK_ST(r.handles.close(write_handle), Status::Ok);
    write_file_object.reset();
    CHECK(access((save_sub + "/collision.dat").c_str(), F_OK) != 0);
    CHECK_ST(r.handles.close(event_handle), Status::Ok);

    // Flush validates the real file object; read-only has no dirty runtime data.
    fill(kIosb, 0xCC, 8);
    c.r3.u64 = game_handle;
    c.r4.u64 = g_scratch + kIosb;
    __imp__NtFlushBuffersFile(c, g_base);
    CHECK_EQ(c.r3.u32, nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kIosb), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 0u);
    CHECK(read_file(asset) == payload);

    uint32_t wrong = 0;
    CHECK_ST(r.handles.insert(std::make_shared<TestObj>(), &wrong), Status::Ok);
    c.r3.u64 = wrong;
    __imp__NtFlushBuffersFile(c, g_base);
    CHECK_EQ(c.r3.u32, nt::kObjectTypeMismatch);
    CHECK_ST(r.handles.close(wrong), Status::Ok);
    CHECK_ST(r.handles.close(game_handle), Status::Ok);
    CHECK_EQ(r.handles.live_count(), 0u);
    fill(kOut, 0xAA, 8);
    CHECK_EQ(query_info(c, game_handle, 14, 8), nt::kInvalidHandle);
    for (unsigned i = 0; i < 8; ++i) CHECK_EQ(g_base[g_scratch + kOut + i], 0xAAu);

    runtime_shutdown();
    unlink((mixed + "/Escape.BIN").c_str());
    unlink((save + "/LinkDir").c_str());
    unlink(asset.c_str());
    unlink((save + "/slot.dat").c_str());
    unlink((save_sub + "/created.dat").c_str());
    unlink((save_sub + "/hle-created.dat").c_str());
    unlink((top + "/outside.bin").c_str());
    rmdir((save + "/NewDir").c_str());
    rmdir((save + "/HleDir").c_str());
    rmdir(save_sub.c_str());
    rmdir(outside_dir.c_str());
    rmdir(mixed.c_str());
    rmdir(game.c_str());
    rmdir(save.c_str());
    rmdir(top.c_str());
    return test_result("rt_test_io");
}
