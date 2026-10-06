// R-comp shelf: the catalog (see titles.h).
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "titles.h"

#include "fsutil.h"
#include "json.h"
#include "xex.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <time.h>

namespace fe
{
void SortGames(std::vector<GameInfo>& games)
{
	std::stable_sort(games.begin(), games.end(), [](const GameInfo& a, const GameInfo& b) {
		const std::string la = rshelf::Lower(a.title), lb = rshelf::Lower(b.title);
		if (la != lb)
			return la < lb;
		return a.stem < b.stem;
	});
}
} // namespace fe

namespace rshelf
{
namespace
{
uint64_t Fnv(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull)
{
	const auto* b = static_cast<const uint8_t*>(p);
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 0x100000001b3ull;
	return h;
}

std::string Hex16(uint64_t v)
{
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
	return buf;
}

double Now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
}

bool RecentMarker(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && std::time(nullptr) - st.st_mtime < 14 * 24 * 3600;
}

// The packaged manifest when there is one (R-comp's link writes manifest.sha256 into the title folder), else
// the two large files' sizes and samples of the eboot's first and last megabyte.
std::string Fingerprint(const std::string& dir)
{
	std::string manifest;
	if (IsRegular(dir + "/manifest.sha256") && ReadWhole(dir + "/manifest.sha256", manifest) && !manifest.empty())
		return "manifest:" + Hex16(Fnv(manifest.data(), manifest.size()));
	uint64_t eboot = 0, xex = 0;
	IsRegular(dir + "/eboot.bin", &eboot);
	IsRegular(dir + "/image/plain.xex", &xex);
	uint64_t h = Fnv(&eboot, sizeof(eboot));
	h = Fnv(&xex, sizeof(xex), h);
	const size_t sample = static_cast<size_t>(std::min<uint64_t>(eboot, 1u << 20));
	std::vector<uint8_t> bytes;
	if (sample && ReadRange(dir + "/eboot.bin", 0, sample, bytes))
		h = Fnv(bytes.data(), bytes.size(), h);
	if (sample && ReadRange(dir + "/eboot.bin", eboot - sample, sample, bytes))
		h = Fnv(bytes.data(), bytes.size(), h);
	return "sample:" + Hex16(h);
}

fe::GameInfo FromFolder(const TitleFolder& t, const XexInfo& x)
{
	fe::GameInfo g;
	g.path = t.path;
	g.file = t.path.substr(t.path.rfind('/') + 1);
	g.stem = t.ps5_title_id;
	g.bytes = t.bytes;
	if (x.has_execution_info)
	{
		g.serial = Hex8(x.title_id);
		g.media = Hex8(x.media_id);
		g.version = x.version;
	}
	g.xdbf_title = x.xdbf_title;
	g.icon_png = x.title_png;
	return g;
}
} // namespace

bool IsPs5TitleId(const std::string& id)
{
	if (id.size() != 9 || id.compare(0, 4, "PPSA") != 0)
		return false;
	for (size_t i = 4; i < 9; i++)
		if (id[i] < '0' || id[i] > '9')
			return false;
	return true;
}

bool ParseParamJson(const std::string& json, std::string& title_id, std::string& title_name)
{
	title_id.clear();
	title_name.clear();
	Json root;
	if (!ParseJson(json, root) || root.kind != Json::Object)
		return false;
	title_id = root.Str("titleId");
	if (const Json* loc = root.Get("localizedParameters"); loc && loc->kind == Json::Object)
	{
		const std::string lang = loc->Str("defaultLanguage");
		const Json* entry = lang.empty() ? nullptr : loc->Get(lang.c_str());
		if (!entry)
			entry = loc->Get("en-US");
		if (entry && entry->kind == Json::Object)
			title_name = entry->Str("titleName");
	}
	return IsPs5TitleId(title_id);
}

bool ReadTitleFolder(const std::string& path, TitleFolder& out, std::string* why)
{
	out = TitleFolder();
	auto no = [&](const char* w) {
		if (why)
			*why = w;
		return false;
	};
	if (!IsDirectory(path))
		return no("not a folder");
	if (!IsRegular(path + "/image/plain.xex"))
		return no("no image/plain.xex (not an R-comp title)");
	if (!IsRegular(path + "/eboot.bin"))
		return no("no eboot.bin");
	std::string param;
	if (!IsRegular(path + "/sce_sys/param.json") || !ReadWhole(path + "/sce_sys/param.json", param))
		return no("no sce_sys/param.json");
	if (!ParseParamJson(param, out.ps5_title_id, out.param_title))
		return no("sce_sys/param.json has no PPSA title id");
	out.path = path;
	out.bytes = TreeBytes(path);
	out.fingerprint = Fingerprint(path);
	return true;
}

std::vector<std::string> UsbPackageDirs()
{
	std::vector<std::string> out;
	for (int i = 0; i < 8; i++)
	{
		const std::string root = "/mnt/usb" + std::to_string(i);
		const std::string dir = FindNoCase(root, "rcomp");
		if (!dir.empty() && IsDirectory(dir))
			out.push_back(dir);
	}
	return out;
}

void NameGame(fe::GameInfo& g, const std::string& param_title, const DbTitle* db)
{
	DiscName disc;
	if (db && !g.media.empty())
		disc = NameDisc(*db, static_cast<uint32_t>(std::strtoul(g.media.c_str(), nullptr, 16)));
	g.title = !disc.title.empty() ? disc.title : !g.xdbf_title.empty() ? g.xdbf_title : !param_title.empty() ? param_title : g.stem;
	g.region = disc.region;
	g.cover_key = (!g.serial.empty() && disc.title_art) ? g.serial : std::string();
	g.cover_names.clear();
	g.cover_names.push_back(g.stem);
	if (!g.serial.empty() && !g.media.empty())
		g.cover_names.push_back(g.serial + "-" + g.media);
	if (!g.cover_key.empty())
		g.cover_names.push_back(g.cover_key);
	g.cover_names.push_back(g.title);
}

std::map<std::string, DbTitle> LoadDbCache(const std::string& dir, const std::vector<fe::GameInfo>& games)
{
	std::map<std::string, DbTitle> out;
	if (dir.empty())
		return out;
	for (const fe::GameInfo& g : games)
	{
		if (g.serial.empty() || out.count(g.serial))
			continue;
		std::string text;
		DbTitle t;
		if (ReadWhole(dir + "/" + g.serial + ".info.json", text, 4u << 20) && ParseInfoJson(text, t))
			out.emplace(g.serial, std::move(t));
	}
	return out;
}

std::vector<std::string> MissingDbEntries(const std::string& dir, const std::vector<fe::GameInfo>& games)
{
	std::vector<std::string> out;
	if (dir.empty())
		return out;
	for (const fe::GameInfo& g : games)
		if (!g.serial.empty() && std::find(out.begin(), out.end(), g.serial) == out.end() &&
			!IsRegular(dir + "/" + g.serial + ".info.json") && !RecentMarker(dir + "/" + g.serial + ".info.missing"))
			out.push_back(g.serial);
	return out;
}

int FetchDbEntries(const std::string& dir, const std::vector<std::string>& title_ids,
	const std::function<int(const std::string&, std::vector<uint8_t>&)>& download, double budget_s, const Log& log)
{
	if (dir.empty() || title_ids.empty() || !download)
		return 0;
	MakeDirs(dir);
	const double t0 = Now();
	int saved = 0;
	for (const std::string& id : title_ids)
	{
		if (Now() - t0 >= budget_s)
		{
			if (log)
				log("x360db: out of time; the rest next time");
			break;
		}
		std::vector<uint8_t> bytes;
		const int status = download(ExpandUrl(kX360dbInfoUrl, id), bytes);
		DbTitle t;
		const std::string text(bytes.begin(), bytes.end());
		bool ok = false;
		if (status == 200 && ParseInfoJson(text, t) && t.id == id)
			ok = WriteAtomic(dir + "/" + id + ".info.json", text.data(), text.size());
		else if (status == 404)
			WriteAtomic(dir + "/" + id + ".info.missing", "404\n", 4);
		if (log)
			log("x360db " + id + ": " + std::to_string(status) + (ok ? " (saved)" : status == 200 ? " (not an info.json; not kept)" : ""));
		saved += ok ? 1 : 0;
		if (status < 0)
			break; // no network: no point in trying the rest now
	}
	return saved;
}

std::vector<fe::GameInfo> ScanCatalog(const CatalogPaths& paths, const Log& log)
{
	struct Found
	{
		TitleFolder folder;
		XexInfo xex;
	};
	auto read = [&](const std::string& dir, std::vector<Found>& out, bool quiet_non_titles) {
		for (const std::string& name : ListDir(dir))
		{
			if (name.empty() || name[0] == '.')
				continue;
			Found f;
			std::string why;
			if (!ReadTitleFolder(dir + "/" + name, f.folder, &why))
			{
				if (!quiet_non_titles && log)
					log(dir + "/" + name + ": skipped, " + why);
				continue;
			}
			if (f.folder.ps5_title_id == paths.self_title_id)
				continue;
			std::string error;
			if (!ReadXexFile(f.folder.path + "/image/plain.xex", f.xex, &error) && log)
				log(f.folder.path + ": image/plain.xex: " + error);
			out.push_back(std::move(f));
		}
	};
	std::vector<Found> installed, packages;
	// /data/homebrew also holds other homebrew applications: they are not the shelf's business.
	read(paths.homebrew_dir, installed, true);
	for (const std::string& dir : paths.package_dirs)
		read(dir, packages, false);

	std::vector<fe::GameInfo> games;
	std::vector<std::string> param_titles;
	for (const Found& f : installed)
	{
		// An installed folder named otherwise than its title id is not where ShadowMount+ expects it.
		if (f.folder.path.substr(f.folder.path.rfind('/') + 1) != f.folder.ps5_title_id)
		{
			if (log)
				log(f.folder.path + ": skipped, its param.json says " + f.folder.ps5_title_id);
			continue;
		}
		fe::GameInfo g = FromFolder(f.folder, f.xex);
		g.state = fe::TitleState::Installed;
		g.installed_path = f.folder.path;
		games.push_back(std::move(g));
		param_titles.push_back(f.folder.param_title);
	}
	for (const Found& p : packages)
	{
		auto it = std::find_if(games.begin(), games.end(), [&](const fe::GameInfo& g) { return g.stem == p.folder.ps5_title_id; });
		if (it == games.end())
		{
			fe::GameInfo g = FromFolder(p.folder, p.xex);
			g.state = fe::TitleState::NotInstalled;
			g.package_path = p.folder.path;
			g.package_bytes = p.folder.bytes;
			games.push_back(std::move(g));
			param_titles.push_back(p.folder.param_title);
			continue;
		}
		if (!it->package_path.empty())
		{
			if (log)
				log(p.folder.path + ": another package of " + p.folder.ps5_title_id + " was found first (" + it->package_path + ")");
			continue;
		}
		const auto inst = std::find_if(installed.begin(), installed.end(), [&](const Found& f) { return f.folder.path == it->installed_path; });
		if (inst != installed.end() && inst->folder.fingerprint == p.folder.fingerprint)
			continue; // the same build as the installed one
		it->package_path = p.folder.path;
		it->package_bytes = p.folder.bytes;
		if (it->state == fe::TitleState::Installed)
			it->state = fe::TitleState::UpdateAvailable;
	}

	const std::map<std::string, DbTitle> db = LoadDbCache(paths.db_cache_dir, games);
	for (size_t i = 0; i < games.size(); i++)
	{
		const auto it = db.find(games[i].serial);
		NameGame(games[i], param_titles[i], it == db.end() ? nullptr : &it->second);
	}
	// Without x360db's entry nothing says whether a title ID's box art shows this disc. Two discs of one title
	// ID (GTA IV and Episodes from Liberty City are both 545407F2) then wait for the entry before using it.
	for (fe::GameInfo& g : games)
	{
		if (g.serial.empty() || db.count(g.serial))
			continue;
		for (const fe::GameInfo& o : games)
			if (&o != &g && o.serial == g.serial && o.media != g.media)
			{
				g.cover_key.clear();
				g.cover_names.erase(std::remove(g.cover_names.begin(), g.cover_names.end(), g.serial), g.cover_names.end());
			}
	}
	fe::SortGames(games);
	if (log)
		for (const fe::GameInfo& g : games)
		{
			const char* state = g.state == fe::TitleState::Installed ? "installed" :
								g.state == fe::TitleState::NotInstalled ? "package" : "installed, update packaged";
			log(g.stem + " | " + (g.serial.empty() ? "no title ID" : g.serial + "/" + g.media) + " | " + g.title + " | " + state + " | " +
				std::to_string(g.bytes) + " bytes | " + (g.installed_path.empty() ? g.package_path : g.installed_path) +
				(g.cover_key.empty() ? " | no title cover" : ""));
		}
	return games;
}
} // namespace rshelf
