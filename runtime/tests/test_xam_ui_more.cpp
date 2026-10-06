// Guide dialogs of src/hle_xam_ui_more.cpp, launch data (src/hle_xam_loader.cpp) and gamer pictures
// (XamWriteGamerTile, src/hle_xam_profile.cpp) for the one local profile signed in offline. Calls go through
// the same __imp__ symbols generated code uses, with Gears of War 2's argument shapes. The SPA is a small
// synthetic XDBF database built here (test data, no game bytes).
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/modules.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam_content.h"
#include "rcomp/runtime/xam_profile.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__XamShowMessagesUI);
PPC_EXTERN_FUNC(__imp__XamShowKeyboardUI);
PPC_EXTERN_FUNC(__imp__XamShowAchievementsUI);
PPC_EXTERN_FUNC(__imp__XamShowPlayersUI);
PPC_EXTERN_FUNC(__imp__XamShowGameInviteUI);
PPC_EXTERN_FUNC(__imp__XamShowFriendRequestUI);
PPC_EXTERN_FUNC(__imp__XamShowCustomPlayerListUI);
PPC_EXTERN_FUNC(__imp__XamLoaderGetLaunchDataSize);
PPC_EXTERN_FUNC(__imp__XamLoaderGetLaunchData);
PPC_EXTERN_FUNC(__imp__XamWriteGamerTile);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);

using namespace rcomp;
using namespace rcomp::rt;

namespace {

GuestMemory g_mem;
uint8_t* g_base;
uint32_t g_buf;  // guest scratch block
enum : uint32_t {
    kOut = 0x000,
    kEventHandle = 0x010,
    kTimeout = 0x018,     // LARGE_INTEGER 0
    kXuids = 0x030,       // two XUIDs
    kPlayers = 0x040,     // opaque player records
    kOverlapped = 0x080,  // 0x1C bytes
    kFrame = 0x100,       // caller frame for stack arguments (r1)
    kStrings = 0x400,     // UTF-16BE strings
    kText = 0x800,        // keyboard result buffer
};
constexpr uint32_t kUnmapped = 0x00001000;  // guest page 0 is never committed
constexpr uint32_t kIoPending = 0x3E5, kInvalidParameter = 0x57, kNotLoggedOn = 0x4DD, kNoSuchUser = 0x525;

uint32_t rd32(uint32_t a) {
    uint32_t v = 0;
    guest_read_be32(a, &v);
    return v;
}
uint16_t rd16(uint32_t a) {
    uint16_t v = 0;
    guest_read_be16(a, &v);
    return v;
}
void wr32(uint32_t a, uint32_t v) { guest_write_be32(a, v); }
void wr16(uint32_t a, uint16_t v) { g_base[a] = uint8_t(v >> 8), g_base[a + 1] = uint8_t(v); }
void zero(uint32_t a, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 4) wr32(a + i, 0);
}
uint32_t put_wstring(uint32_t a, const char* s) {
    uint32_t i = 0;
    for (; s[i]; ++i) wr16(a + 2 * i, uint8_t(s[i]));
    wr16(a + 2 * i, 0);
    return a;
}
// The guest UTF-16BE string at `a` as ASCII (stops at NUL or `max` characters).
std::string get_wstring(uint32_t a, uint32_t max) {
    std::string s;
    for (uint32_t i = 0; i < max; ++i) {
        const uint16_t c = rd16(a + 2 * i);
        if (!c) break;
        s += char(c);
    }
    return s;
}

// r3.. from `args`; `stack` holds arguments 9.. in the caller frame at r1 + 0x54 + 8*i.
uint32_t call(PPCFunc* f, std::initializer_list<uint64_t> args, std::initializer_list<uint32_t> stack = {}) {
    alignas(64) PPCContext ctx{};
    PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    size_t i = 0;
    for (uint64_t v : args) regs[i++]->u64 = v;
    ctx.r1.u64 = g_buf + kFrame;
    i = 0;
    for (uint32_t v : stack) wr32(g_buf + kFrame + 0x54 + 8 * uint32_t(i++), v);
    f(ctx, g_base);
    return ctx.r3.u32;
}

// Runs `fn` as a guest thread on the calling host thread (waits need one).
void (*g_guest_body)() = nullptr;
void guest_entry(PPCContext&, uint8_t*) { g_guest_body(); }
void as_guest(void (*fn)()) {
    alignas(64) PPCContext ctx;
    GuestThread t;
    if (create_guest_thread(runtime()->heap, {0x10000, 0, 0}, &ctx, &t) != Status::Ok) {
        CHECK(false);
        return;
    }
    g_guest_body = fn;
    uint32_t code = 0;
    run_guest_thread(t, ctx, g_base, guest_entry, &code);
    destroy_guest_thread(runtime()->heap, &t);
}

uint32_t g_event = 0;
bool event_signalled() { return call(__imp__NtWaitForSingleObjectEx, {g_event, 0, 0, g_buf + kTimeout}) == 0; }
void reset_event() {
    // Auto-reset event: a successful zero-timeout wait consumed the signal; drain any left over.
    while (event_signalled()) {
    }
}
uint32_t fresh_overlapped() {
    const uint32_t ov = g_buf + kOverlapped;
    zero(ov, 0x1C);
    wr32(ov + 0x0C, g_event);
    reset_event();
    return ov;
}

// ---- LIVE-only Guide screens ----
void live_body() {
    const uint64_t other = 0x0009000000000001ull;
    CHECK_EQ(call(__imp__XamShowMessagesUI, {0}), kNotLoggedOn);
    CHECK_EQ(call(__imp__XamShowMessagesUI, {1}), kNoSuchUser);
    CHECK_EQ(call(__imp__XamShowMessagesUI, {4}), kInvalidParameter);
    CHECK_EQ(call(__imp__XamShowPlayersUI, {0}), kNotLoggedOn);
    CHECK_EQ(call(__imp__XamShowPlayersUI, {3}), kNoSuchUser);
    CHECK_EQ(call(__imp__XamShowFriendRequestUI, {0, other}), kNotLoggedOn);
    CHECK_EQ(call(__imp__XamShowFriendRequestUI, {2, other}), kNoSuchUser);
    CHECK_EQ(call(__imp__XamShowFriendRequestUI, {0xFF, other}), kInvalidParameter);
    // GoW2's two invite shapes: no recipients, with or without a text.
    const uint32_t text = put_wstring(g_buf + kStrings, "Join my \"Horde\"");
    CHECK_EQ(call(__imp__XamShowGameInviteUI, {0, 0, 0, 0}), kNotLoggedOn);
    CHECK_EQ(call(__imp__XamShowGameInviteUI, {0, 0, 0, text}), kNotLoggedOn);
    CHECK_EQ(call(__imp__XamShowGameInviteUI, {1, 0, 0, text}), kNoSuchUser);
    guest_write_be64(g_buf + kXuids, other);
    CHECK_EQ(call(__imp__XamShowGameInviteUI, {0, g_buf + kXuids, 1, 0}), kNotLoggedOn);
    CHECK_EQ(call(__imp__XamShowGameInviteUI, {0, 0, 1, 0}), kInvalidParameter);           // recipients missing
    CHECK_EQ(call(__imp__XamShowGameInviteUI, {0, 0, 0, kUnmapped}), kInvalidParameter);  // text unreadable

    // Custom player list: GoW2 passes NULL buttons and result and an overlapped (stack slot 11).
    const uint32_t title = put_wstring(g_buf + kStrings + 0x100, "Players");
    const uint32_t description = put_wstring(g_buf + kStrings + 0x140, "Pick one");
    uint32_t ov = fresh_overlapped();
    CHECK_EQ(call(__imp__XamShowCustomPlayerListUI, {0, 0, title, description, 0, 0, g_buf + kPlayers, 2},
                  {0, 0, 0, ov}),
             kIoPending);
    CHECK_EQ(rd32(ov + 0x00), kNotLoggedOn);
    CHECK_EQ(rd32(ov + 0x04), 0u);
    CHECK_EQ(rd32(ov + 0x18), 0x800704DDu);
    CHECK(event_signalled());
    ov = fresh_overlapped();
    CHECK_EQ(call(__imp__XamShowCustomPlayerListUI, {2, 0, title, description, 0, 0, g_buf + kPlayers, 2},
                  {0, 0, 0, ov}),
             kIoPending);
    CHECK_EQ(rd32(ov + 0x00), kNoSuchUser);
    CHECK_EQ(call(__imp__XamShowCustomPlayerListUI, {0, 0, title, description, 0, 0, g_buf + kPlayers, 2},
                  {0, 0, 0, 0}),
             kNotLoggedOn);
    // Caller errors are synchronous and leave the overlapped untouched.
    ov = fresh_overlapped();
    wr32(ov, 0x1234);
    CHECK_EQ(call(__imp__XamShowCustomPlayerListUI, {0, 0, title, description, 0, 0, 0, 2}, {0, 0, 0, ov}),
             kInvalidParameter);
    CHECK_EQ(call(__imp__XamShowCustomPlayerListUI, {0, 0, kUnmapped, description, 0, 0, 0, 0}, {0, 0, 0, ov}),
             kInvalidParameter);
    CHECK_EQ(call(__imp__XamShowCustomPlayerListUI, {0, 0, title, description, 0, 0, 0, 0}, {0, 0, 0, kUnmapped}),
             kInvalidParameter);
    CHECK_EQ(rd32(ov), 0x1234u);
    CHECK(!event_signalled());
}

// ---- achievements and keyboard ----
void local_body() {
    CHECK_EQ(call(__imp__XamShowAchievementsUI, {0, 0}), 0u);  // GoW2: title id 0
    CHECK_EQ(call(__imp__XamShowAchievementsUI, {1, 0}), kNoSuchUser);
    CHECK_EQ(call(__imp__XamShowAchievementsUI, {5, 0}), kInvalidParameter);

    const uint32_t s = g_buf + kStrings;
    const uint32_t def = put_wstring(s, "Marcus Fenix");
    const uint32_t title = put_wstring(s + 0x100, "Name");
    const uint32_t description = put_wstring(s + 0x140, "Enter a \"name\"");
    const uint32_t empty = put_wstring(s + 0x1C0, "");
    const uint32_t text = g_buf + kText;
    auto fill = [&] {
        for (uint32_t i = 0; i < 32; ++i) wr16(text + 2 * i, 0xAAAA);
    };

    // GoW2's shape: overlapped always passed; the default text is accepted as typed.
    fill();
    uint32_t ov = fresh_overlapped();
    CHECK_EQ(call(__imp__XamShowKeyboardUI, {0, 0, def, title, description, text, 16, ov}), kIoPending);
    CHECK(get_wstring(text, 32) == "Marcus Fenix");
    CHECK_EQ(rd16(text + 2 * 12), 0u);
    CHECK_EQ(rd16(text + 2 * 13), 0xAAAAu);  // nothing past the terminator
    CHECK_EQ(rd32(ov + 0x00), 0u);
    CHECK_EQ(rd32(ov + 0x04), 0u);
    CHECK_EQ(rd32(ov + 0x18), 0u);
    CHECK(event_signalled());
    // Cut to length - 1 WCHARs and terminated inside the buffer.
    fill();
    ov = fresh_overlapped();
    CHECK_EQ(call(__imp__XamShowKeyboardUI, {0xFF, 0x1, def, title, description, text, 4, ov}), kIoPending);
    CHECK(get_wstring(text, 32) == "Mar");
    CHECK_EQ(rd16(text + 2 * 3), 0u);
    CHECK_EQ(rd16(text + 2 * 4), 0xAAAAu);
    // One WCHAR: only the terminator fits.
    fill();
    CHECK_EQ(call(__imp__XamShowKeyboardUI, {0, 0, def, 0, 0, text, 1, 0}), 0u);
    CHECK_EQ(rd16(text), 0u);
    CHECK_EQ(rd16(text + 2), 0xAAAAu);
    // GoW2 passes an empty string when it has no default; NULL reads the same.
    fill();
    CHECK_EQ(call(__imp__XamShowKeyboardUI, {0, 0, empty, empty, empty, text, 16, 0}), 0u);
    CHECK_EQ(rd16(text), 0u);
    fill();
    CHECK_EQ(call(__imp__XamShowKeyboardUI, {0, 0, 0, 0, 0, text, 16, 0}), 0u);
    CHECK_EQ(rd16(text), 0u);
    CHECK_EQ(rd16(text + 2), 0xAAAAu);

    // Rejected requests leave the buffer and the overlapped untouched.
    fill();
    ov = fresh_overlapped();
    wr32(ov, 0x1234);
    CHECK_EQ(call(__imp__XamShowKeyboardUI, {0, 0, def, title, description, 0, 16, ov}), kInvalidParameter);
    CHECK_EQ(call(__imp__XamShowKeyboardUI, {0, 0, def, title, description, text, 0, ov}), kInvalidParameter);
    CHECK_EQ(call(__imp__XamShowKeyboardUI, {7, 0, def, title, description, text, 16, ov}), kInvalidParameter);
    CHECK_EQ(call(__imp__XamShowKeyboardUI, {0, 0, kUnmapped, title, description, text, 16, ov}), kInvalidParameter);
    CHECK_EQ(call(__imp__XamShowKeyboardUI, {0, 0, def, kUnmapped, description, text, 16, ov}), kInvalidParameter);
    CHECK_EQ(call(__imp__XamShowKeyboardUI, {0, 0, def, title, description, kUnmapped, 16, ov}), kInvalidParameter);
    CHECK_EQ(call(__imp__XamShowKeyboardUI, {0, 0, def, title, description, text, 16, kUnmapped}),
             kInvalidParameter);
    CHECK_EQ(rd16(text), 0xAAAAu);
    CHECK_EQ(rd32(ov), 0x1234u);
    CHECK(!event_signalled());
}

// ---- launch data ----
void launch_data_body() {
    wr32(g_buf + kOut, 0xDEAD);
    CHECK_EQ(call(__imp__XamLoaderGetLaunchDataSize, {g_buf + kOut}), 0x490u);  // ERROR_NOT_FOUND
    CHECK_EQ(rd32(g_buf + kOut), 0u);
    CHECK_EQ(call(__imp__XamLoaderGetLaunchDataSize, {0}), kInvalidParameter);
    CHECK_EQ(call(__imp__XamLoaderGetLaunchDataSize, {kUnmapped}), kInvalidParameter);
    wr32(g_buf + kText, 0xBEEF);
    CHECK_EQ(call(__imp__XamLoaderGetLaunchData, {g_buf + kText, 1000}), 0x490u);
    CHECK_EQ(rd32(g_buf + kText), 0xBEEFu);
}

// ---- synthetic main XEX with a resource-info header and an SPA ----
void be32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    b[o] = uint8_t(v >> 24), b[o + 1] = uint8_t(v >> 16), b[o + 2] = uint8_t(v >> 8), b[o + 3] = uint8_t(v);
}
void be16(std::vector<uint8_t>& b, size_t o, uint16_t v) { b[o] = uint8_t(v >> 8), b[o + 1] = uint8_t(v); }
void be64(std::vector<uint8_t>& b, size_t o, uint64_t v) {
    be32(b, o, uint32_t(v >> 32));
    be32(b, o + 4, uint32_t(v));
}
void le16(std::vector<uint8_t>& b, size_t o, uint16_t v) { b[o] = uint8_t(v), b[o + 1] = uint8_t(v >> 8); }
void le32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + i] = uint8_t(v >> (8 * i));
}
constexpr uint32_t kImageBase = 0x82000000, kImageSize = 0x3000, kImageEntry = kImageBase + 0x1000;
constexpr uint32_t kTitleId = 0x545407F2;
constexpr uint32_t kSpaOffset = 0x2000;
// Test images: a PNG signature followed by distinct filler bytes.
std::vector<uint8_t> test_png(uint8_t fill, size_t n) {
    std::vector<uint8_t> p = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    p.resize(n, fill);
    return p;
}
const std::vector<uint8_t> kSmallPng = test_png(0x32, 40), kBigPng = test_png(0x64, 72);

std::vector<uint8_t> build_spa() {
    // 4 entry slots (3 used), 1 free-table slot; content follows.
    const uint32_t table = 4, count = 3, free_table = 1;
    const uint32_t content = 24 + table * 18 + free_table * 8;
    std::vector<uint8_t> x(content + 8 + kSmallPng.size() + kBigPng.size(), 0);
    be32(x, 0, 0x58444246);  // XDBF
    be32(x, 4, 0x10000);
    be32(x, 8, table);
    be32(x, 12, count);
    be32(x, 16, free_table);
    be32(x, 20, 1);
    struct E { uint16_t ns; uint64_t id; uint32_t offset, length; };
    const E entries[] = {{1, 0x58414348, 0, 8},  // a metadata block (not an image)
                         {2, 0x10001, 8, uint32_t(kSmallPng.size())},
                         {2, 0x20001, 8 + uint32_t(kSmallPng.size()), uint32_t(kBigPng.size())}};
    for (uint32_t i = 0; i < count; ++i) {
        const size_t e = 24 + 18 * i;
        be16(x, e, entries[i].ns);
        be64(x, e + 2, entries[i].id);
        be32(x, e + 10, entries[i].offset);
        be32(x, e + 14, entries[i].length);
    }
    std::copy(kSmallPng.begin(), kSmallPng.end(), x.begin() + content + 8);
    std::copy(kBigPng.begin(), kBigPng.end(), x.begin() + content + 8 + kSmallPng.size());
    return x;
}

XexImage main_image() {
    std::vector<uint8_t> image(kImageSize);
    le16(image, 0, 0x5A4D);
    le32(image, 0x3C, 0x80);
    const size_t nt = 0x80, optional = nt + 24, section = optional + 224;
    le32(image, nt, 0x4550);
    le16(image, nt + 4, 0x1F2);
    le16(image, nt + 6, 1);
    le16(image, nt + 20, 224);
    le16(image, optional, 0x10B);
    le32(image, optional + 16, 0x1000);
    le32(image, optional + 28, kImageBase);
    le32(image, optional + 32, 0x1000);
    le32(image, optional + 36, 0x200);
    le32(image, optional + 56, kImageSize);
    le32(image, optional + 60, 0x200);
    le32(image, optional + 92, 16);
    le32(image, section + 8, 0x1000);
    le32(image, section + 12, 0x1000);
    le32(image, section + 16, 0x1000);
    le32(image, section + 36, 0x60000020);
    const std::vector<uint8_t> spa = build_spa();
    std::copy(spa.begin(), spa.end(), image.begin() + kSpaOffset);
    CHECK_ST(load_raw_image(g_mem, kImageBase, image.data(), image.size()), Status::Ok);

    std::vector<uint8_t> header(0x1000);
    be32(header, 0, 0x58455832);  // XEX2
    be32(header, 8, 0x1000);
    be32(header, 16, 0x400);
    be32(header, 20, 4);
    const uint32_t fields[][2] = {
        {0x000002FF, 0x200}, {0x00010100, kImageEntry}, {0x00010201, kImageBase}, {0x00040006, 0x100}};
    for (size_t i = 0; i < 4; ++i) be32(header, 24 + i * 8, fields[i][0]), be32(header, 28 + i * 8, fields[i][1]);
    be32(header, 0x100, 0x4A53F9F6);
    be32(header, 0x104, 6);
    be32(header, 0x108, 5);
    be32(header, 0x10C, kTitleId);
    header[0x112] = 1;
    header[0x113] = 2;
    // Resource info: an unrelated resource, then the SPA named after the title id.
    be32(header, 0x200, 4 + 2 * 16);
    memcpy(&header[0x204], "ContSrvr", 8);
    be32(header, 0x20C, kImageBase + 0x1800);
    be32(header, 0x210, 0x10);
    memcpy(&header[0x214], "545407F2", 8);
    be32(header, 0x21C, kImageBase + kSpaOffset);
    be32(header, 0x220, uint32_t(spa.size()));
    be32(header, 0x404, kImageSize);
    be32(header, 0x510, kImageBase);
    XexImage result{};
    result.base = kImageBase;
    result.size = kImageSize;
    result.entry_point = kImageEntry;
    result.header = std::move(header);
    return result;
}

std::string g_root;

bool file_equals(const std::string& path, const std::vector<uint8_t>& expected) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    std::vector<uint8_t> got(expected.size() + 16);
    const ssize_t n = read(fd, got.data(), got.size());
    close(fd);
    return n == ssize_t(expected.size()) && std::equal(expected.begin(), expected.end(), got.begin());
}
bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// ---- gamer pictures ----
void gamer_tile_body() {
    const std::string dir = g_root + "/E000000052434F4D/gamertile/545407F2/";
    // XUserAwardGamerPicture(user, 1, 0, overlapped) as GoW2's wrapper forwards it.
    uint32_t ov = fresh_overlapped();
    CHECK_EQ(call(__imp__XamWriteGamerTile, {0, 0, 0x20001, 0x10001, 1, ov}), kIoPending);
    CHECK_EQ(rd32(ov + 0x00), 0x48Fu);  // no save root yet: no storage device
    CHECK(event_signalled());

    CHECK_ST(runtime_configure_save_root(g_root), Status::Ok);
    ov = fresh_overlapped();
    CHECK_EQ(call(__imp__XamWriteGamerTile, {0, 0, 0x20001, 0x10001, 1, ov}), kIoPending);
    CHECK_EQ(rd32(ov + 0x00), 0u);
    CHECK_EQ(rd32(ov + 0x18), 0u);
    CHECK(event_signalled());
    CHECK(file_equals(dir + "00010001.png", kSmallPng));
    CHECK(file_equals(dir + "00020001.png", kBigPng));
    CHECK(!exists(dir + "00010001.png.tmp"));
    // Again, synchronously and with the title id spelled out: the same files.
    CHECK_EQ(call(__imp__XamWriteGamerTile, {0, kTitleId, 0x20001, 0x10001, 1, 0}), 0u);
    CHECK(file_equals(dir + "00020001.png", kBigPng));

    // A picture the SPA does not hold; the empty slots; an index past them.
    ov = fresh_overlapped();
    CHECK_EQ(call(__imp__XamWriteGamerTile, {0, 0, 0x20002, 0x10002, 1, ov}), kIoPending);
    CHECK_EQ(rd32(ov + 0x00), 0x490u);
    CHECK_EQ(rd32(ov + 0x18), 0x80070490u);
    CHECK(!exists(dir + "00020002.png"));
    CHECK_EQ(call(__imp__XamWriteGamerTile, {0, 0, 0x58414348, 0x10001, 1, 0}), 0x490u);  // not an image
    ov = fresh_overlapped();
    CHECK_EQ(call(__imp__XamWriteGamerTile, {1, 0, 0x20001, 0x10001, 1, ov}), kIoPending);
    CHECK_EQ(rd32(ov + 0x00), kNoSuchUser);
    wr32(ov, 0x1234);
    CHECK_EQ(call(__imp__XamWriteGamerTile, {4, 0, 0x20001, 0x10001, 1, ov}), 0x80070057u);
    CHECK_EQ(call(__imp__XamWriteGamerTile, {0, 0, 0x20001, 0x10001, 1, kUnmapped}), kInvalidParameter);
    CHECK_EQ(rd32(ov), 0x1234u);

    // Unestablished forms stop instead of guessing.
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__XamWriteGamerTile, {0, 0, 0x20001, 0x10001, 0, 0}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(call(__imp__XamWriteGamerTile, {0, 0x4D530000, 0x20001, 0x10001, 1, 0}), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
}

}  // namespace

int main() {
    if (g_mem.reserve() != MemStatus::Ok) return 2;
    g_base = g_mem.base();
    CHECK_ST(runtime_init(&g_mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(register_xam_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &g_buf), Status::Ok);
    CHECK_ST(runtime_prepare_main_module({"game:\\Original.xex", "Original.xex"}), Status::Ok);
    CHECK_ST(runtime_finalize_main_module(main_image()), Status::Ok);

    char temp[] = "/tmp/rcomp_xam_ui_more_XXXXXX";
    CHECK(mkdtemp(temp) != nullptr);
    g_root = std::string(temp) + "/savedata";

    as_guest([] {
        CHECK_EQ(call(__imp__NtCreateEvent, {g_buf + kEventHandle, 0, 1, 0}), 0u);  // synchronization event
        g_event = rd32(g_buf + kEventHandle);
        zero(g_buf + kTimeout, 8);
    });
    as_guest(live_body);
    as_guest(local_body);
    as_guest(launch_data_body);
    as_guest(gamer_tile_body);
    as_guest([] { CHECK_EQ(call(__imp__NtClose, {g_event}), 0u); });

    runtime_shutdown();
    clear_imports();
    g_mem.release();
    const std::string cleanup = std::string("rm -rf ") + temp;
    CHECK_EQ(system(cleanup.c_str()), 0);
    return test_result("rt_xam_ui_more");
}
