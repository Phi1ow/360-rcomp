// R-comp shelf: one game on the shelf. Replaces PS5SX2's fe_games.h (PS2 disc images): here a game is a
// statically recompiled Xbox 360 title, a PS5 application of its own, installed under
// /data/homebrew/<PS5 title id>/ or waiting as a package to be installed there (src/catalog/titles.h).
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fe
{
enum class TitleState
{
	Installed,       // in /data/homebrew, no other package of it
	NotInstalled,    // a package only
	UpdateAvailable, // installed, and a package of it differs from the installed copy
};

struct GameInfo
{
	std::string path;   // the folder the shelf acts on: the installed one, else the package
	std::string file;   // that folder's name
	std::string stem;   // the PS5 title id ("PPSA88360")
	std::string title;  // display title
	std::string region; // the disc's region from x360db ("Europe, Asia"; may be empty)
	std::string extra;  // unused
	std::string serial; // the Xbox 360 title ID ("545407F2"; empty when the XEX could not be read)
	uint64_t bytes = 0; // the folder's size
	std::vector<std::string> badges;

	std::string media;      // the disc's media ID ("4A53F9F6")
	uint32_t version = 0;   // the XEX version
	std::string xdbf_title; // the title's own name ("GTA IV")
	// What downloaded covers are named after (the title ID), or empty when the title ID's box art shows
	// another product than this disc (Episodes from Liberty City shares GTA IV's title ID).
	std::string cover_key;
	// Names a cover file may have in the covers folder, best first: the PS5 title id, "<title ID>-<media ID>",
	// the cover key, the title.
	std::vector<std::string> cover_names;
	std::vector<uint8_t> icon_png; // the XDBF title image (64x64 PNG), drawn on the placeholder

	TitleState state = TitleState::Installed;
	std::string installed_path; // /data/homebrew/<PPSA> ("" when not installed)
	std::string package_path;   // the package folder ("" when none)
	uint64_t package_bytes = 0;
};

// The shelf's order: by title (case-insensitive), then by PS5 title id.
void SortGames(std::vector<GameInfo>& games);
} // namespace fe
