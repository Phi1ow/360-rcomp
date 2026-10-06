// Real runtime provider must reject registration when the platform cannot
// open audio. This is the negative oracle against the previous fake-success.
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xaudio.h"
#include "TESTDOUBLE_audio_output.h"
#include "test_util.h"
using namespace rcomp; using namespace rcomp::rt;
void TESTDOUBLE_guest_no_audio(PPCContext& c,uint8_t*) { c.r3.u64=0; }
int main() {
    TESTDOUBLE_audio::reset(); GuestMemory mem;
    CHECK_EQ(mem.reserve(),MemStatus::Ok); CHECK_EQ(mem.commit(0x30000000,0x10000,Protect::ReadWrite),MemStatus::Ok);
    CHECK_ST(runtime_init(&mem),Status::Ok); CHECK_ST(register_xboxkrnl_xaudio_hle(),Status::Ok);
    FuncEntry entry{0x82000100,TESTDOUBLE_guest_no_audio,"TESTDOUBLE_guest_no_audio"}; CHECK(register_functions(&entry,1));
    CHECK(guest_write_be32(0x30000000,0x82000100)); CHECK(guest_write_be32(0x30000004,0));
    CHECK(guest_write_be32(0x30000008,0xA5A5A5A5));
    const auto before=runtime()->heap.stats().live_allocations;
    PPCContext c{}; c.r3.u64=0x30000000; c.r4.u64=0x30000008;
    PPCFunc* registration=find_import(kModuleXboxkrnl,0x1F3); CHECK(registration);
    if(registration)registration(c,mem.base());
    CHECK_EQ(c.r3.u32,0x80004001u);
    uint32_t driver=0; CHECK(guest_read_be32(0x30000008,&driver)); CHECK_EQ(driver,0xA5A5A5A5u);
    CHECK_EQ(runtime()->heap.stats().live_allocations,before);
    CHECK_EQ(TESTDOUBLE_audio::opened(),0u);
    runtime_shutdown(); clear_functions(); clear_imports();
    return test_result("rt_xaudio_no_backend");
}
