// Real runtime thread identities, Body references and object-manager HLE.
#include <chrono>
#include <memory>
#include <thread>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__ExCreateThread);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__NtDuplicateObject);
PPC_EXTERN_FUNC(__imp__NtResumeThread);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__ObDereferenceObject);
PPC_EXTERN_FUNC(__imp__ObReferenceObject);
PPC_EXTERN_FUNC(__imp__ObReferenceObjectByHandle);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory mem;
uint32_t scratch = 0;
uint32_t type_token = 0;
uint32_t main_body = 0;
constexpr uint32_t kWorkerEntry = 0x82000100;
constexpr uint32_t kAccessViolation = 0xC0000005u;
constexpr uint32_t kInvalidHandle = 0xC0000008u;
constexpr uint32_t kTypeMismatch = 0xC0000024u;

uint32_t read32(uint32_t address) {
    uint32_t value=0; CHECK(guest_read_be32(address,&value)); return value;
}
void write32(uint32_t address,uint32_t value) { CHECK(guest_write_be32(address,value)); }
uint32_t call(PPCFunc* fn,PPCContext& c,uint32_t a=0,uint32_t b=0,uint32_t d=0,
              uint32_t e=0,uint32_t f=0,uint32_t g=0,uint32_t h=0) {
    c.r3.u64=a; c.r4.u64=b; c.r5.u64=d; c.r6.u64=e;
    c.r7.u64=f; c.r8.u64=g; c.r9.u64=h;
    fn(c,mem.base()); return c.r3.u32;
}

void TESTDOUBLE_worker(PPCContext& c,uint8_t*) { c.r3.u64=0x1234; }

void TESTDOUBLE_current_thread(PPCContext& c,uint8_t*) {
    GuestThread* current=current_guest_thread();
    CHECK(current && current->identity);
    const auto identity=current->identity;
    const uint32_t expected=thread_object_body(identity);
    CHECK(expected>=0x70000000u && expected<0x71000000u && !(expected&15u));
    CHECK(!mem.is_committed(expected,1));
    write32(scratch,0xBAD0CAFE);
    CHECK_EQ(call(__imp__ObReferenceObjectByHandle,c,0xFFFFFFFEu,type_token,scratch),0u);
    main_body=read32(scratch); CHECK_EQ(main_body,expected);
    CHECK_EQ(thread_object_guest_reference_count(identity),1u);
    c.r3.u64=main_body; __imp__ObReferenceObject(c,mem.base());
    CHECK_EQ(c.r3.u32,main_body); CHECK_EQ(thread_object_guest_reference_count(identity),2u);
    c.r3.u64=main_body; __imp__ObDereferenceObject(c,mem.base());
    CHECK_EQ(c.r3.u32,main_body); CHECK_EQ(thread_object_guest_reference_count(identity),1u);
    c.r3.u64=main_body; __imp__ObDereferenceObject(c,mem.base());
    CHECK_EQ(thread_object_guest_reference_count(identity),0u);
    c.r3.u64=0;
}

uint32_t create_suspended(PPCContext& c) {
    write32(scratch,0);
    CHECK_EQ(call(__imp__ExCreateThread,c,scratch,0x10000,0,0,kWorkerEntry,0,1),0u);
    return read32(scratch);
}

uint32_t body_for_handle(uint32_t handle,std::shared_ptr<ThreadObjectIdentity>* identity=nullptr) {
    std::shared_ptr<HandleObject> object;
    CHECK_ST(runtime()->handles.lookup_any(handle,&object),Status::Ok);
    CHECK(object && object->kind()==HandleKind::Thread);
    const uint32_t body=object ? object->guest_object_body() : 0;
    if(identity) CHECK_ST(find_thread_object(body,identity),Status::Ok);
    return body;
}
}

int main() {
    // Pre-heap opaque reservation is not global adoption. A Runtime whose
    // configuration fails after reservation must not poison a retry on another
    // GuestMemory instance.
    {
        GuestMemory rejected;
        CHECK(rejected.reserve()==MemStatus::Ok);
        RuntimeConfig bad;
        bad.phys_lo=bad.heap_lo;
        bad.phys_hi=bad.heap_lo+0x10000;
        CHECK_ST(runtime_init(&rejected,bad),Status::InvalidArgument);
        CHECK(runtime()==nullptr);
        rejected.release();
    }
    CHECK(mem.reserve()==MemStatus::Ok);
    CHECK_ST(runtime_init(&mem),Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(),Status::Ok);
    CHECK_ST(register_thread_object_type_variable(),Status::Ok);
    CHECK(find_variable_import(kModuleXboxkrnl,0x001B,&type_token));
    CHECK_EQ(type_token,thread_object_type_address());
    CHECK(type_token>=0x70000000u && type_token<0x71000000u && !(type_token&15u));
    CHECK(!mem.is_committed(type_token,1));
    CHECK(mem.commit(0x70000000,0x10000,Protect::ReadWrite)==MemStatus::Conflict);
    CHECK_ST(runtime()->heap.alloc(0x1000,64,true,&scratch),Status::Ok);
    const auto baseline=runtime()->heap.stats();
    const FuncEntry functions[]={{kWorkerEntry,TESTDOUBLE_worker,"TESTDOUBLE_worker"}};
    CHECK(register_functions(functions,1));
    std::shared_ptr<ThreadObjectIdentity> gone;

    // Publishing into a caller-owned shared_ptr happens after the registry
    // mutex is released. Replacing its last ownership may run the previous
    // identity destructor, which itself removes a registry entry.
    {
        std::shared_ptr<ThreadObjectIdentity> first,second,reused;
        CHECK_ST(create_thread_object_identity(runtime()->heap,0xF001,0,&first),Status::Ok);
        CHECK_ST(create_thread_object_identity(runtime()->heap,0xF002,0,&second),Status::Ok);
        const uint32_t first_body=thread_object_body(first);
        const uint32_t second_body=thread_object_body(second);
        reused=first; first.reset();
        CHECK_ST(find_thread_object(second_body,&reused),Status::Ok);
        CHECK(reused==second);
        CHECK_ST(find_thread_object(first_body,&gone),Status::NotFound);

        std::shared_ptr<ThreadObjectIdentity> third;
        CHECK_ST(create_thread_object_identity(runtime()->heap,0xF003,0,&third),Status::Ok);
        const uint32_t third_body=thread_object_body(third);
        reused=third; third.reset();
        CHECK_ST(reference_thread_object(second_body,&reused),Status::Ok);
        CHECK(reused==second);
        CHECK_ST(find_thread_object(third_body,&gone),Status::NotFound);
        CHECK_ST(dereference_thread_object(second_body),Status::Ok);
        reused.reset(); second.reset();
    }

    // Main entry thread has the same object identity mechanism and resolves
    // the corroborated current-thread pseudo-handle without a host pthread ID.
    GuestThread main_thread; PPCContext main_context{}; uint32_t main_exit=~0u;
    CHECK_ST(create_guest_thread(runtime()->heap,{0x10000,0,0},&main_context,&main_thread),Status::Ok);
    CHECK(main_thread.identity);
    CHECK_ST(run_guest_thread(main_thread,main_context,mem.base(),TESTDOUBLE_current_thread,&main_exit),Status::Ok);
    CHECK_EQ(main_exit,0u);
    CHECK_ST(destroy_guest_thread(runtime()->heap,&main_thread),Status::Ok);
    CHECK_ST(find_thread_object(main_body,&gone),Status::NotFound);
    CHECK_EQ(runtime()->heap.stats().live_allocations,baseline.live_allocations);

    PPCContext c{};
    const uint32_t source=create_suspended(c);
    std::shared_ptr<ThreadObjectIdentity> identity;
    const uint32_t body=body_for_handle(source,&identity);
    CHECK(body!=main_body); CHECK(!mem.is_committed(body,1));
    CHECK_EQ(thread_object_handle_count(identity),1u);

    // Output access is checked before mutation; wrong type does not acquire a
    // Body reference.
    CHECK_EQ(call(__imp__ObReferenceObjectByHandle,c,source,type_token,0),kAccessViolation);
    CHECK_EQ(thread_object_guest_reference_count(identity),0u);
    write32(scratch+4,0xBAD0CAFE);
    CHECK_EQ(call(__imp__ObReferenceObjectByHandle,c,source,type_token+16,scratch+4),kTypeMismatch);
    CHECK_EQ(read32(scratch+4),0xBAD0CAFEu);
    CHECK_EQ(thread_object_guest_reference_count(identity),0u);

    // A duplicate is a distinct numeric handle to the exact same object.
    uint32_t duplicate=0;
    write32(scratch+4,0);
    CHECK_EQ(call(__imp__NtDuplicateObject,c,source,scratch+4,0),0u);
    duplicate=read32(scratch+4); CHECK(duplicate && duplicate!=source);
    CHECK_EQ(body_for_handle(duplicate),body);
    CHECK_EQ(thread_object_handle_count(identity),2u);
    CHECK_EQ(call(__imp__NtClose,c,source),0u);
    CHECK_EQ(thread_object_handle_count(identity),1u);
    std::shared_ptr<HandleObject> stale;
    CHECK_ST(runtime()->handles.lookup_any(source,&stale),Status::InvalidHandle);
    write32(scratch+8,0xAABBCCDD);
    CHECK_EQ(call(__imp__NtDuplicateObject,c,source,scratch+8,0),kInvalidHandle);
    CHECK_EQ(read32(scratch+8),0xAABBCCDDu);

    // Reference through the surviving duplicate, run to termination, close
    // the last handle: the Body remains owned solely by the guest reference.
    CHECK_EQ(call(__imp__ObReferenceObjectByHandle,c,duplicate,type_token,scratch+4),0u);
    CHECK_EQ(read32(scratch+4),body);
    CHECK_EQ(thread_object_guest_reference_count(identity),1u);
    CHECK_EQ(call(__imp__NtResumeThread,c,duplicate,0),0u);
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx,c,duplicate,0,0,0),0u);
    CHECK(thread_object_exited(identity)); CHECK_EQ(thread_object_exit_code(identity),0x1234u);
    CHECK_EQ(call(__imp__NtClose,c,duplicate),0u);
    CHECK_EQ(thread_object_handle_count(identity),0u);
    std::shared_ptr<ThreadObjectIdentity> retained;
    CHECK_ST(find_thread_object(body,&retained),Status::Ok);
    CHECK(retained==identity);
    c.r3.u64=body; __imp__ObDereferenceObject(c,mem.base());
    CHECK_EQ(thread_object_guest_reference_count(identity),0u);
    retained.reset(); identity.reset();
    CHECK_ST(find_thread_object(body,&gone),Status::NotFound);

    // A later thread receives a new monotonic opaque token; stale Body does
    // not alias it even though no guest memory was committed for either.
    const uint32_t next=create_suspended(c), next_body=body_for_handle(next);
    CHECK(next_body!=body && next_body>body);
    CHECK_EQ(call(__imp__NtResumeThread,c,next,0),0u);
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx,c,next,0,0,0),0u);
    CHECK_EQ(call(__imp__NtClose,c,next),0u);
    bool fatal=false; c.r3.u64=body;
    CAPTURE_FATAL(__imp__ObReferenceObject(c,mem.base()),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);

    CHECK_EQ(runtime()->heap.stats().live_allocations,baseline.live_allocations);
    CHECK_ST(runtime()->heap.free(scratch),Status::Ok);
    runtime_shutdown(); clear_imports(); clear_functions(); mem.release();
    return test_result("rt_objects");
}
