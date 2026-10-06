// xam.xex runtime services over original synthetic metadata and the scripted
// TESTDOUBLE input backend. No signed-in identity or online service is faked.
#include <string.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/input.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/modules.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xam.h"
#include "test_util.h"

#if defined(__cpp_exceptions)
static thread_local int TESTDOUBLE_host_allocations_until_failure = -1;
static void* TESTDOUBLE_host_allocate(size_t size) {
    if (TESTDOUBLE_host_allocations_until_failure == 0) throw std::bad_alloc();
    if (TESTDOUBLE_host_allocations_until_failure > 0) --TESTDOUBLE_host_allocations_until_failure;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t size) { return TESTDOUBLE_host_allocate(size); }
void* operator new[](size_t size) { return TESTDOUBLE_host_allocate(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
#endif

PPC_EXTERN_FUNC(__imp__XamInputGetState);
PPC_EXTERN_FUNC(__imp__XamInputGetCapabilities);
PPC_EXTERN_FUNC(__imp__XamInputSetState);
PPC_EXTERN_FUNC(__imp__XamAlloc);
PPC_EXTERN_FUNC(__imp__XamFree);
PPC_EXTERN_FUNC(__imp__XamGetExecutionId);
PPC_EXTERN_FUNC(__imp__XGetAVPack);
PPC_EXTERN_FUNC(__imp__XGetGameRegion);
PPC_EXTERN_FUNC(__imp__XGetLanguage);
PPC_EXTERN_FUNC(__imp__XGetVideoMode);
PPC_EXTERN_FUNC(__imp__XamEnableInactivityProcessing);
PPC_EXTERN_FUNC(__imp__XamResetInactivity);

extern rcomp_pad TESTDOUBLE_pad_state;
extern bool TESTDOUBLE_pad_connected;
extern uint16_t TESTDOUBLE_vibration[2];

using namespace rcomp;
using namespace rcomp::rt;

namespace {
uint8_t* g_base;
constexpr uint32_t kImageBase = 0x82000000;
constexpr uint32_t kImageSize = 0x3000;
constexpr uint32_t kImageEntry = kImageBase + 0x1000;
constexpr uint32_t kHeaderSize = 0x1000;

uint32_t rd32(uint32_t a) {
    uint32_t v = 0;
    guest_read_be32(a, &v);
    return v;
}
uint32_t call(PPCFunc* f, uint32_t r3, uint32_t r4, uint32_t r5) {
    alignas(64) PPCContext ctx{};
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    f(ctx, g_base);
    return ctx.r3.u32;
}

void be32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset + 0] = uint8_t(value >> 24);
    bytes[offset + 1] = uint8_t(value >> 16);
    bytes[offset + 2] = uint8_t(value >> 8);
    bytes[offset + 3] = uint8_t(value);
}

void le16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset + 0] = uint8_t(value);
    bytes[offset + 1] = uint8_t(value >> 8);
}

void le32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset + 0] = uint8_t(value);
    bytes[offset + 1] = uint8_t(value >> 8);
    bytes[offset + 2] = uint8_t(value >> 16);
    bytes[offset + 3] = uint8_t(value >> 24);
}

XexImage original_execution_image(GuestMemory& mem) {
    std::vector<uint8_t> image(kImageSize);
    le16(image, 0, 0x5A4D);
    le32(image, 0x3C, 0x80);
    const size_t nt = 0x80, optional = nt + 24;
    le32(image, nt, 0x4550);
    le16(image, nt + 4, 0x1F2);
    le16(image, nt + 6, 1);
    le32(image, nt + 8, 0x13572468);
    le16(image, nt + 20, 224);
    le16(image, optional, 0x10B);
    le32(image, optional + 16, 0x1000);
    le32(image, optional + 28, kImageBase);
    le32(image, optional + 32, 0x1000);
    le32(image, optional + 36, 0x200);
    le32(image, optional + 56, kImageSize);
    le32(image, optional + 60, 0x200);
    le32(image, optional + 64, 0x24681357);
    le32(image, optional + 92, 16);
    const size_t section = optional + 224;
    le32(image, section + 8, 0x1000);
    le32(image, section + 12, 0x1000);
    le32(image, section + 16, 0x1000);
    le32(image, section + 36, 0x60000020);
    CHECK_ST(load_raw_image(mem, kImageBase, image.data(), image.size()), Status::Ok);

    std::vector<uint8_t> header(kHeaderSize);
    be32(header, 0, 0x58455832);  // XEX2
    be32(header, 8, kHeaderSize);
    be32(header, 16, 0x400);
    be32(header, 20, 3);
    const uint32_t fields[][2] = {
        {0x00010100, kImageEntry},
        {0x00010201, kImageBase},
        {0x00040006, 0x100},
    };
    for (size_t i = 0; i < 3; ++i) {
        be32(header, 24 + i * 8, fields[i][0]);
        be32(header, 28 + i * 8, fields[i][1]);
    }
    // Original synthetic execution-info bytes: 4 BE words, 4 bytes, 1 BE word.
    be32(header, 0x100, 0x4A53F9F6);
    be32(header, 0x104, 0x00000006);
    be32(header, 0x108, 0x00000005);
    be32(header, 0x10C, 0x545407F2);
    header[0x110] = 0;
    header[0x111] = 0;
    header[0x112] = 1;
    header[0x113] = 2;
    be32(header, 0x114, 0x11223344);
    be32(header, 0x404, kImageSize);
    be32(header, 0x510, kImageBase);

    XexImage result{};
    result.base = kImageBase;
    result.size = kImageSize;
    result.entry_point = kImageEntry;
    result.header = std::move(header);
    return result;
}

void configure_xam() {
    XamRuntimeConfig config{};
    config.language = 1;
    config.game_region = 0x0101;
    config.av_pack = 6;
    config.video = {1280, 720, false, true, true, 60.0f};
    CHECK_ST(runtime_configure_xam(config), Status::Ok);
    CHECK_ST(runtime_configure_xam(config), Status::Conflict);
}
}  // namespace

int main() {
    GuestMemory mem;
    if (mem.reserve() != MemStatus::Ok) return 2;
    g_base = mem.base();
    CHECK_ST(runtime_init(&mem), Status::Ok);
    CHECK_ST(register_xam_hle(), Status::Ok);
    PPCFunc* const xam_alloc_impl = find_import(kModuleXam, 0x01EA);
    CHECK(xam_alloc_impl != nullptr);
    uint32_t buf = 0;
    CHECK_ST(runtime()->heap.alloc(0x1000, 16, true, &buf), Status::Ok);

    // Configuration is explicit per Runtime lifetime. Missing values do not
    // silently become an English/US/HD console profile.
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__XGetLanguage, 0, 0, 0), fatal);
    CHECK(fatal);
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_UNIMPLEMENTED);
    XamRuntimeConfig invalid{};
    invalid.video.refresh_rate_hz = 60.0f;
    CHECK_ST(runtime_configure_xam(invalid), Status::InvalidArgument);
    configure_xam();
    CHECK_EQ(call(__imp__XGetLanguage, 0, 0, 0), 1u);
    CHECK_EQ(call(__imp__XGetGameRegion, 0, 0, 0), 0x0101u);
    CHECK_EQ(call(__imp__XGetAVPack, 0, 0, 0), 6u);
    call(__imp__XGetVideoMode, buf + 0x300, 0, 0);
    CHECK_EQ(rd32(buf + 0x300), 1280u);
    CHECK_EQ(rd32(buf + 0x304), 720u);
    CHECK_EQ(rd32(buf + 0x308), 0u);
    CHECK_EQ(rd32(buf + 0x30C), 1u);
    CHECK_EQ(rd32(buf + 0x310), 1u);
    CHECK_EQ(rd32(buf + 0x314), 0x42700000u);  // 60.0f

    // The idle clock calls succeed and are recorded (the platform owns the idle policy); the raw arguments of the enable request are kept.
    CHECK_EQ(xam_inactivity_stats().resets, 0u);
    CHECK_EQ(call(__imp__XamResetInactivity, 0, 0, 0), 0u);
    CHECK_EQ(call(__imp__XamResetInactivity, 0, 1, 9), 0u);
    CHECK_EQ(call(__imp__XamEnableInactivityProcessing, 0, 1, 0), 0u);
    CHECK_EQ(xam_inactivity_stats().resets, 2u);
    CHECK_EQ(xam_inactivity_stats().enable_requests, 1u);
    CHECK_EQ(xam_inactivity_stats().last_enable_args[0], 0u);
    CHECK_EQ(xam_inactivity_stats().last_enable_args[1], 1u);

    // XamAlloc returns HRESULT and writes a guest pointer. Only allocations
    // made by XAM may be released through XamFree.
    auto before = runtime()->heap.stats();
    CHECK_EQ(call(__imp__XamAlloc, 0, 0x123, buf + 0x200), 0u);
    uint32_t xam_allocation = rd32(buf + 0x200);
    CHECK(xam_allocation != 0);
    CHECK(runtime()->mem->is_accessible(xam_allocation, 0x123, Protect::ReadWrite));
    CHECK_EQ(runtime()->heap.stats().live_allocations, before.live_allocations + 1);
    call(__imp__XamFree, xam_allocation, 0, 0);
    CHECK_EQ(runtime()->heap.stats().live_allocations, before.live_allocations);
    CAPTURE_FATAL(call(__imp__XamFree, xam_allocation, 0, 0), fatal);
    CHECK(fatal);
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_GUEST_ACCESS);

    CHECK_EQ(call(__imp__XamAlloc, 0x18000000u, 0x40, buf + 0x200), 0u);
    xam_allocation = rd32(buf + 0x200);
    call(__imp__XamFree, xam_allocation, 0, 0);
    CHECK_EQ(call(__imp__XamAlloc, 0, 0, buf + 0x200), 0x80070057u);
    CHECK_EQ(rd32(buf + 0x200), 0u);
    CAPTURE_FATAL(call(__imp__XamAlloc, 1, 0x40, buf + 0x200), fatal);
    CHECK(fatal);
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_UNIMPLEMENTED);
#if defined(__cpp_exceptions)
    // XAM stages its ownership node before touching GuestHeap, so host metadata
    // OOM cannot create an untracked guest allocation.
    const auto before_host_oom = runtime()->heap.stats();
    CHECK(guest_write_be32(buf + 0x200, 0xBAD0CAFE));
    TESTDOUBLE_host_allocations_until_failure = 0;
    CHECK_EQ(call(xam_alloc_impl, 0, 0x40, buf + 0x200), 0x8007000Eu);
    TESTDOUBLE_host_allocations_until_failure = -1;
    CHECK_EQ(rd32(buf + 0x200), 0u);
    CHECK_EQ(runtime()->heap.stats().allocated_bytes, before_host_oom.allocated_bytes);
    CHECK_EQ(runtime()->heap.stats().live_allocations, before_host_oom.live_allocations);
#endif
    uint32_t foreign = 0;
    CHECK_ST(runtime()->heap.alloc(0x40, 16, false, &foreign), Status::Ok);
    CAPTURE_FATAL(call(__imp__XamFree, foreign, 0, 0), fatal);
    CHECK(fatal);
    CHECK_ST(runtime()->heap.free(foreign), Status::Ok);

    // Allocate/free concurrently to exercise address reuse while XAM ownership
    // and GuestHeap bookkeeping are both active.
    const auto before_stress = runtime()->heap.stats();
    std::atomic<uint32_t> stress_failures{0};
    constexpr unsigned kStressThreads = 4, kStressIterations = 250;
    std::thread stress[kStressThreads];
    for (unsigned thread = 0; thread < kStressThreads; ++thread) {
        stress[thread] = std::thread([&, thread] {
            const uint32_t output = buf + 0x400 + thread * 4;
            for (unsigned iteration = 0; iteration < kStressIterations; ++iteration) {
                if (call(__imp__XamAlloc, 0, 0x30 + (iteration & 0xF), output) != 0) {
                    ++stress_failures;
                    continue;
                }
                const uint32_t allocation = rd32(output);
                if (!allocation) {
                    ++stress_failures;
                    continue;
                }
                call(__imp__XamFree, allocation, 0, 0);
            }
        });
    }
    for (auto& thread : stress) thread.join();
    CHECK_EQ(stress_failures.load(), 0u);
    CHECK_EQ(runtime()->heap.stats().allocated_bytes, before_stress.allocated_bytes);
    CHECK_EQ(runtime()->heap.stats().live_allocations, before_stress.live_allocations);

    TESTDOUBLE_pad_state = {uint16_t(RCOMP_XINPUT_A | RCOMP_XINPUT_DPAD_UP), 7, 200, -32768, 32767, 100, -100};
    CHECK_EQ(call(__imp__XamInputGetState, 0, 1, buf), 0u);
    const uint32_t packet1 = rd32(buf);
    CHECK_EQ(rd32(buf + 4), 0x100107C8u);  // buttons 0x1001, lt 7, rt 200
    CHECK_EQ(rd32(buf + 8), 0x80007FFFu);  // lx -32768, ly 32767
    CHECK_EQ(rd32(buf + 12), 0x0064FF9Cu); // rx 100, ry -100
    CHECK_EQ(call(__imp__XamInputGetState, 0xFF, 1, buf), 0u);  // any user -> controller 0
    CHECK_EQ(rd32(buf), packet1);  // unchanged state, same packet number
    TESTDOUBLE_pad_state.buttons = RCOMP_XINPUT_B;
    CHECK_EQ(call(__imp__XamInputGetState, 0, 1, buf), 0u);
    CHECK(rd32(buf) != packet1);
    CHECK_EQ(rd32(buf + 4) >> 16, (uint32_t)RCOMP_XINPUT_B);
    CHECK_EQ(call(__imp__XamInputGetState, 1, 1, buf), 0x48Fu);  // not connected
    CHECK_EQ(call(__imp__XamInputGetState, 4, 1, buf), 0xA0u);   // bad user index

    CHECK_EQ(call(__imp__XamInputGetCapabilities, 0, 1, buf), 0u);
    CHECK_EQ(rd32(buf), 0x01010000u);  // gamepad / gamepad, flags 0
    CHECK_EQ(rd32(buf + 4), 0xF3FFFFFFu);
    CHECK_EQ(rd32(buf + 16), 0u);  // no vibration advertised

    guest_write_be32(buf + 0x100, 0x12345678);
    CHECK_EQ(call(__imp__XamInputSetState, 0, 0, buf + 0x100), 0u);
    CHECK_EQ(TESTDOUBLE_vibration[0], 0x1234u);
    CHECK_EQ(TESTDOUBLE_vibration[1], 0x5678u);
    TESTDOUBLE_pad_connected = false;
    CHECK_EQ(call(__imp__XamInputGetState, 0, 1, buf), 0x48Fu);
    CHECK_EQ(call(__imp__XamInputGetCapabilities, 0, 1, buf), 0x48Fu);
    CHECK_EQ(call(__imp__XamInputSetState, 0, 0, buf + 0x100), 0x48Fu);

    // Buffer rights are checked for the whole structure before the backend or
    // any partial guest write is attempted.
    TESTDOUBLE_pad_connected = true;
    uint32_t protected_page = 0;
    CHECK_ST(runtime()->heap.alloc(0x10000, 0x10000, true, &protected_page), Status::Ok);
    CHECK(mem.protect(protected_page, 0x10000, Protect::Read) == MemStatus::Ok);
    const auto before_bad_alloc_output = runtime()->heap.stats();
    CAPTURE_FATAL(call(__imp__XamAlloc, 0, 0x40, protected_page), fatal);
    CHECK(fatal);
    CHECK_EQ(runtime()->heap.stats().allocated_bytes, before_bad_alloc_output.allocated_bytes);
    CHECK_EQ(runtime()->heap.stats().live_allocations, before_bad_alloc_output.live_allocations);
    CAPTURE_FATAL(call(__imp__XamInputGetState, 0, 0, protected_page), fatal);
    CHECK(fatal);
    CAPTURE_FATAL(call(__imp__XamInputGetCapabilities, 0, 0, protected_page), fatal);
    CHECK(fatal);
    CAPTURE_FATAL(call(__imp__XGetVideoMode, protected_page, 0, 0), fatal);
    CHECK(fatal);
    CHECK(mem.protect(protected_page, 0x10000, Protect::None) == MemStatus::Ok);
    CAPTURE_FATAL(call(__imp__XamInputSetState, 0, 0, protected_page), fatal);
    CHECK(fatal);
    CHECK(mem.protect(protected_page, 0x10000, Protect::ReadWrite) == MemStatus::Ok);

    // XamGetExecutionId exposes the exact 0x40006 payload retained from the
    // finalized XEX header. It does not synthesize title/version identity.
    CAPTURE_FATAL(call(__imp__XamGetExecutionId, buf + 0x240, 0, 0), fatal);
    CHECK(fatal);
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_ST(runtime_prepare_main_module({"game:\\Original.xex", "Original.xex"}), Status::Ok);
    XexImage image = original_execution_image(mem);
    CHECK_ST(runtime_finalize_main_module(image), Status::Ok);
    CHECK_EQ(call(__imp__XamGetExecutionId, buf + 0x240, 0, 0), 0u);
    const uint32_t execution = rd32(buf + 0x240);
    CHECK(mem.is_accessible(execution, 24, Protect::Read));
    CHECK(!mem.is_accessible(execution, 24, Protect::ReadWrite));
    CHECK_EQ(rd32(execution + 0), 0x4A53F9F6u);
    CHECK_EQ(rd32(execution + 4), 0x00000006u);
    CHECK_EQ(rd32(execution + 8), 0x00000005u);
    CHECK_EQ(rd32(execution + 12), 0x545407F2u);
    CHECK_EQ(rd32(execution + 16), 0x00000102u);
    CHECK_EQ(rd32(execution + 20), 0x11223344u);
    CAPTURE_FATAL(call(__imp__XamGetExecutionId, execution, 0, 0), fatal);
    CHECK(fatal);
    CHECK_EQ(g_fatal_kind, (int)RCOMP_FATAL_GUEST_ACCESS);

    // A new Runtime owns a fresh XAM state: config and packet history cannot
    // leak from the prior title.
    runtime_shutdown();
    clear_imports();
    clear_functions();
    mem.release();
    CHECK(mem.reserve() == MemStatus::Ok);
    g_base = mem.base();
    CHECK_ST(runtime_init(&mem), Status::Ok);
    CHECK_ST(register_xam_hle(), Status::Ok);
    CAPTURE_FATAL(call(__imp__XGetLanguage, 0, 0, 0), fatal);
    CHECK(fatal);
    uint32_t second_buf = 0;
    CHECK_ST(runtime()->heap.alloc(0x1000, 16, true, &second_buf), Status::Ok);
    TESTDOUBLE_pad_connected = true;
    TESTDOUBLE_pad_state = {RCOMP_XINPUT_B, 0, 0, 0, 0, 0, 0};
    CHECK_EQ(call(__imp__XamInputGetState, 0, 0, second_buf), 0u);
    CHECK_EQ(rd32(second_buf), 1u);

    runtime_shutdown();
    clear_imports();
    mem.release();
    return test_result("rt_xam");
}
