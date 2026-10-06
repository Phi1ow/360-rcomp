#include <thread>
#include <vector>
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/xex_loader.h"
#include "test_util.h"
using namespace rcomp;
using namespace rcomp::rt;
namespace {
constexpr uint32_t kBase = 0x82000000, kHeader = 0x1000, kImage = 0x10000;
void put(std::vector<uint8_t>& x, size_t p, uint32_t v) {
    x[p] = uint8_t(v >> 24); x[p+1] = uint8_t(v >> 16);
    x[p+2] = uint8_t(v >> 8); x[p+3] = uint8_t(v);
}
std::vector<uint8_t> fixture() {
    std::vector<uint8_t> x(kHeader + kImage);
    put(x, 0, 0x58455832); put(x, 8, kHeader); put(x, 16, 0x400); put(x, 20, 4);
    const uint32_t h[][2] = {{0x3FF,0x100}, {0x10100,kBase+0x100}, {0x10201,kBase}, {0x20104,0x180}};
    for (unsigned i=0;i<4;++i) { put(x,24+i*8,h[i][0]); put(x,28+i*8,h[i][1]); }
    put(x,0x100,8); put(x,0x404,kImage); put(x,0x510,kBase);
    put(x,0x180,4); put(x,0x184,kBase+0x800); put(x,0x188,32); put(x,0x18C,8);
    put(x,kHeader+0x800,0x13579BDF); put(x,kHeader+0x804,0x2468ACE0);
    return x;
}
uint32_t read(const uint8_t* p) { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; }
}
int main() {
    GuestMemory mem; CHECK(mem.reserve() == MemStatus::Ok);
    XexImage image;
    for (int n=0;n<9;++n) {
        auto x = fixture(); Status expected = Status::InvalidArgument;
        if (n==0) put(x,28+3*8,kHeader-8);
        if (n==1) put(x,0x18C,33);
        if (n==2) put(x,0x184,kBase+kImage-4);
        if (n==3) put(x,0x184,0xFFFFFFFC);
        if (n==4) { put(x,0x180,0x40000000); expected=Status::Unsupported; }
        if (n==5) { put(x,0x180,0); expected=Status::Unsupported; }
        if (n==6) { put(x,20,5); put(x,56,0x20104); put(x,60,0x180); }
        if (n==7) put(x,52,24);  // metadata overlapping the optional-header table
        if (n==8) {
            put(x,36,0); put(x,44,0); put(x,0x510,0);
            for (size_t p=0x180;p<0x190;p+=4) put(x,p,0);
            expected=Status::Conflict;  // poison page must never be mapped
        }
        CHECK_ST(load_xex_image(mem,x.data(),x.size(),&image,nullptr),expected);
        CHECK(!mem.is_committed(kBase,1));
    }
    auto x = fixture();
    CHECK_ST(load_xex_image(mem,x.data(),x.size(),&image,nullptr),Status::Ok);
    CHECK(image.tls.present); CHECK_EQ(image.tls.slot_count,4u);
    CHECK_EQ(image.tls.raw_data_address,kBase+0x800); CHECK_EQ(image.tls.data_size,32u);
    CHECK_EQ(image.tls.raw_data_size,8u);
    CHECK_ST(runtime_init(&mem),Status::Ok);
    CHECK_ST(runtime_set_static_tls({false,4,kBase+0x800,32,8}),Status::InvalidArgument);
    CHECK_ST(runtime_set_static_tls(image.tls),Status::Ok);
    memset(mem.base()+kBase+0x800,0xCC,8);
    GuestThread a,b; PPCContext ca,cb;
    CHECK_ST(create_guest_thread(runtime()->heap,{0x10000,0,0},&ca,&a),Status::Ok);
    CHECK_ST(create_guest_thread(runtime()->heap,{0x10000,0,0},&cb,&b),Status::Ok);
    CHECK(a.tls != b.tls); CHECK_EQ(a.tls_size,48u); CHECK_EQ(b.tls_size,48u);
    CHECK_EQ(ca.r13.u32,a.pcr); CHECK_EQ(read(mem.base()+a.pcr),a.tls);
    CHECK_EQ(read(mem.base()+a.tls),0x13579BDFu);
    CHECK_EQ(read(mem.base()+b.tls+4),0x2468ACE0u);
    for (uint32_t i=8;i<48;++i) { CHECK_EQ(mem.base()[a.tls+i],0u); CHECK_EQ(mem.base()[b.tls+i],0u); }
    std::thread ta([&] { memset(mem.base()+a.tls,0x11,a.tls_size); });
    std::thread tb([&] { memset(mem.base()+b.tls,0x22,b.tls_size); });
    ta.join(); tb.join();
    CHECK_EQ(read(mem.base()+a.tls),0x11111111u); CHECK_EQ(read(mem.base()+b.tls),0x22222222u);
    CHECK_ST(runtime_set_static_tls(image.tls),Status::Conflict);
    CHECK_ST(destroy_guest_thread(runtime()->heap,&a),Status::Ok);
    CHECK_ST(destroy_guest_thread(runtime()->heap,&b),Status::Ok);
    CHECK_EQ(runtime()->heap.stats().live_allocations,0u);
    GuestHeap tiny; CHECK_ST(tiny.init(&mem,0x70000000,0x70020000),Status::Ok);
    CHECK_ST(create_guest_thread(tiny,{0x10000,0,0},&ca,&a),Status::OutOfMemory);
    CHECK_EQ(tiny.stats().live_allocations,0u);
    runtime_shutdown(); CHECK(runtime()==nullptr);
    CHECK_ST(runtime_init(&mem),Status::Ok);
    CHECK_ST(create_guest_thread(runtime()->heap,{0x10000,0,0},&ca,&a),Status::Ok);
    CHECK_EQ(a.tls_size,kTlsBytes); CHECK_EQ(read(mem.base()+a.tls),0u);
    CHECK_ST(destroy_guest_thread(runtime()->heap,&a),Status::Ok);
    runtime_shutdown();
    return test_result("rt_static_tls");
}
