// Host-only deterministic metadata-allocation failures. The allocator and HLE
// under test are production code; only operator new is a TESTDOUBLE fault source.
#include <new>
#include <cstdlib>
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/modules.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

static thread_local int TESTDOUBLE_allocations_until_failure=-1;
static thread_local bool TESTDOUBLE_fail_once=false;
static void* TESTDOUBLE_host_allocate(size_t size) {
    if(TESTDOUBLE_allocations_until_failure==0) {
        if(TESTDOUBLE_fail_once)TESTDOUBLE_allocations_until_failure=-1;
        throw std::bad_alloc();
    }
    if(TESTDOUBLE_allocations_until_failure>0) --TESTDOUBLE_allocations_until_failure;
    if(void* p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
static void TESTDOUBLE_host_free(void* p) noexcept { std::free(p); }
// The language-mandated operator names only bind the named test doubles.
void* operator new(size_t size) { return TESTDOUBLE_host_allocate(size); }
void* operator new[](size_t size) { return TESTDOUBLE_host_allocate(size); }
void operator delete(void* p) noexcept { TESTDOUBLE_host_free(p); }
void operator delete(void* p,size_t) noexcept { TESTDOUBLE_host_free(p); }
void operator delete[](void* p) noexcept { TESTDOUBLE_host_free(p); }
void operator delete[](void* p,size_t) noexcept { TESTDOUBLE_host_free(p); }

PPC_EXTERN_FUNC(__imp__ExAllocatePoolTypeWithTag);
PPC_EXTERN_FUNC(__imp__ExFreePool);
PPC_EXTERN_FUNC(__imp__NtAllocateVirtualMemory);
PPC_EXTERN_FUNC(__imp__MmAllocatePhysicalMemory);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
struct TESTDOUBLE_handle_object final : HandleObject {
    HandleKind kind()const override{return HandleKind::Test;}
};
void same(const GuestHeapStats& a,const GuestHeapStats& b) {
    CHECK_EQ(a.allocated_bytes,b.allocated_bytes); CHECK_EQ(a.free_bytes,b.free_bytes);
    CHECK_EQ(a.live_allocations,b.live_allocations);
}
// A small original loaded XEX/PE, with no export or import table. Its exact
// header still exercises every allocator used to retain optional metadata.
XexImage original_module(GuestMemory& mem) {
    constexpr uint32_t base=0x82000000,header=0x1000,size=0x10000;
    std::vector<uint8_t> x(header+size);
    auto be=[&](size_t p,uint32_t value) {for(int i=0;i<4;++i)x[p+i]=(uint8_t)(value>>(24-i*8));};
    auto le=[&](size_t p,uint32_t value) {for(int i=0;i<4;++i)x[p+i]=(uint8_t)(value>>(i*8));};
    be(0,0x58455832);be(8,header);be(16,0x400);be(20,3);
    be(24,0x3FF);be(28,0x100);be(0x100,8);
    be(32,0x10100);be(36,base+0x1000);be(40,0x10201);be(44,base);
    be(0x404,size);be(0x510,base);
    le(header,0x5A4D);le(header+0x3C,0x80);le(header+0x80,0x4550);
    le(header+0x84,0x1F2);le(header+0x94,224);
    le(header+0x98,0x10B);le(header+0xA8,0x1000);le(header+0xB4,base);le(header+0xD0,size);
    XexImage image;CHECK_ST(load_xex_image(mem,x.data(),x.size(),&image,nullptr),Status::Ok);return image;
}
}
int main() {
    // New slots provision both slot and future reuse bookkeeping before
    // publication. A valid close therefore succeeds with allocation disabled.
    {
        HandleTable table(1);auto object=std::make_shared<TESTDOUBLE_handle_object>();
        uint32_t handle=0;
        for(int point=0;point<2;++point) {
            handle=0xBAD0CAFE;
            TESTDOUBLE_allocations_until_failure=point;
            Status status=table.insert(object,&handle);
            TESTDOUBLE_allocations_until_failure=-1;
            CHECK_ST(status,Status::OutOfMemory);CHECK_EQ(handle,0xBAD0CAFEu);CHECK_EQ(table.live_count(),0u);
        }
        CHECK_ST(table.insert(object,&handle),Status::Ok);
        TESTDOUBLE_allocations_until_failure=0;
        Status status=table.close(handle);
        TESTDOUBLE_allocations_until_failure=-1;
        CHECK_ST(status,Status::Ok);CHECK_EQ(table.live_count(),0u);
        std::shared_ptr<HandleObject> found;CHECK_ST(table.lookup(handle,HandleKind::Test,&found),Status::InvalidHandle);
    }
    GuestMemory mem; if(mem.reserve()!=MemStatus::Ok) return 2;
    RuntimeConfig cfg; cfg.heap_hi=0x40040000;
    CHECK_ST(runtime_init(&mem,cfg),Status::Ok); CHECK_ST(register_xboxkrnl_hle(),Status::Ok);
    auto& r=*runtime(); const auto initial=r.heap.stats();
    // Services retain physical allocations independently of the title's
    // ownership table; OOM rollback must preserve those registered services.
    const auto physical_initial=r.physical.stats();
    // Resolve before arming: import-registry key construction itself may
    // allocate on the host and is outside these allocator-failure contracts.
    PPCFunc* const pool_impl=find_import(kModuleXboxkrnl,0xB);
    PPCFunc* const va_impl=find_import(kModuleXboxkrnl,0xCC);
    PPCFunc* const physical_impl=find_import(kModuleXboxkrnl,0xB9);
    // Interior placement needs prefix, suffix and used nodes. All three
    // failures precede the first guest commit and leave the output untouched.
    for(int point=0;point<3;++point) {
        fprintf(stderr,"TESTDOUBLE heap alloc failure point %d\n",point);
        uint32_t output=0xBAD0CAFE; const auto committed=mem.stats().committed_bytes;
        TESTDOUBLE_allocations_until_failure=point;
        Status status=r.heap.alloc_in(32,16,0x40010000,0x40020000,false,true,&output);
        TESTDOUBLE_allocations_until_failure=-1;
        CHECK_ST(status,Status::OutOfMemory); CHECK_EQ(output,0xBAD0CAFEu);
        same(initial,r.heap.stats()); CHECK_EQ(mem.stats().committed_bytes,committed);
    }
    uint32_t p=0; CHECK_ST(r.heap.alloc(32,16,false,&p),Status::Ok);
    const auto live=r.heap.stats();
    for(int point=0;point<2;++point) {
        fprintf(stderr,"TESTDOUBLE heap free failure point %d\n",point);
        TESTDOUBLE_allocations_until_failure=point;
        Status status=r.heap.free(p);
        TESTDOUBLE_allocations_until_failure=-1;
        CHECK_ST(status,Status::OutOfMemory); same(live,r.heap.stats());
        uint32_t size=0; CHECK_ST(r.heap.allocation_size(p,&size),Status::Ok); CHECK_EQ(size,32u);
    }
    CHECK_ST(r.heap.free(p),Status::Ok); same(initial,r.heap.stats());
    {
        uint32_t guard=0;CHECK_ST(r.heap.alloc(0x10000,0x10000,true,&guard),Status::Ok);
        TESTDOUBLE_allocations_until_failure=0;
        Status status=r.heap.set_guard(guard,0x10000,true);
        TESTDOUBLE_allocations_until_failure=-1;
        CHECK_ST(status,Status::OutOfMemory);CHECK(mem.is_accessible(guard,0x10000,Protect::ReadWrite));
        CHECK_ST(r.heap.set_guard(guard,0x10000,true),Status::Ok);
        CHECK(!mem.is_accessible(guard,1,Protect::Read));
        CHECK_ST(r.heap.free(guard),Status::Ok);CHECK(mem.is_accessible(guard,0x10000,Protect::ReadWrite));same(initial,r.heap.stats());
    }
    // Each free can fail independently. The surviving pointers remain owned
    // and a second destroy completes, including when stack already freed.
    for(int point=0;point<6;++point) {
        GuestThread thread;PPCContext context;
        CHECK_ST(create_guest_thread(r.heap,{0x10000,0,0},&context,&thread),Status::Ok);
        TESTDOUBLE_allocations_until_failure=point;
        Status status=destroy_guest_thread(r.heap,&thread);
        TESTDOUBLE_allocations_until_failure=-1;
        CHECK_ST(status,Status::OutOfMemory);
        CHECK(thread.alloc_base||thread.pcr||thread.tls);
        const uint32_t retained=(thread.alloc_base!=0)+(thread.pcr!=0)+(thread.tls!=0);
        CHECK_EQ(r.heap.stats().live_allocations,retained);
        CHECK_ST(destroy_guest_thread(r.heap,&thread),Status::Ok);same(initial,r.heap.stats());
    }
    {
        GuestThread thread;PPCContext context;
        CHECK_ST(create_guest_thread(r.heap,{0x10000,0,0},&context,&thread),Status::Ok);
        TESTDOUBLE_fail_once=true;TESTDOUBLE_allocations_until_failure=0;
        Status status=destroy_guest_thread(r.heap,&thread);
        TESTDOUBLE_allocations_until_failure=-1;TESTDOUBLE_fail_once=false;
        CHECK_ST(status,Status::OutOfMemory);CHECK(thread.tls);CHECK_EQ(thread.alloc_base,0u);CHECK_EQ(thread.pcr,0u);
        CHECK_EQ(r.heap.stats().live_allocations,1u);CHECK_ST(destroy_guest_thread(r.heap,&thread),Status::Ok);same(initial,r.heap.stats());
    }
    for(int point=0;point<3;++point) {
        fprintf(stderr,"TESTDOUBLE pool failure point %d\n",point);
        alignas(64) PPCContext c{}; c.r3.u64=32; c.r4.u64=0x74657374;
        TESTDOUBLE_allocations_until_failure=point;
        pool_impl(c,mem.base());
        TESTDOUBLE_allocations_until_failure=-1;
        CHECK_EQ(c.r3.u32,0u); CHECK(r.pool_allocations.empty()); same(initial,r.heap.stats());
    }
    // Recovery following every staged failure proves ownership locks release.
    alignas(64) PPCContext c{}; c.r3.u64=32; c.r4.u64=0x74657374;
    __imp__ExAllocatePoolTypeWithTag(c,mem.base()); CHECK(c.r3.u32);
    __imp__ExFreePool(c,mem.base()); same(initial,r.heap.stats());
    constexpr uint32_t outputs=0x30000000;
    CHECK(mem.commit(outputs,0x10000,Protect::ReadWrite)==MemStatus::Ok);
    for(int point=0;point<3;++point) {
        fprintf(stderr,"TESTDOUBLE VA/physical failure point %d\n",point);
        CHECK(guest_write_be32(outputs,0)); CHECK(guest_write_be32(outputs+4,0x10000));
        c={}; c.r3.u64=outputs; c.r4.u64=outputs+4; c.r5.u64=0x3000; c.r6.u64=4;
        TESTDOUBLE_allocations_until_failure=point;
        va_impl(c,mem.base());
        TESTDOUBLE_allocations_until_failure=-1;
        CHECK_EQ(c.r3.u32,nt::kNoMemory); CHECK(r.virtual_allocations.empty()); same(initial,r.heap.stats());
        uint32_t base=1,size=0; CHECK(guest_read_be32(outputs,&base)); CHECK(guest_read_be32(outputs+4,&size));
        CHECK_EQ(base,0u); CHECK_EQ(size,0x10000u);
        c={}; c.r4.u64=0x10000; c.r5.u64=4;
        TESTDOUBLE_allocations_until_failure=point;
        physical_impl(c,mem.base());
        TESTDOUBLE_allocations_until_failure=-1;
        CHECK_EQ(c.r3.u32,0u); CHECK(r.physical_allocations.empty());
        same(physical_initial,r.physical.stats());
    }
    runtime_shutdown();
    mem.release();
    // Persistent host OOM at every module-bootstrap allocation. The prepared
    // guest block is retained by Runtime if even rollback bookkeeping cannot
    // allocate; no variable binding survives a refused preparation.
    ModuleConfig module;module.guest_path="game:\\original-module.xex";module.command_line="original --test";
    bool completed=false;int module_failures=0;
    for(int point=0;point<64;++point) {
        CHECK(mem.reserve()==MemStatus::Ok);CHECK_ST(runtime_init(&mem,cfg),Status::Ok);clear_imports();
        TESTDOUBLE_allocations_until_failure=point;
        Status status=runtime_prepare_main_module(module);
        TESTDOUBLE_allocations_until_failure=-1;
        if(status==Status::Ok) {
            completed=true;CHECK_EQ(runtime()->heap.stats().live_allocations,1u);
        } else {
            CHECK_ST(status,Status::OutOfMemory);++module_failures;
            uint32_t address=0;
            for(uint32_t ordinal:{0x193,0x59,0x266,0x1AE})CHECK(!find_variable_import(kModuleXboxkrnl,ordinal,&address));
            const auto allocations=runtime()->heap.stats().live_allocations;
            CHECK(allocations==0 || (allocations==1 && runtime()->modules));
        }
        runtime_shutdown();mem.release();
        if(completed)break;
    }
    CHECK(completed);CHECK(module_failures>=8);
    fprintf(stderr,"module bootstrap host OOM: %d failure points checked\n",module_failures);
    completed=false;int finalize_failures=0;
    for(int point=0;point<64;++point) {
        CHECK(mem.reserve()==MemStatus::Ok);CHECK_ST(runtime_init(&mem,cfg),Status::Ok);clear_imports();
        CHECK_ST(runtime_prepare_main_module(module),Status::Ok);
        auto image=original_module(mem);const auto prepared=runtime()->heap.stats();
        uint32_t handle_cell=0;CHECK(find_variable_import(kModuleXboxkrnl,0x193,&handle_cell));
        TESTDOUBLE_allocations_until_failure=point;
        Status status=runtime_finalize_main_module(image);
        TESTDOUBLE_allocations_until_failure=-1;
        if(status==Status::Ok) {
            completed=true;CHECK_EQ(runtime()->heap.stats().live_allocations,2u);
        } else {
            CHECK_ST(status,Status::OutOfMemory);++finalize_failures;same(prepared,runtime()->heap.stats());
            uint32_t value=1;CHECK(guest_read_be32(handle_cell,&value));CHECK_EQ(value,0u);
            // No partial header/handle publication, and retry after recovery.
            CHECK_ST(runtime_finalize_main_module(image),Status::Ok);
        }
        runtime_shutdown();mem.release();
        if(completed)break;
    }
    CHECK(completed);CHECK(finalize_failures>=4);
    fprintf(stderr,"module finalize host OOM: %d failure points checked\n",finalize_failures);
    return test_result("rt_heap_failures");
}
