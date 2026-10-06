// AOT-compiled secondary XEX modules (title DLLs): the registry of
// include/rcomp/aot_modules.h and the loader behind XexLoadImage /
// XexUnloadImage. Contract, address-space decision and limitations:
// runtime/docs/MODULES.md ("AOT secondary modules").
#include "rcomp/aot_modules.h"

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "kernel_internal.h"
#include "module_state.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/modules.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/vfs.h"

namespace rcomp {
namespace {
// One registration of static descriptors per title (copied pointer).
std::mutex g_registry_mutex;
const AotModule* g_modules = nullptr;
size_t g_module_count = 0;

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
bool same_text(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
// Lower-case, '/'-separated, relative; no empty, "." or ".." component, no ':'.
bool normalize_disc_path(const char* in, std::string* out) {
    if (!in) return false;
    std::string s;
    for (const char* p = in; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7E || c == ':') return false;
        s.push_back(c == '\\' ? '/' : lower((char)c));
    }
    while (!s.empty() && s[0] == '/') s.erase(0, 1);
    if (s.empty() || s.size() > 255) return false;
    size_t start = 0;
    for (;;) {
        const size_t end = s.find('/', start);
        const std::string part = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    *out = s;
    return true;
}
// Import-library name as XenonRecomp spells it in PPC_UNRESOLVED_IMPORT
// (non-alphanumerics as '_'), lower-cased for an ASCII case-insensitive match.
std::string import_symbol_name(const char* name) {
    std::string s;
    for (const char* p = name; *p; ++p) {
        const char c = lower(*p);
        s.push_back((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_');
    }
    return s;
}
bool valid_module_name(const char* name) {
    if (!name || !*name || strlen(name) > 255) return false;
    for (const char* p = name; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c > 0x7E || c == '/' || c == '\\' || c == ':') return false;
    }
    return true;
}
uint64_t page_end(const AotModule& m) {
    return (uint64_t(m.image_base) + m.image_size + kGuestPageSize - 1) & ~(kGuestPageSize - 1);
}
bool valid_descriptor(const AotModule& m) {
    std::string path;
    if (!normalize_disc_path(m.disc_path, &path) || !valid_module_name(m.module_name)) return false;
    if (!m.image_base || (m.image_base & (kGuestPageSize - 1)) || !m.image_size ||
        uint64_t(m.image_base) + m.image_size > kGuestSpaceSize) return false;
    const uint64_t end = uint64_t(m.image_base) + m.image_size;
    if (m.entry_point && (m.entry_point < m.image_base || m.entry_point >= end)) return false;
    if (m.function_count && !m.functions) return false;
    for (size_t i = 0; i < m.function_count; ++i) {
        const FuncEntry& f = m.functions[i];
        if (!f.host || (f.guest & 3u) || f.guest < m.image_base || f.guest >= end) return false;
    }
    return true;
}
}  // namespace

bool register_aot_modules(const AotModule* modules, size_t count) {
    if (!count || !modules) return false;
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    if (g_modules) return false;  // one registration per title (rt::clear_aot_modules resets)
    for (size_t i = 0; i < count; ++i) {
        if (!valid_descriptor(modules[i])) return false;
        std::string pi, pj;
        normalize_disc_path(modules[i].disc_path, &pi);
        for (size_t j = 0; j < i; ++j) {
            normalize_disc_path(modules[j].disc_path, &pj);
            if (pi == pj) return false;
            if (same_text(modules[i].module_name, modules[j].module_name) ||
                import_symbol_name(modules[i].module_name) == import_symbol_name(modules[j].module_name))
                return false;
            // 64 KiB commit granularity: two images may not share a page either.
            if (modules[i].image_base < page_end(modules[j]) && modules[j].image_base < page_end(modules[i]))
                return false;
        }
    }
    g_modules = modules;
    g_module_count = count;
    return true;
}

const AotModule* find_aot_module(const char* disc_path) {
    std::string wanted, candidate;
    if (!normalize_disc_path(disc_path, &wanted)) return nullptr;
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    for (size_t i = 0; i < g_module_count; ++i)
        if (normalize_disc_path(g_modules[i].disc_path, &candidate) && candidate == wanted) return &g_modules[i];
    return nullptr;
}

const AotModule* aot_modules(size_t* count) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    if (count) *count = g_module_count;
    return g_modules;
}

}  // namespace rcomp

namespace rcomp::rt {
namespace {
constexpr uint32_t kModuleFlagDll = 0x8;  // XEX module flags: DLL module
constexpr uint32_t kDllProcessDetach = 0, kDllProcessAttach = 1;
constexpr uint32_t kHeaderEntryPoint = 0x10100, kHeaderImageBase = 0x10201, kHeaderImports = 0x103FF;
constexpr uint32_t kHeaderTls = 0x20104, kHeaderNamedExports = 0xE10402, kHeaderFileFormat = 0x3FF;
constexpr uint64_t kMaximumModuleFile = 256ull << 20;  // runtime policy
constexpr uint32_t kMaximumHeader = 16u << 20;

std::recursive_mutex g_loader;
std::mutex g_image_root_mutex;
std::string g_image_root;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        auto l = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
        if (l(a[i]) != l(b[i])) return false;
    }
    return true;
}
std::string symbol_name(const std::string& name) {
    std::string s;
    for (unsigned char c : name) {
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        s.push_back((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? (char)c : '_');
    }
    return s;
}
bool system_library(const std::string& name) { return same_name(name, kModuleXboxkrnl) || same_name(name, kModuleXam); }
const XexHeaderField* field(const std::vector<XexHeaderField>& fields, uint32_t key) {
    for (const auto& f : fields) if (f.key == key) return &f;
    return nullptr;
}
std::string base_name(const std::string& path) {
    const size_t cut = path.find_last_of("\\/:");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}
ModuleState* state() {
    Runtime* r = runtime();
    return r && r->modules && r->modules->ready && r->modules->generation == r->generation ? r->modules.get() : nullptr;
}
bool export_address(const SecondaryModule& s, uint32_t ordinal, uint32_t* address, bool* executable) {
    if (ordinal < s.export_first || ordinal - s.export_first >= s.export_targets.size()) return false;
    const uint32_t i = ordinal - s.export_first;
    if (!s.export_targets[i]) return false;  // hole in the ordinal table
    *address = s.export_targets[i];
    if (executable) *executable = s.export_executable[i] != 0;
    return true;
}

// Module imports bind against the module's dependencies only (loaded first).
class DependencyResolver final : public XexImportResolver {
public:
    explicit DependencyResolver(const std::vector<SecondaryModule*>& deps) : deps_(deps) {}
    bool resolve(const std::string& library, uint32_t ordinal, uint32_t* address) const override {
        for (const SecondaryModule* d : deps_)
            if (same_name(library, d->aot->module_name)) return export_address(*d, ordinal, address, nullptr);
        return false;
    }
private:
    const std::vector<SecondaryModule*>& deps_;
};

// Whole file through the guest VFS (its open status is the guest's answer).
uint32_t read_guest_file(Runtime& r, const std::string& guest_path, std::vector<uint8_t>* out) {
    uint32_t handle = 0;
    Status s = r.vfs.open(r.handles, guest_path, false, &handle);
    if (s != Status::Ok) return to_ntstatus(s);
    std::shared_ptr<GuestFile> file;
    s = r.handles.lookup_as<GuestFile>(handle, &file);
    if (s == Status::Ok && file->size() > kMaximumModuleFile) {
        r.handles.close(handle);
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): %llu-byte module exceeds the runtime's %llu-byte policy",
                    guest_path.c_str(), (unsigned long long)file->size(), (unsigned long long)kMaximumModuleFile);
    }
    if (s == Status::Ok) {
        out->resize(size_t(file->size()));
        size_t done = 0;
        while (s == Status::Ok && done < out->size()) {
            uint32_t got = 0;
            const uint32_t chunk = uint32_t(std::min<size_t>(out->size() - done, 1u << 24));
            s = file->read_at(done, out->data() + done, chunk, &got);
            if (s == Status::Ok && !got) s = Status::EndOfFile;
            done += got;
        }
    }
    r.handles.close(handle);
    return s == Status::Ok ? 0 : to_ntstatus(s);
}
bool read_host_file(const std::string& path, std::vector<uint8_t>* out) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st {};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && uint64_t(st.st_size) <= kMaximumModuleFile;
    if (ok) {
        out->resize(size_t(st.st_size));
        size_t done = 0;
        while (ok && done < out->size()) {
            const ssize_t n = ::read(fd, out->data() + done, out->size() - done);
            ok = n > 0;
            if (ok) done += size_t(n);
        }
    }
    ::close(fd);
    return ok;
}
// Offset of the file-format field (encryption, compression at +4/+6), or 0.
uint32_t file_format_offset(const std::vector<uint8_t>& x) {
    std::vector<XexHeaderField> fields;
    if (validate_xex_header(x.data(), x.size(), &fields) != Status::Ok) return 0;
    const XexHeaderField* f = field(fields, kHeaderFileFormat);
    return f && f->size >= 8 ? f->value : 0;
}
// The decoded (unencrypted, uncompressed) XEX of the module. A plain disc file
// is used as is. Otherwise the image decoded on the host at packaging time is
// read from the configured image root and must carry the disc file's exact
// header (only the encryption/compression words differ): the header holds the
// image digest, so the pair cannot be mixed up silently.
void decoded_image(const AotModule& aot, const std::string& guest_path, std::vector<uint8_t>& file,
                   std::vector<uint8_t>* plain) {
    const uint32_t ff = file_format_offset(file);
    if (!ff) rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): not a valid XEX2 file", guest_path.c_str());
    if (!be16(file.data() + ff + 4) && !be16(file.data() + ff + 6)) { plain->swap(file); return; }
    std::string root;
    {
        std::lock_guard<std::mutex> lock(g_image_root_mutex);
        root = g_image_root;
    }
    if (root.empty())
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): the disc module is encrypted/compressed (%u/%u) and no "
                    "host-decoded module image root is configured (runtime_configure_module_images)",
                    guest_path.c_str(), be16(file.data() + ff + 4), be16(file.data() + ff + 6));
    const std::string host = root + "/" + aot.disc_path;
    if (!read_host_file(host, plain))
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): decoded module image %s is missing or unreadable",
                    guest_path.c_str(), host.c_str());
    const uint32_t header = be32(file.data() + 8);
    const uint32_t plain_ff = file_format_offset(*plain);
    bool same = plain_ff == ff && plain->size() >= header && be32(plain->data() + 8) == header &&
                !be16(plain->data() + ff + 4) && !be16(plain->data() + ff + 6);
    for (uint32_t i = 0; same && i < header; ++i)
        if ((i < ff + 4 || i >= ff + 8) && (*plain)[i] != file[i]) same = false;
    if (!same)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): decoded image %s is not the plain form of the disc "
                    "file (header mismatch)", guest_path.c_str(), host.c_str());
}
bool import_library_names(const std::vector<uint8_t>& x, const std::vector<XexHeaderField>& fields,
                          std::vector<std::string>* names) {
    names->clear();
    const XexHeaderField* f = field(fields, kHeaderImports);
    if (!f) return true;
    if (f->size < 12) return false;
    const uint8_t* p = x.data() + f->value;
    const uint32_t strings = be32(p + 4), count = be32(p + 8);
    if (uint64_t(12) + strings > f->size) return false;
    for (size_t off = 0; off < strings && names->size() < count;) {
        const char* s = reinterpret_cast<const char*>(p + 12 + off);
        const size_t len = strnlen(s, strings - off);
        if (len == strings - off) return false;
        names->emplace_back(s, len);
        off += (len + 1 + 3) & ~size_t(3);
    }
    return names->size() == count;
}
// The image range must be free: not the runtime heaps, the physical windows,
// the main image, a runtime range, nor any committed page.
void check_range(Runtime& r, ModuleState& m, const AotModule& aot) {
    const uint64_t lo = aot.image_base, hi = (uint64_t(aot.image_base) + aot.image_size + kGuestPageSize - 1) & ~(kGuestPageSize - 1);
    auto overlaps = [&](uint64_t a, uint64_t b) { return lo < b && a < hi; };
    const char* what = nullptr;
    if (overlaps(r.heap.lo(), r.heap.hi())) what = "the guest heap";
    else if (overlaps(r.physical.lo(), r.physical.hi()) || hi > kPhysicalWindowBase) what = "the physical windows";
    else if (overlaps(m.image_base, uint64_t(m.image_base) + m.image_size)) what = "the main image";
    else if (r.mem->overlaps_runtime_range(lo, hi - lo)) what = "a runtime-reserved range";
    else
        for (uint64_t p = lo; p < hi && !what; p += kGuestPageSize)
            if (r.mem->is_committed(p, kGuestPageSize)) what = "committed guest memory";
    if (what)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): fixed image range [0x%08llX, 0x%08llX) overlaps %s",
                    aot.disc_path, (unsigned long long)lo, (unsigned long long)hi, what);
}
// The module's AOT functions in the global table: added once per title; a
// reload finds them all already registered with the same host functions.
void register_module_functions(const AotModule& aot) {
    size_t present = 0;
    for (size_t i = 0; i < aot.function_count; ++i) {
        PPCFunc* const current = lookup_function(aot.functions[i].guest);
        if (current == aot.functions[i].host) ++present;
        else if (current)
            rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): function 0x%08X is already registered to another host function",
                        aot.disc_path, aot.functions[i].guest);
    }
    if (present == aot.function_count) return;
    if (present || !add_functions(aot.functions, aot.function_count))
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): the module's %zu AOT functions cannot be added to the function table",
                    aot.disc_path, aot.function_count);
}
// XEX ordinal export table (security info +0x160), the main module's format
// (src/hle_xboxkrnl_modules.cpp) plus holes: a zero offset (the image's MZ
// header) is no export.
Status parse_exports(Runtime& r, const XexImage& image, const PeInfo& pe, SecondaryModule* sm) {
    sm->export_first = 0;
    sm->export_targets.clear();
    sm->export_executable.clear();
    const uint32_t security = be32(image.header.data() + 16);
    if (uint64_t(security) + 0x164 > image.header.size()) return Status::InvalidArgument;
    const uint32_t address = be32(image.header.data() + security + 0x160);
    if (!address) return Status::Ok;
    const uint64_t end = uint64_t(image.base) + image.size;
    if (address < image.base || uint64_t(address) + 0x2C > end) return Status::InvalidArgument;
    const uint8_t* p = r.mem->base() + address;
    const uint32_t count = be32(p + 0x24), first = be32(p + 0x28), highbase = be32(p + 0x20);
    if (first > 0xFFFF || count > 0x10000 - first || highbase > 0xFFFF || uint64_t(address) + 0x2C + uint64_t(count) * 4 > end)
        return Status::InvalidArgument;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t offset = be32(p + 0x2C + i * 4);
        if (!offset) { sm->export_targets.push_back(0); sm->export_executable.push_back(0); continue; }
        const uint64_t target = ((uint64_t)highbase << 16) + offset;
        if (target < image.base || target >= end) return Status::InvalidArgument;
        bool found = false, executable = false;
        for (const auto& s : pe.sections)
            if (target >= s.begin && target < s.end) {
                if (found) return Status::InvalidArgument;
                found = true;
                executable = s.executable;
            }
        if (!found) return Status::InvalidArgument;
        if (executable && !lookup_function((uint32_t)target)) {
            std::fprintf(stderr, "RCOMP-MODULE export_without_function module=%s ordinal=%u target=0x%08llX\n",
                         sm->aot->module_name, first + i, (unsigned long long)target);
            return Status::Unsupported;
        }
        sm->export_targets.push_back((uint32_t)target);
        sm->export_executable.push_back(executable ? 1 : 0);
    }
    sm->export_first = first;
    return Status::Ok;
}
// imagexex turns a DLL's PE exports into the XEX ordinal table and drops
// .edata from the loaded image; the PE header still describes it. The
// directory is provably not loaded when its RVA is beyond the payload
// (WaveShell-Xbox.dll, WavesLibDLL.dll of Halo 3) or when its 40-byte header
// lies inside a XEX resource (L360.dll / Q10.dll: the resource directory 0x2FF
// places xcli1000 at that RVA). Anything else could be a real loaded PE export
// table, which this loader does not interpret: Unsupported.
bool pe_exports_unloaded(const XexImage& image, const std::vector<XexHeaderField>& fields, const PeInfo& pe) {
    if (pe.export_rva >= image.size) return true;
    const XexHeaderField* f = field(fields, 0x2FF);
    if (!f || f->size < 4) return false;
    const uint64_t begin = uint64_t(image.base) + pe.export_rva, end = begin + 40;
    const uint8_t* directory = image.header.data() + f->value;
    for (uint32_t i = 0; i < (f->size - 4) / 16; ++i) {
        const uint8_t* entry = directory + 4 + i * 16;
        const uint64_t address = be32(entry + 8), bytes = be32(entry + 12);
        if (begin >= address && end <= address + bytes) return true;
    }
    return false;
}
void descriptor(uint8_t* p, uint32_t address, const std::string& text) {
    put16(p, (uint16_t)(text.size() * 2)); put16(p + 2, (uint16_t)(text.size() * 2 + 2)); put32(p + 4, address);
}
void utf16(uint8_t* p, const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) { p[2 * i] = 0; p[2 * i + 1] = (uint8_t)text[i]; }
    p[text.size() * 2] = p[text.size() * 2 + 1] = 0;
}
// LDR entries form one circular load-order list with the main records; a
// secondary module is linked at the tail (before the main module).
void link_entry(Runtime& r, ModuleState& m, uint32_t entry) {
    uint8_t* g = r.mem->base();
    const uint32_t head = m.records[0].handle, tail = be32(g + head + 4);
    put32(g + entry, head); put32(g + entry + 4, tail);
    put32(g + tail, entry); put32(g + head + 4, entry);
}
void unlink_entry(Runtime& r, uint32_t entry) {
    uint8_t* g = r.mem->base();
    const uint32_t next = be32(g + entry), previous = be32(g + entry + 4);
    put32(g + previous, next); put32(g + next + 4, previous);
    put32(g + entry, 0); put32(g + entry + 4, 0);
}
void publish(ModuleState& m, const SecondaryModule* sm, bool visible) {
    std::lock_guard<std::mutex> lock(m.export_mutex);
    auto& v = m.published;
    if (visible) v.push_back(sm);
    else for (size_t i = 0; i < v.size(); ++i) if (v[i] == sm) { v.erase(v.begin() + long(i)); break; }
}
// Releases the per-load guest allocations; the image pages are decommitted.
void release_storage(Runtime& r, SecondaryModule& sm) {
    if (sm.header_storage) {
        if (r.mem->protect(sm.header_storage, sm.header_allocated, Protect::ReadWrite) == MemStatus::Ok &&
            r.heap.free(sm.header_storage) == Status::Ok) sm.header_storage = 0;
        else
            std::fprintf(stderr, "RCOMP-MODULE header_retained module=%s address=0x%08X\n", sm.aot->module_name, sm.header_storage);
    }
    if (sm.ldr_block && r.heap.free(sm.ldr_block) == Status::Ok) sm.ldr_block = 0;
    const uint64_t lo = sm.aot->image_base;
    const uint64_t hi = (uint64_t(sm.aot->image_base) + sm.aot->image_size + kGuestPageSize - 1) & ~(kGuestPageSize - 1);
    if (r.mem->decommit(lo, hi - lo) != MemStatus::Ok)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "XexUnloadImage(%s): image range [0x%08llX, 0x%08llX) cannot be decommitted",
                    sm.aot->disc_path, (unsigned long long)lo, (unsigned long long)hi);
}
SecondaryModule* slot_for(ModuleState& m, const AotModule& aot) {
    for (auto& s : m.secondary) if (s->aot == &aot) return s.get();
    m.secondary.push_back(std::make_unique<SecondaryModule>());
    m.secondary.back()->aot = &aot;
    return m.secondary.back().get();
}
std::string guest_path_of(const AotModule& aot) {
    std::string path = "game:\\";
    for (const char* p = aot.disc_path; *p; ++p) path.push_back(*p == '/' ? '\\' : *p);
    return path;
}
}  // namespace

std::recursive_mutex& secondary_loader_lock() { return g_loader; }

void clear_aot_modules() {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_modules = nullptr;
    g_module_count = 0;
    std::lock_guard<std::mutex> root(g_image_root_mutex);
    g_image_root.clear();
}

Status runtime_configure_module_images(const std::string& host_root) {
    if (host_root.empty() || host_root[0] != '/' || host_root.find('\0') != std::string::npos) return Status::InvalidArgument;
    std::string root = host_root;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    std::lock_guard<std::mutex> lock(g_image_root_mutex);
    g_image_root = root;
    return Status::Ok;
}

bool secondary_disc_path(const std::string& guest_path, std::string* disc_path) {
    std::string s = guest_path;
    if (s.size() >= 4 && (s.compare(0, 4, "\\?" "?\\") == 0 || s.compare(0, 4, "/?" "?/") == 0)) s.erase(0, 4);
    const size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    const std::string device = s.substr(0, colon);
    if (!same_name(device, "game") && !same_name(device, "d")) return false;
    std::string rest = s.substr(colon + 1);
    for (char& c : rest) if (c == '\\') c = '/';
    while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
    if (rest.empty()) return false;
    *disc_path = rest;
    return true;
}

SecondaryModule* secondary_by_handle(ModuleState& m, uint32_t handle) {
    if (!handle) return nullptr;
    for (auto& s : m.secondary) if (s->loaded && s->handle == handle) return s.get();
    return nullptr;
}

SecondaryModule* secondary_by_name(ModuleState& m, const std::string& name) {
    std::string disc;
    const AotModule* aot = secondary_disc_path(name, &disc) ? find_aot_module(disc.c_str()) : nullptr;
    for (auto& s : m.secondary) {
        if (!s->loaded) continue;
        if (s->aot == aot || same_name(name, s->name) || same_name(name, s->path) || same_name(name, s->aot->module_name))
            return s.get();
    }
    return nullptr;
}

uint32_t secondary_add_reference(SecondaryModule& sm) {
    if (sm.unloading)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s) during its DllMain(DLL_PROCESS_DETACH) has no established result",
                    sm.aot->disc_path);
    if (sm.load_count == 0xFFFF) rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "XexLoadImage(%s) LoadCount overflow", sm.aot->disc_path);
    ++sm.load_count;
    put16(runtime()->mem->base() + sm.handle + 0x40, (uint16_t)sm.load_count);
    return nt::kSuccess;
}

uint32_t secondary_load(PPCContext& ctx, uint8_t* base, Runtime& r, ModuleState& m, const AotModule& aot,
                        const std::string& guest_path, uint32_t flags, uint32_t* handle) {
    const char* path = guest_path.c_str();
    SecondaryModule* sm = slot_for(m, aot);
    if (sm->loaded) {  // also while its DllMain(DLL_PROCESS_ATTACH) runs
        const uint32_t status = secondary_add_reference(*sm);
        *handle = sm->handle;
        return status;
    }
    if (sm->loading)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): circular module dependency (loader re-entered while loading it)", path);
    std::vector<uint8_t> file, x;
    const uint32_t opened = read_guest_file(r, guest_path, &file);
    if (opened) return opened;
    decoded_image(aot, guest_path, file, &x);
    std::vector<XexHeaderField> fields;
    if (validate_xex_header(x.data(), x.size(), &fields) != Status::Ok || be32(x.data() + 8) > kMaximumHeader)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): malformed XEX2 header", path);
    const uint32_t module_flags = be32(x.data() + 4), security = be32(x.data() + 16);
    if (!(module_flags & kModuleFlagDll))
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): module flags 0x%X are not a DLL; loading an executable image "
                    "has no established contract", path, module_flags);
    if (flags != UINT32_MAX && flags != module_flags)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): requested module type flags 0x%X differ from the image's 0x%X "
                    "(no established result)", path, flags, module_flags);
    uint32_t image_base = be32(x.data() + security + 0x110), entry = 0;
    if (const auto* f = field(fields, kHeaderImageBase)) image_base = f->value;
    if (const auto* f = field(fields, kHeaderEntryPoint)) entry = f->value;
    const uint32_t image_size = be32(x.data() + security + 4);
    if (image_base != aot.image_base || image_size != aot.image_size || entry != aot.entry_point)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): disc image base/size/entry 0x%08X/0x%X/0x%08X differ from the "
                    "AOT build's 0x%08X/0x%X/0x%08X", path, image_base, image_size, entry, aot.image_base, aot.image_size,
                    aot.entry_point);
    if (field(fields, kHeaderNamedExports))
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): named XEX exports (0xE10402) are not supported", path);
    if (field(fields, kHeaderTls))
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): static TLS of a secondary module has no established contract", path);
    check_range(r, m, aot);
    std::vector<std::string> libraries;
    if (!import_library_names(x, fields, &libraries))
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): malformed import libraries", path);

    // Dependencies first, as the console loader does: each one referenced once.
    sm->loading = true;
    std::vector<SecondaryModule*> deps;
    for (const std::string& library : libraries) {
        if (system_library(library)) continue;
        const AotModule* target = nullptr;
        size_t count = 0;
        const AotModule* all = aot_modules(&count);
        for (size_t i = 0; i < count; ++i) if (same_name(library, all[i].module_name)) target = &all[i];
        if (!target)
            rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): imports %s, which is not an AOT module of this title", path,
                        library.c_str());
        if (target == &aot) rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): the module imports itself", path);
        SecondaryModule* dep = slot_for(m, *target);
        bool listed = false;
        for (auto* d : deps) listed |= d == dep;
        if (listed) continue;
        uint32_t dep_handle = 0;
        const std::string dep_path = guest_path_of(*target);
        const uint32_t status = secondary_load(ctx, base, r, m, *target, dep_path, UINT32_MAX, &dep_handle);
        if (status)
            rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): dependency %s cannot be opened (0x%08X)", path,
                        dep_path.c_str(), status);
        deps.push_back(dep);
    }

    register_module_functions(aot);
    DependencyResolver resolver(deps);
    XexImage image;
    const Status loaded = load_xex_image(*r.mem, x.data(), x.size(), &image, stderr, &resolver);
    if (loaded != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): image load failed: %s", path, status_name(loaded));
    if (image.variables_unresolved)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): %u unresolved variable imports (RCOMP-XEX lines)", path,
                    image.variables_unresolved);
    for (const auto& thunk : image.import_thunks) {
        if (system_library(thunk.module)) {
            PPCFunc* expected = import_thunk(thunk.module.c_str(), thunk.ordinal);
            if (expected && lookup_function(thunk.address) != expected)
                rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): %s ordinal 0x%X thunk 0x%08X is not mapped to its __imp__ "
                            "wrapper", path, thunk.module.c_str(), thunk.ordinal, thunk.address);
            continue;
        }
        uint32_t target = 0;
        if (!resolver.resolve(thunk.module, thunk.ordinal, &target))
            rcomp_fatal(RCOMP_FATAL_MISSING_IMPORT, "XexLoadImage(%s): %s has no export ordinal %u (thunk 0x%08X)", path,
                        thunk.module.c_str(), thunk.ordinal, thunk.address);
        if (!lookup_function(thunk.address))
            rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): import thunk 0x%08X (%s ordinal %u) has no AOT function", path,
                        thunk.address, thunk.module.c_str(), thunk.ordinal);
    }
    PeInfo pe;
    Status parsed = module_parse_pe(r, image, &pe, true);
    if (parsed == Status::Ok && (pe.export_rva || pe.export_size) && !pe_exports_unloaded(image, fields, pe)) {
        std::fprintf(stderr, "RCOMP-MODULE pe_export_directory_loaded module=%s rva=0x%X size=0x%X\n", aot.module_name,
                     pe.export_rva, pe.export_size);
        parsed = Status::Unsupported;
    }
    if (parsed == Status::Ok) parsed = parse_exports(r, image, pe, sm);
    if (parsed != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): PE/export metadata rejected: %s", path, status_name(parsed));

    // Guest metadata: protected header copy and the LDR entry with its names.
    const uint32_t header_size = (uint32_t)image.header.size();
    const uint32_t allocated = (header_size + 0xFFFFu) & ~0xFFFFu;
    uint32_t header = 0, block = 0;
    if (r.heap.alloc(allocated, 0x10000, true, &header) != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): no guest memory for the header copy", path);
    memcpy(r.mem->base() + header, image.header.data(), header_size);
    if (r.mem->protect(header, allocated, Protect::Read) != MemStatus::Ok)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "XexLoadImage(%s): header copy cannot be protected", path);
    const std::string name = base_name(guest_path);
    const uint32_t ldr_size = (uint32_t)((0x80 + 2 * guest_path.size() + 2 + 2 * name.size() + 2 + 15) & ~size_t(15));
    if (r.heap.alloc(ldr_size, 16, true, &block) != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): no guest memory for the LDR entry", path);
    uint8_t* ldr = r.mem->base() + block;
    const uint32_t path_text = block + 0x80, name_text = path_text + uint32_t(2 * guest_path.size() + 2);
    descriptor(ldr + 0x24, path_text, guest_path); utf16(r.mem->base() + path_text, guest_path);
    descriptor(ldr + 0x2C, name_text, name); utf16(r.mem->base() + name_text, name);
    put32(ldr + 0x18, pe.headers); put32(ldr + 0x1C, image.base); put32(ldr + 0x20, pe.size);
    put32(ldr + 0x38, image.size); put32(ldr + 0x3C, image.entry_point); put32(ldr + 0x44, image.base);
    put32(ldr + 0x48, pe.checksum); put32(ldr + 0x50, pe.timestamp); put32(ldr + 0x58, header);
    size_t index = 0;
    while (m.secondary[index].get() != sm) ++index;
    put16(ldr + 0x40, 1); put16(ldr + 0x42, (uint16_t)(m.records.size() + index));
    link_entry(r, m, block);

    sm->path = guest_path; sm->name = name; sm->handle = block; sm->ldr_block = block;
    sm->header_storage = header; sm->header_size = header_size; sm->header_allocated = allocated;
    sm->module_flags = module_flags; sm->fields = std::move(fields); sm->dependencies = std::move(deps);
    sm->load_count = 1; sm->loaded = true;
    publish(m, sm, true);
    std::fprintf(stderr, "RCOMP-MODULE loaded module=%s path=%s base=0x%08X size=0x%X handle=0x%08X exports=%zu imports_from_modules=%u\n",
                 aot.module_name, path, image.base, image.size, block, sm->export_targets.size(), image.module_imports_resolved);
    if (entry) {
        call_guest_routine_on_current_context(ctx, base, entry, block, kDllProcessAttach, 0, "DllMain(DLL_PROCESS_ATTACH)");
        if (!ctx.r3.u32)
            rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexLoadImage(%s): DllMain(DLL_PROCESS_ATTACH) returned FALSE; the console's "
                        "failure path has no established contract", path);
    }
    sm->loading = false;
    *handle = block;
    return nt::kSuccess;
}

uint32_t secondary_unload(PPCContext& ctx, uint8_t* base, Runtime& r, ModuleState& m, SecondaryModule& sm) {
    if (!sm.loaded) return nt::kInvalidHandle;
    if (sm.load_count > 1) {
        --sm.load_count;
        put16(r.mem->base() + sm.handle + 0x40, (uint16_t)sm.load_count);
        return nt::kSuccess;
    }
    if (sm.loading || sm.unloading)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "XexUnloadImage(%s): last reference released while the module is still being "
                    "loaded or unloaded (no established contract)", sm.aot->disc_path);
    sm.unloading = true;  // re-entry guard during DLL_PROCESS_DETACH
    if (sm.aot->entry_point)
        call_guest_routine_on_current_context(ctx, base, sm.aot->entry_point, sm.handle, kDllProcessDetach, 0,
                                              "DllMain(DLL_PROCESS_DETACH)");
    publish(m, &sm, false);
    put16(r.mem->base() + sm.handle + 0x40, 0);
    unlink_entry(r, sm.handle);
    sm.loaded = false;
    sm.load_count = 0;
    std::fprintf(stderr, "RCOMP-MODULE unloaded module=%s handle=0x%08X\n", sm.aot->module_name, sm.handle);
    sm.handle = 0;
    release_storage(r, sm);
    sm.export_targets.clear();
    sm.export_executable.clear();
    sm.fields.clear();
    std::vector<SecondaryModule*> deps;
    deps.swap(sm.dependencies);
    sm.unloading = false;
    for (size_t i = deps.size(); i-- > 0;) secondary_unload(ctx, base, r, m, *deps[i]);
    return nt::kSuccess;
}

bool module_import_target(const char* module, uint32_t ordinal, uint32_t* target, bool* executable) {
    ModuleState* m = state();
    if (!m || !module) return false;
    const std::string wanted = module, wanted_symbol = symbol_name(module);
    std::lock_guard<std::mutex> lock(m->export_mutex);
    for (const SecondaryModule* s : m->published)
        if (same_name(wanted, s->aot->module_name) || wanted_symbol == symbol_name(s->aot->module_name))
            return export_address(*s, ordinal, target, executable);
    return false;
}

}  // namespace rcomp::rt

void rcomp_module_import(PPCContext& ctx, uint8_t* base, const char* module, uint32_t ordinal) {
    uint32_t target = 0;
    bool executable = false;
    if (rcomp::rt::module_import_target(module, ordinal, &target, &executable)) {
        PPCFunc* fn = executable ? rcomp::lookup_function(target) : nullptr;
        if (!fn)
            rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET, "module import %s ordinal %u -> 0x%08X is not a recompiled function lr=0x%08llX",
                        module, ordinal, target, (unsigned long long)ctx.lr);
        fn(ctx, base);  // the console thunk is `mtctr; bctr`: lr and arguments unchanged
        return;
    }
    rcomp_fatal(RCOMP_FATAL_MISSING_IMPORT,
                "module=%s ordinal=0x%04X name=(not an export of a loaded AOT module) lr=0x%08llX r3=0x%08X r4=0x%08X "
                "r5=0x%08X r6=0x%08X",
                module ? module : "(null)", ordinal, (unsigned long long)ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
