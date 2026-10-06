// XEX2 loader: accepted layout, import slots, and every rejection path.
#include <vector>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/xex_loader.h"
#include "test_util.h"

using namespace rcomp;
using namespace rcomp::rt;

namespace {

constexpr uint32_t kBase = 0x82000000, kImageSize = 0x20000, kHeaderSize = 0x1000;
constexpr uint32_t kEntry = kBase + 0x1000;
constexpr uint32_t kSlotVar = kBase + 0x2000, kSlotFn = kBase + 0x2004, kSlotUnknown = kBase + 0x2008;
constexpr uint32_t kThunk = kBase + 0x1100;

void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off] = uint8_t(x >> 24);
    v[off + 1] = uint8_t(x >> 16);
    v[off + 2] = uint8_t(x >> 8);
    v[off + 3] = uint8_t(x);
}
void put16(std::vector<uint8_t>& v, size_t off, uint16_t x) {
    v[off] = uint8_t(x >> 8);
    v[off + 1] = uint8_t(x);
}
uint32_t get32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

struct Opts {
    uint16_t encryption = 0, compression = 0;
    uint32_t entry = kEntry;
    uint32_t var_record = kSlotVar;  // VA of the first record (variable slot)
};

// Header: 4 optional headers (file format, entry, image base, imports), then
// the security info; image at kHeaderSize. Imports: xboxkrnl.exe with a
// variable slot (XboxKrnlVersion 0x158), a function slot (NtClose 0xCF) and a
// thunk (NtClose); "other.xex" with one unknown slot (ordinal 0x42).
std::vector<uint8_t> make_xex(const Opts& o) {
    std::vector<uint8_t> x(kHeaderSize + kImageSize, 0);
    put32(x, 0, 0x58455832);
    put32(x, 8, kHeaderSize);
    const uint32_t fmt = 0x100, imp = 0x200, sec = 0x400;
    put32(x, 16, sec);
    put32(x, 20, 4);
    const uint32_t keys[4][2] = {{0x3FF, fmt}, {0x10100, o.entry}, {0x10201, kBase}, {0x103FF, imp}};
    for (int i = 0; i < 4; ++i) {
        put32(x, 24 + 8 * i, keys[i][0]);
        put32(x, 28 + 8 * i, keys[i][1]);
    }
    put32(x, fmt, 8);
    put16(x, fmt + 4, o.encryption);
    put16(x, fmt + 6, o.compression);
    // Import header: size, string table size, library count, strings, libraries.
    const char strtab[] = "xboxkrnl.exe\0\0\0\0other.xex\0\0\0";  // 16 + 12 bytes
    const uint32_t strtab_len = 28;
    size_t off = imp + 12;
    for (uint32_t i = 0; i < strtab_len; ++i) x[off + i] = uint8_t(strtab[i]);
    off += strtab_len;
    auto library = [&](uint16_t name_index, std::vector<uint32_t> recs) {
        const uint32_t size = 40 + 4 * uint32_t(recs.size());
        put32(x, off, size);
        put16(x, off + 0x24, name_index);
        put16(x, off + 0x26, uint16_t(recs.size()));
        for (size_t i = 0; i < recs.size(); ++i) put32(x, off + 40 + 4 * i, recs[i]);
        off += size;
    };
    library(0, {o.var_record, kSlotFn, kThunk});
    library(1, {kSlotUnknown});
    put32(x, imp, uint32_t(off - imp));
    put32(x, imp + 4, strtab_len);
    put32(x, imp + 8, 2);
    // Security info: image size, load address.
    put32(x, sec + 4, kImageSize);
    put32(x, sec + 4 + 4 + 0x100 + 4 + 4, kBase);
    // Image: records, a thunk, a recognisable word.
    auto img32 = [&](uint32_t va, uint32_t v) { put32(x, kHeaderSize + (va - kBase), v); };
    img32(kSlotVar, 0x00000158);
    img32(kSlotFn, 0x000000CF);
    img32(kSlotUnknown, 0x00000042);
    img32(kThunk, 0x010000CF);
    img32(kEntry, 0x4E800020);  // blr
    return x;
}

}  // namespace

int main() {
    GuestMemory mem;
    CHECK(mem.reserve() == MemStatus::Ok);
    clear_imports();
    // Only variable exports can be registered as variables.
    CHECK_ST(register_variable_import(kModuleXboxkrnl, 0x00CF, 0x40000000, "TESTDOUBLE_x"), Status::InvalidArgument);
    CHECK(export_kind(kModuleXboxkrnl, 0x0158) == ExportKind::Variable);
    CHECK(export_kind(kModuleXboxkrnl, 0x00CF) == ExportKind::Function);
    CHECK(export_kind("other.xex", 0x42) == ExportKind::Unknown);
    CHECK_ST(register_variable_import(kModuleXboxkrnl, 0x0158, 0x40001000, "TESTDOUBLE_XboxKrnlVersion"), Status::Ok);
    CHECK_ST(register_variable_import(kModuleXboxkrnl, 0x0158, 0x40002000, "TESTDOUBLE_other"), Status::AlreadyExists);

    XexImage img;
    {
        auto bad_tls = make_xex({});
        put32(bad_tls, 20, 5);
        put32(bad_tls, 24 + 4 * 8, 0x00020104);
        put32(bad_tls, 28 + 4 * 8, kHeaderSize - 8);
        CHECK_ST(load_xex_image(mem, bad_tls.data(), bad_tls.size(), &img, nullptr), Status::InvalidArgument);
        CHECK(!mem.is_committed(kBase, 1));
    }
    // Rejections leave guest memory untouched.
    {
        auto x = make_xex({});
        x[0] = 'Y';
        CHECK_ST(load_xex_image(mem, x.data(), x.size(), &img, nullptr), Status::InvalidArgument);
    }
    {
        Opts o;
        o.encryption = 1;
        auto x = make_xex(o);
        CHECK_ST(load_xex_image(mem, x.data(), x.size(), &img, nullptr), Status::Unsupported);
        o.encryption = 0;
        o.compression = 2;
        x = make_xex(o);
        CHECK_ST(load_xex_image(mem, x.data(), x.size(), &img, nullptr), Status::Unsupported);
    }
    {
        Opts o;
        o.entry = kBase + kImageSize;
        auto x = make_xex(o);
        CHECK_ST(load_xex_image(mem, x.data(), x.size(), &img, nullptr), Status::InvalidArgument);
    }
    {
        Opts o;
        o.var_record = kBase + kImageSize - 2;  // straddles the end of the image
        auto x = make_xex(o);
        CHECK_ST(load_xex_image(mem, x.data(), x.size(), &img, nullptr), Status::InvalidArgument);
    }
    {
        auto x = make_xex({});
        CHECK_ST(load_xex_image(mem, x.data(), x.size() - 1, &img, nullptr), Status::InvalidArgument);
    }
    CHECK(!mem.is_committed(kBase, 1));

    // Page 0 must stay free for the poison addresses.
    CHECK(mem.commit(0, kGuestPageSize, Protect::ReadWrite) == MemStatus::Ok);
    {
        auto x = make_xex({});
        CHECK_ST(load_xex_image(mem, x.data(), x.size(), &img, nullptr), Status::Conflict);
    }
    CHECK(mem.decommit(0, kGuestPageSize) == MemStatus::Ok);

    // Accepted load.
    auto x = make_xex({});
    CHECK_ST(load_xex_image(mem, x.data(), x.size(), &img, nullptr), Status::Ok);
    CHECK_EQ(img.base, kBase);
    CHECK_EQ(img.size, kImageSize);
    CHECK_EQ(img.entry_point, kEntry);
    CHECK_EQ(img.import_libraries, 2u);
    CHECK_EQ(img.function_thunks, 1u);
    CHECK_EQ(img.function_slots, 1u);
    CHECK_EQ(img.variables_resolved, 1u);
    CHECK_EQ(img.variables_unresolved, 1u);
    const uint8_t* g = mem.base();
    CHECK_EQ(get32(g + kSlotVar), 0x40001000u);
    CHECK_EQ(get32(g + kSlotFn), kUnresolvedImportPoison(0xCF));
    CHECK_EQ(get32(g + kSlotUnknown), kUnresolvedImportPoison(0x42));
    CHECK_EQ(get32(g + kThunk), 0x010000CFu);  // thunks are left to the generated code
    CHECK_EQ(get32(g + kEntry), 0x4E800020u);
    CHECK(!mem.is_committed(0, 1));

    // Loading again (or over any committed page) is refused.
    CHECK_ST(load_xex_image(mem, x.data(), x.size(), &img, nullptr), Status::Conflict);
    const uint8_t raw[4] = {1, 2, 3, 4};
    CHECK_ST(load_raw_image(mem, kBase + kImageSize - 4, raw, sizeof raw), Status::Conflict);
    CHECK_EQ(get32(g + kBase + kImageSize - 4), 0u);
    clear_imports();
    return test_result("rt_xex_loader");
}
