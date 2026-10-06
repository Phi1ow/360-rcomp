#include <atomic>
#include <chrono>
#include <thread>
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"
PPC_EXTERN_FUNC(__imp__ExCreateThread);
PPC_EXTERN_FUNC(__imp__KeInitializeEvent);
PPC_EXTERN_FUNC(__imp__KeWaitForSingleObject);
PPC_EXTERN_FUNC(__imp__KeTlsAlloc);
PPC_EXTERN_FUNC(__imp__NtClose);
using namespace rcomp;
using namespace rcomp::rt;
namespace {
std::atomic<bool> started{false}, finished{false}, suspended_ran{false};
std::atomic<uint32_t> wait_status{0}, slot{~0u};
uint32_t event_address, timeout_address;
void TESTDOUBLE_wait_entry(PPCContext& c,uint8_t* base) {
    started=true; c.r3.u64=event_address; c.r7.u64=timeout_address;
    __imp__KeWaitForSingleObject(c,base);
    wait_status=c.r3.u32; finished=true;
}
void TESTDOUBLE_suspended_entry(PPCContext&,uint8_t*) { suspended_ran=true; }
void TESTDOUBLE_tls_entry(PPCContext& c,uint8_t* base) { __imp__KeTlsAlloc(c,base); slot=c.r3.u32; }
uint32_t create(PPCContext& c,uint8_t* base,uint32_t output,uint32_t fn,bool suspended) {
    c.r3.u64=output; c.r4.u64=0x10000; c.r5.u64=0; c.r6.u64=0;
    c.r7.u64=fn; c.r8.u64=0; c.r9.u64=suspended;
    __imp__ExCreateThread(c,base); return c.r3.u32;
}
void allocate_dynamic_slot(GuestMemory& mem) {
    GuestThread t; PPCContext c; uint32_t code;
    CHECK_ST(create_guest_thread(runtime()->heap,{0x10000,0,0},&c,&t),Status::Ok);
    CHECK_ST(run_guest_thread(t,c,mem.base(),TESTDOUBLE_tls_entry,&code),Status::Ok);
    CHECK_ST(destroy_guest_thread(runtime()->heap,&t),Status::Ok);
}
}
int main() {
    // Failure after claiming the opaque arena must not retain a process-global
    // owner. A different GuestMemory can bootstrap immediately afterward.
    {
        GuestMemory failed;
        CHECK(failed.reserve() == MemStatus::Ok);
        RuntimeConfig config;
        config.heap_hi = config.heap_lo;
        CHECK_ST(runtime_init(&failed, config), Status::InvalidArgument);
        CHECK(runtime() == nullptr);
        GuestMemory recovered;
        CHECK(recovered.reserve() == MemStatus::Ok);
        CHECK_ST(runtime_init(&recovered), Status::Ok);
        runtime_shutdown();
        config = RuntimeConfig{};
        config.phys_lo = config.heap_lo;
        config.phys_hi = config.heap_hi;
        CHECK_ST(runtime_init(&failed, config), Status::InvalidArgument);
        CHECK(runtime() == nullptr);
        CHECK_ST(runtime_init(&recovered), Status::Ok);
        runtime_shutdown();
    }
    GuestMemory mem; CHECK(mem.reserve()==MemStatus::Ok);
    CHECK_ST(runtime_init(&mem),Status::Ok);
    Runtime* owner=runtime();
    CHECK_ST(runtime_init(&mem),Status::AlreadyExists); CHECK(runtime()==owner);
    CHECK_ST(register_xboxkrnl_hle(),Status::Ok);
    const FuncEntry f[]={{0x82000100,TESTDOUBLE_wait_entry,"TESTDOUBLE_wait_entry"},
                        {0x82000200,TESTDOUBLE_suspended_entry,"TESTDOUBLE_suspended_entry"}};
    CHECK(register_functions(f,2));
    uint32_t scratch=0; CHECK_ST(owner->heap.alloc(64,16,true,&scratch),Status::Ok);
    event_address=scratch+16; timeout_address=scratch+40;
    CHECK(guest_write_be64(timeout_address,0x8000000000000000ull));
    PPCContext c; c.r3.u64=event_address; c.r4.u64=0; c.r5.u64=0;
    __imp__KeInitializeEvent(c,mem.base());
    CHECK_EQ(create(c,mem.base(),scratch,0x82000100,false),0u);
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!started && std::chrono::steady_clock::now()<limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(started.load());
    uint32_t h=0; CHECK(guest_read_be32(scratch,&h));
    c.r3.u64=h; __imp__NtClose(c,mem.base()); CHECK_EQ(c.r3.u32,0u);
    CHECK_EQ(create(c,mem.base(),scratch,0x82000200,true),0u);
    CHECK(guest_read_be32(scratch,&h)); c.r3.u64=h; __imp__NtClose(c,mem.base());
    CHECK_EQ(c.r3.u32,0u);
    allocate_dynamic_slot(mem); CHECK_EQ(slot.load(),0u);
    runtime_quiesce_threads();
    CHECK(finished.load()); CHECK_EQ(wait_status.load(),0xC000004Bu);
    CHECK(!suspended_ran.load()); CHECK_EQ(owner->heap.stats().live_allocations,1u);
    CHECK_EQ(create(c,mem.base(),scratch,0x82000200,false),0xC000004Bu);
    CHECK_EQ(owner->heap.stats().live_allocations,1u);
    runtime_shutdown(); CHECK(runtime()==nullptr);
    clear_imports(); clear_functions();
    CHECK_ST(runtime_init(&mem),Status::Ok); CHECK_ST(register_xboxkrnl_hle(),Status::Ok);
    allocate_dynamic_slot(mem); CHECK_EQ(slot.load(),0u);
    runtime_shutdown(); clear_imports(); clear_functions();
    return test_result("rt_lifetime");
}
