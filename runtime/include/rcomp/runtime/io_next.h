// Future NtQueryDirectoryFile support for packaged PS5 mounts.
//
// The writable IO-next prototype was promoted into Vfs/GuestFile. This file
// intentionally retains only the manifest-backed directory index, because the
// current PS5 title environment cannot rely on opendir/readdir.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

struct IoNextDirectoryEntry {
    std::string guest_path;
    std::string name;
    uint32_t file_index = 0;
};

struct IoNextDirectoryCursor {
    std::string directory_key;
    std::string pattern;
    size_t next = 0;
    bool initialized = false;
};

enum class IoNextDirectoryResult {
    Found,
    NoSuchFile,
    NoMoreFiles,
};

class IoNextDirectoryIndex {
public:
    // Adds one canonical packaged path, synthesizing parent directory entries.
    // A case-fold collision with different spelling is rejected.
    Status add_path(std::string_view guest_path);
    Status finalize();

    IoNextDirectoryResult query(std::string_view directory,
                                std::string_view pattern,
                                bool restart_scan,
                                IoNextDirectoryCursor* cursor,
                                IoNextDirectoryEntry* out) const;
    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::string directory;
        std::string directory_key;
        std::string name;
        std::string name_key;
        std::string guest_path;
        uint32_t file_index = 0;
    };
    std::vector<Entry> entries_;
    bool finalized_ = false;
};

}  // namespace rcomp::rt
