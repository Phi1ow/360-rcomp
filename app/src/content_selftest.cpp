// Console diagnostic (RCOMP_M6_CONTENT_SELFTEST builds only): exercises the title's save services on the real
// storage before the guest starts, through the same registered XAM imports the game calls. It creates a
// package "RCOMPSELFTEST" in the title's save root, writes a file through the VFS, closes, reopens it by a
// name of another case, reads the file back, enumerates the packages, sets a thumbnail and reads the creator,
// then empties the package. One RCOMP-CONTENT-SELFTEST line per step, then PASS or FAIL. No game data.
#include "rcomp/app/content_selftest.h"

#include <stdio.h>
#include <string.h>

#include <memory>
#include <string>

#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/vfs.h"
#include "rcomp/runtime/xam_content.h"

namespace rcomp::app {
namespace {
namespace rt = rcomp::rt;

struct Harness {
    rt::Runtime& r;
    uint8_t* base;
    uint32_t buf = 0, stack = 0;
    FILE* log;
    int failures = 0;

    uint32_t call(uint32_t ordinal, const uint64_t (&regs)[8], uint32_t stack9 = 0) {
        PPCFunc* fn = rt::find_import(rt::kModuleXam, ordinal);
        if (!fn) {
            fprintf(log, "RCOMP-CONTENT-SELFTEST import 0x%X not registered\n", ordinal);
            ++failures;
            return 0xFFFFFFFFu;
        }
        alignas(64) PPCContext ctx{};
        ctx.r3.u64 = regs[0]; ctx.r4.u64 = regs[1]; ctx.r5.u64 = regs[2]; ctx.r6.u64 = regs[3];
        ctx.r7.u64 = regs[4]; ctx.r8.u64 = regs[5]; ctx.r9.u64 = regs[6]; ctx.r10.u64 = regs[7];
        ctx.r1.u64 = stack;
        rt::guest_write_be32(stack + 0x54, stack9);
        fn(ctx, base);
        return ctx.r3.u32;
    }
    void check(const char* step, bool ok, uint32_t value) {
        fprintf(log, "RCOMP-CONTENT-SELFTEST %-28s %s (0x%X)\n", step, ok ? "ok" : "FAILED", value);
        if (!ok) ++failures;
    }
    uint8_t* at(uint32_t a, uint32_t n) { return r.mem->translate(a, n); }
    uint32_t rd32(uint32_t a) {
        uint32_t v = 0;
        rt::guest_read_be32(a, &v);
        return v;
    }
    void content(uint32_t a, const char* file) {
        memset(at(a, rt::kXContentDataBytes), 0, rt::kXContentDataBytes);
        rt::guest_write_be32(a, rt::kContentDeviceHdd);
        rt::guest_write_be32(a + 4, rt::kContentTypeSavedGame);
        const char* display = "R-comp self-test";
        for (uint32_t i = 0; display[i]; ++i) at(a + 8 + 2 * i + 1, 1)[0] = uint8_t(display[i]);
        memcpy(at(a + 0x108, 42), file, strlen(file));
    }
    bool write_file(const char* path, const char* text) {
        rt::OpenRequest request;
        request.write_access = true;
        request.disposition = rt::OpenDisposition::OverwriteIf;
        uint32_t handle = 0;
        if (r.vfs.open(r.handles, path, request, &handle) != rt::Status::Ok) return false;
        std::shared_ptr<rt::GuestFile> file;
        uint32_t written = 0;
        const bool ok = r.handles.lookup_as<rt::GuestFile>(handle, &file) == rt::Status::Ok &&
                        file->write(text, uint32_t(strlen(text)), &written) == rt::Status::Ok && file->flush() == rt::Status::Ok;
        file.reset();
        r.handles.close(handle);
        return ok && written == strlen(text);
    }
    std::string read_file(const char* path) {
        uint32_t handle = 0;
        if (r.vfs.open(r.handles, path, false, &handle) != rt::Status::Ok) return "<missing>";
        std::shared_ptr<rt::GuestFile> file;
        char text[64] = {};
        uint32_t got = 0;
        if (r.handles.lookup_as<rt::GuestFile>(handle, &file) == rt::Status::Ok) file->read(text, sizeof text - 1, &got);
        file.reset();
        r.handles.close(handle);
        return std::string(text, got);
    }
};

constexpr uint32_t kCreateEx = 0x259, kClose = 0x25A, kDeviceData = 0x25E, kSetThumbnail = 0x260,
                   kGetCreator = 0x262, kCreateEnumerator = 0x25C, kEnumerate = 0x250, kDelete = 0x25B,
                   kDeviceState = 0x265, kFlush = 0x267;
}  // namespace

bool run_content_selftest(FILE* log) {
    rt::Runtime* r = rt::runtime();
    if (!r || !log) return false;
    Harness h{*r, r->mem->base(), 0, 0, log};
    if (r->heap.alloc(0x2000, 16, true, &h.buf) != rt::Status::Ok || r->heap.alloc(0x100, 16, true, &h.stack) != rt::Status::Ok) {
        fprintf(log, "RCOMP-CONTENT-SELFTEST FAIL guest allocation\n");
        return false;
    }
    fprintf(log, "RCOMP-CONTENT-SELFTEST begin save_root=%s\n", rt::save_root().c_str());
    const uint32_t root = h.buf, data = h.buf + 0x40, out = h.buf + 0x200, items = h.buf + 0x400;
    memcpy(h.at(root, 9), "rcselft\0", 8);
    uint32_t v = h.call(kDeviceData, {rt::kContentDeviceHdd, out, 0, 0, 0, 0, 0, 0});
    h.check("XamContentGetDeviceData", v == 0 && h.rd32(out) == 1 && (h.rd32(out + 0x10) || h.rd32(out + 0x14)), v);
    h.content(data, "RCOMPSELFTEST");
    v = h.call(kCreateEx, {0, root, data, 4 /* OPEN_ALWAYS */, out, 0, 0, 0});
    h.check("XamContentCreateEx open/create", v == 0, v);
    h.check("write rcselft:\\check.bin", h.write_file("rcselft:\\check.bin", "saved-by-r-comp"), 0);
    v = h.call(kFlush, {root, 0, 0, 0, 0, 0, 0, 0});
    h.check("XamContentFlush", v == 0, v);
    v = h.call(kDelete, {0, data, 0, 0, 0, 0, 0, 0});
    h.check("XamContentDelete refused while open", v == 0x20, v);
    v = h.call(kClose, {root, 0, 0, 0, 0, 0, 0, 0});
    h.check("XamContentClose", v == 0, v);
    h.check("closed package unmounted", h.read_file("rcselft:\\check.bin") == "<missing>", 0);
    h.content(data, "rcompselftest");
    v = h.call(kCreateEx, {0, root, data, 3 /* OPEN_EXISTING */, out, 0, 0, 0});
    h.check("XamContentCreateEx reopen", v == 0 && h.rd32(out) == 2, v);
    h.check("read back", h.read_file("rcselft:\\check.bin") == "saved-by-r-comp", 0);
    v = h.call(kClose, {root, 0, 0, 0, 0, 0, 0, 0});
    h.check("XamContentClose again", v == 0, v);
    v = h.call(kCreateEnumerator, {0, rt::kContentDeviceHdd, rt::kContentTypeSavedGame, 0, 16, out, out + 4, 0});
    uint32_t found = 0;
    if (v == 0) {
        const uint32_t handle = h.rd32(out + 4);
        for (;;) {
            const uint32_t e = h.call(kEnumerate, {handle, 0, items, 16 * rt::kXContentDataBytes, out + 8, 0, 0, 0});
            if (e != 0) break;
            for (uint32_t i = 0; i < h.rd32(out + 8); ++i)
                if (!strncmp(reinterpret_cast<const char*>(h.at(items + i * rt::kXContentDataBytes + 0x108, 42)), "RCOMPSELFTEST", 42)) ++found;
        }
        r->handles.close(handle);
    }
    h.check("enumeration lists the package", v == 0 && found == 1, found);
    memcpy(h.at(h.buf + 0x1000, 8), "\x89PNG\r\n\x1a\n", 8);
    v = h.call(kSetThumbnail, {0, data, h.buf + 0x1000, 8, 0, 0, 0, 0});
    h.check("XamContentSetThumbnail", v == 0, v);
    v = h.call(kGetCreator, {0, data, out, out + 8, 0, 0, 0, 0});
    h.check("XamContentGetCreator", v == 0 && h.rd32(out) == 1, v);
    v = h.call(kCreateEx, {0, root, data, 5 /* TRUNCATE_EXISTING */, out, 0, 0, 0});
    h.check("truncate the self-test package", v == 0 && h.read_file("rcselft:\\check.bin") == "<missing>", v);
    h.call(kClose, {root, 0, 0, 0, 0, 0, 0, 0});
    v = h.call(kDeviceState, {rt::kContentDeviceHdd, 0, 0, 0, 0, 0, 0, 0});
    h.check("XamContentGetDeviceState", v == 0, v);
    v = h.call(kDelete, {0, data, 0, 0, 0, 0, 0, 0});
    h.check("XamContentDelete (cleans up)", v == 0, v);
    v = h.call(kDelete, {0, data, 0, 0, 0, 0, 0, 0});
    h.check("deleted package is gone", v == 2, v);
    r->heap.free(h.stack);
    r->heap.free(h.buf);
    fprintf(log, "RCOMP-CONTENT-SELFTEST %s (%d failed steps)\n", h.failures ? "FAIL" : "PASS", h.failures);
    fflush(log);
    return h.failures == 0;
}

}  // namespace rcomp::app
