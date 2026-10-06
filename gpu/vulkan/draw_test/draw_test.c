/*
 * R-comp - Agent 4 (PS5_Vulkan integration).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * rcomp_vk_draw_test: the first deterministic draw through the Vulkan
 * implementation in front of us (PS5_Vulkan on the console, whatever the
 * Khronos loader finds on a host).
 *
 *   rcomp_vk_draw_test [--ppm OUT.ppm] [--present [FRAMES]]
 *
 * Contract (gpu/vulkan/draw_test/README.md): Vulkan 1.0 only.
 *  - one classic VkRenderPass, one subpass, one R8G8B8A8_UNORM colour
 *    attachment (64x64, OPTIMAL tiling, offscreen), loadOp CLEAR, storeOp STORE;
 *  - one vertex buffer (3 x vec2, R32G32_SFLOAT) and one uint16 index buffer;
 *  - one descriptor set, one UNIFORM_BUFFER binding (the colour, fragment);
 *  - SPIR-V 1.0 shaders (draw_test/generated, from draw_test/shaders);
 *  - explicit barriers: host writes -> vertex/index/uniform reads, colour
 *    attachment -> transfer source (layout transition), transfer write -> host
 *    read; one fence; no queue-wait-idle shortcuts in the checked path;
 *  - copy of the image into a HOST_VISIBLE buffer, invalidate when not
 *    coherent, then every pixel is classified and checked (below).
 *  No dynamic rendering, descriptor indexing, buffer device address, timeline
 *  semaphores, MRT, MSAA, push constants, dynamic state or extensions (except
 *  VK_KHR_surface/display/swapchain in the optional --present step).
 *
 * Check: the triangle is A(8,8) B(56,8) C(8,56) in pixel space. Each pixel
 * centre (x+0.5, y+0.5) is classified by its signed distance to the three
 * edges: "inside" when >= MARGIN from all of them, "outside" when <= -MARGIN
 * from at least one, otherwise "edge" and excluded (rasterisation rules and
 * sub-pixel precision are not what this test is about). MARGIN = 1.5 px.
 * Inside pixels must equal EXPECT_TRIANGLE exactly, outside pixels
 * EXPECT_CLEAR exactly (R,G,B,A bytes).
 *
 * Presentation (--present) runs only after the readback check passed, and only
 * when the same gates the probe reports are met: VK_KHR_surface +
 * VK_KHR_display on the instance, a display with a mode, a plane that supports
 * it, a queue family that can present to the display-plane surface, and
 * VK_KHR_swapchain on the device. Otherwise it reports SKIPPED with the reason
 * and does not change the exit code. It clears each swapchain image and copies
 * the checked 64x64 image into its top-left corner with vkCmdCopyImage (a raw
 * copy: R8G8B8A8 bytes land in a B8G8R8A8 image, so red and blue swap on
 * screen; the checked result is the readback, not the screen).
 *
 * Exit: 0 PASS, 1 FAIL (pixels differ), 2 setup/Vulkan error, 3 present step
 * failed after a PASS readback.
 */
#include "rcvk_loader.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw_test_spirv.h"

#if defined(RCOMP_TARGET_PS5)
#define EVIDENCE "PS5 (PS5_Vulkan linked statically)"
#else
#define EVIDENCE "HOST-ONLY: not evidence about PS5_Vulkan"
#endif

#define W 64
#define H 64
#define MARGIN 1.5
static const uint8_t EXPECT_CLEAR[4] = {255, 0, 0, 255};
/* 0.2 * 255 = 51, 0.6 * 255 = 153: exact after UNORM rounding. */
static const float UBO_COLOUR[4] = {0.2f, 0.6f, 1.0f, 1.0f};
static const uint8_t EXPECT_TRIANGLE[4] = {51, 153, 255, 255};
static const float VERTICES[3][2] = {
   {8.0f / 32.0f - 1.0f, 8.0f / 32.0f - 1.0f},  /* A (8, 8)  */
   {56.0f / 32.0f - 1.0f, 8.0f / 32.0f - 1.0f}, /* B (56, 8) */
   {8.0f / 32.0f - 1.0f, 56.0f / 32.0f - 1.0f}, /* C (8, 56) */
};
static const uint16_t INDICES[3] = {0, 1, 2};

#define CHECK(call)                                                                                \
   do {                                                                                            \
      VkResult r_ = (call);                                                                        \
      if (r_ != VK_SUCCESS) {                                                                      \
         fprintf(stderr, "draw_test: %s -> %s (%d) at line %d\n", #call, rcvk_result_name(r_),    \
                 (int)r_, __LINE__);                                                               \
         return 2;                                                                                 \
      }                                                                                            \
   } while (0)

static VkPhysicalDeviceMemoryProperties mem_props;

static int
find_memory(uint32_t bits, VkMemoryPropertyFlags want, uint32_t *index)
{
   for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want) {
         *index = i;
         return 0;
      }
   return -1;
}

struct buffer {
   VkBuffer buffer;
   VkDeviceMemory memory;
   VkDeviceSize size;
   bool coherent;
   void *map;
};

static int
make_host_buffer(VkDevice dev, VkDeviceSize size, VkBufferUsageFlags usage, struct buffer *b)
{
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   CHECK(rc_vkCreateBuffer(dev, &bci, NULL, &b->buffer));
   VkMemoryRequirements req;
   rc_vkGetBufferMemoryRequirements(dev, b->buffer, &req);
   uint32_t type;
   /* Prefer coherent; fall back to visible-only with explicit flush/invalidate. */
   if (find_memory(req.memoryTypeBits,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &type) == 0)
      b->coherent = true;
   else if (find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &type) == 0)
      b->coherent = false;
   else {
      fprintf(stderr, "draw_test: no HOST_VISIBLE memory type for a buffer\n");
      return 2;
   }
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   CHECK(rc_vkAllocateMemory(dev, &mai, NULL, &b->memory));
   CHECK(rc_vkBindBufferMemory(dev, b->buffer, b->memory, 0));
   CHECK(rc_vkMapMemory(dev, b->memory, 0, VK_WHOLE_SIZE, 0, &b->map));
   b->size = size;
   return 0;
}

static int
flush(VkDevice dev, const struct buffer *b)
{
   if (b->coherent)
      return 0;
   VkMappedMemoryRange r = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                            .memory = b->memory,
                            .offset = 0,
                            .size = VK_WHOLE_SIZE};
   CHECK(rc_vkFlushMappedMemoryRanges(dev, 1, &r));
   return 0;
}

static void
destroy_buffer(VkDevice dev, struct buffer *b)
{
   if (b->map)
      rc_vkUnmapMemory(dev, b->memory);
   if (b->buffer)
      rc_vkDestroyBuffer(dev, b->buffer, NULL);
   if (b->memory)
      rc_vkFreeMemory(dev, b->memory, NULL);
   memset(b, 0, sizeof *b);
}

/* -1 outside, 0 edge (excluded), +1 inside. */
static int
classify(int x, int y)
{
   const double px = x + 0.5, py = y + 0.5;
   const double d[3] = {py - 8.0, px - 8.0, (64.0 - (px + py)) / sqrt(2.0)};
   bool inside = true;
   for (int i = 0; i < 3; i++) {
      if (d[i] <= -MARGIN)
         return -1;
      if (d[i] < MARGIN)
         inside = false;
   }
   return inside ? 1 : 0;
}

static int
write_ppm(const char *path, const uint8_t *rgba)
{
   FILE *f = fopen(path, "wb");
   if (!f)
      return -1;
   fprintf(f, "P6\n%d %d\n255\n", W, H);
   for (int i = 0; i < W * H; i++)
      fwrite(rgba + 4 * i, 1, 3, f);
   fclose(f);
   return 0;
}

/* Everything the optional present step needs from the setup. */
struct ctx {
   VkInstance instance;
   VkPhysicalDevice pd;
   VkDevice dev;
   VkQueue queue;
   uint32_t qf;
   VkCommandPool pool;
   VkImage image; /* the checked 64x64 image, TRANSFER_SRC_OPTIMAL after the test */
   bool display_enabled;
   bool swapchain_enabled;
   VkSurfaceKHR surface;
};

static int present_step(struct ctx *c, uint32_t frames);

/* Surface gate, evaluated before device creation (VK_KHR_swapchain is
 * enabled only when this passes). Leaves c->surface set on success. */
static const char *
present_gate(struct ctx *c)
{
   if (!c->display_enabled)
      return "instance does not list both VK_KHR_surface and VK_KHR_display";
   if (!rc_vkGetPhysicalDeviceDisplayPropertiesKHR || !rc_vkGetDisplayModePropertiesKHR ||
       !rc_vkGetPhysicalDeviceDisplayPlanePropertiesKHR ||
       !rc_vkGetDisplayPlaneSupportedDisplaysKHR || !rc_vkCreateDisplayPlaneSurfaceKHR ||
       !rc_vkGetPhysicalDeviceSurfaceSupportKHR || !rc_vkGetPhysicalDeviceSurfaceCapabilitiesKHR ||
       !rc_vkGetPhysicalDeviceSurfaceFormatsKHR || !rc_vkDestroySurfaceKHR)
      return "VK_KHR_display / VK_KHR_surface commands not resolved";
   uint32_t nd = 1;
   VkDisplayPropertiesKHR dp;
   VkResult r = rc_vkGetPhysicalDeviceDisplayPropertiesKHR(c->pd, &nd, &dp);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || nd == 0)
      return "no display";
   uint32_t nm = 1;
   VkDisplayModePropertiesKHR mp;
   r = rc_vkGetDisplayModePropertiesKHR(c->pd, dp.display, &nm, &mp);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || nm == 0)
      return "display has no mode";
   uint32_t np = 0;
   if (rc_vkGetPhysicalDeviceDisplayPlanePropertiesKHR(c->pd, &np, NULL) != VK_SUCCESS || !np)
      return "no display plane";
   uint32_t plane = UINT32_MAX;
   for (uint32_t p = 0; p < np && plane == UINT32_MAX; p++) {
      uint32_t ns = 0;
      if (rc_vkGetDisplayPlaneSupportedDisplaysKHR(c->pd, p, &ns, NULL) != VK_SUCCESS || !ns)
         continue;
      VkDisplayKHR *sd = calloc(ns, sizeof *sd);
      if (sd && rc_vkGetDisplayPlaneSupportedDisplaysKHR(c->pd, p, &ns, sd) == VK_SUCCESS)
         for (uint32_t s = 0; s < ns; s++)
            if (sd[s] == dp.display)
               plane = p;
      free(sd);
   }
   if (plane == UINT32_MAX)
      return "no plane supports the display";
   VkDisplaySurfaceCreateInfoKHR sci = {
      .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
      .displayMode = mp.displayMode,
      .planeIndex = plane,
      .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .globalAlpha = 1.0f,
      .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
      .imageExtent = mp.parameters.visibleRegion,
   };
   if (rc_vkCreateDisplayPlaneSurfaceKHR(c->instance, &sci, NULL, &c->surface) != VK_SUCCESS)
      return "vkCreateDisplayPlaneSurfaceKHR failed";
   VkBool32 ok = VK_FALSE;
   if (rc_vkGetPhysicalDeviceSurfaceSupportKHR(c->pd, c->qf, c->surface, &ok) != VK_SUCCESS ||
       !ok) {
      rc_vkDestroySurfaceKHR(c->instance, c->surface, NULL);
      c->surface = VK_NULL_HANDLE;
      return "the graphics queue family cannot present to the display surface";
   }
   uint32_t ne = 0;
   rc_vkEnumerateDeviceExtensionProperties(c->pd, NULL, &ne, NULL);
   VkExtensionProperties *de = calloc(ne ? ne : 1, sizeof *de);
   bool sc = false;
   if (de && rc_vkEnumerateDeviceExtensionProperties(c->pd, NULL, &ne, de) == VK_SUCCESS)
      for (uint32_t i = 0; i < ne; i++)
         sc |= strcmp(de[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
   free(de);
   if (!sc) {
      rc_vkDestroySurfaceKHR(c->instance, c->surface, NULL);
      c->surface = VK_NULL_HANDLE;
      return "device does not list VK_KHR_swapchain";
   }
   return NULL;
}

static VkShaderModule
shader(VkDevice dev, const uint32_t *code, size_t bytes)
{
   VkShaderModuleCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = bytes,
      .pCode = code,
   };
   VkShaderModule m = VK_NULL_HANDLE;
   VkResult r = rc_vkCreateShaderModule(dev, &ci, NULL, &m);
   if (r != VK_SUCCESS)
      fprintf(stderr, "draw_test: vkCreateShaderModule -> %s\n", rcvk_result_name(r));
   return m;
}

int
main(int argc, char **argv)
{
   const char *ppm = NULL;
   bool present = false;
   uint32_t frames = 120;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--ppm") && i + 1 < argc)
         ppm = argv[++i];
      else if (!strcmp(argv[i], "--present")) {
         present = true;
         if (i + 1 < argc && argv[i + 1][0] != '-')
            frames = (uint32_t)strtoul(argv[++i], NULL, 10);
      } else {
         fprintf(stderr, "usage: %s [--ppm OUT.ppm] [--present [FRAMES]]\n", argv[0]);
         return 2;
      }
   }
   printf("draw_test: evidence: %s\n", EVIDENCE);
   struct ctx c;
   memset(&c, 0, sizeof c);

   if (rcvk_load_global())
      return 2;

   /* Instance: Vulkan 1.0, no layers; surface + display only for --present. */
   const char *iext[2];
   uint32_t n_iext = 0;
   if (present) {
      uint32_t ne = 0;
      rc_vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
      VkExtensionProperties *ie = calloc(ne ? ne : 1, sizeof *ie);
      bool s = false, d = false;
      if (ie && rc_vkEnumerateInstanceExtensionProperties(NULL, &ne, ie) == VK_SUCCESS)
         for (uint32_t i = 0; i < ne; i++) {
            s |= !strcmp(ie[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
            d |= !strcmp(ie[i].extensionName, VK_KHR_DISPLAY_EXTENSION_NAME);
         }
      free(ie);
      if (s && d) {
         iext[n_iext++] = VK_KHR_SURFACE_EXTENSION_NAME;
         iext[n_iext++] = VK_KHR_DISPLAY_EXTENSION_NAME;
         c.display_enabled = true;
      }
   }
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "rcomp_vk_draw_test",
      .applicationVersion = 1,
      .pEngineName = "R-comp",
      .engineVersion = 1,
      .apiVersion = VK_API_VERSION_1_0,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = n_iext,
      .ppEnabledExtensionNames = iext,
   };
   CHECK(rc_vkCreateInstance(&ici, NULL, &c.instance));
   if (rcvk_load_instance(c.instance))
      return 2;
   rcvk_load_instance_optional(c.instance);

   uint32_t npd = 1;
   VkResult r = rc_vkEnumeratePhysicalDevices(c.instance, &npd, &c.pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || npd == 0) {
      fprintf(stderr, "draw_test: no physical device (%s)\n", rcvk_result_name(r));
      return 2;
   }
   VkPhysicalDeviceProperties props;
   rc_vkGetPhysicalDeviceProperties(c.pd, &props);
   printf("draw_test: device \"%s\" apiVersion %u.%u.%u driverVersion 0x%x\n", props.deviceName,
          VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
          VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
   rc_vkGetPhysicalDeviceMemoryProperties(c.pd, &mem_props);

   uint32_t nq = 0;
   rc_vkGetPhysicalDeviceQueueFamilyProperties(c.pd, &nq, NULL);
   VkQueueFamilyProperties *qfp = calloc(nq ? nq : 1, sizeof *qfp);
   rc_vkGetPhysicalDeviceQueueFamilyProperties(c.pd, &nq, qfp);
   c.qf = UINT32_MAX;
   for (uint32_t i = 0; i < nq && c.qf == UINT32_MAX; i++)
      if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
         c.qf = i;
   free(qfp);
   if (c.qf == UINT32_MAX) {
      fprintf(stderr, "draw_test: no graphics queue family\n");
      return 2;
   }

   VkFormatProperties fp;
   rc_vkGetPhysicalDeviceFormatProperties(c.pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
   const VkFormatFeatureFlags need_color =
      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT; /* TRANSFER_SRC is implied in 1.0 */
   if ((fp.optimalTilingFeatures & need_color) != need_color) {
      fprintf(stderr, "draw_test: R8G8B8A8_UNORM optimal lacks COLOR_ATTACHMENT (0x%x)\n",
              fp.optimalTilingFeatures);
      return 2;
   }
   rc_vkGetPhysicalDeviceFormatProperties(c.pd, VK_FORMAT_R32G32_SFLOAT, &fp);
   if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
      fprintf(stderr, "draw_test: R32G32_SFLOAT lacks VERTEX_BUFFER\n");
      return 2;
   }

   const char *present_skip = NULL;
   if (present) {
      present_skip = present_gate(&c);
      c.swapchain_enabled = present_skip == NULL;
   }
   const char *dext[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
   const float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = c.qf,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = c.swapchain_enabled ? 1 : 0,
      .ppEnabledExtensionNames = c.swapchain_enabled ? dext : NULL,
      .pEnabledFeatures = NULL, /* no optional feature */
   };
   CHECK(rc_vkCreateDevice(c.pd, &dci, NULL, &c.dev));
   if (rcvk_load_device(c.dev))
      return 2;
   VkDevice dev = c.dev;
   rc_vkGetDeviceQueue(dev, c.qf, 0, &c.queue);

   /* ---- resources ---- */
   struct buffer vb = {0}, ib = {0}, ub = {0}, rb = {0};
   int e;
   if ((e = make_host_buffer(dev, sizeof VERTICES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb)) ||
       (e = make_host_buffer(dev, sizeof INDICES, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &ib)) ||
       (e = make_host_buffer(dev, sizeof UBO_COLOUR, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ub)) ||
       (e = make_host_buffer(dev, (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb)))
      return e;
   memcpy(vb.map, VERTICES, sizeof VERTICES);
   memcpy(ib.map, INDICES, sizeof INDICES);
   memcpy(ub.map, UBO_COLOUR, sizeof UBO_COLOUR);
   /* Poison the readback so an unperformed copy cannot pass. */
   memset(rb.map, 0xA5, (size_t)W * H * 4);
   if ((e = flush(dev, &vb)) || (e = flush(dev, &ib)) || (e = flush(dev, &ub)) ||
       (e = flush(dev, &rb)))
      return e;

   VkImageCreateInfo imci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {W, H, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   CHECK(rc_vkCreateImage(dev, &imci, NULL, &c.image));
   VkMemoryRequirements ireq;
   rc_vkGetImageMemoryRequirements(dev, c.image, &ireq);
   uint32_t itype;
   if (find_memory(ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &itype) &&
       find_memory(ireq.memoryTypeBits, 0, &itype)) {
      fprintf(stderr, "draw_test: no memory type for the image\n");
      return 2;
   }
   VkMemoryAllocateInfo imai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = ireq.size,
                                .memoryTypeIndex = itype};
   VkDeviceMemory imem;
   CHECK(rc_vkAllocateMemory(dev, &imai, NULL, &imem));
   CHECK(rc_vkBindImageMemory(dev, c.image, imem, 0));
   VkImageViewCreateInfo ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = c.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   VkImageView view;
   CHECK(rc_vkCreateImageView(dev, &ivci, NULL, &view));

   /* ---- render pass: classic, one subpass, explicit external dependencies ---- */
   VkAttachmentDescription att = {
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &ref,
   };
   VkSubpassDependency deps[2] = {
      {.srcSubpass = VK_SUBPASS_EXTERNAL,
       .dstSubpass = 0,
       .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .srcAccessMask = 0,
       .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
      {.srcSubpass = 0,
       .dstSubpass = VK_SUBPASS_EXTERNAL,
       .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
       .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
       .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT},
   };
   VkRenderPassCreateInfo rpci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &att,
      .subpassCount = 1,
      .pSubpasses = &sub,
      .dependencyCount = 2,
      .pDependencies = deps,
   };
   VkRenderPass rp;
   CHECK(rc_vkCreateRenderPass(dev, &rpci, NULL, &rp));
   VkFramebufferCreateInfo fbci = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = rp,
      .attachmentCount = 1,
      .pAttachments = &view,
      .width = W,
      .height = H,
      .layers = 1,
   };
   VkFramebuffer fb;
   CHECK(rc_vkCreateFramebuffer(dev, &fbci, NULL, &fb));

   /* ---- descriptor set: one UBO for the fragment stage ---- */
   VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   VkDescriptorSetLayoutCreateInfo dslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   VkDescriptorSetLayout dsl;
   CHECK(rc_vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
   VkDescriptorPoolSize psize = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &psize,
   };
   VkDescriptorPool dpool;
   CHECK(rc_vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
   VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dpool,
      .descriptorSetCount = 1,
      .pSetLayouts = &dsl,
   };
   VkDescriptorSet ds;
   CHECK(rc_vkAllocateDescriptorSets(dev, &dsai, &ds));
   VkDescriptorBufferInfo dbi = {ub.buffer, 0, sizeof UBO_COLOUR};
   VkWriteDescriptorSet wds = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = ds,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .pBufferInfo = &dbi,
   };
   rc_vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &dsl,
   };
   VkPipelineLayout pl;
   CHECK(rc_vkCreatePipelineLayout(dev, &plci, NULL, &pl));

   /* ---- pipeline: static viewport/scissor, no dynamic state ---- */
   VkShaderModule vs = shader(dev, triangle_vert_spv, sizeof triangle_vert_spv);
   VkShaderModule fs = shader(dev, triangle_frag_spv, sizeof triangle_frag_spv);
   if (!vs || !fs)
      return 2;
   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vs,
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = fs,
       .pName = "main"},
   };
   VkVertexInputBindingDescription vbind = {0, sizeof VERTICES[0], VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription vattr = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &vbind,
      .vertexAttributeDescriptionCount = 1,
      .pVertexAttributeDescriptions = &vattr,
   };
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport vp = {0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f};
   VkRect2D sc = {{0, 0}, {W, H}};
   VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &vp,
      .scissorCount = 1,
      .pScissors = &sc,
   };
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
   };
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineColorBlendAttachmentState cba = {
      .blendEnable = VK_FALSE,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &cba,
   };
   VkGraphicsPipelineCreateInfo gpci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vi,
      .pInputAssemblyState = &ia,
      .pViewportState = &vps,
      .pRasterizationState = &rs,
      .pMultisampleState = &ms,
      .pColorBlendState = &cb,
      .layout = pl,
      .renderPass = rp,
      .subpass = 0,
   };
   VkPipeline pipe;
   CHECK(rc_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

   /* ---- record ---- */
   VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = c.qf,
   };
   CHECK(rc_vkCreateCommandPool(dev, &cpci, NULL, &c.pool));
   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = c.pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(rc_vkAllocateCommandBuffers(dev, &cbai, &cmd));
   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK(rc_vkBeginCommandBuffer(cmd, &cbbi));

   /* Host writes -> vertex, index and uniform reads. */
   VkMemoryBarrier host_to_gpu = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                       VK_ACCESS_UNIFORM_READ_BIT,
   };
   rc_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 1, &host_to_gpu, 0, NULL, 0, NULL);

   VkClearValue clear = {.color = {.float32 = {EXPECT_CLEAR[0] / 255.0f, EXPECT_CLEAR[1] / 255.0f,
                                               EXPECT_CLEAR[2] / 255.0f,
                                               EXPECT_CLEAR[3] / 255.0f}}};
   VkRenderPassBeginInfo rpbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = rp,
      .framebuffer = fb,
      .renderArea = {{0, 0}, {W, H}},
      .clearValueCount = 1,
      .pClearValues = &clear,
   };
   rc_vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   rc_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   rc_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
   VkDeviceSize zero = 0;
   rc_vkCmdBindVertexBuffers(cmd, 0, 1, &vb.buffer, &zero);
   rc_vkCmdBindIndexBuffer(cmd, ib.buffer, 0, VK_INDEX_TYPE_UINT16);
   rc_vkCmdDrawIndexed(cmd, 3, 1, 0, 0, 0);
   rc_vkCmdEndRenderPass(cmd);

   /* Colour attachment -> transfer source. */
   VkImageMemoryBarrier to_src = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = c.image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   rc_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
   VkBufferImageCopy region = {
      .bufferOffset = 0,
      .bufferRowLength = 0, /* tightly packed: W texels */
      .bufferImageHeight = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageOffset = {0, 0, 0},
      .imageExtent = {W, H, 1},
   };
   rc_vkCmdCopyImageToBuffer(cmd, c.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1,
                             &region);
   /* Transfer write -> host read. */
   VkBufferMemoryBarrier to_host = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = rb.buffer,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
   };
   rc_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                           NULL, 1, &to_host, 0, NULL);
   CHECK(rc_vkEndCommandBuffer(cmd));

   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK(rc_vkCreateFence(dev, &fci, NULL, &fence));
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(rc_vkQueueSubmit(c.queue, 1, &si, fence));
   /* 5 s: a lost submission is a FAIL with a reason, not a hang. */
   r = rc_vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_C(5000000000));
   if (r != VK_SUCCESS) {
      fprintf(stderr, "draw_test: fence wait -> %s\n", rcvk_result_name(r));
      return 2;
   }
   if (!rb.coherent) {
      VkMappedMemoryRange mr = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                .memory = rb.memory,
                                .size = VK_WHOLE_SIZE};
      CHECK(rc_vkInvalidateMappedMemoryRanges(dev, 1, &mr));
   }

   /* ---- check ---- */
   const uint8_t *px = rb.map;
   unsigned inside = 0, outside = 0, edge = 0, bad = 0;
   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
         const int k = classify(x, y);
         const uint8_t *p = px + 4 * (y * W + x);
         const uint8_t *want = k > 0 ? EXPECT_TRIANGLE : EXPECT_CLEAR;
         if (k == 0) {
            edge++;
            continue;
         }
         k > 0 ? inside++ : outside++;
         if (memcmp(p, want, 4)) {
            if (bad < 8)
               printf("draw_test: MISMATCH (%d,%d) %s got %u,%u,%u,%u want %u,%u,%u,%u\n", x, y,
                      k > 0 ? "inside" : "outside", p[0], p[1], p[2], p[3], want[0], want[1],
                      want[2], want[3]);
            bad++;
         }
      }
   /* Named control pixels, reported for the log. */
   static const int ctl[][2] = {{16, 16}, {12, 40}, {40, 12}, {2, 2}, {60, 60}, {50, 50}};
   for (unsigned i = 0; i < sizeof ctl / sizeof ctl[0]; i++) {
      const uint8_t *p = px + 4 * (ctl[i][1] * W + ctl[i][0]);
      printf("draw_test: control (%d,%d) class %d = %u,%u,%u,%u\n", ctl[i][0], ctl[i][1],
             classify(ctl[i][0], ctl[i][1]), p[0], p[1], p[2], p[3]);
   }
   printf("draw_test: checked %u inside + %u outside pixels, %u edge pixels excluded "
          "(margin %.1f px), %u mismatches\n",
          inside, outside, edge, MARGIN, bad);
   if (ppm && write_ppm(ppm, px) == 0)
      printf("draw_test: readback written to %s\n", ppm);
   const int status = bad ? 1 : 0;
   printf("draw_test: readback %s\n", bad ? "FAIL" : "PASS");

   /* ---- optional presentation, after the checked readback only ---- */
   int present_status = 0;
   if (present) {
      if (status != 0)
         printf("draw_test: present SKIPPED (readback failed)\n");
      else if (present_skip)
         printf("draw_test: present SKIPPED (%s)\n", present_skip);
      else
         present_status = present_step(&c, frames);
   }

   rc_vkDestroyFence(dev, fence, NULL);
   rc_vkDestroyCommandPool(dev, c.pool, NULL);
   rc_vkDestroyPipeline(dev, pipe, NULL);
   rc_vkDestroyShaderModule(dev, vs, NULL);
   rc_vkDestroyShaderModule(dev, fs, NULL);
   rc_vkDestroyPipelineLayout(dev, pl, NULL);
   rc_vkDestroyDescriptorPool(dev, dpool, NULL);
   rc_vkDestroyDescriptorSetLayout(dev, dsl, NULL);
   rc_vkDestroyFramebuffer(dev, fb, NULL);
   rc_vkDestroyRenderPass(dev, rp, NULL);
   rc_vkDestroyImageView(dev, view, NULL);
   rc_vkDestroyImage(dev, c.image, NULL);
   rc_vkFreeMemory(dev, imem, NULL);
   destroy_buffer(dev, &vb);
   destroy_buffer(dev, &ib);
   destroy_buffer(dev, &ub);
   destroy_buffer(dev, &rb);
   rc_vkDestroyDevice(dev, NULL);
   if (c.surface)
      rc_vkDestroySurfaceKHR(c.instance, c.surface, NULL);
   rc_vkDestroyInstance(c.instance, NULL);
   if (status)
      return status;
   return present_status ? 3 : 0;
}

/* FIFO swapchain on the display-plane surface; each frame clears the image
 * and copies the checked 64x64 image into its corner. */
static int
present_step(struct ctx *c, uint32_t frames)
{
   VkDevice dev = c->dev;
   if (rcvk_load_swapchain(dev))
      return 3;
   VkSurfaceCapabilitiesKHR caps;
   CHECK(rc_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->pd, c->surface, &caps));
   uint32_t nf = 0;
   CHECK(rc_vkGetPhysicalDeviceSurfaceFormatsKHR(c->pd, c->surface, &nf, NULL));
   VkSurfaceFormatKHR *sf = calloc(nf ? nf : 1, sizeof *sf);
   CHECK(rc_vkGetPhysicalDeviceSurfaceFormatsKHR(c->pd, c->surface, &nf, sf));
   VkSurfaceFormatKHR chosen = sf[0];
   for (uint32_t i = 0; i < nf; i++)
      if (sf[i].format == VK_FORMAT_B8G8R8A8_UNORM)
         chosen = sf[i];
   free(sf);
   if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      printf("draw_test: present SKIPPED (surface images lack TRANSFER_DST usage)\n");
      return 0;
   }
   VkExtent2D extent = caps.currentExtent;
   if (extent.width < W || extent.height < H) {
      printf("draw_test: present SKIPPED (surface smaller than 64x64)\n");
      return 0;
   }
   VkSwapchainCreateInfoKHR scci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = c->surface,
      .minImageCount = caps.minImageCount,
      .imageFormat = chosen.format,
      .imageColorSpace = chosen.colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR, /* the only mode required everywhere */
      .clipped = VK_TRUE,
   };
   VkSwapchainKHR swapchain;
   CHECK(rc_vkCreateSwapchainKHR(dev, &scci, NULL, &swapchain));
   uint32_t ni = 0;
   CHECK(rc_vkGetSwapchainImagesKHR(dev, swapchain, &ni, NULL));
   VkImage *images = calloc(ni, sizeof *images);
   CHECK(rc_vkGetSwapchainImagesKHR(dev, swapchain, &ni, images));

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = c->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(rc_vkAllocateCommandBuffers(dev, &cbai, &cmd));
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore acquired, done;
   CHECK(rc_vkCreateSemaphore(dev, &sci, NULL, &acquired));
   CHECK(rc_vkCreateSemaphore(dev, &sci, NULL, &done));
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK(rc_vkCreateFence(dev, &fci, NULL, &fence));

   uint32_t presented = 0;
   for (uint32_t f = 0; f < frames; f++) {
      uint32_t idx;
      CHECK(rc_vkAcquireNextImageKHR(dev, swapchain, UINT64_C(1000000000), acquired,
                                     VK_NULL_HANDLE, &idx));
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CHECK(rc_vkBeginCommandBuffer(cmd, &bi));
      VkImageMemoryBarrier to_dst = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = 0,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = images[idx],
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      rc_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 0, NULL, 1, &to_dst);
      VkClearColorValue grey = {.float32 = {0.1f, 0.1f, 0.1f, 1.0f}};
      VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      rc_vkCmdClearColorImage(cmd, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &grey, 1,
                              &all);
      VkMemoryBarrier clear_then_copy = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
      rc_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 1, &clear_then_copy, 0, NULL, 0, NULL);
      VkImageCopy ic = {
         .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .extent = {W, H, 1},
      };
      rc_vkCmdCopyImage(cmd, c->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[idx],
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
      VkImageMemoryBarrier to_present = to_dst;
      to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_present.dstAccessMask = 0;
      to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      rc_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                              &to_present);
      CHECK(rc_vkEndCommandBuffer(cmd));
      VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo si = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &acquired,
         .pWaitDstStageMask = &wait_stage,
         .commandBufferCount = 1,
         .pCommandBuffers = &cmd,
         .signalSemaphoreCount = 1,
         .pSignalSemaphores = &done,
      };
      CHECK(rc_vkQueueSubmit(c->queue, 1, &si, fence));
      VkPresentInfoKHR pi = {
         .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &done,
         .swapchainCount = 1,
         .pSwapchains = &swapchain,
         .pImageIndices = &idx,
      };
      CHECK(rc_vkQueuePresentKHR(c->queue, &pi));
      /* One command buffer, reused: wait for it before re-recording. */
      CHECK(rc_vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
      CHECK(rc_vkResetFences(dev, 1, &fence));
      presented++;
   }
   CHECK(rc_vkDeviceWaitIdle(dev));
   printf("draw_test: present completed (%u frames presented, format %d; screen content NOT checked, "
          "no readback of the display)\n",
          presented, (int)chosen.format);
   rc_vkDestroyFence(dev, fence, NULL);
   rc_vkDestroySemaphore(dev, acquired, NULL);
   rc_vkDestroySemaphore(dev, done, NULL);
   rc_vkDestroySwapchainKHR(dev, swapchain, NULL);
   free(images);
   return 0;
}
