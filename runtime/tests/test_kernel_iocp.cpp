// File I/O completion ports: NtSetInformationFile FileCompletionInformation
// (class 30) and the packets NtReadFile / NtWriteFile / NtReadFileScatter /
// NtWriteFileGather queue to an associated port (NT rules), read back with
// NtRemoveIoCompletion as Halo 3's GetQueuedCompletionStatus does.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__NtOpenFile);
PPC_EXTERN_FUNC(__imp__NtReadFile);
PPC_EXTERN_FUNC(__imp__NtWriteFile);
PPC_EXTERN_FUNC(__imp__NtReadFileScatter);
PPC_EXTERN_FUNC(__imp__NtWriteFileGather);
PPC_EXTERN_FUNC(__imp__NtSetInformationFile);
PPC_EXTERN_FUNC(__imp__NtQueryInformationFile);
PPC_EXTERN_FUNC(__imp__NtCreateIoCompletion);
PPC_EXTERN_FUNC(__imp__NtRemoveIoCompletion);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__NtDuplicateObject);
PPC_EXTERN_FUNC(__imp__NtClose);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory mem;
uint32_t scratch;  // 0x4000 bytes
enum : uint32_t {
    kOut = 0x00, kIosb = 0x10, kIosb2 = 0x18, kKey = 0x20, kApc = 0x24, kZero = 0x28, kOffset = 0x30,
    kInfo = 0x40, kAttrs = 0x50, kName = 0x60, kText = 0x80, kSegments = 0x100, kBuffer = 0x200, kPage = 0x1000,
};
constexpr uint32_t kInvalidInfoClass = 0xC0000003u, kInfoLengthMismatch = 0xC0000004u,
                   kTypeMismatch = 0xC0000024u, kInvalidParameter = 0xC000000Du, kInvalidHandle = 0xC0000008u,
                   kEndOfFile = 0xC0000011u, kTimedOut = 0x102u;
constexpr uint32_t kFileCompletionInformation = 30;

uint32_t read32(uint32_t address) { uint32_t value = 0; CHECK(guest_read_be32(address, &value)); return value; }
uint32_t call(PPCFunc* fn, uint32_t a = 0, uint32_t b = 0, uint32_t c3 = 0, uint32_t d = 0, uint32_t e = 0,
              uint32_t f = 0, uint32_t g = 0, uint32_t h = 0) {
    alignas(64) PPCContext c{};
    c.r3.u64 = a; c.r4.u64 = b; c.r5.u64 = c3; c.r6.u64 = d; c.r7.u64 = e; c.r8.u64 = f; c.r9.u64 = g; c.r10.u64 = h;
    c.lr = 0x8259FAE0;
    fn(c, mem.base());
    return c.r3.u32;
}
uint32_t open(const char* path, uint32_t options) {
    const uint32_t length = uint32_t(strlen(path));
    memcpy(mem.base() + scratch + kText, path, length);
    CHECK(guest_write_be32(scratch + kName, (length << 16) | length));
    CHECK(guest_write_be32(scratch + kName + 4, scratch + kText));
    CHECK(guest_write_be32(scratch + kAttrs, 0));
    CHECK(guest_write_be32(scratch + kAttrs + 4, scratch + kName));
    CHECK(guest_write_be32(scratch + kAttrs + 8, 0));
    CHECK_EQ(call(__imp__NtOpenFile, scratch + kOut, 0xC0000000u, scratch + kAttrs, scratch + kIosb, options), 0u);
    return read32(scratch + kOut);
}
// NtSetInformationFile(File, IOSB, {Port, Key}, Length, FileCompletionInformation), as Halo 3 builds it.
uint32_t associate(uint32_t file, uint32_t port, uint32_t key, uint32_t length = 8) {
    CHECK(guest_write_be32(scratch + kInfo, port));
    CHECK(guest_write_be32(scratch + kInfo + 4, key));
    CHECK(guest_write_be32(scratch + kIosb, 0xCCCCCCCC));
    return call(__imp__NtSetInformationFile, file, scratch + kIosb, scratch + kInfo, length, kFileCompletionInformation);
}
uint32_t read_file(uint32_t file, uint32_t context, uint32_t bytes, uint64_t at, uint32_t event = 0, uint32_t apc = 0) {
    CHECK(guest_write_be64(scratch + kOffset, at));
    return call(__imp__NtReadFile, file, event, apc, context, scratch + kIosb, scratch + kBuffer, bytes, scratch + kOffset);
}
// GetQueuedCompletionStatus with a zero timeout: status, and the packet in *key/*context/kIosb2.
uint32_t remove(uint32_t port) {
    CHECK(guest_write_be32(scratch + kKey, 0xEEEEEEEE));
    return call(__imp__NtRemoveIoCompletion, port, scratch + kKey, scratch + kApc, scratch + kIosb2, scratch + kZero);
}
void expect_packet(uint32_t port, uint32_t key, uint32_t context, uint32_t status, uint32_t information) {
    CHECK_EQ(remove(port), 0u);
    CHECK_EQ(read32(scratch + kKey), key);
    CHECK_EQ(read32(scratch + kApc), context);
    CHECK_EQ(read32(scratch + kIosb2), status);
    CHECK_EQ(read32(scratch + kIosb2 + 4), information);
}
}  // namespace

int main() {
    char tmpl[] = "/tmp/rcomp_rt_kiocp_XXXXXX";
    std::string top = mkdtemp(tmpl) ? tmpl : "";
    if (top.empty()) return 2;
    const std::string save = top + "/save";
    CHECK(mkdir(save.c_str(), 0755) == 0);
    if (FILE* f = fopen((save + "/map.bin").c_str(), "wb")) {
        for (int i = 0; i < 0x1800; ++i) fputc(i & 0xFF, f);
        fclose(f);
    }

    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK_ST(runtime_init(&mem), Status::Ok);
    Runtime& r = *runtime();
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(r.heap.alloc(0x4000, 0x1000, true, &scratch), Status::Ok);
    CHECK(guest_write_be64(scratch + kZero, 0));
    CHECK_ST(r.vfs.mount("save", save, MountAccess::ReadWrite), Status::Ok);

    // Overlapped (no FILE_SYNCHRONOUS_IO_*) and synchronous handles.
    const uint32_t file = open("save:\\map.bin", 0x40);
    const uint32_t other = open("save:\\map.bin", 0x40);
    const uint32_t sync = open("save:\\map.bin", 0x60);
    CHECK_EQ(call(__imp__NtCreateIoCompletion, scratch + kOut, 0x1F0003, 0, 0), 0u);
    const uint32_t port = read32(scratch + kOut);
    CHECK_EQ(call(__imp__NtCreateIoCompletion, scratch + kOut, 0x1F0003, 0, 0), 0u);
    const uint32_t port2 = read32(scratch + kOut);
    CHECK_EQ(call(__imp__NtCreateEvent, scratch + kOut, 0, 0, 0), 0u);
    const uint32_t event = read32(scratch + kOut);

    // ---- association --------------------------------------------------------
    CHECK_EQ(associate(file, port, 0x1234), 0u);
    CHECK_EQ(read32(scratch + kIosb), 0u);
    CHECK_EQ(read32(scratch + kIosb + 4), 0u);
    CHECK_EQ(associate(file, port2, 0x5678), kInvalidParameter);  // a file object has one port, for good
    CHECK_EQ(associate(file, port, 0x1234), kInvalidParameter);
    CHECK_EQ(associate(sync, port, 1), kInvalidParameter);        // synchronous-I/O file object
    CHECK_EQ(associate(other, event, 1), kTypeMismatch);
    CHECK_EQ(associate(other, 0xF0000F00u, 1), kInvalidHandle);
    CHECK_EQ(associate(other, port, 1, 4), kInfoLengthMismatch);
    CHECK_EQ(call(__imp__NtQueryInformationFile, file, scratch + kIosb, scratch + kInfo, 8, kFileCompletionInformation),
             kInvalidInfoClass);  // set-only class

    // ---- packets ----------------------------------------------------------------
    CHECK_EQ(remove(port), kTimedOut);
    CHECK_EQ(read32(scratch + kKey), 0xEEEEEEEEu);
    // Read with an ApcContext (the OVERLAPPED): one packet with key, context, status, bytes.
    CHECK_EQ(read_file(file, 0x700DF00D, 0x10, 0x20), 0u);
    CHECK_EQ(mem.base()[scratch + kBuffer], 0x20u);
    expect_packet(port, 0x1234, 0x700DF00D, 0, 0x10);
    CHECK_EQ(remove(port), kTimedOut);
    // No ApcContext (an OVERLAPPED whose hEvent has its low bit set): no packet.
    CHECK_EQ(read_file(file, 0, 4, 0), 0u);
    CHECK_EQ(remove(port), kTimedOut);
    // A request that fails queues nothing.
    CHECK_EQ(read_file(file, 0x11, 4, 0x10000), kEndOfFile);
    CHECK_EQ(remove(port), kTimedOut);
    // Short read at the end: the real byte count.
    CHECK_EQ(read_file(file, 0x12, 0x100, 0x1800 - 8), 0u);
    expect_packet(port, 0x1234, 0x12, 0, 8);
    // Event and port both complete; packets are FIFO and the port is waitable.
    CHECK_EQ(read_file(file, 0x21, 1, 0, event), 0u);
    CHECK_EQ(read_file(file, 0x22, 2, 0), 0u);
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx, event, 1, 0, scratch + kZero), 0u);
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx, port, 1, 0, scratch + kZero), 0u);
    expect_packet(port, 0x1234, 0x21, 0, 1);
    expect_packet(port, 0x1234, 0x22, 0, 2);
    // Write.
    memset(mem.base() + scratch + kBuffer, 0xAB, 3);
    CHECK(guest_write_be64(scratch + kOffset, 0x100));
    CHECK_EQ(call(__imp__NtWriteFile, file, 0, 0, 0x31, scratch + kIosb, scratch + kBuffer, 3, scratch + kOffset), 0u);
    expect_packet(port, 0x1234, 0x31, 0, 3);
    // The association belongs to the file object: a duplicate handle completes to it too.
    CHECK_EQ(call(__imp__NtDuplicateObject, file, scratch + kOut, 0), 0u);
    const uint32_t duplicate = read32(scratch + kOut);
    CHECK_EQ(read_file(duplicate, 0x41, 4, 0), 0u);
    expect_packet(port, 0x1234, 0x41, 0, 4);
    // Another file object on the same file is not associated.
    CHECK_EQ(read_file(other, 0x51, 4, 0), 0u);
    CHECK_EQ(read_file(sync, 0x52, 4, 0), 0u);
    CHECK_EQ(remove(port), kTimedOut);
    // Scatter / gather.
    CHECK(guest_write_be32(scratch + kSegments, scratch + kPage));
    CHECK(guest_write_be32(scratch + kSegments + 4, scratch + kPage + 0x1000));
    CHECK(guest_write_be64(scratch + kOffset, 0));
    CHECK_EQ(call(__imp__NtReadFileScatter, file, 0, 0, 0x61, scratch + kIosb, scratch + kSegments, 0x1100,
                  scratch + kOffset), 0u);
    expect_packet(port, 0x1234, 0x61, 0, 0x1100);
    CHECK_EQ(call(__imp__NtWriteFileGather, file, 0, 0, 0x62, scratch + kIosb, scratch + kSegments, 0x1000,
                  scratch + kOffset), 0u);
    expect_packet(port, 0x1234, 0x62, 0, 0x1000);
    // An APC routine on a port-associated file is refused (NT).
    CHECK_EQ(read_file(file, 0x71, 4, 0, 0, 0x82000000u), kInvalidParameter);
    CHECK_EQ(remove(port), kTimedOut);

    // The port outlives its handle while a file object references it.
    CHECK_EQ(call(__imp__NtClose, port), 0u);
    CHECK_EQ(read_file(file, 0x81, 4, 0), 0u);

    for (uint32_t h : {file, duplicate, other, sync, port2, event}) CHECK_EQ(call(__imp__NtClose, h), 0u);
    CHECK_ST(r.heap.free(scratch), Status::Ok);
    runtime_shutdown();
    clear_imports();
    mem.release();
    unlink((save + "/map.bin").c_str());
    rmdir(save.c_str());
    rmdir(top.c_str());
    return test_result("rt_kernel_iocp");
}
