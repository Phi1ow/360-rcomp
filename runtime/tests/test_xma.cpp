// Exercise the production XMA register provider. A kick must not publish
// consumed input while this runtime has no decoder or PCM output.
#include <array>
#include <cstring>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/virtual_fields.h"
#include "rcomp/runtime/xma.h"
#include "rcomp/xenos_gpu.h"
#include "test_util.h"

using namespace rcomp;
using namespace rcomp::rt;

namespace {
constexpr uint32_t kRegisterBase = 0x7FEA0000u;
constexpr uint32_t kContextArrayAddress = 0x600;
constexpr uint32_t kKick = 0x650;
constexpr uint32_t kLock = 0x690;
constexpr uint32_t kClear = 0x6A0;
constexpr uint32_t kContextBytes = 64;
constexpr uint32_t kInputValid = 0x00300000u;

uint32_t read_register(uint32_t index) {
    uint64_t value = 0;
    CHECK_EQ(runtime_virtual_read(kRegisterBase + 4 * index, 4, 0, &value),
             VirtualAccessStatus::Handled);
    return __builtin_bswap32(uint32_t(value));
}

void write_register(uint32_t index, uint32_t value) {
    CHECK_EQ(runtime_virtual_write(kRegisterBase + 4 * index, 4,
                                   __builtin_bswap32(value), 0),
             VirtualAccessStatus::Handled);
}
}  // namespace

int main() {
    GuestMemory memory;
    CHECK(memory.reserve() == MemStatus::Ok);
    CHECK(memory.commit(0x30000000, 0x10000, Protect::ReadWrite) == MemStatus::Ok);
    CHECK_ST(runtime_init(&memory), Status::Ok);
    CHECK_ST(register_xboxkrnl_xma_hle(), Status::Ok);

    const uint32_t physical = read_register(kContextArrayAddress);
    const uint32_t array = physical + xenos::kXenosPhysicalWindow;
    CHECK(memory.is_accessible(array, 320 * kContextBytes, Protect::ReadWrite));
    CHECK_ST(register_xboxkrnl_xma_hle(), Status::Ok);
    CHECK_EQ(read_register(kContextArrayAddress), physical);
    CHECK_EQ(runtime_virtual_write(kRegisterBase + 4 * kContextArrayAddress, 4,
                                   0, 0), VirtualAccessStatus::ReadOnly);
    write_register(0x607, 0xFFFFFFFFu);
    CHECK_EQ(read_register(0x607), 0xFFFFFFFFu % 320u);
    write_register(0x606, 0xFFFFFFFFu);
    CHECK(read_register(0x606) < 320);

    PPCFunc* create = find_import(kModuleXboxkrnl, 0x0224);
    PPCFunc* release = find_import(kModuleXboxkrnl, 0x0226);
    CHECK(create && release);
    if (!create || !release) return test_result("rt_xma");
    PPCContext context{};
    context.r3.u64 = 0x30000000;
    create(context, memory.base());
    CHECK_EQ(context.r3.u32, 0u);
    uint32_t allocated = 0;
    CHECK(guest_read_be32(0x30000000, &allocated));
    CHECK_EQ(allocated, array);

    // Cross register groups and the array's final context. Snapshot all
    // words, including input valid flags, packet cursors and output fields.
    constexpr std::array<uint32_t, 4> indices{0, 31, 32, 319};
    std::array<std::array<uint8_t, kContextBytes>, indices.size()> before{};
    for (size_t slot = 0; slot < indices.size(); ++slot) {
        const uint32_t index = indices[slot];
        const uint32_t address = array + index * kContextBytes;
        for (uint32_t word = 0; word < kContextBytes / 4; ++word) {
            const uint32_t value = word == 0 ? (0xA5300000u | index)
                                             : (0x51000000u | (index << 8) | word);
            CHECK(guest_write_be32(address + 4 * word, value));
        }
        std::memcpy(before[slot].data(), memory.base() + address, kContextBytes);
    }

    write_register(kKick, 0x80000001u);
    write_register(kKick + 1, 1u);
    write_register(kKick + 9, 0x80000000u);
    CHECK_EQ(read_register(kKick), 0x80000001u);
    CHECK_EQ(read_register(kKick + 1), 1u);
    CHECK_EQ(read_register(kKick + 9), 0x80000000u);
    for (size_t slot = 0; slot < indices.size(); ++slot) {
        const uint32_t address = array + indices[slot] * kContextBytes;
        uint32_t word0 = 0;
        CHECK(guest_read_be32(address, &word0));
        CHECK_EQ(word0 & kInputValid, kInputValid);
        CHECK(std::memcmp(before[slot].data(), memory.base() + address, kContextBytes) == 0);
    }
    write_register(kLock, 0x01020304u);
    write_register(kClear + 9, 0x05060708u);
    CHECK_EQ(read_register(kLock), 0x01020304u);
    CHECK_EQ(read_register(kClear + 9), 0x05060708u);
    CHECK_EQ(runtime_virtual_read(kRegisterBase + 4 * kKick, 8, 0, nullptr),
             VirtualAccessStatus::InvalidWidth);

    context.r3.u64 = allocated;
    release(context, memory.base());
    runtime_shutdown();
    clear_imports();
    return test_result("rt_xma");
}
