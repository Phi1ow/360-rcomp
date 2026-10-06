// R-comp shelf: host tests of the catalog and the installer (no GPU, no console, no game data: every XEX here
// is made by the test). Run from the repository root: build/host/shelf_tests.
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fsutil.h"
#include "install.h"
#include "json.h"
#include "titles.h"
#include "x360db.h"
#include "xex.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{
int g_failed = 0, g_checks = 0;

#define CHECK(cond)                                                                    \
	do                                                                                 \
	{                                                                                  \
		g_checks++;                                                                    \
		if (!(cond))                                                                   \
		{                                                                              \
			g_failed++;                                                                \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
		}                                                                              \
	} while (0)

void Put32(std::vector<uint8_t>& b, size_t o, uint32_t v)
{
	b[o] = static_cast<uint8_t>(v >> 24);
	b[o + 1] = static_cast<uint8_t>(v >> 16);
	b[o + 2] = static_cast<uint8_t>(v >> 8);
	b[o + 3] = static_cast<uint8_t>(v);
}
void Put16(std::vector<uint8_t>& b, size_t o, uint16_t v)
{
	b[o] = static_cast<uint8_t>(v >> 8);
	b[o + 1] = static_cast<uint8_t>(v);
}
void Append32(std::vector<uint8_t>& b, uint32_t v)
{
	b.resize(b.size() + 4);
	Put32(b, b.size() - 4, v);
}
void Append16(std::vector<uint8_t>& b, uint16_t v)
{
	b.resize(b.size() + 2);
	Put16(b, b.size() - 2, v);
}

// An XDBF: XSTC (default language `lang`), XSTR tables {language, title}, and an image when `png` isn't empty.
std::vector<uint8_t> MakeXdbf(uint32_t lang, const std::vector<std::pair<uint32_t, std::string>>& titles, const std::vector<uint8_t>& png)
{
	struct Blob
	{
		uint16_t ns;
		uint64_t id;
		std::vector<uint8_t> data;
	};
	std::vector<Blob> blobs;
	std::vector<uint8_t> xstc = {'X', 'S', 'T', 'C'};
	Append32(xstc, 1);
	Append32(xstc, 4);
	Append32(xstc, lang);
	blobs.push_back({1, 0x58535443, xstc});
	for (const auto& t : titles)
	{
		std::vector<uint8_t> x = {'X', 'S', 'T', 'R'};
		Append32(x, 1);
		Append32(x, static_cast<uint32_t>(2 + 4 + t.second.size()));
		Append16(x, 2);
		Append16(x, 0x0001); // another string first
		Append16(x, 2);
		x.push_back('h');
		x.push_back('i');
		Append16(x, 0x8000);
		Append16(x, static_cast<uint16_t>(t.second.size()));
		x.insert(x.end(), t.second.begin(), t.second.end());
		blobs.push_back({3, t.first, x});
	}
	if (!png.empty())
		blobs.push_back({2, 0x8000, png});
	std::vector<uint8_t> out = {'X', 'D', 'B', 'F'};
	Append32(out, 0x10000);
	Append32(out, static_cast<uint32_t>(blobs.size()));
	Append32(out, static_cast<uint32_t>(blobs.size()));
	Append32(out, 0);
	Append32(out, 0);
	std::vector<uint8_t> body;
	for (const Blob& b : blobs)
	{
		Append16(out, b.ns);
		Append32(out, static_cast<uint32_t>(b.id >> 32));
		Append32(out, static_cast<uint32_t>(b.id));
		Append32(out, static_cast<uint32_t>(body.size()));
		Append32(out, static_cast<uint32_t>(b.data.size()));
		body.insert(body.end(), b.data.begin(), b.data.end());
	}
	out.insert(out.end(), body.begin(), body.end());
	return out;
}

// A plain XEX2: execution info, file format (encryption/compression), image base, and the XDBF as a resource.
std::vector<uint8_t> MakeXex(uint32_t title_id, uint32_t media_id, const std::vector<uint8_t>& xdbf, uint16_t compression = 0)
{
	const uint32_t base = 0x82000000, head = 0x1000, res_addr = base + 0x10000;
	std::vector<uint8_t> b(head + 0x10000 + xdbf.size(), 0);
	std::memcpy(b.data(), "XEX2", 4);
	Put32(b, 4, 1);
	Put32(b, 8, head);
	Put32(b, 16, 0x100);
	Put32(b, 20, 4);
	const uint32_t keys[4][2] = {{0x2FF, 0x200}, {0x3FF, 0x220}, {0x10201, base}, {0x40006, 0x240}};
	for (int i = 0; i < 4; i++)
	{
		Put32(b, 24 + 8 * i, keys[i][0]);
		Put32(b, 28 + 8 * i, keys[i][1]);
	}
	Put32(b, 0x200, 20);
	char name[9];
	std::snprintf(name, sizeof(name), "%08X", title_id);
	std::memcpy(&b[0x204], name, 8);
	Put32(b, 0x20C, res_addr);
	Put32(b, 0x210, static_cast<uint32_t>(xdbf.size()));
	Put32(b, 0x220, 8);
	Put16(b, 0x224, 0);
	Put16(b, 0x226, compression);
	Put32(b, 0x240, media_id);
	Put32(b, 0x244, 7);
	Put32(b, 0x248, 3);
	Put32(b, 0x24C, title_id);
	b[0x240 + 18] = 1;
	b[0x240 + 19] = 2;
	std::memcpy(&b[head + (res_addr - base)], xdbf.data(), xdbf.size());
	return b;
}

void WriteBytes(const std::string& path, const std::vector<uint8_t>& b)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	std::fwrite(b.data(), 1, b.size(), f);
	std::fclose(f);
}
void WriteText(const std::string& path, const std::string& s)
{
	WriteBytes(path, std::vector<uint8_t>(s.begin(), s.end()));
}

// A title folder as R-comp's packaging makes it, with a test XEX.
void MakeTitle(const std::string& dir, const std::string& ps5_id, uint32_t title_id, uint32_t media_id, const std::string& name,
	const std::string& eboot = "eboot v1")
{
	rshelf::MakeDirs(dir + "/image");
	rshelf::MakeDirs(dir + "/sce_sys");
	rshelf::MakeDirs(dir + "/game/sub");
	WriteText(dir + "/sce_sys/param.json", "{\"titleId\": \"" + ps5_id +
		"\", \"localizedParameters\": {\"defaultLanguage\": \"en-US\", \"en-US\": {\"titleName\": \"param " + name + "\"}}}");
	WriteText(dir + "/eboot.bin", eboot);
	WriteBytes(dir + "/image/plain.xex", MakeXex(title_id, media_id, MakeXdbf(1, {{1, name}}, {})));
	WriteBytes(dir + "/game/sub/data.bin", std::vector<uint8_t>(300000, 7));
}

bool Exists(const std::string& p)
{
	return access(p.c_str(), F_OK) == 0;
}

void TestXex()
{
	const std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 1, 2, 3};
	const auto x = MakeXdbf(3, {{1, "English name"}, {3, "Deutscher Name"}}, png);
	std::string title;
	std::vector<uint8_t> img;
	CHECK(rshelf::ParseXdbf(x.data(), x.size(), title, img));
	CHECK(title == "Deutscher Name"); // the default language first
	CHECK(img == png);
	const auto x2 = MakeXdbf(9, {{1, "English name"}}, {});
	CHECK(rshelf::ParseXdbf(x2.data(), x2.size(), title, img) && title == "English name" && img.empty());
	CHECK(!rshelf::ParseXdbf(x2.data(), 10, title, img));
	std::vector<uint8_t> bad = x2;
	Put32(bad, 8, 0xFFFFFF); // an entry table larger than the blob
	CHECK(!rshelf::ParseXdbf(bad.data(), bad.size(), title, img));

	const auto xex = MakeXex(0x545407F2, 0x4A53F9F6, x);
	rshelf::XexInfo info;
	std::string error;
	CHECK(rshelf::ParseXex(xex, nullptr, nullptr, info, &error));
	CHECK(info.has_execution_info && info.title_id == 0x545407F2 && info.media_id == 0x4A53F9F6 && info.version == 7);
	CHECK(info.disc_number == 1 && info.disc_count == 2);
	CHECK(info.has_xdbf && info.xdbf_title == "Deutscher Name");

	// Only the header in memory: the resource comes through read_at.
	std::vector<uint8_t> head(xex.begin(), xex.begin() + 0x1000);
	struct Ctx
	{
		const std::vector<uint8_t>* all;
		int calls;
	} ctx{&xex, 0};
	auto read_at = [](void* c, uint64_t off, size_t n, std::vector<uint8_t>& out) {
		auto* k = static_cast<Ctx*>(c);
		k->calls++;
		if (off + n > k->all->size())
			return false;
		out.assign(k->all->begin() + static_cast<long>(off), k->all->begin() + static_cast<long>(off + n));
		return true;
	};
	CHECK(rshelf::ParseXex(head, read_at, &ctx, info, &error) && info.has_xdbf && ctx.calls == 1);

	// A compressed image keeps its execution info but has no readable resource.
	CHECK(rshelf::ParseXex(MakeXex(0x11111111, 2, x, 1), nullptr, nullptr, info, &error) && info.title_id == 0x11111111 && !info.has_xdbf);
	CHECK(!rshelf::ParseXex(std::vector<uint8_t>(64, 0), nullptr, nullptr, info, &error));
	std::vector<uint8_t> trunc = xex;
	Put32(trunc, 8, 0x7FFFFFFF);
	CHECK(!rshelf::ParseXex(trunc, nullptr, nullptr, info, &error));
	CHECK(rshelf::Hex8(0x545407F2) == "545407F2");
}

void TestJsonAndDb()
{
	rshelf::Json j;
	CHECK(rshelf::ParseJson("{\"a\": [1, -2.5e3, true, null, \"x\\u00e9\\n\"], \"b\": {}}", j));
	CHECK(j.kind == rshelf::Json::Object && j.Get("a") && j.Get("a")->items.size() == 5);
	CHECK(j.Get("a")->items[4].str == "x\xC3\xA9\n");
	CHECK(rshelf::ParseJson("\"\\ud83d\\ude00\"", j) && j.str == "\xF0\x9F\x98\x80");
	CHECK(!rshelf::ParseJson("{\"a\": 1,}", j));
	CHECK(!rshelf::ParseJson("[1] x", j));
	CHECK(!rshelf::ParseJson(std::string(40, '[') + std::string(40, ']'), j)); // deeper than 32

	const std::string info = R"({"id": "545407f2", "title": {"full": "GTA IV", "reduced": "GTA IV"},
	  "media": [
	    {"media_id": "198350D4", "title": "Grand Theft Auto: Episodes from Liberty City", "region": "Japan"},
	    {"media_id": "06759F9C", "title": "Grand Theft Auto: Episodes from Liberty City", "region": "World"},
	    {"media_id": "7CF4679F", "title": "Grand Theft Auto IV", "region": "Australia"},
	    {"media_id": "4A53F9F6", "title": "Grand Theft Auto IV", "region": "Europe, Asia"},
	    {"media_id": "6AC07221", "title": "Grand Theft Auto IV", "region": "USA"}]})";
	rshelf::DbTitle t;
	CHECK(rshelf::ParseInfoJson(info, t) && t.id == "545407F2" && t.title == "GTA IV" && t.media.size() == 5);
	rshelf::DiscName d = rshelf::NameDisc(t, 0x4A53F9F6);
	CHECK(d.listed && d.title == "Grand Theft Auto IV" && d.region == "Europe, Asia" && d.title_art);
	d = rshelf::NameDisc(t, 0x06759F9C);
	CHECK(d.listed && d.title == "Grand Theft Auto: Episodes from Liberty City" && !d.title_art);
	d = rshelf::NameDisc(t, 0x12345678);
	CHECK(!d.listed && d.title == "GTA IV" && d.title_art && d.region.empty());
	// A tie: the product listed first is the title's own.
	rshelf::DbTitle tie;
	CHECK(rshelf::ParseInfoJson(R"({"id":"1","media":[{"media_id":"0000000A","title":"P"},{"media_id":"0000000B","title":"Q"},
	  {"media_id":"0000000C","title":"Q"},{"media_id":"0000000D","title":"P"}]})", tie));
	CHECK(rshelf::NameDisc(tie, 0xA).title_art && !rshelf::NameDisc(tie, 0xB).title_art);
	CHECK(rshelf::ExpandUrl(rshelf::kX360dbBoxartUrl, "545407F2").find("titles/545407F2/artwork/boxart.jpg") != std::string::npos);

	std::string id, name;
	CHECK(rshelf::ParseParamJson(R"({"titleId":"PPSA88360","localizedParameters":{"defaultLanguage":"fr-FR","fr-FR":{"titleName":"Jeu"},"en-US":{"titleName":"Game"}}})", id, name));
	CHECK(id == "PPSA88360" && name == "Jeu");
	CHECK(!rshelf::ParseParamJson(R"({"titleId":"CUSA00001"})", id, name));
	CHECK(rshelf::IsPs5TitleId("PPSA00001") && !rshelf::IsPs5TitleId("PPSA0001") && !rshelf::IsPs5TitleId("PPSA0000x"));
}

void TestCatalogAndInstall(const std::string& root)
{
	rshelf::CatalogPaths cp;
	cp.homebrew_dir = root + "/homebrew";
	cp.package_dirs = {root + "/rcomp/packages"};
	cp.db_cache_dir = root + "/rcomp/cache/x360db";
	cp.self_title_id = "PPSA88300";
	rshelf::MakeDirs(cp.homebrew_dir);
	rshelf::MakeDirs(cp.package_dirs[0]);
	rshelf::MakeDirs(cp.db_cache_dir);
	MakeTitle(cp.homebrew_dir + "/PPSA88360", "PPSA88360", 0x545407F2, 0x4A53F9F6, "GTA IV");
	MakeTitle(cp.package_dirs[0] + "/eflc", "PPSA88361", 0x545407F2, 0x06759F9C, "GTA IV");
	MakeTitle(cp.package_dirs[0] + "/gta-same", "PPSA88360", 0x545407F2, 0x4A53F9F6, "GTA IV"); // the installed build again
	MakeTitle(cp.homebrew_dir + "/PPSA88300", "PPSA88300", 1, 1, "the shelf itself");
	MakeTitle(cp.homebrew_dir + "/PPSA77777", "PPSA77778", 2, 2, "misnamed");
	rshelf::MakeDirs(cp.homebrew_dir + "/PPSA99999/sce_sys"); // another homebrew app: not ours
	WriteText(cp.homebrew_dir + "/PPSA99999/eboot.bin", "other");
	std::vector<std::string> lines;
	auto log = [&](const std::string& l) { lines.push_back(l); };

	std::vector<fe::GameInfo> games = rshelf::ScanCatalog(cp, log);
	CHECK(games.size() == 2);
	auto find = [&](const std::string& id) -> const fe::GameInfo* {
		for (const fe::GameInfo& g : games)
			if (g.stem == id)
				return &g;
		return nullptr;
	};
	const fe::GameInfo* gta = find("PPSA88360");
	const fe::GameInfo* eflc = find("PPSA88361");
	CHECK(gta && gta->state == fe::TitleState::Installed && gta->package_path.empty());
	CHECK(eflc && eflc->state == fe::TitleState::NotInstalled && eflc->title == "GTA IV" && eflc->serial == "545407F2");
	// Without x360db's entry two discs of one title ID don't take its box art.
	CHECK(gta && gta->cover_key.empty() && eflc && eflc->cover_key.empty());
	CHECK(eflc && !eflc->cover_names.empty() && eflc->cover_names[0] == "PPSA88361");

	// With the entry: names per disc, and the title's box art for GTA IV's disc only.
	WriteText(cp.db_cache_dir + "/545407F2.info.json", R"({"id":"545407F2","title":{"full":"GTA IV"},"media":[
	  {"media_id":"06759F9C","title":"Grand Theft Auto: Episodes from Liberty City","region":"World"},
	  {"media_id":"4A53F9F6","title":"Grand Theft Auto IV","region":"Europe, Asia"},
	  {"media_id":"6AC07221","title":"Grand Theft Auto IV","region":"USA"}]})");
	games = rshelf::ScanCatalog(cp, log);
	gta = find("PPSA88360");
	eflc = find("PPSA88361");
	CHECK(gta && gta->title == "Grand Theft Auto IV" && gta->cover_key == "545407F2" && gta->region == "Europe, Asia");
	CHECK(eflc && eflc->title == "Grand Theft Auto: Episodes from Liberty City" && eflc->cover_key.empty());
	CHECK(rshelf::MissingDbEntries(cp.db_cache_dir, games).empty());
	CHECK(games.size() == 2 && games[0].stem == "PPSA88360"); // sorted by title ("...Auto IV" before "...Auto: Episodes")

	// Installing the EFLC package (same file system: the package folder moves).
	rshelf::Installer inst;
	rshelf::InstallJob job{eflc->package_path, "PPSA88361", cp.homebrew_dir, root + "/rcomp/trash"};
	CHECK(inst.Run(job) == rshelf::Installer::State::Done);
	CHECK(inst.moved() && Exists(cp.homebrew_dir + "/PPSA88361/image/plain.xex") && !Exists(cp.package_dirs[0] + "/eflc"));
	CHECK(inst.done_bytes() == inst.total_bytes() && inst.total_bytes() > 300000);

	// An update of GTA IV, copied (the package stays); the old copy goes to the trash, untouched.
	MakeTitle(cp.package_dirs[0] + "/gta-v2", "PPSA88360", 0x545407F2, 0x4A53F9F6, "GTA IV", "eboot v2, longer");
	rshelf::RemoveTree(cp.package_dirs[0] + "/gta-same");
	games = rshelf::ScanCatalog(cp, log);
	gta = find("PPSA88360");
	CHECK(gta && gta->state == fe::TitleState::UpdateAvailable && gta->package_path == cp.package_dirs[0] + "/gta-v2");
	WriteText(cp.homebrew_dir + "/PPSA88360/rcomp_title.log", "a log of the old build\n");
	rshelf::Installer up;
	rshelf::InstallJob job2{gta->package_path, "PPSA88360", cp.homebrew_dir, root + "/rcomp/trash", true};
	CHECK(up.Run(job2) == rshelf::Installer::State::Done);
	std::string eboot;
	CHECK(!up.moved() && rshelf::ReadWhole(cp.homebrew_dir + "/PPSA88360/eboot.bin", eboot) && eboot == "eboot v2, longer");
	CHECK(Exists(cp.package_dirs[0] + "/gta-v2/eboot.bin"));
	CHECK(!up.trashed_path().empty() && Exists(up.trashed_path() + "/rcomp_title.log"));
	CHECK(!Exists(cp.homebrew_dir + "/.rcomp-install-PPSA88360"));
	games = rshelf::ScanCatalog(cp, log);
	CHECK(find("PPSA88360") && find("PPSA88360")->state == fe::TitleState::Installed); // same build as the package now

	// Never over another application's folder.
	MakeTitle(cp.package_dirs[0] + "/intruder", "PPSA99999", 3, 3, "intruder");
	rshelf::Installer no;
	CHECK(no.Run({cp.package_dirs[0] + "/intruder", "PPSA99999", cp.homebrew_dir, root + "/rcomp/trash"}) ==
		  rshelf::Installer::State::Failed);
	CHECK(Exists(cp.homebrew_dir + "/PPSA99999/eboot.bin") && !Exists(cp.homebrew_dir + "/PPSA99999/image"));
	// Nor a package whose title id isn't the one asked for.
	CHECK(no.Run({cp.package_dirs[0] + "/intruder", "PPSA12345", cp.homebrew_dir, root + "/rcomp/trash"}) ==
		  rshelf::Installer::State::Failed);

	// A cancelled copy leaves nothing behind.
	MakeTitle(cp.package_dirs[0] + "/new", "PPSA88364", 4, 4, "new");
	rshelf::Installer c;
	c.Cancel();
	CHECK(c.Run({cp.package_dirs[0] + "/new", "PPSA88364", cp.homebrew_dir, root + "/rcomp/trash", true}) ==
		  rshelf::Installer::State::Cancelled);
	CHECK(!Exists(cp.homebrew_dir + "/PPSA88364") && !Exists(cp.homebrew_dir + "/.rcomp-install-PPSA88364"));

	// The threaded path.
	rshelf::Installer t;
	CHECK(t.Start({cp.package_dirs[0] + "/new", "PPSA88364", cp.homebrew_dir, root + "/rcomp/trash", true}));
	CHECK(!t.Start({cp.package_dirs[0] + "/new", "PPSA88364", cp.homebrew_dir, root + "/rcomp/trash", true}));
	while (t.state() == rshelf::Installer::State::Running)
		usleep(1000);
	CHECK(t.state() == rshelf::Installer::State::Done && Exists(cp.homebrew_dir + "/PPSA88364/eboot.bin"));
	t.Finish();
	CHECK(t.state() == rshelf::Installer::State::Idle);
}
} // namespace

int main()
{
	TestXex();
	TestJsonAndDb();
	char tmpl[] = "build/host/test-XXXXXX";
	rshelf::MakeDirs("build/host");
	const char* root = mkdtemp(tmpl);
	if (!root)
	{
		std::fprintf(stderr, "FAIL cannot make a temporary folder under build/host\n");
		return 2;
	}
	TestCatalogAndInstall(root);
	if (g_failed == 0)
		rshelf::RemoveTree(root);
	std::printf("%s: %d checks, %d failed%s\n", g_failed ? "FAIL" : "PASS", g_checks, g_failed, g_failed ? (std::string(" (kept ") + root + ")").c_str() : "");
	return g_failed ? 1 : 0;
}
