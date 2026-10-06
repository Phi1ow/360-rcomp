// XexLoadImage of AOT-compiled secondary XEX modules (title DLLs): registry,
// load (dependencies first, imports bound, exports, LDR, DllMain), queries,
// reference counting, unload/reload and the explicit failure paths. The DLLs
// are synthetic XEX2/PE32 images built here (no game content); their "AOT
// code" is TESTDOUBLE_ host functions registered at their guest addresses.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "kernel_image_util.h"
#include "rcomp/aot_modules.h"
#include "rcomp/runtime/status.h"

PPC_EXTERN_FUNC(__imp__XexLoadImage);
PPC_EXTERN_FUNC(__imp__XexUnloadImage);
PPC_EXTERN_FUNC(__imp__XexGetModuleHandle);
PPC_EXTERN_FUNC(__imp__XexGetProcedureAddress);
PPC_EXTERN_FUNC(__imp__XexGetModuleSection);
PPC_EXTERN_FUNC(__imp__RtlImageXexHeaderField);

using namespace rcomp;
using namespace rcomp::rt;

namespace {
GuestMemory mem;
constexpr uint32_t scratch = 0x30000000, output = scratch + 0x100, output2 = scratch + 0x104, name = scratch + 0x200,
                   name2 = scratch + 0x400, name3 = scratch + 0x600, output3 = scratch + 0x108;
constexpr uint32_t kNoSuchFile = 0xC000000Fu, kEntryNotFound = 0xC0000263u, kStatusNotFound = 0xC0000225u;
constexpr uint32_t kHeader = 0x1000, kImage = 0x20000, kDllFlags = 0x9;

// ---- synthetic XEX2 DLL ----------------------------------------------------
void be(std::vector<uint8_t>& v, size_t p, uint32_t x) { for (int i = 0; i < 4; ++i) v[p + i] = (uint8_t)(x >> (24 - 8 * i)); }
void le(std::vector<uint8_t>& v, size_t p, uint32_t x) { for (int i = 0; i < 4; ++i) v[p + i] = (uint8_t)(x >> (8 * i)); }
void le16(std::vector<uint8_t>& v, size_t p, uint16_t x) { v[p] = (uint8_t)x; v[p + 1] = (uint8_t)(x >> 8); }

struct Import { uint32_t rva; uint32_t ordinal; bool thunk; };
struct Library { std::string name; std::vector<Import> imports; };
struct Dll {
    uint32_t base = 0, flags = kDllFlags;
    std::vector<Library> libraries;
    std::vector<uint32_t> exports;  // RVAs from ordinal 1; 0 = hole
    bool resource = false;
    uint32_t pe_exports_rva = 0;  // PE export directory (0: none)
};
// Image: .text 0x1000 (entry 0x1000, export function 0x1040, thunks 0x1100+),
// .data 0x2000 (exported variable 0x2000, resource 0x2100), .rdata 0x3000
// (import slots, export table 0x3200).
std::vector<uint8_t> make_dll(const Dll& d) {
    std::vector<uint8_t> x(kHeader + kImage);
    const uint32_t base = d.base;
    std::vector<std::pair<uint32_t, uint32_t>> fields = {
        {0x3FF, 0x100}, {0x10100, base + 0x1000}, {0x10201, base}, {0x103FF, 0x200}};
    if (d.resource) fields.push_back({0x2FF, 0x340});
    be(x, 0, 0x58455832); be(x, 4, d.flags); be(x, 8, kHeader); be(x, 16, 0x400); be(x, 20, (uint32_t)fields.size());
    for (size_t i = 0; i < fields.size(); ++i) { be(x, 24 + 8 * i, fields[i].first); be(x, 28 + 8 * i, fields[i].second); }
    be(x, 0x100, 8);
    if (d.resource) { be(x, 0x340, 4 + 16); memcpy(x.data() + 0x344, "res1", 4); be(x, 0x34C, base + 0x2100); be(x, 0x350, 0x40); }
    be(x, 0x404, kImage); be(x, 0x510, base); be(x, 0x560, d.exports.empty() ? 0 : base + 0x3200);
    // Import libraries: string table (4-byte padded), then one record per library.
    size_t strings = 0;
    for (const auto& l : d.libraries) strings += (l.name.size() + 1 + 3) & ~size_t(3);
    size_t at = 0x20C, cursor = 0x20C + strings;
    for (size_t i = 0; i < d.libraries.size(); ++i) {
        const auto& l = d.libraries[i];
        memcpy(x.data() + at, l.name.c_str(), l.name.size() + 1);
        at += (l.name.size() + 1 + 3) & ~size_t(3);
        const uint32_t size = 0x28 + 4 * (uint32_t)l.imports.size();
        be(x, cursor, size); x[cursor + 0x25] = (uint8_t)i; x[cursor + 0x27] = (uint8_t)l.imports.size();
        for (size_t k = 0; k < l.imports.size(); ++k) {
            const Import& im = l.imports[k];
            be(x, cursor + 0x28 + 4 * k, base + im.rva);
            be(x, kHeader + im.rva, (im.thunk ? 0x01000000u : 0u) | (uint32_t(i) << 16) | im.ordinal);
        }
        cursor += size;
    }
    be(x, 0x200, (uint32_t)(cursor - 0x200)); be(x, 0x204, (uint32_t)strings); be(x, 0x208, (uint32_t)d.libraries.size());
    // PE32 headers.
    le16(x, kHeader, 0x5A4D); le(x, kHeader + 0x3C, 0x80);
    const size_t nt = kHeader + 0x80, opt = nt + 24;
    le(x, nt, 0x4550); le16(x, nt + 4, 0x1F2); le16(x, nt + 6, 3); le(x, nt + 8, 0x51525354); le16(x, nt + 20, 224);
    le16(x, opt, 0x10B); le(x, opt + 16, 0x1000); le(x, opt + 28, base); le(x, opt + 32, 0x1000); le(x, opt + 36, 0x200);
    le(x, opt + 56, kImage + 0x10000); le(x, opt + 60, 0x1000); le(x, opt + 64, 0x0BADF00D); le(x, opt + 92, 16);
    if (d.pe_exports_rva) { le(x, opt + 96, d.pe_exports_rva); le(x, opt + 100, 0x100); }
    const uint32_t flags[] = {0x60000020, 0xC0000040, 0x40000040};
    for (unsigned i = 0; i < 3; ++i) {
        const size_t s = opt + 224 + i * 40;
        le(x, s + 8, 0x1000); le(x, s + 12, 0x1000 + i * 0x1000); le(x, s + 16, 0x1000); le(x, s + 36, flags[i]);
    }
    be(x, kHeader + 0x2000, 0xDA7A0003);
    if (!d.exports.empty()) {
        const size_t t = kHeader + 0x3200;
        be(x, t + 0x20, base >> 16); be(x, t + 0x24, (uint32_t)d.exports.size()); be(x, t + 0x28, 1);
        for (size_t i = 0; i < d.exports.size(); ++i) be(x, t + 0x2C + 4 * i, d.exports[i]);
    }
    return x;
}

// ---- TESTDOUBLE "AOT code" ----------------------------------------------------
constexpr uint32_t kA = 0x88000000, kB = 0x89400000, kD = 0x8B900000, kE = 0x30000000, kF = 0x8C000000, kM = 0x8D000000,
                   kG = 0x8E000000;
struct Call { uint32_t entry, handle, reason, reserved, self_lookup; };
std::vector<Call> g_calls;
uint32_t g_self_lookup_name = 0;
void record(PPCContext& c, uint8_t* base, uint32_t entry) {
    uint32_t self = 0;
    if (g_self_lookup_name) {  // the loader lock is re-entrant from DllMain
        alignas(64) PPCContext q{};
        q.r3.u64 = g_self_lookup_name; q.r4.u64 = output2;
        __imp__XexGetModuleHandle(q, base);
        uint32_t v = 0;
        if (q.r3.u32 == 0 && guest_read_be32(output2, &v)) self = v;
    }
    g_calls.push_back({entry, c.r3.u32, c.r4.u32, c.r5.u32, self});
}
bool g_self_load = false;
uint32_t g_self_load_status = 0xFFFFFFFFu;
void TESTDOUBLE_a_entry(PPCContext& c, uint8_t* base) {
    record(c, base, kA + 0x1000);
    if (g_self_load && c.r4.u32 == 1) {
        alignas(64) PPCContext q{};
        q.r3.u64 = name3; q.r4.u64 = kDllFlags; q.r5.u64 = 0; q.r6.u64 = output3;
        __imp__XexLoadImage(q, base);
        g_self_load_status = q.r3.u32;
    }
    c.r3.u64 = 1;
}
void TESTDOUBLE_a_f1(PPCContext& c, uint8_t*) { c.r3.u64 = 0xA1 + c.r3.u64; }
void TESTDOUBLE_b_entry(PPCContext& c, uint8_t* base) { record(c, base, kB + 0x1000); c.r3.u64 = 1; }
void TESTDOUBLE_b_f1(PPCContext& c, uint8_t*) { c.r3.u64 = 0xB1; }
// What the build emits for B's thunk of LibA.dll ordinal 1 (PPC_UNRESOLVED_IMPORT
// routed to the runtime, proposal in runtime/docs/MODULES.md).
void TESTDOUBLE_b_thunk_liba_1(PPCContext& c, uint8_t* base) { rcomp_module_import(c, base, "LibA_dll", 1); }
void TESTDOUBLE_d_entry(PPCContext& c, uint8_t* base) { record(c, base, kD + 0x1000); c.r3.u64 = 1; }
void TESTDOUBLE_f_entry(PPCContext& c, uint8_t* base) { record(c, base, kF + 0x1000); c.r3.u64 = 0; }
void TESTDOUBLE_main_other(PPCContext& c, uint8_t*) { c.r3.u64 = 0; }

const FuncEntry kAFunctions[] = {{kA + 0x1000, TESTDOUBLE_a_entry, "TESTDOUBLE_a_entry"},
                                 {kA + 0x1040, TESTDOUBLE_a_f1, "TESTDOUBLE_a_f1"},
                                 {kA + 0x1100, __imp__RtlCompareStringN, "TESTDOUBLE_a_kernel_thunk"}};
const FuncEntry kBFunctions[] = {{kB + 0x1000, TESTDOUBLE_b_entry, "TESTDOUBLE_b_entry"},
                                 {kB + 0x1040, TESTDOUBLE_b_f1, "TESTDOUBLE_b_f1"},
                                 {kB + 0x1100, TESTDOUBLE_b_thunk_liba_1, "TESTDOUBLE_b_thunk"}};
const FuncEntry kDFunctions[] = {{kD + 0x1000, TESTDOUBLE_d_entry, "TESTDOUBLE_d_entry"}};
const FuncEntry kEFunctions[] = {{kE + 0x1000, TESTDOUBLE_main_other, "TESTDOUBLE_e_entry"}};
const FuncEntry kFFunctions[] = {{kF + 0x1000, TESTDOUBLE_f_entry, "TESTDOUBLE_f_entry"}};
const FuncEntry kMFunctions[] = {{kM + 0x1000, TESTDOUBLE_main_other, "TESTDOUBLE_m_entry"}};
const FuncEntry kGFunctions[] = {{kG + 0x1000, TESTDOUBLE_main_other, "TESTDOUBLE_g_entry"}};
const AotModule kModules[] = {
    {"LibA.dll", "LibA.dll", kA, kImage, kA + 0x1000, kAFunctions, 3},
    {"sub/LibB.dll", "LibB.dll", kB, kImage, kB + 0x1000, kBFunctions, 3},
    {"Enc.dll", "Enc.dll", kD, kImage, kD + 0x1000, kDFunctions, 1},
    {"Low.dll", "Low.dll", kE, kImage, kE + 0x1000, kEFunctions, 1},
    {"False.dll", "False.dll", kF, kImage, kF + 0x1000, kFFunctions, 1},
    {"Missing.dll", "Missing.dll", kM, kImage, kM + 0x1000, kMFunctions, 1},
    {"Pe.dll", "Pe.dll", kG, kImage, kG + 0x1000, kGFunctions, 1},
};

// ---- guest call helpers ---------------------------------------------------------
uint32_t word(uint32_t p) { uint32_t v = 0; CHECK(guest_read_be32(p, &v)); return v; }
uint16_t half(uint32_t p) { return uint16_t((mem.base()[p] << 8) | mem.base()[p + 1]); }
uint32_t call(PPCFunc* fn, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
    alignas(64) PPCContext ctx{};
    ctx.r3.u64 = a; ctx.r4.u64 = b; ctx.r5.u64 = c; ctx.r6.u64 = d; ctx.lr = 0x82001000;
    fn(ctx, mem.base());
    return ctx.r3.u32;
}
uint32_t text(uint32_t at, const char* s) { strcpy((char*)mem.base() + at, s); return at; }
uint32_t load(const char* path, uint32_t flags = kDllFlags, uint32_t minimum = 0) {
    CHECK(guest_write_be32(output, 0xDEADBEEF));
    return call(__imp__XexLoadImage, text(name, path), flags, minimum, output);
}
uint32_t handle_of(const char* module) {
    CHECK(guest_write_be32(output, 0xDEADBEEF));
    const uint32_t s = call(__imp__XexGetModuleHandle, text(name, module), output);
    return s ? 0 : word(output);
}
uint32_t proc(uint32_t handle, uint32_t selector, uint32_t* status) {
    CHECK(guest_write_be32(output, 0xDEADBEEF));
    *status = call(__imp__XexGetProcedureAddress, handle, selector, output);
    return word(output);
}
std::string utf16_text(uint32_t descriptor) {
    const uint16_t bytes = half(descriptor);
    const uint32_t buffer = word(descriptor + 4);
    std::string s;
    for (uint32_t i = 0; i < bytes; i += 2) s.push_back((char)mem.base()[buffer + i + 1]);
    return s;
}
bool write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return fclose(f) == 0 && ok;
}

void registry_validation() {
    clear_aot_modules();
    const FuncEntry outside[] = {{kA + kImage, TESTDOUBLE_a_f1, nullptr}};
    const AotModule overlap[] = {{"x.dll", "x.dll", kA, kImage, 0, nullptr, 0}, {"y.dll", "y.dll", kA + 0x10000, kImage, 0, nullptr, 0}};
    const AotModule same_page[] = {{"x.dll", "x.dll", kA, 0x100, 0, nullptr, 0}, {"y.dll", "y.dll", kA + 0x10000, 0x100, 0, nullptr, 0}};
    const AotModule dup_path[] = {{"Sub/x.dll", "x.dll", kA, kImage, 0, nullptr, 0}, {"sub\\X.DLL", "y.dll", kB, kImage, 0, nullptr, 0}};
    const AotModule dup_name[] = {{"x.dll", "Lib.dll", kA, kImage, 0, nullptr, 0}, {"y.dll", "lib.DLL", kB, kImage, 0, nullptr, 0}};
    const AotModule dup_symbol[] = {{"x.dll", "Lib.dll", kA, kImage, 0, nullptr, 0}, {"y.dll", "Lib_dll", kB, kImage, 0, nullptr, 0}};
    const AotModule bad_function[] = {{"x.dll", "x.dll", kA, kImage, 0, outside, 1}};
    const AotModule unaligned[] = {{"x.dll", "x.dll", kA + 0x1000, kImage, 0, nullptr, 0}};
    const AotModule bad_entry[] = {{"x.dll", "x.dll", kA, kImage, kA + kImage, nullptr, 0}};
    const AotModule bad_path[] = {{"../x.dll", "x.dll", kA, kImage, 0, nullptr, 0}};
    CHECK(!register_aot_modules(overlap, 2));
    CHECK(register_aot_modules(same_page, 1));  // a lone module is fine...
    clear_aot_modules();
    CHECK(register_aot_modules(same_page, 2));  // ...two in different pages too
    clear_aot_modules();
    CHECK(!register_aot_modules(dup_path, 2));
    CHECK(!register_aot_modules(dup_name, 2));
    CHECK(!register_aot_modules(dup_symbol, 2));
    CHECK(!register_aot_modules(bad_function, 1));
    CHECK(!register_aot_modules(unaligned, 1));
    CHECK(!register_aot_modules(bad_entry, 1));
    CHECK(!register_aot_modules(bad_path, 1));
    CHECK(!register_aot_modules(nullptr, 1));
    size_t count = 7;
    CHECK(aot_modules(&count) == nullptr && count == 0);
    CHECK(register_aot_modules(kModules, sizeof kModules / sizeof kModules[0]));
    CHECK(!register_aot_modules(kModules, 1));  // one registration per title
    CHECK(aot_modules(&count) == kModules && count == 7);
    CHECK(find_aot_module("SUB\\libb.DLL") == &kModules[1]);
    CHECK(find_aot_module("/sub/LibB.dll") == &kModules[1]);
    CHECK(find_aot_module("LibB.dll") == nullptr);
    CHECK(find_aot_module("sub/../LibB.dll") == nullptr);
    CHECK(find_aot_module(nullptr) == nullptr);
}
}  // namespace

int main() {
    char tmpl[] = "/tmp/rcomp_rt_aotmod_XXXXXX";
    std::string top = mkdtemp(tmpl) ? tmpl : "";
    if (top.empty()) return 2;
    const std::string game = top + "/game", images = top + "/images";
    CHECK(mkdir(game.c_str(), 0755) == 0);
    CHECK(mkdir((game + "/sub").c_str(), 0755) == 0);
    CHECK(mkdir(images.c_str(), 0755) == 0);

    Dll a;
    a.base = kA; a.resource = true; a.pe_exports_rva = kImage;  // .edata beyond the payload
    a.libraries = {{"xboxkrnl.exe", {{0x1100, 0x11D, true}, {0x3000, 0x193, false}}}};
    a.exports = {0x1040, 0, 0x2000};  // ordinal 1 code, 2 hole, 3 data
    Dll b;
    b.base = kB;
    b.libraries = {{"xboxkrnl.exe", {{0x3100, 0x193, false}}},
                   {"LibA.dll", {{0x1100, 1, true}, {0x3000, 1, false}, {0x3004, 3, false}}}};
    b.exports = {0x1040};
    b.resource = true; b.pe_exports_rva = 0x2100;  // .edata RVA reused by a XEX resource
    Dll d; d.base = kD;
    Dll g; g.base = kG; g.pe_exports_rva = 0x2400;  // could be a loaded PE export table
    Dll e; e.base = kE;
    Dll f; f.base = kF;
    const std::vector<uint8_t> a_bytes = make_dll(a), d_plain = make_dll(d);
    CHECK(write_file(game + "/LibA.dll", a_bytes));
    CHECK(write_file(game + "/sub/LibB.dll", make_dll(b)));
    CHECK(write_file(game + "/Low.dll", make_dll(e)));
    CHECK(write_file(game + "/False.dll", make_dll(f)));
    CHECK(write_file(game + "/Pe.dll", make_dll(g)));
    CHECK(write_file(game + "/other.dll", make_dll(d)));  // a real XEX without descriptor
    // "Encrypted/compressed" disc file: the decoded image's exact header with
    // the file-format words set, and a payload that is not the image.
    std::vector<uint8_t> d_disc = d_plain;
    d_disc[0x105] = 1; d_disc[0x107] = 1;
    for (size_t i = kHeader; i < d_disc.size(); ++i) d_disc[i] = (uint8_t)(i * 7);
    CHECK(write_file(game + "/Enc.dll", d_disc));

    registry_validation();

    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK(mem.commit(scratch, 0x20000, Protect::ReadWrite) == MemStatus::Ok);
    RuntimeConfig cfg;
    cfg.heap_hi = 0x40100000;
    CHECK_ST(runtime_init(&mem, cfg), Status::Ok);
    clear_imports();
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(runtime()->vfs.mount("game", game), Status::Ok);
    CHECK(kimage::load_main(mem));
    {
        const FuncEntry main_functions[] = {{kimage::entry, kimage::TESTDOUBLE_entry, "TESTDOUBLE_entry"},
                                            {kimage::thunk, __imp__RtlCompareStringN, "TESTDOUBLE_thunk_mapping"}};
        CHECK(register_functions(main_functions, 2));
    }
    const uint32_t main_handle = handle_of("Original.xex");
    const uint32_t xam_handle = handle_of("xam.xex");
    uint32_t module_cell = 0;
    CHECK(find_variable_import(kModuleXboxkrnl, 0x193, &module_cell));
    bool fatal = false;

    // Not loaded yet: a bare name is no file; flags and version are checked.
    CHECK_EQ(load("LibA.dll"), kNoSuchFile);
    CHECK_EQ(handle_of("LibA.dll"), 0u);
    CAPTURE_FATAL(load("game:\\LibA.dll", 8), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED && g_fatal_msg.find("flags") != std::string::npos);
    CAPTURE_FATAL(load("game:\\LibA.dll", kDllFlags, 0x20000000u), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED && g_fatal_msg.find("MinimumVersion") != std::string::npos);

    // Load B: its dependency A is loaded and initialized first.
    text(name2, "LibA.dll");
    g_self_lookup_name = name2;
    CHECK_EQ(load("game:\\sub\\LibB.dll"), 0u);
    g_self_lookup_name = 0;
    const uint32_t hb = word(output);
    const uint32_t ha = handle_of("liba.DLL");
    CHECK(ha && hb && ha != hb && ha != main_handle);
    CHECK_EQ(handle_of("LibB.dll"), hb);
    CHECK_EQ(handle_of("game:\\sub\\LibB.dll"), hb);
    CHECK_EQ(handle_of("GAME:/SUB/libb.dll"), hb);
    CHECK_EQ(g_calls.size(), 2u);
    if (g_calls.size() == 2) {
        CHECK_EQ(g_calls[0].entry, kA + 0x1000); CHECK_EQ(g_calls[0].handle, ha);
        CHECK_EQ(g_calls[0].reason, 1u); CHECK_EQ(g_calls[0].reserved, 0u);
        CHECK_EQ(g_calls[0].self_lookup, ha);  // published before DllMain, lock re-entrant
        CHECK_EQ(g_calls[1].entry, kB + 0x1000); CHECK_EQ(g_calls[1].handle, hb); CHECK_EQ(g_calls[1].reason, 1u);
    }
    // LDR entries: real fields, one reference each (A's is held by B).
    CHECK_EQ(word(hb + 0x1C), kB); CHECK_EQ(word(hb + 0x38), kImage); CHECK_EQ(word(hb + 0x3C), kB + 0x1000);
    CHECK_EQ(word(hb + 0x18), kB + 0x80); CHECK_EQ(word(hb + 0x20), kImage + 0x10000);
    CHECK_EQ(word(hb + 0x48), 0x0BADF00Du); CHECK_EQ(word(hb + 0x50), 0x51525354u);
    CHECK_EQ(half(hb + 0x40), 1u); CHECK_EQ(half(ha + 0x40), 1u);
    CHECK(utf16_text(hb + 0x24) == "game:\\sub\\LibB.dll");
    CHECK(utf16_text(hb + 0x2C) == "LibB.dll");
    CHECK(utf16_text(ha + 0x24) == "game:\\LibA.dll");
    CHECK_EQ(memcmp(mem.base() + word(ha + 0x58), a_bytes.data(), kHeader), 0);
    CHECK(!mem.is_accessible(word(ha + 0x58), 4, Protect::ReadWrite));
    // Load-order list: main, xboxkrnl, xam, A, B, back to main.
    CHECK_EQ(word(xam_handle), ha); CHECK_EQ(word(ha), hb); CHECK_EQ(word(hb), main_handle);
    CHECK_EQ(word(main_handle + 4), hb); CHECK_EQ(word(hb + 4), ha); CHECK_EQ(word(ha + 4), xam_handle);
    // Imports bound exactly as the console loader writes them.
    CHECK_EQ(word(kB + 0x3000), kA + 0x1040);
    CHECK_EQ(word(kB + 0x3004), kA + 0x2000);
    CHECK_EQ(word(kB + 0x3100), module_cell);
    CHECK_EQ(word(kA + 0x3000), module_cell);
    CHECK_EQ(word(kA + 0x2000), 0xDA7A0003u);
    // B's thunk of A's ordinal 1 runs A's AOT function on the caller's context.
    CHECK_EQ(call(lookup_function(kB + 0x1100), 5), 0xA6u);
    CHECK_EQ(call(lookup_function(word(kB + 0x3000)), 1), 0xA2u);  // indirect call through the slot

    // Exports.
    uint32_t status = 0;
    CHECK_EQ(proc(ha, 1, &status), kA + 0x1040); CHECK_EQ(status, 0u);
    CHECK_EQ(proc(ha, 3, &status), kA + 0x2000); CHECK_EQ(status, 0u);
    CHECK_EQ(proc(hb, 1, &status), kB + 0x1040); CHECK_EQ(status, 0u);
    CHECK_EQ(proc(ha, 4, &status), 0u); CHECK_EQ(status, kEntryNotFound);
    CHECK_EQ(proc(ha, text(name2, "LibAFunction"), &status), 0u); CHECK_EQ(status, kEntryNotFound);
    CAPTURE_FATAL(proc(ha, 2, &status), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED && g_fatal_msg.find("hole") != std::string::npos);
    CAPTURE_FATAL(proc(ha, 0, &status), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    // Module imports by the generator's name, and their failures.
    {
        alignas(64) PPCContext ctx{};
        ctx.r3.u64 = 1;
        rcomp_module_import(ctx, mem.base(), "LIBA.DLL", 1);
        CHECK_EQ(ctx.r3.u32, 0xA2u);
        CAPTURE_FATAL(rcomp_module_import(ctx, mem.base(), "LibA_dll", 2), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_MISSING_IMPORT);
        CAPTURE_FATAL(rcomp_module_import(ctx, mem.base(), "LibA_dll", 3), fatal);  // a variable is not callable
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_INDIRECT_TARGET);
        CAPTURE_FATAL(rcomp_module_import(ctx, mem.base(), "Other_dll", 1), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_MISSING_IMPORT);
    }
    // Header fields and resources of a secondary module.
    CHECK_EQ(call(__imp__RtlImageXexHeaderField, word(ha + 0x58), 0x10100), kA + 0x1000);
    CHECK_EQ(call(__imp__XexGetModuleSection, ha, text(name2, "res1"), output, output2), 0u);
    CHECK_EQ(word(output), kA + 0x2100); CHECK_EQ(word(output2), 0x40u);
    CHECK_EQ(call(__imp__XexGetModuleSection, ha, text(name2, "none"), output, output2), kStatusNotFound);
    CHECK_EQ(call(__imp__XexGetModuleSection, hb, text(name2, "res1"), output, output2), 0u);
    CHECK_EQ(word(output), kB + 0x2100);
    CHECK_EQ(call(__imp__XexGetModuleSection, hb, text(name2, "res2"), output, output2), kStatusNotFound);

    // References: path, bare name and import name all reach the loaded module.
    CHECK_EQ(load("game:\\LibA.dll"), 0u); CHECK_EQ(word(output), ha); CHECK_EQ(half(ha + 0x40), 2u);
    CHECK_EQ(load("LIBA.dll"), 0u); CHECK_EQ(word(output), ha); CHECK_EQ(half(ha + 0x40), 3u);
    CAPTURE_FATAL(load("LibA.dll", 8), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_EQ(half(ha + 0x40), 3u);
    CHECK_EQ(g_calls.size(), 2u);

    // Unload: B's last reference detaches B and releases its reference on A.
    CHECK_EQ(call(__imp__XexUnloadImage, ha), 0u); CHECK_EQ(half(ha + 0x40), 2u);
    CHECK_EQ(call(__imp__XexUnloadImage, hb), 0u);
    CHECK_EQ(g_calls.size(), 3u);
    if (g_calls.size() == 3) { CHECK_EQ(g_calls[2].entry, kB + 0x1000); CHECK_EQ(g_calls[2].handle, hb); CHECK_EQ(g_calls[2].reason, 0u); }
    CHECK_EQ(handle_of("LibB.dll"), 0u);
    CHECK(!mem.is_committed(kB, 0x10000));
    CHECK_EQ(call(__imp__XexUnloadImage, hb), nt::kInvalidHandle);
    CHECK_EQ(half(ha + 0x40), 1u);
    CHECK_EQ(word(xam_handle), ha); CHECK_EQ(word(ha), main_handle); CHECK_EQ(word(main_handle + 4), ha);
    {
        alignas(64) PPCContext ctx{};
        CAPTURE_FATAL(rcomp_module_import(ctx, mem.base(), "LibB_dll", 1), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_MISSING_IMPORT);
    }
    CHECK_EQ(call(__imp__XexUnloadImage, ha), 0u);
    CHECK_EQ(g_calls.size(), 4u);
    if (g_calls.size() == 4) { CHECK_EQ(g_calls[3].entry, kA + 0x1000); CHECK_EQ(g_calls[3].reason, 0u); }
    CHECK_EQ(handle_of("LibA.dll"), 0u);
    CHECK(!mem.is_committed(kA, 0x10000));
    CHECK_EQ(word(xam_handle), main_handle); CHECK_EQ(word(main_handle + 4), xam_handle);

    // Reload: fresh image from the disc, functions already in the table. A's
    // DllMain(ATTACH) takes a reference on itself: allowed while it runs.
    text(name3, "game:\\LibA.dll");
    g_self_load = true;
    CHECK_EQ(load("game:\\sub\\LibB.dll"), 0u);
    g_self_load = false;
    const uint32_t hb2 = word(output);
    CHECK(hb2 != 0);
    CHECK_EQ(g_self_load_status, 0u);
    CHECK_EQ(g_calls.size(), 6u);
    const uint32_t ha2 = handle_of("LibA.dll");
    CHECK_EQ(word(output3), ha2);
    CHECK_EQ(half(ha2 + 0x40), 2u);
    CHECK_EQ(word(kB + 0x3000), kA + 0x1040);
    CHECK_EQ(call(lookup_function(kB + 0x1100), 1), 0xA2u);
    CHECK_EQ(call(__imp__XexUnloadImage, hb2), 0u);
    CHECK_EQ(g_calls.size(), 7u);  // B detached, A keeps its own reference
    CHECK_EQ(half(ha2 + 0x40), 1u);
    CHECK_EQ(call(__imp__XexUnloadImage, ha2), 0u);
    CHECK_EQ(g_calls.size(), 8u);

    // Missing file with a descriptor: the open's NTSTATUS, output untouched.
    CHECK_EQ(load("game:\\Missing.dll"), nt::kObjectNameNotFound);
    CHECK_EQ(word(output), 0xDEADBEEFu);
    // An existing XEX without a descriptor keeps the explicit fatal.
    CAPTURE_FATAL(load("game:\\other.dll"), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED && g_fatal_msg.find("recompiled into the title") != std::string::npos);
    // A fixed base over the runtime heap is refused.
    CAPTURE_FATAL(load("game:\\Low.dll"), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED && g_fatal_msg.find("guest heap") != std::string::npos);

    // Encrypted/compressed disc module: needs the host-decoded image, which
    // must carry the disc file's header.
    CAPTURE_FATAL(load("game:\\Enc.dll"), fatal);
    CHECK(fatal && g_fatal_msg.find("no host-decoded module image root") != std::string::npos);
    CHECK_ST(runtime_configure_module_images("relative"), Status::InvalidArgument);
    CHECK_ST(runtime_configure_module_images(images + "/"), Status::Ok);
    CAPTURE_FATAL(load("game:\\Enc.dll"), fatal);
    CHECK(fatal && g_fatal_msg.find("missing or unreadable") != std::string::npos);
    std::vector<uint8_t> wrong = d_plain;
    wrong[0x404 + 0x20] ^= 0xFF;  // security info differs from the disc file
    CHECK(write_file(images + "/Enc.dll", wrong));
    CAPTURE_FATAL(load("game:\\Enc.dll"), fatal);
    CHECK(fatal && g_fatal_msg.find("header mismatch") != std::string::npos);
    CHECK(write_file(images + "/Enc.dll", d_plain));
    CHECK_EQ(load("game:\\Enc.dll"), 0u);
    const uint32_t hd = word(output);
    CHECK_EQ(word(hd + 0x1C), kD);
    CHECK_EQ(memcmp(mem.base() + kD, d_plain.data() + kHeader, kImage), 0);
    CHECK_EQ(call(__imp__XexUnloadImage, hd), 0u);

    // A PE export directory that may really be loaded is not interpreted.
    CAPTURE_FATAL(load("game:\\Pe.dll"), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED && g_fatal_msg.find("PE/export metadata rejected") != std::string::npos);

    // DllMain(DLL_PROCESS_ATTACH) returning FALSE has no established result.
    CAPTURE_FATAL(load("game:\\False.dll"), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED && g_fatal_msg.find("returned FALSE") != std::string::npos);

    runtime_shutdown();
    clear_functions();
    clear_imports();
    clear_aot_modules();
    mem.release();
    return test_result("test_aot_modules");
}
