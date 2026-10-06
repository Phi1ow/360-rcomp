// R-comp shelf: XEX2 execution info and XDBF title data (see xex.h).
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xex.h"

#include <cstdio>
#include <cstring>

namespace rshelf
{
namespace
{
constexpr uint32_t kKeyResourceInfo = 0x000002FF;
constexpr uint32_t kKeyFileFormat = 0x000003FF;
constexpr uint32_t kKeyImageBase = 0x00010201;
constexpr uint32_t kKeyExecutionInfo = 0x00040006;
constexpr size_t kMaxHeader = 1u << 20;   // XEX headers are a few KB; anything larger is not one
constexpr size_t kMaxXdbf = 16u << 20;    // the shelf only needs the strings and one small image

uint32_t Be32(const uint8_t* p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t Be16(const uint8_t* p)
{
	return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
uint64_t Be64(const uint8_t* p)
{
	return (uint64_t(Be32(p)) << 32) | Be32(p + 4);
}

bool Fail(std::string* error, const char* what)
{
	if (error)
		*error = what;
	return false;
}

// XSTR: magic, version, size, count (u16), then {id u16, length u16, UTF-8 bytes} entries.
bool StringFromTable(const uint8_t* t, size_t n, uint16_t want, std::string& out)
{
	if (n < 14 || std::memcmp(t, "XSTR", 4) != 0)
		return false;
	const uint16_t count = Be16(t + 12);
	size_t p = 14;
	for (uint16_t i = 0; i < count; i++)
	{
		if (p + 4 > n)
			return false;
		const uint16_t id = Be16(t + p), len = Be16(t + p + 2);
		if (p + 4 + len > n)
			return false;
		if (id == want)
		{
			out.assign(reinterpret_cast<const char*>(t + p + 4), len);
			return true;
		}
		p += 4u + len;
	}
	return false;
}
} // namespace

std::string Hex8(uint32_t v)
{
	char buf[9];
	std::snprintf(buf, sizeof(buf), "%08X", v);
	return buf;
}

// XDBF: magic, version, entry table length, entry count, free table length, free count (u32 each), the
// entries {namespace u16, id u64, offset u32, length u32}, the free entries {offset u32, length u32}, then
// the data the offsets count from. Namespace 1 holds metadata (XSTC: the default language), 2 the images
// and 3 one string table per language (the entry's id).
bool ParseXdbf(const uint8_t* d, size_t n, std::string& title, std::vector<uint8_t>& png)
{
	title.clear();
	png.clear();
	if (n < 0x18 || std::memcmp(d, "XDBF", 4) != 0)
		return false;
	const uint32_t entry_len = Be32(d + 8), entry_count = Be32(d + 12), free_len = Be32(d + 16);
	if (entry_count > entry_len || entry_len > (n / 18) || free_len > (n / 8))
		return false;
	const uint64_t data = 0x18 + uint64_t(entry_len) * 18 + uint64_t(free_len) * 8;
	if (data > n)
		return false;
	uint32_t default_lang = 1; // English when XSTC is missing
	struct Blob
	{
		const uint8_t* p;
		size_t n;
	};
	auto blob = [&](uint32_t i, Blob& b) {
		const uint8_t* e = d + 0x18 + size_t(i) * 18;
		const uint64_t off = data + Be32(e + 10), len = Be32(e + 14);
		if (off + len > n)
			return false;
		b = {d + off, static_cast<size_t>(len)};
		return true;
	};
	for (uint32_t i = 0; i < entry_count; i++)
	{
		const uint8_t* e = d + 0x18 + size_t(i) * 18;
		Blob b;
		if (Be16(e) == 1 && Be64(e + 2) == 0x58535443 /* 'XSTC' */ && blob(i, b) && b.n >= 16 && std::memcmp(b.p, "XSTC", 4) == 0)
			default_lang = Be32(b.p + 12);
	}
	std::string english;
	for (uint32_t i = 0; i < entry_count; i++)
	{
		const uint8_t* e = d + 0x18 + size_t(i) * 18;
		const uint16_t ns = Be16(e);
		const uint64_t id = Be64(e + 2);
		Blob b;
		if (!blob(i, b))
			continue;
		if (ns == 2 && id == 0x8000 && b.n > 8 && std::memcmp(b.p, "\x89PNG", 4) == 0)
			png.assign(b.p, b.p + b.n);
		else if (ns == 3)
		{
			std::string s;
			if (!StringFromTable(b.p, b.n, 0x8000, s) || s.empty())
				continue;
			if (id == default_lang)
				title = s;
			if (id == 1)
				english = s;
		}
	}
	if (title.empty())
		title = english;
	return !title.empty() || !png.empty();
}

bool ParseXex(const std::vector<uint8_t>& head, ReadAt read_at, void* ctx, XexInfo& out, std::string* error)
{
	out = XexInfo();
	const uint8_t* d = head.data();
	const size_t n = head.size();
	if (n < 24 || std::memcmp(d, "XEX2", 4) != 0)
		return Fail(error, "not a XEX2 file");
	const uint32_t header_size = Be32(d + 8), count = Be32(d + 20);
	if (header_size > kMaxHeader || header_size > n || 24 + uint64_t(count) * 8 > header_size)
		return Fail(error, "XEX header outside the file");
	uint32_t exec = 0, resources = 0, format = 0, image_base = 0;
	bool have_base = false;
	for (uint32_t i = 0; i < count; i++)
	{
		const uint32_t key = Be32(d + 24 + 8 * i), value = Be32(d + 28 + 8 * i);
		if (key == kKeyExecutionInfo)
			exec = value;
		else if (key == kKeyResourceInfo)
			resources = value;
		else if (key == kKeyFileFormat)
			format = value;
		else if (key == kKeyImageBase)
		{
			image_base = value;
			have_base = true;
		}
	}
	if (exec)
	{
		if (uint64_t(exec) + 24 > header_size)
			return Fail(error, "execution info outside the header");
		out.has_execution_info = true;
		out.media_id = Be32(d + exec);
		out.version = Be32(d + exec + 4);
		out.base_version = Be32(d + exec + 8);
		out.title_id = Be32(d + exec + 12);
		out.disc_number = d[exec + 18];
		out.disc_count = d[exec + 19];
	}
	// The resource lies at its virtual address minus the image base, after the header, only in a plain
	// image (encryption 0, compression 0): what tools/m6 writes as image/plain.xex.
	if (!resources || !format || !have_base || uint64_t(format) + 8 > header_size || uint64_t(resources) + 4 > header_size)
		return true;
	if (Be16(d + format + 4) != 0 || Be16(d + format + 6) != 0)
		return true;
	const uint32_t res_size = Be32(d + resources);
	if (res_size < 4 || uint64_t(resources) + res_size > header_size)
		return true;
	const std::string want = Hex8(out.title_id);
	for (uint32_t p = resources + 4; p + 16 <= resources + res_size; p += 16)
	{
		char name[9] = {};
		std::memcpy(name, d + p, 8);
		const uint32_t address = Be32(d + p + 8), size = Be32(d + p + 12);
		if (want != name || address < image_base || size == 0 || size > kMaxXdbf)
			continue;
		const uint64_t offset = uint64_t(header_size) + (address - image_base);
		std::vector<uint8_t> bytes;
		if (offset + size <= n)
			bytes.assign(d + offset, d + offset + size);
		else if (!read_at || !read_at(ctx, offset, size, bytes) || bytes.size() != size)
			break;
		out.has_xdbf = ParseXdbf(bytes.data(), bytes.size(), out.xdbf_title, out.title_png);
		break;
	}
	return true;
}

namespace
{
bool FileReadAt(void* ctx, uint64_t offset, size_t size, std::vector<uint8_t>& out)
{
	FILE* f = static_cast<FILE*>(ctx);
	out.resize(size);
	if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0)
		return false;
	out.resize(std::fread(out.data(), 1, size, f));
	return out.size() == size;
}
} // namespace

bool ReadXexFile(const std::string& path, XexInfo& out, std::string* error)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return Fail(error, "cannot open the XEX");
	std::vector<uint8_t> head(24);
	bool ok = std::fread(head.data(), 1, head.size(), f) == head.size();
	if (ok && std::memcmp(head.data(), "XEX2", 4) == 0)
	{
		const uint32_t header_size = Be32(head.data() + 8);
		if (header_size <= kMaxHeader && header_size > 24)
		{
			head.resize(header_size);
			ok = std::fread(head.data() + 24, 1, header_size - 24, f) == header_size - 24;
		}
	}
	const bool parsed = ok ? ParseXex(head, FileReadAt, f, out, error) : Fail(error, "short XEX file");
	std::fclose(f);
	return parsed;
}
} // namespace rshelf
