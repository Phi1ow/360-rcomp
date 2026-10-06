// R-comp shelf: what the shelf reads from a recompiled title's decoded XEX (image/plain.xex): the
// execution info (title ID, media ID, version, disc) and, from the XDBF resource named after the title
// ID, the title's name and its 64x64 PNG image.
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rshelf
{
struct XexInfo
{
	bool has_execution_info = false;
	uint32_t title_id = 0;
	uint32_t media_id = 0;
	uint32_t version = 0;
	uint32_t base_version = 0;
	uint8_t disc_number = 0;
	uint8_t disc_count = 0;
	// The XDBF resource (only in a plain image: no encryption, no compression).
	bool has_xdbf = false;
	std::string xdbf_title;         // string 0x8000 in the default language, else English
	std::vector<uint8_t> title_png; // image 0x8000
};

// Execution info, then the XDBF. False (and *error) when the header is not a XEX2 header or lies
// outside the bytes given. The XDBF part is best effort: a missing or damaged one leaves has_xdbf false.
// `image` holds the file's first bytes; `read_at` fetches more of it (offset, size, out) when the
// resource lies beyond them, and may be empty.
using ReadAt = bool (*)(void* ctx, uint64_t offset, size_t size, std::vector<uint8_t>& out);
bool ParseXex(const std::vector<uint8_t>& head, ReadAt read_at, void* ctx, XexInfo& out, std::string* error);

// Reads `path` (the header, then the resource only) and parses it.
bool ReadXexFile(const std::string& path, XexInfo& out, std::string* error);

// The parts of an XDBF blob the shelf uses. False when it isn't one.
bool ParseXdbf(const uint8_t* data, size_t size, std::string& title, std::vector<uint8_t>& png);

// "545407F2"
std::string Hex8(uint32_t v);
} // namespace rshelf
