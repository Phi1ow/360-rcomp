// R-comp shelf: a small JSON reader (x360db's info.json, the titles' sce_sys/param.json). The console
// build carries no JSON library and the shelf needs a few strings out of small documents.
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace rshelf
{
struct Json
{
	enum Kind
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object
	} kind = Null;
	std::string str;
	std::vector<Json> items;                           // Array
	struct Member;
	std::vector<Member> members; // Object, in order

	const Json* Get(const char* key) const; // the first member named `key`, or null
	std::string Str(const char* key) const; // that member's string ("" when it isn't one)
};

struct Json::Member
{
	std::string key;
	Json value;
};

// False when `text` isn't one JSON value (nesting deeper than 32 levels counts as malformed).
bool ParseJson(const std::string& text, Json& out);
} // namespace rshelf
