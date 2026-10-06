// xboxkrnl HLE exports, called through the same symbols generated code uses.
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/clock_sync.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__NtAllocateVirtualMemory);
PPC_EXTERN_FUNC(__imp__NtFreeVirtualMemory);
PPC_EXTERN_FUNC(__imp__NtCreateFile);
PPC_EXTERN_FUNC(__imp__NtOpenFile);
PPC_EXTERN_FUNC(__imp__NtReadFile);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__KeQuerySystemTime);
PPC_EXTERN_FUNC(__imp__ExTerminateThread);
PPC_EXTERN_FUNC(__imp__NtQueryVirtualMemory);

using namespace rcomp;
using namespace rcomp::rt;

namespace {
uint8_t* g_base;
uint32_t g_scratch;  // guest scratch block for arguments

uint32_t rd32(uint32_t a) {
    uint32_t v = 0;
    guest_read_be32(a, &v);
    return v;
}
void wr32(uint32_t a, uint32_t v) { guest_write_be32(a, v); }

// Guest layout inside the scratch block.
enum : uint32_t {
    kPBase = 0x00, kPSize = 0x04, kHandle = 0x08, kIosb = 0x10, kOffset = 0x20,
    kAttrs = 0x30, kAnsi = 0x40, kName = 0x50, kTime = 0x100, kBuf = 0x200,
};

void set_name(const std::string& s) {
    memcpy(g_base + g_scratch + kName, s.data(), s.size());
    uint16_t len = __builtin_bswap16((uint16_t)s.size());
    memcpy(g_base + g_scratch + kAnsi, &len, 2);
    memcpy(g_base + g_scratch + kAnsi + 2, &len, 2);
    wr32(g_scratch + kAnsi + 4, g_scratch + kName);
    wr32(g_scratch + kAttrs + 0, 0xFFFFFFFD);  // ObDosDevices
    wr32(g_scratch + kAttrs + 4, g_scratch + kAnsi);
    wr32(g_scratch + kAttrs + 8, 0x40);        // OBJ_CASE_INSENSITIVE
}

uint32_t open_file(PPCContext& ctx, const std::string& path, uint32_t access, uint32_t options = 0x20 | 0x40) {
    set_name(path);
    wr32(g_scratch + kHandle, 0);
    ctx.r3.u64 = g_scratch + kHandle;
    ctx.r4.u64 = access;
    ctx.r5.u64 = g_scratch + kAttrs;
    ctx.r6.u64 = g_scratch + kIosb;
    ctx.r7.u64 = 0;           // AllocationSize
    ctx.r8.u64 = 0x80;        // FILE_ATTRIBUTE_NORMAL
    ctx.r9.u64 = 1;           // FILE_SHARE_READ
    ctx.r10.u64 = 1;          // FILE_OPEN
    wr32(ctx.r1.u32 + 0x54, options);  // default FILE_SYNCHRONOUS_IO_NONALERT | NON_DIRECTORY
    __imp__NtCreateFile(ctx, g_base);
    return ctx.r3.u32;
}

uint32_t read_file(PPCContext& ctx, uint32_t h, uint32_t len, bool use_offset, uint64_t off) {
    guest_write_be64(g_scratch + kOffset, off);
    ctx.r3.u64 = h;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 0;
    ctx.r6.u64 = 0;
    ctx.r7.u64 = g_scratch + kIosb;
    ctx.r8.u64 = g_scratch + kBuf;
    ctx.r9.u64 = len;
    ctx.r10.u64 = use_offset ? g_scratch + kOffset : 0;
    __imp__NtReadFile(ctx, g_base);
    return ctx.r3.u32;
}

// Stand-in for recompiled guest code that calls ExTerminateThread.
void guest_exit_entry(PPCContext& ctx, uint8_t* base) {
    ctx.r3.u64 = 0x1234;
    __imp__ExTerminateThread(ctx, base);
    ctx.r3.u64 = 0xBAD;  // must not be reached
}

struct TestObj : HandleObject {
    static constexpr HandleKind kKind = HandleKind::Test;
    HandleKind kind() const override { return kKind; }
};
}  // namespace

int main() {
    GuestMemory mem;
    if (mem.reserve() != MemStatus::Ok) return 2;
    g_base = mem.base();
    CHECK_ST(runtime_init(&mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    Runtime& r = *runtime();

    // Host test tree.
    char tmpl[] = "/tmp/rcomp_rt_hle_XXXXXX";
    std::string root = mkdtemp(tmpl) ? tmpl : "";
    if (root.empty()) return 2;
    mkdir((root + "/data").c_str(), 0755);
    const std::string payload = "HELLO-XENON-0123456789abcdef";
    {
        FILE* f = fopen((root + "/data/x.bin").c_str(), "wb");
        CHECK(f && fwrite(payload.data(), 1, payload.size(), f) == payload.size());
        if (f) fclose(f);
    }
    CHECK_ST(r.vfs.mount("game", root), Status::Ok);

    PPCContext ctx;
    GuestThread th;
    CHECK_ST(create_guest_thread(r.heap, GuestThreadInit{0x10000, 0, 0}, &ctx, &th), Status::Ok);
    CHECK_ST(r.heap.alloc(0x1000, 16, true, &g_scratch), Status::Ok);
    const uint32_t r1 = ctx.r1.u32;

    // --- NtAllocateVirtualMemory / NtFreeVirtualMemory ------------------------
    wr32(g_scratch + kPBase, 0);
    wr32(g_scratch + kPSize, 100000);
    ctx.r3.u64 = g_scratch + kPBase;
    ctx.r4.u64 = g_scratch + kPSize;
    ctx.r5.u64 = 0x1000 | 0x2000;  // MEM_COMMIT | MEM_RESERVE
    ctx.r6.u64 = 0x04;             // PAGE_READWRITE
    ctx.r7.u64 = 0;
    __imp__NtAllocateVirtualMemory(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, nt::kSuccess);
    uint32_t va = rd32(g_scratch + kPBase);
    CHECK(va != 0 && va % 0x10000 == 0);
    CHECK_EQ(rd32(g_scratch + kPSize), 0x20000u);
    // 4 KiB-page allocations live below 0x40000000 as on the console (Gears of War 2's allocator
    // depends on it); runtime-owned blocks and MEM_LARGE_PAGES allocations stay at or above it.
    CHECK(va >= GuestHeap::kSmallPageLo && va < GuestHeap::kLargePageLo);
    CHECK(g_scratch >= GuestHeap::kLargePageLo && th.alloc_base >= GuestHeap::kLargePageLo);
    {
        wr32(g_scratch + kPBase, 0);
        wr32(g_scratch + kPSize, 0x10000);
        ctx.r3.u64 = g_scratch + kPBase;
        ctx.r4.u64 = g_scratch + kPSize;
        ctx.r5.u64 = 0x20000000u | 0x1000 | 0x2000;  // MEM_LARGE_PAGES | MEM_COMMIT | MEM_RESERVE
        ctx.r6.u64 = 0x04;
        ctx.r7.u64 = 0;
        __imp__NtAllocateVirtualMemory(ctx, g_base);
        CHECK_EQ(ctx.r3.u32, nt::kSuccess);
        const uint32_t large = rd32(g_scratch + kPBase);
        CHECK(large >= GuestHeap::kLargePageLo && large < GuestHeap::kDefaultHi);
        wr32(g_scratch + kPSize, 0);
        ctx.r3.u64 = g_scratch + kPBase;
        ctx.r4.u64 = g_scratch + kPSize;
        ctx.r5.u64 = 0x8000;
        ctx.r6.u64 = 0;
        __imp__NtFreeVirtualMemory(ctx, g_base);
        CHECK_EQ(ctx.r3.u32, nt::kSuccess);
        wr32(g_scratch + kPBase, va);
    }
    uint8_t* vp = mem.translate(va, 0x20000);
    CHECK(vp != nullptr);
    bool zero = true;
    for (uint32_t i = 0; vp && i < 0x20000; ++i) zero &= vp[i] == 0;
    CHECK(zero);
    if (vp) vp[0x1FFFF] = 1;

    wr32(g_scratch + kPSize, 0);
    ctx.r3.u64 = g_scratch + kPBase;
    ctx.r4.u64 = g_scratch + kPSize;
    ctx.r5.u64 = 0x8000;  // MEM_RELEASE
    ctx.r6.u64 = 0;
    __imp__NtFreeVirtualMemory(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kPSize), 0x20000u);
    wr32(g_scratch + kPSize, 0);
    ctx.r3.u64 = g_scratch + kPBase;
    ctx.r4.u64 = g_scratch + kPSize;
    __imp__NtFreeVirtualMemory(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, nt::kMemoryNotAllocated);  // double free
    wr32(g_scratch + kPBase, th.alloc_base);        // runtime-owned stack block
    ctx.r3.u64 = g_scratch + kPBase;
    ctx.r4.u64 = g_scratch + kPSize;
    __imp__NtFreeVirtualMemory(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, nt::kMemoryNotAllocated);

    // Commit-only may target an existing guest reservation, but cannot invent
    // ownership for arbitrary raw committed memory used by this test harness.
    bool fatal = false;
    wr32(g_scratch + kPBase, 0x30000000);
    wr32(g_scratch + kPSize, 0x10000);
    ctx.r3.u64 = g_scratch + kPBase;
    ctx.r4.u64 = g_scratch + kPSize;
    ctx.r5.u64 = 0x1000;
    ctx.r6.u64 = 0x04;
    ctx.r7.u64 = 0;
    __imp__NtAllocateVirtualMemory(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, nt::kMemoryNotAllocated);
    CHECK_EQ(rd32(g_scratch + kPBase), 0x30000000u);
    CHECK_EQ(rd32(g_scratch + kPSize), 0x10000u);

    // --- NtCreateFile / NtReadFile / NtClose -----------------------------------
    // Simulate a caller frame: stack arguments live at r1 + 0x54 of the caller.
    ctx.r1.u64 = r1 - 0x100;
    CHECK_EQ(open_file(ctx, "game:\\data\\x.bin", 0x80100000 /*GENERIC_READ|SYNCHRONIZE*/),
             nt::kSuccess);
    uint32_t h = rd32(g_scratch + kHandle);
    CHECK(h != 0);
    CHECK_EQ(rd32(g_scratch + kIosb), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 1u);  // FILE_OPENED

    CHECK_EQ(read_file(ctx, h, 5, false, 0), nt::kSuccess);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 5u);
    CHECK(memcmp(g_base + g_scratch + kBuf, "HELLO", 5) == 0);
    {
        // Halo 3 opens its maps with FILE_RANDOM_ACCESS | NON_DIRECTORY | NO_INTERMEDIATE_BUFFERING (0x848):
        // the access-pattern hint is accepted and the file reads the same bytes.
        // NO_INTERMEDIATE_BUFFERING requires a sector-aligned offset and length, not an aligned buffer: Halo 3
        // reads each map's 0x3000-byte header at the current position into an unaligned buffer.
        CHECK_EQ(open_file(ctx, "game:\\data\\x.bin", 0x80100000, 0x848), nt::kSuccess);
        const uint32_t h848 = rd32(g_scratch + kHandle);
        ctx.r3.u64 = h848; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
        ctx.r7.u64 = g_scratch + kIosb; ctx.r8.u64 = g_scratch + kBuf + 1; ctx.r9.u64 = 0x200; ctx.r10.u64 = 0;
        __imp__NtReadFile(ctx, g_base);
        CHECK_EQ(ctx.r3.u32, nt::kSuccess);
        CHECK_EQ(rd32(g_scratch + kIosb + 4), (uint32_t)payload.size());
        CHECK(memcmp(g_base + g_scratch + kBuf + 1, "HELLO", 5) == 0);
        CHECK_EQ(read_file(ctx, h848, 5, true, 0), nt::kInvalidParameter);  // unaligned length
        CHECK_ST(r.handles.close(h848), Status::Ok);
        CHECK_EQ(open_file(ctx, "game:\\data\\x.bin", 0x80100000, 0x820 | 0x40), nt::kSuccess);
        const uint32_t h2 = rd32(g_scratch + kHandle);
        CHECK_EQ(read_file(ctx, h2, 5, true, 0), nt::kSuccess);
        CHECK(memcmp(g_base + g_scratch + kBuf, "HELLO", 5) == 0);
        CHECK_ST(r.handles.close(h2), Status::Ok);
    }
    CHECK_EQ(read_file(ctx, h, 6, true, 0xFFFFFFFFFFFFFFFEull), nt::kSuccess);  // current pos
    CHECK(memcmp(g_base + g_scratch + kBuf, "-XENON", 6) == 0);
    CHECK_EQ(read_file(ctx, h, 4, true, 12), nt::kSuccess);  // absolute
    CHECK(memcmp(g_base + g_scratch + kBuf, "0123", 4) == 0);
    CHECK_EQ(read_file(ctx, h, 100, false, 0), nt::kSuccess);  // short read to EOF
    CHECK_EQ(rd32(g_scratch + kIosb + 4), (uint32_t)payload.size() - 16);
    CHECK_EQ(read_file(ctx, h, 1, false, 0), nt::kEndOfFile);
    CHECK_EQ(rd32(g_scratch + kIosb), nt::kEndOfFile);
    CHECK_EQ(rd32(g_scratch + kIosb + 4), 0u);
    ctx.r8.u64 = 0x00001000;  // uncommitted buffer
    ctx.r3.u64 = h;
    ctx.r4.u64 = ctx.r5.u64 = ctx.r10.u64 = 0;
    ctx.r7.u64 = g_scratch + kIosb;
    ctx.r9.u64 = 4;
    __imp__NtReadFile(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, nt::kAccessViolation);

    // An APC routine that is not a function address (here 0 | the xapi low bit) traps;
    // completion APCs themselves are covered by test_file_apc.cpp.
    ctx.r3.u64 = h;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 1;
    CAPTURE_FATAL(__imp__NtReadFile(ctx, g_base), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    ctx.r5.u64 = 0;

    // Wrong handle kind.
    uint32_t th_handle = 0;
    CHECK_ST(r.handles.insert(std::make_shared<TestObj>(), &th_handle), Status::Ok);
    CHECK_EQ(read_file(ctx, th_handle, 4, false, 0), nt::kObjectTypeMismatch);

    ctx.r3.u64 = h;
    __imp__NtClose(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, nt::kSuccess);
    ctx.r3.u64 = h;
    __imp__NtClose(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, nt::kInvalidHandle);
    CHECK_EQ(read_file(ctx, h, 4, false, 0), nt::kInvalidHandle);

    // Path errors.
    CHECK_EQ(open_file(ctx, "game:\\..\\x.bin", 0x80000000), nt::kObjectNameInvalid);
    CHECK_EQ(open_file(ctx, "game:\\data\\nope.bin", 0x80000000), nt::kObjectNameNotFound);
    CHECK_EQ(open_file(ctx, "e:\\data\\x.bin", 0x80000000), nt::kObjectPathNotFound);
    CHECK_EQ(open_file(ctx, "game:\\data\\x.bin", 0x40000000 /*GENERIC_WRITE*/), nt::kAccessDenied);
    CHECK_EQ(rd32(g_scratch + kHandle), 0u);

    // NtOpenFile.
    set_name("game:\\data\\x.bin");
    ctx.r3.u64 = g_scratch + kHandle;
    ctx.r4.u64 = 0x80100000;
    ctx.r5.u64 = g_scratch + kAttrs;
    ctx.r6.u64 = g_scratch + kIosb;
    ctx.r7.u64 = 0x20;
    __imp__NtOpenFile(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, nt::kSuccess);
    ctx.r3.u64 = rd32(g_scratch + kHandle);
    __imp__NtClose(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, nt::kSuccess);

    // --- KeQuerySystemTime -----------------------------------------------------
    uint64_t before = system_filetime();
    ctx.r3.u64 = g_scratch + kTime;
    __imp__KeQuerySystemTime(ctx, g_base);
    uint64_t t = 0;
    guest_read_be64(g_scratch + kTime, &t);
    CHECK(t >= before && t - before < 10000000ull);  // within 1 s

    // --- ExTerminateThread ------------------------------------------------------
    ctx.r1.u64 = r1;
    uint32_t code = 0;
    CHECK_ST(run_guest_thread(th, ctx, g_base, guest_exit_entry, &code), Status::Ok);
    CHECK_EQ(code, 0x1234u);
    CHECK_EQ(ctx.r3.u32, 0x1234u);  // the statement after the call never ran

    // A formerly missing import is now real. Temporarily unbind it in this
    // test to keep verifying the generated thunk's missing-import diagnostic.
    PPCFunc* const TESTDOUBLE_saved_query = find_import(kModuleXboxkrnl, 0x00EE);
    CHECK(TESTDOUBLE_saved_query != nullptr);
    CHECK_ST(unregister_import(kModuleXboxkrnl, 0x00EE), Status::Ok);
    CAPTURE_FATAL(__imp__NtQueryVirtualMemory(ctx, g_base), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_MISSING_IMPORT);
    CHECK(g_fatal_msg.find("ordinal=0x00EE name=NtQueryVirtualMemory") != std::string::npos);
    CHECK_ST(register_import(kModuleXboxkrnl, 0x00EE, TESTDOUBLE_saved_query,
                             "NtQueryVirtualMemory"), Status::Ok);

    CHECK_ST(destroy_guest_thread(r.heap, &th), Status::Ok);
    unlink((root + "/data/x.bin").c_str());
    rmdir((root + "/data").c_str());
    rmdir(root.c_str());
    runtime_shutdown();
    return test_result("rt_test_hle");
}
