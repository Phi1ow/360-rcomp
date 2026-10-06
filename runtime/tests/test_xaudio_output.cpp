#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include "TESTDOUBLE_audio_output.h"
#include "rcomp/audio_output.h"
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/xaudio.h"
#include "test_util.h"
using namespace rcomp; using namespace rcomp::rt;
namespace {
constexpr uint32_t scratch=0x30000000, frame=scratch+0x1000;
std::atomic<unsigned> callbacks{0}, invalid{0}, thread_id{0}, thread_changes{0};
std::atomic<uint32_t> argument_seen{0}, last_wrapper{0};
std::atomic<bool> terminate_once{false};
std::atomic<bool> self_unregister_once{false}, self_shutdown_once{false};
std::atomic<uint32_t> self_driver{0}, self_unregister_result{~0u};
std::atomic<bool> shutdown_guard{false};
uint32_t invoke(unsigned ordinal,uint32_t a,uint32_t b=0);
void TESTDOUBLE_guest_audio(PPCContext& c,uint8_t*) {
    GuestThread* guest=current_guest_thread(); uint32_t value=0;
    if(!guest || thread_object_exited(guest->identity) || !guest_read_be32(c.r3.u32,&value)) { ++invalid; return; }
    argument_seen=value; last_wrapper=c.r3.u32;
    const unsigned id=guest->thread_id, old=thread_id.exchange(id);
    if(old && old!=id)++thread_changes;
    ++guest->tls_values[0]; ++callbacks;
    if(self_unregister_once.exchange(false))self_unregister_result=invoke(0x1F4,self_driver.load());
    if(self_shutdown_once.exchange(false)) {
        bool caught=false;
        CAPTURE_FATAL(shutdown_xaudio(),caught);
        shutdown_guard=caught && g_fatal_kind==RCOMP_FATAL_INTERNAL && g_fatal_msg.find("own callback thread")!=std::string::npos;
    }
    if(terminate_once.exchange(false))exit_current_guest_thread(0x55);
}
uint32_t invoke(unsigned ordinal,uint32_t a,uint32_t b) {
    PPCContext c{}; c.r3.u64=a; c.r4.u64=b;
    PPCFunc* f=find_import(kModuleXboxkrnl,ordinal);
    if(!f) { CHECK(f); return 0xFFFFFFFF; }
    f(c,runtime()->mem->base()); return c.r3.u32;
}
uint32_t read(uint32_t a) { uint32_t v=0; CHECK(guest_read_be32(a,&v)); return v; }
void setup_client(uint32_t at,uint32_t value) { CHECK(guest_write_be32(at,0x82000100)); CHECK(guest_write_be32(at+4,value)); }
bool wait_callbacks(unsigned target) {
    for(unsigned n=0;n<100;++n) { if(callbacks.load()>=target)return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    return false;
}
void fill_frame(float base) {
    for(unsigned ch=0;ch<6;++ch)for(unsigned n=0;n<256;++n) {
        float value=base+float(ch*100)+float(n)/1024; uint32_t bits; std::memcpy(&bits,&value,4);
        CHECK(guest_write_be32(frame+(ch*256+n)*4,bits));
    }
}
}
int main() {
    TESTDOUBLE_audio::reset();
    GuestMemory mem; CHECK_EQ(mem.reserve(),MemStatus::Ok); CHECK_EQ(mem.commit(scratch,0x10000,Protect::ReadWrite),MemStatus::Ok);
    CHECK_ST(runtime_init(&mem),Status::Ok); CHECK_ST(register_xboxkrnl_xaudio_hle(),Status::Ok);
    FuncEntry f{0x82000100,TESTDOUBLE_guest_audio,"TESTDOUBLE_guest_audio"}; CHECK(register_functions(&f,1));
    setup_client(scratch,0x76543210); CHECK(guest_write_be32(scratch+16,0xA5A5A5A5));
    const auto baseline=runtime()->heap.stats().live_allocations;
    CHECK_EQ(invoke(0x1F3,scratch,scratch+16),0x80004001u);
    CHECK_EQ(read(scratch+16),0xA5A5A5A5u); CHECK_EQ(callbacks.load(),0u);
    CHECK_EQ(runtime()->heap.stats().live_allocations,baseline);
    TESTDOUBLE_audio::set_open_result(RCOMP_AUDIO_DEVICE_ERROR);
    CHECK_EQ(invoke(0x1F3,scratch,scratch+16),0x80004005u);
    CHECK_EQ(runtime()->heap.stats().live_allocations,baseline);
    TESTDOUBLE_audio::set_open_result(RCOMP_AUDIO_OK);
    setup_client(scratch+32,0x12345678);
    std::array<uint32_t,2> results;
    std::thread first([&] { results[0]=invoke(0x1F3,scratch,scratch+16); });
    std::thread second([&] { results[1]=invoke(0x1F3,scratch+32,scratch+48); });
    first.join(); second.join(); CHECK_EQ(results[0],0u); CHECK_EQ(results[1],0u); CHECK_EQ(TESTDOUBLE_audio::opened(),2u);
    const uint32_t driver=read(scratch+16), other=read(scratch+48);
    CHECK(driver!=other); CHECK(wait_callbacks(8)); CHECK_EQ(invalid.load(),0u); CHECK_EQ(thread_changes.load(),0u);
    CHECK(last_wrapper.load()!=scratch && last_wrapper.load()!=scratch+32);
    CHECK(argument_seen.load()==0x76543210 || argument_seen.load()==0x12345678);
    self_driver=driver; self_unregister_once=true;
    const unsigned before_self=callbacks.load(); CHECK(wait_callbacks(before_self+4));
    CHECK_EQ(self_unregister_result.load(),0x800700AAu); CHECK_EQ(TESTDOUBLE_audio::closed(),0u);
    self_shutdown_once=true; CHECK(wait_callbacks(callbacks.load()+4)); CHECK(shutdown_guard.load());
    CHECK_EQ(invoke(0x1F5,driver,frame+0x10000),0x80070057u);
    fill_frame(0.25f); CHECK_EQ(invoke(0x1F5,driver,frame),0u);
    auto accepted=TESTDOUBLE_audio::frames(); CHECK_EQ(accepted.size(),1u);
    if(!accepted.empty())for(unsigned n=0;n<256;++n)for(unsigned ch=0;ch<6;++ch)
        CHECK_EQ(accepted[0][n*6+ch],0.25f+float(ch*100)+float(n)/1024);
    fill_frame(900.0f); accepted=TESTDOUBLE_audio::frames();
    if(!accepted.empty())CHECK_EQ(accepted[0][0],0.25f);
    for(unsigned n=0;n<7;++n)CHECK_EQ(invoke(0x1F5,driver,frame),0u);
    CHECK_EQ(TESTDOUBLE_audio::queued(),8u); CHECK_EQ(invoke(0x1F5,driver,frame),0x800700AAu);
    CHECK_EQ(TESTDOUBLE_audio::queued(),8u); TESTDOUBLE_audio::drain();
    TESTDOUBLE_audio::set_submit_result(RCOMP_AUDIO_DEVICE_ERROR);
    CHECK_EQ(invoke(0x1F5,driver,frame),0x80004005u); CHECK_EQ(TESTDOUBLE_audio::queued(),0u);
    TESTDOUBLE_audio::set_submit_result(RCOMP_AUDIO_OK);
    TESTDOUBLE_audio::block_submit(true); fill_frame(17.0f);
    uint32_t submitted=~0u, unregistered=~0u;
    std::thread submitter([&] { submitted=invoke(0x1F5,driver,frame); });
    CHECK(TESTDOUBLE_audio::wait_submit_entered());
    // Mutation cannot affect the converted host-owned copy handed to submit.
    fill_frame(44.0f);
    std::thread unregisterer([&] { unregistered=invoke(0x1F4,driver); });
    std::this_thread::sleep_for(std::chrono::milliseconds(15)); CHECK_EQ(TESTDOUBLE_audio::closed(),0u);
    TESTDOUBLE_audio::block_submit(false); submitter.join(); unregisterer.join();
    CHECK_EQ(submitted,0u); CHECK_EQ(unregistered,0u); CHECK_EQ(TESTDOUBLE_audio::closed(),1u);
    accepted=TESTDOUBLE_audio::frames(); CHECK(!accepted.empty()); if(!accepted.empty())CHECK_EQ(accepted.back()[0],17.0f);
    CHECK_EQ(invoke(0x1F5,driver,frame),0x80070057u);
    TESTDOUBLE_audio::set_close_result(RCOMP_AUDIO_DEVICE_ERROR);
    CHECK_EQ(invoke(0x1F4,other),0x80004005u); CHECK_EQ(TESTDOUBLE_audio::closed(),1u);
    CHECK_EQ(invoke(0x1F5,other,frame),0x80004005u);
    TESTDOUBLE_audio::set_close_result(RCOMP_AUDIO_OK); CHECK_EQ(invoke(0x1F4,other),0u); CHECK_EQ(TESTDOUBLE_audio::closed(),2u);
    CHECK_EQ(invoke(0x1F4,other),0x80070057u);
    CHECK_EQ(invoke(0x1F3,scratch,scratch+16),0u);
    const unsigned previous=callbacks.load(); terminate_once=true; CHECK(wait_callbacks(previous+3));
    CHECK_EQ(thread_changes.load(),1u); CHECK_EQ(invalid.load(),0u);
    runtime_shutdown(); CHECK_EQ(TESTDOUBLE_audio::opened(),TESTDOUBLE_audio::closed());
    const unsigned stopped=callbacks.load(); std::this_thread::sleep_for(std::chrono::milliseconds(15)); CHECK_EQ(callbacks.load(),stopped);
    // The next runtime owns independently allocated wrappers/guest contexts.
    CHECK_ST(runtime_init(&mem),Status::Ok); CHECK_ST(register_xboxkrnl_xaudio_hle(),Status::Ok);
    setup_client(scratch,0x24681357); CHECK_EQ(invoke(0x1F3,scratch,scratch+16),0u);
    CHECK(wait_callbacks(stopped+2)); CHECK_EQ(argument_seen.load(),0x24681357u); runtime_shutdown();
    CHECK_EQ(TESTDOUBLE_audio::opened(),TESTDOUBLE_audio::closed()); clear_functions(); clear_imports();
    return test_result("rt_xaudio_output");
}
