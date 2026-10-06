// HLE integration tests: real dispatcher/handles/guest memory. TESTDOUBLE
// entries stand in only for AOT guest callers and call the production thunks.
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/clock_sync.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__KeInitializeEvent);
PPC_EXTERN_FUNC(__imp__KeSetEvent);
PPC_EXTERN_FUNC(__imp__KeInitializeSemaphore);
PPC_EXTERN_FUNC(__imp__KeReleaseSemaphore);
PPC_EXTERN_FUNC(__imp__KeWaitForSingleObject);
PPC_EXTERN_FUNC(__imp__KeWaitForMultipleObjects);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtCreateSemaphore);
PPC_EXTERN_FUNC(__imp__NtReleaseSemaphore);
PPC_EXTERN_FUNC(__imp__NtCreateMutant);
PPC_EXTERN_FUNC(__imp__NtReleaseMutant);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__NtWaitForMultipleObjectsEx);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__ExCreateThread);
PPC_EXTERN_FUNC(__imp__NtResumeThread);
PPC_EXTERN_FUNC(__imp__ExTerminateThread);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory mem;
uint32_t scratch;
constexpr uint32_t timeout_off=0x100, array_off=0x110, prev_off=0x130;
constexpr uint32_t sem_off=0x180, event_off=0x1A0;
constexpr uint32_t timed_out=0x102, invalid_parameter=0xC000000D;
uint32_t sem_handle, event_handle, mutant_handle;
std::atomic<uint32_t> result{~0u}, release_result{~0u}, completed{0}, timed{0};
std::atomic<uint32_t> entering{0};
std::atomic<bool> started{false};

uint32_t read(uint32_t address) { uint32_t value=0; CHECK(guest_read_be32(address,&value)); return value; }
void write(uint32_t address,uint32_t value) { CHECK(guest_write_be32(address,value)); }
uint32_t call(PPCFunc* fn,PPCContext& c,uint32_t a=0,uint32_t b=0,uint32_t d=0,
              uint32_t e=0,uint32_t f=0,uint32_t g=0,uint32_t h=0,uint32_t i=0) {
    c.r3.u64=a; c.r4.u64=b; c.r5.u64=d; c.r6.u64=e;
    c.r7.u64=f; c.r8.u64=g; c.r9.u64=h; c.r10.u64=i;
    fn(c,mem.base()); return c.r3.u32;
}
void as_guest(PPCFunc* fn) {
    GuestThread t; PPCContext c; uint32_t code;
    CHECK_ST(create_guest_thread(runtime()->heap,{0x10000,0,0},&c,&t),Status::Ok);
    CHECK_ST(run_guest_thread(t,c,mem.base(),fn,&code),Status::Ok);
    CHECK_ST(destroy_guest_thread(runtime()->heap,&t),Status::Ok);
}
uint32_t poll(PPCContext& c,uint32_t handle) {
    return call(__imp__NtWaitForSingleObjectEx,c,handle,0,1,scratch+timeout_off);
}
void close(PPCContext& c,uint32_t handle) { CHECK_EQ(call(__imp__NtClose,c,handle),0u); }
uint32_t multiple(PPCContext& c,bool guest,uint32_t count,uint32_t type,uint32_t timeout) {
    return guest ? call(__imp__KeWaitForMultipleObjects,c,count,scratch+array_off,type,0,0,1,timeout,0)
                 : call(__imp__NtWaitForMultipleObjectsEx,c,count,scratch+array_off,type,0,1,timeout);
}

void TESTDOUBLE_semaphores(PPCContext& c,uint8_t*) {
    const uint32_t output=scratch, previous=scratch+prev_off, timeout=scratch+timeout_off;
    guest_write_be64(timeout,0);
    CHECK_EQ(call(__imp__NtCreateSemaphore,c,output,0,1,3),0u); sem_handle=read(output);
    CHECK_EQ(call(__imp__NtCreateEvent,c,output,0,1,0),0u); event_handle=read(output);
    write(scratch+array_off,event_handle); write(scratch+array_off+4,sem_handle);
    CHECK_EQ(multiple(c,false,2,1,timeout),1u);  // lowest ready index
    CHECK_EQ(poll(c,sem_handle),timed_out);
    write(previous,0xDEADBEEF);
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,sem_handle,0,previous),invalid_parameter);
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,sem_handle,0xFFFFFFFF,previous),invalid_parameter);
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,sem_handle,4,previous),0xC0000047u);
    CHECK_EQ(read(previous),0xDEADBEEFu); CHECK_EQ(poll(c,sem_handle),timed_out);
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,sem_handle,3,previous),0u); CHECK_EQ(read(previous),0u);
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,sem_handle,1,previous),0xC0000047u);
    for(int i=0;i<3;++i) CHECK_EQ(poll(c,sem_handle),0u);
    CHECK_EQ(poll(c,sem_handle),timed_out);
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,event_handle,1,0),0xC0000024u);
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,sem_handle,1,1),0xC0000005u);
    CHECK_EQ(poll(c,sem_handle),timed_out);  // invalid output cannot release
    CHECK_EQ(call(__imp__NtCreateSemaphore,c,output,0,4,3),invalid_parameter);
    CHECK_EQ(call(__imp__NtCreateSemaphore,c,output,0,0,0),invalid_parameter);
    CHECK_EQ(call(__imp__NtCreateSemaphore,c,0,0,0,1),0xC0000005u);
    CHECK_EQ(multiple(c,false,0,0,timeout),invalid_parameter);
    CHECK_EQ(multiple(c,false,65,0,timeout),invalid_parameter);
    CHECK_EQ(multiple(c,false,2,2,timeout),invalid_parameter);
    CHECK_EQ(call(__imp__NtWaitForMultipleObjectsEx,c,2,0xFFFFFFFC,0,0,0,timeout),0xC0000005u);
    // Every handle is checked before any signal is consumed.
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,sem_handle,1),0u);
    write(scratch+array_off,sem_handle); write(scratch+array_off+4,0x12345678);
    CHECK_EQ(multiple(c,false,2,1,timeout),0xC0000008u); CHECK_EQ(poll(c,sem_handle),0u);
    write(scratch+array_off+4,sem_handle);
    CHECK_EQ(multiple(c,false,2,0,timeout),invalid_parameter);
    close(c,event_handle); close(c,sem_handle);
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,sem_handle,1),0xC0000008u);
}

void TESTDOUBLE_guest_objects(PPCContext& c,uint8_t*) {
    const uint32_t sem=scratch+sem_off, event=scratch+event_off, timeout=scratch+timeout_off;
    guest_write_be64(timeout,0);
    call(__imp__KeInitializeSemaphore,c,sem,1,2);
    call(__imp__KeInitializeEvent,c,event,1,0);
    CHECK_EQ(mem.base()[sem],5u); CHECK_EQ(mem.base()[sem+2],5u);
    CHECK_EQ(read(sem+4),1u); CHECK_EQ(read(sem+16),2u);
    write(scratch+array_off,sem); write(scratch+array_off+4,event);
    CHECK_EQ(multiple(c,true,2,0,timeout),timed_out); CHECK_EQ(read(sem+4),1u);
    call(__imp__KeSetEvent,c,event,0,0);
    CHECK_EQ(multiple(c,true,2,0,timeout),0u);
    CHECK_EQ(read(sem+4),0u); CHECK_EQ(read(event+4),0u);
    CHECK_EQ(call(__imp__KeReleaseSemaphore,c,sem,0,2,0),0u);
    CHECK_EQ(call(__imp__KeWaitForSingleObject,c,sem,0,0,1,timeout),0u);
    CHECK_EQ(read(sem+4),1u);
    CHECK_EQ(multiple(c,true,2,1,timeout),0u); CHECK_EQ(read(sem+4),0u);
    call(__imp__KeInitializeEvent,c,event,0,1); // manual-reset remains signaled
    CHECK_EQ(multiple(c,true,2,1,timeout),1u); CHECK_EQ(read(event+4),1u);
    CHECK_EQ(call(__imp__KeReleaseSemaphore,c,sem,0,1,1),0u);
    CHECK_EQ(multiple(c,true,2,1,timeout),0u); // both ready: first wins only
    CHECK_EQ(read(event+4),1u);
    CHECK_EQ(call(__imp__KeReleaseSemaphore,c,sem,0,1,0),0u);
    CHECK_EQ(multiple(c,true,2,0,timeout),0u); CHECK_EQ(read(event+4),1u);
    // Relative and elapsed absolute deadlines. Alertable must not fabricate
    // an APC wakeup; no APC producer exists in this supported subset.
    guest_write_be64(timeout,uint64_t(-200000));
    auto begin=std::chrono::steady_clock::now();
    CHECK_EQ(multiple(c,true,2,0,timeout),timed_out);
    CHECK(std::chrono::steady_clock::now()-begin>=std::chrono::milliseconds(18));
    guest_write_be64(timeout,system_filetime()-10000);
    CHECK_EQ(multiple(c,true,2,0,timeout),timed_out);
    guest_write_be64(timeout,0);
}

void TESTDOUBLE_mutant_other(PPCContext& c,uint8_t*) {
    result=poll(c,mutant_handle);
    release_result=call(__imp__NtReleaseMutant,c,mutant_handle,0);
}
void TESTDOUBLE_mutant_abandon(PPCContext& c,uint8_t*) {
    result=poll(c,mutant_handle);  // exits owning mutant
}
void TESTDOUBLE_mutant_terminate(PPCContext& c,uint8_t*) {
    result=poll(c,mutant_handle);
    call(__imp__ExTerminateThread,c,0x42);
}
void TESTDOUBLE_mutants(PPCContext& c,uint8_t*) {
    const uint32_t timeout=scratch+timeout_off;
    guest_write_be64(timeout,0);
    CHECK_EQ(call(__imp__NtCreateMutant,c,scratch,0,1),0u); mutant_handle=read(scratch);
    CHECK_EQ(poll(c,mutant_handle),0u);  // recursive depth 2
    std::thread other([] { as_guest(TESTDOUBLE_mutant_other); }); other.join();
    CHECK_EQ(result.load(),timed_out); CHECK_EQ(release_result.load(),0xC0000046u);
    CHECK_EQ(call(__imp__NtReleaseMutant,c,mutant_handle),0u);
    std::thread other2([] { as_guest(TESTDOUBLE_mutant_other); }); other2.join();
    CHECK_EQ(result.load(),timed_out);  // one recursive acquisition remains
    CHECK_EQ(call(__imp__NtReleaseMutant,c,mutant_handle),0u);
    CHECK_EQ(call(__imp__NtReleaseMutant,c,mutant_handle),0xC0000046u);
    std::thread owner([] { as_guest(TESTDOUBLE_mutant_abandon); }); owner.join();
    CHECK_EQ(result.load(),0u); CHECK_EQ(poll(c,mutant_handle),0x80u);
    CHECK_EQ(poll(c,mutant_handle),0u);  // abandoned indication consumed once
    CHECK_EQ(call(__imp__NtReleaseMutant,c,mutant_handle),0u);
    CHECK_EQ(call(__imp__NtReleaseMutant,c,mutant_handle),0u);
    std::thread owner2([] { as_guest(TESTDOUBLE_mutant_terminate); }); owner2.join();
    CHECK_EQ(result.load(),0u);
    CHECK_EQ(call(__imp__NtCreateSemaphore,c,scratch,0,0,1),0u); sem_handle=read(scratch);
    write(scratch+array_off,sem_handle); write(scratch+array_off+4,mutant_handle);
    // WaitAny reports the abandoned mutant's array index.
    CHECK_EQ(multiple(c,false,2,1,timeout),0x81u);
    CHECK_EQ(call(__imp__NtReleaseMutant,c,mutant_handle),0u);
    std::thread owner3([] { as_guest(TESTDOUBLE_mutant_abandon); }); owner3.join();
    // Failed WaitAll preserves both abandoned status and semaphore count.
    write(scratch+array_off,mutant_handle); write(scratch+array_off+4,sem_handle);
    CHECK_EQ(multiple(c,false,2,0,timeout),timed_out);
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,sem_handle,1),0u);
    CHECK_EQ(multiple(c,false,2,0,timeout),0x80u);
    CHECK_EQ(poll(c,sem_handle),timed_out);
    CHECK_EQ(call(__imp__NtReleaseMutant,c,mutant_handle),0u);
    close(c,sem_handle); close(c,mutant_handle);
    CHECK_EQ(call(__imp__NtReleaseMutant,c,mutant_handle),0xC0000008u);
}

void TESTDOUBLE_semaphore_waiter(PPCContext& c,uint8_t*) {
    ++entering;
    const auto status=call(__imp__NtWaitForSingleObjectEx,c,sem_handle,0,0,scratch+timeout_off);
    if(status==0) ++completed; else if(status==timed_out) ++timed; else result=status;
}
void TESTDOUBLE_waitall_waiter(PPCContext& c,uint8_t*) {
    const auto status=multiple(c,false,2,0,scratch+timeout_off);
    if(status==0) ++completed; else if(status==timed_out) ++timed; else result=status;
}
void TESTDOUBLE_competition(PPCContext& c,uint8_t*) {
    guest_write_be64(scratch+timeout_off,uint64_t(-1500000));  // 150ms, bounded
    CHECK_EQ(call(__imp__NtCreateSemaphore,c,scratch,0,2,2),0u); sem_handle=read(scratch);
    completed=0; timed=0; result=~0u;
    std::vector<std::thread> workers;
    for(int i=0;i<3;++i) workers.emplace_back([] { as_guest(TESTDOUBLE_semaphore_waiter); });
    for(auto& w:workers) w.join();
    CHECK_EQ(completed.load(),2u); CHECK_EQ(timed.load(),1u); CHECK_EQ(result.load(),~0u);
    // Releasing two tokens wakes two already-started waiters and preserves
    // the previous count. Every worker is bounded even if notification fails.
    completed=0; timed=0; entering=0; workers.clear();
    guest_write_be64(scratch+timeout_off,uint64_t(-10000000));
    for(int i=0;i<2;++i) workers.emplace_back([] { as_guest(TESTDOUBLE_semaphore_waiter); });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(entering<2 && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    CHECK_EQ(entering.load(),2u);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(completed.load(),0u);
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,sem_handle,2,scratch+prev_off),0u);
    CHECK_EQ(read(scratch+prev_off),0u);
    for(auto& w:workers) w.join();
    CHECK_EQ(completed.load(),2u); CHECK_EQ(timed.load(),0u);
    guest_write_be64(scratch+timeout_off,uint64_t(-1500000));
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,sem_handle,1),0u);
    CHECK_EQ(call(__imp__NtCreateEvent,c,scratch,0,0,1),0u); event_handle=read(scratch);
    write(scratch+array_off,sem_handle); write(scratch+array_off+4,event_handle);
    completed=0; timed=0; workers.clear();
    for(int i=0;i<2;++i) workers.emplace_back([] { as_guest(TESTDOUBLE_waitall_waiter); });
    for(auto& w:workers) w.join();
    CHECK_EQ(completed.load(),1u); CHECK_EQ(timed.load(),1u);
    close(c,event_handle); close(c,sem_handle);
    guest_write_be64(scratch+timeout_off,0);
}

void TESTDOUBLE_stop_waiter(PPCContext& c,uint8_t*) {
    started=true;
    result=multiple(c,true,2,0,scratch+timeout_off);
}
void TESTDOUBLE_empty_worker(PPCContext& c,uint8_t*) { c.r3.u64=0; }
void TESTDOUBLE_thread_waits(PPCContext& c,uint8_t*) {
    guest_write_be64(scratch+timeout_off,0);
    CHECK_EQ(call(__imp__ExCreateThread,c,scratch,0x10000,0,0,0x82000200,0,1),0u);
    const uint32_t worker=read(scratch);
    CHECK_EQ(call(__imp__NtCreateEvent,c,scratch,0,1,1),0u);
    const uint32_t event=read(scratch);
    write(scratch+array_off,event); write(scratch+array_off+4,worker);
    CHECK_EQ(multiple(c,false,2,0,scratch+timeout_off),timed_out);
    CHECK_EQ(call(__imp__NtResumeThread,c,worker,0),0u);
    guest_write_be64(scratch+timeout_off,uint64_t(-10000000));
    CHECK_EQ(multiple(c,false,2,0,scratch+timeout_off),0u); // event was preserved
    guest_write_be64(scratch+timeout_off,0);
    CHECK_EQ(multiple(c,false,2,1,scratch+timeout_off),1u); // thread stays signaled
    close(c,event); close(c,worker);
}
void TESTDOUBLE_start_worker(PPCContext& c,uint8_t*) {
    call(__imp__KeInitializeSemaphore,c,scratch+sem_off,1,1);
    call(__imp__KeInitializeEvent,c,scratch+event_off,1,0);
    write(scratch+array_off,scratch+sem_off); write(scratch+array_off+4,scratch+event_off);
    guest_write_be64(scratch+timeout_off,0x8000000000000000ull);
    CHECK_EQ(call(__imp__ExCreateThread,c,scratch,0x10000,0,0,0x82000100,0,0),0u);
}

void test_protected_sync() {
    // These are real protected mappings, outside the heap. translate() is
    // intentionally nonnull for Read/None: the HLE must check access rights.
    constexpr uint32_t output=0x02000000, input=output+0x10000, guard=input+0x10000;
    constexpr uint32_t av=0xC0000005, sentinel=0xAABBCCDD;
    CHECK(mem.commit(output,0x30000,Protect::ReadWrite)==MemStatus::Ok);
    PPCContext c{}; bool fatal=false;
    guest_write_be64(scratch+timeout_off,0);
    CHECK_EQ(call(__imp__NtCreateSemaphore,c,scratch,0,0,2),0u);
    const uint32_t semaphore=read(scratch);
    CHECK_EQ(call(__imp__ExCreateThread,c,scratch,0x10000,0,0,0x82000200,0,1),0u);
    const uint32_t suspended_worker=read(scratch), live=runtime()->handles.live_count();
    for(Protect protection:{Protect::Read,Protect::None}) {
        call(__imp__KeInitializeSemaphore,c,output,1,2);
        write(output+0x40,sentinel);
        CHECK(mem.protect(output,0x10000,protection)==MemStatus::Ok);
        CHECK(mem.translate(output+0x40,4)!=nullptr);
        CHECK_EQ(call(__imp__NtCreateSemaphore,c,output+0x40,0,0,1),av);
        CHECK_EQ(call(__imp__NtCreateMutant,c,output+0x40,0,0),av);
        CHECK_EQ(call(__imp__NtReleaseSemaphore,c,semaphore,1,output+0x40),av);
        CHECK_EQ(call(__imp__NtResumeThread,c,suspended_worker,output+0x40),av);
        CHECK_EQ(poll(c,suspended_worker),timed_out);
        CHECK_EQ(runtime()->handles.live_count(),live);
        CHECK_EQ(poll(c,semaphore),timed_out); // failed release did not add a token
        CAPTURE_FATAL(call(__imp__KeInitializeSemaphore,c,output,0,1),fatal);
        CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
        CAPTURE_FATAL(call(__imp__KeReleaseSemaphore,c,output,0,1,0),fatal);
        CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
        CAPTURE_FATAL(call(__imp__KeWaitForSingleObject,c,output,0,0,0,scratch+timeout_off),fatal);
        CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
        CHECK(mem.protect(output,0x10000,Protect::ReadWrite)==MemStatus::Ok);
        CHECK_EQ(read(output+0x40),sentinel); CHECK_EQ(read(output+4),1u);
    }
    write(output+0x40,sentinel);
    CHECK_EQ(call(__imp__NtResumeThread,c,suspended_worker,output+0x40),0u);
    CHECK_EQ(read(output+0x40),1u);
    guest_write_be64(scratch+timeout_off,uint64_t(-10000000));
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx,c,suspended_worker,0,0,scratch+timeout_off),0u);
    close(c,suspended_worker);
    guest_write_be64(scratch+timeout_off,0);

    // Read-only inputs are valid: unnamed attrs, handle array, timeout and a
    // manual-reset event wait all require Read, not ReadWrite.
    memset(mem.base()+input,0,0x10000);
    write(input+0x20,semaphore); guest_write_be64(input+0x40,0);
    call(__imp__KeInitializeEvent,c,input+0x80,0,1);
    call(__imp__KeInitializeEvent,c,input+0xA0,1,1);
    write(input+0x28,input+0x80);
    write(input+0x30,scratch+sem_off); write(input+0x34,input+0xA0);
    write(guard-4,semaphore);
    CHECK(mem.protect(input,0x10000,Protect::Read)==MemStatus::Ok);
    CHECK(mem.protect(guard,0x10000,Protect::None)==MemStatus::Ok);
    CHECK_EQ(call(__imp__NtCreateSemaphore,c,scratch,input,0,1),0u); close(c,read(scratch));
    CHECK_EQ(call(__imp__NtCreateMutant,c,scratch,input,0),0u); close(c,read(scratch));
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,semaphore,1),0u);
    CHECK_EQ(call(__imp__NtWaitForMultipleObjectsEx,c,1,input+0x20,0,0,0,input+0x40),0u);
    CHECK_EQ(call(__imp__KeWaitForMultipleObjects,c,1,input+0x28,0,0,0,0,input+0x40),0u);
    CHECK_EQ(call(__imp__KeWaitForSingleObject,c,input+0x80,0,0,0,input+0x40),0u);

    // Unreadable/cross-page inputs must fail before retaining a new object
    // or consuming a ready semaphore. Rejected NT inputs return NTSTATUS.
    CHECK_EQ(call(__imp__NtReleaseSemaphore,c,semaphore,1),0u);
    for(uint32_t attrs:{guard,guard-8}) {
        CHECK_EQ(call(__imp__NtCreateSemaphore,c,scratch,attrs,0,1),av);
        CHECK_EQ(call(__imp__NtCreateMutant,c,scratch,attrs,0),av);
    }
    CHECK_EQ(call(__imp__NtWaitForMultipleObjectsEx,c,1,guard,0,0,0,input+0x40),av);
    CHECK_EQ(call(__imp__NtWaitForMultipleObjectsEx,c,2,guard-4,0,0,0,input+0x40),av);
    for(uint32_t timeout:{guard,guard-4}) {
        CHECK_EQ(call(__imp__NtWaitForMultipleObjectsEx,c,1,input+0x20,0,0,0,timeout),av);
        CHECK_EQ(call(__imp__NtWaitForSingleObjectEx,c,semaphore,0,0,timeout),av);
    }
    CHECK_EQ(runtime()->handles.live_count(),live-1);
    CHECK_EQ(poll(c,semaphore),0u); CHECK_EQ(poll(c,semaphore),timed_out);

    // Ke failures diagnose guest_access before taking the dispatcher lock.
    // Validation of the second object must not consume the first one.
    call(__imp__KeInitializeSemaphore,c,scratch+sem_off,1,2);
    CAPTURE_FATAL(call(__imp__KeWaitForMultipleObjects,c,2,input+0x30,0,0,0,0,input+0x40),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS); CHECK_EQ(read(scratch+sem_off+4),1u);
    CAPTURE_FATAL(call(__imp__KeWaitForMultipleObjects,c,1,guard,0,0,0,0,input+0x40),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CAPTURE_FATAL(call(__imp__KeWaitForMultipleObjects,c,1,input+0x28,0,0,0,0,guard),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CAPTURE_FATAL(call(__imp__KeWaitForSingleObject,c,input+0x80,0,0,0,guard-4),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CHECK_EQ(call(__imp__KeWaitForSingleObject,c,scratch+sem_off,0,0,0,input+0x40),0u);
    // A successful dispatcher operation after all failures proves no lock
    // was stranded; mapping restoration lets us inspect unchanged bytes.
    CHECK(mem.protect(input,0x20000,Protect::ReadWrite)==MemStatus::Ok);
    CHECK_EQ(read(input+0xA4),1u);
    close(c,semaphore);
    CHECK_EQ(runtime()->handles.live_count(),0u);
    CHECK(mem.decommit(output,0x30000)==MemStatus::Ok);
}
}

int main() {
    CHECK(mem.reserve()==MemStatus::Ok); CHECK_ST(runtime_init(&mem),Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(),Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x1000,64,true,&scratch),Status::Ok);
    const FuncEntry entries[]={{0x82000100,TESTDOUBLE_stop_waiter,"TESTDOUBLE_stop_waiter"},
                              {0x82000200,TESTDOUBLE_empty_worker,"TESTDOUBLE_empty_worker"}};
    CHECK(register_functions(entries,2));
    as_guest(TESTDOUBLE_semaphores); as_guest(TESTDOUBLE_guest_objects);
    as_guest(TESTDOUBLE_mutants); as_guest(TESTDOUBLE_competition);
    as_guest(TESTDOUBLE_thread_waits);
    test_protected_sync();
    CHECK_EQ(runtime()->handles.live_count(),0u);

    // Failing guest operations retain state and do not strand dispatcher lock.
    PPCContext c{}; bool fatal=false;
    call(__imp__KeInitializeSemaphore,c,scratch+sem_off,1,1);
    CAPTURE_FATAL(call(__imp__KeReleaseSemaphore,c,scratch+sem_off,0,1,0),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_EQ(read(scratch+sem_off+4),1u);
    CAPTURE_FATAL(call(__imp__KeWaitForSingleObject,c,scratch+sem_off,0,0,0,1),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    guest_write_be64(scratch+timeout_off,0);
    CHECK_EQ(call(__imp__KeWaitForSingleObject,c,scratch+sem_off,0,0,0,scratch+timeout_off),0u);
    write(scratch+0x44,scratch+0x60);  // named object remains explicit fatal
    CAPTURE_FATAL(call(__imp__NtCreateSemaphore,c,scratch,scratch+0x40,0,1),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_UNIMPLEMENTED);
    CHECK_EQ(runtime()->handles.live_count(),0u);
    CHECK_EQ(runtime()->heap.stats().live_allocations,1u);

    as_guest(TESTDOUBLE_start_worker);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!started && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    CHECK(started.load());
    runtime_quiesce_threads();
    CHECK_EQ(result.load(),0xC000004Bu); CHECK_EQ(read(scratch+sem_off+4),1u);
    CHECK_EQ(runtime()->heap.stats().live_allocations,1u);
    close(c,read(scratch)); CHECK_EQ(runtime()->handles.live_count(),0u);
    runtime_shutdown(); clear_functions(); clear_imports();
    return test_result("rt_sync_objects");
}
