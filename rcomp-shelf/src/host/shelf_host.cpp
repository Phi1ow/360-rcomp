// R-comp shelf on a PC, for looking at it without a console (from PS5SX2's fe_host.cpp). It runs the real catalog
// (src/catalog) over a stand-in for /data, the real installer, fe::App and fe::Renderer on the machine's Vulkan
// (vulkan-1.dll / libvulkan.so.1), drives them with a script of controller presses, and writes the frames it is
// told to as PNG files. Nothing of this goes into the eboot.
//
//   shelf_host --data <folder> --out <folder> [--size 1920x1080] [--lang <ps5 language id>] [--online]
//              [--script <file>] [step ...]
//
// <folder> for --data stands for /data: homebrew/ (installed titles), rcomp/packages/ (packages), rcomp/covers/,
// rcomp/cache/. --online fetches x360db entries and covers with the curl command (else nothing is downloaded).
// Steps (from --script, one a line, and/or the command line, in that order):
//   wait <s>               the shelf runs <s> seconds with no button down
//   press <button> [n]     the button goes down for a frame and up for a frame, n times (default 1)
//   hold <button> <s>      the button stays down for <s> seconds
//   shot <name>            <out>/<name>.png of the current frame
//   game <PPSA id>         (before any other step) the shelf starts on that title
//   untilidle <s>          runs until no install is going on (at most <s> seconds)
//   sleep <s>              runs <s> seconds of real time (downloads and installs go on meanwhile)
// Buttons: left right up down cross circle square triangle options l1 r1 l2 r2.
//
// Copyright (C) 2026 Spyros
// Modified for R-comp, 2026: the R-comp catalog, installer and covers; PNG output without zlib.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../frontend/fe_app.h"
#include "../frontend/fe_i18n.h"
#include "../catalog/fsutil.h"
#include "../catalog/install.h"
#include "../catalog/titles.h"
#include "../catalog/x360db.h"
#include "../shelf/session.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace fe;

namespace
{
bool ReadAll(const std::string& path, std::vector<uint8_t>& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

// ---- A PNG writer: zlib's format with stored (uncompressed) deflate blocks, so no zlib is needed ----
uint32_t Crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu)
{
	static uint32_t table[256];
	static bool made = false;
	if (!made)
	{
		for (uint32_t i = 0; i < 256; i++)
		{
			uint32_t v = i;
			for (int k = 0; k < 8; k++)
				v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
			table[i] = v;
		}
		made = true;
	}
	for (size_t i = 0; i < n; i++)
		c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return c;
}

void PutBe32(std::vector<uint8_t>& v, uint32_t x)
{
	v.push_back(static_cast<uint8_t>(x >> 24));
	v.push_back(static_cast<uint8_t>(x >> 16));
	v.push_back(static_cast<uint8_t>(x >> 8));
	v.push_back(static_cast<uint8_t>(x));
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data)
{
	PutBe32(png, static_cast<uint32_t>(data.size()));
	const size_t start = png.size();
	png.insert(png.end(), type, type + 4);
	png.insert(png.end(), data.begin(), data.end());
	PutBe32(png, Crc32(png.data() + start, png.size() - start) ^ 0xFFFFFFFFu);
}

bool WritePng(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h)
{
	std::vector<uint8_t> raw;
	raw.reserve(static_cast<size_t>(w * 3 + 1) * h);
	for (uint32_t y = 0; y < h; y++)
	{
		raw.push_back(0);
		const uint8_t* row = rgba.data() + static_cast<size_t>(y) * w * 4;
		for (uint32_t x = 0; x < w; x++)
			raw.insert(raw.end(), row + x * 4, row + x * 4 + 3); // no alpha: the display ignores it
	}
	std::vector<uint8_t> z = {0x78, 0x01};
	uint32_t a = 1, b = 0;
	for (uint8_t c : raw)
	{
		a = (a + c) % 65521;
		b = (b + a) % 65521;
	}
	for (size_t off = 0; off < raw.size() || off == 0;)
	{
		const size_t n = std::min<size_t>(65535, raw.size() - off);
		const bool last = off + n >= raw.size();
		z.push_back(last ? 1 : 0);
		z.push_back(static_cast<uint8_t>(n));
		z.push_back(static_cast<uint8_t>(n >> 8));
		z.push_back(static_cast<uint8_t>(~n));
		z.push_back(static_cast<uint8_t>(~n >> 8));
		z.insert(z.end(), raw.begin() + static_cast<long>(off), raw.begin() + static_cast<long>(off + n));
		off += n;
		if (last)
			break;
	}
	PutBe32(z, (b << 16) | a);
	std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	std::vector<uint8_t> ihdr;
	PutBe32(ihdr, w);
	PutBe32(ihdr, h);
	ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
	Chunk(png, "IHDR", ihdr);
	Chunk(png, "IDAT", z);
	Chunk(png, "IEND", {});
	std::ofstream f(path, std::ios::binary);
	f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
	return static_cast<bool>(f);
}

// ---- Downloads with the curl command (--online) ----
int CurlGet(const std::string& url, std::vector<uint8_t>& out, const std::string& tmp)
{
	for (char c : url)
		if (c == '"' || c == '`' || c == '$' || c == '\\')
			return -1;
	const std::string cmd = "curl -s -L --max-time 20 -o \"" + tmp + "\" -w \"%{http_code}\" \"" + url + "\"";
	FILE* p = popen(cmd.c_str(), "r");
	if (!p)
		return -1;
	char buf[32] = {};
	const size_t n = std::fread(buf, 1, sizeof(buf) - 1, p);
	const int rc = pclose(p);
	buf[n] = 0;
	const int status = std::atoi(buf);
	if (rc != 0 && status == 0)
		return -1;
	out.clear();
	if (status == 200)
		ReadAll(tmp, out);
	std::remove(tmp.c_str());
	return status;
}

struct Gpu
{
	void* lib = nullptr;
	Vk vk;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t qf = 0;
	VkQueue queue = VK_NULL_HANDLE;
	std::vector<VkImage> images;
	std::vector<VkDeviceMemory> memory;

	bool Init(uint32_t w, uint32_t h)
	{
		for (const char* name : {"vulkan-1.dll", "libvulkan.so.1"})
			if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL)) != nullptr)
				break;
		if (!lib)
			return Fail("no vulkan-1.dll or libvulkan.so.1");
		auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
		const char* missing = nullptr;
		if (!gipa || !vk.LoadGlobal(gipa, &missing))
			return Fail(missing ? missing : "vkGetInstanceProcAddr");
		VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
		ai.pApplicationName = "shelf_host";
		ai.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ici.pApplicationInfo = &ai;
		if (vk.vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS || !vk.LoadInstance(instance, false, &missing))
			return Fail("instance");
		uint32_t n = 1;
		if (vk.vkEnumeratePhysicalDevices(instance, &n, &pd) < 0 || n == 0)
			return Fail("no physical device");
		VkPhysicalDeviceProperties props;
		vk.vkGetPhysicalDeviceProperties(pd, &props);
		std::printf("[host] device: %s\n", props.deviceName);
		uint32_t nq = 0;
		vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
		std::vector<VkQueueFamilyProperties> qs(nq);
		vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs.data());
		for (qf = 0; qf < nq && !(qs[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); qf++)
			;
		if (qf == nq)
			return Fail("no graphics queue");
		const float prio = 1.0f;
		VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qci.queueFamilyIndex = qf;
		qci.queueCount = 1;
		qci.pQueuePriorities = &prio;
		VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		if (vk.vkCreateDevice(pd, &dci, nullptr, &device) != VK_SUCCESS || !vk.LoadDevice(device, false, &missing))
			return Fail("device");
		vk.vkGetDeviceQueue(device, qf, 0, &queue);

		// Two display images, as the console's swapchain has.
		VkPhysicalDeviceMemoryProperties mp;
		vk.vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		for (int i = 0; i < 2; i++)
		{
			VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			ci.imageType = VK_IMAGE_TYPE_2D;
			ci.format = VK_FORMAT_B8G8R8A8_UNORM;
			ci.extent = {w, h, 1};
			ci.mipLevels = ci.arrayLayers = 1;
			ci.samples = VK_SAMPLE_COUNT_1_BIT;
			ci.tiling = VK_IMAGE_TILING_OPTIMAL;
			ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			VkImage img;
			if (vk.vkCreateImage(device, &ci, nullptr, &img) != VK_SUCCESS)
				return Fail("image");
			VkMemoryRequirements req;
			vk.vkGetImageMemoryRequirements(device, img, &req);
			uint32_t type = 0;
			while (type < mp.memoryTypeCount &&
				   !((req.memoryTypeBits & (1u << type)) && (mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
				type++;
			VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			mai.allocationSize = req.size;
			mai.memoryTypeIndex = type;
			VkDeviceMemory mem;
			if (type == mp.memoryTypeCount || vk.vkAllocateMemory(device, &mai, nullptr, &mem) != VK_SUCCESS)
				return Fail("image memory");
			vk.vkBindImageMemory(device, img, mem, 0);
			images.push_back(img);
			memory.push_back(mem);
		}
		return true;
	}

	bool Fail(const char* what)
	{
		std::fprintf(stderr, "[host] Vulkan: %s\n", what);
		return false;
	}
};

struct Step
{
	std::string op, arg;
	double seconds = 0;
	int count = 1;
};

bool SetButton(Input& in, const std::string& b, bool down)
{
	bool* p = b == "left" ? &in.left : b == "right" ? &in.right : b == "up" ? &in.up : b == "down" ? &in.down :
	          b == "cross" ? &in.cross : b == "circle" ? &in.circle : b == "square" ? &in.square :
	          b == "triangle" ? &in.triangle : b == "options" ? &in.options : b == "l1" ? &in.l1 : b == "r1" ? &in.r1 :
	          b == "l2" ? &in.l2 : b == "r2" ? &in.r2 : nullptr;
	if (!p)
		return false;
	*p = down;
	return true;
}

bool ParseStep(const std::string& line, std::vector<Step>& out)
{
	std::istringstream s(line);
	Step st;
	if (!(s >> st.op) || st.op[0] == '#')
		return true;
	if (st.op == "wait" || st.op == "untilidle" || st.op == "sleep")
		s >> st.seconds;
	else if (st.op == "press")
	{
		s >> st.arg;
		if (!(s >> st.count))
			st.count = 1;
	}
	else if (st.op == "hold")
		s >> st.arg >> st.seconds;
	else if (st.op == "shot" || st.op == "game")
		s >> st.arg;
	else
	{
		std::fprintf(stderr, "[host] unknown step: %s\n", line.c_str());
		return false;
	}
	out.push_back(st);
	return true;
}

} // namespace

int main(int argc, char** argv)
{
	std::string data = "shelf_host_data", out = "shelf_host_shots", script, root;
	uint32_t w = 1920, h = 1080;
	int lang = 1; // English
	bool online = false;
	std::vector<Step> steps;
	{
		const std::string self = argv[0];
		const size_t slash = self.find_last_of('/');
		root = (slash == std::string::npos ? std::string(".") : self.substr(0, slash)) + "/../..";
	}
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
		if (a == "--data")
			data = next();
		else if (a == "--out")
			out = next();
		else if (a == "--script")
			script = next();
		else if (a == "--root")
			root = next();
		else if (a == "--lang")
			lang = std::atoi(next().c_str());
		else if (a == "--online")
			online = true;
		else if (a == "--size")
		{
			const std::string s = next();
			if (std::sscanf(s.c_str(), "%ux%u", &w, &h) != 2)
				return 2;
		}
		else if (!ParseStep(a, steps))
			return 2;
	}
	if (!script.empty())
	{
		std::ifstream f(script);
		std::string line;
		std::vector<Step> from_file;
		while (std::getline(f, line))
			if (!ParseStep(line, from_file))
				return 2;
		steps.insert(steps.begin(), from_file.begin(), from_file.end());
	}
	rshelf::MakeDirs(out);

	std::vector<uint8_t> text_font, icon_font, brand_font;
	if (!ReadAll(root + "/assets/fonts/Roboto-Regular.ttf", text_font) || !ReadAll(root + "/assets/fonts/promptfont.otf", icon_font) ||
		!ReadAll(root + "/assets/fonts/fa-brands-400.otf", brand_font))
	{
		std::fprintf(stderr, "[host] fonts not found under %s/assets/fonts (--root is the repository)\n", root.c_str());
		return 1;
	}
	SetLanguage(lang, "");

	const std::string rcomp_dir = data + "/rcomp";
	rshelf::CatalogPaths cp;
	cp.homebrew_dir = data + "/homebrew";
	cp.package_dirs = {rcomp_dir + "/packages"};
	cp.db_cache_dir = rcomp_dir + "/cache/x360db";
	cp.self_title_id = "PPSA88300";
	rshelf::MakeDirs(cp.homebrew_dir);
	rshelf::MakeDirs(cp.package_dirs[0]);
	auto log = [](const std::string& line) { std::printf("[catalog] %s\n", line.c_str()); };
	const std::string tmp = data + "/.download.tmp";
	auto download = [&](const std::string& url, std::vector<uint8_t>& bytes) {
		const int s = online ? CurlGet(url, bytes, tmp) : -1;
		std::printf("[host] get %s -> %d (%zu bytes)\n", url.c_str(), s, bytes.size());
		return s;
	};

	if (online)
	{
		const std::vector<GameInfo> found = rshelf::ScanCatalog(cp, log);
		rshelf::FetchDbEntries(cp.db_cache_dir, rshelf::MissingDbEntries(cp.db_cache_dir, found), download, 20.0, log);
	}

	Gpu gpu;
	if (!gpu.Init(w, h))
		return 1;
	Fonts* fonts = new Fonts();
	Renderer renderer;
	if (!fonts->Init(text_font.data(), text_font.size(), icon_font.data(), icon_font.size(), brand_font.data(), brand_font.size()) ||
		!renderer.Init(&gpu.vk, gpu.pd, gpu.device, gpu.qf, gpu.queue, w, h, VK_FORMAT_B8G8R8A8_UNORM, gpu.images,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))
	{
		std::fprintf(stderr, "[host] renderer: %s\n", renderer.error().c_str());
		return 1;
	}

	rshelf::SessionConfig sc;
	sc.catalog = cp;
	sc.covers_dir = rcomp_dir + "/covers";
	sc.cover_cache_dir = rcomp_dir + "/cache/covers";
	sc.trash_dir = rcomp_dir + "/trash";
	sc.allow_download = online;
	sc.download = download;
	sc.build_tag = "shelf (host)";
	sc.log = log;
	std::string preselect;
	for (const Step& s : steps)
		if (s.op == "game")
			preselect = s.arg;
	rshelf::Session session;
	if (!session.Start(&renderer, fonts, sc, preselect, ""))
	{
		std::fprintf(stderr, "[host] shelf: %s\n", renderer.error().c_str());
		return 1;
	}

	const double dt = 1.0 / 60.0;
	Input in;
	FrameDesc frame;
	uint32_t index = 0;
	bool played = false;
	auto run = [&](double seconds) {
		for (double t = 0; t < seconds - 1e-9; t += dt)
		{
			session.Update(dt, in);
			session.Build(frame, "21:47");
			GameInfo g;
			if (session.PlayChosen(g) && !played)
			{
				played = true;
				std::printf("[host] play %s (%s): on the console the shelf closes and the title starts\n", g.stem.c_str(), g.title.c_str());
			}
		}
	};
	int failures = 0;
	for (const Step& s : steps)
	{
		if (s.op == "wait")
			run(s.seconds);
		else if (s.op == "sleep")
			for (double t = 0; t < s.seconds; t += 1.0 / 60.0)
			{
				run(dt);
				usleep(16667);
			}
		else if (s.op == "untilidle")
		{
			for (double t = 0; t < s.seconds && session.Installing(); t += 0.05)
			{
				run(dt);
				usleep(50000);
			}
			run(dt * 3);
		}
		else if (s.op == "press")
			for (int n = 0; n < s.count; n++)
			{
				if (!SetButton(in, s.arg, true))
				{
					std::fprintf(stderr, "[host] unknown button %s\n", s.arg.c_str());
					return 2;
				}
				run(dt);
				SetButton(in, s.arg, false);
				run(dt * 2);
			}
		else if (s.op == "hold")
		{
			SetButton(in, s.arg, true);
			run(s.seconds);
			SetButton(in, s.arg, false);
			run(dt);
		}
		else if (s.op == "shot")
		{
			session.Build(frame, "21:47");
			if (!renderer.Render(frame, index, VK_NULL_HANDLE, VK_NULL_HANDLE))
			{
				std::fprintf(stderr, "[host] render: %s\n", renderer.error().c_str());
				return 1;
			}
			// A second frame: textures made during the first (covers, the atlas) are uploaded by then.
			session.Build(frame, "21:47");
			renderer.Render(frame, index ^ 1, VK_NULL_HANDLE, VK_NULL_HANDLE);
			std::vector<uint8_t> rgba;
			const std::string path = out + "/" + s.arg + ".png";
			if (!renderer.ReadPixels(index ^ 1, rgba) || !WritePng(path, rgba, w, h))
			{
				std::fprintf(stderr, "[host] could not write %s\n", path.c_str());
				failures++;
			}
			else
				std::printf("[host] %s\n", path.c_str());
		}
	}
	session.Stop(-1);
	renderer.Shutdown();
	return failures ? 1 : 0;
}
