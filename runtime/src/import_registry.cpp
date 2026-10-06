#include <cstdio>
#include <vector>
#include <atomic>
#include <algorithm>
#include "rcomp/runtime/import_registry.h"

#include <string.h>

#include <map>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#include "rcomp/diag.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/wait_stats.h"
#include "thread_suspend.h"

namespace rcomp::rt {

namespace {

struct ExportRow {
    uint32_t ordinal;
    const char* name;
    ExportKind kind;
};

// Xenia export tables as vendored by XenonRecomp (BSD, notice kept in the
// .inc files): names for diagnostics, name->ordinal lookup, export kind.
constexpr ExportKind kFunction = ExportKind::Function;
constexpr ExportKind kVariable = ExportKind::Variable;
#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) {ORDINAL, #NAME, TYPE}
const ExportRow kXboxkrnlExports[] = {
#include "xbox/xboxkrnl_table.inc"
};
const ExportRow kXamExports[] = {
#include "xbox/xam_table.inc"
};
#undef XE_EXPORT

bool module_is(const char* a, const char* b) { return a && strcasecmp(a, b) == 0; }

const ExportRow* table_for(const char* module, size_t* n) {
    if (module_is(module, kModuleXboxkrnl)) {
        *n = sizeof kXboxkrnlExports / sizeof kXboxkrnlExports[0];
        return kXboxkrnlExports;
    }
    if (module_is(module, kModuleXam)) {
        *n = sizeof kXamExports / sizeof kXamExports[0];
        return kXamExports;
    }
    *n = 0;
    return nullptr;
}

std::string key_module(const char* m) {
    std::string s = m ? m : "";
    for (auto& c : s) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return s;
}

struct Entry {
    PPCFunc* fn;
    const char* registry_name;
};

std::mutex g_mu;

// Lock-free read side of the registry for xboxkrnl.exe and xam.xex: guest code
// dispatches millions of imports per second (spin locks, IRQL, critical
// sections), so dispatch must not take g_mu or build a key. Writers (register,
// unregister, clear) update the map and this table under g_mu; readers only
// load one atomic pointer. Ordinals outside the table use the map.
constexpr uint32_t kFastOrdinals = 0x1000;
std::atomic<PPCFunc*> g_fast[2][kFastOrdinals];
int fast_module(const char* module) {
    if (module_is(module, kModuleXboxkrnl)) return 0;
    if (module_is(module, kModuleXam)) return 1;
    return -1;
}
void set_fast(const char* module, uint32_t ordinal, PPCFunc* fn) {
    const int m = fast_module(module);
    if (m >= 0 && ordinal < kFastOrdinals) g_fast[m][ordinal].store(fn, std::memory_order_release);
}

std::map<std::pair<std::string, uint32_t>, Entry>& registry() {
    static std::map<std::pair<std::string, uint32_t>, Entry> r;
    return r;
}

struct VarEntry {
    uint32_t guest_address;
    const char* registry_name;
};
std::map<std::pair<std::string, uint32_t>, VarEntry>& var_registry() {
    static std::map<std::pair<std::string, uint32_t>, VarEntry> r;
    return r;
}

}  // namespace

const char* export_name(const char* module, uint32_t ordinal) {
    size_t n;
    const ExportRow* t = table_for(module, &n);
    for (size_t i = 0; i < n; ++i)
        if (t[i].ordinal == ordinal) return t[i].name;
    return nullptr;
}

ExportKind export_kind(const char* module, uint32_t ordinal) {
    size_t n;
    const ExportRow* t = table_for(module, &n);
    for (size_t i = 0; i < n; ++i)
        if (t[i].ordinal == ordinal) return t[i].kind;
    return ExportKind::Unknown;
}

bool export_ordinal(const char* module, const char* name, uint32_t* ordinal) {
    size_t n;
    const ExportRow* t = table_for(module, &n);
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(t[i].name, name) == 0) {
            *ordinal = t[i].ordinal;
            return true;
        }
    }
    return false;
}

Status register_import(const char* module, uint32_t ordinal, PPCFunc* fn,
                       const char* registry_name) {
    if (!module || !fn || !registry_name) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(g_mu);
    auto key = std::make_pair(key_module(module), ordinal);
    auto it = registry().find(key);
    if (it != registry().end()) return it->second.fn == fn ? Status::Ok : Status::AlreadyExists;
    registry()[key] = {fn, registry_name};
    set_fast(module, ordinal, fn);
    return Status::Ok;
}

Status unregister_import(const char* module, uint32_t ordinal) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!registry().erase(std::make_pair(key_module(module), ordinal))) return Status::NotFound;
    set_fast(module, ordinal, nullptr);
    return Status::Ok;
}

PPCFunc* find_import(const char* module, uint32_t ordinal) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = registry().find(std::make_pair(key_module(module), ordinal));
    return it == registry().end() ? nullptr : it->second.fn;
}

const char* import_registry_name(const char* module, uint32_t ordinal) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = registry().find(std::make_pair(key_module(module), ordinal));
    return it == registry().end() ? nullptr : it->second.registry_name;
}

void clear_imports() {
    std::lock_guard<std::mutex> lock(g_mu);
    registry().clear();
    var_registry().clear();
    for (auto& module : g_fast)
        for (auto& slot : module) slot.store(nullptr, std::memory_order_release);
}

Status register_variable_import(const char* module, uint32_t ordinal, uint32_t guest_address,
                                const char* registry_name) {
    if (!module || !registry_name || guest_address == 0) return Status::InvalidArgument;
    if (export_kind(module, ordinal) != ExportKind::Variable) return Status::InvalidArgument;
    #if defined(__cpp_exceptions)
    try {
    #endif
    std::lock_guard<std::mutex> lock(g_mu);
    auto key = std::make_pair(key_module(module), ordinal);
    auto it = var_registry().find(key);
    if (it != var_registry().end())
        return it->second.guest_address == guest_address ? Status::Ok : Status::AlreadyExists;
    var_registry()[key] = {guest_address, registry_name};
    return Status::Ok;
    #if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) { return Status::OutOfMemory; }
    #endif
}

bool find_variable_import(const char* module, uint32_t ordinal, uint32_t* guest_address) {
    std::lock_guard<std::mutex> lock(g_mu);
    // Bootstrap rollback and shutdown must not allocate a temporary string.
    for (const auto& item : var_registry()) if (item.first.second == ordinal && module_is(module,item.first.first.c_str())) {
        if (guest_address) *guest_address = item.second.guest_address;
        return true;
    }
    return false;
}

Status unregister_variable_import(const char* module, uint32_t ordinal) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto it=var_registry().begin();it!=var_registry().end();++it)
        if (it->first.second == ordinal && module_is(module,it->first.first.c_str())) {
            var_registry().erase(it); return Status::Ok;
        }
    return Status::NotFound;
}

namespace {
// The import the calling thread is executing (module << 12 | ordinal, 0 = none): the PC sampler of the diagnostic titles reads it from its signal handler to
// attribute wall time, blocked time included, to HLE calls. Only the owning thread writes it; the handler runs on that same thread.
thread_local uint32_t t_current_import = 0;
// NtSuspendThread's flag for the calling worker (src/thread_suspend.h); null on other threads.
thread_local const std::atomic<uint32_t>* t_suspend_request = nullptr;
}  // namespace

void set_current_thread_suspend_request(const std::atomic<uint32_t>* flag) { t_suspend_request = flag; }

void dispatch_import_fast(ImportModule module, const ImportId& id, PPCContext& ctx, uint8_t* base) {
    // A suspended worker stops here, before its next kernel call runs.
    if (const std::atomic<uint32_t>* suspend = t_suspend_request;
        suspend && __builtin_expect(suspend->load(std::memory_order_relaxed) != 0, 0))
        runtime_suspend_checkpoint();
#if RCOMP_RUNTIME_WAIT_STATS
    thread_cpu_sample_tick();  // the per-thread CPU report of the frame-rate counter (wait_stats.h)
#endif
    PPCFunc* fn = id.ordinal < kFastOrdinals
        ? g_fast[static_cast<unsigned>(module)][id.ordinal].load(std::memory_order_acquire)
        : find_import(id.module, id.ordinal);
    if (!fn) hle_missing_import(id, ctx);
    const uint32_t outer = t_current_import;  // an import may run guest code that imports again
    t_current_import = (static_cast<uint32_t>(module) << 12) | (id.ordinal & 0xFFFu);
    fn(ctx, base);
    t_current_import = outer;
}

// Read by the PC sampler's signal handler (cpu/runtime/indirect.cpp, weak reference): async-signal-safe, a plain read of the interrupted thread's own variable.
extern "C" uint32_t rcomp_sampler_current_import(void) { return t_current_import; }

void dispatch_import(const ImportId& id, PPCContext& ctx, uint8_t* base) {
    const int m = fast_module(id.module);
    if (m >= 0) {
        dispatch_import_fast(static_cast<ImportModule>(m), id, ctx, base);
        return;
    }
    PPCFunc* fn = find_import(id.module, id.ordinal);
    if (!fn) hle_missing_import(id, ctx);
    fn(ctx, base);
}

}  // namespace rcomp::rt

namespace rcomp {

void hle_missing_import(const ImportId& id, PPCContext& ctx) {
    const char* name = id.name;
    if (!name) name = rt::export_name(id.module, id.ordinal);
    rcomp_fatal(RCOMP_FATAL_MISSING_IMPORT,
                "module=%s ordinal=0x%04X name=%s lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X "
                "r6=0x%08X",
                id.module ? id.module : "?", id.ordinal, name ? name : "?", (uint32_t)ctx.lr,
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

}  // namespace rcomp
