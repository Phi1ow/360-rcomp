// Write tracking and host offsets of the console's 4 KiB-page window shift
// (RCOMP_PHYSICAL_4K_WINDOW_OFFSET, include/rcomp/ppc_prelude.h and
// include/rcomp/guest_write_tracking.h). Expected pages are computed here from the
// console map: guest 0xE0000000 + X is physical X + 0x1000 (modulo 512 MiB), the
// other windows are physical (guest - window base). Host test only.
#include "rcomp/ppc_prelude.h"

#include <cstdio>
#include <cstring>
#include <set>

namespace {
int failures = 0;
unsigned checks = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::printf("physical_4k/%s FAIL\n", what);
        ++failures;
    }
}

void clear() { std::memset(rcomp::g_guest_physical_written, 0, sizeof rcomp::g_guest_physical_written); }

std::set<uint32_t> marks() {
    std::set<uint32_t> pages;
    for (uint32_t p = 0; p < rcomp::kGuestWritePages; ++p)
        if (rcomp::g_guest_physical_written[p]) pages.insert(p);
    return pages;
}

// Console physical page of one guest byte (window addresses only).
uint32_t console_page(uint64_t guest, bool shift) {
    uint64_t physical;
    if (guest >= 0xE0000000u) physical = guest - 0xE0000000u + (shift ? 0x1000u : 0u);
    else if (guest >= 0xC0000000u) physical = guest - 0xC0000000u;
    else physical = guest - 0xA0000000u;
    return uint32_t((physical & 0x1FFFFFFFu) >> 12);
}

std::set<uint32_t> expected_range(uint64_t address, uint64_t size, bool shift) {
    std::set<uint32_t> pages;
    for (uint64_t a = address; a < address + size && a < 0x100000000ull; ++a)
        if (a >= 0xA0000000u) pages.insert(console_page(a, shift));
    return pages;
}

void range_case(const char* what, uint64_t address, uint64_t size, bool shift) {
    clear();
    rcomp::note_guest_write_range(address, size, shift);
    check(marks() == expected_range(address, size, shift), what);
    if (!shift) {
        const std::set<uint32_t> three = marks();
        clear();
        rcomp::note_guest_write_range(address, size);
        check(marks() == three, "range_off_equals_two_argument_form");
    }
}
}  // namespace

int main() {
    // Host offsets the generated code uses with the option at 1.
    check(rcomp::physical_4k_host_offset(0xDFFFFFFFu) == 0xDFFFFFFFull, "host_below_window");
    check(rcomp::physical_4k_host_offset(0xE0000000u) == 0xE0001000ull, "host_window_start");
    check(rcomp::physical_4k_host_offset(0xFFFFFFFFu) == 0x100000FFFull, "host_window_end");
    check(rcomp::physical_4k_host_offset(0xA0001234u) == 0xA0001234ull, "host_a_window");
    check(rcomp::physical_4k_window_shift(0xC0000000u) == 0 && rcomp::physical_4k_window_shift(0xE0000000u) == 0x1000,
          "window_shift");

    // Store tracking of the generated code with the option at 1 (RCOMP_NOTE_GUEST_WRITE).
    const struct { uint32_t address, size; } stores[] = {
        {0xA0002FFCu, 8}, {0xC0010000u, 4}, {0xE0000000u, 1}, {0xE0002FFCu, 8},
        {0xFFFFF000u, 16}, {0xFFFFEFF8u, 16}, {0x80000000u, 8}, {0x00010000u, 4}};
    for (const auto& s : stores) {
        clear();
        rcomp::note_shifted_guest_write(s.address, s.size);
        check(marks() == expected_range(s.address, s.size, true), "note_shifted_guest_write");
        clear();
        rcomp::note_guest_write(s.address, s.size);
        check(marks() == expected_range(s.address, s.size, false), "note_guest_write_unshifted");
    }

    // Runtime range helper, both settings.
    for (bool shift : {false, true}) {
        range_case("range_a_window", 0xA0001800u, 0x2000, shift);
        range_case("range_crossing_into_e", 0xDFFFF800u, 0x1000, shift);
        range_case("range_e_window", 0xE0003000u, 0x3001, shift);
        range_case("range_last_page", 0xFFFFF000u, 0x10, shift);
        range_case("range_past_4gib", 0xFFFFE800u, 0x4000, shift);
        range_case("range_below_windows", 0x40000000u, 0x1000, shift);
    }
    for (bool shift : {false, true}) {
        clear();
        rcomp::note_guest_write_range(0xE0000000u, 0x20000000u, shift);
        check(marks().size() == rcomp::kGuestWritePages, "range_whole_window");
    }
    // A range of more than 512 MiB starting below the 0xE0000000 window.
    clear();
    rcomp::note_guest_write_range(0xC0000000u, 0x30000000u, true);
    check(marks().size() == rcomp::kGuestWritePages, "range_over_512mib");

    std::printf("physical_4k/tracking %s (%u checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
