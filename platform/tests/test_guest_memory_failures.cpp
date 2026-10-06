#include "rcomp/guest_memory.h"
#include <cstdio>

void TESTDOUBLE_fail_vm_once(int operation);

using namespace rcomp;
namespace {
int failures;
void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL permissions_on_vm_failure:%d: %s\n", line, expression);
        ++failures;
    }
}
}
#define CHECK(expression) check((expression), #expression, __LINE__)

int main() {
    GuestMemory m;
    constexpr uint64_t P = kGuestPageSize, a = 63 * P;
    if (m.reserve() != MemStatus::Ok) return 1;

    TESTDOUBLE_fail_vm_once(0);
    CHECK(m.commit(a, P, Protect::ReadWrite) == MemStatus::OutOfMemory);
    CHECK(!m.is_committed(a, P));
    CHECK(!m.is_accessible(a, P, Protect::Read));
    CHECK(!m.is_accessible(a, P, Protect::ReadWrite));
    CHECK(m.stats().committed_bytes == 0);

    CHECK(m.commit(a, P, Protect::Read) == MemStatus::Ok);
    TESTDOUBLE_fail_vm_once(0);
    CHECK(m.commit(a, 2 * P, Protect::ReadWrite) == MemStatus::OutOfMemory);
    CHECK(m.is_accessible(a, P, Protect::Read));
    CHECK(!m.is_accessible(a, P, Protect::ReadWrite));
    CHECK(!m.is_accessible(a + P, P, Protect::Read));
    CHECK(m.stats().committed_bytes == P);

    TESTDOUBLE_fail_vm_once(1);
    CHECK(m.protect(a, P, Protect::ReadWrite) == MemStatus::PlatformError);
    CHECK(m.is_accessible(a, P, Protect::Read));
    CHECK(!m.is_accessible(a, P, Protect::ReadWrite));
    CHECK(m.protect(a, P, Protect::ReadWrite) == MemStatus::Ok);
    m.base()[a] = 0x75;
    TESTDOUBLE_fail_vm_once(1);
    CHECK(m.protect(a, P, Protect::None) == MemStatus::PlatformError);
    CHECK(m.is_accessible(a, P, Protect::ReadWrite));
    CHECK(m.base()[a] == 0x75);
    TESTDOUBLE_fail_vm_once(2);
    CHECK(m.decommit(a, P) == MemStatus::PlatformError);
    CHECK(m.is_accessible(a, P, Protect::ReadWrite));
    CHECK(m.stats().committed_bytes == P);
    CHECK(m.base()[a] == 0x75);

    CHECK(m.protect(a, P, Protect::Read) == MemStatus::Ok);
    TESTDOUBLE_fail_vm_once(0);
    CHECK(m.reserve_runtime_range(a, P, "failed_promotion") == MemStatus::OutOfMemory);
    CHECK(!m.overlaps_runtime_range(a, P));
    CHECK(m.is_accessible(a, P, Protect::Read));
    CHECK(!m.is_accessible(a, P, Protect::ReadWrite));
    CHECK(m.reserve_runtime_range(a, P, "promoted") == MemStatus::Ok);
    CHECK(m.is_accessible(a, P, Protect::ReadWrite));
    CHECK(m.protect(a, P, Protect::None) == MemStatus::Conflict);
    CHECK(m.decommit(a, P) == MemStatus::Conflict);
    CHECK(m.is_accessible(a, P, Protect::ReadWrite));
    CHECK(m.base()[a] == 0x75);
    m.release();
    CHECK(!m.is_accessible(a, P, Protect::Read));
    CHECK(m.reserve() == MemStatus::Ok);
    CHECK(m.commit(a, P, Protect::Read) == MemStatus::Ok);
    CHECK(m.is_accessible(a, P, Protect::Read));
    CHECK(!m.is_accessible(a, P, Protect::ReadWrite));
    std::printf("%s permissions_on_vm_failure failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
