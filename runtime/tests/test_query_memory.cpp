// Read-only virtual-memory query tests against real GuestMemory/GuestHeap state.
#include <memory>

#include "../src/module_state.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/query_memory.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__MmAllocatePhysicalMemory);
PPC_EXTERN_FUNC(__imp__MmFreePhysicalMemory);
PPC_EXTERN_FUNC(__imp__MmQueryAddressProtect);
PPC_EXTERN_FUNC(__imp__MmSetAddressProtect);
PPC_EXTERN_FUNC(__imp__NtAllocateVirtualMemory);
PPC_EXTERN_FUNC(__imp__NtFreeVirtualMemory);
PPC_EXTERN_FUNC(__imp__NtProtectVirtualMemory);
PPC_EXTERN_FUNC(__imp__NtQueryVirtualMemory);

using namespace rcomp;
using namespace rcomp::rt;

namespace {

GuestMemory mem;
constexpr uint32_t kScratch = 0x30000000u;
constexpr uint32_t kScratchBytes = 0x40000u;
constexpr uint32_t kOutput = kScratch + 0x100;
constexpr uint32_t kBaseWord = kScratch + 0x200;
constexpr uint32_t kSizeWord = kScratch + 0x204;
constexpr uint32_t kOldProtectWord = kScratch + 0x208;
constexpr uint32_t kHeader = 0x81000000u;
constexpr uint32_t kPage = (uint32_t)kGuestPageSize;
constexpr uint32_t kMemCommit = 0x1000u;
constexpr uint32_t kMemReserve = 0x2000u;
constexpr uint32_t kMemFree = 0x10000u;
constexpr uint32_t kMemPrivate = 0x20000u;
constexpr uint32_t kMemImage = 0x1000000u;
constexpr uint32_t kPageNoAccess = 0x01u;
constexpr uint32_t kPageReadOnly = 0x02u;
constexpr uint32_t kPageReadWrite = 0x04u;
constexpr uint32_t kPageNoCache = 0x200u;
constexpr uint32_t kPageWriteCombine = 0x400u;

uint32_t call(PPCFunc* fn, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0,
              uint32_t d = 0, uint32_t e = 0) {
    alignas(64) PPCContext context{};
    context.r3.u64 = a;
    context.r4.u64 = b;
    context.r5.u64 = c;
    context.r6.u64 = d;
    context.r7.u64 = e;
    fn(context, mem.base());
    return context.r3.u32;
}

uint32_t word(uint32_t address) {
    uint32_t value = 0;
    CHECK(guest_read_be32(address, &value));
    return value;
}

void put(uint32_t address, uint32_t value) { CHECK(guest_write_be32(address, value)); }

void fill_output(uint32_t value) {
    for (unsigned i = 0; i < sizeof(MemoryBasicInformation32) / 4; ++i)
        put(kOutput + i * 4, value);
}

void expect_output(uint32_t base, uint32_t allocation_base, uint32_t allocation_protect,
                   uint32_t region_size, uint32_t state, uint32_t protect, uint32_t type) {
    CHECK_EQ(word(kOutput + 0), base);
    CHECK_EQ(word(kOutput + 4), allocation_base);
    CHECK_EQ(word(kOutput + 8), allocation_protect);
    CHECK_EQ(word(kOutput + 12), region_size);
    CHECK_EQ(word(kOutput + 16), state);
    CHECK_EQ(word(kOutput + 20), protect);
    CHECK_EQ(word(kOutput + 24), type);
}

void setup() {
    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK(mem.commit(kScratch, kScratchBytes, Protect::ReadWrite) == MemStatus::Ok);
    RuntimeConfig cfg;
    cfg.heap_lo = 0x40000000u;
    cfg.heap_hi = 0x40400000u;
    CHECK_ST(runtime_init(&mem, cfg), Status::Ok);
    clear_imports();
    CHECK_ST(register_xboxkrnl_memory_hle(), Status::Ok);
    CHECK_ST(register_xboxkrnl_query_memory_hle(), Status::Ok);
}

void registration() {
    struct Entry { uint32_t ordinal; const char* name; };
    const Entry entries[] = {{0xB9, "MmAllocatePhysicalMemory"},
                             {0xC4, "MmQueryAddressProtect"},
                             {0xC7, "MmSetAddressProtect"},
                             {0xCC, "NtAllocateVirtualMemory"},
                             {0xDC, "NtFreeVirtualMemory"},
                             {0xE1, "NtProtectVirtualMemory"},
                             {0xEE, "NtQueryVirtualMemory"}};
    for (const auto& entry : entries) {
        uint32_t ordinal = 0;
        CHECK(export_ordinal(kModuleXboxkrnl, entry.name, &ordinal));
        CHECK_EQ(ordinal, entry.ordinal);
        CHECK(find_import(kModuleXboxkrnl, entry.ordinal) != nullptr);
        CHECK(import_thunk(kModuleXboxkrnl, entry.ordinal) != nullptr);
        CHECK(strcmp(import_registry_name(kModuleXboxkrnl, entry.ordinal), entry.name) == 0);
    }
}

uint32_t allocate_virtual(uint32_t size, uint32_t protect = kPageReadWrite,
                          uint32_t type = kMemCommit | kMemReserve) {
    put(kBaseWord, 0);
    put(kSizeWord, size);
    CHECK_EQ(call(__imp__NtAllocateVirtualMemory, kBaseWord, kSizeWord, type, protect, 0), 0u);
    CHECK_EQ(word(kSizeWord), size);
    return word(kBaseWord);
}

void virtual_regions() {
    const uint32_t initial_protect = kPageReadWrite | kPageWriteCombine;
    const uint32_t base = allocate_virtual(3 * kPage, initial_protect);
    CHECK_EQ(base, 0x40000000u);

    uint32_t containing_base = 0, containing_size = 0;
    CHECK_ST(runtime()->heap.allocation_containing(base + kPage + 0x1234,
                                                   &containing_base, &containing_size),
             Status::Ok);
    CHECK_EQ(containing_base, base);
    CHECK_EQ(containing_size, 3 * kPage);
    GuestHeapProtectionInfo heap_protection{};
    CHECK_ST(runtime()->heap.query_guest_protection(base + 8, &heap_protection), Status::Ok);
    CHECK_EQ(heap_protection.allocation_protect, initial_protect);
    CHECK_EQ(heap_protection.protect, initial_protect);

    fill_output(0xCCCCCCCCu);
    put(kOutput + sizeof(MemoryBasicInformation32), 0x13579BDFu);
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, base + 0x1234, kOutput, 0), 0u);
    expect_output(base, base, initial_protect, 3 * kPage, kMemCommit,
                  initial_protect, kMemPrivate);
    // GTA provides at least 32 writable bytes here, but the pinned Xbox layout
    // is seven BE32 words (28 bytes). Never overwrite the caller's trailing word.
    CHECK_EQ(word(kOutput + sizeof(MemoryBasicInformation32)), 0x13579BDFu);

    MemoryQueryResult direct{};
    CHECK_ST(query_memory_basic(base + 0x1234, &direct), Status::Ok);
    CHECK_EQ((uint32_t)direct.provenance,
             (uint32_t)MemoryQueryProvenance::VirtualAllocation);
    CHECK(direct.protection_modifiers_known);
    bool modifiers_known = false;
    CHECK_EQ(query_address_protect(base + 3, &modifiers_known), initial_protect);
    CHECK(modifiers_known);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, base + 3), initial_protect);

    // NtProtectVirtualMemory rounds the requested middle-page subrange to the
    // 64-KiB guest page and preserves exact cache modifiers.
    put(kBaseWord, base + kPage + 0x1234);
    put(kSizeWord, 0x1000);
    put(kOldProtectWord, 0xDEADBEEFu);
    const uint32_t middle_protect = kPageReadOnly | kPageNoCache;
    CHECK_EQ(call(__imp__NtProtectVirtualMemory, kBaseWord, kSizeWord,
                  middle_protect, kOldProtectWord, 0), 0u);
    CHECK_EQ(word(kBaseWord), base + kPage);
    CHECK_EQ(word(kSizeWord), kPage);
    CHECK_EQ(word(kOldProtectWord), initial_protect);

    CHECK_EQ(call(__imp__NtQueryVirtualMemory, base + 1, kOutput, 0), 0u);
    expect_output(base, base, initial_protect, kPage, kMemCommit,
                  initial_protect, kMemPrivate);
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, base + kPage + 7, kOutput, 0), 0u);
    expect_output(base + kPage, base, initial_protect, kPage, kMemCommit,
                  middle_protect, kMemPrivate);
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, base + 2 * kPage + 9, kOutput, 0), 0u);
    expect_output(base + 2 * kPage, base, initial_protect, kPage, kMemCommit,
                  initial_protect, kMemPrivate);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, base + kPage + 3), middle_protect);

    const uint32_t tail_protect = kPageNoAccess | kPageWriteCombine;
    call(__imp__MmSetAddressProtect, base + 2 * kPage, kPage, tail_protect);
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, base + 2 * kPage + 4, kOutput, 0), 0u);
    expect_output(base + 2 * kPage, base, initial_protect, kPage, kMemCommit,
                  tail_protect, kMemPrivate);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, base + 2 * kPage + 4), tail_protect);

    // The allocation provenance remains live while the backing page is
    // decommitted, so this is a truthful MEM_RESERVE rather than interpreting
    // the process-wide host reservation as guest reserved memory.
    CHECK(mem.decommit(base + kPage, kPage) == MemStatus::Ok);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, base + kPage), 0u);
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, base + kPage + 8, kOutput, 0), 0u);
    expect_output(base + kPage, base, initial_protect, kPage, kMemReserve,
                  0, kMemPrivate);
    CHECK(mem.commit(base + kPage, kPage, Protect::Read) == MemStatus::Ok);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, base + kPage), middle_protect);

    // Freeing GuestHeap memory keeps its host pages committed. Once the
    // Nt-allocation ownership disappears, allocator metadata makes the whole
    // released page range MEM_FREE even though backing stays committed. free()
    // also restores RO/NOACCESS pages to RW before recycling the block.
    put(kBaseWord, base);
    put(kSizeWord, 0);
    CHECK_EQ(call(__imp__NtFreeVirtualMemory, kBaseWord, kSizeWord, 0x8000, 0), 0u);
    CHECK_ST(query_memory_basic(base, &direct), Status::Ok);
    CHECK_EQ(direct.basic.state, kMemFree);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, base), 0u);
    GuestHeapRegionInfo free_region{};
    CHECK_ST(runtime()->heap.region_containing(base, &free_region), Status::Ok);
    CHECK(!free_region.allocated);
    CHECK(mem.is_accessible(base, 3 * kPage, Protect::ReadWrite));

    // Reuse proves free-time protection cleanup is complete.
    const uint32_t reused = allocate_virtual(3 * kPage, kPageReadWrite | kPageNoCache);
    CHECK_EQ(reused, base);
    put(kBaseWord, reused);
    put(kSizeWord, 0);
    CHECK_EQ(call(__imp__NtFreeVirtualMemory, kBaseWord, kSizeWord, 0x8000, 0), 0u);

    const uint32_t top = allocate_virtual(kPage, kPageReadWrite | kPageWriteCombine,
                                          kMemCommit | kMemReserve | 0x00100000u);
    CHECK_EQ(top, 0x403F0000u);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, top),
             kPageReadWrite | kPageWriteCombine);
    put(kBaseWord, top);
    put(kSizeWord, 0);
    CHECK_EQ(call(__imp__NtFreeVirtualMemory, kBaseWord, kSizeWord, 0x8000, 0), 0u);
}

void free_regions_and_bounds() {
    // Guest page zero is intentionally never committed by the loader. It is a
    // genuine free guest region; the host 4-GiB reservation is irrelevant.
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, 0, kOutput, 0), 0u);
    expect_output(0, 0, 0, kScratch, kMemFree, 0, 0);

    // Scratch ends at 0x30040000 and the heap starts at 0x40000000. Neither
    // the host 4-GiB VA reservation nor the uncommitted GuestHeap range is a
    // guest MEM_RESERVE by itself.
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, 0x35001234u, kOutput, 0), 0u);
    expect_output(0x30040000u, 0, 0, 0x0FFC0000u, kMemFree, 0, 0);

    MemoryQueryResult top{};
    CHECK_ST(query_memory_basic(0xFFFFFFFEu, &top), Status::Ok);
    CHECK_EQ(top.basic.state, kMemFree);
    CHECK_EQ(uint64_t(top.basic.base_address) + top.basic.region_size,
             kGuestSpaceSize);

    // Output validation is all-or-nothing and includes 32-bit overflow.
    fill_output(0xBEEFBEEFu);
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, 0x35000000u, 0, 0),
             nt::kAccessViolation);
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, 0x35000000u, 0xFFFFFFF0u, 0),
             nt::kAccessViolation);
    for (unsigned i = 0; i < 7; ++i) CHECK_EQ(word(kOutput + i * 4), 0xBEEFBEEFu);

    CHECK_EQ(call(__imp__NtQueryVirtualMemory, 0x35000000u, kOutput, 1),
             nt::kInvalidParameter);
    for (unsigned i = 0; i < 7; ++i) CHECK_EQ(word(kOutput + i * 4), 0xBEEFBEEFu);

    const uint32_t readonly_output = kScratch + kPage;
    for (unsigned i = 0; i < 7; ++i) put(readonly_output + i * 4, 0x2468ACE0u);
    CHECK(mem.protect(readonly_output, kPage, Protect::Read) == MemStatus::Ok);
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, 0x35000000u, readonly_output, 0),
             nt::kAccessViolation);
    for (unsigned i = 0; i < 7; ++i)
        CHECK_EQ(word(readonly_output + i * 4), 0x2468ACE0u);
    CHECK(mem.protect(readonly_output, kPage, Protect::ReadWrite) == MemStatus::Ok);

    CHECK_ST(query_memory_basic(0x35000000u, nullptr), Status::InvalidArgument);

    // Runtime's opaque thread-object arena is address-space identity owned by
    // R-comp. It is deliberately not exposed as a title MEM_RESERVE region.
    MemoryQueryResult opaque{};
    CHECK_ST(query_memory_basic(0x70000000u, &opaque), Status::Unsupported);

    // The HLE's exact, fully known free-page protection result is zero.
    CHECK_EQ(call(__imp__MmQueryAddressProtect, 0x1000u), 0u);
}

void physical_region() {
    const uint32_t initial = kPageReadWrite | kPageWriteCombine;
    const uint32_t physical = call(__imp__MmAllocatePhysicalMemory, 0, kPage,
                                   initial);
    CHECK(physical != 0);
    CHECK_EQ(physical & (kPage - 1), 0u);
    MemoryQueryResult result{};
    CHECK_ST(query_memory_basic(physical + 0x20, &result), Status::Ok);
    CHECK_EQ((uint32_t)result.provenance,
             (uint32_t)MemoryQueryProvenance::PhysicalAllocation);
    CHECK_EQ(result.basic.base_address, physical);
    CHECK_EQ(result.basic.allocation_base, physical);
    CHECK_EQ(result.basic.region_size, kPage);
    CHECK_EQ(result.basic.state, kMemCommit);
    CHECK_EQ(result.basic.allocation_protect, initial);
    CHECK_EQ(result.basic.protect, initial);
    CHECK_EQ(result.basic.type, kMemPrivate);
    CHECK(result.protection_modifiers_known);

    bool modifiers_known = false;
    CHECK_EQ(query_address_protect(physical, &modifiers_known), initial);
    CHECK(modifiers_known);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, physical), initial);

    const uint32_t changed = kPageReadOnly | kPageNoCache;
    call(__imp__MmSetAddressProtect, physical, kPage, changed);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, physical + 7), changed);
    CHECK_ST(query_memory_basic(physical + 7, &result), Status::Ok);
    CHECK_EQ(result.basic.allocation_protect, initial);
    CHECK_EQ(result.basic.protect, changed);
    call(__imp__MmFreePhysicalMemory, 0, physical);

    // The physical heap also exposes the Xbox 4-KiB allocation class. Metadata
    // boundaries remain 4 KiB even though GuestMemory backs the containing
    // 64-KiB host page.
    const uint32_t small_protect = kPageReadWrite | kPageNoCache;
    const uint32_t small = call(__imp__MmAllocatePhysicalMemory, 0, 0x1000, small_protect);
    CHECK(small != 0);
    CHECK_ST(query_memory_basic(small + 0x100, &result), Status::Ok);
    CHECK_EQ(result.basic.base_address, small);
    CHECK_EQ(result.basic.region_size, 0x1000u);
    CHECK_EQ(result.basic.allocation_protect, small_protect);
    CHECK_EQ(result.basic.protect, small_protect);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, small + 0x100), small_protect);
    GuestHeapProtectionInfo small_info{};
    CHECK_ST(runtime()->physical.query_guest_protection(small + 0x100, &small_info), Status::Ok);
    CHECK_EQ(small_info.page_size, 0x1000u);

    // Modifier-only changes are representable at 4 KiB because host access
    // remains RW. Base-access changes would affect neighboring 4-KiB physical
    // allocations sharing one 64-KiB GuestMemory page and are rejected.
    const uint32_t small_changed = kPageReadWrite | kPageWriteCombine;
    call(__imp__MmSetAddressProtect, small, 0x1000, small_changed);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, small + 0x100), small_changed);
    uint32_t old_small = 0;
    CHECK_ST(runtime()->physical.protect_guest_range(
                 small, 0x1000, kPageReadOnly, Protect::Read, &old_small),
             Status::InvalidArgument);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, small + 0x100), small_changed);
    call(__imp__MmFreePhysicalMemory, 0, small);
}

void image_region() {
    constexpr uint32_t image_base = 0x82000000u;
    constexpr uint32_t image_bytes = 0x18000u;
    constexpr uint32_t header_bytes = 0x12Cu;
    constexpr uint32_t security = 24u;
    CHECK(mem.commit(image_base, 2 * kPage, Protect::ReadWrite) == MemStatus::Ok);
    CHECK(mem.commit(kHeader, kPage, Protect::ReadWrite) == MemStatus::Ok);

    // Minimal validated XEX2 header. Query provenance is derived from this
    // immutable copy retained by ModuleState, never from guest-writable LDR data.
    put(kHeader + 0, 0x58455832u);  // XEX2
    put(kHeader + 8, header_bytes);
    put(kHeader + 16, security);
    put(kHeader + 20, 0);           // zero optional-header records
    put(kHeader + security + 4, image_bytes);
    put(kHeader + security + 0x110, image_base);
    CHECK(mem.protect(kHeader, kPage, Protect::Read) == MemStatus::Ok);

    auto modules = std::make_unique<ModuleState>();
    modules->generation = runtime()->generation;
    modules->ready = true;
    modules->header_storage = kHeader;
    modules->header_size = header_bytes;
    runtime()->modules = std::move(modules);

    CHECK_EQ(call(__imp__NtQueryVirtualMemory, image_base + 0x2345, kOutput, 0), 0u);
    expect_output(image_base, image_base, kPageReadWrite, 2 * kPage,
                  kMemCommit, kPageReadWrite, kMemImage);
    MemoryQueryResult result{};
    CHECK_ST(query_memory_basic(image_base + 4, &result), Status::Ok);
    CHECK_EQ((uint32_t)result.provenance,
             (uint32_t)MemoryQueryProvenance::MainImage);
    CHECK(result.protection_modifiers_known);

    CHECK(mem.protect(image_base + kPage, kPage, Protect::Read) == MemStatus::Ok);
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, image_base + 1, kOutput, 0), 0u);
    expect_output(image_base, image_base, kPageReadWrite, kPage,
                  kMemCommit, kPageReadWrite, kMemImage);
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, image_base + kPage + 1, kOutput, 0), 0u);
    expect_output(image_base + kPage, image_base, kPageReadWrite, kPage,
                  kMemCommit, kPageReadOnly, kMemImage);
    bool modifiers_known = false;
    CHECK_EQ(query_address_protect(image_base + kPage, &modifiers_known), kPageReadOnly);
    CHECK(modifiers_known);
    CHECK_EQ(call(__imp__MmQueryAddressProtect, image_base + kPage), kPageReadOnly);
}

void untracked_committed_page() {
    MemoryQueryResult result{};
    // kScratch is intentionally a raw GuestMemory commit used by the test
    // harness. Its base GuestMemory protection is observable by the standalone
    // helper, but the HLE refuses it because no guest allocation provenance
    // exists for the complete Xbox result.
    bool modifiers_known = true;
    CHECK_EQ(query_address_protect(kScratch, &modifiers_known), kPageReadWrite);
    CHECK(!modifiers_known);
    CHECK_ST(query_memory_basic(kScratch, &result), Status::Unsupported);
}

void fatal_path_releases_family_lock() {
    uint32_t runtime_block = 0;
    CHECK_ST(runtime()->heap.alloc(kPage, kPage, true, &runtime_block), Status::Ok);
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__MmSetAddressProtect, runtime_block, kPage, kPageReadOnly), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(g_fatal_msg.find("MmSetAddressProtect") != std::string::npos);

    // MmSet looked up the allocation while holding virtual_allocation_mutex.
    // If its fatal escaped with that mutex held, this query would deadlock.
    MemoryQueryResult result{};
    CHECK_ST(query_memory_basic(runtime_block, &result), Status::Unsupported);
    CHECK_ST(runtime()->heap.free(runtime_block), Status::Ok);
}

void reserve_commit_decommit() {
    const auto initial = runtime()->heap.stats();
    put(kBaseWord, 0); put(kSizeWord, 3 * kPage);
    CHECK_EQ(call(__imp__NtAllocateVirtualMemory, kBaseWord, kSizeWord,
                  0x60002000u, kPageReadWrite), 0u);
    const uint32_t allocation = word(kBaseWord);
    CHECK(allocation != 0);
    CHECK(!mem.is_committed(allocation, 1));
    CHECK(!mem.is_accessible(allocation, 1, Protect::Read));
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, allocation + 1, kOutput), 0u);
    expect_output(allocation, allocation, kPageReadWrite, 3*kPage,
                  kMemReserve, 0, kMemPrivate);

    put(kBaseWord, allocation + kPage); put(kSizeWord, kPage);
    CHECK_EQ(call(__imp__NtAllocateVirtualMemory, kBaseWord, kSizeWord,
                  kMemCommit, kPageReadWrite | kPageNoCache), 0u);
    CHECK(!mem.is_committed(allocation, 1));
    CHECK(mem.is_accessible(allocation+kPage, kPage, Protect::ReadWrite));
    CHECK(!mem.is_committed(allocation+2*kPage, 1));
    CHECK_EQ(call(__imp__NtQueryVirtualMemory, allocation+kPage+1, kOutput), 0u);
    expect_output(allocation+kPage, allocation, kPageReadWrite, kPage,
                  kMemCommit, kPageReadWrite | kPageNoCache, kMemPrivate);
    put(allocation+kPage+4, 0xAABBCCDDu);
    CHECK_EQ(call(__imp__NtAllocateVirtualMemory, kBaseWord, kSizeWord,
                  kMemCommit, kPageReadWrite | kPageNoCache), 0u);
    CHECK_EQ(word(allocation+kPage+4), 0xAABBCCDDu);

    put(kBaseWord, allocation+kPage); put(kSizeWord, kPage);
    CHECK_EQ(call(__imp__NtFreeVirtualMemory, kBaseWord, kSizeWord, 0x4000u), 0u);
    CHECK(!mem.is_committed(allocation+kPage, 1));
    CHECK_EQ(call(__imp__NtAllocateVirtualMemory, kBaseWord, kSizeWord,
                  kMemCommit, kPageReadOnly), 0u);
    CHECK(mem.is_accessible(allocation+kPage, kPage, Protect::Read));
    CHECK(!mem.is_accessible(allocation+kPage, 1, Protect::ReadWrite));
    CHECK_EQ(word(allocation+kPage+4), 0u);

    // Bounds/ownership failures neither create a second allocation nor modify
    // valid committed bytes. Full release removes all reservation backing.
    put(kBaseWord, allocation+2*kPage); put(kSizeWord, 2*kPage);
    CHECK_EQ(call(__imp__NtAllocateVirtualMemory, kBaseWord, kSizeWord,
                  kMemCommit, kPageReadWrite), nt::kMemoryNotAllocated);
    CHECK(!mem.is_committed(allocation+2*kPage, 1));
    put(kBaseWord, allocation); put(kSizeWord, 0);
    CHECK_EQ(call(__imp__NtFreeVirtualMemory, kBaseWord, kSizeWord, 0x8000u), 0u);
    CHECK(!mem.is_committed(allocation+kPage, 1));
    CHECK_EQ(runtime()->heap.stats().live_allocations, initial.live_allocations);
    CHECK_EQ(runtime()->heap.stats().allocated_bytes, initial.allocated_bytes);
}

}  // namespace

int main() {
    setup();
    registration();
    virtual_regions();
    free_regions_and_bounds();
    physical_region();
    image_region();
    untracked_committed_page();
    fatal_path_releases_family_lock();
    reserve_commit_decommit();
    runtime_shutdown();
    clear_imports();
    return test_result("rt_query_memory");
}
