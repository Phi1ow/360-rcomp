// Kernel stacks, MmIsAddressValid, MmMapIoSpace, KeSetCurrentStackPointers,
// KeQueryBasePriorityThread, KeSetCurrentProcessType, VdSetDisplayModeOverride,
// ObLookupAnyThreadByThreadId, ObOpenObjectByPointer, ExEventObjectType,
// NtWriteFileGather and the explicit diagnostics of the same batch.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/kernel_variables.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/virtual_fields.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__MmCreateKernelStack);
PPC_EXTERN_FUNC(__imp__MmDeleteKernelStack);
PPC_EXTERN_FUNC(__imp__MmIsAddressValid);
PPC_EXTERN_FUNC(__imp__MmMapIoSpace);
PPC_EXTERN_FUNC(__imp__MmLockAndMapSegmentArray);
PPC_EXTERN_FUNC(__imp__MmUnlockAndUnmapSegmentArray);
PPC_EXTERN_FUNC(__imp__KeSetCurrentStackPointers);
PPC_EXTERN_FUNC(__imp__KeQueryBasePriorityThread);
PPC_EXTERN_FUNC(__imp__KeSetBasePriorityThread);
PPC_EXTERN_FUNC(__imp__KeSetCurrentProcessType);
PPC_EXTERN_FUNC(__imp__KeGetCurrentProcessType);
PPC_EXTERN_FUNC(__imp__VdSetDisplayModeOverride);
PPC_EXTERN_FUNC(__imp__IoInvalidDeviceRequest);
PPC_EXTERN_FUNC(__imp__ObLookupAnyThreadByThreadId);
PPC_EXTERN_FUNC(__imp__ObLookupThreadByThreadId);
PPC_EXTERN_FUNC(__imp__ObOpenObjectByPointer);
PPC_EXTERN_FUNC(__imp__ObReferenceObjectByHandle);
PPC_EXTERN_FUNC(__imp__ObDereferenceObject);
PPC_EXTERN_FUNC(__imp__ExCreateThread);
PPC_EXTERN_FUNC(__imp__NtResumeThread);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__NtWriteFileGather);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory mem;
uint32_t scratch;  // 0x4000 bytes
constexpr uint32_t kOut = 0x00, kOut2 = 0x04, kIosb = 0x10, kOffset = 0x20, kSegments = 0x40,
                   kPages = 0x1000;  // three 4 KiB pages at +0x1000
constexpr uint32_t kWorkerEntry = 0x82000100;
constexpr uint32_t kInvalidParameter = 0xC000000Du, kAccessViolation = 0xC0000005u, kTypeMismatch = 0xC0000024u,
                   kAccessDenied = 0xC0000022u;

uint32_t read(uint32_t address) { uint32_t value = 0; CHECK(guest_read_be32(address, &value)); return value; }
uint32_t call(PPCFunc* fn, PPCContext& c, uint32_t a = 0, uint32_t b = 0, uint32_t d = 0, uint32_t e = 0,
              uint32_t f = 0, uint32_t g = 0, uint32_t h = 0, uint32_t i = 0) {
    c.r3.u64 = a; c.r4.u64 = b; c.r5.u64 = d; c.r6.u64 = e; c.r7.u64 = f; c.r8.u64 = g; c.r9.u64 = h; c.r10.u64 = i;
    c.lr = 0x82000010;
    fn(c, mem.base());
    return c.r3.u32;
}
void TESTDOUBLE_worker(PPCContext& c, uint8_t*) { c.r3.u64 = 0x77; }

void TESTDOUBLE_main(PPCContext& guest, uint8_t*) {
    PPCContext c = guest;
    GuestThread* self = current_guest_thread();
    const uint32_t body = thread_object_body(self->identity);
    bool fatal = false;

    // ---- kernel stacks + KeSetCurrentStackPointers ------------------------
    const uint32_t base = call(__imp__MmCreateKernelStack, c, 0x3000, 0);
    CHECK(base && !(base & 0xFFF));
    const uint32_t limit = base - 0x3000;
    CHECK(mem.is_accessible(limit, 0x3000, Protect::ReadWrite));
    CHECK_EQ(mem.base()[limit + 0x10], 0u);  // zeroed
    // StackPointer == r1: the stack fields change and the continuation is the
    // caller's own return (fiber switches: test_fibers.cpp, docs/THREAD_OBJECTS.md).
    PPCContext s = c;
    s.r1.u64 = base - 0x100;
    call(__imp__KeSetCurrentStackPointers, s, base - 0x100, body, limit, base, limit);
    CHECK_EQ(s.r1.u32, base - 0x100);
    CHECK_EQ(read(self->pcr + kPcrStackBase), base);
    CHECK_EQ(read(self->pcr + kPcrStackEnd), limit);
    // Restore the thread's own stack the same way.
    s.r1.u64 = guest.r1.u32;
    call(__imp__KeSetCurrentStackPointers, s, guest.r1.u32, body, self->stack_base, self->stack_base, self->stack_limit);
    CHECK_EQ(s.r1.u32, guest.r1.u32);
    CHECK_EQ(read(self->pcr + kPcrStackBase), self->stack_base);
    // Another StackPointer whose continuation (LR 0x82000010) is neither a
    // suspended guest fiber nor a recompiled function entry.
    s = c;
    CAPTURE_FATAL(call(__imp__KeSetCurrentStackPointers, s, base - 0x100, body, limit, base, limit), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    s.r1.u64 = guest.r1.u32;
    call(__imp__KeSetCurrentStackPointers, s, guest.r1.u32, body, self->stack_base, self->stack_base, self->stack_limit);
    CHECK_EQ(read(self->pcr + kPcrStackEnd), self->stack_limit);
    s = c;
    CAPTURE_FATAL(call(__imp__KeSetCurrentStackPointers, s, base + 0x100, body, limit, base, limit), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);  // pointer outside the new stack
    CAPTURE_FATAL(call(__imp__KeSetCurrentStackPointers, s, base - 0x100, body + 0x1000, limit, base, limit), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);  // another thread
    PPCContext d = c;
    call(__imp__MmDeleteKernelStack, d, base, limit);
    CAPTURE_FATAL(call(__imp__MmDeleteKernelStack, d, base, limit), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);  // already deleted
    CAPTURE_FATAL(call(__imp__MmCreateKernelStack, d, 0x1000, 1), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);  // unknown type
    CAPTURE_FATAL(call(__imp__MmCreateKernelStack, d, 0x02000000u, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);  // not a stack size

    // ---- base priority ------------------------------------------------------
    call(__imp__KeSetBasePriorityThread, d, body, 3);
    CHECK_EQ(call(__imp__KeQueryBasePriorityThread, d, body), 3u);
    call(__imp__KeSetBasePriorityThread, d, body, uint32_t(-2));
    CHECK_EQ(call(__imp__KeQueryBasePriorityThread, d, body), uint32_t(-2));
    CAPTURE_FATAL(call(__imp__KeQueryBasePriorityThread, d, 0x12345678u), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);

    // ---- process type ---------------------------------------------------------
    call(__imp__KeSetCurrentProcessType, d, 1);
    CHECK_EQ(call(__imp__KeGetCurrentProcessType, d), 1u);
    CAPTURE_FATAL(call(__imp__KeSetCurrentProcessType, d, 2), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(call(__imp__KeSetCurrentProcessType, d, 9), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);

    // ---- thread lookup by id / handle from pointer --------------------------
    std::shared_ptr<ThreadObjectIdentity> identity;
    CHECK_ST(find_thread_object(body, &identity), Status::Ok);
    CHECK_EQ(call(__imp__ObLookupAnyThreadByThreadId, d, self->thread_id, scratch + kOut), 0u);
    CHECK_EQ(read(scratch + kOut), body);
    CHECK_EQ(thread_object_guest_reference_count(identity), 1u);
    call(__imp__ObDereferenceObject, d, body);
    CHECK_EQ(thread_object_guest_reference_count(identity), 0u);
    CHECK(guest_write_be32(scratch + kOut, 0xABCD));
    CHECK_EQ(call(__imp__ObLookupAnyThreadByThreadId, d, 0x7FFFFFF0u, scratch + kOut), kInvalidParameter);
    CHECK_EQ(read(scratch + kOut), 0xABCDu);
    // The title-process form finds the same (title) threads with the same reference contract.
    CHECK_EQ(call(__imp__ObLookupThreadByThreadId, d, self->thread_id, scratch + kOut), 0u);
    CHECK_EQ(read(scratch + kOut), body);
    CHECK_EQ(thread_object_guest_reference_count(identity), 1u);
    call(__imp__ObDereferenceObject, d, body);
    CHECK_EQ(call(__imp__ObLookupThreadByThreadId, d, 0x7FFFFFF0u, scratch + kOut), kInvalidParameter);
    CHECK_EQ(call(__imp__ObLookupAnyThreadByThreadId, d, self->thread_id, 0), kAccessViolation);
    CHECK_EQ(thread_object_guest_reference_count(identity), 0u);
    // The bootstrap thread has no handle object to open.
    CAPTURE_FATAL(call(__imp__ObOpenObjectByPointer, d, body, scratch + kOut), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(call(__imp__ObOpenObjectByPointer, d, 0x7100F000u, scratch + kOut), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    guest.r3.u64 = 0;
}
}  // namespace

int main() {
    char tmpl[] = "/tmp/rcomp_rt_kmore_XXXXXX";
    std::string top = mkdtemp(tmpl) ? tmpl : "";
    if (top.empty()) return 2;
    const std::string save = top + "/save";
    CHECK(mkdir(save.c_str(), 0755) == 0);

    CHECK(mem.reserve() == MemStatus::Ok);
    CHECK_ST(runtime_init(&mem), Status::Ok);
    Runtime& r = *runtime();
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(register_thread_object_type_variable(), Status::Ok);
    CHECK_ST(r.heap.alloc(0x4000, 0x1000, true, &scratch), Status::Ok);
    const FuncEntry functions[] = {{kWorkerEntry, TESTDOUBLE_worker, "TESTDOUBLE_worker"}};
    CHECK(register_functions(functions, 1));
    PPCContext c{};
    bool fatal = false;

    {
        GuestThread t; PPCContext g{}; uint32_t code = ~0u;
        CHECK_ST(create_guest_thread(r.heap, {0x10000, 0, 0}, &g, &t), Status::Ok);
        CHECK_ST(run_guest_thread(t, g, mem.base(), TESTDOUBLE_main, &code), Status::Ok);
        CHECK_EQ(code, 0u);
        CHECK_ST(destroy_guest_thread(r.heap, &t), Status::Ok);
    }

    // ---- MmIsAddressValid -------------------------------------------------------
    CHECK_EQ(call(__imp__MmIsAddressValid, c, scratch + 0x123), 1u);
    CHECK_EQ(call(__imp__MmIsAddressValid, c, 0x50000000u), 0u);
    CHECK_EQ(call(__imp__MmIsAddressValid, c, 0), 0u);
    CHECK_EQ(call(__imp__MmIsAddressValid, c, kExThreadObjectTypeVirtualAddress), 1u);  // registered type token
    CHECK_EQ(call(__imp__MmIsAddressValid, c, kExEventObjectTypeVirtualAddress + 4), 1u);
    CHECK_EQ(call(__imp__MmIsAddressValid, c, kXboxKrnlVersionVirtualAddress), 0u);     // not registered here
    CHECK_EQ(call(__imp__MmIsAddressValid, c, kThreadVirtualTokenBase + 0x00F00000u), 0u);  // no such thread

    // ---- MmMapIoSpace -------------------------------------------------------------
    uint32_t physical = 0;
    CHECK_ST(r.physical.alloc(0x10000, 0x10000, true, &physical), Status::Ok);
    CHECK_EQ(call(__imp__MmMapIoSpace, c, 2, physical + 0x40, 0x40, 0x404), physical + 0x40);
    CHECK_EQ(call(__imp__MmMapIoSpace, c, 2, physical - xenos::kXenosPhysicalWindow + 0x80, 0x40, 0x404), physical + 0x80);
    CAPTURE_FATAL(call(__imp__MmMapIoSpace, c, 2, 0x7FC80000u, 0x1000, 0x404), fatal);  // GPU registers
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(call(__imp__MmMapIoSpace, c, 2, physical + 0x10000 - 0x20, 0x40, 0x404), fatal);  // runs past
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_ST(r.physical.free(physical), Status::Ok);

    // ---- explicit diagnostics (no established contract / no IRP) -----------------
    CAPTURE_FATAL(call(__imp__MmLockAndMapSegmentArray, c, 1, 2, 3, 4), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(call(__imp__MmUnlockAndUnmapSegmentArray, c, 1, 2, 3, 4), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(call(__imp__IoInvalidDeviceRequest, c, 0x40001000u, 0x40002000u), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);

    // ---- VdSetDisplayModeOverride -------------------------------------------------
    c.f1.f64 = 0.0;
    CHECK_EQ(call(__imp__VdSetDisplayModeOverride, c, 0, 0, 0, 0, 0), 0u);
    c.f1.f64 = 50.0;
    CAPTURE_FATAL(call(__imp__VdSetDisplayModeOverride, c, 0, 0, 0, 0, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(g_fatal_msg.find("refresh=50.000") != std::string::npos);
    c.f1.f64 = 0.0;

    // ---- ObOpenObjectByPointer on a worker; ExEventObjectType -----------------------
    CHECK_EQ(call(__imp__ExCreateThread, c, scratch + kOut, 0x10000, 0, 0, kWorkerEntry, 0, 1), 0u);
    const uint32_t worker = read(scratch + kOut);
    CHECK_EQ(call(__imp__ObReferenceObjectByHandle, c, worker, 0, scratch + kOut2), 0u);
    const uint32_t worker_body = read(scratch + kOut2);
    CHECK_EQ(call(__imp__ObOpenObjectByPointer, c, worker_body, scratch + kOut), 0u);
    const uint32_t reopened = read(scratch + kOut);
    CHECK(reopened && reopened != worker);
    std::shared_ptr<ThreadObjectIdentity> worker_identity;
    CHECK_ST(find_thread_object(worker_body, &worker_identity), Status::Ok);
    CHECK_EQ(thread_object_handle_count(worker_identity), 2u);
    CHECK_EQ(call(__imp__ObOpenObjectByPointer, c, worker_body, 0), kAccessViolation);
    CHECK_EQ(call(__imp__NtResumeThread, c, reopened, 0), 0u);  // the new handle is a real thread handle
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx, c, reopened, 0, 0, 0), 0u);
    CHECK(thread_object_exited(worker_identity));
    CHECK_EQ(thread_object_exit_code(worker_identity), 0x77u);
    // ObLookupAnyThreadByThreadId still finds the exited worker while its object lives.
    CHECK_EQ(call(__imp__ObLookupAnyThreadByThreadId, c, thread_object_thread_id(worker_identity), scratch + kOut), 0u);
    CHECK_EQ(read(scratch + kOut), worker_body);
    call(__imp__ObDereferenceObject, c, worker_body);
    uint32_t event_type = 0, thread_type = 0;
    CHECK(find_variable_import(kModuleXboxkrnl, 0x000E, &event_type));
    CHECK(find_variable_import(kModuleXboxkrnl, 0x001B, &thread_type));
    CHECK_EQ(event_type, kExEventObjectTypeVirtualAddress);
    CHECK_EQ(event_type, event_object_type_address());
    CHECK(!mem.is_committed(event_type, 1));
    uint64_t value = 0;
    // Identity-only: no field of the token is ever served (the kernel-token page
    // provider reports UnknownField once title variables are registered).
    CHECK(runtime_virtual_read(event_type, 4, 0, &value) != VirtualAccessStatus::Handled);
    CHECK_EQ(call(__imp__NtCreateEvent, c, scratch + kOut, 0, 0, 0), 0u);
    const uint32_t ev = read(scratch + kOut);
    CHECK_EQ(call(__imp__ObReferenceObjectByHandle, c, worker, event_type, scratch + kOut2), kTypeMismatch);
    CHECK_EQ(call(__imp__ObReferenceObjectByHandle, c, ev, thread_type, scratch + kOut2), kTypeMismatch);
    CAPTURE_FATAL(call(__imp__ObReferenceObjectByHandle, c, ev, event_type, scratch + kOut2), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    CHECK(g_fatal_msg.find("KEVENT") != std::string::npos);
    call(__imp__ObDereferenceObject, c, worker_body);  // the ObReferenceObjectByHandle above
    for (uint32_t h : {worker, reopened, ev}) CHECK_EQ(call(__imp__NtClose, c, h), 0u);
    worker_identity.reset();

    // ---- NtWriteFileGather ------------------------------------------------------------
    CHECK_ST(r.vfs.mount("save", save, MountAccess::ReadWrite), Status::Ok);
    OpenRequest create;
    create.write_access = true;
    create.disposition = OpenDisposition::Create;
    uint32_t file = 0;
    CHECK_ST(r.vfs.open(r.handles, "save:\\out.bin", create, &file), Status::Ok);
    for (uint32_t i = 0; i < 3 * 0x1000; ++i) mem.base()[scratch + kPages + i] = uint8_t(i / 0x1000 + 1);
    // Segment order differs from memory order: page 2, page 0, page 1.
    CHECK(guest_write_be32(scratch + kSegments + 0, scratch + kPages + 0x2000));
    CHECK(guest_write_be32(scratch + kSegments + 4, scratch + kPages + 0x0000));
    CHECK(guest_write_be32(scratch + kSegments + 8, scratch + kPages + 0x1000));
    CHECK(guest_write_be64(scratch + kOffset, 0x10));
    CHECK_EQ(call(__imp__NtWriteFileGather, c, file, 0, 0, 0, scratch + kIosb, scratch + kSegments, 0x2000 + 0x10,
                  scratch + kOffset), 0u);
    CHECK_EQ(read(scratch + kIosb), 0u);
    CHECK_EQ(read(scratch + kIosb + 4), 0x2010u);
    // Appending (FILE_WRITE_TO_END_OF_FILE) one more partial page.
    CHECK(guest_write_be64(scratch + kOffset, 0xFFFFFFFFFFFFFFFFull));
    CHECK_EQ(call(__imp__NtWriteFileGather, c, file, 0, 0, 0, scratch + kIosb, scratch + kSegments + 8, 4,
                  scratch + kOffset), 0u);
    CHECK_EQ(call(__imp__NtClose, c, file), 0u);
    {
        FILE* f = fopen((save + "/out.bin").c_str(), "rb");
        CHECK(f != nullptr);
        std::string bytes;
        int ch;
        while (f && (ch = fgetc(f)) != EOF) bytes.push_back(char(ch));
        if (f) fclose(f);
        CHECK_EQ(bytes.size(), size_t(0x10 + 0x2010 + 4));
        if (bytes.size() == 0x10 + 0x2010 + 4) {
            CHECK_EQ(uint8_t(bytes[0x10]), 3u);           // page 2 first
            CHECK_EQ(uint8_t(bytes[0x10 + 0xFFF]), 3u);
            CHECK_EQ(uint8_t(bytes[0x10 + 0x1000]), 1u);  // then page 0
            CHECK_EQ(uint8_t(bytes[0x10 + 0x2000]), 2u);  // 0x10 bytes of page 1
            CHECK_EQ(uint8_t(bytes[0x10 + 0x2010]), 2u);  // the appended bytes (page 1)
        }
    }
    // Errors: unreadable page, read-only handle, APC routine.
    CHECK_ST(r.vfs.open(r.handles, "save:\\out.bin", false, &file), Status::Ok);
    CHECK(guest_write_be32(scratch + kSegments + 12, 0x50000000u));
    CHECK_EQ(call(__imp__NtWriteFileGather, c, file, 0, 0, 0, scratch + kIosb, scratch + kSegments + 12, 4, 0),
             kAccessViolation);
    CHECK_EQ(call(__imp__NtWriteFileGather, c, file, 0, 0, 0, scratch + kIosb, scratch + kSegments, 4, 0), kAccessDenied);
    CAPTURE_FATAL(call(__imp__NtWriteFileGather, c, file, 0, 0x82000000u, 0, scratch + kIosb, scratch + kSegments, 4, 0), fatal);
    CHECK(fatal && g_fatal_kind == RCOMP_FATAL_INDIRECT_TARGET);  // APC routine without a recompiled function
    CHECK_EQ(call(__imp__NtClose, c, file), 0u);

    CHECK_ST(r.heap.free(scratch), Status::Ok);
    runtime_shutdown();
    clear_imports();
    clear_functions();
    mem.release();
    unlink((save + "/out.bin").c_str());
    rmdir(save.c_str());
    rmdir(top.c_str());
    return test_result("rt_kernel_more");
}
