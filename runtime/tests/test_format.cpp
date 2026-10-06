// Original guest-ABI vectors plus optional independently recorded Windows CRT cases.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/format.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__sprintf);
PPC_EXTERN_FUNC(__imp___vsnprintf);
PPC_EXTERN_FUNC(__imp___snprintf);
PPC_EXTERN_FUNC(__imp__DbgPrint);
PPC_EXTERN_FUNC(__imp__KeBugCheckEx);
PPC_EXTERN_FUNC(__imp__DbgBreakPoint);
using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory memory;
constexpr uint32_t base = 0x30000000, destination = base + 0x1000;
constexpr uint32_t format = base + 0x10000, arguments = base + 0x20000;
constexpr uint32_t stack = base + 0x30000, strings = base + 0x40000, guard = base + 0x60000;
void put_string(uint32_t address, const std::string& text) {
    std::memcpy(memory.base() + address, text.data(), text.size());
    memory.base()[address + text.size()] = 0;
}
void put64(uint32_t address, uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte)
        memory.base()[address + byte] = uint8_t(value >> (56 - byte * 8));
}
void put_register(PPCContext& context, unsigned index, uint64_t value) {
    switch (index) {
    case 0: context.r3.u64 = value; break; case 1: context.r4.u64 = value; break;
    case 2: context.r5.u64 = value; break; case 3: context.r6.u64 = value; break;
    case 4: context.r7.u64 = value; break; case 5: context.r8.u64 = value; break;
    case 6: context.r9.u64 = value; break; case 7: context.r10.u64 = value; break;
    default: put64(stack + 0x50 + (index - 8) * 8, value); break;
    }
}
int32_t call(const std::string& text, const std::vector<uint64_t>& values, bool array,
             uint32_t count = 256, uint32_t output = destination) {
    put_string(format, text);
    alignas(64) PPCContext context{};
    context.r1.u64 = stack;
    context.r3.u64 = output;
    if (array) {
        context.r4.u64 = count; context.r5.u64 = format; context.r6.u64 = arguments;
        for (size_t i = 0; i < values.size(); ++i) put64(arguments + uint32_t(i) * 8, values[i]);
        __imp___vsnprintf(context, memory.base());
    } else {
        context.r4.u64 = format;
        for (size_t i = 0; i < values.size(); ++i) put_register(context, unsigned(i) + 2, values[i]);
        __imp__sprintf(context, memory.base());
    }
    return context.r3.s32;
}
// _snprintf(buffer, count, format, ...): variable arguments from r6, then the
// 64-bit overflow slots at SP+0x50 (argument index 8 and up), as for sprintf.
int32_t call_snprintf(const std::string& text, const std::vector<uint64_t>& values, uint32_t count = 256) {
    put_string(format, text);
    alignas(64) PPCContext context{};
    context.r1.u64 = stack;
    context.r3.u64 = destination;
    context.r4.u64 = count;
    context.r5.u64 = format;
    for (size_t i = 0; i < values.size(); ++i) put_register(context, unsigned(i) + 3, values[i]);
    __imp___snprintf(context, memory.base());
    return context.r3.s32;
}
void expect(const std::string& text, const std::vector<uint64_t>& values, const std::string& expected) {
    for (int form = 0; form < 3; ++form) {  // sprintf, _vsnprintf, _snprintf
        std::memset(memory.base() + destination, 0xA5, 512);
        CHECK_EQ(form == 2 ? call_snprintf(text, values) : call(text, values, form == 1), int32_t(expected.size()));
        CHECK(std::memcmp(memory.base() + destination, expected.data(), expected.size()) == 0);
        CHECK_EQ(memory.base()[destination + expected.size()], 0u);
        CHECK_EQ(memory.base()[destination + expected.size() + 1], 0xA5u);
    }
}
std::vector<uint8_t> unhex(const std::string& text) {
    std::vector<uint8_t> bytes;
    if (text.size() & 1) return bytes;
    for (size_t i = 0; i < text.size(); i += 2)
        bytes.push_back(uint8_t(std::stoul(text.substr(i, 2), nullptr, 16)));
    return bytes;
}
void differential(const char* path) {
    std::ifstream input(path);
    CHECK(bool(input));
    std::string line;
    unsigned count = 0, failed = 0;
    while (std::getline(input, line)) {
        std::istringstream columns(line);
        std::string capacity, format_hex, arguments_text, expected_return, output_hex;
        CHECK(bool(std::getline(columns, capacity, '|')));
        CHECK(bool(std::getline(columns, format_hex, '|')));
        CHECK(bool(std::getline(columns, arguments_text, '|')));
        CHECK(bool(std::getline(columns, expected_return, '|')));
        CHECK(bool(std::getline(columns, output_hex, '|')));
        const auto text = unhex(format_hex), expected = unhex(output_hex);
        std::vector<uint64_t> values;
        std::istringstream items(arguments_text);
        std::string item;
        uint32_t address = strings;
        while (arguments_text != "-" && std::getline(items, item, ',')) {
            if (item.substr(0, 2) == "u:") values.push_back(std::stoull(item.substr(2), nullptr, 16));
            else {
                const auto bytes = unhex(item.substr(2));
                std::memcpy(memory.base() + address, bytes.data(), bytes.size());
                memory.base()[address + bytes.size()] = 0;
                memory.base()[address + bytes.size() + 1] = 0;
                values.push_back(address);
                address += uint32_t(bytes.size()) + 16;
            }
        }
        std::memset(memory.base() + destination, 0xA5, 512);
        const std::string fmt(text.begin(), text.end());
        const int32_t result = call(fmt, values, true, uint32_t(std::stoul(capacity)));
        ++count;
        if (result != std::stoi(expected_return) ||
            std::memcmp(memory.base() + destination, expected.data(), expected.size()) != 0) {
            ++failed;
            std::fprintf(stderr, "FORMAT-DIFFERENTIAL mismatch case=%u format=%s return=%d expected=%s\n",
                         count, fmt.c_str(), result, expected_return.c_str());
        }
    }
    CHECK(count > 900); CHECK_EQ(failed, 0u);
    std::printf("FORMAT-DIFFERENTIAL checks=%u pass=%u fail=%u scope=host\n", count, count - failed, failed);
}
}

int main(int argc, char** argv) {
    CHECK(memory.reserve() == MemStatus::Ok);
    CHECK(memory.commit(base, 0x70000, Protect::ReadWrite) == MemStatus::Ok);
    CHECK_ST(runtime_init(&memory), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(register_xboxkrnl_format_hle(), Status::Ok);
    expect("%+08d|%#08x|%.0d|%#.0o", {uint32_t(-42), 42, 0, 0}, "-0000042|0x00002a||0");
    expect("%I64d %I64X %ld %p", {0x8000000000000000ull, 0xFEDCBA9876543210ull, uint32_t(-17), 0x12345678},
           "-9223372036854775808 FEDCBA9876543210 -17 12345678");
    expect("%u %u %u %u %u %u %I64X %u", {1,2,3,4,5,6,0x1122334455667788ull,0x99AABBCC},
           "1 2 3 4 5 6 1122334455667788 2578103244");
    put_string(strings, "abcdef");
    expect("%*.*s", {8, 3, strings}, "     abc");
    expect("%*.*s", {uint32_t(-8), uint32_t(-1), strings}, "abcdef  ");
    expect("%.0s!", {0xDEADBEEF}, "!");
    expect("%s", {0}, "(null)");
    memory.base()[strings] = 0x20; memory.base()[strings+1] = 0xAC;
    memory.base()[strings+2] = 0x00; memory.base()[strings+3] = 0xE9;
    memory.base()[strings+4] = memory.base()[strings+5] = 0;
    expect("%S", {strings}, std::string("\x80\xE9", 2));
    for (unsigned capacity : {1u,4u,5u,6u}) {
        std::memset(memory.base() + destination, 0xA5, 32);
        CHECK_EQ(call("ABCDE", {}, true, capacity), capacity < 5 ? -1 : 5);
        CHECK(std::memcmp(memory.base() + destination, "ABCDE", std::min(capacity, 5u)) == 0);
        CHECK_EQ(memory.base()[destination + std::min(capacity,5u)], capacity > 5 ? 0u : 0xA5u);
        // _snprintf truncates exactly as _vsnprintf (Windows msvcrt rule).
        std::memset(memory.base() + destination, 0xA5, 32);
        put_string(strings + 0x100, "CD");
        CHECK_EQ(call_snprintf("AB%sE", {strings + 0x100}, capacity), capacity < 5 ? -1 : 5);
        CHECK(std::memcmp(memory.base() + destination, "ABCDE", std::min(capacity, 5u)) == 0);
        CHECK_EQ(memory.base()[destination + std::min(capacity,5u)], capacity > 5 ? 0u : 0xA5u);
    }
    put_string(destination, "overlap");
    CHECK_EQ(call("<%s>", {destination}, false), 9);
    CHECK(std::strcmp(reinterpret_cast<const char*>(memory.base() + destination), "<overlap>") == 0);

    // All guest accesses are checked and no error partially writes output.
    bool fatal = false;
    std::memset(memory.base() + destination, 0xA5, 32);
    CAPTURE_FATAL(call("valid %q", {1}, false), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_EQ(memory.base()[destination], 0xA5u);
    CAPTURE_FATAL(call("%n", {destination}, false), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(memory.protect(guard, 0x10000, Protect::None) == MemStatus::Ok);
    CAPTURE_FATAL(call("%s", {guard}, false), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    CAPTURE_FATAL(call("ABCDEFG", {}, true, 8, guard - 4), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    CHECK(memory.protect(base, 0x10000, Protect::Read) == MemStatus::Ok);
    CAPTURE_FATAL(call("output", {}, false), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    CHECK(memory.protect(base, 0x10000, Protect::ReadWrite) == MemStatus::Ok);
    put_string(format, "%u %u %u %u %u %u %I64X");
    alignas(64) PPCContext context{};
    context.r1.u64 = 0xFFFFFFF0u; context.r3.u64 = destination; context.r4.u64 = format;
    CAPTURE_FATAL(__imp__sprintf(context, memory.base()), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    context = {}; context.r3.u64 = destination; context.r4.u64 = 8;
    context.r5.u64 = format; context.r6.u64 = 0xFFFFFFFC;
    CAPTURE_FATAL(__imp___vsnprintf(context, memory.base()), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    put_string(format, "RCOMP formatter test %u\n");
    context = {}; context.r3.u64 = format; context.r4.u64 = 17;
    __imp__DbgPrint(context, memory.base()); CHECK_EQ(context.r3.u32, 0u);
    context = {}; context.r3.u64 = 0x1234; context.r4.u64 = 1; context.r5.u64 = 2;
    context.r6.u64 = 3; context.r7.u64 = 4;
    CAPTURE_FATAL(__imp__KeBugCheckEx(context, memory.base()), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_TRAP);
    context = {}; context.lr = 0x82001234;
    CAPTURE_FATAL(__imp__DbgBreakPoint(context, memory.base()), fatal);
    CHECK(fatal && g_fatal_msg.find("DbgBreakPoint lr=0x82001234") != std::string::npos);
    context = {}; context.r3.u64 = 0x1234; context.r4.u64 = 1; context.r5.u64 = 2;
    context.r6.u64 = 3; context.r7.u64 = 4;
    CAPTURE_FATAL(__imp__KeBugCheckEx(context, memory.base()), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_TRAP);
    CHECK(g_fatal_msg.find("code=0x00001234 args=0x00000001,0x00000002,0x00000003,0x00000004") != std::string::npos);
    if (argc == 2) differential(argv[1]);
    runtime_shutdown(); clear_imports(); memory.release();
    return test_result("rt_format");
}
