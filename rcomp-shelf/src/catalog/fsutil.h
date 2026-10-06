// R-comp shelf: small POSIX file helpers shared by the catalog and the installer.
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rshelf
{
bool IsRegular(const std::string& path, uint64_t* size = nullptr); // not following a symlink
bool IsDirectory(const std::string& path);                         // not following a symlink
// The entries of `dir` (no "." or ".."), sorted; empty when it can't be opened.
std::vector<std::string> ListDir(const std::string& dir);
// The entry of `dir` named `name` in any letter case, as a path, or "".
std::string FindNoCase(const std::string& dir, const std::string& name);
// The whole file, at most `limit` bytes; false when it can't be read or is larger.
bool ReadWhole(const std::string& path, std::string& out, size_t limit = 1u << 20);
bool ReadRange(const std::string& path, uint64_t offset, size_t size, std::vector<uint8_t>& out);
// Writes `path`.part, then renames it over `path`.
bool WriteAtomic(const std::string& path, const void* data, size_t size);
void MakeDirs(const std::string& path);
// The bytes of the regular files under `root` (symlinks are not followed and count as nothing).
uint64_t TreeBytes(const std::string& root);
std::string Lower(std::string s);
} // namespace rshelf
