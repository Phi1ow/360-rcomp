// Real generated PPC through production TitleRuntime; only GPU is doubled.
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "ppc_recomp_shared.h"
#include "rcomp/app/title_runtime.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime_state.h"
namespace {
namespace rt = rcomp::rt;
using rcomp::app::TitleConfig;
using rcomp::app::TitleRuntime;
int failures = 0, checks = 0;
void check(const char* id, bool ok, const std::string& reason = {}) {
    ++checks; failures += !ok;
    std::printf("{\"id\":\"xex/boot/%s\",\"status\":\"%s\",\"scope\":\"host\",\"reason\":\"", id, ok ? "PASS" : "FAIL");
    for (char c : reason) {
        if (c == '\n') std::fputs("\\n", stdout);
        else if (c == '\r') std::fputs("\\r", stdout);
        else { if (c == '"' || c == '\\') std::putchar('\\'); std::putchar(c); }
    }
    std::puts("\"}"); std::fflush(stdout);
}
void watchdog(int) {
    const char message[] = "FAIL xex/boot/watchdog: exceeded 45 seconds\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1); _exit(124);
}
struct SyntheticFiles {
    std::string root, data, payload, outside, escape;
    bool setup() {
        std::string pattern = std::string(RCOMP_TEST_DATA_ROOT) + "/synthetic boot data.XXXXXX";
        std::vector<char> chars(pattern.begin(), pattern.end()); chars.push_back(0);
        char* name = mkdtemp(chars.data());
        if (!name) return false;
        root = name; data = root + "/title data"; payload = data + "/boot-data.bin";
        outside = root + "/outside.bin"; escape = data + "/escape.bin";
        if (mkdir(data.c_str(), 0700)) return false;
        const unsigned char bytes[] = {1, 2, 3, 4};
        FILE* f = std::fopen(payload.c_str(), "wb");
        if (!f) return false;
        bool ok = std::fwrite(bytes, 1, sizeof(bytes), f) == sizeof(bytes);
        ok = std::fclose(f) == 0 && ok;
        f = std::fopen(outside.c_str(), "wb");
        if (!f) return false;
        ok = std::fwrite("outside", 1, 7, f) == 7 && ok;
        ok = std::fclose(f) == 0 && ok;
        return symlink(outside.c_str(), escape.c_str()) == 0 && ok;
    }
    ~SyntheticFiles() {
        // Only the exact files created above; no recursive cleanup.
        if (!escape.empty()) unlink(escape.c_str());
        if (!payload.empty()) unlink(payload.c_str());
        if (!outside.empty()) unlink(outside.c_str());
        if (!data.empty()) rmdir(data.c_str());
        if (!root.empty()) rmdir(root.c_str());
    }
};
bool read_file(const char* path, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(path, "rb"); if (!f) return false;
    uint8_t bytes[4096];
    for (size_t n; (n = std::fread(bytes, 1, sizeof bytes, f)) != 0;) out->insert(out->end(), bytes, bytes + n);
    bool ok = !std::ferror(f); return std::fclose(f) == 0 && ok;
}
bool clean(uint32_t entry) {
    return !rt::runtime() && !rcomp::active_guest_memory() && !rcomp::lookup_function(entry) &&
           !rt::find_import(rt::kModuleXboxkrnl, 0x00CC);
}
void reject(const char* id, TitleConfig bad, const char* diagnostic, uint32_t entry) {
    std::string error;
    auto title = TitleRuntime::Create(bad, &error);
    bool rejected = !title && error.find(diagnostic) != std::string::npos;
    title.reset(); check(id, rejected && clean(entry), error);
}
void mounted_files() {
    auto& r = *rt::runtime();
    for (const char* path : {"game:/boot-data.bin", "d:/boot-data.bin"}) {
        uint32_t handle = 0, count = 0;
        auto s = r.vfs.open(r.handles, path, false, &handle);
        std::shared_ptr<rt::GuestFile> file; uint8_t bytes[4]{};
        bool ok = s == rt::Status::Ok && r.handles.lookup_as<rt::GuestFile>(handle, &file) == rt::Status::Ok;
        if (ok) ok = file->read(bytes, 4, &count) == rt::Status::Ok && count == 4 &&
                     bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4;
        if (handle) ok = r.handles.close(handle) == rt::Status::Ok && ok;
        check(path[0] == 'g' ? "game_mount_read" : "d_mount_read", ok);
    }
    struct Case { const char* id; const char* path; bool write; rt::Status expected; };
    const Case cases[] = {
        {"file_missing", "game:/missing.bin", false, rt::Status::NotFound},
        {"mount_missing", "absent:/boot-data.bin", false, rt::Status::NoSuchDevice},
        {"write_access_denied", "game:/boot-data.bin", true, rt::Status::AccessDenied},
        {"parent_escape_denied", "game:/../outside.bin", false, rt::Status::PathRejected},
        {"symlink_escape_denied", "game:/escape.bin", false, rt::Status::PathRejected},
    };
    for (const auto& c : cases) {
        uint32_t handle = 0;
        auto s = r.vfs.open(r.handles, c.path, c.write, &handle);
        check(c.id, s == c.expected && handle == 0, rt::status_name(s));
        if (handle) r.handles.close(handle);
    }
}
uint32_t be32(const std::vector<uint8_t>& b, size_t p) {
    return (uint32_t(b[p]) << 24) | (uint32_t(b[p+1]) << 16) | (uint32_t(b[p+2]) << 8) | b[p+3];
}
bool unresolve_video(std::vector<uint8_t>* bytes) {
    auto& b = *bytes;
    if (b.size() < 24) return false;
    const uint32_t raw = be32(b, 8), nopt = be32(b, 20);
    if (nopt > (b.size() - 24) / 8) return false;
    for (uint32_t i = 0; i < nopt; ++i) {
        const size_t opt = 24 + 8*i;
        if (be32(b, opt) != 0x000103FF) continue;
        const size_t hdr = be32(b, opt + 4);
        if (hdr + 12 > b.size()) return false;
        const uint32_t libraries = be32(b, hdr + 8);
        size_t lib = hdr + 12 + be32(b, hdr + 4);
        for (uint32_t j = 0; j < libraries; ++j) {
            if (lib + 40 > b.size()) return false;
            const uint32_t size = be32(b, lib);
            const uint32_t count = (uint32_t(b[lib+38]) << 8) | b[lib+39];
            if (size < 40 || size > b.size() - lib || count > (size - 40) / 4) return false;
            for (uint32_t k = 0; k < count; ++k) {
                const uint32_t address = be32(b, lib + 40 + 4*k);
                if (address < uint32_t(PPC_IMAGE_BASE)) continue;
                const uint64_t pos = uint64_t(raw) + address - uint32_t(PPC_IMAGE_BASE);
                if (pos + 4 > b.size()) return false;
                if (be32(b, size_t(pos)) == 0x000001C0) {
                    b[size_t(pos)+2] = 0x01; b[size_t(pos)+3] = 0x56; return true;
                }
            }
            lib += size;
        }
    }
    return false;
}
}
int main(int argc, char** argv) {
    signal(SIGALRM, watchdog); alarm(45);
    SyntheticFiles files;
    if (!files.setup()) { std::perror("synthetic fixture setup"); return 2; }
    TitleConfig cfg;
    if (!read_file(argc > 1 ? argv[1] : RCOMP_XEX_PATH, &cfg.xex)) return 2;
    cfg.game_root = files.data; cfg.log = stderr; cfg.screen = false; cfg.main_stack_size = 0x10000;
    for (auto* m = PPCFuncMappings; m->host; ++m) cfg.functions.push_back({uint32_t(m->guest), m->host, nullptr});
    uint32_t entry = 0;
    for (uint32_t i = 0; i < be32(cfg.xex, 20); ++i)
        if (be32(cfg.xex, 24 + 8*i) == 0x00010100) entry = be32(cfg.xex, 28 + 8*i);
    if (!entry || cfg.functions.empty()) return 2;
    TitleConfig bad = cfg; bad.xex.clear(); reject("empty_image_rejected", bad, "XEX", entry);
    bad = cfg; bad.xex[0] = 0; reject("malformed_image_rejected", bad, "XEX", entry);
    bad = cfg; bad.functions.clear(); reject("missing_mapping_rejected", bad, "entry point", entry);
    bad = cfg; bad.functions.push_back(bad.functions.front()); reject("duplicate_mapping_rejected", bad, "duplicate", entry);
    bad = cfg; bad.game_root = files.root + "/missing"; reject("missing_data_root_rejected", bad, "mount", entry);
    bad = cfg;
    bool mutated = unresolve_video(&bad.xex);
    FILE* diagnostics = std::tmpfile();
    check("negative_video_fixture", mutated && diagnostics != nullptr);
    if (mutated && diagnostics) {
        bad.log = diagnostics;
        reject("unresolved_variable_rejected", bad, "unresolved variable", entry);
        std::fflush(diagnostics); std::rewind(diagnostics);
        std::string log; char bytes[512];
        for (size_t n; (n = std::fread(bytes, 1, sizeof bytes, diagnostics)) != 0;) log.append(bytes, n);
        check("unresolved_variable_diagnostic", log.find("xboxkrnl") != std::string::npos &&
              log.find("0x0156") != std::string::npos, log);
    }
    if (diagnostics) std::fclose(diagnostics);
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        std::string error;
        auto title = TitleRuntime::Create(cfg, &error);
        check("title_create", bool(title), error);
        if (!title) continue;
        auto* owner = rt::runtime(); auto* memory = rcomp::active_guest_memory();
        auto* function = rcomp::lookup_function(entry);
        auto second = TitleRuntime::Create(cfg, &error);
        check("failed_second_create_retains_owner", !second && rt::runtime() == owner &&
              rcomp::active_guest_memory() == memory && rcomp::lookup_function(entry) == function &&
              rt::find_import(rt::kModuleXboxkrnl, 0x00CC) != nullptr, error);
        if (second) { second.reset(); return 1; }
        mounted_files();
        const auto before = owner->heap.stats().live_allocations;
        for (unsigned run = 0; run < 2; ++run) {
            uint32_t code = 0;
            bool ok = title->RunEntry(0, &code, &error);
            char detail[128];
            std::snprintf(detail, sizeof detail, "cycle=%u run=%u exit=0x%08X expected=0x0000006D", cycle, run, code);
            check("ppc_file_threads_static_tls_termination", ok && code == 0x6D, detail);
            check("ppc_releases_allocations_handles", owner->heap.stats().live_allocations == before && owner->handles.live_count() == 0);
            if (!ok || code != 0x6D) break;
        }
        title.reset(); check("destroy_clears_runtime_memory_imports_functions", clean(entry));
    }
    alarm(0);
    std::printf("RCOMP-BOOT checks=%d pass=%d fail=%d scope=host graphics=NOT_TESTED PS5=NOT_TESTED\n", checks, checks-failures, failures);
    return failures ? 1 : 0;
}
