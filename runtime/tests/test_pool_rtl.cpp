// Production imports, ownership and guest protection, without a mock allocator.
#include <atomic>
#include <thread>
#include <vector>
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__ExAllocatePoolTypeWithTag);
PPC_EXTERN_FUNC(__imp__ExAllocatePool);
PPC_EXTERN_FUNC(__imp__ExFreePool);
PPC_EXTERN_FUNC(__imp__RtlCompareMemoryUlong);
PPC_EXTERN_FUNC(__imp__RtlCompareMemory);
PPC_EXTERN_FUNC(__imp__RtlFillMemoryUlong);
PPC_EXTERN_FUNC(__imp__RtlCompareStringN);
PPC_EXTERN_FUNC(__imp__RtlInitAnsiString);
PPC_EXTERN_FUNC(__imp__RtlInitUnicodeString);
PPC_EXTERN_FUNC(__imp__NtAllocateVirtualMemory);
PPC_EXTERN_FUNC(__imp__NtFreeVirtualMemory);
PPC_EXTERN_FUNC(__imp__MmAllocatePhysicalMemory);
PPC_EXTERN_FUNC(__imp__MmFreePhysicalMemory);

using namespace rcomp;
using namespace rcomp::rt;
namespace {
GuestMemory mem;
constexpr uint32_t scratch = 0x30000000, pattern = 0x12345678;
constexpr uint32_t buffer = scratch + 0x100, descriptor = scratch + 0x200;
constexpr uint32_t boundary = scratch + 0x10000;
uint32_t call(PPCFunc* fn, uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0,uint32_t e=0) {
    alignas(64) PPCContext context{};
    context.r3.u64=a; context.r4.u64=b; context.r5.u64=c; context.r6.u64=d; context.r7.u64=e;
    fn(context,mem.base()); return context.r3.u32;
}
uint32_t word(uint32_t p) { uint32_t v=0; CHECK(guest_read_be32(p,&v)); return v; }
void put(uint32_t p,uint32_t v) { CHECK(guest_write_be32(p,v)); }
uint32_t pool(uint32_t size) { return call(__imp__ExAllocatePoolTypeWithTag,size,0x74657374,0); }
void pool_free(uint32_t p) { call(__imp__ExFreePool,p); }
void setup() {
    RuntimeConfig cfg; cfg.heap_lo=0x40000000; cfg.heap_hi=0x40100000;
    CHECK_ST(runtime_init(&mem,cfg),Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(),Status::Ok);
}
void unchanged(const GuestHeapStats& before) {
    const auto after=runtime()->heap.stats();
    CHECK_EQ(after.allocated_bytes,before.allocated_bytes);
    CHECK_EQ(after.free_bytes,before.free_bytes);
    CHECK_EQ(after.live_allocations,before.live_allocations);
}
void allocator() {
    const auto initial=runtime()->heap.stats();
    const uint32_t a=pool(1),b=pool(0x1000),c=pool(0xFD9);
    CHECK(a && b && c); CHECK_EQ(a&15,0u); CHECK_EQ(b&4095,0u); CHECK_EQ(c&4095,0u);
    CHECK(mem.is_accessible(a,1,Protect::ReadWrite));
    CHECK(mem.is_accessible(b,0x1000,Protect::ReadWrite));
    memset(mem.base()+b,0xAC,0x1000);  // allocation is genuinely writable
    CHECK_EQ(runtime()->pool_allocations.size(),3u);
    CHECK_EQ(pool(0),0u); CHECK_EQ(pool(UINT32_MAX),0u);
    bool fatal=false;
    CAPTURE_FATAL(call(__imp__ExAllocatePoolTypeWithTag,16,0,1),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(pool_free(b+16),fatal); CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CAPTURE_FATAL(pool_free(0),fatal); CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    uint32_t private_block=0;
    CHECK_ST(runtime()->heap.alloc(64,16,false,&private_block),Status::Ok);
    CAPTURE_FATAL(pool_free(private_block),fatal); CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CHECK_ST(runtime()->heap.free(private_block),Status::Ok);
    pool_free(b); CAPTURE_FATAL(pool_free(b),fatal); CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CHECK_EQ(pool(0x1000),b); pool_free(b); pool_free(a); pool_free(c);
    unchanged(initial); CHECK(runtime()->pool_allocations.empty());
    // ExAllocatePool is the default pool with the tag 'None', freed by ExFreePool.
    const uint32_t plain=call(__imp__ExAllocatePool,0x20,0xFFFFFFFF,0xFFFFFFFF);
    CHECK(plain && (plain&15)==0 && mem.is_accessible(plain,0x20,Protect::ReadWrite));
    CHECK_EQ(runtime()->pool_allocations.size(),1u);
    CHECK_EQ(runtime()->pool_allocations.front().tag,0x656E6F4Eu);
    CHECK_EQ(call(__imp__ExAllocatePool,0),0u);
    pool_free(plain); unchanged(initial); CHECK(runtime()->pool_allocations.empty());
    const uint32_t whole=pool(0x100000); CHECK(whole);
    auto full=runtime()->heap.stats(); CHECK_EQ(pool(16),0u); unchanged(full);
    CHECK_EQ(runtime()->pool_allocations.size(),1u); pool_free(whole); unchanged(initial);
    // Freed pages retained by the heap may have been protected externally.
    const uint32_t protected_block=pool(0x10000); pool_free(protected_block);
    CHECK(mem.protect(protected_block,0x10000,Protect::Read)==MemStatus::Ok);
    CHECK_EQ(pool(16),0u); unchanged(initial); CHECK(runtime()->pool_allocations.empty());
    CHECK(mem.protect(protected_block,0x10000,Protect::None)==MemStatus::Ok);
    CHECK_EQ(pool(16),0u); unchanged(initial);
    CHECK(mem.protect(protected_block,0x10000,Protect::ReadWrite)==MemStatus::Ok);
    const auto recovered=pool(16); CHECK(recovered); pool_free(recovered);
}
void memory_operations() {
    memset(mem.base()+buffer-4,0xCC,24);
    call(__imp__RtlFillMemoryUlong,buffer,16,pattern);
    const uint8_t expected[]={0x12,0x34,0x56,0x78};
    for(int i=0;i<16;++i) CHECK_EQ(mem.base()[buffer+i],expected[i%4]);
    CHECK_EQ(word(buffer-4),0xCCCCCCCCu); CHECK_EQ(word(buffer+16),0xCCCCCCCCu);
    CHECK_EQ(call(__imp__RtlCompareMemoryUlong,buffer,16,pattern),16u);
    put(buffer+4,0x12345679);
    CHECK_EQ(call(__imp__RtlCompareMemoryUlong,buffer,16,pattern),4u);
    put(buffer,0x02345678); CHECK_EQ(call(__imp__RtlCompareMemoryUlong,buffer,16,pattern),0u);
    CHECK_EQ(call(__imp__RtlCompareMemoryUlong,buffer+1,16,pattern),0u);
    CHECK_EQ(call(__imp__RtlCompareMemoryUlong,buffer,15,pattern),0u);
    // RtlCompareMemory: leading equal bytes, any alignment and length.
    memcpy(mem.base()+buffer+64,"abcdefgh",8); memcpy(mem.base()+buffer+80,"abcdXfgh",8);
    CHECK_EQ(call(__imp__RtlCompareMemory,buffer+64,buffer+80,8),4u);
    CHECK_EQ(call(__imp__RtlCompareMemory,buffer+65,buffer+81,3),3u);
    CHECK_EQ(call(__imp__RtlCompareMemory,buffer+64,buffer+64,8),8u);
    CHECK_EQ(call(__imp__RtlCompareMemory,buffer+64,buffer+80,0),0u);
    CHECK_EQ(call(__imp__RtlCompareMemoryUlong,0,0,pattern),0u);
    call(__imp__RtlFillMemoryUlong,0,0,pattern);
    bool fatal=false;
    CAPTURE_FATAL(call(__imp__RtlFillMemoryUlong,buffer,7,pattern),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CHECK_EQ(word(buffer),0x02345678u);
    put(boundary-4,0xAABBCCDD);
    CHECK(mem.protect(boundary,0x10000,Protect::Read)==MemStatus::Ok);
    CAPTURE_FATAL(call(__imp__RtlFillMemoryUlong,boundary-4,8,pattern),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS); CHECK_EQ(word(boundary-4),0xAABBCCDDu);
    CHECK(mem.protect(boundary,0x10000,Protect::None)==MemStatus::Ok);
    CAPTURE_FATAL(call(__imp__RtlCompareMemoryUlong,boundary,4,pattern),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CHECK(mem.protect(boundary,0x10000,Protect::ReadWrite)==MemStatus::Ok);
    // Back the end-of-space alias through its canonical physical window;
    // aliases cannot own independent commits. Exercise a valid production
    // fill there before keeping the original 32-bit overflow request below.
    constexpr uint32_t canonical_end = 0xBFFFFFFC;
    CHECK(mem.commit(0xBFFF0000,0x10000,Protect::ReadWrite)==MemStatus::Ok);
    call(__imp__RtlFillMemoryUlong,canonical_end,4,pattern);
    CHECK_EQ(word(canonical_end),pattern);
    put(canonical_end,0xCAFEBABE);
    CAPTURE_FATAL(call(__imp__RtlFillMemoryUlong,0xFFFFFFFC,8,pattern),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CHECK(g_fatal_msg.find("address=0xFFFFFFFC length=0x8") != std::string::npos);
    CHECK_EQ(word(canonical_end),0xCAFEBABEu);
    if (!mem.is_accessible(0xFFFFFFFC,4,Protect::ReadWrite))
        std::puts("rt_pool_rtl: host physical alias NOT TESTED (vm_alias ENOSYS)");
}
void strings() {
    memcpy(mem.base()+buffer,"MiXeD",6);
    call(__imp__RtlInitAnsiString,descriptor,buffer);
    CHECK_EQ(word(descriptor),0x00050006u); CHECK_EQ(word(descriptor+4),buffer);
    CHECK(memcmp(mem.base()+buffer,"MiXeD",6)==0);
    const uint8_t unicode[]={0,0x41,3,0xA9,0,0x62,0,0};
    memcpy(mem.base()+buffer+16,unicode,8);
    CHECK_EQ(call(__imp__RtlInitUnicodeString,descriptor,buffer+16),descriptor);
    CHECK_EQ(word(descriptor),0x00060008u); CHECK_EQ(word(descriptor+4),buffer+16);
    for(auto fn : {__imp__RtlInitAnsiString,__imp__RtlInitUnicodeString}) {
        call(fn,descriptor,0); CHECK_EQ(word(descriptor),0u); CHECK_EQ(word(descriptor+4),0u);
    }
    memcpy(mem.base()+buffer+32,"mixed",6);
    CHECK_EQ(call(__imp__RtlCompareStringN,buffer,5,buffer+32,5,1),0u);
    CHECK_EQ(call(__imp__RtlCompareStringN,buffer,5,buffer+32,5,0),(uint32_t)('M'-'m'));
    CHECK_EQ(call(__imp__RtlCompareStringN,buffer,3,buffer,5,0),(uint32_t)-2);
    CHECK_EQ(call(__imp__RtlCompareStringN,buffer,UINT32_MAX,buffer+32,5,1),0u);
    CHECK_EQ(call(__imp__RtlCompareStringN,0,0,0,0,0),0u);
    mem.base()[buffer]=0; mem.base()[buffer+32]=0;
    mem.base()[buffer+1]='b'; mem.base()[buffer+33]='a';
    CHECK_EQ(call(__imp__RtlCompareStringN,buffer,2,buffer+32,2,0),1u); // counted embedded NUL
    mem.base()[buffer]=0xE9; mem.base()[buffer+32]=0xC9;
    CHECK_EQ(call(__imp__RtlCompareStringN,buffer,1,buffer+32,1,1),0u);
    mem.base()[buffer]=0xFF; mem.base()[buffer+32]='?';
    CHECK_EQ(call(__imp__RtlCompareStringN,buffer,1,buffer+32,1,1),0u);
    mem.base()[buffer]=0xF7; mem.base()[buffer+32]=0xD7;
    CHECK_EQ(call(__imp__RtlCompareStringN,buffer,1,buffer+32,1,1),0x20u);
    // Same source/destination overlap is observed before descriptor mutation.
    memcpy(mem.base()+descriptor,"ABC",4);
    call(__imp__RtlInitAnsiString,descriptor,descriptor); CHECK_EQ(word(descriptor),0x00030004u);
    // Last readable bytes are terminated: scanner cannot overread the page.
    mem.base()[boundary-2]='X'; mem.base()[boundary-1]=0;
    CHECK(mem.protect(boundary,0x10000,Protect::None)==MemStatus::Ok);
    call(__imp__RtlInitAnsiString,descriptor,boundary-2); CHECK_EQ(word(descriptor),0x00010002u);
    put(descriptor,0xDEADBEEF); put(descriptor+4,0xBAADF00D);
    mem.base()[boundary-1]='X';
    bool fatal=false;
    CAPTURE_FATAL(call(__imp__RtlInitAnsiString,descriptor,boundary-2),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CHECK_EQ(word(descriptor),0xDEADBEEFu); CHECK_EQ(word(descriptor+4),0xBAADF00Du);
    CAPTURE_FATAL(call(__imp__RtlInitUnicodeString,descriptor,boundary-1),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CHECK_EQ(word(descriptor),0xDEADBEEFu);
    CAPTURE_FATAL(call(__imp__RtlCompareStringN,boundary,1,buffer,1,0),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CHECK(mem.protect(boundary,0x10000,Protect::ReadWrite)==MemStatus::Ok);
    put(boundary-4,0x55AA55AA);
    CHECK(mem.protect(boundary,0x10000,Protect::Read)==MemStatus::Ok);
    CAPTURE_FATAL(call(__imp__RtlInitAnsiString,boundary-4,0),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS); CHECK_EQ(word(boundary-4),0x55AA55AAu);
    CHECK(mem.protect(boundary,0x10000,Protect::ReadWrite)==MemStatus::Ok);
    // 16-bit descriptor lengths saturate without wrapping.
    const uint32_t large=scratch+0x20000;
    memset(mem.base()+large,'a',0x10000); mem.base()[large+0x10000]=0;
    call(__imp__RtlInitAnsiString,descriptor,large); CHECK_EQ(word(descriptor),0xFFFEFFFFu);
    call(__imp__RtlInitUnicodeString,descriptor,large); CHECK_EQ(word(descriptor),0xFFFCFFFEu);
    // 0xFFFF is an ordinary explicit length, not the sentinel from old Xenia.
    CHECK_EQ(call(__imp__RtlCompareStringN,large,0xFFFF,large,0xFFFE,0),1u);
}
void lifecycle() {
    // Both output words must be RW before allocating/freeing. Reject a range
    // that crosses into Read/None without consuming memory or ownership.
    for(Protect rights : {Protect::Read,Protect::None}) {
        put(boundary,0); put(boundary+4,0x10000);
        const auto initial=runtime()->heap.stats();
        CHECK(mem.protect(boundary,0x10000,rights)==MemStatus::Ok);
        CHECK_EQ(call(__imp__NtAllocateVirtualMemory,boundary,boundary+4,0x3000,4,0),nt::kAccessViolation);
        unchanged(initial); CHECK(runtime()->virtual_allocations.empty());
        CHECK(mem.protect(boundary,0x10000,Protect::ReadWrite)==MemStatus::Ok);
        CHECK_EQ(word(boundary),0u); CHECK_EQ(word(boundary+4),0x10000u);
        CHECK_EQ(call(__imp__NtAllocateVirtualMemory,boundary,boundary+4,0x3000,4,0),0u);
        const uint32_t base=word(boundary); put(boundary+4,0);
        const auto live=runtime()->heap.stats();
        CHECK(mem.protect(boundary,0x10000,rights)==MemStatus::Ok);
        CHECK_EQ(call(__imp__NtFreeVirtualMemory,boundary,boundary+4,0x8000,0),nt::kAccessViolation);
        unchanged(live); CHECK(runtime()->virtual_allocations.count(base));
        CHECK(mem.protect(boundary,0x10000,Protect::ReadWrite)==MemStatus::Ok);
        CHECK_EQ(word(boundary),base); CHECK_EQ(word(boundary+4),0u);
        CHECK_EQ(call(__imp__NtFreeVirtualMemory,boundary,boundary+4,0x8000,0),0u);
        unchanged(initial);
        // The first output is writable: rejection must come from the second.
        put(scratch,0); put(boundary,0x10000);
        CHECK(mem.protect(boundary,0x10000,rights)==MemStatus::Ok);
        CHECK_EQ(call(__imp__NtAllocateVirtualMemory,scratch,boundary,0x3000,4,0),nt::kAccessViolation);
        unchanged(initial); CHECK_EQ(word(scratch),0u);
        CHECK(mem.protect(boundary,0x10000,Protect::ReadWrite)==MemStatus::Ok);
        CHECK_EQ(call(__imp__NtAllocateVirtualMemory,scratch,boundary,0x3000,4,0),0u);
        const uint32_t second_base=word(scratch); put(boundary,0);
        CHECK(mem.protect(boundary,0x10000,rights)==MemStatus::Ok);
        CHECK_EQ(call(__imp__NtFreeVirtualMemory,scratch,boundary,0x8000,0),nt::kAccessViolation);
        CHECK(runtime()->virtual_allocations.count(second_base)); CHECK_EQ(word(scratch),second_base);
        CHECK(mem.protect(boundary,0x10000,Protect::ReadWrite)==MemStatus::Ok);
        CHECK_EQ(word(boundary),0u);
        CHECK_EQ(call(__imp__NtFreeVirtualMemory,scratch,boundary,0x8000,0),0u);
        unchanged(initial);
    }
    // Unfreed Nt ownership from an old title must not free a new pool block.
    put(scratch,0); put(scratch+4,0x10000);
    CHECK_EQ(call(__imp__NtAllocateVirtualMemory,scratch,scratch+4,0x3000,4,0),0u);
    const uint32_t old_virtual=word(scratch);
    const uint32_t old_physical=call(__imp__MmAllocatePhysicalMemory,0,0x10000,4);
    CHECK(old_physical);
    const uint32_t old_pool=pool(32); CHECK(old_pool);
    runtime_shutdown(); setup();
    CHECK(runtime()->pool_allocations.empty());
    const uint32_t fresh=pool(0x10000); CHECK_EQ(fresh,old_virtual);
    put(scratch,fresh); put(scratch+4,0);
    CHECK_EQ(call(__imp__NtFreeVirtualMemory,scratch,scratch+4,0x8000,0),nt::kMemoryNotAllocated);
    CHECK_EQ(runtime()->heap.stats().live_allocations,1u);
    bool fatal=false;
    CAPTURE_FATAL(pool_free(old_pool),fatal); CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    uint32_t phys=0;
    CHECK_ST(runtime()->physical.alloc_in(0x10000,0x10000,old_physical,(uint64_t)old_physical+0x10000,
                                         false,false,&phys),Status::Ok);
    CHECK_EQ(phys,old_physical);
    CAPTURE_FATAL(call(__imp__MmFreePhysicalMemory,0,phys),fatal);
    CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    uint32_t size=0; CHECK_ST(runtime()->physical.allocation_size(phys,&size),Status::Ok);
    CHECK_ST(runtime()->physical.free(phys),Status::Ok);
    pool_free(fresh);
}
void concurrency() {
    std::atomic<unsigned> failures{0}; std::vector<std::thread> workers;
    for(unsigned t=0;t<4;++t) workers.emplace_back([&failures] {
        for(unsigned i=0;i<128;++i) {
            const uint32_t a=pool(64);
            if (!a) { ++failures; continue; }
            memset(mem.base()+a,0xCE,64);
            pool_free(a);
        }
    });
    for(auto& t:workers) t.join();
    CHECK_EQ(failures.load(),0u); CHECK(runtime()->pool_allocations.empty());
    CHECK_EQ(runtime()->heap.stats().live_allocations,0u);
}
} // namespace
int main() {
    if(mem.reserve()!=MemStatus::Ok) return 2;
    if(mem.commit(scratch,0x40000,Protect::ReadWrite)!=MemStatus::Ok) return 2;
    setup();
    allocator(); memory_operations(); strings(); lifecycle(); concurrency();
    runtime_shutdown();
    return test_result("rt_pool_rtl");
}
