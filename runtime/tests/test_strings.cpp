// Production Xbox/NT string helpers over real guest memory and GuestHeap.
#include <cstring>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_heap.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/strings.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__RtlFreeAnsiString);
PPC_EXTERN_FUNC(__imp__RtlMultiByteToUnicodeN);
PPC_EXTERN_FUNC(__imp__RtlNtStatusToDosError);
PPC_EXTERN_FUNC(__imp__RtlUnicodeStringToAnsiString);
PPC_EXTERN_FUNC(__imp__RtlUnicodeToMultiByteN);
PPC_EXTERN_FUNC(__imp__RtlUpcaseUnicodeChar);
PPC_EXTERN_FUNC(__imp__XeCryptSha);
PPC_EXTERN_FUNC(__imp__XeCryptShaFinal);
PPC_EXTERN_FUNC(__imp__XeCryptShaInit);
PPC_EXTERN_FUNC(__imp__XeCryptShaUpdate);

using namespace rcomp;
using namespace rcomp::rt;

namespace {

GuestMemory mem;
constexpr uint32_t kScratch = 0x31000000u;
constexpr uint32_t kAnsi = kScratch + 0x100;
constexpr uint32_t kUnicode = kScratch + 0x400;
constexpr uint32_t kWritten = kScratch + 0x800;
constexpr uint32_t kAnsiDesc = kScratch + 0x900;
constexpr uint32_t kUnicodeDesc = kScratch + 0x920;
constexpr uint32_t kBoundary = kScratch + 0x10000;

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

void call8(PPCFunc* fn, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
           uint32_t e, uint32_t f, uint32_t g, uint32_t h) {
    alignas(64) PPCContext context{};
    context.r3.u64 = a; context.r4.u64 = b; context.r5.u64 = c; context.r6.u64 = d;
    context.r7.u64 = e; context.r8.u64 = f; context.r9.u64 = g; context.r10.u64 = h;
    fn(context, mem.base());
}

uint32_t word(uint32_t address) {
    uint32_t value = 0;
    CHECK(guest_read_be32(address, &value));
    return value;
}

void put_word(uint32_t address, uint32_t value) { CHECK(guest_write_be32(address, value)); }

void put_be16(uint32_t address, uint16_t value) {
    mem.base()[address] = (uint8_t)(value >> 8);
    mem.base()[address + 1] = (uint8_t)value;
}

uint16_t get_be16(uint32_t address) {
    return (uint16_t)((uint16_t)mem.base()[address] << 8 | mem.base()[address + 1]);
}

void put_descriptor(uint32_t address, uint16_t length, uint16_t maximum, uint32_t buffer) {
    put_be16(address, length);
    put_be16(address + 2, maximum);
    put_word(address + 4, buffer);
}

void setup() {
    RuntimeConfig cfg;
    cfg.heap_lo = 0x41000000u;
    cfg.heap_hi = 0x41100000u;
    CHECK_ST(runtime_init(&mem, cfg), Status::Ok);
    clear_imports();
    CHECK_ST(register_xboxkrnl_strings_hle(), Status::Ok);
    CHECK_EQ(kAnsiCodePage, 1252u);
}

void registrations() {
    struct Expected { uint32_t ordinal; const char* name; };
    const Expected expected[] = {
        {0x127, "RtlFreeAnsiString"},
        {0x133, "RtlMultiByteToUnicodeN"},
        {0x135, "RtlNtStatusToDosError"},
        {0x142, "RtlUnicodeStringToAnsiString"},
        {0x143, "RtlUnicodeToMultiByteN"},
        {0x149, "RtlUpcaseUnicodeChar"},
        {0x18F, "XeCryptShaInit"},
        {0x190, "XeCryptShaUpdate"},
        {0x191, "XeCryptShaFinal"},
        {0x192, "XeCryptSha"},
    };
    for (const auto& e : expected) {
        CHECK(find_import(kModuleXboxkrnl, e.ordinal) != nullptr);
        CHECK(import_thunk(kModuleXboxkrnl, e.ordinal) != nullptr);
        CHECK(strcmp(import_registry_name(kModuleXboxkrnl, e.ordinal), e.name) == 0);
        uint32_t ordinal = 0;
        CHECK(export_ordinal(kModuleXboxkrnl, e.name, &ordinal));
        CHECK_EQ(ordinal, e.ordinal);
    }
    CHECK_EQ(unicode_from_ansi(0x80), 0x20ACu);
    CHECK_EQ(unicode_from_ansi(0x81), 0x0081u);
    CHECK_EQ(ansi_from_unicode(0x20AC), 0x80u);
    CHECK_EQ(ansi_from_unicode(0x03A9), 0x4Fu);
    CHECK_EQ(ansi_from_unicode(0xD800), (uint8_t)'?');
}

void multibyte_to_unicode() {
    const uint8_t input[] = {0x41, 0x80, 0x81, 0x82, 0xE9, 0xFF};
    memcpy(mem.base() + kAnsi, input, sizeof input);
    memset(mem.base() + kUnicode, 0xCC, 20);
    put_word(kWritten, 0xDEADBEEFu);

    CHECK_EQ(call(__imp__RtlMultiByteToUnicodeN, kUnicode, 8, kWritten, kAnsi, sizeof input), 0u);
    CHECK_EQ(word(kWritten), 8u);
    CHECK_EQ(get_be16(kUnicode + 0), 0x0041u);
    CHECK_EQ(get_be16(kUnicode + 2), 0x20ACu);
    CHECK_EQ(get_be16(kUnicode + 4), 0x0081u);
    CHECK_EQ(get_be16(kUnicode + 6), 0x201Au);
    CHECK_EQ(mem.base()[kUnicode + 8], 0xCCu);  // truncation is success, no over-write

    // Odd destination byte capacity only admits complete UTF-16 code units.
    memset(mem.base() + kUnicode, 0xCC, 8);
    CHECK_EQ(call(__imp__RtlMultiByteToUnicodeN, kUnicode, 3, kWritten, kAnsi, 2), 0u);
    CHECK_EQ(word(kWritten), 2u);
    CHECK_EQ(get_be16(kUnicode), 0x0041u);
    CHECK_EQ(mem.base()[kUnicode + 2], 0xCCu);

    // Zero effective length touches neither NULL buffer and reports zero bytes.
    put_word(kWritten, 0xFFFFFFFFu);
    CHECK_EQ(call(__imp__RtlMultiByteToUnicodeN, 0, 0, kWritten, 0, 0), 0u);
    CHECK_EQ(word(kWritten), 0u);
    CHECK_EQ(call(__imp__RtlMultiByteToUnicodeN, 0, 1, 0, 0, 99), 0u);

    bool fatal = false;
    memcpy(mem.base() + kAnsi, input, sizeof input);
    const uint8_t before[8] = {input[0], input[1], input[2], input[3], input[4], input[5], 0, 0};
    CAPTURE_FATAL(call(__imp__RtlMultiByteToUnicodeN, kAnsi, 8, kWritten, kAnsi, 4), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    CHECK(memcmp(mem.base() + kAnsi, before, 6) == 0);  // overlap rejected before mutation

    CAPTURE_FATAL(call(__imp__RtlMultiByteToUnicodeN, 0, 2, 0, kAnsi, 1), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
}

void unicode_to_multibyte() {
    // A, Euro, undefined C1 control, low quote, Omega (best fit -> 'O'), surrogate (default '?').
    const uint16_t input[] = {0x0041, 0x20AC, 0x0081, 0x201A, 0x03A9, 0xD800};
    for (size_t i = 0; i < sizeof input / sizeof input[0]; ++i) put_be16(kUnicode + (uint32_t)i * 2, input[i]);
    memset(mem.base() + kAnsi, 0xCC, 16);
    put_word(kWritten, 0xDEADBEEFu);
    CHECK_EQ(call(__imp__RtlUnicodeToMultiByteN, kAnsi, 6, kWritten, kUnicode, sizeof input), 0u);
    CHECK_EQ(word(kWritten), 6u);
    const uint8_t expected[] = {0x41, 0x80, 0x81, 0x82, 0x4F, 0x3F};
    CHECK(memcmp(mem.base() + kAnsi, expected, sizeof expected) == 0);

    // Source byte count is counted in bytes: a trailing odd byte is ignored.
    memset(mem.base() + kAnsi, 0xCC, 4);
    CHECK_EQ(call(__imp__RtlUnicodeToMultiByteN, kAnsi, 4, kWritten, kUnicode, 3), 0u);
    CHECK_EQ(word(kWritten), 1u);
    CHECK_EQ(mem.base()[kAnsi], 0x41u);
    CHECK_EQ(mem.base()[kAnsi + 1], 0xCCu);

    put_word(kWritten, 7);
    CHECK_EQ(call(__imp__RtlUnicodeToMultiByteN, 0, 0, kWritten, 0, 0), 0u);
    CHECK_EQ(word(kWritten), 0u);

    bool fatal = false;
    put_be16(kUnicode, 0x0041);
    put_be16(kUnicode + 2, 0x20AC);
    const uint8_t before[] = {0, 0x41, 0x20, 0xAC};
    CHECK(memcmp(mem.base() + kUnicode, before, sizeof before) == 0);
    CAPTURE_FATAL(call(__imp__RtlUnicodeToMultiByteN, kUnicode, 2, kWritten, kUnicode, 4), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    CHECK(memcmp(mem.base() + kUnicode, before, sizeof before) == 0);
}

void descriptor_conversion_and_free() {
    const uint16_t input[] = {0x0041, 0x20AC, 0x03A9, 0x00E9};
    for (size_t i = 0; i < sizeof input / sizeof input[0]; ++i) put_be16(kUnicode + (uint32_t)i * 2, input[i]);
    put_descriptor(kUnicodeDesc, 8, 8, kUnicode);

    // Native ntdll CP1252 oracle: a 3-byte caller buffer receives two payload
    // bytes + NUL, reports Length=2, leaves MaximumLength/Buffer intact, and
    // returns STATUS_BUFFER_OVERFLOW.
    memset(mem.base() + kAnsi, 0xCC, 8);
    put_descriptor(kAnsiDesc, 0x7777, 3, kAnsi);
    CHECK_EQ(call(__imp__RtlUnicodeStringToAnsiString, kAnsiDesc, kUnicodeDesc, 0), 0x80000005u);
    CHECK_EQ(get_be16(kAnsiDesc), 2u);
    CHECK_EQ(get_be16(kAnsiDesc + 2), 3u);
    CHECK_EQ(word(kAnsiDesc + 4), kAnsi);
    CHECK_EQ(mem.base()[kAnsi + 0], 0x41u);
    CHECK_EQ(mem.base()[kAnsi + 1], 0x80u);
    CHECK_EQ(mem.base()[kAnsi + 2], 0u);
    CHECK_EQ(mem.base()[kAnsi + 3], 0xCCu);

    // Odd UNICODE_STRING Length follows ntdll: floor to whole WCHARs.
    put_descriptor(kUnicodeDesc, 3, 8, kUnicode);
    memset(mem.base() + kAnsi, 0xCC, 8);
    put_descriptor(kAnsiDesc, 0x7777, 8, kAnsi);
    CHECK_EQ(call(__imp__RtlUnicodeStringToAnsiString, kAnsiDesc, kUnicodeDesc, 0), 0u);
    CHECK_EQ(get_be16(kAnsiDesc), 1u);
    CHECK_EQ(mem.base()[kAnsi], 0x41u);
    CHECK_EQ(mem.base()[kAnsi + 1], 0u);

    // Allocate=TRUE owns an independent GuestHeap block with exact descriptor fields.
    put_descriptor(kUnicodeDesc, 8, 8, kUnicode);
    put_descriptor(kAnsiDesc, 0xAAAA, 0xBBBB, 0xCCCCCCCCu);
    const GuestHeapStats before = runtime()->heap.stats();
    CHECK_EQ(call(__imp__RtlUnicodeStringToAnsiString, kAnsiDesc, kUnicodeDesc, 1), 0u);
    CHECK_EQ(get_be16(kAnsiDesc), 4u);
    CHECK_EQ(get_be16(kAnsiDesc + 2), 5u);
    const uint32_t owned = word(kAnsiDesc + 4);
    CHECK(owned != 0);
    CHECK_EQ(runtime()->ansi_string_allocations.size(), 1u);
    const uint8_t expected[] = {0x41, 0x80, 0x4F, 0xE9, 0};
    CHECK(memcmp(mem.base() + owned, expected, sizeof expected) == 0);
    CHECK_EQ(runtime()->heap.stats().live_allocations, before.live_allocations + 1);

    call(__imp__RtlFreeAnsiString, kAnsiDesc);
    CHECK_EQ(word(kAnsiDesc), 0u);
    CHECK_EQ(word(kAnsiDesc + 4), 0u);
    CHECK(runtime()->ansi_string_allocations.empty());
    CHECK_EQ(runtime()->heap.stats().live_allocations, before.live_allocations);
    // Freeing the reset NULL string is idempotent and does not touch the heap.
    call(__imp__RtlFreeAnsiString, kAnsiDesc);
    CHECK_EQ(runtime()->heap.stats().live_allocations, before.live_allocations);

    // Empty allocated conversion still owns one byte for its terminator.
    put_descriptor(kUnicodeDesc, 0, 0, 0);
    put_descriptor(kAnsiDesc, 0, 0, 0);
    CHECK_EQ(call(__imp__RtlUnicodeStringToAnsiString, kAnsiDesc, kUnicodeDesc, 1), 0u);
    CHECK_EQ(get_be16(kAnsiDesc), 0u);
    CHECK_EQ(get_be16(kAnsiDesc + 2), 1u);
    CHECK_EQ(mem.base()[word(kAnsiDesc + 4)], 0u);
    call(__imp__RtlFreeAnsiString, kAnsiDesc);

    // RtlFreeAnsiString must not release an arbitrary heap allocation.
    uint32_t foreign = 0;
    CHECK_ST(runtime()->heap.alloc(32, 16, false, &foreign), Status::Ok);
    put_descriptor(kAnsiDesc, 4, 32, foreign);
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__RtlFreeAnsiString, kAnsiDesc), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    uint32_t foreign_size = 0;
    CHECK_ST(runtime()->heap.allocation_size(foreign, &foreign_size), Status::Ok);
    CHECK_EQ(foreign_size, 32u);
    CHECK_ST(runtime()->heap.free(foreign), Status::Ok);
}

void allocation_failure_is_transactional() {
    put_be16(kUnicode, 0x0041);
    put_descriptor(kUnicodeDesc, 2, 2, kUnicode);
    put_descriptor(kAnsiDesc, 0x1111, 0x2222, 0x33333333u);
    const auto initial = runtime()->heap.stats();
    uint32_t exhaust = 0;
    CHECK_ST(runtime()->heap.alloc((uint32_t)initial.free_bytes, 16, false, &exhaust), Status::Ok);
    const auto full = runtime()->heap.stats();
    CHECK_EQ(call(__imp__RtlUnicodeStringToAnsiString, kAnsiDesc, kUnicodeDesc, 1), nt::kNoMemory);
    CHECK_EQ(word(kAnsiDesc), 0x11112222u);
    CHECK_EQ(word(kAnsiDesc + 4), 0x33333333u);
    CHECK(runtime()->ansi_string_allocations.empty());
    CHECK_EQ(runtime()->heap.stats().live_allocations, full.live_allocations);
    CHECK_ST(runtime()->heap.free(exhaust), Status::Ok);
    CHECK_EQ(runtime()->heap.stats().live_allocations, initial.live_allocations);
}

void upcase_and_status_mapping() {
    CHECK_EQ(call(__imp__RtlUpcaseUnicodeChar, 'a'), (uint32_t)'A');
    CHECK_EQ(call(__imp__RtlUpcaseUnicodeChar, 0x00E9), 0x00C9u);
    CHECK_EQ(call(__imp__RtlUpcaseUnicodeChar, 0x00FF), 0x0178u);
    CHECK_EQ(call(__imp__RtlUpcaseUnicodeChar, 0x00DF), 0x00DFu);  // ntdll single-WCHAR behavior
    CHECK_EQ(call(__imp__RtlUpcaseUnicodeChar, 0x03BC), 0x039Cu);
    CHECK_EQ(call(__imp__RtlUpcaseUnicodeChar, 0x0430), 0x0410u);
    CHECK_EQ(call(__imp__RtlUpcaseUnicodeChar, 0xD800), 0xD800u);

    struct Mapping { uint32_t status; uint32_t dos; };
    const Mapping mappings[] = {
        {0x00000000u, 0u}, {0x00000102u, 1460u}, {0x80000005u, 234u},
        {0xC0000001u, 31u}, {0xC0000002u, 1u}, {0xC0000005u, 998u},
        {0xC0000008u, 6u}, {0xC000000Du, 87u}, {0xC0000011u, 38u},
        {0xC0000017u, 8u}, {0xC0000018u, 487u}, {0xC0000022u, 5u},
        {0xC0000024u, 6u}, {0xC0000033u, 123u}, {0xC0000034u, 2u},
        {0xC0000035u, 183u}, {0xC000003Au, 3u}, {0xC000009Au, 1450u},
        {0xC00000A0u, 487u}, {0xC00000BAu, 5u}, {0xC0000185u, 1117u},
    };
    for (const auto& m : mappings) CHECK_EQ(call(__imp__RtlNtStatusToDosError, m.status), m.dos);
    CHECK_EQ(call(__imp__RtlNtStatusToDosError, 0xDEADBEEFu), 317u);
}

void sha1_vectors() {
    constexpr uint8_t empty_digest[20] = {
        0xDA,0x39,0xA3,0xEE,0x5E,0x6B,0x4B,0x0D,0x32,0x55,
        0xBF,0xEF,0x95,0x60,0x18,0x90,0xAF,0xD8,0x07,0x09,
    };
    constexpr uint8_t abc_digest[20] = {
        0xA9,0x99,0x3E,0x36,0x47,0x06,0x81,0x6A,0xBA,0x3E,
        0x25,0x71,0x78,0x50,0xC2,0x6C,0x9C,0xD0,0xD8,0x9D,
    };
    constexpr uint8_t long_digest[20] = {
        0x84,0x98,0x3E,0x44,0x1C,0x3B,0xD2,0x6E,0xBA,0xAE,
        0x4A,0xA1,0xF9,0x51,0x29,0xE5,0xE5,0x46,0x70,0xF1,
    };

    memset(mem.base() + kAnsi, 0xCC, 64);
    call8(__imp__XeCryptSha, 0,0,0,0,0,0,kAnsi,20);
    CHECK(memcmp(mem.base() + kAnsi, empty_digest, 20) == 0);

    memcpy(mem.base() + kUnicode, "abc", 3);
    memset(mem.base() + kAnsi, 0xCC, 32);
    call8(__imp__XeCryptSha, kUnicode,3,0,0,0,0,kAnsi,20);
    CHECK(memcmp(mem.base() + kAnsi, abc_digest, 20) == 0);
    CHECK_EQ(mem.base()[kAnsi + 20], 0xCCu);

    // OutputSize greater than SHA-1's 20-byte digest does not extend the write.
    memset(mem.base() + kAnsi, 0xCC, 32);
    call8(__imp__XeCryptSha, kUnicode,3,0,0,0,0,kAnsi,32);
    CHECK(memcmp(mem.base() + kAnsi, abc_digest, 20) == 0);
    CHECK_EQ(mem.base()[kAnsi + 20], 0xCCu);

    constexpr char message[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static_assert(sizeof(message) - 1 == 56, "FIPS SHA-1 vector length");
    memcpy(mem.base() + kUnicode, message, 56);
    memset(mem.base() + kAnsi, 0xCC, 32);
    call8(__imp__XeCryptSha, kUnicode,20,kUnicode+20,20,kUnicode+40,16,kAnsi,20);
    CHECK(memcmp(mem.base() + kAnsi, long_digest, 20) == 0);

    // OutputSize truncates the digest and does not overwrite the next byte.
    memset(mem.base() + kAnsi, 0xCC, 32);
    call8(__imp__XeCryptSha, kUnicode,20,kUnicode+20,20,kUnicode+40,16,kAnsi,7);
    CHECK(memcmp(mem.base() + kAnsi, long_digest, 7) == 0);
    CHECK_EQ(mem.base()[kAnsi + 7], 0xCCu);

    // All input is consumed before output is written, so exact input/output
    // overlap is deterministic and useful for callers reusing scratch space.
    memcpy(mem.base() + kAnsi, "abc", 3);
    call8(__imp__XeCryptSha, kAnsi,3,0,0,0,0,kAnsi,20);
    CHECK(memcmp(mem.base() + kAnsi, abc_digest, 20) == 0);

    // A zero-sized output may be NULL; a non-zero buffer with NULL input is a
    // guest pointer error rather than silently hashing fewer bytes.
    call8(__imp__XeCryptSha, 0,0,0,0,0,0,0,0);
    bool fatal = false;
    CAPTURE_FATAL(call8(__imp__XeCryptSha, 0,1,0,0,0,0,kAnsi,20), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
}

// XeCryptShaInit/Update/Final keep the running hash in the guest's 88-byte XECRYPT_SHA_STATE: be32 count of bytes hashed, be32 state[5], u8 buffer[64].
constexpr uint32_t kShaState = kScratch + 0xA00;
constexpr uint32_t kShaInput = kScratch + 0x1000;
constexpr uint32_t kShaWhole = kScratch + 0x2000;

// Hashes data[0, length) through the streaming imports, alternating between two update sizes (a size of 0 is an empty update), into out[20].
void sha1_stream(const uint8_t* data, uint32_t length, uint32_t first_size, uint32_t second_size, uint8_t* out) {
    call(__imp__XeCryptShaInit, kShaState);
    uint32_t done = 0;
    bool first = true;
    while (done < length) {
        uint32_t take = first ? first_size : second_size;
        first = !first;
        if (take > length - done) take = length - done;
        memcpy(mem.base() + kShaInput, data + done, take);
        call(__imp__XeCryptShaUpdate, kShaState, kShaInput, take);
        done += take;
    }
    memset(mem.base() + kAnsi, 0xCC, 32);
    call(__imp__XeCryptShaFinal, kShaState, kAnsi, 20);
    memcpy(out, mem.base() + kAnsi, 20);
}

void sha1_streaming() {
    constexpr uint8_t abc_digest[20] = {
        0xA9,0x99,0x3E,0x36,0x47,0x06,0x81,0x6A,0xBA,0x3E,
        0x25,0x71,0x78,0x50,0xC2,0x6C,0x9C,0xD0,0xD8,0x9D,
    };
    constexpr uint8_t long_digest[20] = {
        0x84,0x98,0x3E,0x44,0x1C,0x3B,0xD2,0x6E,0xBA,0xAE,
        0x4A,0xA1,0xF9,0x51,0x29,0xE5,0xE5,0x46,0x70,0xF1,
    };
    // FIPS 180 test vector: one million repetitions of 'a'.
    constexpr uint8_t million_digest[20] = {
        0x34,0xAA,0x97,0x3C,0xD4,0xC4,0xDA,0xA4,0xF6,0x1E,
        0xEB,0x2B,0xDB,0xAD,0x27,0x31,0x65,0x34,0x01,0x6F,
    };

    // Init: zero count, the SHA-1 initial chaining value, empty buffer, and nothing written past the 88 bytes.
    memset(mem.base() + kShaState, 0xCC, 96);
    call(__imp__XeCryptShaInit, kShaState);
    CHECK_EQ(word(kShaState), 0u);
    CHECK_EQ(word(kShaState + 4), 0x67452301u);
    CHECK_EQ(word(kShaState + 8), 0xEFCDAB89u);
    CHECK_EQ(word(kShaState + 12), 0x98BADCFEu);
    CHECK_EQ(word(kShaState + 16), 0x10325476u);
    CHECK_EQ(word(kShaState + 20), 0xC3D2E1F0u);
    for (unsigned i = 24; i < 88; ++i) CHECK_EQ(mem.base()[kShaState + i], 0u);
    CHECK_EQ(mem.base()[kShaState + 88], 0xCCu);

    // The pending bytes and the count are visible in the state until a block completes.
    memcpy(mem.base() + kShaInput, "abc", 3);
    call(__imp__XeCryptShaUpdate, kShaState, kShaInput, 3);
    CHECK_EQ(word(kShaState), 3u);
    CHECK(memcmp(mem.base() + kShaState + 24, "abc", 3) == 0);
    CHECK_EQ(word(kShaState + 4), 0x67452301u);
    memset(mem.base() + kAnsi, 0xCC, 32);
    call(__imp__XeCryptShaFinal, kShaState, kAnsi, 20);
    CHECK(memcmp(mem.base() + kAnsi, abc_digest, 20) == 0);
    CHECK_EQ(mem.base()[kAnsi + 20], 0xCCu);

    // An output size below 20 truncates the digest and leaves the next byte alone.
    call(__imp__XeCryptShaInit, kShaState);
    call(__imp__XeCryptShaUpdate, kShaState, kShaInput, 3);
    memset(mem.base() + kAnsi, 0xCC, 32);
    call(__imp__XeCryptShaFinal, kShaState, kAnsi, 7);
    CHECK(memcmp(mem.base() + kAnsi, abc_digest, 7) == 0);
    CHECK_EQ(mem.base()[kAnsi + 7], 0xCCu);

    // The FIPS two-block message, split so that updates straddle the 64-byte block boundary.
    constexpr char message[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t digest[20];
    const uint32_t splits[][2] = {{1, 1}, {20, 35}, {55, 1}, {56, 0}, {0, 56}, {7, 49}, {64, 64}};
    for (const auto& split : splits) {
        sha1_stream(reinterpret_cast<const uint8_t*>(message), 56, split[0], split[1], digest);
        CHECK(memcmp(digest, long_digest, 20) == 0);
    }

    // Any chunking of a 1000-byte message agrees with the one-shot XeCryptSha (which the vectors above anchor).
    uint8_t blob[1000];
    for (unsigned i = 0; i < sizeof blob; ++i) blob[i] = (uint8_t)(i * 7u + 3u);
    memcpy(mem.base() + kShaWhole, blob, sizeof blob);
    memset(mem.base() + kAnsi, 0xCC, 32);
    call8(__imp__XeCryptSha, kShaWhole,400,kShaWhole+400,400,kShaWhole+800,200,kAnsi,20);
    uint8_t oneshot[20];
    memcpy(oneshot, mem.base() + kAnsi, 20);
    const uint32_t sizes[][2] = {{1, 1}, {63, 65}, {64, 64}, {64, 1}, {65, 63}, {7, 120}, {999, 1}, {1000, 1}, {128, 3}};
    for (const auto& size : sizes) {
        sha1_stream(blob, sizeof blob, size[0], size[1], digest);
        CHECK(memcmp(digest, oneshot, 20) == 0);
    }

    // A count past 16 bits and many updates: one million 'a'.
    memset(mem.base() + kShaWhole, 'a', 4000);
    call(__imp__XeCryptShaInit, kShaState);
    for (unsigned i = 0; i < 250; ++i) call(__imp__XeCryptShaUpdate, kShaState, kShaWhole, 4000);
    CHECK_EQ(word(kShaState), 1000000u);
    memset(mem.base() + kAnsi, 0xCC, 32);
    call(__imp__XeCryptShaFinal, kShaState, kAnsi, 20);
    CHECK(memcmp(mem.base() + kAnsi, million_digest, 20) == 0);

    // Empty updates are legal (even with a NULL input); a guest pointer error stops the title instead of hashing fewer bytes.
    call(__imp__XeCryptShaInit, kShaState);
    call(__imp__XeCryptShaUpdate, kShaState, 0, 0);
    CHECK_EQ(word(kShaState), 0u);
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__XeCryptShaUpdate, kShaState, 0, 1), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    fatal = false;
    CAPTURE_FATAL(call(__imp__XeCryptShaInit, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    fatal = false;
    CAPTURE_FATAL(call(__imp__XeCryptShaFinal, kShaState, 0, 20), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
}

void guard_and_overlap_cases() {
    // A truncated conversion reads only the source characters it can emit. A
    // later protected page is not probed.
    mem.base()[kBoundary - 1] = 'A';
    CHECK(mem.protect(kBoundary, 0x10000, Protect::None) == MemStatus::Ok);
    CHECK_EQ(call(__imp__RtlMultiByteToUnicodeN, kUnicode, 2, kWritten, kBoundary - 1, 0x1000), 0u);
    CHECK_EQ(get_be16(kUnicode), 0x0041u);
    CHECK_EQ(word(kWritten), 2u);
    CHECK(mem.protect(kBoundary, 0x10000, Protect::ReadWrite) == MemStatus::Ok);

    // Byte-count pointer overlap is rejected before either output changes.
    mem.base()[kAnsi] = 'A';
    memset(mem.base() + kUnicode, 0xCC, 8);
    bool fatal = false;
    CAPTURE_FATAL(call(__imp__RtlMultiByteToUnicodeN, kUnicode, 2, kUnicode, kAnsi, 1), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    for (unsigned i = 0; i < 8; ++i) CHECK_EQ(mem.base()[kUnicode + i], 0xCCu);
}

}  // namespace

int main() {
    if (mem.reserve() != MemStatus::Ok) return 2;
    if (mem.commit(kScratch, 0x30000, Protect::ReadWrite) != MemStatus::Ok) return 2;
    setup();
    registrations();
    multibyte_to_unicode();
    unicode_to_multibyte();
    descriptor_conversion_and_free();
    allocation_failure_is_transactional();
    upcase_and_status_mapping();
    sha1_vectors();
    sha1_streaming();
    guard_and_overlap_cases();
    runtime_shutdown();
    clear_imports();
    return test_result("rt_strings");
}
