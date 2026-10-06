#include <atomic>
#include <chrono>
#include <thread>
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/notifications.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

using namespace rcomp;
using namespace rcomp::rt;
PPC_EXTERN_FUNC(__imp__XamNotifyCreateListener);
PPC_EXTERN_FUNC(__imp__XNotifyGetNext);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__NtClose);
namespace {
GuestMemory memory;
constexpr uint32_t scratch=0x30000000;
uint32_t call(PPCFunc* function,uint64_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0) {
    PPCContext ctx{};ctx.r3.u64=a;ctx.r4.u64=b;ctx.r5.u64=c;ctx.r6.u64=d;
    function(ctx,memory.base());return ctx.r3.u32;
}
uint32_t read(unsigned offset) {uint32_t value=0;CHECK(guest_read_be32(scratch+offset,&value));return value;}
uint32_t next(uint32_t handle,uint32_t match=0) {return call(__imp__XNotifyGetNext,handle,match,scratch,scratch+4);}
uint32_t poll(uint32_t handle) {
    guest_write_be64(scratch+8,0);
    return call(__imp__NtWaitForSingleObjectEx,handle,0,0,scratch+8);
}
void initialize() {
    CHECK(memory.reserve()==MemStatus::Ok);
    CHECK(memory.commit(scratch,0x10000,Protect::ReadWrite)==MemStatus::Ok);
    CHECK_ST(runtime_init(&memory),Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(),Status::Ok);
    CHECK_ST(register_xam_notifications_hle(),Status::Ok);
}
}
int main() {
    initialize();
    const uint32_t listener=call(__imp__XamNotifyCreateListener,1,3);
    CHECK(listener!=0);CHECK_EQ(poll(listener),0x102u);
    guest_write_be32(scratch,0xAABBCCDD);guest_write_be32(scratch+4,0x55667788);
    CHECK_EQ(next(listener),0u);CHECK_EQ(read(0),0u);CHECK_EQ(read(4),0u);
    CHECK_ST(publish_xam_notification((2u<<25)|1,7),Status::Ok);
    CHECK_ST(publish_xam_notification((4u<<16)|1,8),Status::Ok);
    CHECK_EQ(next(listener),0u);
    const uint32_t one=(1u<<16)|10,two=(3u<<16)|11;
    CHECK_ST(publish_xam_notification(one,0x11223344),Status::Ok);
    CHECK_ST(publish_xam_notification(two,0xFFEEDDCC),Status::Ok);
    CHECK_EQ(poll(listener),0u);CHECK_EQ(poll(listener),0u);
    CHECK_EQ(next(listener,1234),0u);CHECK_EQ(poll(listener),0u);
    CHECK_EQ(next(listener,two),1u);CHECK_EQ(read(0),two);CHECK_EQ(read(4),0xFFEEDDCCu);
    CHECK_EQ(next(listener),1u);CHECK_EQ(read(0),one);CHECK_EQ(read(4),0x11223344u);
    CHECK_EQ(poll(listener),0x102u);
    const uint32_t high=call(__imp__XamNotifyCreateListener,uint64_t(1)<<63,0);
    CHECK(high!=0);CHECK_ST(publish_xam_notification((63u<<25)|1,123),Status::Ok);
    CHECK_EQ(next(listener),0u);CHECK_EQ(next(high),1u);CHECK_EQ(read(4),123u);
    CHECK_ST(publish_xam_notification(one,55),Status::Ok);
    bool fatal=false;
    CAPTURE_FATAL(call(__imp__XNotifyGetNext,listener,0,scratch+0xFFFF,scratch+4),fatal);
    CHECK(fatal&&g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CHECK_EQ(next(listener),1u);CHECK_EQ(read(4),55u);
    CHECK_ST(publish_xam_notification(one,99),Status::Ok);
    CHECK_EQ(call(__imp__XNotifyGetNext,listener,0,0,scratch+4),0u);CHECK_EQ(poll(listener),0u);
    CHECK_EQ(call(__imp__XNotifyGetNext,listener,0,scratch,0),1u);
    CHECK_EQ(read(0),one);CHECK_EQ(poll(listener),0x102u);
    std::atomic<bool> started{false};
    std::thread producer([&]{
        while(!started.load()) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK_ST(publish_xam_notification(one,73),Status::Ok);
    });
    guest_write_be64(scratch+8,uint64_t(0)-10000000ull);started=true;
    CHECK_EQ(call(__imp__NtWaitForSingleObjectEx,listener,0,0,scratch+8),0u);
    producer.join();CHECK_EQ(next(listener),1u);CHECK_EQ(read(4),73u);
    uint32_t duplicate=0;
    CHECK_ST(runtime()->handles.duplicate(listener,&duplicate),Status::Ok);
    CHECK_EQ(call(__imp__NtClose,listener),0u);
    CHECK_ST(publish_xam_notification(one,81),Status::Ok);
    CHECK_EQ(next(listener),0u);CHECK_EQ(next(duplicate),1u);CHECK_EQ(read(4),81u);
    CHECK_EQ(call(__imp__NtClose,duplicate),0u);CHECK_EQ(call(__imp__NtClose,high),0u);
    CHECK_EQ(runtime()->handles.live_count(),0u);CHECK_EQ(poll(listener),0xC0000008u);
    runtime_shutdown();clear_imports();memory.release();
    CHECK_ST(publish_xam_notification(one,91),Status::NotInitialized);
    initialize();
    const uint32_t fresh=call(__imp__XamNotifyCreateListener,1,3);
    CHECK(fresh!=0);CHECK_EQ(next(fresh),0u);CHECK_EQ(call(__imp__NtClose,fresh),0u);
    runtime_shutdown();clear_imports();memory.release();
    return test_result("rt_notifications");
}
