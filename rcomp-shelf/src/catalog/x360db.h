// R-comp shelf: the Xbox 360 metadata archive the shelf names games and finds covers with:
// xenia-manager/x360db (titles/<TitleID>/info.json and titles/<TitleID>/artwork/boxart.jpg).
//
// Several products can share one title ID: Grand Theft Auto IV and its Episodes from Liberty City disc are
// both 545407F2. The archive keeps one box art per title ID but lists every disc (media ID) with its own
// product name, so a disc is named after its media entry, and the title ID's box art is used only for a
// disc of the product most of the title's discs belong to.
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rshelf
{
constexpr const char* kX360dbInfoUrl = "https://raw.githubusercontent.com/xenia-manager/x360db/main/titles/${serial}/info.json";
constexpr const char* kX360dbBoxartUrl =
	"https://raw.githubusercontent.com/xenia-manager/x360db/main/titles/${serial}/artwork/boxart.jpg";

struct DbMedia
{
	std::string media_id; // "06759F9C"
	std::string title;    // "Grand Theft Auto: Episodes from Liberty City"
	std::string region;   // "World"
};

struct DbTitle
{
	std::string id;    // "545407F2"
	std::string title; // title.full ("GTA IV")
	std::vector<DbMedia> media;
};

// False when `json` isn't an info.json object with an id.
bool ParseInfoJson(const std::string& json, DbTitle& out);

struct DiscName
{
	std::string title;  // the disc's product, else the title's own name ("" when neither)
	std::string region; // the disc's region ("" when the disc isn't listed)
	bool listed = false;
	// The title ID's box art shows this disc's product: the disc isn't listed (nothing says otherwise), or
	// its product is the one most of the title's discs carry (the first such product on a tie).
	bool title_art = true;
};
DiscName NameDisc(const DbTitle& t, uint32_t media_id);

// `url` with "${serial}" replaced.
std::string ExpandUrl(const char* url, const std::string& serial);
} // namespace rshelf
