// The local profile, content packages (saves) and profile settings over a temporary host save root.
// Original synthetic XEX metadata (title id 545407F2); no game data.
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/modules.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/vfs.h"
#include "rcomp/runtime/xam.h"
#include "rcomp/runtime/xam_content.h"
#include "rcomp/runtime/xam_profile.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__XamUserGetSigninState);
PPC_EXTERN_FUNC(__imp__XamUserGetXUID);
PPC_EXTERN_FUNC(__imp__XamUserGetName);
PPC_EXTERN_FUNC(__imp__XamUserGetSigninInfo);
PPC_EXTERN_FUNC(__imp__XamUserCheckPrivilege);
PPC_EXTERN_FUNC(__imp__XamContentCreateEx);
PPC_EXTERN_FUNC(__imp__XamContentClose);
PPC_EXTERN_FUNC(__imp__XamContentGetCreator);
PPC_EXTERN_FUNC(__imp__XamContentSetThumbnail);
PPC_EXTERN_FUNC(__imp__XamContentGetDeviceData);
PPC_EXTERN_FUNC(__imp__XamShowDeviceSelectorUI);
PPC_EXTERN_FUNC(__imp__XamContentCreateEnumerator);
PPC_EXTERN_FUNC(__imp__XamEnumerate);
PPC_EXTERN_FUNC(__imp__XamUserReadProfileSettings);
PPC_EXTERN_FUNC(__imp__XamUserWriteProfileSettings);
PPC_EXTERN_FUNC(__imp__XamUserCreateAchievementEnumerator);
PPC_EXTERN_FUNC(__imp__XamContentDelete);
PPC_EXTERN_FUNC(__imp__XamContentFlush);
PPC_EXTERN_FUNC(__imp__XamContentGetDeviceState);

using namespace rcomp;
using namespace rcomp::rt;

namespace {
uint8_t* g_base;
uint32_t g_stack;  // guest r1 for calls with stack arguments
constexpr uint32_t kImageBase = 0x82000000, kImageSize = 0x3000, kImageEntry = kImageBase + 0x1000;
constexpr uint32_t kHeaderSize = 0x1000;

uint32_t rd32(uint32_t a) {
    uint32_t v = 0;
    guest_read_be32(a, &v);
    return v;
}
void wr32(uint32_t a, uint32_t v) { guest_write_be32(a, v); }
uint8_t* host(uint32_t a, uint32_t n) { return runtime()->mem->translate(a, n); }

// Calls with r3..r10 and, when `stack9` is used, the ninth argument in the caller's stack slot.
uint32_t call(PPCFunc* f, std::vector<uint64_t> regs, uint32_t stack9 = 0) {
    alignas(64) PPCContext ctx{};
    regs.resize(8, 0);
    ctx.r3.u64 = regs[0]; ctx.r4.u64 = regs[1]; ctx.r5.u64 = regs[2]; ctx.r6.u64 = regs[3];
    ctx.r7.u64 = regs[4]; ctx.r8.u64 = regs[5]; ctx.r9.u64 = regs[6]; ctx.r10.u64 = regs[7];
    ctx.r1.u64 = g_stack;
    wr32(g_stack + 0x54, stack9);
    f(ctx, g_base);
    return ctx.r3.u32;
}

void put_be32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    b[o] = uint8_t(v >> 24); b[o + 1] = uint8_t(v >> 16); b[o + 2] = uint8_t(v >> 8); b[o + 3] = uint8_t(v);
}
void put_le16(std::vector<uint8_t>& b, size_t o, uint16_t v) { b[o] = uint8_t(v); b[o + 1] = uint8_t(v >> 8); }
void put_le32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + i] = uint8_t(v >> (8 * i));
}

XexImage synthetic_image(GuestMemory& mem) {
    std::vector<uint8_t> image(kImageSize);
    put_le16(image, 0, 0x5A4D);
    put_le32(image, 0x3C, 0x80);
    const size_t nt = 0x80, optional = nt + 24;
    put_le32(image, nt, 0x4550);
    put_le16(image, nt + 4, 0x1F2);
    put_le16(image, nt + 6, 1);
    put_le16(image, nt + 20, 224);
    put_le16(image, optional, 0x10B);
    put_le32(image, optional + 16, 0x1000);
    put_le32(image, optional + 28, kImageBase);
    put_le32(image, optional + 32, 0x1000);
    put_le32(image, optional + 36, 0x200);
    put_le32(image, optional + 56, kImageSize);
    put_le32(image, optional + 60, 0x200);
    put_le32(image, optional + 92, 16);
    const size_t section = optional + 224;
    put_le32(image, section + 8, 0x1000);
    put_le32(image, section + 12, 0x1000);
    put_le32(image, section + 16, 0x1000);
    put_le32(image, section + 36, 0x60000020);
    CHECK_ST(load_raw_image(mem, kImageBase, image.data(), image.size()), Status::Ok);
    std::vector<uint8_t> header(kHeaderSize);
    put_be32(header, 0, 0x58455832);
    put_be32(header, 8, kHeaderSize);
    put_be32(header, 16, 0x400);
    put_be32(header, 20, 3);
    const uint32_t fields[][2] = {{0x00010100, kImageEntry}, {0x00010201, kImageBase}, {0x00040006, 0x100}};
    for (size_t i = 0; i < 3; ++i) {
        put_be32(header, 24 + i * 8, fields[i][0]);
        put_be32(header, 28 + i * 8, fields[i][1]);
    }
    put_be32(header, 0x100, 0x4A53F9F6);
    put_be32(header, 0x104, 6);
    put_be32(header, 0x108, 5);
    put_be32(header, 0x10C, 0x545407F2);
    put_be32(header, 0x404, kImageSize);
    put_be32(header, 0x510, kImageBase);
    XexImage result{};
    result.base = kImageBase;
    result.size = kImageSize;
    result.entry_point = kImageEntry;
    result.header = std::move(header);
    return result;
}

// XCONTENT_DATA at `a`: device 1, the given type, a UTF-16BE display name and an ANSI file name.
void content_data(uint32_t a, uint32_t type, const char* display, const char* file) {
    memset(host(a, kXContentDataBytes), 0, kXContentDataBytes);
    wr32(a, kContentDeviceHdd);
    wr32(a + 4, type);
    for (size_t i = 0; display[i]; ++i) host(a + 8 + 2 * uint32_t(i) + 1, 1)[0] = uint8_t(display[i]);
    memcpy(host(a + 0x108, 42), file, strlen(file));
}
void ansi(uint32_t a, const char* s) { memcpy(host(a, uint32_t(strlen(s) + 1)), s, strlen(s) + 1); }

bool host_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

uint32_t create(uint32_t root, uint32_t data, uint32_t flags, uint32_t disposition, uint32_t overlapped = 0) {
    return call(__imp__XamContentCreateEx, {0, root, data, flags, disposition, 0, 0, 0}, overlapped);
}

std::string write_file(const char* guest_path, const char* text) {
    uint32_t handle = 0;
    OpenRequest request;
    request.write_access = true;
    request.disposition = OpenDisposition::OverwriteIf;
    const Status s = runtime()->vfs.open(runtime()->handles, guest_path, request, &handle);
    if (s != Status::Ok) return status_name(s);
    std::shared_ptr<GuestFile> file;
    CHECK_ST(runtime()->handles.lookup_as<GuestFile>(handle, &file), Status::Ok);
    uint32_t written = 0;
    CHECK_ST(file->write(text, uint32_t(strlen(text)), &written), Status::Ok);
    file.reset();
    CHECK_ST(runtime()->handles.close(handle), Status::Ok);
    return "Ok";
}

std::string read_file(const char* guest_path) {
    uint32_t handle = 0;
    if (runtime()->vfs.open(runtime()->handles, guest_path, false, &handle) != Status::Ok) return "<missing>";
    std::shared_ptr<GuestFile> file;
    CHECK_ST(runtime()->handles.lookup_as<GuestFile>(handle, &file), Status::Ok);
    char buffer[64] = {};
    uint32_t got = 0;
    file->read(buffer, sizeof buffer - 1, &got);
    file.reset();
    CHECK_ST(runtime()->handles.close(handle), Status::Ok);
    return std::string(buffer, got);
}
}  // namespace

int main() {
    GuestMemory mem;
    if (mem.reserve() != MemStatus::Ok) return 2;
    g_base = mem.base();
    CHECK_ST(runtime_init(&mem), Status::Ok);
    CHECK_ST(register_xam_hle(), Status::Ok);
    CHECK_ST(runtime_prepare_main_module({"game:\\Original.xex", "Original.xex"}), Status::Ok);
    XexImage image = synthetic_image(mem);
    CHECK_ST(runtime_finalize_main_module(image), Status::Ok);
    uint32_t buf = 0, stack = 0;
    CHECK_ST(runtime()->heap.alloc(0x4000, 16, true, &buf), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x200, 16, true, &stack), Status::Ok);
    g_stack = stack;

    char temp[] = "/tmp/rcomp_xam_content_XXXXXX";
    CHECK(mkdtemp(temp) != nullptr);
    const std::string root = std::string(temp) + "/savedata";

    // ---- the local profile -------------------------------------------------------------------------
    CHECK_EQ(call(__imp__XamUserGetSigninState, {0}), kSigninStateSignedInLocally);
    CHECK_EQ(call(__imp__XamUserGetSigninState, {1}), kSigninStateNotSignedIn);
    CHECK_EQ(call(__imp__XamUserGetXUID, {0, 1, buf}), 0u);
    CHECK_EQ(rd32(buf), 0xE0000000u);
    CHECK_EQ(rd32(buf + 4), 0x52434F4Du);
    CHECK_EQ(call(__imp__XamUserGetXUID, {0, 2, buf}), 0x80070525u);  // no online XUID
    CHECK_EQ(call(__imp__XamUserGetXUID, {2, 1, buf}), 0x80070525u);
    CHECK_EQ(call(__imp__XamUserGetName, {0, buf, 16}), 0u);
    CHECK(!strcmp(reinterpret_cast<const char*>(host(buf, 16)), "Player"));
    CHECK_EQ(call(__imp__XamUserGetName, {0, buf, 3}), 0u);
    CHECK(!strcmp(reinterpret_cast<const char*>(host(buf, 3)), "Pl"));
    CHECK_EQ(call(__imp__XamUserGetSigninInfo, {0, 0, buf}), 0u);
    CHECK_EQ(rd32(buf + 4), 0x52434F4Du);
    CHECK_EQ(rd32(buf + 0x0C), 1u);
    CHECK(!strcmp(reinterpret_cast<const char*>(host(buf + 0x18, 16)), "Player"));
    wr32(buf + 0x40, 7);
    CHECK_EQ(call(__imp__XamUserCheckPrivilege, {0, 0xFC, buf + 0x40}), 0u);
    CHECK_EQ(rd32(buf + 0x40), 0u);  // LIVE privileges are not held

    // ---- no save root: no storage device ---------------------------------------------------------
    const uint32_t root_name = buf + 0x100, data = buf + 0x200, out = buf + 0x400, ov = buf + 0x440;
    ansi(root_name, "save");
    content_data(data, kContentTypeSavedGame, "Save 1", "SAVE01");
    CHECK_EQ(create(root_name, data, 4, out), 0x48Fu);
    CHECK_EQ(call(__imp__XamContentGetDeviceData, {1, out}), 0x48Fu);

    CHECK_ST(runtime_configure_save_root(root), Status::Ok);
    CHECK_ST(runtime_configure_save_root(root + "/"), Status::Ok);
    CHECK_ST(runtime_configure_save_root(std::string(temp) + "/other"), Status::Conflict);

    // ---- devices ---------------------------------------------------------------------------------
    CHECK_EQ(call(__imp__XamContentGetDeviceData, {1, out}), 0u);
    CHECK_EQ(rd32(out), 1u);
    CHECK_EQ(rd32(out + 4), 1u);
    CHECK(rd32(out + 8) || rd32(out + 12));  // total bytes from statvfs
    CHECK_EQ(call(__imp__XamContentGetDeviceData, {2, out}), 0x48Fu);
    wr32(out, 0);
    CHECK_EQ(call(__imp__XamShowDeviceSelectorUI, {0, 1, 0, 0x10000, out, 0}), 0u);
    CHECK_EQ(rd32(out), 1u);
    memset(host(ov, 0x1C), 0, 0x1C);
    CHECK_EQ(call(__imp__XamShowDeviceSelectorUI, {0, 1, 0, 0x10000, out, ov}), 0x3E5u);
    CHECK_EQ(rd32(ov), 0u);

    // ---- create, write, close, reopen --------------------------------------------------------------
    CHECK_EQ(create(root_name, data, 3, out), 3u);  // OPEN_EXISTING on nothing
    CHECK_EQ(create(root_name, data, 1, out), 0u);  // CREATE_NEW
    CHECK_EQ(rd32(out), 1u);
    const std::string type_dir = root + "/E000000052434F4D/545407F2/00000001";
    CHECK(host_exists(type_dir + "/SAVE01"));
    CHECK(host_exists(type_dir + "/SAVE01.xcontent"));
    CHECK(write_file("save:\\slot.bin", "progress") == "Ok");
    CHECK_EQ(create(root_name, data, 4, out), 0xB7u);  // root name already open
    CHECK_EQ(call(__imp__XamContentClose, {root_name, 0}), 0u);
    CHECK_EQ(call(__imp__XamContentClose, {root_name, 0}), 2u);
    CHECK(read_file("save:\\slot.bin") == "<missing>");  // unmounted
    CHECK_EQ(create(root_name, data, 1, out), 0xB7u);  // CREATE_NEW on an existing package
    content_data(data, kContentTypeSavedGame, "Save 1", "save01");  // case-insensitive lookup
    CHECK_EQ(create(root_name, data, 3, out, ov), 0x3E5u);
    CHECK_EQ(rd32(ov), 0u);
    CHECK_EQ(rd32(out), 2u);  // opened
    CHECK(read_file("save:\\slot.bin") == "progress");
    CHECK_EQ(call(__imp__XamContentClose, {root_name, 0}), 0u);

    // ---- creator, thumbnail ----------------------------------------------------------------------
    CHECK_EQ(call(__imp__XamContentGetCreator, {0, data, out, out + 8, 0}), 0u);
    CHECK_EQ(rd32(out), 1u);
    CHECK_EQ(rd32(out + 8), 0xE0000000u);
    ansi(buf + 0x600, "PNGDATA");
    CHECK_EQ(call(__imp__XamContentSetThumbnail, {0, data, buf + 0x600, 7, 0}), 0u);
    CHECK(host_exists(type_dir + "/SAVE01.thumbnail"));

    // ---- enumeration, after a simulated restart ----------------------------------------------------
    reset_xam_content();
    CHECK_ST(runtime_configure_save_root(root), Status::Ok);
    content_data(data, kContentTypeSavedGame, "Save 2", "SAVE02");
    CHECK_EQ(create(root_name, data, 2, out), 0u);  // CREATE_ALWAYS
    CHECK_EQ(call(__imp__XamContentClose, {root_name, 0}), 0u);
    CHECK_EQ(call(__imp__XamContentCreateEnumerator, {0, 1, kContentTypeSavedGame, 0, 1, out, out + 4}), 0u);
    CHECK_EQ(rd32(out), kXContentDataBytes);
    const uint32_t handle = rd32(out + 4), items = buf + 0x1000;
    CHECK_EQ(call(__imp__XamEnumerate, {handle, 0, items, kXContentDataBytes, out + 8, 0}), 0u);
    CHECK_EQ(rd32(out + 8), 1u);
    CHECK(!strcmp(reinterpret_cast<const char*>(host(items + 0x108, 42)), "SAVE01"));
    CHECK_EQ(host(items + 8 + 1, 1)[0], uint8_t('S'));
    CHECK_EQ(call(__imp__XamEnumerate, {handle, 0, items, kXContentDataBytes, out + 8, ov}), 0x3E5u);
    CHECK_EQ(rd32(ov), 0u);
    CHECK_EQ(rd32(ov + 4), 1u);  // InternalHigh = items
    CHECK(!strcmp(reinterpret_cast<const char*>(host(items + 0x108, 42)), "SAVE02"));
    CHECK_EQ(call(__imp__XamEnumerate, {handle, 0, items, kXContentDataBytes, out + 8, 0}), 0x12u);
    CHECK_ST(runtime()->handles.close(handle), Status::Ok);
    CHECK_EQ(call(__imp__XamContentCreateEnumerator, {1, 1, kContentTypeSavedGame, 0, 4, out, out + 4}), 0u);
    CHECK_EQ(call(__imp__XamEnumerate, {rd32(out + 4), 0, items, 4 * kXContentDataBytes, out + 8, 0}), 0x12u);

    // ---- TRUNCATE_EXISTING empties the package; bad names are refused -------------------------------
    content_data(data, kContentTypeSavedGame, "Save 1", "SAVE01");
    CHECK_EQ(create(root_name, data, 5, out), 0u);
    CHECK(read_file("save:\\slot.bin") == "<missing>");
    CHECK_EQ(call(__imp__XamContentClose, {root_name, 0}), 0u);
    content_data(data, kContentTypeSavedGame, "Bad", "..");
    CHECK_EQ(create(root_name, data, 4, out), 0x57u);
    content_data(data, kContentTypeSavedGame, "Bad", "a/b");
    CHECK_EQ(create(root_name, data, 4, out), 0x57u);
    ansi(root_name, "game");
    content_data(data, kContentTypeSavedGame, "Save 1", "SAVE01");
    CHECK_EQ(create(root_name, data, 4, out), 0xB7u);  // the game device cannot be replaced

    // ---- flush, device state, delete ------------------------------------------------------------------
    ansi(root_name, "save");
    content_data(data, kContentTypeSavedGame, "Save 3", "SAVE03");
    CHECK_EQ(create(root_name, data, 1, out), 0u);
    CHECK(write_file("save:\\gone.bin", "x") == "Ok");
    CHECK_EQ(call(__imp__XamContentFlush, {root_name, 0}), 0u);
    CHECK_EQ(call(__imp__XamContentDelete, {0, data, 0}), 0x20u);  // in use while mounted
    CHECK_EQ(call(__imp__XamContentClose, {root_name, 0}), 0u);
    CHECK_EQ(call(__imp__XamContentFlush, {root_name, 0}), 2u);    // nothing open under that name
    CHECK_EQ(call(__imp__XamContentGetDeviceState, {1, 0}), 0u);
    CHECK_EQ(call(__imp__XamContentGetDeviceState, {2, 0}), 0x48Fu);
    CHECK_EQ(call(__imp__XamContentDelete, {0, data, ov}), 0x3E5u);
    CHECK_EQ(rd32(ov), 0u);
    CHECK(!host_exists(type_dir + "/SAVE03"));
    CHECK(!host_exists(type_dir + "/SAVE03.xcontent"));
    CHECK_EQ(call(__imp__XamContentDelete, {0, data, 0}), 2u);

    // ---- profile settings --------------------------------------------------------------------------
    const uint32_t ids = buf + 0x2000, size = buf + 0x2040, results = buf + 0x2100;
    wr32(ids, 0x63E83FFF);  // XPROFILE_TITLE_SPECIFIC1
    wr32(ids + 4, 0x10040003);  // XPROFILE_OPTION_CONTROLLER_VIBRATION
    wr32(size, 0);
    CHECK_EQ(call(__imp__XamUserReadProfileSettings, {0, 0, 0, 0, 2, ids, size, 0}, 0), 0x7Au);
    CHECK_EQ(rd32(size), 8u + 2 * 40u);
    CHECK_EQ(call(__imp__XamUserReadProfileSettings, {0, 0, 0, 0, 2, ids, size, results}, 0), 0u);
    CHECK_EQ(rd32(results), 2u);
    CHECK_EQ(rd32(results + 4), results + 8);
    CHECK_EQ(rd32(results + 8), 0u);                 // title-specific never written: no value
    CHECK_EQ(rd32(results + 8 + 0x10), 0x63E83FFFu);
    CHECK_EQ(host(results + 8 + 0x18, 1)[0], uint8_t(6));
    CHECK_EQ(rd32(results + 8 + 0x20), 0u);
    CHECK_EQ(rd32(results + 48), 1u);                // vibration: console default
    CHECK_EQ(rd32(results + 48 + 0x20), 3u);
    // Write five bytes into TITLE_SPECIFIC1, then read them back (source TITLE).
    const uint32_t setting = buf + 0x2400, blob = buf + 0x2500;
    memset(host(setting, 40), 0, 40);
    wr32(setting + 0x10, 0x63E83FFF);
    host(setting + 0x18, 1)[0] = 6;
    wr32(setting + 0x20, 5);
    wr32(setting + 0x24, blob);
    ansi(blob, "OPTS!");
    CHECK_EQ(call(__imp__XamUserWriteProfileSettings, {0, 0, 1, setting, 0}), 0u);
    wr32(size, 0x100);
    CHECK_EQ(call(__imp__XamUserReadProfileSettings, {0, 0, 0, 0, 1, ids, size, results}, ov), 0x3E5u);
    CHECK_EQ(rd32(ov), 0u);
    CHECK_EQ(rd32(results + 8), 2u);
    CHECK_EQ(rd32(results + 8 + 0x20), 5u);
    CHECK(!memcmp(host(rd32(results + 8 + 0x24), 5), "OPTS!", 5));
    CHECK(host_exists(root + "/E000000052434F4D/545407F2/profile/63E83FFF.setting"));
    // A type that contradicts the id, an unknown id and another user are refused.
    host(setting + 0x18, 1)[0] = 1;
    CHECK_EQ(call(__imp__XamUserWriteProfileSettings, {0, 0, 1, setting, 0}), 0x57u);
    wr32(ids, 0x10049999);
    CHECK_EQ(call(__imp__XamUserReadProfileSettings, {0, 0, 0, 0, 1, ids, size, results}, 0), 0x57u);
    wr32(ids, 0x63E83FFF);
    CHECK_EQ(call(__imp__XamUserReadProfileSettings, {0, 1, 0, 0, 1, ids, size, results}, 0), 0x525u);

    // ---- achievements: valid and empty for the local player ---------------------------------------------
    CHECK_EQ(call(__imp__XamUserCreateAchievementEnumerator, {0, 0, 0, 0, 0, 8, out, out + 4}), 0u);
    CHECK_EQ(rd32(out), 8u * 36u);
    CHECK_EQ(call(__imp__XamEnumerate, {rd32(out + 4), 0, items, 8 * 36, out + 8, 0}), 0x12u);
    CHECK_EQ(call(__imp__XamUserCreateAchievementEnumerator, {0, 1, 0, 0, 0, 8, out, out + 4}), 0x525u);

    runtime_shutdown();
    clear_imports();
    mem.release();
    std::string cleanup = std::string("rm -rf ") + temp;
    CHECK_EQ(system(cleanup.c_str()), 0);
    return test_result("rt_xam_content");
}
