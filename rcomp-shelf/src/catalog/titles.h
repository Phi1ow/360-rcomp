// R-comp shelf: the catalog. A recompiled Xbox 360 title is a PS5 application folder of its own, as R-comp's
// packaging assembles it (eboot.bin, sce_sys/param.json, image/plain.xex, game/...):
//   - installed: /data/homebrew/<PS5 title id>/, where ShadowMount+ mounts it and puts it on the home screen;
//   - a package: the same folder anywhere in a package folder (/data/rcomp/packages/, <USB drive>/rcomp/),
//     waiting to be installed.
// Each title keeps its own PS5 title id (sce_sys/param.json titleId); the shelf never invents one.
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_games.h"
#include "x360db.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rshelf
{
struct CatalogPaths
{
	std::string homebrew_dir = "/data/homebrew";
	std::vector<std::string> package_dirs; // folders holding package folders
	std::string db_cache_dir;              // x360db info.json files: <title ID>.info.json
	std::string self_title_id;             // the shelf's own PS5 title id, never listed
};

// A folder that looks like an R-comp title.
struct TitleFolder
{
	std::string path;
	std::string ps5_title_id; // "PPSA88360"
	std::string param_title;  // sce_sys/param.json's titleName (default language)
	uint64_t bytes = 0;
	std::string fingerprint;  // tells two copies of a title apart (manifest.sha256, else sizes and eboot samples)
};

// False (and *why) when `path` isn't an R-comp title folder: sce_sys/param.json with a PPSA title id,
// eboot.bin and image/plain.xex, all regular files.
bool ReadTitleFolder(const std::string& path, TitleFolder& out, std::string* why);

// The PS5 title id and the default language's titleName from a param.json text. False when it has no
// valid titleId (PPSA and five digits).
bool ParseParamJson(const std::string& json, std::string& title_id, std::string& title_name);
bool IsPs5TitleId(const std::string& id);

// The USB drives' package folders: <drive>/rcomp (any letter case) on /mnt/usb0 to /mnt/usb7.
std::vector<std::string> UsbPackageDirs();

using Log = std::function<void(const std::string&)>;

// Every R-comp title installed or packaged, one entry per PS5 title id, sorted for the shelf. A title id
// both installed and packaged is one entry: Installed when the copies match, UpdateAvailable when they
// differ (the first package found is the one offered). x360db names come from db_cache_dir when present.
std::vector<fe::GameInfo> ScanCatalog(const CatalogPaths& paths, const Log& log);

// Fills `g`'s names and cover key from the XEX facts it holds and the title's x360db entry (null when
// none): the disc's product name, else the XDBF title, else param.json's name, else the PS5 title id.
void NameGame(fe::GameInfo& g, const std::string& param_title, const DbTitle* db);

// The x360db entries cached for these titles (keyed by title ID); unreadable files are skipped.
std::map<std::string, DbTitle> LoadDbCache(const std::string& dir, const std::vector<fe::GameInfo>& games);

// The title IDs whose info.json the cache lacks (no file and no recent .missing marker).
std::vector<std::string> MissingDbEntries(const std::string& dir, const std::vector<fe::GameInfo>& games);

// Downloads those info.json files into the cache (only ones that parse are kept), until `budget_s` passes or
// the network fails. `download` returns the HTTP status (negative: no network). Returns how many were saved.
int FetchDbEntries(const std::string& dir, const std::vector<std::string>& title_ids,
	const std::function<int(const std::string&, std::vector<uint8_t>&)>& download, double budget_s, const Log& log);
} // namespace rshelf
