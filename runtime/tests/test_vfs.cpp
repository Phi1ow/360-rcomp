#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "rcomp/runtime/handle_table.h"
#include "rcomp/runtime/vfs.h"
#include "test_util.h"

using namespace rcomp::rt;

namespace {
bool write_file(const std::string& p, const std::string& data) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}
struct EventObj : HandleObject {
    static constexpr HandleKind kKind = HandleKind::Event;
    HandleKind kind() const override { return kKind; }
};
}  // namespace

int main() {
    const char* tmp = getenv("TMPDIR");
    std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/rcomp_rt_vfs_XXXXXX";
    std::string top = mkdtemp(&tmpl[0]) ? tmpl : "";
    if (top.empty()) return 2;
    char canon_top[PATH_MAX];
    if (!realpath(top.c_str(), canon_top)) return 2;
    top = canon_top;
    std::string root = top + "/root";
    mkdir(root.c_str(), 0755);
    mkdir((root + "/data").c_str(), 0755);
    const std::string payload = "R-comp runtime file test\n0123456789";
    CHECK(write_file(root + "/data/x.bin", payload));
    CHECK(write_file(top + "/secret.txt", "outside"));
    CHECK(symlink((top + "/secret.txt").c_str(), (root + "/data/escape").c_str()) == 0);
    CHECK(symlink(top.c_str(), (root + "/updir").c_str()) == 0);

    Vfs vfs;
    HandleTable handles;
    CHECK_ST(vfs.mount("game:", root), Status::Ok);
    CHECK_ST(vfs.mount("GAME", root), Status::AlreadyExists);
    CHECK_ST(vfs.mount("d", root + "/nonexistent"), Status::NotFound);
    CHECK_ST(vfs.mount("a/b", root), Status::InvalidArgument);

    // --- mapping --------------------------------------------------------------
    std::string hp;
    const std::string expect = root + "/data/x.bin";
    CHECK_ST(vfs.resolve("game:\\data\\x.bin", &hp), Status::Ok);
    CHECK(hp == expect);
    CHECK_ST(vfs.resolve("GAME:/data/x.bin", &hp), Status::Ok);
    CHECK(hp == expect);
    CHECK_ST(vfs.resolve("\\??\\game:\\data\\\\x.bin", &hp), Status::Ok);
    CHECK(hp == expect);

    // --- rejected -------------------------------------------------------------
    const char* rejected[] = {
        "game:\\..\\secret.txt",
        "game:\\data\\..\\..\\secret.txt",
        "game:/data/../x.bin",   // even if it would stay inside
        "game:\\.\\data\\x.bin",
        "game:\\data\\x.bin\\..",
        "/etc/passwd",
        "\\etc\\passwd",
        "data\\x.bin",
        "\\Device\\Harddisk0\\Partition1\\x.bin",
        "game:\\data\\a:b",
        "game:\\data\\*.bin",
        "game:",
        "game:\\",
        "",
        "/tmp/x:\\y",
    };
    for (const char* p : rejected) {
        hp = "unchanged";
        Status s = vfs.resolve(p, &hp);
        if (s != Status::PathRejected) {
            fprintf(stderr, "path '%s' -> %s\n", p, status_name(s));
            ++g_failures;
        }
        CHECK(hp == "unchanged");
    }
    std::string nul_path("game:\\data\\x.bin\0/../../etc", 27);
    CHECK_ST(vfs.resolve(nul_path, &hp), Status::PathRejected);
    CHECK_ST(vfs.resolve("game:\\" + std::string(300, 'a'), &hp), Status::PathRejected);
    CHECK_ST(vfs.resolve("d:\\x.bin", &hp), Status::NoSuchDevice);
    CHECK_ST(vfs.resolve("C:\\Windows\\win.ini", &hp), Status::NoSuchDevice);

    // Host symlinks may not escape the sandbox.
    uint32_t h = 0;
    CHECK_ST(vfs.open(handles, "game:\\data\\escape", false, &h), Status::PathRejected);
    CHECK_ST(vfs.open(handles, "game:\\updir\\secret.txt", false, &h), Status::PathRejected);

    // --- open / read / seek / close -------------------------------------------
    CHECK_ST(vfs.open(handles, "game:\\data\\missing.bin", false, &h), Status::NotFound);
    CHECK_ST(vfs.open(handles, "game:\\data", false, &h), Status::IsDirectory);
    CHECK_ST(vfs.open(handles, "game:\\data\\x.bin", true, &h), Status::AccessDenied);
    CHECK_ST(vfs.open(handles, "game:\\..\\secret.txt", false, &h), Status::PathRejected);
    CHECK_EQ(handles.live_count(), 0u);

    CHECK_ST(vfs.open(handles, "game:\\data\\x.bin", false, &h), Status::Ok);
    std::shared_ptr<GuestFile> f;
    CHECK_ST(handles.lookup_as<GuestFile>(h, &f), Status::Ok);
    std::shared_ptr<EventObj> wrong;
    CHECK_ST(handles.lookup_as<EventObj>(h, &wrong), Status::WrongHandleKind);
    CHECK_EQ(f->size(), (uint64_t)payload.size());

    char buf[128] = {};
    uint32_t got = 0;
    CHECK_ST(f->read(buf, 6, &got), Status::Ok);
    CHECK_EQ(got, 6u);
    CHECK(memcmp(buf, "R-comp", 6) == 0);
    CHECK_EQ(f->position(), 6ull);
    CHECK_ST(f->read(buf, 128, &got), Status::Ok);  // short read at EOF is Ok
    CHECK_EQ(got, (uint32_t)payload.size() - 6);
    CHECK(memcmp(buf, payload.data() + 6, got) == 0);
    CHECK_ST(f->read(buf, 1, &got), Status::EndOfFile);
    CHECK_EQ(got, 0u);
    CHECK_ST(f->read(buf, 0, &got), Status::Ok);

    uint64_t pos = 0;
    CHECK_ST(f->seek(-10, SeekOrigin::End, &pos), Status::Ok);
    CHECK_EQ(pos, (uint64_t)payload.size() - 10);
    CHECK_ST(f->read(buf, 10, &got), Status::Ok);
    CHECK(memcmp(buf, "0123456789", 10) == 0);
    CHECK_ST(f->seek(-1, SeekOrigin::Begin, &pos), Status::InvalidArgument);
    CHECK_ST(f->seek(2, SeekOrigin::Begin, &pos), Status::Ok);
    CHECK_ST(f->seek(3, SeekOrigin::Current, &pos), Status::Ok);
    CHECK_EQ(pos, 5ull);
    CHECK_ST(f->read_at(7, buf, 7, &got), Status::Ok);
    CHECK(memcmp(buf, "runtime", 7) == 0);
    CHECK_ST(f->read_at(1000, buf, 4, &got), Status::EndOfFile);

    CHECK_ST(handles.close(h), Status::Ok);
    CHECK_ST(handles.lookup_as<GuestFile>(h, &f), Status::InvalidHandle);
    CHECK_ST(handles.close(h), Status::InvalidHandle);

    // Cleanup.
    unlink((root + "/data/escape").c_str());
    unlink((root + "/updir").c_str());
    unlink((root + "/data/x.bin").c_str());
    unlink((top + "/secret.txt").c_str());
    rmdir((root + "/data").c_str());
    rmdir(root.c_str());
    rmdir(top.c_str());
    return test_result("rt_test_vfs");
}
