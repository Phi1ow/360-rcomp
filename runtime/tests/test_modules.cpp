// Original XEX/PE bytes exercising the production loader and module services.
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/modules.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__RtlCompareStringN);
PPC_EXTERN_FUNC(__imp__RtlImageXexHeaderField);
PPC_EXTERN_FUNC(__imp__XexGetModuleHandle);
PPC_EXTERN_FUNC(__imp__XexGetProcedureAddress);
PPC_EXTERN_FUNC(__imp__XexCheckExecutablePrivilege);
PPC_EXTERN_FUNC(__imp__XexGetModuleSection);
using namespace rcomp;
using namespace rcomp::rt;
namespace {
constexpr uint32_t base=0x82000000, image_size=0x20000, header_size=0x1000;
constexpr uint32_t entry=base+0x1000, thunk=base+0x1100, data=base+0x2400;
constexpr uint32_t scratch=0x30000000, output=scratch+0x100, name=scratch+0x200;
GuestMemory mem;
void TESTDOUBLE_entry(PPCContext& c,uint8_t*) { c.r3.u64=0xEA01; }
void be(std::vector<uint8_t>& v,size_t p,uint32_t x) {for(int i=0;i<4;++i)v[p+i]=(uint8_t)(x>>(24-8*i));}
void le(std::vector<uint8_t>& v,size_t p,uint32_t x) {for(int i=0;i<4;++i)v[p+i]=(uint8_t)(x>>(8*i));}
void le16(std::vector<uint8_t>& v,size_t p,uint16_t x) {v[p]=(uint8_t)x;v[p+1]=(uint8_t)(x>>8);}
uint32_t word(uint32_t p) {uint32_t v=0;CHECK(guest_read_be32(p,&v));return v;}
void put(uint32_t p,uint32_t v) {CHECK(guest_write_be32(p,v));}
uint32_t call(PPCFunc* fn,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0,uint32_t e=0) {
    alignas(64) PPCContext ctx{};ctx.r3.u64=a;ctx.r4.u64=b;ctx.r5.u64=c;ctx.r6.u64=d;ctx.r7.u64=e;
    fn(ctx,mem.base());return ctx.r3.u32;
}
std::vector<uint8_t> original_xex() {
    std::vector<uint8_t> x(header_size+image_size);
    be(x,0,0x58455832);be(x,8,header_size);be(x,16,0x400);be(x,20,8);
    const uint32_t fields[][2]={{0x3FF,0x100},{0x10100,entry},{0x10201,base},{0x103FF,0x200},
        {0x30000,0x80000008},{0x1234502,0x300},{0x12346FF,0x310},{0x2FF,0x340}};
    for(size_t i=0;i<8;++i){be(x,24+8*i,fields[i][0]);be(x,28+8*i,fields[i][1]);}
    be(x,0x100,8);be(x,0x300,0xFACE1234);be(x,0x304,0x01020304);be(x,0x310,12);be(x,0x314,0x50607080);
    // XEX resource directory (0x2FF): {name[8], address, size} entries.
    be(x,0x340,4+3*16);memcpy(x.data()+0x344,"HashSec",8);be(x,0x34C,data);be(x,0x350,4);
    memcpy(x.data()+0x354,"4D53082D",8);be(x,0x35C,data+4);be(x,0x360,8);
    memcpy(x.data()+0x364,"Outside",8);be(x,0x36C,base+image_size-4);be(x,0x370,8);
    be(x,0x404,image_size);be(x,0x510,base);be(x,0x560,base+0x2200);
    // Imports: the four real module variables and a real AOT function thunk.
    const char module[]="xboxkrnl.exe";memcpy(x.data()+0x20C,module,sizeof module);
    be(x,0x200,12+16+40+20);be(x,0x204,16);be(x,0x208,1);
    be(x,0x21C,60);x[0x21C+0x27]=5;
    const uint32_t ordinals[]={0x193,0x59,0x266,0x1AE};
    for(unsigned i=0;i<4;++i){be(x,0x21C+40+4*i,base+0x2000+4*i);be(x,header_size+0x2000+4*i,ordinals[i]);}
    be(x,0x21C+56,thunk);be(x,header_size+0x1100,0x0100011D);
    // A minimal valid PE32 with independent code/data sections.
    le16(x,header_size,0x5A4D);le(x,header_size+0x3C,0x80);
    const size_t nt=header_size+0x80, opt=nt+24;
    le(x,nt,0x4550);le16(x,nt+4,0x1F2);le16(x,nt+6,2);le(x,nt+8,0x24681357);le16(x,nt+20,224);
    le16(x,opt,0x10B);le(x,opt+16,0x1000);le(x,opt+28,base);le(x,opt+32,0x1000);le(x,opt+36,0x200);
    le(x,opt+56,image_size);le(x,opt+60,0x1000);le(x,opt+64,0x13572468);le(x,opt+92,16);
    for(unsigned i=0;i<2;++i){size_t s=opt+224+i*40;le(x,s+8,0x1000);le(x,s+12,0x1000+i*0x1000);le(x,s+16,0x1000);le(x,s+36,i ? 0xC0000040 : 0x60000020);}
    // XEX ordinal table: ordinal 7 is executable, 8 is readable data.
    const size_t exports=header_size+0x2200;
    be(x,exports+0x20,base>>16);be(x,exports+0x24,2);be(x,exports+0x28,7);
    be(x,exports+0x2C,0x1000);be(x,exports+0x30,0x2400);be(x,header_size+0x2400,0xAABBCCDD);
    return x;
}
void setup() {
    CHECK(mem.reserve()==MemStatus::Ok);CHECK(mem.commit(scratch,0x20000,Protect::ReadWrite)==MemStatus::Ok);
    RuntimeConfig cfg;cfg.heap_hi=0x40100000;
    CHECK_ST(runtime_init(&mem,cfg),Status::Ok);clear_imports();CHECK_ST(register_xboxkrnl_hle(),Status::Ok);
}
void clean() {runtime_shutdown();clear_functions();clear_imports();mem.release();}
uint32_t binding(uint32_t ordinal) {uint32_t p=0;CHECK(find_variable_import(kModuleXboxkrnl,ordinal,&p));return p;}
uint32_t get_module(const char* text) {
    if(text) strcpy((char*)mem.base()+name,text);
    CHECK_EQ(call(__imp__XexGetModuleHandle,text?name:0,output),0u);return word(output);
}
void mappings(bool correct=true,bool include_entry=true) {
    std::vector<FuncEntry> f;
    if(include_entry)f.push_back({entry,TESTDOUBLE_entry,"TESTDOUBLE_entry"});
    f.push_back({thunk,correct?__imp__RtlCompareStringN:TESTDOUBLE_entry,"TESTDOUBLE_thunk_mapping"});
    CHECK(register_functions(f.data(),f.size()));
}
void unchanged(const GuestHeapStats& before) {
    const auto after=runtime()->heap.stats();CHECK_EQ(after.live_allocations,before.live_allocations);CHECK_EQ(after.allocated_bytes,before.allocated_bytes);
}
void headers() {
    auto x=original_xex();std::vector<XexHeaderField> fields;
    CHECK_ST(validate_xex_header(x.data(),x.size(),&fields),Status::Ok);CHECK_EQ(fields.size(),8u);
    for(int which=0;which<6;++which) {
        auto bad=x;
        if(which==0)be(bad,24+8,0x3FF); // duplicate
        if(which==1)be(bad,28+5*8,24); // points into optional table
        if(which==2)be(bad,28+5*8,header_size-4); // fixed payload overflow
        if(which==3)be(bad,0x310,UINT32_MAX); // variable length overflow
        if(which==4)be(bad,16,header_size-4); // security truncation
        if(which==5)be(bad,20,UINT32_MAX); // count overflow
        CHECK_ST(validate_xex_header(bad.data(),bad.size(),nullptr),Status::InvalidArgument);
        XexImage rejected;CHECK_ST(load_xex_image(mem,bad.data(),bad.size(),&rejected,nullptr),Status::InvalidArgument);
        CHECK(!mem.is_committed(base,1));
    }
}
void modules() {
    ModuleConfig cfg;cfg.guest_path="game:\\Original.xex";cfg.command_line="original --level 2";
    ModuleConfig bad=cfg;bad.guest_path="";
    CHECK_ST(runtime_prepare_main_module(bad),Status::InvalidArgument);CHECK_EQ(runtime()->heap.stats().live_allocations,0u);
    bad.guest_path="C:\\host.xex";CHECK_ST(runtime_prepare_main_module(bad),Status::InvalidArgument);
    CHECK_ST(runtime_prepare_main_module(cfg),Status::Ok);CHECK_ST(runtime_prepare_main_module(cfg),Status::Conflict);
    const auto prepared=runtime()->heap.stats();CHECK_EQ(prepared.live_allocations,1u);
    uint32_t handle_cell=binding(0x193);CHECK_EQ(word(handle_cell),0u);
    CHECK_EQ(word(binding(0x59)),0u);CHECK_EQ(word(binding(0x266)),0u);
    CHECK(strcmp((char*)mem.base()+binding(0x1AE),cfg.command_line.c_str())==0);
    auto x=original_xex();XexImage img;CHECK_ST(load_xex_image(mem,x.data(),x.size(),&img,nullptr),Status::Ok);
    CHECK_EQ(img.variables_resolved,4u);CHECK_EQ(img.function_thunks,1u);CHECK_EQ(img.import_thunks.size(),1u);
    CHECK(img.header==std::vector<uint8_t>(x.begin(),x.begin()+header_size));
    CHECK_EQ(word(base+0x2000),handle_cell);CHECK_EQ(word(base+0x200C),binding(0x1AE));
    mappings(false);CHECK_ST(runtime_finalize_main_module(img),Status::Unsupported);unchanged(prepared);CHECK_EQ(word(handle_cell),0u);
    mappings(true,false);CHECK_ST(runtime_finalize_main_module(img),Status::Unsupported);unchanged(prepared);
    mappings();
    {auto false_metadata=img;false_metadata.entry_point+=4;CHECK_ST(runtime_finalize_main_module(false_metadata),Status::InvalidArgument);unchanged(prepared);}
    {auto false_metadata=img;false_metadata.base+=0x10000;CHECK_ST(runtime_finalize_main_module(false_metadata),Status::InvalidArgument);unchanged(prepared);}
    {auto unsupported=img;be(unsupported.header,24+5*8,0xE10402);CHECK_ST(runtime_finalize_main_module(unsupported),Status::Unsupported);unchanged(prepared);}
    // Ordinary PE export metadata must be explicitly rejected, not hidden.
    mem.base()[base+0x80+24+96]=0x40;
    CHECK_ST(runtime_finalize_main_module(img),Status::Unsupported);unchanged(prepared);
    mem.base()[base+0x80+24+96]=0;
    CHECK_ST(runtime_finalize_main_module(img),Status::Ok);CHECK_ST(runtime_finalize_main_module(img),Status::Conflict);
    CHECK_EQ(runtime()->heap.stats().live_allocations,2u);
    uint32_t main=word(handle_cell), kernel=get_module("XBOXKRNL.EXE"), xam=get_module("xam.xex");
    CHECK(main && kernel && xam && main!=kernel && kernel!=xam);
    CHECK_EQ(get_module(nullptr),main);CHECK_EQ(get_module("ORIGINAL.XEX"),main);CHECK_EQ(get_module("GAME:\\original.xex"),main);
    CHECK_EQ(word(main+0x18),base+0x80);CHECK_EQ(word(main+0x1C),base);CHECK_EQ(word(main+0x20),image_size);
    CHECK_EQ(word(main+0x38),image_size);CHECK_EQ(word(main+0x3C),entry);CHECK_EQ(word(main+0x48),0x13572468u);CHECK_EQ(word(main+0x50),0x24681357u);
    uint16_t len=0;CHECK(guest_read_be16(main+0x2C,&len));CHECK_EQ(len,24u); // Original.xex UTF16BE
    CHECK_EQ(mem.base()[word(main+0x30)],0u);CHECK_EQ(mem.base()[word(main+0x30)+1],'O');
    CHECK_EQ(word(main),kernel);CHECK_EQ(word(kernel),xam);CHECK_EQ(word(xam),main);
    CHECK_EQ(word(kernel+0x58),0u);CHECK_EQ(word(xam+0x1C),0u);
    uint32_t header=word(main+0x58);
    CHECK(mem.is_accessible(header,header_size,Protect::Read));CHECK(!mem.is_accessible(header,1,Protect::ReadWrite));
    CHECK(memcmp(mem.base()+header,img.header.data(),header_size)==0);
    CHECK_EQ(call(__imp__RtlImageXexHeaderField,header,0x30000),0x80000008u);
    CHECK_EQ(call(__imp__RtlImageXexHeaderField,header,0x10201),header+28+2*8);
    CHECK_EQ(call(__imp__RtlImageXexHeaderField,header,0x1234502),header+0x300);
    CHECK_EQ(call(__imp__RtlImageXexHeaderField,header,0x12346FF),header+0x310);
    CHECK_EQ(call(__imp__RtlImageXexHeaderField,header,0x55500),0u);
    CHECK_EQ(call(__imp__XexCheckExecutablePrivilege,3),1u);CHECK_EQ(call(__imp__XexCheckExecutablePrivilege,31),1u);
    CHECK_EQ(call(__imp__XexCheckExecutablePrivilege,32),0u);CHECK_EQ(call(__imp__XexCheckExecutablePrivilege,UINT32_MAX),0u);
    CHECK_EQ(call(__imp__XexCheckExecutablePrivilege,4),0u);
    {   // XexGetModuleSection over the main module's resource directory.
        constexpr uint32_t not_found=0xC0000225u;
        auto section=[&](uint32_t module,const char* text,uint32_t data_out,uint32_t size_out) {
            strcpy((char*)mem.base()+name,text);return call(__imp__XexGetModuleSection,module,name,data_out,size_out);
        };
        CHECK_EQ(section(main,"HashSec",output,output+4),0u);CHECK_EQ(word(output),data);CHECK_EQ(word(output+4),4u);
        CHECK_EQ(word(word(output)),0xAABBCCDDu);  // the section bytes are the loaded image's
        CHECK_EQ(section(main,"4D53082D",output,output+4),0u);CHECK_EQ(word(output),data+4);CHECK_EQ(word(output+4),8u);
        put(output,0x11111111);put(output+4,0x22222222);
        for(const char* absent:{"hashsec","HashSe","HashSecX","4D53082DX",""}) {
            CHECK_EQ(section(main,absent,output,output+4),not_found);
            CHECK_EQ(word(output),0x11111111u);CHECK_EQ(word(output+4),0x22222222u);
        }
        CHECK_EQ(section(main+4,"HashSec",output,output+4),nt::kInvalidHandle);
        CHECK_EQ(section(main,"HashSec",0,output+4),nt::kAccessViolation);
        CHECK_EQ(section(main,"HashSec",output,output+2),nt::kAccessViolation);
        CHECK_EQ(call(__imp__XexGetModuleSection,main,0x50000000,output,output+4),nt::kAccessViolation);
        CHECK_EQ(word(output),0x11111111u);CHECK_EQ(word(output+4),0x22222222u);
        bool trapped=false;
        CAPTURE_FATAL(section(kernel,"HashSec",output,output+4),trapped);CHECK(trapped && g_fatal_kind==RCOMP_FATAL_UNIMPLEMENTED);
        CAPTURE_FATAL(section(0,"HashSec",output,output+4),trapped);CHECK(trapped && g_fatal_kind==RCOMP_FATAL_UNIMPLEMENTED);
        CAPTURE_FATAL(section(main,"Outside",output,output+4),trapped);CHECK(trapped && g_fatal_kind==RCOMP_FATAL_UNIMPLEMENTED);
        CHECK_EQ(word(output),0x11111111u);
    }
    CHECK_EQ(call(__imp__XexGetProcedureAddress,kernel,0x11D,output),0u);CHECK_EQ(word(output),thunk);
    CHECK_EQ(call(lookup_function(word(output)),0,0,0,0,0),0u);
    strcpy((char*)mem.base()+name,"RtlCompareStringN");
    CHECK_EQ(call(__imp__XexGetProcedureAddress,kernel,name,output),0u);CHECK_EQ(word(output),thunk);
    PPCFunc* available=find_import(kModuleXboxkrnl,0x11D);
    CHECK(available!=nullptr);
    unregister_import(kModuleXboxkrnl,0x11D);
    put(output,0xAABBCCDDu);
    CHECK_EQ(call(__imp__XexGetProcedureAddress,kernel,0x11D,output),0xC0000263u);
    CHECK_EQ(word(output),0u);
    put(output,0xAABBCCDDu);
    CHECK_EQ(call(__imp__XexGetProcedureAddress,kernel,name,output),0xC0000263u);
    CHECK_EQ(word(output),0u);CHECK(find_import(kModuleXboxkrnl,0x11D)==nullptr);
    CHECK_ST(register_import(kModuleXboxkrnl,0x11D,available,"RtlCompareStringN"),Status::Ok);
    CHECK_EQ(call(__imp__XexGetProcedureAddress,kernel,0x1AE,output),0u);CHECK_EQ(word(output),binding(0x1AE));
    CHECK_EQ(call(__imp__XexGetProcedureAddress,main,7,output),0u);CHECK_EQ(word(output),entry);CHECK_EQ(call(lookup_function(word(output))),0xEA01u);
    CHECK_EQ(call(__imp__XexGetProcedureAddress,0,8,output),0u);CHECK_EQ(word(output),data);CHECK_EQ(word(data),0xAABBCCDDu);
    CHECK_EQ(call(__imp__XexGetProcedureAddress,main,9,output),0xC0000263u);CHECK_EQ(word(output),0u);
    mem.base()[name]=0;CHECK_EQ(call(__imp__XexGetProcedureAddress,main,name,output),0xC0000263u);CHECK_EQ(word(output),0u);
    strcpy((char*)mem.base()+name,"absent");
    CHECK_EQ(call(__imp__XexGetModuleHandle,name,output),1168u);CHECK_EQ(word(output),0u);
    CHECK_EQ(call(__imp__XexGetProcedureAddress,kernel,name,output),0xC0000263u);CHECK_EQ(word(output),0u);
    mem.base()[name]=0xE9;mem.base()[name+1]=0; // unknown name is not an access fault
    CHECK_EQ(call(__imp__XexGetModuleHandle,name,output),1168u);
    CHECK_EQ(call(__imp__XexGetProcedureAddress,kernel,name,output),0xC0000263u);
    put(output,0x12345678);CHECK_EQ(call(__imp__XexGetProcedureAddress,main+4,7,output),nt::kInvalidHandle);CHECK_EQ(word(output),0x12345678u);
    CHECK_EQ(call(__imp__XexGetProcedureAddress,kernel,0x50000000,output),nt::kAccessViolation);CHECK_EQ(word(output),0x12345678u);
    CHECK_EQ(call(__imp__XexGetModuleHandle,0x50000000,output),nt::kAccessViolation);CHECK_EQ(word(output),0x12345678u);
    CHECK(mem.protect(scratch+0x10000,0x10000,Protect::None)==MemStatus::Ok);
    mem.base()[scratch+0xFFFF]='x';CHECK_EQ(call(__imp__XexGetModuleHandle,scratch+0xFFFF,output),nt::kAccessViolation);CHECK_EQ(word(output),0x12345678u);
    CHECK(mem.protect(scratch,0x10000,Protect::Read)==MemStatus::Ok);
    CHECK_EQ(call(__imp__XexGetModuleHandle,0,output),nt::kAccessViolation);CHECK_EQ(word(output),0x12345678u);
    CHECK_EQ(call(__imp__XexGetProcedureAddress,kernel,0x11D,output),nt::kAccessViolation);CHECK_EQ(word(output),0x12345678u);
    CHECK(mem.protect(scratch,0x10000,Protect::ReadWrite)==MemStatus::Ok);
    bool fatal=false;
    CAPTURE_FATAL(call(__imp__RtlImageXexHeaderField,0,0x30000),fatal);CHECK(fatal && g_fatal_kind==RCOMP_FATAL_GUEST_ACCESS);
    CAPTURE_FATAL(call(__imp__XexGetProcedureAddress,kernel,0,output),fatal);CHECK(fatal && g_fatal_kind==RCOMP_FATAL_UNIMPLEMENTED);
    CAPTURE_FATAL(call(__imp__XexGetProcedureAddress,kernel,0x126,output),fatal);CHECK(fatal && g_fatal_kind==RCOMP_FATAL_UNIMPLEMENTED); // implemented, no static thunk
    CAPTURE_FATAL(call(__imp__XexGetProcedureAddress,kernel,0x158,output),fatal);CHECK(fatal && g_fatal_kind==RCOMP_FATAL_UNIMPLEMENTED); // variable has no real storage
    clear_functions();CAPTURE_FATAL(call(__imp__XexGetProcedureAddress,main,7,output),fatal);CHECK(fatal && g_fatal_kind==RCOMP_FATAL_UNIMPLEMENTED);
    runtime_shutdown();uint32_t gone=0;
    for(uint32_t ordinal:{0x193,0x59,0x266,0x1AE})CHECK(!find_variable_import(kModuleXboxkrnl,ordinal,&gone));
}
}
int main() {
    setup();headers();modules();clean();
    // A fresh Runtime owns its own bindings. No old bootstrap metadata survives.
    setup();CHECK_ST(runtime_prepare_main_module({}),Status::Ok);CHECK_EQ(mem.base()[binding(0x1AE)],0u);CHECK_EQ(word(binding(0x193)),0u);clean();
    return test_result("modules");
}
