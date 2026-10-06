// Original fixture only (patch0020_window.s). Runs the unmodified generated C++ of the
// synthetic XEX, compiled with include/rcomp/ppc_prelude.h in one of its access modes
// (plain, RCOMP_CHECKED_GUEST_ACCESS, RCOMP_VIRTUAL_GUEST_ACCESS, RCOMP_STORE_COMPARE) with
// RCOMP_PHYSICAL_4K_WINDOW_OFFSET 0 (default) or 1. Expected host locations and GPU
// write-tracking pages are computed here from the console's address map, independently of
// the prelude:
//   host offset   = ea + 0x1000 when the option is 1 and ea >= 0xE0000000, else ea
//   physical page = ((ea - 0xA0000000 + that shift) mod 512 MiB) / 4 KiB for ea >= 0xA0000000
// The host mapping models GuestMemory with the page past 4 GiB that the last page of the
// 0xE0000000 window reaches when the option is 1 (rcomp/guest_memory.h).
#include <ppc_recomp_shared.h>
#include <patch0020_decls.h>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sys/mman.h>

namespace {
uint8_t* base = nullptr;
int failures = 0;
constexpr bool kShift = RCOMP_PHYSICAL_4K_WINDOW_OFFSET != 0;
constexpr uint32_t kStack = 0x40000000u;
constexpr uint64_t kMapSize = 0x100000000ull + 0x20000;

#if defined(RCOMP_CHECKED_GUEST_ACCESS)
constexpr const char* kChecked = "checked";
#else
constexpr const char* kChecked = "unchecked";
#endif
#if defined(RCOMP_VIRTUAL_GUEST_ACCESS)
constexpr const char* kVirtual = "virtual";
#else
constexpr const char* kVirtual = "plain";
#endif
#if defined(RCOMP_STORE_COMPARE)
constexpr const char* kCompare = "store_compare";
#else
constexpr const char* kCompare = "store_marks";
#endif

uint64_t host(uint32_t ea) { return uint64_t(ea) + (kShift && ea >= 0xE0000000u ? 0x1000u : 0u); }
// The host offset the other convention would use (only meaningful inside the 0xE0000000 window).
uint64_t other(uint32_t ea) { return kShift ? uint64_t(ea) : uint64_t(ea) + 0x1000u; }
uint32_t page(uint32_t ea) {
    return uint32_t(((uint64_t(ea) - 0xA0000000u + (kShift && ea >= 0xE0000000u ? 0x1000u : 0u)) & 0x1FFFFFFFu) >> 12);
}
// A checked access asks about the unshifted guest address: GuestMemory::is_accessible applies
// the shift itself (GuestMemory::set_physical_4k_offset), so the prelude must not shift it twice.
uint32_t check_address(uint32_t ea) { return ea; }

void check(const char* window, const char* what, uint64_t got, uint64_t expected) {
    const bool ok = got == expected;
    if (!ok || std::getenv("PATCH0020_VERBOSE"))
        std::printf("patch0020/%s/%s %s got=0x%llX expected=0x%llX\n", window, what, ok ? "PASS" : "FAIL",
                    (unsigned long long)got, (unsigned long long)expected);
    failures += !ok;
}

uint64_t be(uint64_t at, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = v << 8 | base[at + i];
    return v;
}
void put_be(uint64_t at, uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) base[at + i] = uint8_t(value >> (8 * (bytes - 1 - i)));
}

struct Region { uint32_t guest; uint32_t size; };
// Guest ranges mapped by the harness (host = base + guest; the last one ends past 4 GiB).
constexpr Region kRegions[] = {
    {kStack, 0x10000}, {0xA0010000u, 0x10000}, {0xE0010000u, 0x20000}, {0xFFFF0000u, 0x10000}};
// Host offsets the harness maps: the image, the regions, the start of the 0xE0000000 window
// and the page past 4 GiB (both alias physical 0 in this model).
bool mapped(uint64_t at, uint32_t size) {
    if (at >= PPC_IMAGE_BASE && at + size <= PPC_IMAGE_BASE + PPC_IMAGE_SIZE) return true;
    if (at >= 0xE0000000u && at + size <= 0xE0010000u) return true;
    if (at >= 0x100000000ull && at + size <= 0x100010000ull) return true;
    for (const Region& r : kRegions)
        if (at >= r.guest && at + size <= uint64_t(r.guest) + r.size) return true;
    return false;
}
// Models GuestMemory::is_accessible(guest, ...): it takes the guest address and applies the
// shift itself when the title runs with the option (GuestMemory::host_address).
bool committed(uint32_t address, uint32_t size) { return mapped(host(address), size); }

void clear() {
    for (const Region& r : kRegions) std::memset(base + r.guest, 0, r.size);
    std::memset(base + 0x100000000ull, 0, 0x10000);
    std::memset(base + 0xE0000000u, 0, 0x10000);
    std::memset(rcomp::g_guest_physical_written, 0, sizeof rcomp::g_guest_physical_written);
}
std::set<uint32_t> marks() {
    std::set<uint32_t> pages;
    for (uint32_t p = 0; p < rcomp::kGuestWritePages; ++p)
        if (rcomp::g_guest_physical_written[p]) pages.insert(p);
    return pages;
}
void check_marks(const char* window, const char* what, uint32_t ea) {
    const std::set<uint32_t> got = marks();
    const bool ok = got.size() == 1 && *got.begin() == page(ea);
    check(window, what, ok ? page(ea) : (got.empty() ? 0xFFFFFFFFu : *got.begin() | (uint64_t(got.size()) << 32)), page(ea));
}
void check_no_marks(const char* window, const char* what) { check(window, what, marks().size(), 0); }

uint64_t call(PPCFunc* fn, uint64_t r3, uint64_t r4 = 0) {
    PPCContext ctx{};
    ctx.r1.u64 = kStack + 0x8000;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    fn(ctx, base);
    return ctx.r3.u64;
}
}  // namespace

// Out-of-line hooks of the prelude's checked and virtual modes, implemented by the harness
// against its own mapping (TESTDOUBLE_: tests only; cpu/runtime/guest_access.cpp is the title's).
namespace {
uint32_t TESTDOUBLE_last_check = 0;
uint32_t TESTDOUBLE_checks = 0;
}
namespace rcomp {
bool guest_access_permitted(uint32_t addr, uint32_t size, int) {
    TESTDOUBLE_last_check = addr;
    ++TESTDOUBLE_checks;
    return committed(addr, size);
}
bool guest_access_ok(uint32_t addr, uint32_t size) { return committed(addr, size); }
void bad_guest_access(uint32_t addr, uint32_t size, int is_store) {
    std::printf("patch0020/bad_guest_access FAIL addr=0x%08X size=%u store=%d\n", addr, size, is_store);
    std::exit(1);
}
#if defined(RCOMP_VIRTUAL_GUEST_ACCESS)
uint64_t virtual_window_load(uint8_t*, uint32_t address, uint8_t, uint32_t) {
    std::printf("patch0020/virtual_window_load FAIL unexpected addr=0x%08X\n", address);
    std::exit(1);
}
void virtual_window_store(uint8_t*, uint32_t address, uint8_t, uint64_t, uint32_t) {
    std::printf("patch0020/virtual_window_store FAIL unexpected addr=0x%08X\n", address);
    std::exit(1);
}
#endif
}  // namespace rcomp

namespace {
void run_window(const char* window, uint32_t a) {
    const bool in_e = a >= 0xE0000000u;
    // stw / lwz
    clear();
    TESTDOUBLE_checks = 0;
    call(execute_store_word, a, 0x11223344u);
    check(window, "stw_host", be(host(a), 4), 0x11223344u);
    if (in_e) check(window, "stw_other_untouched", be(other(a), 4), 0);
    check_marks(window, "stw_marks", a);
#if defined(RCOMP_CHECKED_GUEST_ACCESS)
    check(window, "stw_check_address", TESTDOUBLE_last_check, check_address(a));
    check(window, "stw_checked", TESTDOUBLE_checks != 0, 1);
#endif
    clear();
    put_be(host(a), 0xCAFEBABEu, 4);
    check(window, "lwz", call(execute_load_word, a), 0xCAFEBABEu);
    check_no_marks(window, "lwz_marks");

    // stb / sth / std, then lbz / lhz / ld
    clear();
    const uint64_t d = 0x0123456789ABCDEFull;
    call(execute_store_mixed, a, d);
    check(window, "stb_host", be(host(a + 4), 1), 0xEF);
    check(window, "sth_host", be(host(a + 6), 2), 0xCDEF);
    check(window, "std_host", be(host(a + 8), 8), d);
    if (in_e) check(window, "std_other_untouched", be(other(a + 8), 8), 0);
    check_marks(window, "stb_sth_std_marks", a);
    std::memset(rcomp::g_guest_physical_written, 0, sizeof rcomp::g_guest_physical_written);
    check(window, "lbz_lhz", call(execute_load_mixed, a), 0xEFCDEFu);
    check(window, "ld", call(execute_load_double, a), d);
    check_no_marks(window, "loads_marks");

    // lvx + stvx
    clear();
    for (int i = 0; i < 16; ++i) base[host(a + 0x20) + i] = uint8_t(0x10 + i);
    call(execute_vector_copy, a + 0x20, a + 0x40);
    for (int i = 0; i < 16; ++i) check(window, "lvx_stvx", base[host(a + 0x40) + i], uint8_t(0x10 + i));
    if (in_e) check(window, "stvx_other_untouched", be(other(a + 0x40), 8), 0);
    check_marks(window, "stvx_marks", a);

    // lvlx + lvrx (unaligned load)
    clear();
    for (int i = 0; i < 32; ++i) base[host(a + 0x60) + i] = uint8_t(0x80 + i);
    call(execute_unaligned_load, a + 0x65, a + 0xA0);
    for (int i = 0; i < 16; ++i) check(window, "lvlx_lvrx", base[host(a + 0xA0) + i], uint8_t(0x85 + i));

    // stvlx + stvrx (unaligned store)
    clear();
    for (int i = 0; i < 16; ++i) base[host(a + 0xC0) + i] = uint8_t(0x40 + i);
    call(execute_unaligned_store, a + 0xE3, a + 0xC0);
    for (int i = 0; i < 16; ++i) check(window, "stvlx_stvrx", base[host(a + 0xE3) + i], uint8_t(0x40 + i));
    check(window, "stvlx_before", base[host(a + 0xE2)], 0);
    check(window, "stvrx_after", base[host(a + 0xF3)], 0);
    if (in_e) check(window, "stvlx_other_untouched", be(other(a + 0xE3), 8), 0);
    check_marks(window, "stvlx_stvrx_marks", a);

    // lvewx (whole line) + stvx; stvewx (one word)
    clear();
    for (int i = 0; i < 16; ++i) base[host(a + 0x100) + i] = uint8_t(0xC0 + i);
    call(execute_load_element, a + 0x108, a + 0x110);
    for (int i = 0; i < 16; ++i) check(window, "lvewx", base[host(a + 0x110) + i], uint8_t(0xC0 + i));
    clear();
    for (int i = 0; i < 4; ++i) put_be(host(a + 0x120 + 4 * i), 0xA0A0A0A0u + i, 4);
    call(execute_store_element, a + 0x148, a + 0x120);
    check(window, "stvewx", be(host(a + 0x148), 4), 0xA0A0A0A2u);
    check(window, "stvewx_neighbours", be(host(a + 0x140), 8) | be(host(a + 0x14C), 4), 0);
    if (in_e) check(window, "stvewx_other_untouched", be(other(a + 0x148), 4), 0);
    check_marks(window, "stvewx_marks", a);

    // dcbz (32-byte block) and dcbzl (128-byte block)
    clear();
    std::memset(base + host(a + 0x180), 0xFF, 0x60);
    call(execute_dcbz, a + 0x1A5);
    check(window, "dcbz_block", be(host(a + 0x1A0), 8) | be(host(a + 0x1B8), 8), 0);
    check(window, "dcbz_before", base[host(a + 0x19F)], 0xFF);
    check(window, "dcbz_after", base[host(a + 0x1C0)], 0xFF);
    check_marks(window, "dcbz_marks", a);
    clear();
    std::memset(base + host(a + 0x200), 0xFF, 0x120);
    call(execute_dcbzl, a + 0x2C1);
    uint64_t any = 0;
    for (int i = 0; i < 128; ++i) any |= base[host(a + 0x280) + i];
    check(window, "dcbzl_block", any, 0);
    check(window, "dcbzl_before", base[host(a + 0x27F)], 0xFF);
    check(window, "dcbzl_after", base[host(a + 0x300)], 0xFF);
    if (in_e) check(window, "dcbzl_other_untouched", base[other(a + 0x27F)] | base[other(a + 0x280)], 0);
    check_marks(window, "dcbzl_marks", a);

    // lwarx/stwcx. and ldarx/stdcx.
    clear();
    put_be(host(a + 0x340), 5, 4);
    check(window, "lwarx_stwcx_result", call(execute_atomic_add32, a + 0x340, 3), 8);
    check(window, "lwarx_stwcx_host", be(host(a + 0x340), 4), 8);
    check_marks(window, "stwcx_marks", a);
    clear();
    put_be(host(a + 0x348), 0x100000005ull, 8);
    check(window, "ldarx_stdcx_result", call(execute_atomic_add64, a + 0x348, 3), 0x100000008ull);
    check(window, "ldarx_stdcx_host", be(host(a + 0x348), 8), 0x100000008ull);
    check_marks(window, "stdcx_marks", a);

    // setjmp/longjmp with the jmp_buf in guest memory
    clear();
    check(window, "setjmp_longjmp", call(execute_jump, a + 0x400), 5);
    uint64_t written = 0, stray = 0;
    for (size_t i = 0; i < sizeof(jmp_buf); ++i) {
        written |= base[host(a + 0x400) + i];
        if (in_e) stray |= base[other(a + 0x400) + i];
    }
    check(window, "jmp_buf_host_written", written != 0, 1);
    if (in_e) check(window, "jmp_buf_other_untouched", stray, 0);
    check_marks(window, "setjmp_marks", a);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::fseek(f, 0, SEEK_END);
    const size_t bytes = size_t(std::ftell(f));
    std::rewind(f);
    base = static_cast<uint8_t*>(mmap(nullptr, kMapSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (base == MAP_FAILED || bytes > PPC_IMAGE_SIZE ||
        mprotect(base + PPC_IMAGE_BASE, PPC_IMAGE_SIZE, PROT_READ | PROT_WRITE) ||
        mprotect(base + 0xE0000000u, 0x10000, PROT_READ | PROT_WRITE) ||
        mprotect(base + 0x100000000ull, 0x10000, PROT_READ | PROT_WRITE)) return 2;
    for (const Region& r : kRegions)
        if (mprotect(base + r.guest, r.size, PROT_READ | PROT_WRITE)) return 2;
    if (std::fread(base + PPC_IMAGE_BASE, 1, bytes, f) != bytes) return 2;
    std::fclose(f);

    std::printf("patch0020/mode offset=%d %s %s %s\n", int(kShift), kVirtual, kChecked, kCompare);
    run_window("A_window", 0xA0010100u);
    run_window("E_window", 0xE0010100u);
    run_window("E_last_page", 0xFFFFF100u);
    std::printf("patch0020/execution %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
