// Host GPU bench of the resolution-scaled texture load shaders: bit-exactness of a variant against the current shader and its speed.
//
// usage: load_bench <device index> <width_texels> <height_texels> <scale> <bpb_log2 2|3> <convert 0|1|2> <reference.spv> <variant.spv> <variant_group_x_blocks> <variant_rows_per_group> <iterations>
//
// The source is a pseudo-random "scaled resolve buffer" of the guest layout (rexglue's scaled layout, tiled, endian swap 2); the destination is a
// storage image R32_UINT (32 bits per block) or R32G32_UINT (64 bits). Both shaders get the same push constants and the same dispatch rounding
// (the reference: 32 blocks x 32 rows a group, 16 x 32 for 64 bpb; the variant: as given), the images are compared texel by texel, then each is timed
// over <iterations> back-to-back dispatches with a barrier between them (timestamp queries).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define VKCHECK(call)                                                                              \
  do {                                                                                             \
    VkResult r_ = (call);                                                                          \
    if (r_ != VK_SUCCESS) {                                                                        \
      std::fprintf(stderr, "%s:%d: %s failed: %d\n", __FILE__, __LINE__, #call, int(r_));          \
      std::exit(2);                                                                                \
    }                                                                                              \
  } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
#define INSTANCE_FN(name) static PFN_##name name;
#define DEVICE_FN(name) static PFN_##name name;
INSTANCE_FN(vkCreateInstance)
INSTANCE_FN(vkEnumeratePhysicalDevices)
INSTANCE_FN(vkGetPhysicalDeviceProperties)
INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties)
INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties)
INSTANCE_FN(vkCreateDevice)
INSTANCE_FN(vkGetDeviceProcAddr)
DEVICE_FN(vkGetDeviceQueue)
DEVICE_FN(vkCreateBuffer)
DEVICE_FN(vkGetBufferMemoryRequirements)
DEVICE_FN(vkAllocateMemory)
DEVICE_FN(vkBindBufferMemory)
DEVICE_FN(vkMapMemory)
DEVICE_FN(vkCreateImage)
DEVICE_FN(vkGetImageMemoryRequirements)
DEVICE_FN(vkBindImageMemory)
DEVICE_FN(vkCreateImageView)
DEVICE_FN(vkCreateShaderModule)
DEVICE_FN(vkCreateDescriptorSetLayout)
DEVICE_FN(vkCreatePipelineLayout)
DEVICE_FN(vkCreateComputePipelines)
DEVICE_FN(vkCreateDescriptorPool)
DEVICE_FN(vkAllocateDescriptorSets)
DEVICE_FN(vkUpdateDescriptorSets)
DEVICE_FN(vkCreateCommandPool)
DEVICE_FN(vkAllocateCommandBuffers)
DEVICE_FN(vkBeginCommandBuffer)
DEVICE_FN(vkEndCommandBuffer)
DEVICE_FN(vkResetCommandBuffer)
DEVICE_FN(vkCmdBindPipeline)
DEVICE_FN(vkCmdBindDescriptorSets)
DEVICE_FN(vkCmdPushConstants)
DEVICE_FN(vkCmdDispatch)
DEVICE_FN(vkCmdPipelineBarrier)
DEVICE_FN(vkCmdCopyImageToBuffer)
DEVICE_FN(vkCmdCopyBuffer)
DEVICE_FN(vkCmdClearColorImage)
DEVICE_FN(vkQueueSubmit)
DEVICE_FN(vkQueueWaitIdle)
DEVICE_FN(vkCreateQueryPool)
DEVICE_FN(vkCmdResetQueryPool)
DEVICE_FN(vkCmdWriteTimestamp)
DEVICE_FN(vkGetQueryPoolResults)

static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice device;
static VkQueue queue;
static uint32_t family;
static VkPhysicalDeviceMemoryProperties mem_props;
static float timestamp_period_ns = 1.0f;

static uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) {
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want &&
        !(mem_props.memoryTypes[i].propertyFlags & avoid)) {
      return i;
    }
  }
  std::fprintf(stderr, "no memory type\n");
  std::exit(2);
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  uint8_t* mapped = nullptr;
};

static Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
  Buffer b;
  b.size = size;
  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VKCHECK(vkCreateBuffer(device, &info, nullptr, &b.buffer));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device, b.buffer, &req);
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = host_visible
                              ? FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                              : FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKCHECK(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
  VKCHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
  if (host_visible) {
    void* p;
    VKCHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &p));
    b.mapped = static_cast<uint8_t*>(p);
  }
  return b;
}

static std::vector<uint32_t> LoadSpirv(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path);
    std::exit(2);
  }
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint32_t> data(size / 4);
  if (std::fread(data.data(), 1, size, f) != size_t(size)) std::exit(2);
  std::fclose(f);
  return data;
}

struct LoadConstants {
  uint32_t is_tiled_3d_endian_scale;
  uint32_t guest_offset;
  uint32_t guest_pitch_aligned;
  uint32_t guest_z_stride_block_rows_aligned;
  uint32_t size_blocks[3];
  uint32_t host_row_offset;
  uint32_t host_pitch;
  uint32_t height_texels;
};

int main(int argc, char** argv) {
  if (argc < 12) {
    std::fprintf(stderr, "usage: load_bench dev width height scale bpb_log2 convert ref.spv variant.spv variant_group_x_blocks variant_rows iterations\n");
    return 1;
  }
  const uint32_t dev_index = std::atoi(argv[1]);
  const uint32_t width = std::atoi(argv[2]), height = std::atoi(argv[3]), scale = std::atoi(argv[4]);
  const uint32_t bpb_log2 = std::atoi(argv[5]);
  const char* ref_path = argv[7];
  const char* var_path = argv[8];
  const uint32_t var_blocks_x = std::atoi(argv[9]), var_rows = std::atoi(argv[10]);
  const uint32_t iterations = std::atoi(argv[11]);

  HMODULE lib = LoadLibraryA("vulkan-1.dll");
  if (!lib) {
    std::fprintf(stderr, "no vulkan-1.dll\n");
    return 2;
  }
  gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
  vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VKCHECK(vkCreateInstance(&ici, nullptr, &instance));
#define LOAD_I(name) name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
  LOAD_I(vkEnumeratePhysicalDevices);
  LOAD_I(vkGetPhysicalDeviceProperties);
  LOAD_I(vkGetPhysicalDeviceMemoryProperties);
  LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties);
  LOAD_I(vkCreateDevice);
  LOAD_I(vkGetDeviceProcAddr);
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  if (dev_index >= count) {
    std::fprintf(stderr, "device index out of range (%u devices)\n", count);
    return 2;
  }
  phys = devices[dev_index];
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(phys, &props);
  timestamp_period_ns = props.limits.timestampPeriod;
  std::printf("device %u: %s (timestamp period %.3f ns)\n", dev_index, props.deviceName, timestamp_period_ns);
  vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
  uint32_t qcount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, nullptr);
  std::vector<VkQueueFamilyProperties> qprops(qcount);
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, qprops.data());
  family = UINT32_MAX;
  for (uint32_t i = 0; i < qcount; ++i) {
    if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      family = i;
      break;
    }
  }
  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VKCHECK(vkCreateDevice(phys, &dci, nullptr, &device));
#define LOAD_D(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))
  LOAD_D(vkGetDeviceQueue); LOAD_D(vkCreateBuffer); LOAD_D(vkGetBufferMemoryRequirements); LOAD_D(vkAllocateMemory);
  LOAD_D(vkBindBufferMemory); LOAD_D(vkMapMemory); LOAD_D(vkCreateImage); LOAD_D(vkGetImageMemoryRequirements);
  LOAD_D(vkBindImageMemory); LOAD_D(vkCreateImageView); LOAD_D(vkCreateShaderModule); LOAD_D(vkCreateDescriptorSetLayout);
  LOAD_D(vkCreatePipelineLayout); LOAD_D(vkCreateComputePipelines); LOAD_D(vkCreateDescriptorPool);
  LOAD_D(vkAllocateDescriptorSets); LOAD_D(vkUpdateDescriptorSets); LOAD_D(vkCreateCommandPool);
  LOAD_D(vkAllocateCommandBuffers); LOAD_D(vkBeginCommandBuffer); LOAD_D(vkEndCommandBuffer); LOAD_D(vkResetCommandBuffer);
  LOAD_D(vkCmdBindPipeline); LOAD_D(vkCmdBindDescriptorSets); LOAD_D(vkCmdPushConstants); LOAD_D(vkCmdDispatch);
  LOAD_D(vkCmdPipelineBarrier); LOAD_D(vkCmdCopyImageToBuffer); LOAD_D(vkCmdCopyBuffer); LOAD_D(vkCmdClearColorImage); LOAD_D(vkQueueSubmit);
  LOAD_D(vkQueueWaitIdle); LOAD_D(vkCreateQueryPool); LOAD_D(vkCmdResetQueryPool); LOAD_D(vkCmdWriteTimestamp);
  LOAD_D(vkGetQueryPoolResults);
  vkGetDeviceQueue(device, family, 0, &queue);

  // ---- geometry: a guest texture of width x height blocks (32 or 64 bits per block), tiled, scaled scale x scale
  const uint32_t bpb = 1u << bpb_log2;
  const uint32_t guest_pitch_blocks = (width + 31) & ~31u;
  const uint32_t guest_rows_aligned = (height + 31) & ~31u;
  const uint64_t guest_bytes = uint64_t(guest_pitch_blocks) * guest_rows_aligned * bpb;
  const uint64_t source_bytes = guest_bytes * scale * scale;
  const uint32_t host_w = width * scale, host_h = height * scale;
  std::printf("guest %ux%u (pitch %u, %u bpb) scale %u -> host %ux%u, source %.1f MB, image %.1f MB\n", width, height,
              guest_pitch_blocks, bpb * 8, scale, host_w, host_h, source_bytes / 1e6, host_w * host_h * double(bpb) / 1e6);

  Buffer staging = MakeBuffer(source_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
  Buffer source = MakeBuffer(source_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
  {
    uint32_t* p = reinterpret_cast<uint32_t*>(staging.mapped);
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (uint64_t i = 0; i < source_bytes / 4; ++i) {
      s ^= s << 13; s ^= s >> 7; s ^= s << 17;
      p[i] = uint32_t(s >> 16);
    }
  }
  const VkFormat image_format = bpb_log2 == 2 ? VK_FORMAT_R32_UINT : VK_FORMAT_R32G32_UINT;
  struct Image { VkImage image; VkDeviceMemory memory; VkImageView view; };
  auto make_image = [&]() {
    Image im;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = image_format;
    ii.extent = {host_w, host_h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(device, &ii, nullptr, &im.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, im.image, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkAllocateMemory(device, &alloc, nullptr, &im.memory));
    VKCHECK(vkBindImageMemory(device, im.image, im.memory, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = image_format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKCHECK(vkCreateImageView(device, &vi, nullptr, &im.view));
    return im;
  };
  Image image_ref = make_image(), image_var = make_image();
  const VkDeviceSize image_bytes = VkDeviceSize(host_w) * host_h * bpb;
  Buffer readback_ref = MakeBuffer(image_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  Buffer readback_var = MakeBuffer(image_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);

  // ---- layouts, pipelines
  VkDescriptorSetLayoutBinding image_binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutBinding buffer_binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dsl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dsl.bindingCount = 1;
  VkDescriptorSetLayout layouts[2];
  dsl.pBindings = &image_binding;
  VKCHECK(vkCreateDescriptorSetLayout(device, &dsl, nullptr, &layouts[0]));
  dsl.pBindings = &buffer_binding;
  VKCHECK(vkCreateDescriptorSetLayout(device, &dsl, nullptr, &layouts[1]));
  VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LoadConstants)};
  VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 2;
  plci.pSetLayouts = layouts;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  VkPipelineLayout pipeline_layout;
  VKCHECK(vkCreatePipelineLayout(device, &plci, nullptr, &pipeline_layout));
  auto make_pipeline = [&](const char* path) {
    std::vector<uint32_t> code = LoadSpirv(path);
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = code.size() * 4;
    smci.pCode = code.data();
    VkShaderModule module;
    VKCHECK(vkCreateShaderModule(device, &smci, nullptr, &module));
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    cpci.layout = pipeline_layout;
    VkPipeline pipeline;
    VKCHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));
    return pipeline;
  };
  VkPipeline pipeline_ref = make_pipeline(ref_path), pipeline_var = make_pipeline(var_path);

  VkDescriptorPoolSize pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
  VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 3;
  dpci.poolSizeCount = 2;
  dpci.pPoolSizes = pool_sizes;
  VkDescriptorPool pool;
  VKCHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));
  VkDescriptorSet set_image_ref, set_image_var, set_source;
  auto alloc_set = [&](VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &layout;
    VkDescriptorSet set;
    VKCHECK(vkAllocateDescriptorSets(device, &dsai, &set));
    return set;
  };
  set_image_ref = alloc_set(layouts[0]);
  set_image_var = alloc_set(layouts[0]);
  set_source = alloc_set(layouts[1]);
  VkDescriptorImageInfo ii_ref = {VK_NULL_HANDLE, image_ref.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo ii_var = {VK_NULL_HANDLE, image_var.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorBufferInfo bi = {source.buffer, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet writes[3] = {};
  writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set_image_ref, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii_ref, nullptr, nullptr};
  writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set_image_var, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii_var, nullptr, nullptr};
  writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set_source, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi, nullptr};
  vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

  VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = family;
  VkCommandPool cmd_pool;
  VKCHECK(vkCreateCommandPool(device, &cpi, nullptr, &cmd_pool));
  VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = cmd_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  VKCHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));
  VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qpi.queryCount = 2;
  VkQueryPool qpool;
  VKCHECK(vkCreateQueryPool(device, &qpi, nullptr, &qpool));

  LoadConstants constants = {};
  const uint32_t endian = 2;
  constants.is_tiled_3d_endian_scale = 1u | (endian << 2) | (scale << 4) | (scale << 7);
  constants.guest_offset = 0;
  constants.guest_pitch_aligned = guest_pitch_blocks;
  constants.guest_z_stride_block_rows_aligned = guest_rows_aligned;
  constants.size_blocks[0] = host_w;
  constants.size_blocks[1] = host_h;
  constants.size_blocks[2] = 1;
  constants.host_row_offset = 0;
  constants.host_pitch = host_w;
  constants.height_texels = height;
  const uint32_t ref_blocks_x = bpb_log2 == 2 ? 32 : 16;
  // HARNESS_BAND="first,count" (guest rows): a band load as the texture cache issues it (guest offset and host row offset set, fewer rows)
  if (const char* band = std::getenv("HARNESS_BAND")) {
    unsigned first = 0, rows = 0;
    if (std::sscanf(band, "%u,%u", &first, &rows) == 2 && rows) {
      constants.guest_offset = first * guest_pitch_blocks * bpb * scale * scale;
      constants.size_blocks[1] = rows * scale;
      constants.host_row_offset = first * scale;
      std::printf("band: guest rows %u..%u, guest offset %u, host rows %u..%u\n", first, first + rows, constants.guest_offset,
                  constants.host_row_offset, constants.host_row_offset + constants.size_blocks[1]);
    }
  }

  auto barrier_image_general = [&](VkImage image, VkAccessFlags src, VkAccessFlags dst, VkImageLayout old_layout,
                                   VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = old_layout;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
  };
  auto run = [&](VkPipeline pipeline, VkDescriptorSet set_image, VkImage image, uint32_t group_blocks_x, uint32_t group_rows,
                 uint32_t repeats, bool timed, Buffer* readback) {
    VKCHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VKCHECK(vkBeginCommandBuffer(cmd, &cbbi));
    barrier_image_general(image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (readback) {
      VkClearColorValue zero = {};
      VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
      VkMemoryBarrier cb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &cb, 0, nullptr, 0, nullptr);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    VkDescriptorSet sets[2] = {set_image, set_source};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    if (timed) {
      vkCmdResetQueryPool(cmd, qpool, 0, 2);
    }
    // warm-up dispatch, then the timed ones
    const uint32_t gx = (constants.size_blocks[0] + group_blocks_x - 1) / group_blocks_x, gy = (constants.size_blocks[1] + group_rows - 1) / group_rows;
    vkCmdDispatch(cmd, gx, gy, 1);
    if (timed) {
      VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qpool, 0);
      for (uint32_t i = 0; i < repeats; ++i) {
        vkCmdDispatch(cmd, gx, gy, 1);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
      }
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qpool, 1);
    }
    if (readback) {
      barrier_image_general(image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      VkBufferImageCopy region = {};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {host_w, host_h, 1};
      // layout GENERAL is valid for transfer source
      vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, readback->buffer, 1, &region);
    }
    VKCHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VKCHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VKCHECK(vkQueueWaitIdle(queue));
    double us = 0;
    if (timed) {
      uint64_t t[2];
      VKCHECK(vkGetQueryPoolResults(device, qpool, 0, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
      us = double(t[1] - t[0]) * timestamp_period_ns / 1000.0 / repeats;
    }
    return us;
  };

  // upload the source once
  {
    VKCHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VKCHECK(vkBeginCommandBuffer(cmd, &cbbi));
    VkBufferCopy region = {0, 0, source_bytes};
    vkCmdCopyBuffer(cmd, staging.buffer, source.buffer, 1, &region);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VKCHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VKCHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VKCHECK(vkQueueWaitIdle(queue));
  }

  // correctness
  run(pipeline_ref, set_image_ref, image_ref.image, ref_blocks_x, 32, 0, false, &readback_ref);
  run(pipeline_var, set_image_var, image_var.image, var_blocks_x, var_rows, 0, false, &readback_var);
  const uint32_t* a = reinterpret_cast<const uint32_t*>(readback_ref.mapped);
  const uint32_t* b = reinterpret_cast<const uint32_t*>(readback_var.mapped);
  uint64_t mismatches = 0, first = UINT64_MAX;
  for (uint64_t i = 0; i < image_bytes / 4; ++i) {
    if (a[i] != b[i]) {
      if (first == UINT64_MAX) first = i;
      ++mismatches;
    }
  }
  if (mismatches) {
    std::printf("MISMATCH: %llu of %llu words differ, first at word %llu (texel %llu,%llu) ref %08X variant %08X\n",
                (unsigned long long)mismatches, (unsigned long long)(image_bytes / 4), (unsigned long long)first,
                (unsigned long long)((first / (bpb / 4)) % host_w), (unsigned long long)((first / (bpb / 4)) / host_w), a[first], b[first]);
  } else {
    std::printf("EQUAL: all %llu words match\n", (unsigned long long)(image_bytes / 4));
  }
  // an all-zero reference would make the comparison meaningless
  uint64_t nonzero = 0;
  for (uint64_t i = 0; i < image_bytes / 4; ++i) nonzero += a[i] != 0;
  std::printf("reference non-zero words: %llu of %llu\n", (unsigned long long)nonzero, (unsigned long long)(image_bytes / 4));

  // speed
  for (int rep = 0; rep < 3; ++rep) {
    double us_ref = run(pipeline_ref, set_image_ref, image_ref.image, ref_blocks_x, 32, iterations, true, nullptr);
    double us_var = run(pipeline_var, set_image_var, image_var.image, var_blocks_x, var_rows, iterations, true, nullptr);
    const double mb = (double(source_bytes) / (scale * scale) * 0 + double(image_bytes)) / 1e6;  // bytes written
    std::printf("reference %8.1f us   variant %8.1f us   ratio %.2f   (written %.1f MB: %.0f / %.0f GB/s)\n", us_ref, us_var,
                us_ref / us_var, mb, mb / us_ref * 1e-3 * 1e3, mb / us_var * 1e-3 * 1e3);
  }
  return mismatches ? 1 : 0;
}
