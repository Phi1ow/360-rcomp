// The compared guest stores of the generated code (RCOMP_STORE_COMPARE, include/rcomp/ppc_prelude.h and rcomp/guest_write_tracking.h): an
// unchanged store to the physical windows does not mark its page for the GPU caches, a changed one does, and every store to a page the GPU
// wrote does. Stores outside the windows and writes whose old bytes are unknown behave as before. The macros are the ones the recompiled code uses.
#include <cstring>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "test_util.h"

using namespace rcomp;

namespace {
uint32_t page_of(uint32_t guest_address) { return (guest_address - kGuestWriteWindowBase) >> kGuestWritePageShift; }
bool marked(uint32_t guest_address) { return g_guest_physical_written[page_of(guest_address) & (kGuestWritePages - 1)] != 0; }
void clear_marks() { std::memset(g_guest_physical_written, 0, sizeof g_guest_physical_written); }
uint32_t marked_pages() {
    uint32_t n = 0;
    for (uint32_t p = 0; p < kGuestWritePages; ++p) n += g_guest_physical_written[p] != 0;
    return n;
}
}  // namespace

int main() {
    GuestMemory memory;
    CHECK(memory.reserve() == MemStatus::Ok);
    CHECK(memory.commit(kPhysicalWindowBase, 0x20000, Protect::ReadWrite) == MemStatus::Ok);
    CHECK(memory.commit(0x00100000, 0x10000, Protect::ReadWrite) == MemStatus::Ok);
    uint8_t* base = memory.base();
    alignas(64) PPCContext ctx{};
    std::memset(g_guest_physical_gpu_written, 0, sizeof g_guest_physical_gpu_written);
    clear_marks();

    const uint32_t w = uint32_t(kPhysicalWindowBase);

    // --- scalar stores --------------------------------------------------------------------------------------------------
    PPC_STORE_U32(w + 0x1000, 0x11223344u);  // memory is zero: a change
    CHECK(marked(w + 0x1000));
    CHECK_EQ(marked_pages(), 1u);
    CHECK_EQ(PPC_LOAD_U32(w + 0x1000), 0x11223344u);
    clear_marks();
    PPC_STORE_U32(w + 0x1000, 0x11223344u);  // the same bytes
    PPC_STORE_U8(w + 0x1001, 0x22u);         // 0x11 0x22 0x33 0x44: the byte there already
    PPC_STORE_U16(w + 0x1002, 0x3344u);
    PPC_STORE_U64(w + 0x1010, 0u);           // zero over zero
    CHECK_EQ(marked_pages(), 0u);
    CHECK_EQ(PPC_LOAD_U32(w + 0x1000), 0x11223344u);
    PPC_STORE_U8(w + 0x1001, 0x23u);  // one byte differs
    CHECK(marked(w + 0x1000));
    CHECK_EQ(marked_pages(), 1u);
    CHECK_EQ(PPC_LOAD_U32(w + 0x1000), 0x11233344u);
    clear_marks();

    // A store across a page boundary marks both pages when it changes a byte, neither when it does not.
    PPC_STORE_U32(w + 0x2000 - 2, 0xAABBCCDDu);
    CHECK(marked(w + 0x1000 + 0xFFE));
    CHECK(marked(w + 0x2000));
    CHECK_EQ(marked_pages(), 2u);
    clear_marks();
    PPC_STORE_U32(w + 0x2000 - 2, 0xAABBCCDDu);
    CHECK_EQ(marked_pages(), 0u);
    CHECK_EQ(PPC_LOAD_U32(w + 0x2000 - 2), 0xAABBCCDDu);

    // The three windows alias one physical memory (the host test backend cannot map the aliases, so the table is driven directly):
    // a change through the second or the third window marks the same physical page.
    clear_marks();
    note_guest_window_store(0xC0000000u - kGuestWriteWindowBase + 0x3000, 4, true);
    CHECK(g_guest_physical_written[3] != 0);
    CHECK_EQ(marked_pages(), 1u);
    clear_marks();
    note_guest_window_store(0xE0000000u - kGuestWriteWindowBase + 0x3000, 4, false);  // unchanged
    CHECK_EQ(marked_pages(), 0u);
    note_gpu_written_pages(3, 1, true);
    note_guest_window_store(0xE0000000u - kGuestWriteWindowBase + 0x3000, 4, false);  // unchanged, but the GPU wrote the page
    CHECK(g_guest_physical_written[3] != 0);
    note_gpu_written_pages(3, 1, false);
    clear_marks();

    // --- pages the GPU wrote -----------------------------------------------------------------------------------------------
    note_gpu_written_pages(5, 2, true);  // pages 5 and 6
    PPC_STORE_U32(w + 0x5000, 0u);       // zero over zero, but the GPU's copy of the page is not guest memory
    CHECK(marked(w + 0x5000));
    CHECK_EQ(marked_pages(), 1u);
    clear_marks();
    PPC_STORE_U8(w + 0x7000, 0u);  // page 7: not written by the GPU
    CHECK_EQ(marked_pages(), 0u);
    PPC_STORE_U32(w + 0x7000 - 2, 0u);  // across 6 (the GPU's) and 7: both are marked
    CHECK(marked(w + 0x6000));
    CHECK(marked(w + 0x7000));
    clear_marks();
    note_gpu_written_pages(5, 1, false);  // page 5 was made equal to guest memory again (an upload, an invalidation)
    PPC_STORE_U32(w + 0x5000, 0u);
    CHECK_EQ(marked_pages(), 0u);
    note_gpu_written_pages(kGuestWritePages - 1, 100, true);  // clamped to the table
    CHECK_EQ(g_guest_physical_gpu_written[kGuestWritePages - 1], 1);
    note_gpu_written_pages(kGuestWritePages + 3, 4, true);    // outside: ignored
    std::memset(g_guest_physical_gpu_written, 0, sizeof g_guest_physical_gpu_written);

    // --- vector stores ---------------------------------------------------------------------------------------------------
    const simde__m128i pattern = simde_mm_set_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    clear_marks();
    PPC_VSTORE128(w + 0x8010, pattern);  // memory is zero
    CHECK(marked(w + 0x8010));
    CHECK_EQ(marked_pages(), 1u);
    CHECK(std::memcmp(base + w + 0x8010, &pattern, 16) == 0);
    clear_marks();
    PPC_VSTORE128(w + 0x8010, pattern);  // the same sixteen bytes
    CHECK_EQ(marked_pages(), 0u);
    CHECK(std::memcmp(base + w + 0x8010, &pattern, 16) == 0);
    const simde__m128i other = simde_mm_set_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 1);
    PPC_VSTORE128(w + 0x8010, other);  // one byte differs
    CHECK(marked(w + 0x8010));
    CHECK(std::memcmp(base + w + 0x8010, &other, 16) == 0);
    clear_marks();
    note_gpu_written_pages(8, 1, true);
    PPC_VSTORE128(w + 0x8010, other);  // the same bytes, on a page the GPU wrote
    CHECK(marked(w + 0x8010));
    clear_marks();
    std::memset(g_guest_physical_gpu_written, 0, sizeof g_guest_physical_gpu_written);

    // --- outside the windows: the store happens, nothing is recorded ---------------------------------------------------
    PPC_STORE_U32(0x00100000u, 0x55667788u);
    PPC_VSTORE128(0x00100010u, pattern);
    CHECK_EQ(PPC_LOAD_U32(0x00100000u), 0x55667788u);
    CHECK(std::memcmp(base + 0x00100010u, &pattern, 16) == 0);
    CHECK_EQ(marked_pages(), 0u);

    // --- writes whose old bytes are unknown always mark -----------------------------------------------------------------
    PPC_WRITE_NOTIFY(w + 0x9000, 128);  // dcbzl, store-conditional, setjmp
    CHECK(marked(w + 0x9000));
    CHECK_EQ(marked_pages(), 1u);
    clear_marks();
    note_guest_write_range(w + 0xA000 + 100, 0x2000);  // runtime writes: pages 10, 11 and 12
    CHECK(marked(w + 0xA000));
    CHECK(marked(w + 0xB000));
    CHECK(marked(w + 0xC000));
    CHECK_EQ(marked_pages(), 3u);

    return g_failures ? 1 : 0;
}
