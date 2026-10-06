#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/video.h"
#include "test_util.h"
using namespace rcomp;
using namespace rcomp::rt;
// TESTDOUBLE hook boundary: matched contexts are removed before retirement;
// last-chance unregister of an already removed context is idempotent.
namespace {
thread_local void* TESTDOUBLE_profile_context = nullptr;
thread_local uint32_t TESTDOUBLE_profile_tid = 0;
std::atomic<unsigned> TESTDOUBLE_profile_binds{0}, TESTDOUBLE_profile_removals{0};
std::atomic<bool> TESTDOUBLE_profile_order{true};
}
extern "C" void rcomp_register_thread_slot(uint32_t) {}
extern "C" void rcomp_register_thread_ctx(uint32_t tid, void* context) {
    if (TESTDOUBLE_profile_context) TESTDOUBLE_profile_order = false;
    TESTDOUBLE_profile_tid = tid; TESTDOUBLE_profile_context = context;
    ++TESTDOUBLE_profile_binds;
}
extern "C" void rcomp_unregister_thread_ctx(uint32_t tid, void* context) {
    if (tid != TESTDOUBLE_profile_tid || context != TESTDOUBLE_profile_context) return;
    GuestThread* guest = current_guest_thread();
    if (!guest || guest->thread_id != tid || thread_object_exited(guest->identity))
        TESTDOUBLE_profile_order = false;
    TESTDOUBLE_profile_context = nullptr; ++TESTDOUBLE_profile_removals;
}
namespace {
std::atomic<uint32_t> observed{0}, pcr_seen{0}, dynamic_seen{0};
uint32_t TESTDOUBLE_selected_source = 1, TESTDOUBLE_selected_user = 0x55, TESTDOUBLE_selected_cpu = 2;
bool TESTDOUBLE_nested_once = false;
std::shared_ptr<ThreadObjectIdentity> observed_identity;
uint32_t word(const uint8_t* p) { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; }
void TESTDOUBLE_guest_irq(PPCContext& c,uint8_t* base) {
    GuestThread* guest = current_guest_thread();
    CHECK(guest && TESTDOUBLE_profile_context == &c);
    observed_identity = guest->identity;
    CHECK_EQ(c.r3.u32, TESTDOUBLE_selected_source);
    CHECK_EQ(c.r4.u32, TESTDOUBLE_selected_user);
    CHECK_EQ(base[c.r13.u32 + kPcrProcessorNumber], TESTDOUBLE_selected_cpu);
    CHECK_EQ(c.r1.u32, guest->initial_r1);
    CHECK_EQ(uint32_t(c.lr), kGuestLrSentinel);
    CHECK_EQ(guest->irql, 0u);  // current contract, ISR elevation NOT TESTED
    dynamic_seen = guest->tls_values[7]++;
    if (TESTDOUBLE_nested_once) {
        TESTDOUBLE_nested_once = false;
        const uint32_t r1=c.r1.u32, r3=c.r3.u32, r4=c.r4.u32, r13=c.r13.u32;
        const uint64_t lr=c.lr;
        const uint8_t cpu=base[r13+kPcrProcessorNumber];
        bool rejected=false;
        CAPTURE_FATAL(dispatch_graphics_interrupt(0x82000100,0xDEADBEEF,99,0),rejected);
        CHECK(rejected && g_fatal_kind==RCOMP_FATAL_INTERNAL);
        CHECK_EQ(c.r1.u32,r1); CHECK_EQ(c.r3.u32,r3); CHECK_EQ(c.r4.u32,r4);
        CHECK_EQ(c.r13.u32,r13); CHECK_EQ(c.lr,lr);
        CHECK_EQ(base[r13+kPcrProcessorNumber],cpu);
        CHECK(current_guest_thread()==guest && TESTDOUBLE_profile_context==&c);
    }
    const uint32_t tls=word(base+c.r13.u32), value=word(base+tls);
    observed=value; pcr_seen=c.r13.u32;
    const uint32_t next=__builtin_bswap32(value+1); memcpy(base+tls,&next,4);
}
void TESTDOUBLE_guest_irq_exit(PPCContext&, uint8_t*) {
    observed_identity = current_guest_thread()->identity;
    exit_current_guest_thread(0x55);
}
}
int main() {
    GuestMemory mem1, mem2, mem3;
    constexpr uint32_t source=0x82000000;
    for (auto* mem : {&mem1,&mem2,&mem3}) {
        CHECK(mem->reserve()==MemStatus::Ok);
        CHECK(mem->commit(source,0x10000,Protect::ReadWrite)==MemStatus::Ok);
    }
    GuestMemory* current=&mem1;
    const FuncEntry functions[] = {
        {0x82000100, TESTDOUBLE_guest_irq, "TESTDOUBLE_guest_irq"},
        {0x82000200, TESTDOUBLE_guest_irq_exit, "TESTDOUBLE_guest_irq_exit"},
    };
    CHECK(register_functions(functions, 2));
    uint32_t selected_callback = 0x82000100;
    std::mutex mu; std::condition_variable cv; int command=0; bool done=false;
    std::thread worker([&] {
        for (;;) {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock,[&] { return command!=0; });
            if (command==2) break;
            command=0; const uint32_t callback=selected_callback;
            const uint32_t user=TESTDOUBLE_selected_user, source=TESTDOUBLE_selected_source, cpu=TESTDOUBLE_selected_cpu;
            lock.unlock();
            dispatch_graphics_interrupt(callback,user,source,cpu);
            lock.lock(); done=true; cv.notify_all();
        }
    });
    auto invoke=[&](uint32_t callback=0x82000100, uint32_t source=1, uint32_t user=0x55, uint32_t cpu=2) {
        std::unique_lock<std::mutex> lock(mu); selected_callback=callback;
        TESTDOUBLE_selected_source=source; TESTDOUBLE_selected_user=user; TESTDOUBLE_selected_cpu=cpu;
        done=false; command=1; cv.notify_all();
        CHECK(cv.wait_for(lock,std::chrono::seconds(2),[&] { return done; }));
    };
    auto configure=[&](uint32_t value) {
        CHECK_ST(runtime_init(current),Status::Ok); CHECK(guest_write_be32(source,value));
        CHECK_ST(runtime_set_static_tls({true,4,source,16,4}),Status::Ok);
    };
    configure(0x11111111); invoke(); CHECK_EQ(observed.load(),0x11111111u);
    CHECK(observed_identity && !thread_object_exited(observed_identity));
    CHECK_EQ(dynamic_seen.load(),0u);
    const uint32_t first_pcr=pcr_seen.load();
    TESTDOUBLE_nested_once=true;
    invoke(0x82000100,2,0x11223344,5);
    CHECK_EQ(dynamic_seen.load(),1u);
    CHECK_EQ(observed.load(),0x11111112u); CHECK_EQ(pcr_seen.load(),first_pcr);
    CHECK(observed_identity && !thread_object_exited(observed_identity));
    invoke(0x82000200);
    const auto terminated_identity = observed_identity;
    CHECK(thread_object_exited(terminated_identity));
    CHECK_EQ(thread_object_exit_code(terminated_identity), 0x55u);
    invoke();
    CHECK_EQ(observed.load(),0x11111111u);  // recreated from the title's TLS template
    CHECK_EQ(dynamic_seen.load(),0u);
    CHECK(observed_identity != terminated_identity);
    CHECK(observed_identity && !thread_object_exited(observed_identity));
    CHECK_EQ(runtime()->heap.stats().live_allocations,3u);
    runtime_shutdown(); current=&mem2;
    configure(0x22222222); invoke(); CHECK_EQ(observed.load(),0x22222222u);
    CHECK_EQ(runtime()->heap.stats().live_allocations,3u);
    runtime_shutdown(); current=&mem3;
    // The late host TLS destructor must not free identical guest addresses
    // that now belong to a different runtime's independently created thread.
    CHECK_ST(runtime_init(current),Status::Ok);
    GuestThread replacement; PPCContext context;
    CHECK_ST(create_guest_thread(runtime()->heap,{0x40000,0,0},&context,&replacement),Status::Ok);
    { std::lock_guard<std::mutex> lock(mu); command=2; cv.notify_all(); }
    worker.join();
    CHECK_EQ(runtime()->heap.stats().live_allocations,3u);
    // A host callback context is retired once when its actual pthread ends.
    TESTDOUBLE_selected_source=1; TESTDOUBLE_selected_user=0x55; TESTDOUBLE_selected_cpu=2;
    std::thread final_worker([&] { dispatch_graphics_interrupt(0x82000100,0x55,1,2); });
    final_worker.join();
    CHECK(observed_identity && thread_object_exited(observed_identity));
    CHECK_EQ(thread_object_exit_code(observed_identity),0u);
    CHECK_EQ(runtime()->heap.stats().live_allocations,3u);
    CHECK_ST(destroy_guest_thread(runtime()->heap,&replacement),Status::Ok);
    CHECK_EQ(TESTDOUBLE_profile_binds.load(), TESTDOUBLE_profile_removals.load());
    CHECK(TESTDOUBLE_profile_order.load());
    runtime_shutdown(); clear_functions();
    return test_result("rt_video_lifetime");
}
