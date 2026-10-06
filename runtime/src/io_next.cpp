#include "rcomp/runtime/io_next.h"

#include <algorithm>

namespace rcomp::rt {
namespace {

constexpr size_t kMaxGuestPath = 255;

std::string lower_ascii(std::string_view text) {
    std::string result(text);
    for (char& c : result)
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    return result;
}

bool valid_device_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

bool valid_component(std::string_view component) {
    if (component.empty() || component == "." || component == "..") return false;
    for (unsigned char c : component) {
        if (c < 0x20 || c == 0x7F) return false;
        switch (c) {
        case ':': case '*': case '?': case '"': case '<': case '>': case '|':
            return false;
        default:
            break;
        }
    }
    return true;
}

struct ParsedPath {
    std::string device;
    std::vector<std::string> components;
};

Status parse_path(std::string_view input, bool allow_root, ParsedPath* out) {
    if (!out || input.empty() || input.size() > kMaxGuestPath)
        return Status::PathRejected;
    if (input.size() >= 4 && input.substr(0, 4) == "\\??\\") input.remove_prefix(4);
    const size_t colon = input.find(':');
    if (colon == std::string_view::npos || colon == 0) return Status::PathRejected;
    for (char c : input.substr(0, colon))
        if (!valid_device_char(c)) return Status::PathRejected;

    ParsedPath parsed;
    parsed.device = lower_ascii(input.substr(0, colon));
    std::string_view rest = input.substr(colon + 1);
    size_t i = 0;
    while (i <= rest.size()) {
        size_t j = rest.find_first_of("\\/", i);
        if (j == std::string_view::npos) j = rest.size();
        std::string_view component = rest.substr(i, j - i);
        if (!component.empty()) {
            if (!valid_component(component)) return Status::PathRejected;
            parsed.components.emplace_back(component);
        }
        i = j + 1;
    }
    if (!allow_root && parsed.components.empty()) return Status::PathRejected;
    *out = std::move(parsed);
    return Status::Ok;
}

bool wildcard_match_ci(std::string_view pattern, std::string_view text) {
    size_t p = 0, t = 0, star = std::string_view::npos, retry = 0;
    auto equal = [](char a, char b) {
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        return a == b;
    };
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || equal(pattern[p], text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            retry = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++retry;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

Status directory_key(std::string_view path, std::string* key) {
    if (!key) return Status::InvalidArgument;
    ParsedPath parsed;
    Status status = parse_path(path, true, &parsed);
    if (status != Status::Ok) return status;
    std::string result = parsed.device + ":";
    for (const auto& component : parsed.components)
        result += "\\" + component;
    *key = lower_ascii(result);
    return Status::Ok;
}

}  // namespace

Status IoNextDirectoryIndex::add_path(std::string_view guest_path) {
    if (finalized_) return Status::Conflict;
    ParsedPath parsed;
    Status status = parse_path(guest_path, false, &parsed);
    if (status != Status::Ok) return status;

    std::string directory = parsed.device + ":";
    for (const std::string& name : parsed.components) {
        const std::string directory_folded = lower_ascii(directory);
        const std::string name_folded = lower_ascii(name);
        bool exact = false;
        for (const auto& entry : entries_) {
            if (entry.directory_key != directory_folded ||
                entry.name_key != name_folded)
                continue;
            if (entry.name != name) return Status::AlreadyExists;
            exact = true;
            break;
        }
        const std::string full = directory + "\\" + name;
        if (!exact)
            entries_.push_back(
                {directory, directory_folded, name, name_folded, full, 0});
        directory = full;
    }
    return Status::Ok;
}

Status IoNextDirectoryIndex::finalize() {
    if (finalized_) return Status::Ok;
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.directory_key != b.directory_key)
            return a.directory_key < b.directory_key;
        if (a.name_key != b.name_key) return a.name_key < b.name_key;
        return a.name < b.name;
    });
    std::string current;
    uint32_t index = 0;
    for (auto& entry : entries_) {
        if (entry.directory_key != current) {
            current = entry.directory_key;
            index = 0;
        }
        entry.file_index = index++;
    }
    finalized_ = true;
    return Status::Ok;
}

IoNextDirectoryResult IoNextDirectoryIndex::query(
    std::string_view directory, std::string_view pattern, bool restart_scan,
    IoNextDirectoryCursor* cursor, IoNextDirectoryEntry* out) const {
    if (!finalized_ || !cursor || !out) return IoNextDirectoryResult::NoSuchFile;
    std::string key;
    if (directory_key(directory, &key) != Status::Ok)
        return IoNextDirectoryResult::NoSuchFile;

    if (!cursor->initialized || !pattern.empty()) {
        cursor->directory_key = key;
        cursor->pattern = pattern.empty() ? "*" : std::string(pattern);
        cursor->next = 0;
        cursor->initialized = true;
    } else if (cursor->directory_key != key) {
        cursor->directory_key = key;
        cursor->pattern = "*";
        cursor->next = 0;
    } else if (restart_scan) {
        cursor->next = 0;
    }

    const bool first = cursor->next == 0;
    for (size_t i = cursor->next; i < entries_.size(); ++i) {
        const Entry& entry = entries_[i];
        if (entry.directory_key < key) continue;
        if (entry.directory_key > key) break;
        cursor->next = i + 1;
        if (!wildcard_match_ci(cursor->pattern, entry.name)) continue;
        *out = {entry.guest_path, entry.name, entry.file_index};
        return IoNextDirectoryResult::Found;
    }
    return first ? IoNextDirectoryResult::NoSuchFile
                 : IoNextDirectoryResult::NoMoreFiles;
}

}  // namespace rcomp::rt
