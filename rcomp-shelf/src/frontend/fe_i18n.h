// R-comp shelf (from PS5SX2's frontend): the shelf's and the notifications' text in the PS5's language (vk-285-110).
//
// The PS5's system language (sceSystemServiceParamGetInt parameter 1) picks a table: English, French,
// Spanish (Spain and Latin America), German, Italian, Dutch and Portuguese (Portugal and Brazil). Other
// languages get English: the shelf's font has the accented Latin letters (Latin-1) but no Cyrillic, Greek
// or CJK. A file <lang_dir>/<code>.txt, when present, replaces entries ("hint.play = Jugar"), so a wrong
// word can be fixed without a new build.
//
// Copyright (C) 2026 Spyros
// Modified for R-comp, 2026: the R-comp shelf's strings.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

namespace fe
{
enum class Str : int
{
	HintPlay,           // "Play"
	HintBrowse,         // "Browse"
	HintJump,           // "Jump"
	HintInstall,        // "Install"
	HintUpdate,         // "Update"
	HintCancel,         // "Cancel"
	HintRefresh,        // "Refresh"
	NoGames,            // "No recompiled games in /data/homebrew or /data/rcomp/packages"
	DownloadingCovers,  // "Downloading covers  %d / %d"
	SizeGB,             // "%s GB"
	SizeMB,             // "%s MB"
	Decimal,            // "."
	StateInstalled,     // "Installed"
	StateNotInstalled,  // "Not installed"
	StateUpdate,        // "Update available"
	Installing,         // "Installing  %s / %s"
	InstallDone,        // "Installed as %s"
	InstallFailed,      // "Install failed: %s"
	InstallCancelled,   // "Install cancelled"
	Starting,           // "Starting %s"
	LaunchFailed,       // "%s didn't start (%s)"
	NotifyCoversOne,    // "R-comp: downloading %d cover"
	NotifyCoversMany,   // "R-comp: downloading %d covers"
	Count
};

// Picks the table for the PS5's language id and reads <lang_dir>/<code>.txt when it's there (an empty
// `lang_dir` reads nothing). Call once at start, before the shelf or any notification.
void SetLanguage(int ps5_language, const std::string& lang_dir);

// The text in the current language (English where the table or the file has nothing).
const char* Tr(Str id);

// "en", "fr", "es", "es-419", "de", "it", "nl", "pt" or "pt-BR".
const char* LanguageCode();

// The override file's key of an entry ("hint.play"), for messages and tests.
const char* Key(Str id);

// "Europe, Australia" with the region names the table knows translated; anything else unchanged.
std::string Region(const std::string& region);

// A title's size: "4.2 GB" / "4,2 GB" / "4,2 Go", or MB under 1 GB.
std::string Size(uint64_t bytes);

// True when `a` and `b` use the same printf conversions in the same order (a translation must).
bool SameFormat(const char* a, const char* b);
} // namespace fe
