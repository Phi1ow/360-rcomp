#include "rcomp/runtime/xex_loader.h"

#include <string.h>
#include <strings.h>

#include <string>
#include <vector>
#include <set>
#include <utility>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/import_registry.h"

namespace rcomp::rt {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

constexpr uint32_t kMagicXex2 = 0x58455832;
constexpr uint32_t kHeaderFileFormat = 0x000003FF;
constexpr uint32_t kHeaderEntryPoint = 0x00010100;
constexpr uint32_t kHeaderImageBase = 0x00010201;
constexpr uint32_t kHeaderImportLibraries = 0x000103FF;
constexpr uint32_t kHeaderTlsInfo = 0x00020104;
constexpr size_t kSecurityLoadAddress = 4 + 4 + 0x100 + 4 + 4;  // Xex2SecurityInfo.loadAddress
// Xex2ImportLibrary: size, digest[0x14], id, version, minVersion, name index (u16), count (u16).
constexpr size_t kImportLibraryHeader = 4 + 0x14 + 4 + 4 + 4 + 2 + 2;

#define LOGF(...) \
    do {                              \
        if (log) fprintf(log, __VA_ARGS__); \
    } while (0)

// Resolves the import records inside the loaded image [base, base+size).
Status apply_imports(uint8_t* image, uint32_t base, uint32_t size, const uint8_t* hdr, size_t hdr_len,
                     XexImage* out, FILE* log, const XexImportResolver* modules) {
    if (hdr_len < 12) return Status::InvalidArgument;
    const uint32_t total = be32(hdr), strtab_len = be32(hdr + 4), nlibs = be32(hdr + 8);
    if (total > hdr_len || uint64_t(12) + strtab_len > total) return Status::InvalidArgument;
    std::vector<std::string> names;
    for (size_t off = 0; off < strtab_len && names.size() < nlibs;) {
        const char* s = reinterpret_cast<const char*>(hdr + 12 + off);
        size_t len = strnlen(s, strtab_len - off);
        if (len == strtab_len - off) return Status::InvalidArgument;  // unterminated
        names.emplace_back(s, len);
        off += (len + 1 + 3) & ~size_t(3);
    }
    if (names.size() != nlibs) return Status::InvalidArgument;
    size_t off = 12 + strtab_len;
    for (uint32_t l = 0; l < nlibs; ++l) {
        if (off + kImportLibraryHeader > total) return Status::InvalidArgument;
        const uint8_t* lib = hdr + off;
        const uint32_t lib_size = be32(lib);
        const uint16_t name_index = be16(lib + 0x24), count = be16(lib + 0x26);
        if (name_index >= nlibs || lib_size < kImportLibraryHeader + 4u * count || off + lib_size > total)
            return Status::InvalidArgument;
        const char* module = names[name_index].c_str();
        const bool other_module =
            modules && strcasecmp(module, kModuleXboxkrnl) != 0 && strcasecmp(module, kModuleXam) != 0;
        for (uint16_t i = 0; i < count; ++i) {
            const uint32_t va = be32(lib + kImportLibraryHeader + 4 * i);
            if (va < base || uint64_t(va) + 4 > uint64_t(base) + size) {
                LOGF("rcomp xex: import record 0x%08X of %s outside the image\n", va, module);
                return Status::InvalidArgument;
            }
            uint8_t* rec = image + (va - base);
            const uint32_t word = be32(rec), type = word >> 24, ordinal = word & 0xFFFF;
            if (type == 1) {
                if ((va & 3u) || uint64_t(va) + 16 > uint64_t(base) + size)
                    return Status::InvalidArgument;
                out->import_thunks.push_back({module,ordinal,va});
                ++out->function_thunks;
                continue;
            }
            if (type != 0) {
                LOGF("rcomp xex: import record 0x%08X of %s has type %u\n", va, module, type);
                return Status::InvalidArgument;
            }
            uint32_t addr = 0;
            if (other_module) {
                if (!modules->resolve(names[name_index], ordinal, &addr)) {
                    LOGF("RCOMP-XEX module_import unresolved module=%s ordinal=0x%04X slot=0x%08X\n", module, ordinal, va);
                    return Status::NotFound;
                }
                put_be32(rec, addr);
                ++out->module_imports_resolved;
                continue;
            }
            const ExportKind kind = export_kind(module, ordinal);
            if (kind == ExportKind::Function) {
                put_be32(rec, kUnresolvedImportPoison(ordinal));
                ++out->function_slots;
            } else if (kind == ExportKind::Variable && find_variable_import(module, ordinal, &addr)) {
                put_be32(rec, addr);
                ++out->variables_resolved;
            } else {
                const char* name = export_name(module, ordinal);
                put_be32(rec, kUnresolvedImportPoison(ordinal));
                ++out->variables_unresolved;
                LOGF("RCOMP-XEX variable_import unresolved module=%s ordinal=0x%04X name=%s slot=0x%08X "
                     "poison=0x%08X\n",
                     module, ordinal, name ? name : "(not in any export table)", va, kUnresolvedImportPoison(ordinal));
            }
        }
        off += lib_size;
        ++out->import_libraries;
    }
    return Status::Ok;
}

}  // namespace

Status validate_xex_header(const uint8_t* x,size_t n,std::vector<XexHeaderField>* fields) {
    if(!x || n<24 || be32(x)!=kMagicXex2) return Status::InvalidArgument;
    const uint32_t bytes=be32(x+8),count=be32(x+20),security=be32(x+16);
    const uint64_t table_end=24ull+8ull*count;
    if(bytes>n || bytes<24 || table_end>bytes || security<table_end ||
       uint64_t(security)+kSecurityLoadAddress+4>bytes) return Status::InvalidArgument;
    std::set<uint32_t> seen;
    std::vector<XexHeaderField> result;
    for(uint32_t i=0;i<count;++i) {
        const uint32_t offset=24+8*i,key=be32(x+offset),value=be32(x+offset+4),words=key&255;
        if(!seen.insert(key).second) return Status::InvalidArgument;
        uint32_t field_offset=offset+4,length=4;
        if(words>1) {
            field_offset=value;
            if(value<table_end || uint64_t(value)+4>bytes || (value&3)) return Status::InvalidArgument;
            length=words==255 ? be32(x+value) : words*4;
            if(length<4 || uint64_t(value)+length>bytes) return Status::InvalidArgument;
        }
        result.push_back({key,value,field_offset,length});
    }
    if(fields) *fields=std::move(result);
    return Status::Ok;
}

Status validate_xex_tls(const XexTlsInfo& tls) {
    if (!tls.present)
        return (tls.slot_count || tls.raw_data_address || tls.data_size || tls.raw_data_size)
            ? Status::InvalidArgument : Status::Ok;
    if (tls.raw_data_size > tls.data_size) return Status::InvalidArgument;
    if (tls.raw_data_size && !tls.raw_data_address) return Status::InvalidArgument;
    // The pinned reference defaults to zeroed slots when slot_count is zero.
    // Nonempty data with zero slots has no supported verified semantics.
    if (!tls.slot_count && (tls.data_size || tls.raw_data_size)) return Status::Unsupported;
    const uint64_t total = uint64_t(tls.slot_count) * 4 + tls.data_size;
    if (total > kMaxStaticTlsBytes) return Status::Unsupported;
    return Status::Ok;
}

Status load_raw_image(GuestMemory& mem, uint32_t base, const uint8_t* image, size_t size) {
    if (!image || size == 0 || uint64_t(base) + size > kGuestSpaceSize) return Status::InvalidArgument;
    uint64_t lo = base & ~(kGuestPageSize - 1);
    uint64_t hi = (uint64_t(base) + size + kGuestPageSize - 1) & ~(kGuestPageSize - 1);
    // A second load, or an image over heap/runtime data, must not overwrite it.
    for (uint64_t p = lo; p < hi; p += kGuestPageSize)
        if (mem.is_committed(p, kGuestPageSize)) return Status::Conflict;
    MemStatus s = mem.commit(lo, hi - lo, Protect::ReadWrite);
    if (s == MemStatus::Conflict) return Status::Conflict;
    if (s != MemStatus::Ok) return Status::OutOfMemory;
    memcpy(mem.host(base), image, size);
    return Status::Ok;
}

Status load_xex_image(GuestMemory& mem, const uint8_t* x, size_t n, XexImage* out, FILE* log,
                      const XexImportResolver* modules) {
    if (!x || !out || n < 24 || be32(x) != kMagicXex2) return Status::InvalidArgument;
    Status header_status=validate_xex_header(x,n);
    if(header_status!=Status::Ok) return header_status;
    const uint32_t header_size = be32(x + 8), security_off = be32(x + 16), count = be32(x + 20);
    if (header_size > n || uint64_t(24) + uint64_t(count) * 8 > header_size ||
        uint64_t(security_off) + kSecurityLoadAddress + 4 > header_size)
        return Status::InvalidArgument;
    uint32_t entry = 0, base = be32(x + security_off + kSecurityLoadAddress);
    const uint32_t image_size = be32(x + security_off + 4);
    const uint8_t* imports = nullptr;
    size_t imports_len = 0;
    XexTlsInfo tls;
    bool have_format = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t key = be32(x + 24 + 8 * i), val = be32(x + 24 + 8 * i + 4);
        if (key == kHeaderEntryPoint) entry = val;
        else if (key == kHeaderImageBase) base = val;
        else if (key == kHeaderImportLibraries) {
            if (val >= header_size) return Status::InvalidArgument;
            imports = x + val;
            imports_len = header_size - val;
        } else if (key == kHeaderTlsInfo) {
            if (tls.present || val < uint64_t(24) + uint64_t(count) * 8 || uint64_t(val) + 16 > header_size) {
                LOGF("rcomp xex: duplicate or truncated static TLS header\n");
                return Status::InvalidArgument;
            }
            tls = {true, be32(x + val), be32(x + val + 4),
                   be32(x + val + 8), be32(x + val + 12)};
        } else if (key == kHeaderFileFormat) {
            if (uint64_t(val) + 8 > header_size) return Status::InvalidArgument;
            const uint16_t encryption = be16(x + val + 4), compression = be16(x + val + 6);
            if (encryption != 0 || compression != 0) {
                LOGF("rcomp xex: encryption=%u compression=%u not supported by the runtime loader "
                     "(decode on the host, then load_raw_image)\n", encryption, compression);
                return Status::Unsupported;
            }
            have_format = true;
        }
    }
    if (!have_format) {
        LOGF("rcomp xex: no file format header\n");
        return Status::Unsupported;
    }
    if (uint64_t(header_size) + image_size > n || image_size == 0) return Status::InvalidArgument;
    Status tls_status = validate_xex_tls(tls);
    if (tls_status != Status::Ok) {
        LOGF("rcomp xex: invalid/unsupported static TLS slots=%u data=%u raw=%u\n",
             tls.slot_count, tls.data_size, tls.raw_data_size);
        return tls_status;
    }
    if (tls.raw_data_size && (tls.raw_data_address < base ||
        uint64_t(tls.raw_data_address) + tls.raw_data_size > uint64_t(base) + image_size)) {
        LOGF("rcomp xex: static TLS template 0x%08X+%u outside image\n",
             tls.raw_data_address, tls.raw_data_size);
        return Status::InvalidArgument;
    }
    if (entry != 0 && (entry < base || uint64_t(entry) >= uint64_t(base) + image_size)) {
        LOGF("rcomp xex: entry point 0x%08X outside the image [0x%08X, +0x%X)\n", entry, base, image_size);
        return Status::InvalidArgument;
    }
    // Poisoned import slots point into guest page 0: it must stay uncommitted.
    if (base < kGuestPageSize) {
        LOGF("rcomp xex: image overlaps reserved import-poison page\n");
        return Status::Conflict;
    }
    if (mem.is_committed(0, 1)) return Status::Conflict;
    // Resolve imports on a copy so that a malformed table leaves memory untouched.
    std::vector<uint8_t> image(x + header_size, x + header_size + image_size);
    XexImage img{};
    img.header.assign(x,x+header_size);
    if (imports) {
        Status s = apply_imports(image.data(), base, image_size, imports, imports_len, &img, log, modules);
        if (s != Status::Ok) return s;
    }
    Status s = load_raw_image(mem, base, image.data(), image.size());
    if (s != Status::Ok) return s;
    img.base = base;
    img.size = image_size;
    img.entry_point = entry;
    img.tls = tls;
    *out = std::move(img);
    return Status::Ok;
}

}  // namespace rcomp::rt
