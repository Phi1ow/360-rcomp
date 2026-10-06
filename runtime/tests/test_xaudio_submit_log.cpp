#include <cstdio>
#include <string>
#include <unistd.h>
#include "TESTDOUBLE_audio_output.h"
#include "rcomp/audio_output.h"
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xaudio.h"
#include "test_util.h"
using namespace rcomp; using namespace rcomp::rt;
void TESTDOUBLE_submit_log_callback(PPCContext& c,uint8_t*) { c.r3.u64=0; }
unsigned occurrences(const std::string& text,const std::string& wanted) {
    unsigned count=0;
    for(size_t at=0;(at=text.find(wanted,at))!=std::string::npos;at+=wanted.size())++count;
    return count;
}
int main() {
    TESTDOUBLE_audio::reset(); TESTDOUBLE_audio::set_open_result(RCOMP_AUDIO_OK);
    GuestMemory memory; CHECK_EQ(memory.reserve(),MemStatus::Ok);
    CHECK_EQ(memory.commit(0x30000000,0x10000,Protect::ReadWrite),MemStatus::Ok);
    CHECK_ST(runtime_init(&memory),Status::Ok); CHECK_ST(register_xboxkrnl_xaudio_hle(),Status::Ok);
    FuncEntry entry{0x82000100,TESTDOUBLE_submit_log_callback,"TESTDOUBLE_submit_log_callback"}; CHECK(register_functions(&entry,1));
    CHECK(guest_write_be32(0x30000000,0x82000100)); CHECK(guest_write_be32(0x30000004,0));
    auto invoke=[&](unsigned ordinal,uint32_t a,uint32_t b) {
        PPCContext c{}; c.r3.u32=a; c.r4.u32=b;
        PPCFunc* function=find_import(kModuleXboxkrnl,ordinal); CHECK(function);
        if(function)function(c,memory.base()); return c.r3.u32;
    };
    auto driver=[&] { uint32_t value=0; CHECK(guest_read_be32(0x30000008,&value)); return value; };
    FILE* capture=std::tmpfile(); CHECK(capture);
    const int previous=dup(STDERR_FILENO); CHECK(previous>=0);
    if(!capture || previous<0) { runtime_shutdown(); return test_result("rt_xaudio_submit_log"); }
    CHECK_EQ(dup2(fileno(capture),STDERR_FILENO),STDERR_FILENO);
    CHECK_EQ(invoke(0x1F3,0x30000000,0x30000008),0u);
    const uint32_t first=driver();
    for(unsigned n=0;n<8;++n)CHECK_EQ(invoke(0x1F5,first,0x30001000),0u);
    for(unsigned n=0;n<40;++n)CHECK_EQ(invoke(0x1F5,first,0x30001000),0x800700AAu);
    CHECK_EQ(TESTDOUBLE_audio::queued(),8u);
    CHECK_EQ(invoke(0x1F4,first,0),0u);
    CHECK_EQ(invoke(0x1F3,0x30000000,0x30000008),0u);
    TESTDOUBLE_audio::set_submit_result(RCOMP_AUDIO_DEVICE_ERROR);
    for(unsigned n=0;n<12;++n)CHECK_EQ(invoke(0x1F5,driver(),0x30001000),0x80004005u);
    CHECK_EQ(TESTDOUBLE_audio::queued(),0u);
    CHECK_EQ(invoke(0x1F4,driver(),0),0u);
    CHECK_EQ(invoke(0x1F3,0x30000000,0x30000008),0u);
    for(unsigned n=0;n<12;++n)CHECK_EQ(invoke(0x1F5,driver(),0x30010000),0x80070057u);
    runtime_shutdown();
    std::fflush(stderr); CHECK_EQ(dup2(previous,STDERR_FILENO),STDERR_FILENO); close(previous);
    std::rewind(capture); std::string text; char buffer[512];
    while(const size_t n=std::fread(buffer,1,sizeof(buffer),capture))text.append(buffer,n);
    std::fclose(capture);
    CHECK_EQ(occurrences(text,"RCOMP-XAUDIO-SUBMIT "),3u);
    CHECK_EQ(occurrences(text,"hresult=0x800700AA backend_called=1 backend_result=3"),1u);
    CHECK_EQ(occurrences(text,"hresult=0x80004005 backend_called=1 backend_result=4"),1u);
    CHECK_EQ(occurrences(text,"hresult=0x80070057 backend_called=0"),1u);
    CHECK_EQ(occurrences(text,"hresult=0x00000000"),0u);
    CHECK_EQ(TESTDOUBLE_audio::opened(),TESTDOUBLE_audio::closed());
    clear_imports(); clear_functions(); return test_result("rt_xaudio_submit_log");
}
