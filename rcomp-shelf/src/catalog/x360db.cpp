// R-comp shelf: x360db info.json (see x360db.h).
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "x360db.h"

#include "json.h"
#include "xex.h"

#include <map>

namespace rshelf
{
namespace
{
std::string Upper(std::string s)
{
	for (char& c : s)
		if (c >= 'a' && c <= 'z')
			c = static_cast<char>(c - 'a' + 'A');
	return s;
}
} // namespace

bool ParseInfoJson(const std::string& json, DbTitle& out)
{
	out = DbTitle();
	Json root;
	if (!ParseJson(json, root) || root.kind != Json::Object)
		return false;
	out.id = Upper(root.Str("id"));
	if (out.id.empty())
		return false;
	if (const Json* t = root.Get("title"))
		out.title = t->kind == Json::Object ? t->Str("full") : t->kind == Json::String ? t->str : std::string();
	if (const Json* m = root.Get("media"); m && m->kind == Json::Array)
		for (const Json& e : m->items)
			if (e.kind == Json::Object && !e.Str("media_id").empty())
				out.media.push_back({Upper(e.Str("media_id")), e.Str("title"), e.Str("region")});
	return true;
}

DiscName NameDisc(const DbTitle& t, uint32_t media_id)
{
	DiscName r;
	r.title = t.title;
	const std::string want = Hex8(media_id);
	const DbMedia* mine = nullptr;
	for (const DbMedia& m : t.media)
		if (m.media_id == want)
			mine = &m;
	if (!mine)
		return r;
	r.listed = true;
	r.region = mine->region;
	if (!mine->title.empty())
		r.title = mine->title;
	// The product most discs carry, the first one listed on a tie.
	std::map<std::string, int> counts;
	for (const DbMedia& m : t.media)
		if (!m.title.empty())
			counts[m.title]++;
	std::string main;
	int best = 0;
	for (const DbMedia& m : t.media)
		if (!m.title.empty() && counts[m.title] > best)
		{
			best = counts[m.title];
			main = m.title;
		}
	r.title_art = mine->title.empty() || mine->title == main;
	return r;
}

std::string ExpandUrl(const char* url, const std::string& serial)
{
	std::string s = url;
	const size_t at = s.find("${serial}");
	if (at != std::string::npos)
		s.replace(at, 9, serial);
	return s;
}
} // namespace rshelf
