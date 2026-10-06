/*
 * R-comp - M5 graphics slice: offscreen Vulkan 1.0 renderer behind the
 * guest render calls of include/rcomp/gfx.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Derived from gpu/vulkan/draw_test/draw_test.c (same contract: classic
 * render pass, one R8G8B8A8_UNORM target, vertex buffer, UBO colour, explicit
 * barriers, fence, copy to a HOST_VISIBLE buffer). Differences: several draws
 * per frame (one UBO + descriptor set each, vkCmdDraw on a range of one
 * vertex buffer), and the frame is rendered only at end_frame.
 * No extension, no optional feature, no dynamic state.
 */
#include "rcomp/gfx.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw_test_spirv.h"
#include "rcvk_loader.h"

#define MAX_DRAWS 16
#define MAX_VERTS (3 * 64)

struct hbuf {
   VkBuffer buffer;
   VkDeviceMemory memory;
   bool coherent;
   void *map;
};

struct rcomp_gfx {
   uint32_t w, h;
   VkInstance instance;
   VkPhysicalDevice pd;
   VkPhysicalDeviceMemoryProperties mp;
   VkPhysicalDeviceProperties props;
   VkDevice dev;
   VkQueue queue;
   uint32_t qf;
   VkImage image;
   VkDeviceMemory imem;
   VkImageView view;
   VkRenderPass rp;
   VkFramebuffer fb;
   VkDescriptorSetLayout dsl;
   VkDescriptorPool dpool;
   VkDescriptorSet ds[MAX_DRAWS];
   struct hbuf ub[MAX_DRAWS];
   struct hbuf vb, rb;
   VkPipelineLayout pl;
   VkShaderModule vs, fs;
   VkPipeline pipe;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkFence fence;
   /* frame state */
   bool in_frame;
   float clear[4];
   uint32_t ndraws, nverts;
   uint32_t first[MAX_DRAWS], count[MAX_DRAWS];
};

#define VK(call)                                                                                   \
   do {                                                                                            \
      VkResult r_ = (call);                                                                        \
      if (r_ != VK_SUCCESS) {                                                                      \
         fprintf(stderr, "rcomp_gfx: %s -> %s (%d)\n", #call, rcvk_result_name(r_), (int)r_);     \
         return RCOMP_GFX_VULKAN_ERROR;                                                            \
      }                                                                                            \
   } while (0)

static int
find_memory(const rcomp_gfx *g, uint32_t bits, VkMemoryPropertyFlags want, uint32_t *index)
{
   for (uint32_t i = 0; i < g->mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (g->mp.memoryTypes[i].propertyFlags & want) == want) {
         *index = i;
         return 0;
      }
   return -1;
}

static int
make_hbuf(rcomp_gfx *g, VkDeviceSize size, VkBufferUsageFlags usage, struct hbuf *b)
{
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VK(rc_vkCreateBuffer(g->dev, &bci, NULL, &b->buffer));
   VkMemoryRequirements req;
   rc_vkGetBufferMemoryRequirements(g->dev, b->buffer, &req);
   uint32_t type;
   if (find_memory(g, req.memoryTypeBits,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &type) == 0)
      b->coherent = true;
   else if (find_memory(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &type) == 0)
      b->coherent = false;
   else {
      fprintf(stderr, "rcomp_gfx: no HOST_VISIBLE memory type\n");
      return RCOMP_GFX_VULKAN_ERROR;
   }
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = type};
   VK(rc_vkAllocateMemory(g->dev, &mai, NULL, &b->memory));
   VK(rc_vkBindBufferMemory(g->dev, b->buffer, b->memory, 0));
   VK(rc_vkMapMemory(g->dev, b->memory, 0, VK_WHOLE_SIZE, 0, &b->map));
   return 0;
}

static int
flush_hbuf(rcomp_gfx *g, const struct hbuf *b)
{
   if (b->coherent)
      return 0;
   VkMappedMemoryRange r = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                            .memory = b->memory,
                            .size = VK_WHOLE_SIZE};
   VK(rc_vkFlushMappedMemoryRanges(g->dev, 1, &r));
   return 0;
}

static void
free_hbuf(rcomp_gfx *g, struct hbuf *b)
{
   if (b->map)
      rc_vkUnmapMemory(g->dev, b->memory);
   if (b->buffer)
      rc_vkDestroyBuffer(g->dev, b->buffer, NULL);
   if (b->memory)
      rc_vkFreeMemory(g->dev, b->memory, NULL);
   memset(b, 0, sizeof *b);
}

static VkShaderModule
module(rcomp_gfx *g, const uint32_t *code, size_t bytes)
{
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                  .codeSize = bytes,
                                  .pCode = code};
   VkShaderModule m = VK_NULL_HANDLE;
   VkResult r = rc_vkCreateShaderModule(g->dev, &ci, NULL, &m);
   if (r != VK_SUCCESS)
      fprintf(stderr, "rcomp_gfx: vkCreateShaderModule -> %s\n", rcvk_result_name(r));
   return m;
}

static int
setup(rcomp_gfx *g)
{
   if (rcvk_load_global())
      return RCOMP_GFX_VULKAN_ERROR;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "rcomp_m5",
                            .pEngineName = "R-comp",
                            .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app};
   VK(rc_vkCreateInstance(&ici, NULL, &g->instance));
   if (rcvk_load_instance(g->instance))
      return RCOMP_GFX_VULKAN_ERROR;
   uint32_t npd = 1;
   VkResult r = rc_vkEnumeratePhysicalDevices(g->instance, &npd, &g->pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || npd == 0) {
      fprintf(stderr, "rcomp_gfx: no physical device\n");
      return RCOMP_GFX_VULKAN_ERROR;
   }
   rc_vkGetPhysicalDeviceProperties(g->pd, &g->props);
   rc_vkGetPhysicalDeviceMemoryProperties(g->pd, &g->mp);
   uint32_t nq = 0;
   rc_vkGetPhysicalDeviceQueueFamilyProperties(g->pd, &nq, NULL);
   VkQueueFamilyProperties qfp[16];
   if (nq > 16)
      nq = 16;
   rc_vkGetPhysicalDeviceQueueFamilyProperties(g->pd, &nq, qfp);
   g->qf = UINT32_MAX;
   for (uint32_t i = 0; i < nq && g->qf == UINT32_MAX; i++)
      if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
         g->qf = i;
   if (g->qf == UINT32_MAX) {
      fprintf(stderr, "rcomp_gfx: no graphics queue\n");
      return RCOMP_GFX_VULKAN_ERROR;
   }
   const float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = g->qf,
                                  .queueCount = 1,
                                  .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qci};
   VK(rc_vkCreateDevice(g->pd, &dci, NULL, &g->dev));
   if (rcvk_load_device(g->dev))
      return RCOMP_GFX_VULKAN_ERROR;
   rc_vkGetDeviceQueue(g->dev, g->qf, 0, &g->queue);

   int e;
   if ((e = make_hbuf(g, MAX_VERTS * 2 * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &g->vb)) ||
       (e = make_hbuf(g, (VkDeviceSize)g->w * g->h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g->rb)))
      return e;
   for (int i = 0; i < MAX_DRAWS; i++)
      if ((e = make_hbuf(g, 4 * sizeof(float), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &g->ub[i])))
         return e;

   VkImageCreateInfo imci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM,
                             .extent = {g->w, g->h, 1},
                             .mipLevels = 1,
                             .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VK(rc_vkCreateImage(g->dev, &imci, NULL, &g->image));
   VkMemoryRequirements ireq;
   rc_vkGetImageMemoryRequirements(g->dev, g->image, &ireq);
   uint32_t itype;
   if (find_memory(g, ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &itype) &&
       find_memory(g, ireq.memoryTypeBits, 0, &itype)) {
      fprintf(stderr, "rcomp_gfx: no memory type for the image\n");
      return RCOMP_GFX_VULKAN_ERROR;
   }
   VkMemoryAllocateInfo imai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = ireq.size,
                                .memoryTypeIndex = itype};
   VK(rc_vkAllocateMemory(g->dev, &imai, NULL, &g->imem));
   VK(rc_vkBindImageMemory(g->dev, g->image, g->imem, 0));
   VkImageViewCreateInfo ivci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .image = g->image,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VK(rc_vkCreateImageView(g->dev, &ivci, NULL, &g->view));

   VkAttachmentDescription att = {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                  .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                               .colorAttachmentCount = 1,
                               .pColorAttachments = &ref};
   VkSubpassDependency deps[2] = {
      {.srcSubpass = VK_SUBPASS_EXTERNAL,
       .dstSubpass = 0,
       .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
      {.srcSubpass = 0,
       .dstSubpass = VK_SUBPASS_EXTERNAL,
       .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
       .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
       .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT},
   };
   VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                  .attachmentCount = 1,
                                  .pAttachments = &att,
                                  .subpassCount = 1,
                                  .pSubpasses = &sub,
                                  .dependencyCount = 2,
                                  .pDependencies = deps};
   VK(rc_vkCreateRenderPass(g->dev, &rpci, NULL, &g->rp));
   VkFramebufferCreateInfo fbci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                   .renderPass = g->rp,
                                   .attachmentCount = 1,
                                   .pAttachments = &g->view,
                                   .width = g->w,
                                   .height = g->h,
                                   .layers = 1};
   VK(rc_vkCreateFramebuffer(g->dev, &fbci, NULL, &g->fb));

   VkDescriptorSetLayoutBinding binding = {.binding = 0,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .descriptorCount = 1,
                                           .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo dslci = {.sType =
                                               VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 1,
                                            .pBindings = &binding};
   VK(rc_vkCreateDescriptorSetLayout(g->dev, &dslci, NULL, &g->dsl));
   VkDescriptorPoolSize psize = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_DRAWS};
   VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = MAX_DRAWS,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &psize};
   VK(rc_vkCreateDescriptorPool(g->dev, &dpci, NULL, &g->dpool));
   VkDescriptorSetLayout layouts[MAX_DRAWS];
   for (int i = 0; i < MAX_DRAWS; i++)
      layouts[i] = g->dsl;
   VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = g->dpool,
                                       .descriptorSetCount = MAX_DRAWS,
                                       .pSetLayouts = layouts};
   VK(rc_vkAllocateDescriptorSets(g->dev, &dsai, g->ds));
   for (int i = 0; i < MAX_DRAWS; i++) {
      VkDescriptorBufferInfo dbi = {g->ub[i].buffer, 0, 4 * sizeof(float)};
      VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = g->ds[i],
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  .pBufferInfo = &dbi};
      rc_vkUpdateDescriptorSets(g->dev, 1, &wds, 0, NULL);
   }
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &g->dsl};
   VK(rc_vkCreatePipelineLayout(g->dev, &plci, NULL, &g->pl));

   g->vs = module(g, triangle_vert_spv, sizeof triangle_vert_spv);
   g->fs = module(g, triangle_frag_spv, sizeof triangle_frag_spv);
   if (!g->vs || !g->fs)
      return RCOMP_GFX_VULKAN_ERROR;
   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = g->vs,
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = g->fs,
       .pName = "main"},
   };
   VkVertexInputBindingDescription vbind = {0, 2 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription vattr = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &vbind,
      .vertexAttributeDescriptionCount = 1,
      .pVertexAttributeDescriptions = &vattr};
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0.0f, 0.0f, (float)g->w, (float)g->h, 0.0f, 1.0f};
   VkRect2D sc = {{0, 0}, {g->w, g->h}};
   VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &vp,
      .scissorCount = 1,
      .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &cba};
   VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        .stageCount = 2,
                                        .pStages = stages,
                                        .pVertexInputState = &vi,
                                        .pInputAssemblyState = &ia,
                                        .pViewportState = &vps,
                                        .pRasterizationState = &rs,
                                        .pMultisampleState = &ms,
                                        .pColorBlendState = &cb,
                                        .layout = g->pl,
                                        .renderPass = g->rp};
   VK(rc_vkCreateGraphicsPipelines(g->dev, VK_NULL_HANDLE, 1, &gpci, NULL, &g->pipe));

   VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g->qf};
   VK(rc_vkCreateCommandPool(g->dev, &cpci, NULL, &g->pool));
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = g->pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
   VK(rc_vkAllocateCommandBuffers(g->dev, &cbai, &g->cmd));
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VK(rc_vkCreateFence(g->dev, &fci, NULL, &g->fence));
   return RCOMP_GFX_OK;
}

int
rcomp_gfx_create(uint32_t width, uint32_t height, rcomp_gfx **out)
{
   if (!out || width == 0 || height == 0 || width > 256 || height > 256)
      return RCOMP_GFX_BAD_ARGUMENT;
   rcomp_gfx *g = calloc(1, sizeof *g);
   if (!g)
      return RCOMP_GFX_VULKAN_ERROR;
   g->w = width;
   g->h = height;
   int e = setup(g);
   if (e) {
      rcomp_gfx_destroy(g);
      return e;
   }
   *out = g;
   return RCOMP_GFX_OK;
}

void
rcomp_gfx_destroy(rcomp_gfx *g)
{
   if (!g)
      return;
   if (g->dev) {
      rc_vkDeviceWaitIdle(g->dev);
      if (g->fence) rc_vkDestroyFence(g->dev, g->fence, NULL);
      if (g->pool) rc_vkDestroyCommandPool(g->dev, g->pool, NULL);
      if (g->pipe) rc_vkDestroyPipeline(g->dev, g->pipe, NULL);
      if (g->vs) rc_vkDestroyShaderModule(g->dev, g->vs, NULL);
      if (g->fs) rc_vkDestroyShaderModule(g->dev, g->fs, NULL);
      if (g->pl) rc_vkDestroyPipelineLayout(g->dev, g->pl, NULL);
      if (g->dpool) rc_vkDestroyDescriptorPool(g->dev, g->dpool, NULL);
      if (g->dsl) rc_vkDestroyDescriptorSetLayout(g->dev, g->dsl, NULL);
      if (g->fb) rc_vkDestroyFramebuffer(g->dev, g->fb, NULL);
      if (g->rp) rc_vkDestroyRenderPass(g->dev, g->rp, NULL);
      if (g->view) rc_vkDestroyImageView(g->dev, g->view, NULL);
      if (g->image) rc_vkDestroyImage(g->dev, g->image, NULL);
      if (g->imem) rc_vkFreeMemory(g->dev, g->imem, NULL);
      free_hbuf(g, &g->vb);
      free_hbuf(g, &g->rb);
      for (int i = 0; i < MAX_DRAWS; i++)
         free_hbuf(g, &g->ub[i]);
      rc_vkDestroyDevice(g->dev, NULL);
   }
   if (g->instance)
      rc_vkDestroyInstance(g->instance, NULL);
   free(g);
}

const char *
rcomp_gfx_device_name(const rcomp_gfx *g)
{
   return g ? g->props.deviceName : "";
}

int
rcomp_gfx_begin_frame(rcomp_gfx *g, const float clear_rgba[4])
{
   if (!g)
      return RCOMP_GFX_NOT_INITIALISED;
   if (g->in_frame)
      return RCOMP_GFX_BAD_STATE;
   memcpy(g->clear, clear_rgba, sizeof g->clear);
   g->ndraws = g->nverts = 0;
   g->in_frame = true;
   return RCOMP_GFX_OK;
}

int
rcomp_gfx_draw(rcomp_gfx *g, const float *xy, uint32_t n, const float rgba[4])
{
   if (!g)
      return RCOMP_GFX_NOT_INITIALISED;
   if (!g->in_frame)
      return RCOMP_GFX_BAD_STATE;
   if (n == 0 || n % 3 || n > MAX_VERTS - g->nverts)
      return RCOMP_GFX_BAD_ARGUMENT;
   if (g->ndraws == MAX_DRAWS)
      return RCOMP_GFX_TOO_MANY_DRAWS;
   memcpy((float *)g->vb.map + 2 * g->nverts, xy, n * 2 * sizeof(float));
   memcpy(g->ub[g->ndraws].map, rgba, 4 * sizeof(float));
   g->first[g->ndraws] = g->nverts;
   g->count[g->ndraws] = n;
   g->nverts += n;
   g->ndraws++;
   return RCOMP_GFX_OK;
}

int
rcomp_gfx_end_frame(rcomp_gfx *g, uint8_t *rgba_out)
{
   if (!g)
      return RCOMP_GFX_NOT_INITIALISED;
   if (!g->in_frame)
      return RCOMP_GFX_BAD_STATE;
   g->in_frame = false;
   int e;
   if ((e = flush_hbuf(g, &g->vb)))
      return e;
   for (uint32_t i = 0; i < g->ndraws; i++)
      if ((e = flush_hbuf(g, &g->ub[i])))
         return e;
   memset(g->rb.map, 0xA5, (size_t)g->w * g->h * 4); /* an unperformed copy cannot pass */
   if ((e = flush_hbuf(g, &g->rb)))
      return e;

   VK(rc_vkResetCommandBuffer(g->cmd, 0));
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VK(rc_vkBeginCommandBuffer(g->cmd, &bi));
   VkMemoryBarrier host_to_gpu = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                                                   VK_ACCESS_UNIFORM_READ_BIT};
   rc_vkCmdPipelineBarrier(g->cmd, VK_PIPELINE_STAGE_HOST_BIT,
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 1, &host_to_gpu, 0, NULL, 0, NULL);
   VkClearValue clear;
   memcpy(clear.color.float32, g->clear, sizeof g->clear);
   VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                 .renderPass = g->rp,
                                 .framebuffer = g->fb,
                                 .renderArea = {{0, 0}, {g->w, g->h}},
                                 .clearValueCount = 1,
                                 .pClearValues = &clear};
   rc_vkCmdBeginRenderPass(g->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   if (g->ndraws) {
      rc_vkCmdBindPipeline(g->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipe);
      VkDeviceSize zero = 0;
      rc_vkCmdBindVertexBuffers(g->cmd, 0, 1, &g->vb.buffer, &zero);
      for (uint32_t i = 0; i < g->ndraws; i++) {
         rc_vkCmdBindDescriptorSets(g->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pl, 0, 1, &g->ds[i],
                                    0, NULL);
         rc_vkCmdDraw(g->cmd, g->count[i], 1, g->first[i], 0);
      }
   }
   rc_vkCmdEndRenderPass(g->cmd);
   VkImageMemoryBarrier to_src = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = g->image,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   rc_vkCmdPipelineBarrier(g->cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
   VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {g->w, g->h, 1}};
   rc_vkCmdCopyImageToBuffer(g->cmd, g->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->rb.buffer, 1,
                             &region);
   VkBufferMemoryBarrier to_host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .buffer = g->rb.buffer,
                                    .size = VK_WHOLE_SIZE};
   rc_vkCmdPipelineBarrier(g->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                           NULL, 1, &to_host, 0, NULL);
   VK(rc_vkEndCommandBuffer(g->cmd));
   VK(rc_vkResetFences(g->dev, 1, &g->fence));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .commandBufferCount = 1,
                      .pCommandBuffers = &g->cmd};
   VK(rc_vkQueueSubmit(g->queue, 1, &si, g->fence));
   VK(rc_vkWaitForFences(g->dev, 1, &g->fence, VK_TRUE, UINT64_C(5000000000)));
   if (!g->rb.coherent) {
      VkMappedMemoryRange mr = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                .memory = g->rb.memory,
                                .size = VK_WHOLE_SIZE};
      VK(rc_vkInvalidateMappedMemoryRanges(g->dev, 1, &mr));
   }
   memcpy(rgba_out, g->rb.map, (size_t)g->w * g->h * 4);
   return RCOMP_GFX_OK;
}
