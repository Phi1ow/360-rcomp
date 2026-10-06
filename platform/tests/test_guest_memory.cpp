// Guest memory contract tests (owner: Agent 2). Plain C++, no framework.
//
// Built three ways (platform/CMakeLists.txt):
//   host, POSIX backend           test_guest_memory
//   host, PS5 backend + TESTDOUBLE kernel   test_guest_memory_ps5emu
//   PS5 (payload ELF / title)      same source; faults observed with
//                                  sigsetjmp/siglongjmp because a PS5
//                                  process cannot fork.
// Output: one line per test, "PASS <name>", "FAIL <name>: <detail>" or
// "SKIP <name>: <reason>", then "SUMMARY pass=N fail=N skip=N".
// Exit status: 0 all pass, 1 any failure.
//
// Define RCOMP_SELFTEST_NO_MAIN to link the suite into another program (the
// PS5 test title calls rcomp_platform_selftest()).
#include "rcomp/guest_memory.h"
#if defined(__CYGWIN__)
#include "../common/os_vm.h"
#endif

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__PROSPERO__) || defined(__CYGWIN__)
#define RCOMP_TEST_HAVE_FORK 0
#else
#define RCOMP_TEST_HAVE_FORK 1
#include <sys/resource.h>
#include <sys/wait.h>
#endif

#if defined(RCOMP_TEST_PS5_BACKEND)
#include "../ps5/os_vm_ps5.h"
#endif
#if defined(RCOMP_TEST_TESTDOUBLE_PS5)
extern "C" void TESTDOUBLE_ps5_fail_allocate_after(long n);
extern "C" uint64_t TESTDOUBLE_ps5_direct_used_bytes();
#endif

using namespace rcomp;

namespace {

FILE* g_out = nullptr;
int g_pass = 0, g_fail = 0, g_skip = 0;
bool g_cur_failed = false;
const char* g_cur = "";

void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
    fflush(g_out);
}

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            out("FAIL %s: %s:%d: CHECK(%s)\n", g_cur, __FILE__, __LINE__, #cond);     \
            g_cur_failed = true;                                                      \
        }                                                                             \
    } while (0)

#define CHECK_ST(expr, want)                                                          \
    do {                                                                              \
        MemStatus s_ = (expr);                                                        \
        if (s_ != (want)) {                                                           \
            out("FAIL %s: %s:%d: %s = %s, want %s\n", g_cur, __FILE__, __LINE__,      \
                #expr, mem_status_name(s_), mem_status_name(want));                   \
            g_cur_failed = true;                                                      \
        }                                                                             \
    } while (0)

constexpr uint64_t P = kGuestPageSize;
constexpr uint64_t G4 = kGuestSpaceSize;

// ---- fault probing --------------------------------------------------------

enum class Access { Read, Write };
struct Probe {
    volatile uint8_t* p;
    Access a;
};

void touch(const Probe& pr) {
    if (pr.a == Access::Read) {
        volatile uint8_t v = *pr.p;
        (void)v;
    } else {
        *pr.p = 0x5A;
    }
}

#if !RCOMP_TEST_HAVE_FORK
sigjmp_buf g_jmp;
volatile sig_atomic_t g_sig = 0;
void on_fault(int sig) {
    g_sig = sig;
    siglongjmp(g_jmp, 1);
}
#endif

// Returns true if the access faulted (SIGSEGV or SIGBUS).
bool faults(volatile uint8_t* p, Access a) {
    Probe pr{p, a};
#if RCOMP_TEST_HAVE_FORK
    fflush(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        touch(pr);
        _exit(0);
    }
    if (pid < 0) {
        out("FAIL %s: fork failed errno=%d\n", g_cur, errno);
        g_cur_failed = true;
        return false;
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS);
#else
    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    g_sig = 0;
    bool faulted = false;
    if (sigsetjmp(g_jmp, 1) == 0) {
        touch(pr);
    } else {
        faulted = true;
    }
    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGBUS, &old_bus, nullptr);
    return faulted;
#endif
}

// ---- helpers ---------------------------------------------------------------

void begin(const char* name) {
    g_cur = name;
    g_cur_failed = false;
}
void end() {
    if (g_cur_failed) ++g_fail;
    else {
        ++g_pass;
        out("PASS %s\n", g_cur);
    }
}
[[maybe_unused]] void skip(const char* name, const char* why) {
    ++g_skip;
    out("SKIP %s: %s\n", name, why);
}

bool all_equal(const uint8_t* p, size_t n, uint8_t v) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != v) return false;
    return true;
}

#if RCOMP_TEST_HAVE_FORK && defined(__linux__)
// Virtual size of this process in KiB (Linux), or 0.
unsigned long vm_size_kib() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    unsigned long v = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "VmSize: %lu kB", &v) == 1) break;
    fclose(f);
    return v;
}
#endif

// ---- tests -----------------------------------------------------------------

void test_reserve_release() {
    begin("reserve_release");
    GuestMemory m;
    CHECK(!m.reserved());
    CHECK_ST(m.commit(0, P, Protect::ReadWrite), MemStatus::NotReserved);
    CHECK_ST(m.reserve(), MemStatus::Ok);
    CHECK(m.reserved());
    CHECK(((uintptr_t)m.base() % kBaseAlignment) == 0);
    GuestMemoryStats s = m.stats();
    CHECK(s.reserved_bytes == G4 + kGuestGuardSize);
    CHECK(s.committed_bytes == 0);
    CHECK_ST(m.reserve(), MemStatus::InvalidArgument);  // already reserved
    m.release();
    CHECK(!m.reserved());
    CHECK(m.stats().reserved_bytes == 0);
    m.release();  // idempotent
    CHECK_ST(m.commit(0, P, Protect::ReadWrite), MemStatus::NotReserved);
    CHECK_ST(m.decommit(0, P), MemStatus::NotReserved);
    CHECK_ST(m.protect(0, P, Protect::Read), MemStatus::NotReserved);
    CHECK(m.translate(0, 1) == nullptr);
    end();
}

void test_commit_bounds() {
    begin("commit_bounds");
    GuestMemory m;
    CHECK_ST(m.reserve(), MemStatus::Ok);
    CHECK_ST(m.commit(G4 - P, P, Protect::ReadWrite), MemStatus::Ok);
    if (m.is_committed(G4 - P, P)) {
        m.base()[G4 - 1] = 0x77;
        CHECK(m.base()[G4 - 1] == 0x77);
    }
    CHECK_ST(m.commit(G4, P, Protect::ReadWrite), MemStatus::InvalidArgument);
    CHECK_ST(m.commit(G4 - P, 2 * P, Protect::ReadWrite), MemStatus::InvalidArgument);
    CHECK_ST(m.commit(0x1000, P, Protect::ReadWrite), MemStatus::InvalidArgument);
    CHECK_ST(m.commit(0, 0x1000, Protect::ReadWrite), MemStatus::InvalidArgument);
    CHECK_ST(m.commit(0, 0, Protect::ReadWrite), MemStatus::InvalidArgument);
    CHECK_ST(m.commit(0, P, Protect::None), MemStatus::InvalidArgument);
    // size overflow: addr + size wraps past 2^64
    CHECK_ST(m.commit(P, ~0ull - (P - 1), Protect::ReadWrite), MemStatus::InvalidArgument);
    CHECK_ST(m.commit(G4 - P, ~0ull - (P - 1), Protect::ReadWrite), MemStatus::InvalidArgument);
    CHECK_ST(m.commit(~0ull - (P - 1), P, Protect::ReadWrite), MemStatus::InvalidArgument);
    CHECK_ST(m.decommit(G4, P), MemStatus::InvalidArgument);
    CHECK_ST(m.decommit(0x1000, P), MemStatus::InvalidArgument);
    CHECK_ST(m.decommit(P, ~0ull - (P - 1)), MemStatus::InvalidArgument);
    CHECK_ST(m.protect(G4, P, Protect::Read), MemStatus::InvalidArgument);
    CHECK(!m.is_committed(G4 - 1, 2));
    CHECK(m.is_committed(G4 - 1, 1));
    CHECK(m.stats().committed_bytes == P);
    end();
}

void test_write_read() {
    begin("write_read");
    GuestMemory m;
    CHECK_ST(m.reserve(), MemStatus::Ok);
    const uint64_t a = 0x10000, n = 4 * P;
    CHECK_ST(m.commit(a, n, Protect::ReadWrite), MemStatus::Ok);
    if (m.is_committed(a, n)) {
        uint8_t* p = m.base() + a;
        CHECK(all_equal(p, n, 0));  // fresh pages are zero
        for (uint64_t i = 0; i < n; ++i) p[i] = (uint8_t)(i * 131 + 7);
        bool ok = true;
        for (uint64_t i = 0; i < n; ++i) ok &= p[i] == (uint8_t)(i * 131 + 7);
        CHECK(ok);
    }
    CHECK(m.translate((uint32_t)a, (uint32_t)n) == m.base() + a);
    CHECK(m.translate((uint32_t)(a + n - 1), 1) == m.base() + a + n - 1);
    CHECK(m.translate((uint32_t)(a + n - 1), 2) == nullptr);
    CHECK(m.translate((uint32_t)a, 0) == nullptr);
    CHECK(m.is_committed(a + 5, n - 5));
    CHECK(!m.is_committed(a - 1, 2));
    CHECK(!m.is_committed(0, 0));
    // Re-committing an overlapping range keeps existing contents.
    if (m.is_committed(a, n)) {
        m.base()[a + 3] = 0xC3;
        CHECK_ST(m.commit(a - P, n + 2 * P, Protect::ReadWrite), MemStatus::Ok);
        CHECK(m.base()[a + 3] == 0xC3);
        CHECK(all_equal(m.base() + a - P, P, 0));
        CHECK(all_equal(m.base() + a + n, P, 0));
    }
    end();
}

void test_decommit_zeroes() {
    begin("decommit_zeroes");
    GuestMemory m;
    CHECK_ST(m.reserve(), MemStatus::Ok);
    const uint64_t a = 0x20000000;
    CHECK_ST(m.commit(a, 3 * P, Protect::ReadWrite), MemStatus::Ok);
    if (m.is_committed(a, 3 * P)) {
        memset(m.base() + a, 0xAB, 3 * P);
        CHECK_ST(m.decommit(a + P, P), MemStatus::Ok);  // middle page only
        CHECK(!m.is_committed(a + P, 1));
        CHECK(m.is_committed(a, P) && m.is_committed(a + 2 * P, P));
        CHECK(all_equal(m.base() + a, P, 0xAB));
        CHECK(all_equal(m.base() + a + 2 * P, P, 0xAB));
        CHECK(faults(m.base() + a + P, Access::Read));
        CHECK_ST(m.commit(a + P, P, Protect::ReadWrite), MemStatus::Ok);
        CHECK(all_equal(m.base() + a + P, P, 0));
        CHECK_ST(m.decommit(a, 3 * P), MemStatus::Ok);
        CHECK_ST(m.commit(a, 3 * P, Protect::ReadWrite), MemStatus::Ok);
        CHECK(all_equal(m.base() + a, 3 * P, 0));
    }
    // Decommitting never-committed pages is allowed and changes nothing.
    CHECK_ST(m.decommit(0x40000000, 2 * P), MemStatus::Ok);
    end();
}

void test_protect() {
    begin("protect_read_faults");
    GuestMemory m;
    CHECK_ST(m.reserve(), MemStatus::Ok);
    const uint64_t a = 0x30000;
    CHECK_ST(m.protect(a, P, Protect::Read), MemStatus::InvalidArgument);  // not committed
    CHECK_ST(m.commit(a, P, Protect::ReadWrite), MemStatus::Ok);
    if (m.is_committed(a, P)) {
        m.base()[a] = 0x11;
        CHECK_ST(m.protect(a, P, Protect::Read), MemStatus::Ok);
        CHECK(m.base()[a] == 0x11);
        CHECK(!faults(m.base() + a, Access::Read));
        CHECK(faults(m.base() + a, Access::Write));
        CHECK(m.base()[a] == 0x11);
        CHECK_ST(m.protect(a, P, Protect::None), MemStatus::Ok);
        CHECK(faults(m.base() + a, Access::Read));
        CHECK_ST(m.protect(a, P, Protect::ReadWrite), MemStatus::Ok);
        CHECK(!faults(m.base() + a, Access::Write));
        m.base()[a] = 0x22;
        CHECK(m.base()[a] == 0x22);
        // commit with Read on a fresh page: readable zero, not writable
        CHECK_ST(m.commit(a + P, P, Protect::Read), MemStatus::Ok);
        CHECK(m.base()[a + P] == 0);
        CHECK(faults(m.base() + a + P, Access::Write));
    }
    end();
}

void test_uncommitted_and_guard_fault() {
    begin("uncommitted_and_guard_fault");
    GuestMemory m;
    CHECK_ST(m.reserve(), MemStatus::Ok);
    CHECK(faults(m.base() + 0x12345678, Access::Read));
    CHECK(faults(m.base() + 0x12345678, Access::Write));
    CHECK_ST(m.commit(G4 - P, P, Protect::ReadWrite), MemStatus::Ok);
    CHECK(!faults(m.base() + G4 - 1, Access::Write));
    CHECK(faults(m.base() + G4, Access::Read));  // first guard byte
    CHECK(faults(m.base() + G4, Access::Write));
    CHECK(faults(m.base() + G4 + kGuestGuardSize - 1, Access::Write));  // last guard byte
    end();
}

void test_access_permissions() {
    begin("access_permissions_and_transitions");
    GuestMemory m;
    const uint64_t a = 63 * P;  // Cross a bitmap word as well as page boundaries.
    CHECK(!m.is_accessible(a, 1, Protect::Read));
    CHECK_ST(m.reserve(), MemStatus::Ok);
    CHECK(!m.is_accessible(a, 1, Protect::Read));
    CHECK_ST(m.commit(a, 3 * P, Protect::ReadWrite), MemStatus::Ok);
    CHECK(m.is_accessible(a + 1, 3 * P - 2, Protect::ReadWrite));
    CHECK(!m.is_accessible(a - 1, 2, Protect::Read));
    CHECK(!m.is_accessible(a, 0, Protect::Read));
    CHECK(!m.is_accessible(G4 - 1, 2, Protect::Read));
    CHECK(!m.is_accessible(~0ull, 2, Protect::Read));
    CHECK(!m.is_accessible(a, ~0ull, Protect::Read));
    CHECK(!m.is_accessible(a, 1, Protect::None));
    CHECK(!m.is_accessible(a, 1, static_cast<Protect>(2)));
    CHECK_ST(m.protect(a + P, P, Protect::Read), MemStatus::Ok);
    CHECK(m.is_accessible(a, 3 * P, Protect::Read));
    CHECK(!m.is_accessible(a + P - 1, 2, Protect::ReadWrite));
    CHECK(m.is_accessible(a + P - 1, 1, Protect::ReadWrite));
    CHECK(m.is_accessible(a + 2 * P, P, Protect::ReadWrite));
    CHECK_ST(m.protect(a + P, P, Protect::None), MemStatus::Ok);
    CHECK(!m.is_accessible(a + P, P, Protect::Read));
    CHECK(!m.is_accessible(a + P, P, Protect::ReadWrite));
    CHECK(m.is_committed(a + P, P));
    CHECK(m.translate((uint32_t)(a + P), 1) == m.base() + a + P);
    CHECK_ST(m.protect(a + P, P, static_cast<Protect>(2)), MemStatus::InvalidArgument);
    CHECK(!m.is_accessible(a + P, 1, Protect::Read));
    CHECK_ST(m.commit(a + P, P, Protect::Read), MemStatus::Ok);
    CHECK(m.is_accessible(a + P, P, Protect::Read));
    CHECK(!m.is_accessible(a + P, P, Protect::ReadWrite));
    CHECK(!faults(m.base() + a + P, Access::Read));
    CHECK(faults(m.base() + a + P, Access::Write));
    CHECK_ST(m.commit(a + P, P, Protect::ReadWrite), MemStatus::Ok);
    CHECK(m.is_accessible(a, 3 * P, Protect::ReadWrite));
    CHECK(!faults(m.base() + a + P, Access::Write));
    CHECK_ST(m.decommit(a + P, P), MemStatus::Ok);
    CHECK(!m.is_accessible(a + P - 1, 2, Protect::Read));
    CHECK(m.is_accessible(a, P, Protect::ReadWrite));
    CHECK_ST(m.commit(a + P, P, Protect::Read), MemStatus::Ok);
    CHECK(!m.is_accessible(a + P, P, Protect::ReadWrite));
    CHECK_ST(m.commit(G4 - P, P, Protect::Read), MemStatus::Ok);
    CHECK(m.is_accessible(G4 - 1, 1, Protect::Read));
    CHECK_ST(m.commit(a, P, static_cast<Protect>(5)), MemStatus::InvalidArgument);
    CHECK(m.is_accessible(a, P, Protect::ReadWrite));
    m.release();
    CHECK(!m.is_accessible(a, 1, Protect::Read));
    CHECK_ST(m.reserve(), MemStatus::Ok);
    CHECK_ST(m.commit(a, P, Protect::Read), MemStatus::Ok);
    CHECK(m.is_accessible(a, P, Protect::Read));
    CHECK(!m.is_accessible(a, P, Protect::ReadWrite));
    end();
}

void test_runtime_range_permissions() {
    begin("runtime_range_permissions");
    GuestMemory m;
    const uint64_t a = 0x42000000;
    CHECK_ST(m.reserve(), MemStatus::Ok);
    CHECK_ST(m.commit(a, 2 * P, Protect::ReadWrite), MemStatus::Ok);
    if (m.is_committed(a, 2 * P)) {
        m.base()[a] = 0x37;
        m.base()[a + P] = 0x45;
    }
    CHECK_ST(m.protect(a, P, Protect::Read), MemStatus::Ok);
    CHECK_ST(m.protect(a + P, P, Protect::None), MemStatus::Ok);
    CHECK_ST(m.reserve_runtime_range(a, 2 * P, "readonly_to_runtime"), MemStatus::Ok);
    CHECK(m.is_accessible(a, 2 * P, Protect::ReadWrite));
    CHECK(!faults(m.base() + a, Access::Write));
    CHECK(!faults(m.base() + a + P, Access::Write));
    CHECK_ST(m.protect(a, P, Protect::None), MemStatus::Conflict);
    CHECK_ST(m.decommit(a, P), MemStatus::Conflict);
    CHECK(m.is_accessible(a, 2 * P, Protect::ReadWrite));
    end();
}

void test_runtime_range() {
    begin("runtime_range_conflict");
    GuestMemory m;
    CHECK_ST(m.reserve(), MemStatus::Ok);
    const uint64_t t = 0x82000000, tn = 8 * P;  // stand-in for the func table
    CHECK_ST(m.reserve_runtime_range(t, tn, "functable"), MemStatus::Ok);
    CHECK(m.is_committed(t, tn));
    CHECK(m.stats().committed_bytes == tn);
    if (m.is_committed(t, tn)) {
        m.base()[t] = 0x42;  // runtime-owned table is RW
        CHECK(m.base()[t] == 0x42);
    }
    CHECK(m.overlaps_runtime_range(t + tn - 1, 1));
    CHECK(!m.overlaps_runtime_range(t + tn, P));
    CHECK(!m.overlaps_runtime_range(t - P, P));
    CHECK_ST(m.commit(t - P, 2 * P, Protect::ReadWrite), MemStatus::Conflict);
    CHECK_ST(m.commit(t + tn - P, 2 * P, Protect::ReadWrite), MemStatus::Conflict);
    CHECK_ST(m.decommit(t, P), MemStatus::Conflict);
    CHECK_ST(m.protect(t, P, Protect::Read), MemStatus::Conflict);
    CHECK_ST(m.reserve_runtime_range(t + P, P, "dup"), MemStatus::Conflict);
    CHECK_ST(m.reserve_runtime_range(0x1000, P, "unaligned"), MemStatus::InvalidArgument);
    CHECK_ST(m.commit(t + tn, P, Protect::ReadWrite), MemStatus::Ok);  // adjacent is fine
    CHECK(m.is_committed(t, tn));  // conflicts did not decommit anything
    if (m.is_committed(t, 1)) CHECK(m.base()[t] == 0x42);
    // Capacity: 16 ranges in total.
    int ok = 1;
    for (int i = 1; i < 16; ++i)
        ok &= m.reserve_runtime_range(0x10000000 + (uint64_t)i * 2 * P, P, "r") == MemStatus::Ok;
    CHECK(ok);
    CHECK_ST(m.reserve_runtime_range(0x0F000000, P, "17th"), MemStatus::OutOfMemory);
    end();
}

void test_stats() {
    begin("stats_accounting");
    GuestMemory m;
    CHECK_ST(m.reserve(), MemStatus::Ok);
    CHECK_ST(m.commit(0, 3 * P, Protect::ReadWrite), MemStatus::Ok);
    CHECK(m.stats().committed_bytes == 3 * P);
    CHECK_ST(m.commit(2 * P, 2 * P, Protect::ReadWrite), MemStatus::Ok);  // 1 new page
    CHECK(m.stats().committed_bytes == 4 * P);
    CHECK_ST(m.protect(0, P, Protect::Read), MemStatus::Ok);
    CHECK(m.stats().committed_bytes == 4 * P);
    CHECK_ST(m.decommit(P, P), MemStatus::Ok);
    CHECK(m.stats().committed_bytes == 3 * P);
    CHECK_ST(m.decommit(P, P), MemStatus::Ok);  // already decommitted
    CHECK(m.stats().committed_bytes == 3 * P);
    CHECK(m.stats().peak_committed_bytes == 4 * P);
    CHECK(m.stats().reserved_bytes == G4 + kGuestGuardSize);
    m.release();
    CHECK(m.stats().committed_bytes == 0);
    CHECK(m.stats().reserved_bytes == 0);
    CHECK_ST(m.reserve(), MemStatus::Ok);
    CHECK(m.stats().committed_bytes == 0 && m.stats().peak_committed_bytes == 0);
    end();
}

void test_repeat_no_leak() {
    begin("repeat_reserve_release_x20");
#if RCOMP_TEST_HAVE_FORK && defined(__linux__)
    unsigned long vm0 = vm_size_kib();
#endif
#if defined(RCOMP_TEST_PS5_BACKEND)
    uint64_t d0 = os::ps5::direct_bytes_held();
#endif
    for (int i = 0; i < 20; ++i) {
        GuestMemory m;
        MemStatus s = m.reserve();
        if (s != MemStatus::Ok) {
            out("FAIL %s: iteration %d reserve = %s\n", g_cur, i, mem_status_name(s));
            g_cur_failed = true;
            break;
        }
        uint64_t a = (uint64_t)i * 0x07000000ull % (G4 - 16 * P);
        a -= a % P;
        CHECK_ST(m.commit(a, 16 * P, Protect::ReadWrite), MemStatus::Ok);
        if (m.is_committed(a, 16 * P)) memset(m.base() + a, i, 16 * P);
        CHECK_ST(m.reserve_runtime_range(G4 - 2 * P, P, "t"), MemStatus::Ok);
        // m.release() via destructor on odd iterations, explicit on even
        if (i % 2 == 0) m.release();
    }
#if RCOMP_TEST_HAVE_FORK && defined(__linux__)
    unsigned long vm1 = vm_size_kib();
    if (vm0 && vm1 != vm0) {
        out("FAIL %s: VmSize %lu kB -> %lu kB\n", g_cur, vm0, vm1);
        g_cur_failed = true;
    }
#endif
#if defined(RCOMP_TEST_PS5_BACKEND)
    CHECK(os::ps5::direct_bytes_held() == d0);
#endif
#if defined(RCOMP_TEST_TESTDOUBLE_PS5)
    CHECK(TESTDOUBLE_ps5_direct_used_bytes() == 0);
#endif
    end();
}

#if !RCOMP_TEST_HAVE_FORK
constexpr int kMany = 6;
GuestMemory g_many[kMany];  // file scope: no function-local static guard
#endif

void test_alloc_failure() {
#if defined(__CYGWIN__)
    // RLIMIT_AS is not enforced by Windows. Exercise the real native VM's
    // rejection of an impossible reservation; do not exhaust host RAM.
    begin("alloc_failure_native_limit");
    CHECK(rcomp::os::vm_reserve(SIZE_MAX & ~(size_t(kGuestPageSize) - 1), kBaseAlignment) == nullptr);
    GuestMemory recovered;
    CHECK_ST(recovered.reserve(), MemStatus::Ok);
    end();
#elif RCOMP_TEST_HAVE_FORK
    begin("alloc_failure_rlimit_as");
    fflush(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        // Leave far less than 4 GiB of address space.
        struct rlimit rl;
        unsigned long vm = 0;
#if defined(__linux__)
        vm = vm_size_kib();
#endif
        rl.rlim_cur = rl.rlim_max = (rlim_t)(vm * 1024ull + (256ull << 20));
        if (setrlimit(RLIMIT_AS, &rl) != 0) _exit(10);
        GuestMemory m;
        MemStatus s = m.reserve();
        if (s != MemStatus::OutOfMemory) _exit(11);
        if (m.reserved() || m.base() != nullptr || m.stats().reserved_bytes != 0) _exit(12);
        if (m.commit(0, P, Protect::ReadWrite) != MemStatus::NotReserved) _exit(13);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        out("FAIL %s: child status 0x%x (10 setrlimit, 11 status, 12 state, 13 commit)\n",
            g_cur, st);
        g_cur_failed = true;
    }
    end();
#else
    // A PS5 process cannot fork or lower RLIMIT_AS safely; exhaust the
    // backend's reservation slots instead (or whatever the kernel refuses first).
    begin("alloc_failure_many_reservations");
    GuestMemory* ms = g_many;
    int ok = 0;
    MemStatus last = MemStatus::Ok;
    for (int i = 0; i < kMany; ++i) {
        GuestMemory& m = ms[i];
        last = m.reserve();
        if (last != MemStatus::Ok) break;
        ++ok;
    }
    out("INFO %s: %d concurrent 4 GiB reservations succeeded, next = %s\n", g_cur, ok,
        mem_status_name(last));
    CHECK(ok >= 1);
    CHECK(last == MemStatus::OutOfMemory);
    for (int i = 0; i < kMany; ++i) ms[i].release();
    GuestMemory again;
    CHECK_ST(again.reserve(), MemStatus::Ok);
    end();
#endif
}

#if defined(RCOMP_TEST_PS5_BACKEND)
void test_ps5_backend() {
    begin("ps5_backend_runs_and_unwind");
    GuestMemory m;
    CHECK_ST(m.reserve(), MemStatus::Ok);
    uintptr_t b = (uintptr_t)m.base();
    CHECK(!(b < 0x300000000ull && 0x200000000ull < b + G4 + kGuestGuardSize));  // GPU window
    uint64_t d0 = os::ps5::direct_bytes_held();
    // Per-page allocations: decommit of a sub-range never splits an allocation.
    os::ps5::set_max_run_bytes(P);
    CHECK_ST(m.commit(0x100000, 4 * P, Protect::ReadWrite), MemStatus::Ok);
    CHECK(os::ps5::direct_bytes_held() == d0 + 4 * P);
    CHECK_ST(m.decommit(0x100000 + P, 2 * P), MemStatus::Ok);
    CHECK(os::ps5::direct_bytes_held() == d0 + 2 * P);
    os::ps5::set_max_run_bytes((size_t)-1);
    // One allocation per run, then a sub-range decommit (partial release).
    CHECK_ST(m.commit(0x200000, 8 * P, Protect::ReadWrite), MemStatus::Ok);
    if (m.is_committed(0x200000, 8 * P)) memset(m.base() + 0x200000, 0x5C, 8 * P);
    CHECK_ST(m.decommit(0x200000 + 3 * P, 2 * P), MemStatus::Ok);
    CHECK(os::ps5::direct_bytes_held() == d0 + 8 * P);
    if (m.is_committed(0x200000, 3 * P)) CHECK(all_equal(m.base() + 0x200000, 3 * P, 0x5C));
#if defined(RCOMP_TEST_TESTDOUBLE_PS5)
    // Failure in the middle of a multi-run commit unwinds only this call's pages.
    os::ps5::set_max_run_bytes(P);
    const uint64_t a = 0x400000;
    CHECK_ST(m.commit(a + 2 * P, P, Protect::ReadWrite), MemStatus::Ok);
    m.base()[a + 2 * P] = 0x99;
    uint64_t before = os::ps5::direct_bytes_held();
    uint64_t committed = m.stats().committed_bytes;
    TESTDOUBLE_ps5_fail_allocate_after(3);  // 4th page allocation fails
    CHECK_ST(m.commit(a, 6 * P, Protect::ReadWrite), MemStatus::OutOfMemory);
    TESTDOUBLE_ps5_fail_allocate_after(-1);
    CHECK(os::ps5::direct_bytes_held() == before);
    CHECK(m.stats().committed_bytes == committed);
    CHECK(!m.is_committed(a, P) && !m.is_committed(a + 3 * P, P));
    CHECK(m.is_committed(a + 2 * P, P) && m.base()[a + 2 * P] == 0x99);
    CHECK(faults(m.base() + a, Access::Read));
    os::ps5::set_max_run_bytes((size_t)-1);
#endif
    m.release();
    CHECK(os::ps5::direct_bytes_held() == d0);
    end();
}
#endif

}  // namespace

extern "C" int rcomp_platform_selftest(FILE* log) {
    g_out = log ? log : stdout;
    g_pass = g_fail = g_skip = 0;
    out("rcomp platform selftest: backend=%s fork=%d page=%zu\n",
#if defined(RCOMP_TEST_TESTDOUBLE_PS5)
        "ps5(TESTDOUBLE kernel on host)",
#elif defined(RCOMP_TEST_PS5_BACKEND)
        "ps5",
#else
        "posix",
#endif
        RCOMP_TEST_HAVE_FORK, (size_t)getpagesize());
    test_reserve_release();
    test_commit_bounds();
    test_write_read();
    test_decommit_zeroes();
    test_protect();
    test_access_permissions();
    test_runtime_range_permissions();
    test_uncommitted_and_guard_fault();
    test_runtime_range();
    test_stats();
    test_repeat_no_leak();
    test_alloc_failure();
#if defined(RCOMP_TEST_PS5_BACKEND)
    test_ps5_backend();
#else
    skip("ps5_backend_runs_and_unwind", "POSIX backend");
#endif
    out("SUMMARY pass=%d fail=%d skip=%d\n", g_pass, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}

#ifndef RCOMP_SELFTEST_NO_MAIN
int main() { return rcomp_platform_selftest(stdout); }
#endif
