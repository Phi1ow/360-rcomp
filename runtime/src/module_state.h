#pragma once
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "rcomp/runtime/xex_loader.h"
#include "rcomp/runtime/status.h"
#include "rcomp/func_table.h"

struct PPCContext;

namespace rcomp {
struct AotModule;
}

namespace rcomp::rt {
struct Runtime;

// A secondary XEX module (title DLL) compiled into the title ahead of time and
// loaded by XexLoadImage (src/aot_modules.cpp, runtime/docs/MODULES.md).
struct SecondaryModule {
    const AotModule* aot = nullptr;  // registry descriptor (static data of the title)
    std::string path, name;          // LDR FullDllName / BaseDllName
    uint32_t handle = 0;             // HMODULE = guest LDR address (start of ldr_block)
    uint32_t ldr_block = 0;          // heap block: LDR entry + UTF-16BE names
    uint32_t header_storage = 0, header_size = 0, header_allocated = 0;  // protected header copy
    uint32_t module_flags = 0;
    std::vector<XexHeaderField> fields;
    uint32_t export_first = 0;               // first ordinal of the XEX export table
    std::vector<uint32_t> export_targets;    // [ordinal - first] -> guest address, 0 = hole
    std::vector<uint8_t> export_executable;  // target in an executable section
    std::vector<SecondaryModule*> dependencies;  // one reference held on each while loaded
    uint32_t load_count = 0;
    bool loading = false, loaded = false, unloading = false;
};

struct ModuleState {
    struct Record { std::string path, name; uint32_t handle=0; };
    struct Export { uint32_t ordinal=0,address=0; std::string name; bool executable=false; PPCFunc* function=nullptr; };
    uint64_t generation=0;
    bool ready=false;
    uint32_t storage=0,header_storage=0,header_size=0;
    uint32_t system_flags=0;
    uint32_t image_base=0,image_size=0;  // loaded main image (XEX security image_size)
    std::array<Record,3> records;
    std::vector<XexHeaderField> fields;
    std::vector<XexImage::ImportThunk> thunks;
    std::vector<Export> main_exports;
    // Secondary modules ever loaded in this runtime (entries are kept after the
    // last unload so that the slot and module index stay stable). Guarded by
    // the loader lock (secondary_loader_lock); `published` additionally by
    // export_mutex, the only lock taken by calls through module imports.
    std::vector<std::unique_ptr<SecondaryModule>> secondary;
    std::mutex export_mutex;
    std::vector<const SecondaryModule*> published;
};
void runtime_remove_module_bindings();

// PE metadata of a loaded XEX image (src/hle_xboxkrnl_modules.cpp).
struct PeInfo {
    uint32_t headers=0,size=0,checksum=0,timestamp=0;
    uint32_t export_rva=0,export_size=0;  // PE export directory (recorded when accepted)
    struct Section {uint64_t begin,end;bool executable;};
    std::vector<Section> sections;
};
// An ordinary PE export directory is Unsupported unless `unloaded_pe_exports_ok`,
// in which case it is only recorded in `pe` for the caller to judge.
Status module_parse_pe(Runtime& r,const XexImage& image,PeInfo* pe,bool unloaded_pe_exports_ok=false);

// Loader lock of the secondary modules: recursive, held across DllMain like the
// console/Windows loader lock (a DllMain may call XexLoadImage/XexGetModuleHandle).
std::recursive_mutex& secondary_loader_lock();
// HLE entry points of src/aot_modules.cpp, called by the Xex* exports with the
// loader lock held. Each returns an NTSTATUS; anything without an established
// contract is an explicit rcomp_fatal.
SecondaryModule* secondary_by_handle(ModuleState& m,uint32_t handle);
SecondaryModule* secondary_by_name(ModuleState& m,const std::string& name);
// `guest_path` names an existing file of the AOT module `aot`.
uint32_t secondary_load(PPCContext& ctx,uint8_t* base,Runtime& r,ModuleState& m,const AotModule& aot,
                        const std::string& guest_path,uint32_t flags,uint32_t* handle);
uint32_t secondary_add_reference(SecondaryModule& module);
uint32_t secondary_unload(PPCContext& ctx,uint8_t* base,Runtime& r,ModuleState& m,SecondaryModule& module);
// The disc-relative path of a device path on the game devices ("game:", "d:",
// optional "\??\" prefix), or false.
bool secondary_disc_path(const std::string& guest_path,std::string* disc_path);
// Export `ordinal` of the loaded secondary module imported as `module` (its
// import-library name, or that name with non-alphanumerics as '_').
bool module_import_target(const char* module,uint32_t ordinal,uint32_t* target,bool* executable);
} // namespace rcomp::rt
