// Host GPU bench of the resolve copy shaders: bit-exactness of a transcription against the precompiled shader of the Xenos GPU subsystem.
//
// usage: resolve_bench <device index> <scale 1..3> <iterations> <original.spv> <variant.spv> [seed]
//
// The original is resolve_fast_32bpp_1x2xmsaa_scaled_cs (its SPIR-V is in the rexglue SDK, its source is not); the variant (copy_clone.comp) is a transcription of its
// disassembly. Both read a pseudo-random "EDRAM buffer" of the scaled layout (2,048 tiles of 80x16 samples, scale^2 host samples each) and write a destination buffer in the
// guest texture layout with the same push constants (edram_info, coordinate_info, dest_info, dest_coordinate_info), over random legal parameters: MSAA 1x/2x, color or depth,
// the 32-bit color formats, the sample select, endian swaps, the red/blue swap, 2D and 3D destinations, region and destination offsets, the duplicated second pixel. The
// destination buffers are zero-filled before each pair of dispatches and compared word for word afterwards.
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
DEVICE_FN(vkCmdFillBuffer)
DEVICE_FN(vkQueueSubmit)
DEVICE_FN(vkQueueWaitIdle)

static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice device;
static VkQueue queue;
static uint32_t family;
static VkPhysicalDeviceMemoryProperties mem_props;

static uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want) return i;
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

static Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
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
  alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VKCHECK(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
  VKCHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
  void* p;
  VKCHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &p));
  b.mapped = static_cast<uint8_t*>(p);
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

struct Rng {
  uint64_t s;
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  uint32_t below(uint32_t n) { return uint32_t((next() >> 11) % n); }
  uint32_t range(uint32_t lo, uint32_t hi) { return lo + below(hi - lo + 1); }  // inclusive
};

struct PushConstants {
  uint32_t edram_info, coordinate_info, dest_info, dest_coordinate_info;
};

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr, "usage: resolve_bench <device> <scale 1..3> <iterations> <original.spv> <variant.spv> [seed]\n");
    return 1;
  }
  const uint32_t dev_index = std::atoi(argv[1]);
  const uint32_t scale = std::atoi(argv[2]);
  const uint32_t iterations = std::atoi(argv[3]);
  const char* original_path = argv[4];
  const char* variant_path = argv[5];
  Rng rng{argc > 6 ? std::strtoull(argv[6], nullptr, 0) : 0x9E3779B97F4A7C15ull};
  if (scale < 1 || scale > 3) return 1;

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
  std::printf("device %u: %s\n", dev_index, props.deviceName);
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
  LOAD_D(vkBindBufferMemory); LOAD_D(vkMapMemory); LOAD_D(vkCreateShaderModule); LOAD_D(vkCreateDescriptorSetLayout);
  LOAD_D(vkCreatePipelineLayout); LOAD_D(vkCreateComputePipelines); LOAD_D(vkCreateDescriptorPool);
  LOAD_D(vkAllocateDescriptorSets); LOAD_D(vkUpdateDescriptorSets); LOAD_D(vkCreateCommandPool);
  LOAD_D(vkAllocateCommandBuffers); LOAD_D(vkBeginCommandBuffer); LOAD_D(vkEndCommandBuffer); LOAD_D(vkResetCommandBuffer);
  LOAD_D(vkCmdBindPipeline); LOAD_D(vkCmdBindDescriptorSets); LOAD_D(vkCmdPushConstants); LOAD_D(vkCmdDispatch);
  LOAD_D(vkCmdPipelineBarrier); LOAD_D(vkCmdFillBuffer); LOAD_D(vkQueueSubmit); LOAD_D(vkQueueWaitIdle);
  vkGetDeviceQueue(device, family, 0, &queue);

  // The EDRAM buffer: 2,048 tiles of 80x16 samples at 4 bytes, each host sample scale^2 times. Random content.
  const uint64_t edram_bytes = uint64_t(2048) * 80 * 16 * scale * scale * 4;
  const uint64_t dest_bytes = uint64_t(96) << 20;
  Buffer edram = MakeBuffer(edram_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  Buffer dest_original = MakeBuffer(dest_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  Buffer dest_variant = MakeBuffer(dest_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  {
    uint32_t* p = reinterpret_cast<uint32_t*>(edram.mapped);
    Rng fill{0xD1B54A32D192ED03ull};
    for (uint64_t i = 0; i < edram_bytes / 4; ++i) p[i] = uint32_t(fill.next() >> 16);
  }

  VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dsl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dsl.bindingCount = 1;
  dsl.pBindings = &binding;
  VkDescriptorSetLayout layouts[2];
  VKCHECK(vkCreateDescriptorSetLayout(device, &dsl, nullptr, &layouts[0]));
  VKCHECK(vkCreateDescriptorSetLayout(device, &dsl, nullptr, &layouts[1]));
  VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
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
  VkPipeline pipeline_original = make_pipeline(original_path), pipeline_variant = make_pipeline(variant_path);

  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
  VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 3;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &pool_size;
  VkDescriptorPool pool;
  VKCHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));
  auto alloc_set = [&](VkDescriptorSetLayout layout, const Buffer& buffer) {
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &layout;
    VkDescriptorSet set;
    VKCHECK(vkAllocateDescriptorSets(device, &dsai, &set));
    VkDescriptorBufferInfo bi = {buffer.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    return set;
  };
  VkDescriptorSet set_edram = alloc_set(layouts[0], edram);
  VkDescriptorSet set_dest_original = alloc_set(layouts[1], dest_original);
  VkDescriptorSet set_dest_variant = alloc_set(layouts[1], dest_variant);

  VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.queueFamilyIndex = family;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool cmd_pool;
  VKCHECK(vkCreateCommandPool(device, &cpi, nullptr, &cmd_pool));
  VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = cmd_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  VKCHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));

  uint32_t failures = 0, compared_words_total = 0;
  uint64_t nonzero_words_total = 0;
  for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
    // ---- legal random parameters
    const uint32_t msaa = rng.below(2);                       // 1x, 2x (the 4x resolves use another shader)
    const bool is_depth = rng.below(4) == 0;
    uint32_t format;
    if (is_depth) {
      format = rng.below(2);                                  // D24S8, D24FS8
    } else {
      static const uint32_t formats[] = {0, 1, 2, 3, 10, 12};  // 32-bit color formats the shader knows
      format = formats[rng.below(6)];
    }
    const uint32_t pitch_tiles = rng.range(1, 24);
    const uint32_t base_tiles = rng.below(2048);
    const uint32_t duplicate = rng.below(3) == 0;
    const uint32_t offset_x_div_8 = rng.below(16), offset_y_div_8 = rng.below(2);
    const uint32_t width_div_8 = rng.range(1, 128), height_div_8 = rng.range(1, 90);
    const uint32_t endian = rng.below(4);
    const bool is_3d = rng.below(6) == 0;
    const uint32_t slice = rng.below(8);
    const uint32_t swap = rng.below(2);
    const uint32_t dest_offset_x_div_8 = rng.below(16), dest_offset_y_div_8 = rng.below(16);
    uint32_t sample_select = rng.below(7);
    const uint32_t width = width_div_8 * 8 + dest_offset_x_div_8 * 8, height = height_div_8 * 8 + dest_offset_y_div_8 * 8;
    const uint32_t pitch32 = (width + 31) / 32 + rng.below(3);
    const uint32_t height32 = (height + 31) / 32 + rng.below(3);

    PushConstants pc;
    pc.edram_info = pitch_tiles | (msaa << 10) | (is_depth ? 4096u : 0u) | (base_tiles << 13) | (format << 24) | (duplicate ? 0x20000000u : 0u);
    pc.coordinate_info = offset_x_div_8 | (offset_y_div_8 << 4) | (width_div_8 << 5) | (scale << 16) | (scale << 19);
    pc.dest_info = endian | (is_3d ? 8u : 0u) | (slice << 4) | (swap ? 0x1000000u : 0u);
    pc.dest_coordinate_info = pitch32 | (height32 << 10) | (dest_offset_x_div_8 << 20) | (dest_offset_y_div_8 << 24) | (sample_select << 28);
    const uint32_t group_count_x = (width_div_8 * 8 * scale + 63) / 64;
    const uint32_t group_count_y = (height_div_8 * 8 * scale + 7) / 8;

    VKCHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdFillBuffer(cmd, dest_original.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, dest_variant.buffer, 0, VK_WHOLE_SIZE, 0);
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    for (int which = 0; which < 2; ++which) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, which ? pipeline_variant : pipeline_original);
      VkDescriptorSet sets[2] = {set_edram, which ? set_dest_variant : set_dest_original};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 2, sets, 0, nullptr);
      vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
      vkCmdDispatch(cmd, group_count_x, group_count_y, 1);
    }
    VkMemoryBarrier host_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0, nullptr, 0, nullptr);
    VKCHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VKCHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VKCHECK(vkQueueWaitIdle(queue));

    const uint32_t* a = reinterpret_cast<const uint32_t*>(dest_original.mapped);
    const uint32_t* b = reinterpret_cast<const uint32_t*>(dest_variant.mapped);
    const uint64_t words = dest_bytes / 4;
    uint64_t first_difference = UINT64_MAX, nonzero = 0, differences = 0;
    for (uint64_t i = 0; i < words; ++i) {
      nonzero += a[i] != 0;
      if (a[i] != b[i]) {
        ++differences;
        if (first_difference == UINT64_MAX) first_difference = i;
      }
    }
    nonzero_words_total += nonzero;
    compared_words_total += 1;
    if (differences) {
      ++failures;
      if (failures <= 10) {
        std::printf("MISMATCH iteration %u: %llu words differ, first at word %llu (original %08x, variant %08x)\n", iteration,
                    (unsigned long long)differences, (unsigned long long)first_difference, a[first_difference], b[first_difference]);
        std::printf("  scale %u msaa %u depth %u format %u pitch_tiles %u base %u dup %u off(%u,%u) size_div8(%u,%u) endian %u 3d %u slice %u swap %u dest_off(%u,%u) pitch32 %u height32 %u select %u\n",
                    scale, msaa, is_depth, format, pitch_tiles, base_tiles, duplicate, offset_x_div_8, offset_y_div_8, width_div_8, height_div_8, endian,
                    is_3d, slice, swap, dest_offset_x_div_8, dest_offset_y_div_8, pitch32, height32, sample_select);
      }
    } else if (nonzero == 0) {
      std::printf("note: iteration %u wrote nothing\n", iteration);
    }
  }
  std::printf("%u iterations, %u with a difference; %.1f million non-zero destination words compared in all\n", iterations, failures,
              nonzero_words_total / 1e6);
  return failures ? 1 : 0;
}
