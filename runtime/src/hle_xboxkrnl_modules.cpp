// Main-image metadata and real statically linked HLE module resolution.
// Public contracts/limitations: runtime/docs/MODULES.md.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <new>
#include <set>
#include "module_state.h"
#include "rcomp/aot_modules.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/kernel_variables.h"
#include "rcomp/runtime/modules.h"
#include "rcomp/runtime/runtime.h"
#include <mutex>

namespace rcomp::rt {
namespace {
constexpr uint32_t kStorageBytes=0x10000,kMaximumHeader=16u<<20;
constexpr uint32_t kHandleOffset=0,kDebugOffset=4,kCertOffset=8,kCommandOffset=0x400;
constexpr uint32_t kNotFound=1168,kEntryNotFound=0xC0000263;
struct Variable { uint32_t ordinal; const char* name; uint32_t offset; };
constexpr Variable vars[] = {
    {0x0193,"XexExecutableModuleHandle",kHandleOffset},
    {0x0059,"KeDebugMonitorData",kDebugOffset},
    {0x0266,"KeCertMonitorData",kCertOffset},
    {0x01AE,"ExLoadedCommandLine",kCommandOffset},
};
uint32_t be32(const uint8_t* p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
uint32_t le32(const uint8_t* p) {return (uint32_t)p[3]<<24|(uint32_t)p[2]<<16|(uint32_t)p[1]<<8|p[0];}
uint16_t le16(const uint8_t* p) {return (uint16_t)((p[1]<<8)|p[0]);}
void put32(uint8_t* p,uint32_t v) {p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
void put16(uint8_t* p,uint16_t v) {p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
bool ascii(const std::string& s) {
    for(unsigned char c:s) if(!c || c>0x7F) return false;
    return true;
}
bool same_name(const std::string& a,const std::string& b) {
    if(a.size()!=b.size()) return false;
    for(size_t i=0;i<a.size();++i) {
        auto lower=[](unsigned char c) {return c>='A'&&c<='Z' ? c+32 : c;};
        if(lower(a[i])!=lower(b[i])) return false;
    }
    return true;
}
bool read_name(Runtime& r,uint32_t address,std::string* out) {
    out->clear();
    if(!address) return false;
    for(uint64_t i=0;i<4096;++i) {
        if(!r.mem->is_accessible((uint64_t)address+i,1,Protect::Read)) return false;
        uint8_t c=r.mem->base()[(uint64_t)address+i];
        if(!c) return true;
        out->push_back((char)c);
    }
    return false;
}
ModuleState* state() {
    Runtime* r=runtime();
    return r && r->modules && r->modules->ready && r->modules->generation==r->generation ? r->modules.get() : nullptr;
}
int module_index(const ModuleState& m,uint32_t handle) {
    if(!handle) return 0;
    for(size_t i=0;i<m.records.size();++i) if(m.records[i].handle==handle) return (int)i;
    return -1;
}
const XexHeaderField* field(const std::vector<XexHeaderField>& fields,uint32_t key) {
    for(const auto& f:fields) if(f.key==key) return &f;
    return nullptr;
}
void descriptor(uint8_t* p,uint32_t address,const std::string& text) {
    put16(p,(uint16_t)(text.size()*2));put16(p+2,(uint16_t)(text.size()*2+2));put32(p+4,address);
}
void utf16(uint8_t* p,const std::string& text) {
    for(size_t i=0;i<text.size();++i) {p[2*i]=0;p[2*i+1]=(uint8_t)text[i];}
    p[text.size()*2]=p[text.size()*2+1]=0;
}
} // namespace
Status module_parse_pe(Runtime& r,const XexImage& image,PeInfo* pe,bool unloaded_pe_exports_ok) {
    if(image.size<64 || !r.mem->is_accessible(image.base,image.size,Protect::Read)) return Status::InvalidArgument;
    const uint8_t* p=r.mem->base()+image.base;
    if(le16(p)!=0x5A4D) return Status::InvalidArgument;
    uint32_t nt=le32(p+0x3C);
    if((nt&3) || uint64_t(nt)+24>image.size || le32(p+nt)!=0x4550 || le16(p+nt+4)!=0x1F2)
        return Status::InvalidArgument;
    uint32_t optional=nt+24,bytes=le16(p+nt+20),count=le16(p+nt+6);
    if(bytes<96 || uint64_t(optional)+bytes+uint64_t(count)*40>image.size || le16(p+optional)!=0x10B)
        return Status::InvalidArgument;
    pe->size=le32(p+optional+56);
    // XEX decompression already yields a loaded image. The original PE can
    // describe discarded sections beyond that payload; retain its true size
    // for the LDR, but bound every actual read/export by the XEX loaded size.
    if(!pe->size || uint64_t(image.base)+pe->size>0x100000000ull || le32(p+optional+28)!=image.base ||
       uint64_t(image.base)+le32(p+optional+16)!=image.entry_point) return Status::InvalidArgument;
    pe->headers=image.base+nt;pe->checksum=le32(p+optional+64);pe->timestamp=le32(p+nt+8);
    const uint32_t directories=le32(p+optional+92);
    if(uint64_t(directories)*8>bytes-96) return Status::InvalidArgument;
    // Ordinary PE exports are a different format from the XEX table below.
    // Do not silently advertise their named/ordinal exports as absent. The
    // secondary-module loader decides on the recorded directory itself
    // (src/aot_modules.cpp: only a provably unloaded .edata is accepted).
    if(directories && (le32(p+optional+96)||le32(p+optional+100))) {
        if(!unloaded_pe_exports_ok) return Status::Unsupported;
        pe->export_rva=le32(p+optional+96);pe->export_size=le32(p+optional+100);
    }
    for(uint32_t i=0;i<count;++i) {
        const uint8_t* s=p+optional+bytes+i*40;
        uint32_t start=le32(s+12),size=le32(s+8);
        if(uint64_t(start)+size>pe->size) return Status::InvalidArgument;
        if(size) pe->sections.push_back({uint64_t(image.base)+start,uint64_t(image.base)+start+size,(le32(s+36)&0x20000020u)!=0});
    }
    return Status::Ok;
}
namespace {
Status parse_ordinal_exports(Runtime& r,const XexImage& image,const PeInfo& pe,
                             std::vector<ModuleState::Export>* exports) {
    const uint32_t security=be32(image.header.data()+16);
    if(uint64_t(security)+0x164>image.header.size()) return Status::InvalidArgument;
    const uint32_t address=be32(image.header.data()+security+0x160);
    if(!address) return Status::Ok;
    if(address<image.base || uint64_t(address)+0x2C>uint64_t(image.base)+image.size) return Status::InvalidArgument;
    const uint8_t* p=r.mem->base()+address;
    const uint32_t count=be32(p+0x24),first=be32(p+0x28),highbase=be32(p+0x20);
    if(first>0xFFFF || count>0x10000-first || highbase>0xFFFF ||
       uint64_t(address)+0x2C+uint64_t(count)*4>uint64_t(image.base)+image.size) return Status::InvalidArgument;
    for(uint32_t i=0;i<count;++i) {
        const uint64_t target=((uint64_t)highbase<<16)+be32(p+0x2C+i*4);
        if(target<image.base || target>=uint64_t(image.base)+image.size) return Status::InvalidArgument;
        bool section_found=false,executable=false;
        for(const auto& section:pe.sections) if(target>=section.begin && target<section.end) {
            if(section_found) return Status::InvalidArgument;
            section_found=true;executable=section.executable;
        }
        if(!section_found) return Status::InvalidArgument;
        PPCFunc* function=executable ? lookup_function((uint32_t)target) : nullptr;
        // An export in a code section cannot be misrepresented as data merely
        // because its AOT function is absent.
        if(executable && !function) return Status::Unsupported;
        exports->push_back({first+i,(uint32_t)target,{},executable,function});
    }
    return Status::Ok;
}

void RtlImageXexHeaderField(PPCContext& c,uint8_t*) {
    ModuleState* m=state(); Runtime* r=runtime();
    const std::vector<XexHeaderField>* fields=nullptr;
    uint32_t storage=0,size=0;
    if(m && c.r3.u32==m->header_storage) {fields=&m->fields;storage=m->header_storage;size=m->header_size;}
    std::lock_guard<std::recursive_mutex> lock(secondary_loader_lock());
    if(m && !fields) for(const auto& s:m->secondary) if(s->loaded && s->header_storage==c.r3.u32) {
        fields=&s->fields;storage=s->header_storage;size=s->header_size;
    }
    if(!fields || !r->mem->is_accessible(storage,size,Protect::Read))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,"RtlImageXexHeaderField unknown/unreadable header=0x%08X",c.r3.u32);
    const auto* f=field(*fields,c.r4.u32);
    c.r3.u64=!f ? 0 : (f->key&255)==0 ? f->value : storage+f->offset;
}
void XexCheckExecutablePrivilege(PPCContext& c,uint8_t*) {
    const ModuleState* m=state(); uint32_t bit=c.r3.u32;
    c.r3.u64=m && bit<32 ? (m->system_flags>>bit)&1u : 0;
}
void XexGetModuleHandle(PPCContext& c,uint8_t*) {
    Runtime* r=runtime(); ModuleState* m=state(); const uint32_t output=c.r4.u32;
    if(!r || !output || !r->mem->is_accessible(output,4,Protect::ReadWrite)) {c.r3.u64=nt::kAccessViolation;return;}
    std::string name;
    if(c.r3.u32 && !read_name(*r,c.r3.u32,&name)) {c.r3.u64=nt::kAccessViolation;return;}
    uint32_t result=0;
    if(m) {
        if(!c.r3.u32) result=m->records[0].handle;
        else for(const auto& rec:m->records)
            if(same_name(name,rec.name)||same_name(name,rec.path)) {result=rec.handle;break;}
        if(!result && c.r3.u32) {
            std::lock_guard<std::recursive_mutex> lock(secondary_loader_lock());
            if(const SecondaryModule* s=secondary_by_name(*m,name)) result=s->handle;
        }
    }
    put32(r->mem->host(output),result); c.r3.u64=result ? 0 : kNotFound;
}
void XexGetProcedureAddress(PPCContext& c,uint8_t*) {
    Runtime* r=runtime(); ModuleState* m=state();
    const uint32_t handle=c.r3.u32,selector=c.r4.u32,output=c.r5.u32;
    const int index=m ? module_index(*m,handle) : -1;
    std::unique_lock<std::recursive_mutex> lock(secondary_loader_lock(),std::defer_lock);
    const SecondaryModule* secondary=nullptr;
    if(index<0 && m) {lock.lock();secondary=secondary_by_handle(*m,handle);}
    if(index<0 && !secondary) {c.r3.u64=nt::kInvalidHandle;return;}
    if(!output || !r->mem->is_accessible(output,4,Protect::ReadWrite)) {c.r3.u64=nt::kAccessViolation;return;}
    if(!selector) rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexGetProcedureAddress ordinal zero has no established contract");
    const bool by_name=(selector&0xFFFF0000u)!=0;
    std::string name;
    if(by_name && !read_name(*r,selector,&name)) {c.r3.u64=nt::kAccessViolation;return;}
    uint32_t address=0,ordinal=selector;
    if(secondary) {
        // Ordinal table only: a title DLL's names live in a PE .edata beyond
        // the loaded payload and it has no 0xE10402 table (load rejects one),
        // so a name finds nothing, as for the main module.
        const uint32_t first=secondary->export_first;
        if(!by_name && ordinal>=first && ordinal-first<secondary->export_targets.size()) {
            const uint32_t i=ordinal-first;
            address=secondary->export_targets[i];
            if(!address)
                rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexGetProcedureAddress %s ordinal %u is a hole of the export table "
                            "(no established result) lr=0x%08X",secondary->name.c_str(),ordinal,uint32_t(c.lr));
            if(secondary->export_executable[i] && !lookup_function(address))
                rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"secondary module export lost its AOT mapping");
        }
    } else if(index==0) {
        for(const auto& e:m->main_exports) if(by_name ? !e.name.empty() && e.name==name : e.ordinal==ordinal) {
            if(e.executable && lookup_function(e.address)!=e.function)
                rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"main module export lost its AOT mapping");
            address=e.address;break;
        }
    } else {
        const char* library=index==1 ? kModuleXboxkrnl : kModuleXam;
        if(by_name && !export_ordinal(library,name.c_str(),&ordinal)) ordinal=UINT32_MAX;
        const ExportKind kind=export_kind(library,ordinal);
        if(kind==ExportKind::Function) {
            if(!find_import(library,ordinal)) {
                // This is an availability query, not a call to the missing
                // service. Let the guest follow its own optional-export path.
                // Static imports and direct calls remain explicitly blocked.
                std::fprintf(stderr,
                    "RCOMP-MODULE optional_export_unavailable module=%s ordinal=0x%X lr=0x%08X\n",
                    library,ordinal,uint32_t(c.lr));
                put32(r->mem->host(output),0);
                c.r3.u64=kEntryNotFound;
                return;
            }
            for(const auto& thunk:m->thunks) if(same_name(thunk.module,library)&&thunk.ordinal==ordinal &&
                lookup_function(thunk.address)==import_thunk(library,ordinal)) {address=thunk.address;break;}
            if(!address) rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexGetProcedureAddress %s ordinal=0x%X has no static AOT thunk",library,ordinal);
        } else if(kind==ExportKind::Variable) {
            if(!find_variable_import(library,ordinal,&address))
                rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexGetProcedureAddress %s ordinal=0x%X has no variable storage",library,ordinal);
        }
    }
    if(address && !r->mem->is_accessible(address,1,Protect::Read)) {c.r3.u64=nt::kAccessViolation;return;}
    put32(r->mem->host(output),address);c.r3.u64=address ? 0 : kEntryNotFound;
}
// XexGetModuleSection (0x0196): NTSTATUS (PVOID ModuleHandle, PCSTR Name,
// PVOID* SectionData, PULONG SectionSize). Looks Name up in the module's XEX
// resource directory (optional header 0x000002FF: BE32 byte length, then
// 16-byte entries {char name[8] NUL-padded, BE32 guest address, BE32 size}),
// exact case-sensitive match of up to 8 characters, as Xenia 95a5c3e
// UserModule::GetSection. Found: both outputs written, STATUS_SUCCESS.
// Absent directory or name: STATUS_NOT_FOUND (0xC0000225), outputs untouched.
// Handles are the real LDR handles of XexGetModuleHandle. The HLE modules
// (xboxkrnl.exe, xam.xex) have no resource image in R-comp and a NULL handle
// has no established meaning: both trap instead of answering.
void XexGetModuleSection(PPCContext& c,uint8_t*) {
    constexpr uint32_t kStatusNotFound=0xC0000225u,kResourceInfo=0x000002FFu;
    Runtime* r=runtime(); ModuleState* m=state();
    const uint32_t handle=c.r3.u32,name_address=c.r4.u32,data=c.r5.u32,size=c.r6.u32;
    if(!handle) rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexGetModuleSection NULL module handle has no established contract lr=0x%08X",uint32_t(c.lr));
    const int index=m ? module_index(*m,handle) : -1;
    std::unique_lock<std::recursive_mutex> lock(secondary_loader_lock(),std::defer_lock);
    const SecondaryModule* secondary=nullptr;
    if(index<0 && m) {lock.lock();secondary=secondary_by_handle(*m,handle);}
    if(index<0 && !secondary) {c.r3.u64=nt::kInvalidHandle;return;}
    if(index>0) rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexGetModuleSection %s has no resource image in R-comp lr=0x%08X",
                            m->records[index].name.c_str(),uint32_t(c.lr));
    const std::vector<XexHeaderField>& fields=secondary ? secondary->fields : m->fields;
    const uint32_t header_storage=secondary ? secondary->header_storage : m->header_storage;
    const uint32_t header_size=secondary ? secondary->header_size : m->header_size;
    const uint32_t image_base=secondary ? secondary->aot->image_base : m->image_base;
    const uint32_t image_size=secondary ? secondary->aot->image_size : m->image_size;
    auto writable=[&](uint32_t a) {return a && !(a&3) && r->mem->is_accessible(a,4,Protect::ReadWrite);};
    if(!writable(data) || !writable(size)) {c.r3.u64=nt::kAccessViolation;return;}
    std::string name;
    if(!read_name(*r,name_address,&name)) {c.r3.u64=nt::kAccessViolation;return;}
    const XexHeaderField* f=field(fields,kResourceInfo);
    if(!f || name.empty() || name.size()>8) {c.r3.u64=kStatusNotFound;return;}
    if(!r->mem->is_accessible(header_storage,header_size,Protect::Read))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,"XexGetModuleSection retained XEX header 0x%08X unreadable",header_storage);
    const uint8_t* directory=r->mem->base()+header_storage+f->offset;
    const uint32_t count=(f->size-4)/16;
    for(uint32_t i=0;i<count;++i) {
        const uint8_t* entry=directory+4+i*16;
        size_t length=0;
        while(length<8 && entry[length]) ++length;
        if(length!=name.size() || memcmp(entry,name.data(),length)!=0) continue;
        const uint32_t address=be32(entry+8),bytes=be32(entry+12);
        // A resource is part of the loaded image; anything else would hand the
        // title a pointer to memory this loader never mapped for it.
        if(address<image_base || uint64_t(address)+bytes>uint64_t(image_base)+image_size)
            rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexGetModuleSection %s at 0x%08X+0x%X outside the loaded image",
                        name.c_str(),address,bytes);
        put32(r->mem->host(data),address);put32(r->mem->host(size),bytes);
        c.r3.u64=0;return;
    }
    c.r3.u64=kStatusNotFound;
}
// LDR LoadCount (+0x40, BE16): bootstrap owns one reference per record.
std::mutex g_load_count_mutex;
uint16_t get16(const uint8_t* p) {return (uint16_t)((p[0]<<8)|p[1]);}

// XexLoadImage (0x0199): NTSTATUS (PCSZ ModuleName, DWORD ModuleTypeFlags,
// DWORD MinimumVersion, PHMODULE ModuleHandle) (Xenia; corroborated by the
// community xbdm declaration of XexLoadImageFromMemory's trailing arguments).
// R-comp is an AOT title: the only images that exist are the main module and
// the HLE system modules xboxkrnl.exe / xam.xex. A name or full path of one of
// them (case-insensitive, as XexGetModuleHandle) adds one reference to its LDR
// LoadCount and returns its HMODULE (Xenia's behaviour for loaded modules).
// MinimumVersion 0 is accepted; for a system module a non-zero minimum is
// compared with the presented kernel version (both libraries report the same
// system version); a newer minimum, or a minimum for the main module, has no
// established result and traps. Secondary modules compiled into the title
// (include/rcomp/aot_modules.h, src/aot_modules.cpp) are loaded from the disc:
// a loaded one (bare name, path or same disc file) gains a reference; a file
// on the game devices with an AotModule descriptor is mapped at its fixed base,
// its imports bound, its dependencies loaded first, and DllMain(handle,
// DLL_PROCESS_ATTACH, 0) runs on the calling thread before the handle is
// returned. ModuleTypeFlags must equal the image's module flags and
// MinimumVersion must be 0 for them (no established comparison otherwise).
// Any other module is not part of the build:
//  * a device path ("game:\x.xex") is looked up through the VFS; a missing
//    file returns the NTSTATUS the open would return (what the console's
//    loader propagates), an existing file without a descriptor traps --
//    loading it would need its code recompiled into the title;
//  * a bare name that is no loaded module: STATUS_NO_SUCH_FILE (Xenia).
void XexLoadImage(PPCContext& c,uint8_t* base) {
    constexpr uint32_t kNoSuchFile=0xC000000Fu;
    Runtime* r=runtime(); ModuleState* m=state();
    const uint32_t name_address=c.r3.u32,minimum=c.r5.u32,output=c.r6.u32;
    if(!r || !output || (output&3) || !r->mem->is_accessible(output,4,Protect::ReadWrite)) {c.r3.u64=nt::kAccessViolation;return;}
    std::string name;
    if(!read_name(*r,name_address,&name)) {c.r3.u64=nt::kAccessViolation;return;}
    if(!m) rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexLoadImage(%s) before the main module was finalized lr=0x%08X",name.c_str(),uint32_t(c.lr));
    for(size_t i=0;i<m->records.size();++i) {
        const auto& rec=m->records[i];
        if(!same_name(name,rec.name) && !same_name(name,rec.path)) continue;
        if(minimum) {
            KernelCompatibilityProfile profile;
            if(i==0 || xboxkrnl_kernel_compatibility_profile(&profile)!=Status::Ok || minimum>profile.packed)
                rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexLoadImage(%s) MinimumVersion=0x%08X has no established result for this module lr=0x%08X",
                            name.c_str(),minimum,uint32_t(c.lr));
        }
        uint8_t* ldr=r->mem->base()+rec.handle;
        bool overflow=false;
        {
            std::lock_guard<std::mutex> lock(g_load_count_mutex);
            const uint16_t count=get16(ldr+0x40);
            overflow=count==0xFFFF;
            if(!overflow) put16(ldr+0x40,(uint16_t)(count+1));
        }
        if(overflow) rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,"XexLoadImage(%s) LoadCount overflow",name.c_str());
        put32(r->mem->host(output),rec.handle);
        c.r3.u64=nt::kSuccess;return;
    }
    const uint32_t flags=c.r4.u32;
    std::lock_guard<std::recursive_mutex> lock(secondary_loader_lock());
    if(SecondaryModule* loaded=secondary_by_name(*m,name)) {
        if(minimum || flags!=loaded->module_flags)
            rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexLoadImage(%s) flags=0x%X MinimumVersion=0x%08X have no established result "
                        "for this module (flags 0x%X) lr=0x%08X",name.c_str(),flags,minimum,loaded->module_flags,uint32_t(c.lr));
        const uint32_t status=secondary_add_reference(*loaded);
        put32(r->mem->host(output),loaded->handle);
        c.r3.u64=status;return;
    }
    if(name.find(':')==std::string::npos) {c.r3.u64=kNoSuchFile;return;}
    FileMetadata meta;
    const Status found=r->vfs.query(name,&meta);
    if(found!=Status::Ok) {c.r3.u64=to_ntstatus(found);return;}
    std::string disc;
    const AotModule* aot=secondary_disc_path(name,&disc) ? find_aot_module(disc.c_str()) : nullptr;
    if(!aot)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexLoadImage(%s): the file exists but loading another XEX image needs its code "
                    "recompiled into the title (AOT model) lr=0x%08X",name.c_str(),uint32_t(c.lr));
    if(minimum)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexLoadImage(%s) MinimumVersion=0x%08X has no established result for this module lr=0x%08X",
                    name.c_str(),minimum,uint32_t(c.lr));
    uint32_t handle=0;
    const uint32_t status=secondary_load(c,base,*r,*m,*aot,name,flags,&handle);
    if(status==nt::kSuccess) put32(r->mem->host(output),handle);
    c.r3.u64=status;
}

// XexUnloadImage (0x01A1): NTSTATUS (HMODULE). Releases one reference taken
// by XexLoadImage. The bootstrap reference of a module is never released:
// unloading the running title or a system module has no meaning in R-comp and
// traps. Unknown handle: STATUS_INVALID_HANDLE (Xenia). A secondary module's
// last reference runs DllMain(handle, DLL_PROCESS_DETACH, 0), unlinks and frees
// its LDR entry and header copy, decommits its image and releases the
// references it held on its dependencies (src/aot_modules.cpp).
void XexUnloadImage(PPCContext& c,uint8_t* base) {
    Runtime* r=runtime(); ModuleState* m=state();
    const uint32_t handle=c.r3.u32;
    const int index=m && handle ? module_index(*m,handle) : -1;
    if(index<0 && m && handle) {
        std::lock_guard<std::recursive_mutex> lock(secondary_loader_lock());
        if(SecondaryModule* s=secondary_by_handle(*m,handle)) {c.r3.u64=secondary_unload(c,base,*r,*m,*s);return;}
    }
    if(index<0) {c.r3.u64=nt::kInvalidHandle;return;}
    uint8_t* ldr=r->mem->base()+handle;
    bool last=false;
    {
        std::lock_guard<std::mutex> lock(g_load_count_mutex);
        const uint16_t count=get16(ldr+0x40);
        last=count<=1;
        if(!last) put16(ldr+0x40,(uint16_t)(count-1));
    }
    if(last)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XexUnloadImage(%s) would release the module's last (bootstrap) reference lr=0x%08X",
                    m->records[index].name.c_str(),uint32_t(c.lr));
    c.r3.u64=nt::kSuccess;
}
} // namespace

static Status prepare_main_module(const ModuleConfig& config) {
    Runtime* r=runtime(); if(!r) return Status::NotInitialized;
    if(r->modules || r->static_tls_frozen.load()) return Status::Conflict;
    if(config.guest_path.empty() || config.guest_path.size()>1023 || config.command_line.size()>4095 ||
       !ascii(config.guest_path) || !ascii(config.command_line)) return Status::InvalidArgument;
    // Guest device-qualified path only, never an absolute host path.
    const size_t colon=config.guest_path.find(':');
    if(colon==std::string::npos || colon<2 || config.guest_path.find(':',colon+1)!=std::string::npos)
        return Status::InvalidArgument;
    const size_t slash=config.guest_path.find_last_of("/\\");
    const std::string name=config.guest_path.substr(slash==std::string::npos ? colon+1 : slash+1);
    if(name.empty() || same_name(name,kModuleXboxkrnl) || same_name(name,kModuleXam)) return Status::InvalidArgument;
    for(const auto& v:vars) {uint32_t old=0;if(find_variable_import(kModuleXboxkrnl,v.ordinal,&old)) return Status::AlreadyExists;}
    auto m=std::make_unique<ModuleState>();m->generation=r->generation;
    m->records[0]={config.guest_path,name,0};m->records[1]={kModuleXboxkrnl,kModuleXboxkrnl,0};m->records[2]={kModuleXam,kModuleXam,0};
    uint32_t block=0;Status result=r->heap.alloc(kStorageBytes,kStorageBytes,true,&block);
    if(result!=Status::Ok) return result;
    m->storage=block;uint8_t* p=r->mem->base()+block;
    memcpy(p+kCommandOffset,config.command_line.c_str(),config.command_line.size()+1);
    uint32_t cursor=0x2000;
    for(size_t i=0;i<m->records.size();++i) {
        auto& rec=m->records[i];rec.handle=block+0x100+(uint32_t)i*0x80;
        uint8_t* ldr=r->mem->base()+rec.handle;
        descriptor(ldr+0x24,block+cursor,rec.path);utf16(p+cursor,rec.path);cursor+=(uint32_t)(2*rec.path.size()+2);
        descriptor(ldr+0x2C,block+cursor,rec.name);utf16(p+cursor,rec.name);cursor+=(uint32_t)(2*rec.name.size()+2);
        put16(ldr+0x40,1);put16(ldr+0x42,(uint16_t)i); // one bootstrap-owned reference
    }
    // Only the verified load-order links are exposed as a real circular list.
    for(size_t i=0;i<m->records.size();++i) {
        uint8_t* ldr=r->mem->base()+m->records[i].handle;
        put32(ldr,m->records[(i+1)%3].handle);put32(ldr+4,m->records[(i+2)%3].handle);
    }
    unsigned registered=0;
    for(;registered<4;++registered) {
        const auto& v=vars[registered];
        result=register_variable_import(kModuleXboxkrnl,v.ordinal,block+v.offset,v.name);
        if(result!=Status::Ok) break;
    }
    if(result!=Status::Ok) {
        for(unsigned i=0;i<registered;++i) unregister_variable_import(kModuleXboxkrnl,vars[i].ordinal);
        if(r->heap.free(block)!=Status::Ok) r->modules=std::move(m); // retain owner on cleanup failure
        return result;
    }
    r->modules=std::move(m);return Status::Ok;
}

static Status finalize_main_module(const XexImage& image) {
    Runtime* r=runtime();if(!r || !r->modules) return Status::NotInitialized;
    ModuleState& m=*r->modules;
    if(m.ready || m.header_storage || r->static_tls_frozen.load()) return Status::Conflict;
    if(image.header.size()>kMaximumHeader) return Status::Unsupported;
    std::vector<XexHeaderField> fields;
    Status result=validate_xex_header(image.header.data(),image.header.size(),&fields);
    if(result!=Status::Ok || be32(image.header.data()+8)!=image.header.size()) return Status::InvalidArgument;
    const uint32_t security=be32(image.header.data()+16);
    uint32_t base=be32(image.header.data()+security+0x110),entry=0;
    if(const auto* f=field(fields,0x10201)) base=f->value;
    if(const auto* f=field(fields,0x10100)) entry=f->value;
    if(base!=image.base || entry!=image.entry_point || be32(image.header.data()+security+4)!=image.size)
        return Status::InvalidArgument;
    auto overlaps=[&](const GuestHeap& heap) { return uint64_t(image.base)<heap.hi() && uint64_t(heap.lo())<uint64_t(image.base)+image.size; };
    if(overlaps(r->heap)||overlaps(r->physical)) return Status::Conflict;
    PeInfo pe;result=module_parse_pe(*r,image,&pe);if(result!=Status::Ok) return result;
    // Names in the XEX-specific PE directory need an independently established
    // relative-address contract before this parser can claim to load them.
    if(field(fields,0xE10402)) return Status::Unsupported;
    std::vector<ModuleState::Export> exports;
    result=parse_ordinal_exports(*r,image,pe,&exports);if(result!=Status::Ok) return result;
    std::vector<XexImage::ImportThunk> thunks=image.import_thunks;
    std::set<uint32_t> thunk_addresses;
    for(const auto& thunk:thunks) {
        if(!thunk_addresses.insert(thunk.address).second || (thunk.address&3) || thunk.address<image.base ||
           uint64_t(thunk.address)+16>uint64_t(image.base)+image.size) return Status::InvalidArgument;
        if(auto* expected=import_thunk(thunk.module.c_str(),thunk.ordinal)) {
            if(lookup_function(thunk.address)!=expected) return Status::Unsupported;
        }
    }
    if(!r->mem->is_accessible(m.storage,kStorageBytes,Protect::ReadWrite)) return Status::GuestFault;
    const uint32_t allocated=(uint32_t)((image.header.size()+0xFFFF)&~size_t(0xFFFF));
    uint32_t header=0;result=r->heap.alloc(allocated,0x10000,true,&header);if(result!=Status::Ok) return result;
    memcpy(r->mem->base()+header,image.header.data(),image.header.size());
    if(r->mem->protect(header,allocated,Protect::Read)!=MemStatus::Ok) {
        // A failed platform protection call may have affected part of a range.
        // Never return an inaccessible page to the heap's free list.
        if(r->mem->protect(header,allocated,Protect::ReadWrite)!=MemStatus::Ok || r->heap.free(header)!=Status::Ok)
            m.header_storage=header; // retained owner; retries are blocked
        return Status::OutOfMemory;
    }
    uint8_t* ldr=r->mem->base()+m.records[0].handle;
    put32(ldr+0x18,pe.headers);put32(ldr+0x1C,image.base);put32(ldr+0x20,pe.size);
    put32(ldr+0x38,image.size);put32(ldr+0x3C,image.entry_point);put32(ldr+0x44,image.base);
    put32(ldr+0x48,pe.checksum);put32(ldr+0x50,pe.timestamp);put32(ldr+0x58,header);
    m.header_storage=header;m.header_size=(uint32_t)image.header.size();m.fields=std::move(fields);
    m.thunks=std::move(thunks);m.main_exports=std::move(exports);
    if(const auto* f=field(m.fields,0x30000)) m.system_flags=f->value;
    m.image_base=image.base;m.image_size=image.size;
    m.ready=true;
    put32(r->mem->base()+m.storage+kHandleOffset,m.records[0].handle);
    return Status::Ok;
}

Status runtime_prepare_main_module(const ModuleConfig& config) {
#if defined(__cpp_exceptions)
    try { return prepare_main_module(config); } catch(const std::bad_alloc&) { return Status::OutOfMemory; }
#else
    return prepare_main_module(config);
#endif
}
Status runtime_finalize_main_module(const XexImage& image) {
#if defined(__cpp_exceptions)
    try { return finalize_main_module(image); } catch(const std::bad_alloc&) { return Status::OutOfMemory; }
#else
    return finalize_main_module(image);
#endif
}

void runtime_remove_module_bindings() {
    Runtime* r=runtime();if(!r || !r->modules) return;
    const uint32_t block=r->modules->storage;
    r->modules->ready=false;
    for(unsigned i=0;i<4;++i) {
        uint32_t address=0;
        if(find_variable_import(kModuleXboxkrnl,vars[i].ordinal,&address) && address==block+vars[i].offset)
            unregister_variable_import(kModuleXboxkrnl,vars[i].ordinal);
    }
    // The entire Runtime/heap is about to be destroyed. No individual free,
    // protection upgrade or new host allocation is required during teardown.
}
Status register_xboxkrnl_module_hle() {
    struct Impl {uint32_t ordinal;const char* name;PPCFunc* fn;};
    const Impl entries[]={{0x012B,"RtlImageXexHeaderField",&RtlImageXexHeaderField},
        {0x0194,"XexCheckExecutablePrivilege",&XexCheckExecutablePrivilege},
        {0x0195,"XexGetModuleHandle",&XexGetModuleHandle},{0x0196,"XexGetModuleSection",&XexGetModuleSection},
        {0x0197,"XexGetProcedureAddress",&XexGetProcedureAddress},
        {0x0199,"XexLoadImage",&XexLoadImage},{0x01A1,"XexUnloadImage",&XexUnloadImage}};
    for(const auto& i:entries) {
        uint32_t ordinal=0;
        if(!export_ordinal(kModuleXboxkrnl,i.name,&ordinal)||ordinal!=i.ordinal) return Status::InvalidArgument;
        Status s=register_import(kModuleXboxkrnl,i.ordinal,i.fn,i.name);if(s!=Status::Ok)return s;
    }
    return Status::Ok;
}
} // namespace rcomp::rt
