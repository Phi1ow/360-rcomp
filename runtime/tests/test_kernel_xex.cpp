// XexLoadImage / XexUnloadImage on an AOT title (main module and HLE system
// modules only) and RtlImageNtHeader.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "kernel_image_util.h"
#include "rcomp/runtime/status.h"

PPC_EXTERN_FUNC(__imp__XexLoadImage);
PPC_EXTERN_FUNC(__imp__XexUnloadImage);
PPC_EXTERN_FUNC(__imp__XexGetModuleHandle);
PPC_EXTERN_FUNC(__imp__RtlImageNtHeader);

using namespace rcomp;
using namespace rcomp::rt;

namespace {
GuestMemory mem;
constexpr uint32_t scratch = 0x30000000, output = scratch + 0x100, name = scratch + 0x200;
constexpr uint32_t kNoSuchFile = 0xC000000Fu;
uint32_t word(uint32_t p) { uint32_t v = 0; CHECK(guest_read_be32(p, &v)); return v; }
uint16_t load_count(uint32_t ldr) { return uint16_t((mem.base()[ldr + 0x40] << 8) | mem.base()[ldr + 0x41]); }
uint32_t call(PPCFunc* fn, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
    alignas(64) PPCContext ctx{};
    ctx.r3.u64 = a; ctx.r4.u64 = b; ctx.r5.u64 = c; ctx.r6.u64 = d; ctx.lr = 0x82001000;
    fn(ctx, mem.base());
    return ctx.r3.u32;
}
uint32_t load(const char* text, uint32_t minimum = 0) {
    strcpy((char*)mem.base() + name, text);
    CHECK(guest_write_be32(output, 0xDEADBEEF));
    return call(__imp__XexLoadImage, name, 0, minimum, output);
}
}  // namespace

int main() {
    char tmpl[] = "/tmp/rcomp_rt_kxex_XXXXXX";
    std::string top = mkdtemp(tmpl) ? tmpl : "";
    if (top.empty()) return 2;
    const std::string game = top + "/game";
    CHECK(mkdir(game.c_str(), 0755) == 0);
    if (FILE* f = fopen((game + "/other.xex").c_str(), "wb")) { fputs("XEX2", f); fclose(f); }

    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK(mem.commit(scratch, 0x20000, Protect::ReadWrite) == MemStatus::Ok);
    RuntimeConfig cfg;
    cfg.heap_hi = 0x40100000;
    CHECK_ST(runtime_init(&mem, cfg), Status::Ok);
    clear_imports();
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(runtime()->vfs.mount("game", game), Status::Ok);
    bool fatal = false;

    // Before the main module exists the request has no answer.
    CAPTURE_FATAL(load("xboxkrnl.exe"), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);

    CHECK(kimage::load_main(mem));
    strcpy((char*)mem.base() + name, "xboxkrnl.exe");
    CHECK_EQ(call(__imp__XexGetModuleHandle, name, output), 0u);
    const uint32_t kernel = word(output);
    CHECK_EQ(call(__imp__XexGetModuleHandle, 0, output), 0u);
    const uint32_t main = word(output);
    CHECK(kernel && main && kernel != main);
    CHECK_EQ(load_count(main), 1u);

    // Loaded modules: one more reference each time, the real HMODULE back.
    CHECK_EQ(load("ORIGINAL.XEX"), 0u);
    CHECK_EQ(word(output), main);
    CHECK_EQ(load_count(main), 2u);
    CHECK_EQ(load("game:\\original.xex"), 0u);
    CHECK_EQ(word(output), main);
    CHECK_EQ(load_count(main), 3u);
    CHECK_EQ(load("XboxKrnl.exe"), 0u);
    CHECK_EQ(word(output), kernel);
    CHECK_EQ(load_count(kernel), 2u);
    CHECK_EQ(load("xam.xex"), 0u);
    const uint32_t xam = word(output);
    CHECK(xam && xam != kernel && xam != main);

    // Output and name faults.
    CHECK_EQ(call(__imp__XexLoadImage, name, 0, 0, 0), nt::kAccessViolation);
    CHECK_EQ(call(__imp__XexLoadImage, 0x50000000u, 0, 0, output), nt::kAccessViolation);

    // Modules that are not part of the AOT build.
    CHECK_EQ(load("xbdm.xex"), kNoSuchFile);
    CHECK_EQ(word(output), 0xDEADBEEFu);
    CHECK_EQ(load("game:\\missing.xex"), nt::kObjectNameNotFound);
    CHECK_EQ(load("x:\\other.xex"), to_ntstatus(Status::NoSuchDevice));
    CAPTURE_FATAL(load("game:\\other.xex"), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(g_fatal_msg.find("recompiled into the title") != std::string::npos);
    // A minimum version without an established comparison.
    CAPTURE_FATAL(load("original.xex", 0x20000000u), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);

    // Unload releases references; the bootstrap reference is never released.
    CHECK_EQ(call(__imp__XexUnloadImage, main), 0u);
    CHECK_EQ(load_count(main), 2u);
    CHECK_EQ(call(__imp__XexUnloadImage, main), 0u);
    CHECK_EQ(load_count(main), 1u);
    CAPTURE_FATAL(call(__imp__XexUnloadImage, main), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_EQ(load_count(main), 1u);
    CHECK_EQ(call(__imp__XexUnloadImage, kernel), 0u);
    CHECK_EQ(load_count(kernel), 1u);
    CHECK_EQ(call(__imp__XexUnloadImage, scratch), nt::kInvalidHandle);
    CHECK_EQ(call(__imp__XexUnloadImage, 0), nt::kInvalidHandle);

    // RtlImageNtHeader: the loaded image, then the NT rejection rules.
    CHECK_EQ(call(__imp__RtlImageNtHeader, kimage::base), kimage::base + 0x80);
    CHECK_EQ(call(__imp__RtlImageNtHeader, 0), 0u);
    CHECK_EQ(call(__imp__RtlImageNtHeader, scratch + 0x1000), 0u);  // no "MZ"
    mem.base()[scratch + 0x1000] = 'M'; mem.base()[scratch + 0x1001] = 'Z';
    mem.base()[scratch + 0x103C] = 0x40;  // e_lfanew 0x40, little-endian
    CHECK_EQ(call(__imp__RtlImageNtHeader, scratch + 0x1000), 0u);  // no "PE\0\0"
    memcpy(mem.base() + scratch + 0x1040, "PE\0\0", 4);
    CHECK_EQ(call(__imp__RtlImageNtHeader, scratch + 0x1000), scratch + 0x1040);
    mem.base()[scratch + 0x103F] = 0x10;  // e_lfanew 0x10000040 >= 256 MiB
    CHECK_EQ(call(__imp__RtlImageNtHeader, scratch + 0x1000), 0u);
    CAPTURE_FATAL(call(__imp__RtlImageNtHeader, 0x50000000u), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);

    runtime_shutdown();
    clear_functions();
    clear_imports();
    mem.release();
    unlink((game + "/other.xex").c_str());
    rmdir(game.c_str());
    rmdir(top.c_str());
    return test_result("rt_kernel_xex");
}
