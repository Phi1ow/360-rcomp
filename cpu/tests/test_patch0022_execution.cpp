// Original fixtures only (patch0022_provider.s, patch0022_consumer.s). Links the
// unmodified generated C++ of two modules, each generated with its own
// symbol_prefix, into one program and runs it: the consumer's import of
// Provider.dll ordinal 1 calls the address its import slot holds (unfilled: the
// call reaches the import record word and stops; filled as a loader does: the
// provider's function runs), its kernel import reaches the one host __imp__ thunk, and its
// import of a library no module provides stays an explicit unresolved import.
// The runtime hooks are the test doubles of patch0022_hooks.h.
#include <ppc_recomp_shared.h>
#include <patch0022_decls.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <sys/mman.h>

namespace {
uint8_t* image = nullptr;
int failures = 0;
int indirectCalls = 0;
int kernelCalls = 0;
constexpr uint32_t kStack = 0x40000000;

struct Unmapped { uint32_t target; };
struct Unresolved { std::string module; uint32_t ordinal; };
struct Trapped { uint32_t address; };

void check(const char* what, uint64_t got, uint64_t expected) {
    const bool ok = got == expected;
    std::printf("patch0022/%s %s got=0x%llX expected=0x%llX\n", what, ok ? "PASS" : "FAIL",
                (unsigned long long)got, (unsigned long long)expected);
    failures += !ok;
}
PPCFunc* lookup(uint32_t target) {
    for (const PPCFuncMapping* table : {kProviderTable, kConsumerTable})
        for (const PPCFuncMapping* m = table; m->host; ++m)
            if (m->guest == target) return m->host;
    return nullptr;
}
uint32_t call(uint32_t guest, uint32_t r3, uint32_t r4) {
    PPCFunc* fn = lookup(guest);
    if (!fn) throw Unmapped{guest};
    PPCContext ctx{};
    ctx.r1.u64 = kStack + 0x8000;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    fn(ctx, image);
    return ctx.r3.u32;
}
bool load(const char* path, uint32_t guest_base, uint32_t bytes) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    const bool ok = mprotect(image + guest_base, (bytes + 0xFFFF) & ~0xFFFFu, PROT_READ | PROT_WRITE) == 0 &&
                    std::fread(image + guest_base, 1, bytes, f) == bytes;
    std::fclose(f);
    return ok;
}
}  // namespace

[[noreturn]] void TESTDOUBLE_guest_trap(uint32_t address) { throw Trapped{address}; }
[[noreturn]] void TESTDOUBLE_unresolved_import(const char* module, uint32_t ordinal) {
    throw Unresolved{module, ordinal};
}
void TESTDOUBLE_call_indirect(PPCContext& ctx, uint8_t* base, uint32_t target) {
    ++indirectCalls;
    PPCFunc* fn = lookup(target);
    if (!fn) throw Unmapped{target};
    fn(ctx, base);
}
void TESTDOUBLE_kernel_frequency(PPCContext& ctx) {
    ++kernelCalls;
    ctx.r3.u64 = 0x1234;
}
// The host defines each kernel thunk once (runtime/src/import_thunks.cpp); every module calls it.
PPC_FUNC(__imp__KeQueryPerformanceFrequency) {
    (void)base;
    TESTDOUBLE_kernel_frequency(ctx);
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    image = static_cast<uint8_t*>(mmap(nullptr, size_t(1) << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (image == MAP_FAILED || mprotect(image + kStack, 0x10000, PROT_READ | PROT_WRITE) ||
        !load(argv[1], kProviderBase, kProviderBytes) || !load(argv[2], kConsumerBase, kConsumerBytes))
        return 2;

    // Both entry points are in the tables (each module's _xstart carries its prefix).
    check("provider_entry", call(kProviderEntry, 0, 0), 1);
    check("consumer_entry", call(kConsumerEntry, 0, 0), 2);

    // Before a loader fills it, the slot holds the XEX import record (type 0 << 24 | ordinal 1): the
    // call goes to that word, which no module defines, and stops there.
    uint32_t record;
    std::memcpy(&record, image + kSlotProviderAdd, 4);
    record = __builtin_bswap32(record);
    check("unfilled_slot_holds_the_import_record", record, 1);
    uint32_t reached = 0xFFFFFFFFu;
    try {
        call(kConsumerCallAdd, 3, 4);
    } catch (const Unmapped& u) {
        reached = u.target;
    }
    check("unfilled_slot_stops_at_the_record_word", reached, record);

    // The loader stores the export's address in the slot (big-endian guest word).
    const uint32_t be = __builtin_bswap32(kProviderAdd);
    std::memcpy(image + kSlotProviderAdd, &be, 4);
    indirectCalls = 0;
    check("module_import_calls_provider", call(kConsumerCallAdd, 3, 4), 107);
    check("module_import_one_indirect_call", indirectCalls, 1);
    // The thunk address itself is a function (indirect calls through it, e.g. a function pointer).
    check("thunk_address_mapped", call(kThunkProviderAdd, 20, 22), 42);

    check("kernel_import_shared_thunk", call(kConsumerCallKernel, 0, 0), 0x1235);
    check("kernel_import_calls", kernelCalls, 1);

    std::string module = "(none)";
    uint32_t ordinal = 0;
    try {
        call(kConsumerCallMissing, 0, 0);
    } catch (const Unresolved& u) {
        module = u.module;
        ordinal = u.ordinal;
    }
    check("unprovided_import_stays_unresolved", module == "Missing_dll" && ordinal == 7, 1);

    std::printf("patch0022/execution %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
