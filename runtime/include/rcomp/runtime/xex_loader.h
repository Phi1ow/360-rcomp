// XEX2 image loader (PRIME, M6 preparation).
//
// Maps the PE image of an *unencrypted, uncompressed* XEX2 into guest memory
// at its image base, as the Xbox 360 loader would before running the title.
// XenonRecomp translates the code; the image still has to be present in guest
// memory for data sections (.rdata/.data), import thunk addresses and any
// code-as-data reads. Encrypted or compressed XEX files (retail titles) are
// reported as Unsupported: for those the decoded image must be produced on the
// host (XenonRecomp already decodes it) and loaded with load_raw_image().
//
// Import records (XEX2 import libraries, one 32-bit record per import in the
// image: type << 24 | ordinal):
//   * type 1 = function thunk. XenonRecomp replaced the thunk by a direct call
//     to __imp__<Name> in the generated code; the bytes are left as they are.
//   * type 0 = import slot. For a *variable* export the loader writes the
//     guest address registered with register_variable_import(); if none is
//     registered (or the module/ordinal is unknown) it writes the poison
//     address kUnresolvedImportPoison(ordinal), inside guest page 0 which is
//     never committed: any access through it faults instead of reading
//     garbage, and the address encodes the ordinal. Every unresolved variable
//     is also logged. Slots of *function* exports are only read by the
//     original thunks (not executed) and receive the poison too.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "rcomp/runtime/status.h"

namespace rcomp {
class GuestMemory;
}

namespace rcomp::rt {

// XEX_HEADER_TLS_INFO: static data followed by slot_count zeroed words.
struct XexTlsInfo {
    bool present = false;
    uint32_t slot_count = 0;
    uint32_t raw_data_address = 0;
    uint32_t data_size = 0;
    uint32_t raw_data_size = 0;
};
// Runtime resource policy, not an Xbox ABI limit.
constexpr uint32_t kMaxStaticTlsBytes = 16u << 20;
Status validate_xex_tls(const XexTlsInfo& tls);

struct XexImage {
    uint32_t base = 0;         // guest address of the image
    uint32_t size = 0;         // bytes
    uint32_t entry_point = 0;  // guest address (0 if the XEX has none), inside the image
    uint32_t import_libraries = 0;
    uint32_t function_thunks = 0;       // type 1 records
    uint32_t function_slots = 0;        // type 0 records of function exports (poisoned)
    uint32_t variables_resolved = 0;    // type 0 records filled from the registry
    uint32_t variables_unresolved = 0;  // type 0 records poisoned (logged)
    uint32_t module_imports_resolved = 0;  // type 0 records of other XEX modules (XexImportResolver)
    XexTlsInfo tls;
    // Exact validated header of the input being loaded (no commercial bytes
    // logged). Kept before relocations; owned host data until module bootstrap.
    std::vector<uint8_t> header;
    struct ImportThunk { std::string module; uint32_t ordinal, address; };
    std::vector<ImportThunk> import_thunks;
};

// XEX optional header entries: 00 immediate; 01 inline word; other fixed word
// counts or FF length-prefixed block. Reject duplicate keys and range overflow.
struct XexHeaderField { uint32_t key, value, offset, size; };
Status validate_xex_header(const uint8_t* bytes, size_t size,
                           std::vector<XexHeaderField>* fields = nullptr);

constexpr uint32_t kUnresolvedImportPoison(uint32_t ordinal) { return (ordinal & 0xFFFu) << 4; }

// Imports of libraries other than xboxkrnl.exe / xam.xex: the ordinal exports of
// other loaded XEX modules (AOT secondary modules, runtime/docs/MODULES.md).
// With a resolver, every type 0 record of such a library receives the export's
// guest address, as the console loader writes it; a library or ordinal the
// resolver does not know fails the load with NotFound (logged). Type 1 records
// are listed in XexImage::import_thunks as for the system libraries. Without a
// resolver (the main image) these records keep the poison behaviour above.
class XexImportResolver {
public:
    virtual ~XexImportResolver() = default;
    virtual bool resolve(const std::string& library, uint32_t ordinal, uint32_t* address) const = 0;
};

// `log` receives one line per unresolved variable import and the reason of
// an Unsupported/InvalidArgument result (nullptr: silent).
// Errors: InvalidArgument (malformed XEX, entry point or import record outside
// the image), Unsupported (encrypted/compressed, no file format header),
// Conflict (part of the image range, or guest page 0, is already committed).
Status load_xex_image(GuestMemory& mem, const uint8_t* xex, size_t xex_size, XexImage* out,
                      FILE* log = stderr, const XexImportResolver* modules = nullptr);
// Commits [base, base + size) (64 KiB granularity) and copies `image` there.
// Conflict if any page of the range is already committed.
Status load_raw_image(GuestMemory& mem, uint32_t base, const uint8_t* image, size_t size);

}  // namespace rcomp::rt
