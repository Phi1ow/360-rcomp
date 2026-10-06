#include <cstdio>
#include "rcomp/guest_memory.h"

using namespace rcomp;
int main() {
    unsigned checks = 0, failures = 0;
    auto check = [&](bool value, const char* message) {
        ++checks;
        if (!value) { ++failures; std::fprintf(stderr, "FAIL %s\n", message); }
    };
    constexpr uint64_t start = 0x70000000, length = 0x1000000;
    GuestMemory memory;
    check(memory.reserve_opaque_runtime_range(start, length, "tokens") == MemStatus::NotReserved,
          "uninitialized memory rejected");
    if (memory.reserve() != MemStatus::Ok) return 2;
    check(memory.reserve_opaque_runtime_range(start, 1, "tokens") == MemStatus::InvalidArgument,
          "unaligned extent rejected");
    check(memory.reserve_opaque_runtime_range(0xFFFF0000, 0x20000, "tokens") == MemStatus::InvalidArgument,
          "wrapping range rejected");
    check(memory.reserve_opaque_runtime_range(start, length, nullptr) == MemStatus::InvalidArgument,
          "missing owner rejected");
    check(memory.commit(start + kGuestPageSize, kGuestPageSize, Protect::ReadWrite) == MemStatus::Ok,
          "create existing backing inside candidate");
    const auto backed = memory.stats().committed_bytes;
    check(memory.reserve_opaque_runtime_range(start, length, "tokens") == MemStatus::Conflict,
          "partially committed candidate rejected");
    check(memory.stats().committed_bytes == backed, "refused candidate preserves backing");
    check(memory.decommit(start + kGuestPageSize, kGuestPageSize) == MemStatus::Ok,
          "refused candidate did not claim range");
    check(memory.reserve_opaque_runtime_range(start, length, "tokens") == MemStatus::Ok,
          "opaque reservation created");
    check(memory.stats().committed_bytes == 0, "opaque reservation has no physical backing");
    check(!memory.is_committed(start, 1) && !memory.is_accessible(start, 1, Protect::Read),
          "opaque token cannot be read");
    check(memory.overlaps_runtime_range(start + 16, 16), "token addresses owned by runtime");
    check(memory.reserve_opaque_runtime_range(start, length, "tokens") == MemStatus::Ok,
          "same owner can reuse reserved arena");
    check(memory.reserve_opaque_runtime_range(start, length, "other") == MemStatus::Conflict,
          "other owner rejected");
    check(memory.reserve_runtime_range(start, kGuestPageSize, "data") == MemStatus::Conflict,
          "runtime data cannot replace opaque tokens");
    check(memory.commit(start, kGuestPageSize, Protect::ReadWrite) == MemStatus::Conflict,
          "guest commit cannot claim opaque range");
    check(memory.decommit(start, kGuestPageSize) == MemStatus::Conflict,
          "guest decommit cannot release opaque ownership");
    check(memory.protect(start, kGuestPageSize, Protect::ReadWrite) != MemStatus::Ok,
          "guest protect cannot expose opaque range");
    check(memory.commit(start + length, kGuestPageSize, Protect::ReadWrite) == MemStatus::Ok,
          "neighboring page remains usable");
    memory.release();
    if (memory.reserve() != MemStatus::Ok) return 2;
    check(memory.commit(start, kGuestPageSize, Protect::ReadWrite) == MemStatus::Ok,
          "releasing memory releases opaque ownership");
    std::printf("OPAQUE-RANGE checks=%u pass=%u fail=%u scope=host\n", checks, checks - failures, failures);
    return failures ? 1 : 0;
}
