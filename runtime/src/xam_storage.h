// File and byte-order helpers shared by the XAM storage services (content packages, profile settings).
// Internal to runtime/src.
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace rcomp::rt::storage {

inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
inline void put_be64(uint8_t* p, uint64_t v) {
    put_be32(p, uint32_t(v >> 32));
    put_be32(p + 4, uint32_t(v));
}
inline uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

inline std::string hex(uint64_t v, int digits) {
    char s[17];
    snprintf(s, sizeof s, "%0*llX", digits, (unsigned long long)v);
    return s;
}
inline bool mkdir_p(const std::string& path) {
    std::string partial;
    size_t i = 0;
    while (i <= path.size()) {
        const size_t j = path.find('/', i);
        partial = path.substr(0, j == std::string::npos ? path.size() : j);
        if (!partial.empty() && mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) return false;
        if (j == std::string::npos) break;
        i = j + 1;
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool write_file_atomic(const std::string& path, const void* data, size_t size) {
    const std::string tmp = path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        const ssize_t n = write(fd, p + done, size - done);
        if (n <= 0) { close(fd); unlink(tmp.c_str()); return false; }
        done += size_t(n);
    }
    const bool synced = fsync(fd) == 0;
    if (close(fd) != 0 || !synced || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

inline bool read_file(const std::string& path, std::vector<uint8_t>* out, size_t max) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    out->clear();
    uint8_t buffer[4096];
    for (;;) {
        const ssize_t n = read(fd, buffer, sizeof buffer);
        if (n < 0) { close(fd); return false; }
        if (n == 0) break;
        out->insert(out->end(), buffer, buffer + n);
        if (out->size() > max) { close(fd); return false; }
    }
    close(fd);
    return true;
}

}  // namespace rcomp::rt::storage
