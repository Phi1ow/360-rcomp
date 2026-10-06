// I/O completion APCs of file transfers (ReadFileEx / WriteFileEx): NtReadFile and
// NtWriteFile with an ApcRoutine queue Routine(ApcContext, IoStatusBlock, 0) to the
// calling guest thread, run by its next user-mode alertable wait. TESTDOUBLE entries
// stand in only for AOT guest routines.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__NtCreateFile);
PPC_EXTERN_FUNC(__imp__NtReadFile);
PPC_EXTERN_FUNC(__imp__NtWriteFile);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__KeDelayExecutionThread);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory mem;
uint32_t scratch;
constexpr uint32_t kHandle = 0x00, kIosb = 0x08, kOffset = 0x10, kDelay = 0x18, kZero = 0x20, kOut = 0x28,
                   kAttrs = 0x30, kAnsi = 0x40, kName = 0x50, kBuf = 0x200;
constexpr uint32_t kUserApc = 0xC0, kTimedOut = 0x102, kEndOfFile = 0xC0000011;
// xapi's completion thunk is 4-byte aligned and passed with the low bit set (Halo 3: 0x8259F900 | 1).
constexpr uint32_t kApcRoutine = 0x82000400, kMissingRoutine = 0x82000600;
constexpr uint32_t kGenericRead = 0x80000000u, kGenericWrite = 0x40000000u, kSynchronize = 0x00100000u;

struct ApcCall { uint32_t context, iosb, reserved, status, information; };
int g_calls = 0;
ApcCall g_last{};

void TESTDOUBLE_apc(PPCContext& c, uint8_t*) {
    uint32_t status = 0, information = 0;
    CHECK(guest_read_be32(c.r4.u32, &status));
    CHECK(guest_read_be32(c.r4.u32 + 4, &information));
    g_last = {c.r3.u32, c.r4.u32, c.r5.u32, status, information};
    ++g_calls;
}

uint32_t read32(uint32_t address) { uint32_t v = 0; CHECK(guest_read_be32(address, &v)); return v; }

uint32_t open_file(PPCContext& c, const std::string& path, uint32_t access, uint32_t disposition) {
    std::memcpy(mem.base() + scratch + kName, path.data(), path.size());
    const uint16_t len = uint16_t(path.size());
    CHECK(guest_write_be32(scratch + kAnsi, uint32_t(len) << 16 | len));  // Length, MaximumLength
    CHECK(guest_write_be32(scratch + kAnsi + 4, scratch + kName));
    CHECK(guest_write_be32(scratch + kAttrs, 0xFFFFFFFD));  // ObDosDevices
    CHECK(guest_write_be32(scratch + kAttrs + 4, scratch + kAnsi));
    CHECK(guest_write_be32(scratch + kAttrs + 8, 0x40));
    c.r3.u64 = scratch + kHandle; c.r4.u64 = access; c.r5.u64 = scratch + kAttrs; c.r6.u64 = scratch + kIosb;
    c.r7.u64 = 0; c.r8.u64 = 0x80; c.r9.u64 = 1; c.r10.u64 = disposition;
    CHECK(guest_write_be32(c.r1.u32 + 0x54, 0x40));  // FILE_NON_DIRECTORY_FILE, overlapped (no SYNCHRONOUS_IO)
    __imp__NtCreateFile(c, mem.base());
    CHECK_EQ(c.r3.u32, 0u);
    return read32(scratch + kHandle);
}

uint32_t transfer(PPCFunc* fn, PPCContext& c, uint32_t handle, uint32_t apc, uint32_t context, uint32_t length,
                  uint64_t offset) {
    CHECK(guest_write_be64(scratch + kOffset, offset));
    c.r3.u64 = handle; c.r4.u64 = 0; c.r5.u64 = apc; c.r6.u64 = context; c.r7.u64 = scratch + kIosb;
    c.r8.u64 = scratch + kBuf; c.r9.u64 = length; c.r10.u64 = scratch + kOffset;
    fn(c, mem.base());
    return c.r3.u32;
}

uint32_t alertable_sleep(PPCContext& c) {
    CHECK(guest_write_be64(scratch + kDelay, uint64_t(-int64_t(1'000'000))));  // 100 ms
    c.r3.u64 = 1; c.r4.u64 = 1; c.r5.u64 = scratch + kDelay;
    __imp__KeDelayExecutionThread(c, mem.base());
    return c.r3.u32;
}

void TESTDOUBLE_main(PPCContext& guest, uint8_t*) {
    PPCContext c = guest;
    c.r1.u64 = c.r1.u32 - 0x100;  // a caller frame: NtCreateFile reads its 9th argument at r1+0x54
    const uint32_t file = open_file(c, "save:\\data\\x.bin", kGenericRead | kGenericWrite | kSynchronize, 1);

    // ReadFileEx: the transfer completes, the APC waits for an alertable wait of this thread.
    CHECK_EQ(transfer(__imp__NtReadFile, c, file, kApcRoutine | 1, 0x1234, 5, 0), 0u);
    CHECK(std::memcmp(mem.base() + scratch + kBuf, "HELLO", 5) == 0);
    CHECK_EQ(g_calls, 0);
    CHECK(guest_write_be64(scratch + kZero, 0));
    {   // a non-alertable wait does not deliver it

        PPCContext e = c;
        e.r3.u64 = scratch + kOut; e.r4.u64 = 0; e.r5.u64 = 1; e.r6.u64 = 0;
        __imp__NtCreateEvent(e, mem.base());
        CHECK_EQ(e.r3.u32, 0u);
        const uint32_t idle = read32(scratch + kOut);
        e.r3.u64 = idle; e.r4.u64 = 1; e.r5.u64 = 0; e.r6.u64 = scratch + kZero;
        __imp__NtWaitForSingleObjectEx(e, mem.base());
        CHECK_EQ(e.r3.u32, kTimedOut);
        CHECK_EQ(g_calls, 0);
        e.r3.u64 = idle;
        __imp__NtClose(e, mem.base());
    }
    CHECK_EQ(alertable_sleep(c), kUserApc);
    CHECK_EQ(g_calls, 1);
    CHECK_EQ(g_last.context, 0x1234u);
    CHECK_EQ(g_last.iosb, scratch + kIosb);
    CHECK_EQ(g_last.reserved, 0u);
    CHECK_EQ(g_last.status, 0u);
    CHECK_EQ(g_last.information, 5u);
    CHECK_EQ(alertable_sleep(c), 0u);  // delivered once
    CHECK_EQ(g_calls, 1);

    // WriteFileEx.
    std::memcpy(mem.base() + scratch + kBuf, "WORLD", 5);
    CHECK_EQ(transfer(__imp__NtWriteFile, c, file, kApcRoutine | 1, 0x5678, 5, 0), 0u);
    CHECK_EQ(alertable_sleep(c), kUserApc);
    CHECK_EQ(g_calls, 2);
    CHECK_EQ(g_last.context, 0x5678u);
    CHECK_EQ(g_last.information, 5u);

    // A failed request (read at the end of the file) returns its error and queues nothing.
    CHECK_EQ(transfer(__imp__NtReadFile, c, file, kApcRoutine | 1, 0x9ABC, 5, 0x1000), kEndOfFile);
    CHECK_EQ(alertable_sleep(c), 0u);
    CHECK_EQ(g_calls, 2);

    // A routine without a recompiled function traps before any transfer.
    bool fatal = false;
    CAPTURE_FATAL(transfer(__imp__NtReadFile, c, file, kMissingRoutine | 1, 0, 5, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_INDIRECT_TARGET);

    c.r3.u64 = file;
    __imp__NtClose(c, mem.base());
    CHECK_EQ(c.r3.u32, 0u);
    guest.r3.u64 = 0;
}
}  // namespace

int main() {
    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK_ST(runtime_init(&mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    char tmpl[] = "/tmp/rcomp_rt_file_apc_XXXXXX";
    const std::string root = mkdtemp(tmpl) ? tmpl : "";
    CHECK(!root.empty());
    mkdir((root + "/data").c_str(), 0755);
    {
        FILE* f = std::fopen((root + "/data/x.bin").c_str(), "wb");
        CHECK(f && std::fwrite("HELLO-XENON", 1, 11, f) == 11);
        if (f) std::fclose(f);
    }
    CHECK_ST(runtime()->vfs.mount("save", root, MountAccess::ReadWrite), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000, 64, true, &scratch), Status::Ok);
    const FuncEntry functions[] = {{kApcRoutine, TESTDOUBLE_apc, "TESTDOUBLE_apc"}};
    CHECK(register_functions(functions, 1));
    {
        GuestThread t; PPCContext c{}; uint32_t code = ~0u;
        CHECK_ST(create_guest_thread(runtime()->heap, {0x10000, 0, 0}, &c, &t), Status::Ok);
        CHECK_ST(run_guest_thread(t, c, mem.base(), TESTDOUBLE_main, &code), Status::Ok);
        CHECK_EQ(code, 0u);
        CHECK_ST(destroy_guest_thread(runtime()->heap, &t), Status::Ok);
    }
    CHECK_ST(runtime()->heap.free(scratch), Status::Ok);
    unlink((root + "/data/x.bin").c_str());
    rmdir((root + "/data").c_str());
    rmdir(root.c_str());
    runtime_shutdown();
    clear_imports();
    clear_functions();
    mem.release();
    return test_result("rt_file_apc");
}
