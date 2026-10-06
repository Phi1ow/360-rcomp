// Synthetic main image for the kernel tests that need a finalized main module
// (runtime/tests only). Same construction as tests/test_modules.cpp's
// original_xex(), plus a ".pdata" section: no game content.
#pragma once

#include <string.h>

#include <vector>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/modules.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xex_loader.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__RtlCompareStringN);

namespace kimage {
using namespace rcomp;
using namespace rcomp::rt;

constexpr uint32_t base = 0x82000000, image_size = 0x20000, header_size = 0x1000;
constexpr uint32_t entry = base + 0x1000, thunk = base + 0x1100;
// .pdata (RVA 0x3000): plain function at 0x1000 (0x40 bytes), function with an
// exception handler at 0x1040 (0x40 bytes).
constexpr uint32_t plain_function = base + 0x1000, handler_function = base + 0x1040;

inline void be(std::vector<uint8_t>& v, size_t p, uint32_t x) { for (int i = 0; i < 4; ++i) v[p + i] = (uint8_t)(x >> (24 - 8 * i)); }
inline void le(std::vector<uint8_t>& v, size_t p, uint32_t x) { for (int i = 0; i < 4; ++i) v[p + i] = (uint8_t)(x >> (8 * i)); }
inline void le16(std::vector<uint8_t>& v, size_t p, uint16_t x) { v[p] = (uint8_t)x; v[p + 1] = (uint8_t)(x >> 8); }

inline void TESTDOUBLE_entry(PPCContext& c, uint8_t*) { c.r3.u64 = 0xEA01; }

inline std::vector<uint8_t> make_xex() {
    std::vector<uint8_t> x(header_size + image_size);
    be(x, 0, 0x58455832); be(x, 8, header_size); be(x, 16, 0x400); be(x, 20, 5);
    const uint32_t fields[][2] = {{0x3FF, 0x100}, {0x10100, entry}, {0x10201, base}, {0x103FF, 0x200}, {0x30000, 0x80000008}};
    for (size_t i = 0; i < 5; ++i) { be(x, 24 + 8 * i, fields[i][0]); be(x, 28 + 8 * i, fields[i][1]); }
    be(x, 0x100, 8);
    be(x, 0x404, image_size); be(x, 0x510, base); be(x, 0x560, 0);
    // Imports: the four module variables and one real AOT function thunk.
    const char module[] = "xboxkrnl.exe"; memcpy(x.data() + 0x20C, module, sizeof module);
    be(x, 0x200, 12 + 16 + 40 + 20); be(x, 0x204, 16); be(x, 0x208, 1);
    be(x, 0x21C, 60); x[0x21C + 0x27] = 5;
    const uint32_t ordinals[] = {0x193, 0x59, 0x266, 0x1AE};
    for (unsigned i = 0; i < 4; ++i) { be(x, 0x21C + 40 + 4 * i, base + 0x2000 + 4 * i); be(x, header_size + 0x2000 + 4 * i, ordinals[i]); }
    be(x, 0x21C + 56, thunk); be(x, header_size + 0x1100, 0x0100011D);
    // PE32: code, data and .pdata sections.
    le16(x, header_size, 0x5A4D); le(x, header_size + 0x3C, 0x80);
    const size_t nt = header_size + 0x80, opt = nt + 24;
    le(x, nt, 0x4550); le16(x, nt + 4, 0x1F2); le16(x, nt + 6, 3); le(x, nt + 8, 0x24681357); le16(x, nt + 20, 224);
    le16(x, opt, 0x10B); le(x, opt + 16, 0x1000); le(x, opt + 28, base); le(x, opt + 32, 0x1000); le(x, opt + 36, 0x200);
    le(x, opt + 56, image_size); le(x, opt + 60, 0x1000); le(x, opt + 64, 0x13572468); le(x, opt + 92, 16);
    for (unsigned i = 0; i < 3; ++i) {
        size_t s = opt + 224 + i * 40;
        if (i == 2) memcpy(x.data() + s, ".pdata", 6);
        le(x, s + 8, i == 2 ? 16 : 0x1000); le(x, s + 12, 0x1000 + i * 0x1000); le(x, s + 16, 0x1000);
        le(x, s + 36, i == 0 ? 0x60000020 : 0x40000040);
    }
    const size_t pdata = header_size + 0x3000;
    be(x, pdata + 0, plain_function); be(x, pdata + 4, (0x10u << 8) | 4u);
    be(x, pdata + 8, handler_function); be(x, pdata + 12, 0x80000000u | (0x10u << 8) | 4u);
    return x;
}

// Requires runtime_init + register_xboxkrnl_hle; prepares, loads and finalizes.
inline bool load_main(GuestMemory& mem, const char* guest_path = "game:\\Original.xex") {
    ModuleConfig cfg;
    cfg.guest_path = guest_path;
    if (runtime_prepare_main_module(cfg) != Status::Ok) return false;
    auto x = make_xex();
    XexImage img;
    if (load_xex_image(mem, x.data(), x.size(), &img, nullptr) != Status::Ok) return false;
    const FuncEntry f[] = {{entry, TESTDOUBLE_entry, "TESTDOUBLE_entry"},
                           {thunk, __imp__RtlCompareStringN, "TESTDOUBLE_thunk_mapping"}};
    if (!register_functions(f, 2)) return false;
    return runtime_finalize_main_module(img) == Status::Ok;
}

}  // namespace kimage
