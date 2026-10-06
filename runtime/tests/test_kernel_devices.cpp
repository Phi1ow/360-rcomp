// Device objects, share access, title symbolic links, scatter reads, IOCTL
// refusal, L2 locking and console-key signatures
// (src/hle_xboxkrnl_devices.cpp).
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/xboxkrnl_devices.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__IoCheckShareAccess);
PPC_EXTERN_FUNC(__imp__IoSetShareAccess);
PPC_EXTERN_FUNC(__imp__IoRemoveShareAccess);
PPC_EXTERN_FUNC(__imp__IoCreateDevice);
PPC_EXTERN_FUNC(__imp__IoDeleteDevice);
PPC_EXTERN_FUNC(__imp__IoDismountVolume);
PPC_EXTERN_FUNC(__imp__IoDismountVolumeByFileHandle);
PPC_EXTERN_FUNC(__imp__KeLockL2);
PPC_EXTERN_FUNC(__imp__KeUnlockL2);
PPC_EXTERN_FUNC(__imp__NtDeviceIoControlFile);
PPC_EXTERN_FUNC(__imp__NtReadFileScatter);
PPC_EXTERN_FUNC(__imp__ObCreateSymbolicLink);
PPC_EXTERN_FUNC(__imp__ObDeleteSymbolicLink);
PPC_EXTERN_FUNC(__imp__ObIsTitleObject);
PPC_EXTERN_FUNC(__imp__ObReferenceObject);
PPC_EXTERN_FUNC(__imp__ObDereferenceObject);
PPC_EXTERN_FUNC(__imp__StfsCreateDevice);
PPC_EXTERN_FUNC(__imp__StfsControlDevice);
PPC_EXTERN_FUNC(__imp__XeKeysConsolePrivateKeySign);
PPC_EXTERN_FUNC(__imp__XeKeysConsoleSignatureVerification);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__IoCompleteRequest);

using namespace rcomp;
using namespace rcomp::rt;

namespace {

constexpr uint32_t kSharingViolation = 0xC0000043u;
constexpr uint32_t kNotSupported = 0xC00000BBu;
constexpr uint32_t kInvalidDeviceRequest = 0xC0000010u;
constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kFileSize = 3 * kPage + 100;

uint8_t* g_base;
uint32_t g_scratch;  // 0x1000 bytes

enum : uint32_t {
    kIosb = 0x00,
    kOffset = 0x10,
    kTimeout = 0x18,
    kEventCell = 0x20,
    kOut = 0x24,
    kResult = 0x28,
    kShare = 0x30,
    kSegments = 0x40,
    kAnsiA = 0x80,
    kAnsiB = 0x88,
    kTextA = 0x100,
    kTextB = 0x200,
    kFileObjects = 0x300,
    kHash = 0x400,
    kSignature = 0x500,  // 0x228 bytes
};

uint32_t rd32(uint32_t address) {
    uint32_t v = 0;
    CHECK(guest_read_be32(address, &v));
    return v;
}

uint8_t pattern(uint32_t i) { return uint8_t((i * 7u + 3u) % 251u); }

uint32_t call(PPCFunc* fn, uint32_t a = 0, uint32_t b = 0, uint32_t c3 = 0, uint32_t d = 0, uint32_t e = 0,
              uint32_t f = 0, uint32_t g = 0, uint32_t h = 0) {
    PPCContext c{};
    c.r3.u64 = a; c.r4.u64 = b; c.r5.u64 = c3; c.r6.u64 = d;
    c.r7.u64 = e; c.r8.u64 = f; c.r9.u64 = g; c.r10.u64 = h;
    c.lr = 0x82001234;
    fn(c, g_base);
    return c.r3.u32;
}

// ANSI_STRING at `descriptor` naming text stored at `text`.
uint32_t ansi(uint32_t descriptor, uint32_t text, const std::string& s) {
    memcpy(g_base + g_scratch + text, s.data(), s.size());
    g_base[g_scratch + text + s.size()] = 0;
    const uint32_t d = g_scratch + descriptor;
    g_base[d] = uint8_t(s.size() >> 8); g_base[d + 1] = uint8_t(s.size());
    g_base[d + 2] = uint8_t((s.size() + 1) >> 8); g_base[d + 3] = uint8_t(s.size() + 1);
    CHECK(guest_write_be32(d + 4, g_scratch + text));
    return d;
}

uint32_t create_link(const std::string& link, const std::string& target) {
    return call(__imp__ObCreateSymbolicLink, ansi(kAnsiA, kTextA, link), ansi(kAnsiB, kTextB, target));
}

uint32_t delete_link(const std::string& link) {
    return call(__imp__ObDeleteSymbolicLink, ansi(kAnsiA, kTextA, link));
}

uint32_t create_device(const std::string& name, uint32_t extension, uint32_t* device) {
    CHECK(guest_write_be32(g_scratch + kOut, 0));
    const uint32_t st = call(__imp__IoCreateDevice, 0x82B0EC40u, extension,
                             name.empty() ? 0 : ansi(kAnsiA, kTextA, name), 61, 0x1000,
                             g_scratch + kOut);
    *device = rd32(g_scratch + kOut);
    return st;
}

uint32_t scatter(uint32_t handle, uint32_t event, uint32_t length, const uint64_t* offset) {
    memset(g_base + g_scratch + kIosb, 0xCC, 8);
    if (offset) CHECK(guest_write_be64(g_scratch + kOffset, *offset));
    return call(__imp__NtReadFileScatter, handle, event, 0, 0, g_scratch + kIosb, g_scratch + kSegments, length,
                offset ? g_scratch + kOffset : 0);
}

void share(uint8_t out[7]) { memcpy(out, g_base + g_scratch + kShare, 7); }

bool share_is(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f, uint8_t g) {
    uint8_t s[7];
    share(s);
    const uint8_t want[7] = {a, b, c, d, e, f, g};
    return memcmp(s, want, 7) == 0;
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/rcomp_rt_kdev_XXXXXX";
    std::string top = mkdtemp(tmpl) ? tmpl : "";
    if (top.empty()) return 2;
    const std::string game = top + "/game";
    CHECK(mkdir(game.c_str(), 0755) == 0);
    {
        FILE* f = fopen((game + "/data.bin").c_str(), "wb");
        CHECK(f != nullptr);
        for (uint32_t i = 0; f && i < kFileSize; ++i) fputc(pattern(i), f);
        if (f) fclose(f);
    }

    GuestMemory mem;
    if (mem.reserve() != MemStatus::Ok) return 2;
    g_base = mem.base();
    CHECK_ST(runtime_init(&mem), Status::Ok);
    Runtime& r = *runtime();
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(r.heap.alloc(0x1000, 16, true, &g_scratch), Status::Ok);
    CHECK(guest_write_be64(g_scratch + kTimeout, 0));
    CHECK_ST(r.vfs.mount("game", game), Status::Ok);

    // IRP completion has no IRP to act on: an explicit diagnostic, never a
    // missing import or a silent success (src/hle_xboxkrnl_more.cpp).
    {
        bool fatal = false;
        CAPTURE_FATAL(call(__imp__IoCompleteRequest, 0x40000000u, 1), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
        CHECK(g_fatal_msg.find("IoCompleteRequest") != std::string::npos);
    }

    // ---- NtReadFileScatter ------------------------------------------------
    uint32_t handle = 0;
    CHECK_ST(r.vfs.open(r.handles, "game:\\data.bin", false, &handle), Status::Ok);
    std::shared_ptr<GuestFile> file;
    CHECK_ST(r.handles.lookup_as<GuestFile>(handle, &file), Status::Ok);
    uint32_t pages[4] = {};
    for (uint32_t& p : pages) CHECK_ST(r.heap.alloc(kPage, kPage, true, &p), Status::Ok);
    // Segment i -> pages[3 - i]: the pages are not contiguous nor ascending.
    for (uint32_t i = 0; i < 4; ++i) CHECK(guest_write_be32(g_scratch + kSegments + 4 * i, pages[3 - i]));
    {
        const uint64_t zero = 0;
        CHECK_EQ(scatter(handle, 0, kFileSize, &zero), nt::kSuccess);
        CHECK_EQ(rd32(g_scratch + kIosb), nt::kSuccess);
        CHECK_EQ(rd32(g_scratch + kIosb + 4), kFileSize);
        bool same = true;
        for (uint32_t i = 0; i < kFileSize; ++i)
            same = same && g_base[pages[3 - i / kPage] + i % kPage] == pattern(i);
        CHECK(same);
        CHECK_EQ(file->position(), uint64_t(kFileSize));

        // Short read: stops at EOF with success and the bytes read.
        memset(g_base + pages[3], 0, kPage);
        const uint64_t last = 3 * kPage;
        CHECK_EQ(scatter(handle, 0, 2 * kPage, &last), nt::kSuccess);
        CHECK_EQ(rd32(g_scratch + kIosb + 4), 100u);
        CHECK_EQ(g_base[pages[3]], pattern(3 * kPage));
        CHECK_EQ(g_base[pages[3] + 99], pattern(3 * kPage + 99));

        // At EOF: STATUS_END_OF_FILE, Information 0.
        const uint64_t end = kFileSize;
        CHECK_EQ(scatter(handle, 0, kPage, &end), nt::kEndOfFile);
        CHECK_EQ(rd32(g_scratch + kIosb), nt::kEndOfFile);
        CHECK_EQ(rd32(g_scratch + kIosb + 4), 0u);
    }
    {
        // Current-position mode: two reads continue each other.
        CHECK_ST(file->set_position(0), Status::Ok);
        CHECK_EQ(scatter(handle, 0, kPage, nullptr), nt::kSuccess);
        CHECK_EQ(scatter(handle, 0, kPage, nullptr), nt::kSuccess);
        CHECK_EQ(g_base[pages[3]], pattern(kPage));
        CHECK_EQ(file->position(), uint64_t(2 * kPage));
    }
    {
        // Completion event: signalled after the read.
        PPCContext c{};
        c.r3.u64 = g_scratch + kEventCell;
        c.r5.u64 = 1;
        __imp__NtCreateEvent(c, g_base);
        CHECK_EQ(c.r3.u32, nt::kSuccess);
        const uint32_t event = rd32(g_scratch + kEventCell);
        const uint64_t zero = 0;
        CHECK_EQ(scatter(handle, event, kPage, &zero), nt::kSuccess);
        PPCContext w{};
        w.r3.u64 = event;
        w.r6.u64 = g_scratch + kTimeout;
        __imp__NtWaitForSingleObjectEx(w, g_base);
        CHECK_EQ(w.r3.u32, nt::kSuccess);
        // Wrong handle kinds.
        CHECK_EQ(scatter(event, 0, kPage, &zero), nt::kObjectTypeMismatch);
        CHECK_EQ(scatter(0xF0000F00u, 0, kPage, &zero), nt::kInvalidHandle);
        CHECK_EQ(scatter(handle, handle, kPage, &zero), nt::kObjectTypeMismatch);
    }
    {
        // An unwritable page is refused before anything is read.
        CHECK_ST(file->set_position(5), Status::Ok);
        CHECK(guest_write_be32(g_scratch + kSegments + 4, 0x00001000u));  // never committed
        CHECK_EQ(scatter(handle, 0, 2 * kPage, nullptr), nt::kAccessViolation);
        CHECK_EQ(file->position(), 5u);
        CHECK(guest_write_be32(g_scratch + kSegments + 4, pages[2]));
        bool fatal = false;
        CAPTURE_FATAL(call(__imp__NtReadFileScatter, handle, 0, 0x82000000u, 0, g_scratch + kIosb,
                           g_scratch + kSegments, kPage, 0),
                      fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_INDIRECT_TARGET);  // APC routine without a recompiled function
    }

    // ---- NtDeviceIoControlFile / IoDismountVolumeByFileHandle --------------
    memset(g_base + g_scratch + kIosb, 0xAA, 8);
    CHECK_EQ(call(__imp__NtDeviceIoControlFile, handle, 0, 0, 0, g_scratch + kIosb, 0x70000, 0, 0),
             kInvalidDeviceRequest);
    CHECK_EQ(rd32(g_scratch + kIosb), 0xAAAAAAAAu);  // synchronous failure: IOSB untouched
    CHECK_EQ(call(__imp__NtDeviceIoControlFile, 0xF0000F00u, 0, 0, 0, g_scratch + kIosb, 0x74004, 0, 0),
             nt::kInvalidHandle);
    CHECK_EQ(call(__imp__NtDeviceIoControlFile, handle, handle, 0, 0, g_scratch + kIosb, 0x70000, 0, 0),
             nt::kObjectTypeMismatch);
    CHECK_EQ(call(__imp__IoDismountVolumeByFileHandle, handle), kNotSupported);
    CHECK_EQ(call(__imp__IoDismountVolumeByFileHandle, 0xF0000F00u), nt::kInvalidHandle);
    CHECK_EQ(call(__imp__StfsCreateDevice, 0, 0, 0), kNotSupported);
    CHECK_EQ(call(__imp__StfsControlDevice, 0, 0, 0), kNotSupported);

    // ---- share access -----------------------------------------------------
    {
        const uint32_t sa = g_scratch + kShare;
        const uint32_t fo1 = g_scratch + kFileObjects, fo2 = fo1 + 0x40, fo3 = fo1 + 0x80;
        memset(g_base + sa, 0x77, 7);
        call(__imp__IoSetShareAccess, 0x1 /*READ_DATA*/, 1 /*SHARE_READ*/, fo1, sa);
        CHECK(share_is(1, 1, 0, 0, 1, 0, 0));
        // Write while the existing open does not share write.
        CHECK_EQ(call(__imp__IoCheckShareAccess, 0x2, 3, fo2, sa, 1), kSharingViolation);
        CHECK(share_is(1, 1, 0, 0, 1, 0, 0));
        CHECK_EQ(call(__imp__IoCheckShareAccess, 0x1, 1, fo2, sa, 1), nt::kSuccess);
        CHECK(share_is(2, 2, 0, 0, 2, 0, 0));
        // GENERIC_READ maps to read; no update leaves the counts.
        CHECK_EQ(call(__imp__IoCheckShareAccess, 0x80000000u, 1, fo3, sa, 0), nt::kSuccess);
        CHECK(share_is(2, 2, 0, 0, 2, 0, 0));
        // An open that refuses to share read conflicts with the readers.
        CHECK_EQ(call(__imp__IoCheckShareAccess, 0x1, 0, fo3, sa, 1), kSharingViolation);
        // Attribute-only access takes no part in sharing.
        CHECK_EQ(call(__imp__IoCheckShareAccess, 0x80 /*READ_ATTRIBUTES*/, 0, fo3, sa, 1), nt::kSuccess);
        CHECK(share_is(2, 2, 0, 0, 2, 0, 0));
        call(__imp__IoRemoveShareAccess, fo3, sa);  // recorded without access: nothing to remove
        CHECK(share_is(2, 2, 0, 0, 2, 0, 0));
        call(__imp__IoRemoveShareAccess, fo2, sa);
        CHECK(share_is(1, 1, 0, 0, 1, 0, 0));
        call(__imp__IoRemoveShareAccess, fo1, sa);
        CHECK(share_is(0, 0, 0, 0, 0, 0, 0));
        call(__imp__IoRemoveShareAccess, fo1, sa);  // consumed: no second removal
        CHECK(share_is(0, 0, 0, 0, 0, 0, 0));
        // Full access with full sharing, then a delete-sharing conflict.
        call(__imp__IoSetShareAccess, 0x10003 /*READ|WRITE|DELETE*/, 7, fo1, sa);
        CHECK(share_is(1, 1, 1, 1, 1, 1, 1));
        CHECK_EQ(call(__imp__IoCheckShareAccess, 0x1, 3, fo2, sa, 1), kSharingViolation);
        CHECK_EQ(call(__imp__IoCheckShareAccess, 0x10000, 7, fo2, sa, 1), nt::kSuccess);
        CHECK(share_is(2, 1, 1, 2, 2, 2, 2));
        // Underflow of a counter the guest corrupted is a diagnostic.
        memset(g_base + sa, 0, 7);
        bool fatal = false;
        CAPTURE_FATAL(call(__imp__IoRemoveShareAccess, fo2, sa), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
        CAPTURE_FATAL(call(__imp__IoSetShareAccess, 1, 1, fo1, 0x00001000u), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    }

    // ---- devices, object references, ObIsTitleObject ------------------------
    {
        uint32_t device = 0;
        CHECK_EQ(create_device("\\Device\\RcompTest", 0x44, &device), nt::kSuccess);
        CHECK(device != 0 && (device & 15) == 0);
        uint16_t type = 0;
        CHECK(guest_read_be16(device, &type));
        CHECK_EQ(type, 3u);
        CHECK_EQ(rd32(device + 0x08), 0x82B0EC40u);
        CHECK_EQ(rd32(device + 0x14), 0x10u);
        CHECK_EQ(rd32(device + 0x18), device + 0x60);
        CHECK_EQ(g_base[device + 0x1C], 61u);
        CHECK_EQ(g_base[device + 0x1E], 1u);
        bool zero = true;
        for (uint32_t i = 0; i < 0x44; ++i) zero = zero && g_base[device + 0x60 + i] == 0;
        CHECK(zero);
        uint32_t other = 0;
        CHECK_EQ(create_device("\\DEVICE\\rcomptest", 0, &other), nt::kObjectNameCollision);
        CHECK_EQ(create_device("\\Nowhere\\Dev", 0, &other), nt::kObjectPathNotFound);
        CHECK_EQ(create_link("\\Device\\RcompTest", "\\??\\game:"), nt::kObjectNameCollision);
        CHECK_EQ(delete_link("\\Device\\RcompTest"), nt::kObjectTypeMismatch);
        CHECK_EQ(call(__imp__ObIsTitleObject, device), 1u);
        CHECK_EQ(call(__imp__IoDismountVolume, device), kNotSupported);
        call(__imp__ObReferenceObject, device);
        call(__imp__ObDereferenceObject, device);
        bool fatal = false;
        CAPTURE_FATAL(call(__imp__ObDereferenceObject, device), fatal);  // the creation reference is not Ob's
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
        call(__imp__IoDeleteDevice, device);
        CAPTURE_FATAL(call(__imp__IoDeleteDevice, device), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
        CAPTURE_FATAL(call(__imp__IoDismountVolume, device), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
        // The name is free again once the device is deleted.
        CHECK_EQ(create_device("\\Device\\RcompTest", 0, &other), nt::kSuccess);
        // A reference keeps a deleted device alive; the last one frees it.
        call(__imp__ObReferenceObject, other);
        call(__imp__IoDeleteDevice, other);
        CHECK_EQ(call(__imp__ObIsTitleObject, other), 1u);
        uint32_t size = 0;
        CHECK_ST(r.heap.allocation_size(other, &size), Status::Ok);
        call(__imp__ObDereferenceObject, other);
        CHECK(r.heap.allocation_size(other, &size) != Status::Ok);
        CAPTURE_FATAL(call(__imp__ObIsTitleObject, other), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
        // Unnamed device; unwritable output pointer.
        CHECK_EQ(create_device("", 8, &other), nt::kSuccess);
        call(__imp__IoDeleteDevice, other);
        CHECK_EQ(call(__imp__IoCreateDevice, 0, 0, 0, 61, 0, 0x00001000u), nt::kAccessViolation);
        // Thread Bodies are title objects too.
        std::shared_ptr<ThreadObjectIdentity> thread;
        CHECK_ST(create_thread_object_identity(r.heap, 0xF00D, 0, &thread), Status::Ok);
        CHECK_EQ(call(__imp__ObIsTitleObject, thread_object_body(thread)), 1u);
        thread.reset();
        CAPTURE_FATAL(call(__imp__ObIsTitleObject, g_scratch), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    }

    // ---- symbolic links -----------------------------------------------------
    {
        CHECK_EQ(create_link("\\??\\cache:", "\\??\\game:\\Cache"), nt::kSuccess);
        CHECK_EQ(create_link("\\??\\CACHE:", "\\??\\game:"), nt::kObjectNameCollision);
        CHECK_EQ(create_link("\\DosDevices\\cache:", "\\??\\game:"), nt::kObjectNameCollision);
        CHECK_EQ(create_link("\\??\\GAME:", "\\Device\\Cdrom0"), nt::kObjectNameCollision);  // runtime mount
        CHECK_EQ(create_link("cache:", "\\??\\game:"), nt::kObjectNameInvalid);
        CHECK_EQ(create_link("\\??\\x:", ""), nt::kInvalidParameter);
        std::string target;
        CHECK_ST(find_title_drive_link("Cache:", &target), Status::Ok);
        CHECK(target == "\\??\\game:\\Cache");
        CHECK_ST(find_title_drive_link("\\??\\cache", &target), Status::Ok);
        std::string path;
        CHECK_ST(rewrite_path_through_title_links("cache:\\a\\b.bin", &path), Status::Ok);
        CHECK(path == "\\??\\game:\\Cache\\a\\b.bin");
        CHECK_ST(rewrite_path_through_title_links("\\??\\CACHE:\\x", &path), Status::Ok);
        CHECK(path == "\\??\\game:\\Cache\\x");
        CHECK_ST(rewrite_path_through_title_links("game:\\x", &path), Status::NotFound);
        CHECK_ST(rewrite_path_through_title_links("\\Device\\x", &path), Status::PathRejected);
        // The VFS follows title links: a file opens through a linked drive, and a chain of links resolves.
        CHECK_EQ(create_link("\\??\\alias:", "\\??\\game:"), nt::kSuccess);
        CHECK_EQ(create_link("\\??\\chain:", "alias:"), nt::kSuccess);
        for (const char* linked : {"alias:\\data.bin", "\\??\\ALIAS:\\data.bin", "chain:\\data.bin"}) {
            uint32_t linked_handle = 0;
            CHECK_ST(r.vfs.open(r.handles, linked, false, &linked_handle), Status::Ok);
            CHECK_ST(r.handles.close(linked_handle), Status::Ok);
        }
        uint32_t missing_handle = 0;
        CHECK_ST(r.vfs.open(r.handles, "nolink:\\data.bin", false, &missing_handle), Status::NoSuchDevice);
        CHECK_EQ(delete_link("\\??\\chain:"), nt::kSuccess);
        CHECK_EQ(delete_link("\\??\\alias:"), nt::kSuccess);
        CHECK_EQ(delete_link("\\??\\Cache:"), nt::kSuccess);
        CHECK_EQ(delete_link("\\??\\cache:"), nt::kObjectNameNotFound);
        CHECK_ST(find_title_drive_link("cache:", &target), Status::NotFound);
        bool fatal = false;
        CAPTURE_FATAL(delete_link("\\??\\game:"), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    }

    // ---- KeLockL2 / KeUnlockL2 ------------------------------------------------
    {
        memset(g_base + g_scratch + kOut, 0x5A, 4);
        CHECK_EQ(call(__imp__KeLockL2, 0, g_scratch, 0x1000, 1, 1), 0u);
        call(__imp__KeUnlockL2, 0);
        CHECK_EQ(rd32(g_scratch + kOut), 0x5A5A5A5Au);
    }

    // ---- XeKeys console signatures -------------------------------------------
    {
        const uint32_t hash = g_scratch + kHash, sig = g_scratch + kSignature, result = g_scratch + kResult;
        for (uint32_t i = 0; i < 20; ++i) g_base[hash + i] = uint8_t(i * 13 + 1);
        memset(g_base + sig, 0xEE, 0x228);
        CHECK_EQ(call(__imp__XeKeysConsolePrivateKeySign, hash, sig), 1u);
        uint16_t cert_size = 0;
        CHECK(guest_read_be16(sig, &cert_size));
        CHECK_EQ(cert_size, 0x1A8u);
        CHECK(memcmp(g_base + sig + 7, "RCOMP-LOCAL", 11) == 0);
        // Known answer (Python hmac/hashlib over the documented construction).
        static const uint8_t kExpectedHead[8] = {0x28, 0x37, 0x29, 0x02, 0x82, 0x60, 0x05, 0xC0};
        CHECK(memcmp(g_base + sig + 0x1A8, kExpectedHead, 8) == 0);
        uint8_t first[0x228];
        memcpy(first, g_base + sig, 0x228);
        CHECK_EQ(call(__imp__XeKeysConsolePrivateKeySign, hash, sig), 1u);
        CHECK(memcmp(first, g_base + sig, 0x228) == 0);  // deterministic

        CHECK(guest_write_be32(result, 0xFFFFFFFFu));
        CHECK_EQ(call(__imp__XeKeysConsoleSignatureVerification, hash, sig, result), 1u);
        CHECK_EQ(rd32(result), 1u);
        g_base[hash + 5] ^= 1;  // tampered data
        CHECK_EQ(call(__imp__XeKeysConsoleSignatureVerification, hash, sig, result), 1u);
        CHECK_EQ(rd32(result), 0u);
        g_base[hash + 5] ^= 1;
        g_base[sig + 0x1A8 + 0x7F] ^= 0x80;  // tampered signature
        CHECK_EQ(call(__imp__XeKeysConsoleSignatureVerification, hash, sig, result), 1u);
        CHECK_EQ(rd32(result), 0u);
        g_base[sig + 0x1A8 + 0x7F] ^= 0x80;
        g_base[sig + 0x30] ^= 1;  // tampered certificate (modulus) breaks the signature
        CHECK_EQ(call(__imp__XeKeysConsoleSignatureVerification, hash, sig, result), 1u);
        CHECK_EQ(rd32(result), 0u);
        g_base[sig + 0x30] ^= 1;
        g_base[sig + 8] ^= 1;  // not an R-comp certificate: FALSE, invalid
        CHECK_EQ(call(__imp__XeKeysConsoleSignatureVerification, hash, sig, result), 0u);
        CHECK_EQ(rd32(result), 0u);
        g_base[sig + 8] ^= 1;
        CHECK_EQ(call(__imp__XeKeysConsoleSignatureVerification, hash, sig, result), 1u);
        CHECK_EQ(rd32(result), 1u);
        bool fatal = false;
        CAPTURE_FATAL(call(__imp__XeKeysConsolePrivateKeySign, hash, 0x00001000u), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_GUEST_ACCESS);
    }

    CHECK_ST(r.handles.close(handle), Status::Ok);
    file.reset();
    runtime_shutdown();
    mem.release();
    unlink((game + "/data.bin").c_str());
    rmdir(game.c_str());
    rmdir(top.c_str());
    return test_result("rt_kernel_devices");
}
