// R-comp shelf: file helpers (see fsutil.h).
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fsutil.h"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rshelf
{
bool IsRegular(const std::string& path, uint64_t* size)
{
	struct stat st = {};
	if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
		return false;
	if (size)
		*size = static_cast<uint64_t>(st.st_size);
	return true;
}

bool IsDirectory(const std::string& path)
{
	struct stat st = {};
	return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> ListDir(const std::string& dir)
{
	std::vector<std::string> out;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
		{
			const std::string n = e->d_name;
			if (n != "." && n != "..")
				out.push_back(n);
		}
		closedir(d);
	}
	std::sort(out.begin(), out.end());
	return out;
}

std::string Lower(std::string s)
{
	for (char& c : s)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	return s;
}

std::string FindNoCase(const std::string& dir, const std::string& name)
{
	const std::string want = Lower(name);
	for (const std::string& n : ListDir(dir))
		if (Lower(n) == want)
			return dir + "/" + n;
	return {};
}

bool ReadWhole(const std::string& path, std::string& out, size_t limit)
{
	out.clear();
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char buf[16384];
	size_t n;
	bool ok = true;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
	{
		if (out.size() + n > limit)
		{
			ok = false;
			break;
		}
		out.append(buf, n);
	}
	ok = ok && !std::ferror(f);
	std::fclose(f);
	return ok;
}

bool ReadRange(const std::string& path, uint64_t offset, size_t size, std::vector<uint8_t>& out)
{
	out.assign(size, 0);
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	const bool ok = fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0 && std::fread(out.data(), 1, size, f) == size;
	std::fclose(f);
	return ok;
}

bool WriteAtomic(const std::string& path, const void* data, size_t size)
{
	const std::string tmp = path + ".part";
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	const bool written = std::fwrite(data, 1, size, f) == size;
	const bool closed = std::fclose(f) == 0;
	if (!written || !closed || std::rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

void MakeDirs(const std::string& path)
{
	for (size_t i = 1; i <= path.size(); i++)
		if (i == path.size() || path[i] == '/')
			mkdir(path.substr(0, i).c_str(), 0777);
}

uint64_t TreeBytes(const std::string& root)
{
	uint64_t total = 0;
	for (const std::string& n : ListDir(root))
	{
		const std::string p = root + "/" + n;
		uint64_t size = 0;
		if (IsRegular(p, &size))
			total += size;
		else if (IsDirectory(p))
			total += TreeBytes(p);
	}
	return total;
}
} // namespace rshelf
