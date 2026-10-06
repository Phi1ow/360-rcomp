// Profile settings of the local player: XamUserReadProfileSettings (0x219) and XamUserWriteProfileSettings
// (0x21A). See rcomp/runtime/xam_profile.h for the profile and xam_content.h for where settings persist.
//
// A setting id encodes its type in bits 28-31 (1 INT32, 2 INT64, 3 DOUBLE, 4 UNICODE, 5 FLOAT, 6 BINARY,
// 7 DATETIME) and its maximum size in bits 16-27. XUSER_PROFILE_SETTING is 40 bytes: source +0, user index
// (or XUID when XUIDs are passed) +8, setting id +0x10, XUSER_DATA +0x18 (type byte +0x18, value +0x20;
// UNICODE/BINARY: byte count +0x20, pointer +0x24). The read buffer starts with XUSER_READ_PROFILE_SETTING_RESULT
// {count, pointer to the settings} followed by the settings and their UNICODE/BINARY payloads.
// Layouts and the default values follow Xenia's xam_user.cc / user_profile.cc (behavioural reference).
//
// Values: what the title wrote (persisted, source TITLE = 2), else the console's default for the known
// system settings (source DEFAULT = 1), else, for a title-specific setting never written, no value
// (source 0, empty binary). An id outside these is ERROR_INVALID_PARAMETER, as on the console. Another
// player's settings would need Xbox LIVE: they are returned with no value.
//
// XamWriteGamerTile (0x2F0, XUserAwardGamerPicture) copies a gamer picture from the title's SPA into the
// local profile (layout below, before its implementation).
#include "rcomp/runtime/xam_profile.h"

#include <stdio.h>
#include <string.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam.h"
#include "rcomp/runtime/xam_content.h"
#include "rcomp/runtime/xam_enum.h"
#include "module_state.h"
#include "xam_storage.h"

namespace rcomp::rt {
namespace {
using namespace storage;

constexpr uint32_t kErrorSuccess = 0, kErrorAccessDenied = 5, kErrorInvalidParameter = 0x57,
                   kErrorInsufficientBuffer = 0x7A, kErrorNoSuchUser = 0x525, kErrorIoPending = 0x3E5,
                   kErrorDeviceNotConnected = 0x48F, kErrorNotFound = 0x490, kErrorFunctionFailed = 0x65B;
constexpr uint32_t kHresultInvalidArgument = 0x80070057u;
constexpr uint32_t kAwardFlag = 1;  // XUserAwardGamerPicture's XamWriteGamerTile flag
constexpr uint32_t kOverlappedBytes = 0x1C, kSettingBytes = 40, kHeaderBytes = 8, kMaxSettings = 32;
constexpr uint32_t kUserAny = 0xFF;
constexpr uint32_t kSourceNoValue = 0, kSourceDefault = 1, kSourceTitle = 2;
constexpr uint8_t kTypeInt32 = 1, kTypeInt64 = 2, kTypeDouble = 3, kTypeUnicode = 4, kTypeFloat = 5,
                  kTypeBinary = 6, kTypeDateTime = 7;
constexpr uint32_t kTitleSpecific1 = 0x63E83FFF, kTitleSpecific2 = 0x63E83FFE, kTitleSpecific3 = 0x63E83FFD;

uint8_t setting_type(uint32_t id) { return uint8_t(id >> 28); }
uint32_t setting_max_size(uint32_t id) { return (id >> 16) & 0xFFF; }
bool title_specific(uint32_t id) { return id == kTitleSpecific1 || id == kTitleSpecific2 || id == kTitleSpecific3; }
bool variable_size(uint8_t type) { return type == kTypeUnicode || type == kTypeBinary; }

// A value: fixed types keep their 8 value bytes (big-endian, as at +0x20); UNICODE/BINARY their payload.
struct Value {
    uint8_t type = 0;
    std::vector<uint8_t> bytes;
};

// Console defaults of the system settings a title may read (values from Xenia's user_profile.cc).
bool default_value(uint32_t id, Value* out) {
    struct Int { uint32_t id; uint32_t value; };
    static constexpr Int ints[] = {
        {0x10040002, 0},           // XPROFILE_GAMER_YAXIS_INVERSION
        {0x10040003, 3},           // XPROFILE_OPTION_CONTROLLER_VIBRATION (on)
        {0x10040004, 0},           // XPROFILE_GAMERCARD_ZONE
        {0x10040005, 0},           // XPROFILE_GAMERCARD_REGION
        {0x10040006, 0xFA},        // XPROFILE_GAMERCARD_CRED
        {0x1004000C, 0},           // XPROFILE_OPTION_VOICE_MUTED
        {0x1004000D, 0},           // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
        {0x1004000E, 0x64},        // XPROFILE_OPTION_VOICE_VOLUME
        {0x10040012, 1},           // XPROFILE_GAMERCARD_TITLES_PLAYED
        {0x10040013, 0},           // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
        {0x10040015, 0},           // XPROFILE_GAMER_DIFFICULTY
        {0x10040018, 0},           // XPROFILE_GAMER_CONTROL_SENSITIVITY
        {0x1004001D, 0xFFFF0000u}, // XPROFILE_GAMER_PREFERRED_COLOR_FIRST
        {0x1004001E, 0xFF00FF00u}, // XPROFILE_GAMER_PREFERRED_COLOR_SECOND
        {0x10040022, 1},           // XPROFILE_GAMER_ACTION_AUTO_AIM
        {0x10040023, 0},           // XPROFILE_GAMER_ACTION_AUTO_CENTER
        {0x10040024, 0},           // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
        {0x10040026, 0},           // XPROFILE_GAMER_RACE_TRANSMISSION
        {0x10040027, 0},           // XPROFILE_GAMER_RACE_CAMERA_LOCATION
        {0x10040028, 0},           // XPROFILE_GAMER_RACE_BRAKE_CONTROL
        {0x10040029, 0},           // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL
        {0x10040038, 0},           // XPROFILE_GAMERCARD_TITLE_CRED_EARNED
        {0x10040039, 0},           // XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED
    };
    for (const Int& i : ints) {
        if (i.id != id) continue;
        out->type = kTypeInt32;
        out->bytes.assign(8, 0);
        put_be32(out->bytes.data(), i.value);
        return true;
    }
    if (id == 0x5004000B) {  // XPROFILE_GAMERCARD_REP: 0.0f
        out->type = kTypeFloat;
        out->bytes.assign(8, 0);
        return true;
    }
    if (id == 0x402C0011 || id == 0x4064000F) {  // XPROFILE_GAMERCARD_MOTTO, _PICTURE_KEY: empty strings
        out->type = kTypeUnicode;
        out->bytes.assign(2, 0);  // UTF-16 NUL
        return true;
    }
    return false;
}

std::mutex g_mutex;

std::string setting_path(uint32_t title_id, uint32_t id) {
    const std::string root = save_root();
    if (root.empty()) return {};
    return root + "/" + hex(kLocalUserXuid, 16) + "/" + hex(title_id, 8) + "/profile/" + hex(id, 8) + ".setting";
}

// A value the title wrote, if any. Stored as: type byte, 3 zero bytes, then the value bytes.
bool stored_value(uint32_t title_id, uint32_t id, Value* out) {
    const std::string path = setting_path(title_id, id);
    std::vector<uint8_t> raw;
    if (path.empty() || !read_file(path, &raw, 4 + 0xFFF) || raw.size() < 4 || raw[0] != setting_type(id)) return false;
    out->type = raw[0];
    out->bytes.assign(raw.begin() + 4, raw.end());
    if (!variable_size(out->type) && out->bytes.size() != 8) return false;
    return true;
}

Runtime& current(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xam.xex!%s before runtime_init", fn);
    return *r;
}

bool accessible(Runtime& r, uint32_t address, uint32_t size, Protect p) {
    return address && r.mem->is_accessible(address, size, p);
}

void finish(PPCContext& ctx, uint32_t overlapped, uint32_t result) {
    if (overlapped) {
        xam_complete_overlapped(overlapped, result, 0);
        ctx.r3.u64 = kErrorIoPending;
    } else {
        ctx.r3.u64 = result;
    }
}

struct Entry {
    uint32_t source = kSourceNoValue;
    bool by_xuid = false;
    uint64_t xuid = 0;
    uint32_t user_index = 0;
    uint32_t id = 0;
    Value value;
};

// XamUserReadProfileSettings(title_id, user_index, xuid_count, xuids, setting_count, setting_ids,
// buffer_size*, buffer, overlapped (stack)).
void XamUserReadProfileSettings(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamUserReadProfileSettings");
    uint32_t title_id = ctx.r3.u32;
    const uint32_t user_index = ctx.r4.u32, xuid_count = ctx.r5.u32, xuids = ctx.r6.u32,
                   setting_count = ctx.r7.u32, setting_ids = ctx.r8.u32, size_ptr = ctx.r9.u32,
                   buffer = ctx.r10.u32;
    const uint64_t slot = uint64_t(ctx.r1.u32) + 0x54;
    uint32_t overlapped = 0;
    if (slot > UINT32_MAX || !guest_read_be32(uint32_t(slot), &overlapped))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xam.xex!XamUserReadProfileSettings: stack argument unreadable");
    if (!setting_count || setting_count > kMaxSettings || !accessible(r, setting_ids, 4 * setting_count, Protect::Read) ||
        !accessible(r, size_ptr, 4, Protect::ReadWrite) || xuid_count > 16 ||
        (xuid_count && !accessible(r, xuids, 8 * xuid_count, Protect::Read)) ||
        (overlapped && !accessible(r, overlapped, kOverlappedBytes, Protect::ReadWrite))) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    if (!xuid_count && !is_local_user(user_index) && user_index != kUserAny) {
        ctx.r3.u64 = user_index < 4 ? kErrorNoSuchUser : kErrorInvalidParameter;
        return;
    }
    if (!title_id) title_id = xam_main_title_id("XamUserReadProfileSettings");
    std::vector<Entry> entries;
    uint32_t payload = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const uint32_t users = xuid_count ? xuid_count : 1;
        for (uint32_t u = 0; u < users; ++u) {
            uint64_t xuid = 0;
            if (xuid_count) {
                uint32_t hi = 0, lo = 0;
                guest_read_be32(xuids + 8 * u, &hi);
                guest_read_be32(xuids + 8 * u + 4, &lo);
                xuid = uint64_t(hi) << 32 | lo;
            }
            for (uint32_t i = 0; i < setting_count; ++i) {
                Entry e;
                guest_read_be32(setting_ids + 4 * i, &e.id);
                e.by_xuid = xuid_count != 0;
                e.xuid = xuid;
                e.user_index = kLocalUserIndex;
                const bool local = !xuid_count || xuid == kLocalUserXuid;
                Value v;
                if (local && stored_value(title_id, e.id, &v)) {
                    e.source = kSourceTitle;
                    e.value = std::move(v);
                } else if (default_value(e.id, &v)) {
                    e.source = local ? kSourceDefault : kSourceNoValue;
                    e.value = std::move(v);
                } else if (title_specific(e.id)) {
                    e.value.type = kTypeBinary;
                } else {
                    finish(ctx, overlapped, kErrorInvalidParameter);
                    return;
                }
                if (e.source == kSourceNoValue && variable_size(e.value.type)) e.value.bytes.clear();
                if (variable_size(e.value.type)) payload += (uint32_t(e.value.bytes.size()) + 7) & ~7u;
                entries.push_back(std::move(e));
            }
        }
    }
    const uint32_t needed = kHeaderBytes + kSettingBytes * uint32_t(entries.size()) + payload;
    uint32_t have = 0;
    guest_read_be32(size_ptr, &have);
    if (!buffer || have < needed) {
        guest_write_be32(size_ptr, needed);
        ctx.r3.u64 = kErrorInsufficientBuffer;
        return;
    }
    if (!accessible(r, buffer, needed, Protect::ReadWrite)) { ctx.r3.u64 = kErrorInvalidParameter; return; }
    std::vector<uint8_t> out(needed, 0);
    put_be32(out.data(), uint32_t(entries.size()));
    put_be32(out.data() + 4, buffer + kHeaderBytes);
    uint32_t data_offset = kHeaderBytes + kSettingBytes * uint32_t(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        uint8_t* s = out.data() + kHeaderBytes + kSettingBytes * i;
        put_be32(s, e.source);
        if (e.by_xuid) put_be64(s + 8, e.xuid);
        else put_be32(s + 8, e.user_index);
        put_be32(s + 0x10, e.id);
        s[0x18] = e.value.type;
        if (variable_size(e.value.type)) {
            const uint32_t n = uint32_t(e.value.bytes.size());
            put_be32(s + 0x20, n);
            put_be32(s + 0x24, n ? buffer + data_offset : 0);
            if (n) memcpy(out.data() + data_offset, e.value.bytes.data(), n);
            data_offset += (n + 7) & ~7u;
        } else if (!e.value.bytes.empty()) {
            memcpy(s + 0x20, e.value.bytes.data(), 8);
        }
    }
    memcpy(r.mem->translate(buffer, needed), out.data(), needed);
    finish(ctx, overlapped, kErrorSuccess);
}

// XamUserWriteProfileSettings(title_id, user_index, setting_count, settings, overlapped).
void XamUserWriteProfileSettings(PPCContext& ctx, uint8_t*) {
    Runtime& r = current("XamUserWriteProfileSettings");
    uint32_t title_id = ctx.r3.u32;
    const uint32_t user_index = ctx.r4.u32, count = ctx.r5.u32, settings = ctx.r6.u32, overlapped = ctx.r7.u32;
    if (!count || count > kMaxSettings || !accessible(r, settings, kSettingBytes * count, Protect::Read) ||
        (overlapped && !accessible(r, overlapped, kOverlappedBytes, Protect::ReadWrite))) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    if (!is_local_user(user_index)) { ctx.r3.u64 = user_index < 4 ? kErrorNoSuchUser : kErrorInvalidParameter; return; }
    if (!title_id) title_id = xam_main_title_id("XamUserWriteProfileSettings");
    // Validate every record before writing any, so a bad one leaves the profile unchanged.
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> files;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t s[kSettingBytes];
        memcpy(s, r.mem->translate(settings + kSettingBytes * i, kSettingBytes), kSettingBytes);
        const uint32_t id = be32(s + 0x10);
        const uint8_t type = s[0x18];
        Value probe;
        if (type != setting_type(id) || (!title_specific(id) && !default_value(id, &probe))) {
            finish(ctx, overlapped, kErrorInvalidParameter);
            return;
        }
        std::vector<uint8_t> file = {type, 0, 0, 0};
        if (variable_size(type)) {
            const uint32_t n = be32(s + 0x20), data = be32(s + 0x24);
            if (n > setting_max_size(id) || (n && !accessible(r, data, n, Protect::Read))) {
                finish(ctx, overlapped, kErrorInvalidParameter);
                return;
            }
            const uint8_t* p = n ? r.mem->translate(data, n) : nullptr;
            if (p) file.insert(file.end(), p, p + n);
        } else if (type == kTypeInt32 || type == kTypeInt64 || type == kTypeDouble || type == kTypeFloat ||
                   type == kTypeDateTime) {
            file.insert(file.end(), s + 0x20, s + 0x28);
        } else {
            finish(ctx, overlapped, kErrorInvalidParameter);
            return;
        }
        files.push_back({id, std::move(file)});
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& [id, file] : files) {
        const std::string path = setting_path(title_id, id);
        const size_t slash = path.rfind('/');
        if (path.empty() || !mkdir_p(path.substr(0, slash)) || !write_file_atomic(path, file.data(), file.size())) {
            finish(ctx, overlapped, kErrorAccessDenied);
            return;
        }
    }
    finish(ctx, overlapped, kErrorSuccess);
}

// ---- gamer pictures -----------------------------------------------------------------------------------
// A title's gamer pictures are PNG images in its SPA: the XEX resource named after the title id (8 hex
// digits) in the resource-info optional header (key 0x000002FF: u32 block size, then 16-byte records
// {char name[8]; u32 guest address; u32 size}; Xenia xex2_info.h / user_module.cc GetSection). The SPA is an
// XDBF database: header {'XDBF', version, entry table length, entry count, free table length, free count},
// entry table of 18-byte records {u16 namespace, u64 id, u32 offset, u32 length}, the free table (8-byte
// records), then the content the offsets are relative to. Images are namespace 2. Gears of War 2's SPA
// holds 0x10001/0x10002 (32x32) and 0x20001/0x20002 (64x64).
constexpr uint32_t kResourceInfoKey = 0x000002FF, kResourceRecordBytes = 16;
constexpr uint32_t kXdbfSignature = 0x58444246, kXdbfHeaderBytes = 24, kXdbfEntryBytes = 18;
constexpr uint32_t kXdbfFreeEntryBytes = 8;
constexpr uint16_t kXdbfImageNamespace = 2;

// Guest address and size of the running title's SPA. False when the main XEX carries none.
bool title_spa(Runtime& r, uint32_t title_id, uint32_t* address, uint32_t* size) {
    const ModuleState* m = r.modules.get();
    if (!m || !m->ready || m->generation != r.generation) return false;
    for (const auto& f : m->fields) {
        if (f.key != kResourceInfoKey) continue;
        const uint32_t block = m->header_storage + f.offset;
        if (f.size < 4 || uint64_t(f.offset) + f.size > m->header_size || !r.mem->is_accessible(block, f.size, Protect::Read))
            return false;
        const uint8_t* p = r.mem->translate(block, f.size);
        const uint32_t declared = be32(p), bytes = declared < f.size ? declared : f.size;
        const std::string name = hex(title_id, 8);
        for (uint32_t at = 4; bytes >= kResourceRecordBytes && at <= bytes - kResourceRecordBytes;
             at += kResourceRecordBytes) {
            bool match = true;
            for (size_t i = 0; i < 8 && match; ++i) {
                char c = char(p[at + i]);
                if (c >= 'a' && c <= 'f') c = char(c - 'a' + 'A');
                match = c == name[i];
            }
            if (!match) continue;
            *address = be32(p + at + 8);
            *size = be32(p + at + 12);
            return *address && r.mem->is_accessible(*address, *size, Protect::Read);
        }
        return false;
    }
    return false;
}

// Copies image `id` of the SPA at [address, address + size). False when the database is malformed or has
// no such image.
bool spa_image(Runtime& r, uint32_t address, uint32_t size, uint32_t id, std::vector<uint8_t>* out) {
    if (size < kXdbfHeaderBytes) return false;
    const uint8_t* p = r.mem->translate(address, size);
    if (!p || be32(p) != kXdbfSignature) return false;
    const uint64_t table = be32(p + 8), count = be32(p + 12), free_table = be32(p + 16);
    const uint64_t content = kXdbfHeaderBytes + table * kXdbfEntryBytes + free_table * kXdbfFreeEntryBytes;
    if (count > table || content > size) return false;
    for (uint64_t i = 0; i < count; ++i) {
        const uint8_t* e = p + kXdbfHeaderBytes + i * kXdbfEntryBytes;
        const uint16_t ns = uint16_t(e[0] << 8 | e[1]);
        if (ns != kXdbfImageNamespace || be64(e + 2) != id) continue;
        const uint64_t offset = content + be32(e + 10), length = be32(e + 14);
        if (!length || offset + length > size) return false;
        out->assign(p + offset, p + offset + length);
        return true;
    }
    return false;
}

std::string gamer_picture_path(uint32_t title_id, uint32_t image_id) {
    const std::string root = save_root();
    if (root.empty()) return {};
    return root + "/" + hex(kLocalUserXuid, 16) + "/gamertile/" + hex(title_id, 8) + "/" + hex(image_id, 8) + ".png";
}

// XamWriteGamerTile (0x02F0): (user, title id (0 = the running title), big tile id, small tile id, DWORD
// flag, PXOVERLAPPED), the import behind XUserAwardGamerPicture. ABI from Xenia Canary xam_user.cc (read only)
// and Gears of War 2's wrapper sub_82AAC160: XUserAwardGamerPicture(user, picture id, reserved,
// overlapped) passes title id 0, big = id | 0x20000, small = id | 0x10000 and flag 1 (the only value
// established; anything else stops), and its caller (sub_8282E3D0) always passes an overlapped and accepts 0
// or ERROR_IO_PENDING.
// The award copies both images of the picture from the title's SPA into the local profile:
//   <save root>/<XUID>/gamertile/<TitleID>/<ImageID>.png    (8 upper-case hex digits each)
// Awarding a picture again rewrites the same files. Results: E_INVALIDARG (an HRESULT, as Xenia) for a user
// index past 3, returned directly; then, through the overlapped (call returns ERROR_IO_PENDING) or as the
// return value: ERROR_NO_SUCH_USER for the empty slots 1-3, ERROR_DEVICE_NOT_CONNECTED without a save root
// (no storage device, as the content services), ERROR_NOT_FOUND when the SPA has no such image (R-comp's
// choice; the console's code for that title error is not established), ERROR_FUNCTION_FAILED when the files
// cannot be written, else ERROR_SUCCESS.
void XamWriteGamerTile(PPCContext& ctx, uint8_t*) {
    const char* fn = "XamWriteGamerTile";
    Runtime& r = current(fn);
    const uint32_t user = ctx.r3.u32, requested_title = ctx.r4.u32, big = ctx.r5.u32, small = ctx.r6.u32,
                   flag = ctx.r7.u32, overlapped = ctx.r8.u32;
    if (user >= 4) {
        ctx.r3.u64 = kHresultInvalidArgument;
        return;
    }
    if (flag != kAwardFlag)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xam.xex!%s flag=0x%08X not established (only 1, XUserAwardGamerPicture) lr=0x%08X",
                    fn, flag, uint32_t(ctx.lr));
    if (overlapped && !accessible(r, overlapped, kOverlappedBytes, Protect::ReadWrite)) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    const uint32_t title_id = xam_main_title_id(fn);
    if (requested_title && requested_title != title_id)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xam.xex!%s title 0x%08X: only the running title's (0x%08X) SPA is available lr=0x%08X", fn,
                    requested_title, title_id, uint32_t(ctx.lr));
    if (!is_local_user(user)) { finish(ctx, overlapped, kErrorNoSuchUser); return; }
    if (save_root().empty()) { finish(ctx, overlapped, kErrorDeviceNotConnected); return; }
    uint32_t spa = 0, spa_size = 0;
    std::vector<uint8_t> big_png, small_png;
    if (!title_spa(r, title_id, &spa, &spa_size) || !spa_image(r, spa, spa_size, big, &big_png) ||
        !spa_image(r, spa, spa_size, small, &small_png)) {
        fprintf(stderr, "RCOMP-XAM GamerPicture title=0x%08X big=0x%08X small=0x%08X: not in the title's SPA\n",
                title_id, big, small);
        finish(ctx, overlapped, kErrorNotFound);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [id, png] : {std::make_pair(small, &small_png), std::make_pair(big, &big_png)}) {
            const std::string path = gamer_picture_path(title_id, id);
            if (path.empty() || !mkdir_p(path.substr(0, path.rfind('/'))) ||
                !write_file_atomic(path, png->data(), png->size())) {
                finish(ctx, overlapped, kErrorFunctionFailed);
                return;
            }
        }
    }
    fprintf(stderr, "RCOMP-XAM GamerPicture awarded title=0x%08X big=0x%08X (%zu bytes) small=0x%08X (%zu bytes)\n",
            title_id, big, big_png.size(), small, small_png.size());
    finish(ctx, overlapped, kErrorSuccess);
}

struct Impl {
    uint32_t ordinal;
    const char* name;
    PPCFunc* function;
};
constexpr Impl impls[] = {
    {0x0219, "XamUserReadProfileSettings", &XamUserReadProfileSettings},
    {0x021A, "XamUserWriteProfileSettings", &XamUserWriteProfileSettings},
    {0x02F0, "XamWriteGamerTile", &XamWriteGamerTile},
};
}  // namespace

Status register_xam_profile_hle() {
    for (const auto& entry : impls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXam, entry.name, &ordinal) || ordinal != entry.ordinal) return Status::Conflict;
        const Status status = register_import(kModuleXam, entry.ordinal, entry.function, entry.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}
}  // namespace rcomp::rt
