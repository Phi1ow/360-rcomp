// R-comp shelf on the PS5 (from PS5SX2's fe_ps5.cpp): its own application (title id PPSA88300). The shelf runs
// on its own Vulkan device (R-comp's pinned RADV, linked into the eboot) with a VK_KHR_display swapchain, reads
// the DualSense, plays its key sounds on an audio port of its own and downloads x360db entries and covers over
// HTTPS with the console's own libSceHttp2. It lists the recompiled Xbox 360 titles installed in /data/homebrew
// and the packages in /data/rcomp/packages and <USB drive>/rcomp, installs packages there, and starts a title:
// everything is torn down, then sceLncUtilLaunchApp starts the title's own application.
//
// Status: built for the console; launching another application from this one is NOT TESTED (see README.md).
//
// Copyright (C) 2026 Spyros
// Modified for R-comp, 2026: the R-comp catalog, installer and launch; no settings page, no PCSX2.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_app.h"
#include "fe_i18n.h"
#include "fe_renderer.h"
#include "fe_sound.h"
#include "fe_text.h"
#include "fe_vk.h"
#include "fsutil.h"
#include "session.h"
#include "titles.h"
#include "x360db.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

#ifndef RSHELF_TITLE_ID
#define RSHELF_TITLE_ID "PPSA88300"
#endif
#ifndef RSHELF_BUILD_TAG
#define RSHELF_BUILD_TAG "dev"
#endif

// What sceKernelConvertUtcToLocaltime fills in: 16 bytes (PS5SX2 vk-285-40 found that an 8-byte struct timezone
// let the call write over the next stack slot).
struct KernelTimesec
{
	int64_t t;
	uint32_t west_sec;
	uint32_t dst_sec;
};

// sceLncUtilLaunchApp's parameter block, as the PS5 homebrew launchers fill it.
struct LncAppParam
{
	uint32_t size;
	int32_t user_id;
	uint32_t app_opt;
	uint64_t crash_report;
	uint64_t check_flag;
};

extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);
int scePadInit(void);
int scePadOpen(int32_t userId, int32_t type, int32_t index, const void* param);
int scePadClose(int32_t handle);
int scePadReadState(int32_t handle, void* data);
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int32_t* userId);
int sceKernelConvertUtcToLocaltime(time_t utc, time_t* local, KernelTimesec* sec, uint64_t* dst_sec);
int sceSystemServiceParamGetInt(int param, int* value);
int sceSystemServiceHideSplashScreen(void);
int sceLncUtilLaunchApp(const char* title_id, const char** argv, LncAppParam* param);
int sceLncUtilGetAppIdOfRunningBigApp(void);

// libSceNet, libSceSsl and libSceHttp2 as the payload SDK's http2_get sample declares them.
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceSslInit(size_t pool_size);
int sceSslTerm(int ctx);
int sceHttp2Init(int net_pool, int ssl_ctx, size_t pool_size, int max_requests);
int sceHttp2Term(int ctx);
int sceHttp2CreateTemplate(int ctx, const char* user_agent, int http_version, int auto_proxy);
int sceHttp2DeleteTemplate(int tmpl);
int sceHttp2CreateRequestWithURL(int tmpl, const char* method, const char* url, uint64_t content_length);
int sceHttp2DeleteRequest(int req);
int sceHttp2SendRequest(int req, const void* data, size_t size);
int sceHttp2GetStatusCode(int req, int* status);
int sceHttp2ReadData(int req, void* data, size_t size);
int sceHttp2SetResolveTimeOut(int id, uint32_t usec);
int sceHttp2SetConnectTimeOut(int id, uint32_t usec);
int sceHttp2SetSendTimeOut(int id, uint32_t usec);
int sceHttp2SetRecvTimeOut(int id, uint32_t usec);
int sceHttp2AbortRequest(int req);
int sceHttp2SetTimeOut(int id, uint32_t usec);
int sceHttp2SetAutoRedirect(int id, int enable);
int sceNetCtlInit(void);
void sceNetCtlTerm(void);
int sceNetCtlGetState(int* state);

int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutOutput(int handle, const void* p);
int sceAudioOutClose(int handle);
}

// The fonts, embedded (assets/fonts): Roboto (Apache-2.0), PromptFont (OFL-1.1), Font Awesome Free brands (OFL-1.1).
#ifndef RSHELF_FONT_DIR
#error "RSHELF_FONT_DIR must name assets/fonts"
#endif
#define RSHELF_INCBIN(sym, file) \
	__asm__(".section .rodata." #sym ",\"a\",@progbits\n" \
			".balign 16\n" \
			".global " #sym "\n" #sym ":\n" \
			".incbin \"" RSHELF_FONT_DIR "/" file "\"\n" \
			".global " #sym "_end\n" #sym "_end:\n" \
			".byte 0\n" \
			".previous\n")
RSHELF_INCBIN(rshelf_font_text, "Roboto-Regular.ttf");
RSHELF_INCBIN(rshelf_font_icons, "promptfont.otf");
RSHELF_INCBIN(rshelf_font_brands, "fa-brands-400.otf");
extern "C" const uint8_t rshelf_font_text[], rshelf_font_text_end[], rshelf_font_icons[], rshelf_font_icons_end[], rshelf_font_brands[],
	rshelf_font_brands_end[];

namespace
{
using namespace fe;

constexpr const char* kRcompDir = "/data/rcomp";

double Now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
}

void Log(const std::string& line)
{
	std::printf("%s\n", line.c_str());
	std::fflush(stdout);
}

// The key sounds' output: a port on the main output, float stereo at 48 kHz in 256-frame grains (PS5SX2's).
class AudioOut
{
public:
	bool Start(Mixer* mixer)
	{
		const int init = sceAudioOutInit();
		m_handle = sceAudioOutOpen(255, 0, 0, kGrain, SoundBank::kRate, 4);
		std::printf("[shelf] sound: sceAudioOutInit %x, port %x\n", static_cast<unsigned>(init), static_cast<unsigned>(m_handle));
		if (m_handle < 0)
			return false;
		m_mixer = mixer;
		m_thread = std::thread([this]() { Run(); });
		return true;
	}

	void Stop()
	{
		m_quit.store(true);
		if (m_thread.joinable())
			m_thread.join();
		if (m_handle >= 0)
			sceAudioOutClose(m_handle);
		m_handle = -1;
	}

private:
	static constexpr unsigned kGrain = 256;

	void Run()
	{
		alignas(64) float buf[kGrain * 2];
		while (!m_quit.load(std::memory_order_relaxed))
		{
			m_mixer->Mix(buf, static_cast<int>(kGrain));
			sceAudioOutOutput(m_handle, buf);
		}
	}

	Mixer* m_mixer = nullptr;
	int m_handle = -1;
	std::atomic<bool> m_quit{false};
	std::thread m_thread;
};

// libScePad's state (PS5SX2's reading of it).
struct PadData
{
	uint32_t buttons;
	uint8_t lx, ly, rx, ry, l2, r2, pad0, pad1;
	uint8_t rest[256];
};
constexpr uint32_t kPadRight = 0x20, kPadLeft = 0x80, kPadCross = 0x4000, kPadOptions = 0x8, kPadL1 = 0x400, kPadR1 = 0x800;
constexpr uint32_t kPadUp = 0x10, kPadDown = 0x40, kPadTriangle = 0x1000, kPadCircle = 0x2000, kPadSquare = 0x8000;
constexpr uint32_t kPadL2 = 0x100, kPadR2 = 0x200;

// The time of day in the console's time zone and clock format.
std::string Clock()
{
	const time_t utc = time(nullptr);
	time_t local = utc;
	KernelTimesec sec[2] = {};
	uint64_t dst[2] = {};
	if (sceKernelConvertUtcToLocaltime(utc, &local, &sec[0], &dst[0]) != 0)
		local = utc;
	struct tm tm = {};
	gmtime_r(&local, &tm);
	static int s_format = -1;
	if (s_format < 0)
	{
		int v = 1;
		s_format = sceSystemServiceParamGetInt(3 /* time format */, &v) == 0 ? v : 1;
	}
	char buf[16];
	if (s_format == 0)
		std::snprintf(buf, sizeof(buf), "%d:%02d %s", (tm.tm_hour + 11) % 12 + 1, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
	else
		std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
	return buf;
}

// ---- HTTPS through libSceHttp2, on the downloading thread only (PS5SX2's, unchanged in substance) ----
struct Http
{
	bool tried = false, ok = false, netctl = false;
	int pool = -1, ssl = -1, ctx = -1, tmpl = -1;
	std::atomic<int> active{-1};
	std::atomic<bool> stopping{false};
	std::mutex abort_mutex;

	void Release(int req)
	{
		{
			std::lock_guard<std::mutex> lock(abort_mutex);
			active = -1;
		}
		sceHttp2DeleteRequest(req);
	}

	bool Init()
	{
		if (tried)
			return ok;
		tried = true;
		// An offline console must not wait on the resolver: state 3 is "IP address obtained".
		const int nc = sceNetCtlInit();
		netctl = nc == 0;
		int state[4] = {-1, 0, 0, 0};
		const int gs = sceNetCtlGetState(state);
		std::printf("[shelf] netctl: init %#x, state %d (%#x)\n", static_cast<unsigned>(nc), state[0], static_cast<unsigned>(gs));
		if (gs == 0 && state[0] >= 0 && state[0] < 3)
		{
			Log("[shelf] the console is not connected; no downloads this time");
			return false;
		}
		const int net = sceNetInit();
		pool = sceNetPoolCreate("rcomp-shelf", 64 * 1024, 0);
		ssl = pool >= 0 ? sceSslInit(256 * 1024) : -1;
		ctx = ssl >= 0 ? sceHttp2Init(pool, ssl, 256 * 1024, 1) : -1;
		tmpl = ctx >= 0 ? sceHttp2CreateTemplate(ctx, "R-comp-shelf/1.0", 3, 1) : -1;
		std::printf("[shelf] https: net %#x pool %#x ssl %#x http2 %#x template %#x\n", static_cast<unsigned>(net),
			static_cast<unsigned>(pool), static_cast<unsigned>(ssl), static_cast<unsigned>(ctx), static_cast<unsigned>(tmpl));
		std::fflush(stdout);
		if (tmpl < 0)
			return false;
		ok = true;
		return true;
	}

	void Abort()
	{
		stopping.store(true);
		std::lock_guard<std::mutex> lock(abort_mutex);
		const int req = active.load();
		if (req >= 0)
			std::printf("[shelf] aborted request %#x: %#x\n", static_cast<unsigned>(req), static_cast<unsigned>(sceHttp2AbortRequest(req)));
	}

	void Resume()
	{
		stopping.store(false);
	}

	void Term()
	{
		if (tmpl >= 0)
			sceHttp2DeleteTemplate(tmpl);
		if (ctx >= 0)
			sceHttp2Term(ctx);
		if (ssl >= 0)
			sceSslTerm(ssl);
		if (pool >= 0)
			sceNetPoolDestroy(pool);
		if (netctl)
			sceNetCtlTerm();
		tmpl = ctx = ssl = pool = -1;
		tried = ok = netctl = false;
		stopping = false;
	}

	int Get(const std::string& url, std::vector<uint8_t>& out)
	{
		if (stopping || !Init())
			return -1;
		const double t0 = Now();
		const int req = sceHttp2CreateRequestWithURL(tmpl, "GET", url.c_str(), 0);
		if (req < 0)
		{
			std::printf("[shelf] get %s: request %#x\n", url.c_str(), static_cast<unsigned>(req));
			return -1;
		}
		sceHttp2SetResolveTimeOut(req, 10 * 1000 * 1000);
		sceHttp2SetConnectTimeOut(req, 10 * 1000 * 1000);
		sceHttp2SetSendTimeOut(req, 10 * 1000 * 1000);
		sceHttp2SetRecvTimeOut(req, 10 * 1000 * 1000);
		sceHttp2SetTimeOut(req, 20 * 1000 * 1000);
		sceHttp2SetAutoRedirect(req, 1);
		active = req;
		if (stopping)
		{
			Release(req);
			return -1;
		}
		int status = -1;
		const int sent = sceHttp2SendRequest(req, nullptr, 0);
		const int got = sent == 0 ? sceHttp2GetStatusCode(req, &status) : -1;
		if (sent != 0 || got != 0)
			status = -1;
		else if (status == 200)
		{
			std::vector<uint8_t> buf(64 * 1024);
			for (;;)
			{
				const int n = sceHttp2ReadData(req, buf.data(), buf.size());
				if (n < 0)
				{
					status = -1;
					break;
				}
				if (n == 0)
					break;
				out.insert(out.end(), buf.begin(), buf.begin() + n);
				if (out.size() > (16u << 20))
				{
					status = -1;
					break;
				}
			}
		}
		std::printf("[shelf] get %s: %d, %zu bytes in %.0f ms\n", url.c_str(), status, out.size(), (Now() - t0) * 1000.0);
		std::fflush(stdout);
		Release(req);
		return status;
	}
};
Http g_http;

// ---- The display (PS5SX2's: the first display, its largest mode nearest 60 Hz, a plane that can drive it) ----
struct Display
{
	Vk vk;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t qf = 0;
	VkQueue queue = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	std::vector<VkImage> images;
	VkExtent2D extent = {};
	VkSemaphore acquired[2] = {}, rendered[2] = {};

	bool Fail(const char* what, VkResult r)
	{
		std::printf("[shelf] %s failed: %d\n", what, static_cast<int>(r));
		std::fflush(stdout);
		return false;
	}

	bool Init()
	{
		const char* missing = nullptr;
		if (!vk.LoadGlobal(vk_icdGetInstanceProcAddr, &missing))
		{
			std::printf("[shelf] no %s\n", missing);
			return false;
		}
		const char* inst_ext[2] = {"VK_KHR_surface", "VK_KHR_display"};
		VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
		ai.pApplicationName = "R-comp shelf";
		ai.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ici.pApplicationInfo = &ai;
		ici.enabledExtensionCount = 2;
		ici.ppEnabledExtensionNames = inst_ext;
		VkResult r = vk.vkCreateInstance(&ici, nullptr, &instance);
		if (r != VK_SUCCESS)
			return Fail("vkCreateInstance", r);
		if (!vk.LoadInstance(instance, true, &missing))
		{
			std::printf("[shelf] no %s\n", missing);
			return false;
		}
		uint32_t count = 1;
		r = vk.vkEnumeratePhysicalDevices(instance, &count, &pd);
		if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0)
			return Fail("vkEnumeratePhysicalDevices", r);
		uint32_t qcount = 0;
		vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
		std::vector<VkQueueFamilyProperties> qfs(qcount);
		vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qfs.data());
		qf = UINT32_MAX;
		for (uint32_t i = 0; i < qcount; i++)
			if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				qf = i;
				break;
			}
		if (qf == UINT32_MAX)
			return Fail("a graphics queue", VK_ERROR_UNKNOWN);
		const float prio = 1.0f;
		VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qi.queueFamilyIndex = qf;
		qi.queueCount = 1;
		qi.pQueuePriorities = &prio;
		const char* dev_ext[1] = {"VK_KHR_swapchain"};
		VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qi;
		dci.enabledExtensionCount = 1;
		dci.ppEnabledExtensionNames = dev_ext;
		r = vk.vkCreateDevice(pd, &dci, nullptr, &device);
		if (r != VK_SUCCESS)
			return Fail("vkCreateDevice", r);
		if (!vk.LoadDevice(device, true, &missing))
		{
			std::printf("[shelf] no %s\n", missing);
			return false;
		}
		vk.vkGetDeviceQueue(device, qf, 0, &queue);
		return CreateSurface() && CreateSwapchain();
	}

	bool CreateSurface()
	{
		uint32_t display_count = 0;
		if (vk.vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &display_count, nullptr) != VK_SUCCESS || !display_count)
			return Fail("vkGetPhysicalDeviceDisplayPropertiesKHR", VK_ERROR_UNKNOWN);
		std::vector<VkDisplayPropertiesKHR> displays(display_count);
		vk.vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &display_count, displays.data());
		uint32_t plane_count = 0;
		if (vk.vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &plane_count, nullptr) != VK_SUCCESS || !plane_count)
			return Fail("vkGetPhysicalDeviceDisplayPlanePropertiesKHR", VK_ERROR_UNKNOWN);
		std::vector<VkDisplayPlanePropertiesKHR> planes(plane_count);
		vk.vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &plane_count, planes.data());
		VkDisplayKHR display = VK_NULL_HANDLE;
		VkDisplayModePropertiesKHR mode = {};
		for (const VkDisplayPropertiesKHR& d : displays)
		{
			uint32_t mode_count = 0;
			if (vk.vkGetDisplayModePropertiesKHR(pd, d.display, &mode_count, nullptr) != VK_SUCCESS || !mode_count)
				continue;
			std::vector<VkDisplayModePropertiesKHR> modes(mode_count);
			vk.vkGetDisplayModePropertiesKHR(pd, d.display, &mode_count, modes.data());
			auto off_60hz = [](const VkDisplayModePropertiesKHR& x) {
				const int64_t mhz = static_cast<int64_t>(x.parameters.refreshRate);
				return mhz > 60000 ? mhz - 60000 : 60000 - mhz;
			};
			for (const VkDisplayModePropertiesKHR& m : modes)
			{
				const uint64_t area = static_cast<uint64_t>(m.parameters.visibleRegion.width) * m.parameters.visibleRegion.height;
				const uint64_t best = static_cast<uint64_t>(mode.parameters.visibleRegion.width) * mode.parameters.visibleRegion.height;
				if (!display || area > best || (area == best && off_60hz(m) < off_60hz(mode)))
				{
					display = d.display;
					mode = m;
				}
			}
			if (display)
				break;
		}
		if (!display)
			return Fail("a display mode", VK_ERROR_UNKNOWN);
		uint32_t plane = UINT32_MAX;
		for (uint32_t i = 0; i < plane_count; i++)
			if (planes[i].currentDisplay == VK_NULL_HANDLE || planes[i].currentDisplay == display)
			{
				plane = i;
				break;
			}
		if (plane == UINT32_MAX)
			return Fail("a display plane", VK_ERROR_UNKNOWN);
		const VkDisplaySurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR, nullptr, 0, mode.displayMode, plane,
			planes[plane].currentStackIndex, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, 1.0f, VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
			mode.parameters.visibleRegion};
		const VkResult r = vk.vkCreateDisplayPlaneSurfaceKHR(instance, &ci, nullptr, &surface);
		if (r != VK_SUCCESS)
			return Fail("vkCreateDisplayPlaneSurfaceKHR", r);
		extent = mode.parameters.visibleRegion;
		std::printf("[shelf] display %ux%u at %.2f Hz, plane %u\n", extent.width, extent.height, mode.parameters.refreshRate / 1000.0, plane);
		return true;
	}

	bool CreateSwapchain()
	{
		VkSurfaceCapabilitiesKHR caps = {};
		VkResult r = vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
		if (r != VK_SUCCESS)
			return Fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
		if (caps.currentExtent.width != UINT32_MAX)
			extent = caps.currentExtent;
		uint32_t images_wanted = std::max(2u, caps.minImageCount);
		if (caps.maxImageCount)
			images_wanted = std::min(images_wanted, caps.maxImageCount);
		VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
		sci.surface = surface;
		sci.minImageCount = images_wanted;
		sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
		sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		sci.imageExtent = extent;
		sci.imageArrayLayers = 1;
		sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
		sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
		sci.clipped = VK_TRUE;
		r = vk.vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain);
		if (r != VK_SUCCESS)
			return Fail("vkCreateSwapchainKHR", r);
		uint32_t n = 0;
		vk.vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
		images.resize(n);
		vk.vkGetSwapchainImagesKHR(device, swapchain, &n, images.data());
		VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		for (int i = 0; i < 2; i++)
			if (vk.vkCreateSemaphore(device, &si, nullptr, &acquired[i]) != VK_SUCCESS ||
				vk.vkCreateSemaphore(device, &si, nullptr, &rendered[i]) != VK_SUCCESS)
				return Fail("vkCreateSemaphore", VK_ERROR_UNKNOWN);
		std::printf("[shelf] swapchain: %u images %ux%u\n", n, extent.width, extent.height);
		return true;
	}

	void Destroy()
	{
		if (device)
		{
			vk.vkDeviceWaitIdle(device);
			for (int i = 0; i < 2; i++)
			{
				if (acquired[i])
					vk.vkDestroySemaphore(device, acquired[i], nullptr);
				if (rendered[i])
					vk.vkDestroySemaphore(device, rendered[i], nullptr);
				acquired[i] = rendered[i] = VK_NULL_HANDLE;
			}
			if (swapchain)
				vk.vkDestroySwapchainKHR(device, swapchain, nullptr); // gives VideoOut back before the title starts
			swapchain = VK_NULL_HANDLE;
			vk.vkDestroyDevice(device, nullptr);
			device = VK_NULL_HANDLE;
		}
		if (surface)
			vk.vkDestroySurfaceKHR(instance, surface, nullptr);
		surface = VK_NULL_HANDLE;
		if (instance)
			vk.vkDestroyInstance(instance, nullptr);
		instance = VK_NULL_HANDLE;
	}
};

int HideSplash()
{
	static int s_result = 1;
	static bool s_done = false;
	if (!s_done)
	{
		s_done = true;
		s_result = sceSystemServiceHideSplashScreen();
	}
	return s_result;
}

std::string ReadLast()
{
	std::string s;
	rshelf::ReadWhole(std::string(kRcompDir) + "/lastgame.txt", s, 64);
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
		s.pop_back();
	return s;
}

void WriteLast(const std::string& id)
{
	const std::string s = id + "\n";
	rshelf::WriteAtomic(std::string(kRcompDir) + "/lastgame.txt", s.data(), s.size());
}

rshelf::SessionConfig MakeConfig(fe::SoundSink* sound)
{
	rshelf::SessionConfig sc;
	sc.catalog.homebrew_dir = "/data/homebrew";
	sc.catalog.package_dirs = {std::string(kRcompDir) + "/packages"};
	for (const std::string& d : rshelf::UsbPackageDirs())
		sc.catalog.package_dirs.push_back(d);
	sc.catalog.db_cache_dir = std::string(kRcompDir) + "/cache/x360db";
	sc.catalog.self_title_id = RSHELF_TITLE_ID;
	sc.covers_dir = std::string(kRcompDir) + "/covers";
	sc.cover_cache_dir = std::string(kRcompDir) + "/cache/covers";
	sc.trash_dir = std::string(kRcompDir) + "/trash";
	sc.allow_download = true;
	sc.download = [](const std::string& url, std::vector<uint8_t>& out) { return g_http.Get(url, out); };
	sc.abort_download = []() { g_http.Abort(); };
	sc.resume_download = []() { g_http.Resume(); };
	sc.build_tag = RSHELF_BUILD_TAG;
	sc.sound = sound;
	sc.log = [](const std::string& l) { Log("[catalog] " + l); };
	return sc;
}

// One shelf, from opening the display to closing it. Returns the title picked to play, or false when the shelf
// could not run (no display, no controller).
bool RunShelf(int pad, const std::string& preselect, const std::string& message, GameInfo& picked)
{
	const double t0 = Now();
	Display* display = new Display();
	if (!display->Init())
	{
		display->Destroy();
		delete display;
		return false;
	}
	Fonts* fonts = new Fonts();
	Renderer renderer;
	if (!fonts->Init(rshelf_font_text, static_cast<size_t>(rshelf_font_text_end - rshelf_font_text), rshelf_font_icons,
			static_cast<size_t>(rshelf_font_icons_end - rshelf_font_icons), rshelf_font_brands,
			static_cast<size_t>(rshelf_font_brands_end - rshelf_font_brands)) ||
		!renderer.Init(&display->vk, display->pd, display->device, display->qf, display->queue, display->extent.width,
			display->extent.height, VK_FORMAT_B8G8R8A8_UNORM, display->images, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR))
	{
		std::printf("[shelf] renderer: %s\n", renderer.error().c_str());
		renderer.Shutdown();
		display->Destroy();
		delete display;
		delete fonts;
		return false;
	}
	Mixer* mixer = new Mixer();
	mixer->Build();
	AudioOut* audio = new AudioOut();
	if (!audio->Start(mixer))
	{
		delete audio;
		audio = nullptr;
	}

	rshelf::Session* session = new rshelf::Session();
	bool ok = session->Start(&renderer, fonts, MakeConfig(audio ? mixer : nullptr), preselect, message);
	std::printf("[shelf] up in %.0f ms (%s), %zu title(s)\n", (Now() - t0) * 1000.0, ok ? "ok" : renderer.error().c_str(),
		ok ? session->games().size() : size_t(0));
	std::fflush(stdout);

	FrameDesc frame;
	double last_t = Now(), report_t = last_t, worst = 0;
	unsigned frames = 0, slot = 0;
	bool first_shown = false, play = false;
	while (ok && !(play = session->PlayChosen(picked)))
	{
		const double now = Now();
		const double dt = now - last_t;
		last_t = now;
		worst = std::max(worst, dt);
		PadData pd;
		std::memset(&pd, 0, sizeof(pd));
		pd.lx = pd.ly = 128;
		Input in;
		if (scePadReadState(pad, &pd) == 0)
		{
			in.left = (pd.buttons & kPadLeft) || pd.lx < 48;
			in.right = (pd.buttons & kPadRight) || pd.lx > 208;
			in.cross = pd.buttons & kPadCross;
			in.options = pd.buttons & kPadOptions;
			in.l1 = pd.buttons & kPadL1;
			in.r1 = pd.buttons & kPadR1;
			in.up = (pd.buttons & kPadUp) || pd.ly < 48;
			in.down = (pd.buttons & kPadDown) || pd.ly > 208;
			in.square = pd.buttons & kPadSquare;
			in.triangle = pd.buttons & kPadTriangle;
			in.circle = pd.buttons & kPadCircle;
			in.l2 = (pd.buttons & kPadL2) || pd.l2 > 160;
			in.r2 = (pd.buttons & kPadR2) || pd.r2 > 160;
		}
		session->Update(dt, in);
		session->Build(frame, Clock());
		if (!renderer.WaitForSlot())
		{
			ok = false;
			break;
		}
		uint32_t index = 0;
		VkResult r = display->vk.vkAcquireNextImageKHR(display->device, display->swapchain, UINT64_MAX, display->acquired[slot],
			VK_NULL_HANDLE, &index);
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		{
			std::printf("[shelf] vkAcquireNextImageKHR: %d\n", static_cast<int>(r));
			ok = false;
			break;
		}
		if (!renderer.Render(frame, index, display->acquired[slot], display->rendered[slot]))
		{
			std::printf("[shelf] render: %s\n", renderer.error().c_str());
			ok = false;
			break;
		}
		VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &display->rendered[slot];
		pi.swapchainCount = 1;
		pi.pSwapchains = &display->swapchain;
		pi.pImageIndices = &index;
		r = display->vk.vkQueuePresentKHR(display->queue, &pi);
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		{
			std::printf("[shelf] vkQueuePresentKHR: %d\n", static_cast<int>(r));
			ok = false;
			break;
		}
		slot ^= 1;
		frames++;
		if (!first_shown)
		{
			first_shown = true;
			// The launch screen (sce_sys/pic1) covers the app until it is hidden.
			std::printf("[shelf] first frame %.0f ms after start; launch screen hidden (%d)\n", (Now() - t0) * 1000.0, HideSplash());
			std::fflush(stdout);
		}
		if (now - report_t >= 10.0)
		{
			std::printf("[shelf] %u frames in %.1f s, worst %.1f ms\n", frames, now - report_t, worst * 1000.0);
			std::fflush(stdout);
			frames = 0;
			worst = 0;
			report_t = now;
		}
	}

	renderer.WaitIdle();
	const bool stopped = session->Stop(1500);
	renderer.Shutdown();
	display->Destroy();
	delete display;
	if (stopped)
	{
		delete session;
		delete fonts;
		g_http.Term();
	}
	else
		Log("[shelf] a download is still running; leaving it to finish");
	if (audio)
	{
		// Let the launch sound ring out.
		const double s0 = Now();
		while (!mixer->Idle() && Now() - s0 < 0.4)
			usleep(5000);
		audio->Stop();
		delete audio;
	}
	delete mixer;
	return play;
}

// Starts `g`'s own application. The result of sceLncUtilLaunchApp: negative on failure.
int Launch(const GameInfo& g, int32_t user)
{
	LncAppParam param = {};
	param.size = sizeof(param);
	param.user_id = user;
	const char* argv[] = {nullptr};
	const int rc = sceLncUtilLaunchApp(g.stem.c_str(), argv, &param);
	std::printf("[shelf] sceLncUtilLaunchApp(%s) = %#x\n", g.stem.c_str(), static_cast<unsigned>(rc));
	std::fflush(stdout);
	return rc;
}
} // namespace

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	rshelf::MakeDirs(std::string(kRcompDir) + "/logs");
	if (!std::freopen("/data/rcomp/logs/shelf.log", "a", stdout))
		std::freopen("/app0/shelf.log", "a", stdout);
	setvbuf(stdout, nullptr, _IOLBF, 0);
	std::printf("RSHELF begin title=%s build=%s\n", RSHELF_TITLE_ID, RSHELF_BUILD_TAG);
	for (const char* d : {"/packages", "/covers", "/cache/covers", "/cache/x360db", "/trash", "/lang"})
		rshelf::MakeDirs(std::string(kRcompDir) + d);
	int lang = 1;
	if (sceSystemServiceParamGetInt(1 /* language */, &lang) != 0)
		lang = 1;
	SetLanguage(lang, std::string(kRcompDir) + "/lang");

	// The titles' names come from x360db: entries the cache lacks are fetched before the shelf opens.
	{
		const std::vector<GameInfo> found = rshelf::ScanCatalog(MakeConfig(nullptr).catalog, nullptr);
		const std::vector<std::string> missing = rshelf::MissingDbEntries(std::string(kRcompDir) + "/cache/x360db", found);
		if (!missing.empty())
			rshelf::FetchDbEntries(std::string(kRcompDir) + "/cache/x360db", missing,
				[](const std::string& url, std::vector<uint8_t>& out) { return g_http.Get(url, out); }, 8.0,
				[](const std::string& l) { Log("[catalog] " + l); });
		g_http.Term();
	}

	int32_t user = -1;
	(void)sceUserServiceInitialize(nullptr);
	(void)sceUserServiceGetInitialUser(&user);
	(void)scePadInit();
	int pad = -1;
	for (int tries = 0; pad < 0; tries++)
	{
		pad = user >= 0 ? scePadOpen(user, 0, 0, nullptr) : -1;
		if (pad < 0)
		{
			if (tries % 10 == 0)
				std::printf("[shelf] no controller yet (%d)\n", pad);
			sleep(1);
			(void)sceUserServiceGetInitialUser(&user);
		}
	}

	std::string preselect = ReadLast(), message;
	for (;;)
	{
		GameInfo g;
		if (!RunShelf(pad, preselect, message, g))
		{
			Log("[shelf] the shelf could not run; trying again in 5 s");
			sleep(5);
			continue;
		}
		WriteLast(g.stem);
		preselect = g.stem;
		const int self_app = sceLncUtilGetAppIdOfRunningBigApp();
		const int rc = Launch(g, user);
		if (rc < 0)
		{
			char code[16];
			std::snprintf(code, sizeof(code), "%#x", static_cast<unsigned>(rc));
			message = rshelf::Format(Tr(Str::LaunchFailed), g.title, code);
			continue; // back to the shelf, with the reason
		}
		message.clear();
		// The title is the console's application now. If the system keeps this one in the background and brings it
		// back once the title is gone (the running big application is this one again), the shelf opens again.
		std::printf("[shelf] started %s; big application %#x before, waiting\n", g.stem.c_str(), static_cast<unsigned>(self_app));
		std::fflush(stdout);
		bool away = false;
		for (;;)
		{
			sleep(1);
			const int running = sceLncUtilGetAppIdOfRunningBigApp();
			if (running != self_app)
				away = true;
			else if (away)
			{
				std::printf("[shelf] back in front (big application %#x)\n", static_cast<unsigned>(running));
				break;
			}
		}
	}
}
