// Real synchronous NtReadFile completion: event, data, IOSB and file position.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/io_event.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__NtReadFile);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtSetEvent);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__NtCreateFile);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
constexpr uint32_t scratch = 0x30000000, event_cell = scratch + 0x20;
constexpr uint32_t iosb = scratch + 0x80, offset = scratch + 0x90;
constexpr uint32_t timeout = scratch + 0xA0, buffer = scratch + 0x10000;
constexpr uint32_t protected_page = scratch + 0x20000;
constexpr uint32_t sentinel = 0xA5A5A5A5;
GuestMemory memory;

uint32_t read32(uint32_t address) {
    uint32_t value = 0;
    CHECK(guest_read_be32(address, &value));
    return value;
}
uint32_t read_file(uint32_t file, uint32_t event, uint32_t output = iosb,
                   uint32_t data = buffer, uint32_t size = 2,
                   uint32_t at = 0) {
    alignas(64) PPCContext context{};
    context.r3.u64 = file; context.r4.u64 = event;
    context.r7.u64 = output; context.r8.u64 = data;
    context.r9.u64 = size; context.r10.u64 = at;
    __imp__NtReadFile(context, memory.base());
    return context.r3.u32;
}
uint32_t poll(uint32_t event) {
    alignas(64) PPCContext context{};
    context.r3.u64 = event; context.r6.u64 = timeout;
    __imp__NtWaitForSingleObjectEx(context, memory.base());
    return context.r3.u32;
}
void signal(uint32_t event) {
    alignas(64) PPCContext context{};
    context.r3.u64 = event;
    __imp__NtSetEvent(context, memory.base());
    CHECK_EQ(context.r3.u32, nt::kSuccess);
}
void sentinel_iosb(uint32_t address = iosb) {
    CHECK(guest_write_be32(address, sentinel));
    CHECK(guest_write_be32(address + 4, sentinel));
}
void expect_iosb(uint32_t status, uint32_t bytes) {
    CHECK_EQ(read32(iosb), status);
    CHECK_EQ(read32(iosb + 4), bytes);
}
}

int main() {
    CHECK(memory.reserve() == MemStatus::Ok);
    CHECK(memory.commit(scratch, 0x30000, Protect::ReadWrite) == MemStatus::Ok);
    CHECK_ST(runtime_init(&memory), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK(guest_write_be64(timeout, 0));

    char directory_template[] = "/tmp/rcomp_io_completion_XXXXXX";
    const char* created = mkdtemp(directory_template);
    CHECK(created != nullptr);
    if (!created) return 2;
    const std::string directory(created), path = directory + "/data.bin";
    const unsigned char original[] = {1, 2, 3, 4};
    FILE* source = std::fopen(path.c_str(), "wb");
    CHECK(source != nullptr);
    if (!source) return 2;
    CHECK_EQ(std::fwrite(original, 1, sizeof original, source), sizeof original);
    CHECK_EQ(std::fclose(source), 0);
    auto& owner = *runtime();
    CHECK_ST(owner.vfs.mount("game:", directory), Status::Ok);
    uint32_t file_handle = 0;
    CHECK_ST(owner.vfs.open(owner.handles, "game:\\data.bin", false, &file_handle), Status::Ok);
    std::shared_ptr<GuestFile> file;
    CHECK_ST(owner.handles.lookup_as<GuestFile>(file_handle, &file), Status::Ok);

    alignas(64) PPCContext context{};
    context.r3.u64 = event_cell; context.r5.u64 = 1; // auto-reset, initially clear
    __imp__NtCreateEvent(context, memory.base());
    CHECK_EQ(context.r3.u32, nt::kSuccess);
    const uint32_t event = read32(event_cell);
    CHECK_EQ(poll(event), nt::kTimeout);

    // Completion publishes both bytes and status before the event is consumed.
    sentinel_iosb();
    CHECK_EQ(read_file(file_handle, event), nt::kSuccess);
    expect_iosb(nt::kSuccess, 2);
    CHECK_EQ(memory.base()[buffer], 1u); CHECK_EQ(memory.base()[buffer + 1], 2u);
    CHECK_EQ(file->position(), 2u);
    CHECK_EQ(poll(event), nt::kSuccess); CHECK_EQ(poll(event), nt::kTimeout);
    CHECK_EQ(read_file(file_handle, event, iosb, buffer, 8), nt::kSuccess);
    expect_iosb(nt::kSuccess, 2);
    CHECK_EQ(memory.base()[buffer], 3u); CHECK_EQ(memory.base()[buffer + 1], 4u);
    CHECK_EQ(poll(event), nt::kSuccess);
    CHECK_EQ(read_file(file_handle, event), nt::kEndOfFile);
    expect_iosb(nt::kEndOfFile, 0);
    CHECK_EQ(poll(event), nt::kSuccess); CHECK_EQ(poll(event), nt::kTimeout);

    // Explicit offsets and synchronous completion without an event still work.
    CHECK(guest_write_be64(offset, 1));
    CHECK_EQ(read_file(file_handle, 0, iosb, buffer, 1, offset), nt::kSuccess);
    expect_iosb(nt::kSuccess, 1); CHECK_EQ(memory.base()[buffer], 2u);
    CHECK_EQ(file->position(), 2u);
    CHECK_EQ(read_file(file_handle, event, 0, 0, 0), nt::kSuccess);
    CHECK_EQ(file->position(), 2u); CHECK_EQ(poll(event), nt::kSuccess);

    // A protection fault cannot consume/reset an existing signal or advance I/O.
    for (Protect protection : {Protect::Read, Protect::None}) {
        sentinel_iosb(); signal(event);
        CHECK(memory.protect(buffer, 0x10000, protection) == MemStatus::Ok);
        CHECK_EQ(read_file(file_handle, event), nt::kAccessViolation);
        expect_iosb(sentinel, sentinel); CHECK_EQ(file->position(), 2u);
        CHECK_EQ(poll(event), nt::kSuccess); CHECK_EQ(poll(event), nt::kTimeout);
        CHECK(memory.protect(buffer, 0x10000, Protect::ReadWrite) == MemStatus::Ok);
    }
    sentinel_iosb(protected_page); signal(event);
    CHECK(memory.protect(protected_page, 0x10000, Protect::Read) == MemStatus::Ok);
    CHECK_EQ(read_file(file_handle, event, protected_page), nt::kAccessViolation);
    CHECK_EQ(read32(protected_page), sentinel); CHECK_EQ(read32(protected_page + 4), sentinel);
    CHECK_EQ(file->position(), 2u); CHECK_EQ(poll(event), nt::kSuccess);
    CHECK(memory.protect(protected_page, 0x10000, Protect::None) == MemStatus::Ok);
    for (uint32_t input : {protected_page, protected_page - 4, 0xFFFFFFFCu}) {
        sentinel_iosb(); signal(event);
        CHECK_EQ(read_file(file_handle, event, iosb, buffer, 2, input), nt::kAccessViolation);
        expect_iosb(sentinel, sentinel); CHECK_EQ(file->position(), 2u);
        CHECK_EQ(poll(event), nt::kSuccess);
    }
    CHECK_EQ(read_file(file_handle, event, iosb, protected_page - 1, 2), nt::kAccessViolation);
    CHECK(memory.protect(protected_page, 0x10000, Protect::ReadWrite) == MemStatus::Ok);
    CHECK(guest_write_be64(offset, UINT64_MAX)); signal(event);
    CHECK_EQ(read_file(file_handle, event, iosb, buffer, 2, offset), nt::kInvalidParameter);
    CHECK_EQ(file->position(), 2u); CHECK_EQ(poll(event), nt::kSuccess);

    // Handle validation precedes reset/read, and wrong-kind handles are rejected.
    CHECK_EQ(read_file(file_handle, file_handle), nt::kObjectTypeMismatch);
    CHECK_EQ(file->position(), 2u);
    std::shared_ptr<HandleObject> retained;
    CHECK_ST(reference_io_event(event, &retained), Status::Ok);
    context = {}; context.r3.u64 = event;
    __imp__NtClose(context, memory.base()); CHECK_EQ(context.r3.u32, nt::kSuccess);
    CHECK_EQ(read_file(file_handle, event), nt::kInvalidHandle);
    CHECK_EQ(file->position(), 2u);
    set_io_event(retained, false); set_io_event(retained, true); // remains a live object
    retained.reset();

    // Stack argument arithmetic cannot wrap into a valid, unrelated guest page.
    context = {}; context.r1.u64 = 0xFFFFFFF0u;
    bool fatal = false;
    CAPTURE_FATAL(__imp__NtCreateFile(context, memory.base()), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);

    CHECK_ST(owner.handles.close(file_handle), Status::Ok);
    CHECK_EQ(owner.handles.live_count(), 0u);
    file.reset();
    source = std::fopen(path.c_str(), "rb");
    unsigned char observed[5]{};
    CHECK(source != nullptr);
    if (source) {
        CHECK_EQ(std::fread(observed, 1, sizeof observed, source), sizeof original);
        CHECK(std::memcmp(observed, original, sizeof original) == 0);
        CHECK_EQ(std::fclose(source), 0);
    }
    runtime_shutdown(); clear_imports(); memory.release();
    CHECK_EQ(unlink(path.c_str()), 0); CHECK_EQ(rmdir(directory.c_str()), 0);
    return test_result("rt_file_completion");
}
