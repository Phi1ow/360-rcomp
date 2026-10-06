// Host GPU test and bench of the fused dump + resolve copy shader (fused_fast32.comp) against the title's own two-pass path.
//
// usage: fused_resolve_bench <device> <scale 1..3> <iterations> <variant> <dir> [seed] [native2x 0|1] [bench]
//
// <dir> holds the SPIR-V files: dump_<variant>.spv (the title's render target dump shader, logged by the title with rcomp_diag_log_dump_spirv), copy.spv
// (resolve_fast_32bpp_1x2xmsaa_scaled_cs of the SDK), fused_<variant>.spv (fused_fast32.comp compiled with the variant's defines), fill_<variant>.spv and compare.spv.
//
// Each iteration: a render target image of the variant's host format filled with random contents, a random legal resolve (the copy shader's parameters, the render target's
// base and pitch), then (a) the dump of the render target into a poisoned EDRAM buffer followed by the original copy shader, and (b) the fused shader, each into its own zeroed
// destination buffer; the two destination buffers are compared on the GPU word by word. Host validation only: it is never PS5 evidence.
// With "bench" the same two paths are timed (GPU timestamps) on a few representative resolves.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
#define FN(name) static PFN_##name name;
FN(vkCreateInstance) FN(vkEnumeratePhysicalDevices) FN(vkGetPhysicalDeviceProperties) FN(vkGetPhysicalDeviceMemoryProperties)
FN(vkGetPhysicalDeviceQueueFamilyProperties) FN(vkGetPhysicalDeviceFeatures) FN(vkCreateDevice) FN(vkGetDeviceProcAddr)
FN(vkGetDeviceQueue) FN(vkCreateBuffer) FN(vkGetBufferMemoryRequirements) FN(vkAllocateMemory) FN(vkBindBufferMemory) FN(vkMapMemory)
FN(vkCreateImage) FN(vkGetImageMemoryRequirements) FN(vkBindImageMemory) FN(vkCreateImageView)
FN(vkCreateShaderModule) FN(vkCreateDescriptorSetLayout) FN(vkCreatePipelineLayout) FN(vkCreateComputePipelines)
FN(vkCreateDescriptorPool) FN(vkAllocateDescriptorSets) FN(vkUpdateDescriptorSets) FN(vkCreateCommandPool) FN(vkAllocateCommandBuffers)
FN(vkBeginCommandBuffer) FN(vkEndCommandBuffer) FN(vkResetCommandBuffer) FN(vkCmdBindPipeline) FN(vkCmdBindDescriptorSets)
FN(vkCmdPushConstants) FN(vkCmdDispatch) FN(vkCmdPipelineBarrier) FN(vkCmdFillBuffer) FN(vkCmdCopyBufferToImage) FN(vkCmdCopyBuffer)
FN(vkQueueSubmit) FN(vkQueueWaitIdle) FN(vkCreateQueryPool) FN(vkCmdResetQueryPool) FN(vkCmdWriteTimestamp) FN(vkGetQueryPoolResults)

static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice device;
static VkQueue queue;
static uint32_t family;
static VkPhysicalDeviceMemoryProperties mem_props;
static float timestamp_period = 1.0f;

static uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want) return i;
  }
  std::fprintf(stderr, "no memory type for flags %x\n", unsigned(want));
  std::exit(2);
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  uint8_t* mapped = nullptr;
};

static Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
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
  alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKCHECK(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
  VKCHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
  if (host) {
    void* p;
    VKCHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &p));
    b.mapped = static_cast<uint8_t*>(p);
  }
  return b;
}

static std::vector<uint32_t> LoadSpirv(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
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

enum FillKind { kFillRgba8 = 0, kFillRgba16f = 1, kFillR32 = 2, kFillDepth = 3 };

// The "full" copies (resolve_full_32bpp_scaled_cs / resolve_full_64bpp_scaled_cs against fused_full.comp): which copy shader, the dump shader of the title that fills the EDRAM for
// the reference path, the copy's EDRAM formats and sample selects the test draws from, and the destination format of the required resolve.
struct FullSpec {
  uint32_t bpp = 0;                 // 0 = the fast 32-bit copy (fused_fast32.comp), 32 / 64 = destination bits per pixel of the full copy (fused_full.comp)
  const char* dump = nullptr;       // dump_<dump>.spv of the reference path (the variant's name when null)
  uint32_t formats[2] = {0, 0};     // copy EDRAM format numbers to draw from (formats[1] == 0: only formats[0], except for the 8:8:8:8 format number 0 itself)
  uint32_t n_formats = 1;
  uint32_t selects[8] = {0};        // sample selects to draw from
  uint32_t n_selects = 1;
  uint32_t required_dest = 0;       // the destination texture format of the measured resolve ("req" destination mode)
};

struct Variant {
  const char* name;
  bool depth;
  uint32_t msaa;          // guest MSAA of the render target: 0 = 1x, 1 = 2x, 2 = 4x
  uint32_t guest_format;  // color format number or depth format number (edram_info.format)
  VkFormat vk_format;     // host image format
  bool source_is_uint;    // the dump samples the image through an unsigned integer view
  FillKind fill;
  FullSpec full;          // default: the fast copy
};

// Destination texture formats of the 32-bit and 64-bit full copies: every label of the original's switch plus formats that fall to its default branch.
static const uint32_t kDest32[] = {6, 14, 50, 7, 54, 16, 55, 17, 56, 25, 31, 36, 22, 23, 61};
static const uint32_t kDest64[] = {32, 26, 38, 37, 63};

static const Variant kVariants[] = {
    // ---- the full copies: required variants of the TBoGT measurement
    {"a64_fp10_1x", false, 0, 3, VK_FORMAT_R16G16B16A16_SFLOAT, false, kFillRgba16f, {64, "c2101010f_1x", {3, 12}, 2, {0, 0, 0}, 1, 32}},
    {"a64_fp10_4x", false, 2, 3, VK_FORMAT_R16G16B16A16_SFLOAT, false, kFillRgba16f, {64, "c2101010f_4x", {3, 12}, 2, {6, 0, 0}, 1, 32}},
    {"b32_8888_2x", false, 1, 0, VK_FORMAT_R8G8B8A8_UNORM, false, kFillRgba8, {32, "c8888_2x", {0, 0}, 1, {4, 0, 0}, 1, 6}},
    {"b32_32f_4x", false, 2, 14, VK_FORMAT_R32_SFLOAT, true, kFillR32, {32, "c32f_4x", {14, 0}, 1, {6, 0, 0}, 1, 36}},
    {"b32_fp10_1x", false, 0, 12, VK_FORMAT_R16G16B16A16_SFLOAT, false, kFillRgba16f, {32, "c2101010f_1x", {12, 3}, 2, {0, 0, 0}, 1, 6}},
    // ---- more of the same class (not asked for, the dump shaders exist)
    {"b32_8888_1x", false, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, false, kFillRgba8, {32, "c8888_1x", {0, 0}, 1, {0, 0, 0}, 1, 6}},
    {"b32_8888_4x", false, 2, 0, VK_FORMAT_R8G8B8A8_UNORM, false, kFillRgba8, {32, "c8888_4x", {0, 0}, 1, {0, 4, 6}, 3, 6}},
    {"b32_8888_4x_sall", false, 2, 0, VK_FORMAT_R8G8B8A8_UNORM, false, kFillRgba8, {32, "c8888_4x", {0, 0}, 1, {0, 1, 2, 3, 4, 5, 6}, 7, 6}},
    {"a64_fp10_4x_sall", false, 2, 3, VK_FORMAT_R16G16B16A16_SFLOAT, false, kFillRgba16f, {64, "c2101010f_4x", {3, 12}, 2, {0, 1, 2, 3, 4, 5, 6}, 7, 32}},
    {"b32_8888_2x_s01", false, 1, 0, VK_FORMAT_R8G8B8A8_UNORM, false, kFillRgba8, {32, "c8888_2x", {0, 0}, 1, {0, 1, 4}, 3, 6}},
    {"b32_fp10_4x", false, 2, 3, VK_FORMAT_R16G16B16A16_SFLOAT, false, kFillRgba16f, {32, "c2101010f_4x", {3, 12}, 2, {6, 4, 0}, 2, 6}},
    {"b32_32f_1x", false, 0, 14, VK_FORMAT_R32_SFLOAT, true, kFillR32, {32, "c32f_1x", {14, 0}, 1, {0, 0, 0}, 1, 36}},
    {"a64_8888_1x", false, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, false, kFillRgba8, {64, "c8888_1x", {0, 0}, 1, {0, 0, 0}, 1, 32}},
    {"a64_8888_2x", false, 1, 0, VK_FORMAT_R8G8B8A8_UNORM, false, kFillRgba8, {64, "c8888_2x", {0, 0}, 1, {4, 0, 0}, 1, 32}},
    {"a64_8888_4x", false, 2, 0, VK_FORMAT_R8G8B8A8_UNORM, false, kFillRgba8, {64, "c8888_4x", {0, 0}, 1, {6, 0, 0}, 1, 32}},
    {"a64_32f_1x", false, 0, 14, VK_FORMAT_R32_SFLOAT, true, kFillR32, {64, "c32f_1x", {14, 0}, 1, {0, 0, 0}, 1, 32}},
    // ---- the fast 32-bit copy (fused_fast32.comp, unchanged)
    {"c8888_1x", false, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, false, kFillRgba8},
    {"c8888_2x", false, 1, 0, VK_FORMAT_R8G8B8A8_UNORM, false, kFillRgba8},
    {"c2101010_1x", false, 0, 2, VK_FORMAT_A8B8G8R8_UNORM_PACK32, false, kFillRgba8},
    {"c2101010_2x", false, 1, 2, VK_FORMAT_A8B8G8R8_UNORM_PACK32, false, kFillRgba8},
    {"c2101010f_1x", false, 0, 3, VK_FORMAT_R16G16B16A16_SFLOAT, false, kFillRgba16f},
    {"c2101010f_2x", false, 1, 3, VK_FORMAT_R16G16B16A16_SFLOAT, false, kFillRgba16f},
    {"c32f_1x", false, 0, 14, VK_FORMAT_R32_SFLOAT, true, kFillR32},
    {"c32f_2x", false, 1, 14, VK_FORMAT_R32_SFLOAT, true, kFillR32},
    {"c32fF_1x", false, 0, 14, VK_FORMAT_R32_SFLOAT, false, kFillR32},
    {"d24s8_1x", true, 0, 0, VK_FORMAT_D32_SFLOAT_S8_UINT, false, kFillDepth},
    {"d24fs8_1x", true, 0, 1, VK_FORMAT_D32_SFLOAT_S8_UINT, false, kFillDepth},
};

struct PushCopy {  // draw_util::ResolveCopyShaderConstants::DestRelative
  uint32_t edram_info, coordinate_info, dest_info, dest_coordinate_info;
};
struct PushFused {  // DirectResolvePushConstants
  PushCopy copy;
  uint32_t dest_base, source_base_tiles, source_pitch_tiles, dispatch_first_tile;
};
struct PushDump {
  uint32_t pitches, offsets, sample_mask;
};

static constexpr uint32_t kImgPitchTiles = 16;
static constexpr uint32_t kImgRowsTiles = 64;

struct Context {
  VkDescriptorSetLayout dsl_storage = VK_NULL_HANDLE, dsl_image1 = VK_NULL_HANDLE, dsl_image2 = VK_NULL_HANDLE, dsl_storage_image = VK_NULL_HANDLE,
                        dsl_compare = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
};

static VkDescriptorSetLayout MakeSetLayout(const std::vector<VkDescriptorType>& types) {
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  for (uint32_t i = 0; i < types.size(); ++i) bindings.push_back({i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
  VkDescriptorSetLayoutCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = uint32_t(bindings.size());
  info.pBindings = bindings.data();
  VkDescriptorSetLayout layout;
  VKCHECK(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout));
  return layout;
}

static VkPipelineLayout MakePipelineLayout(const std::vector<VkDescriptorSetLayout>& sets, uint32_t push_bytes) {
  VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
  VkPipelineLayoutCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  info.setLayoutCount = uint32_t(sets.size());
  info.pSetLayouts = sets.data();
  info.pushConstantRangeCount = push_bytes ? 1 : 0;
  info.pPushConstantRanges = &range;
  VkPipelineLayout layout;
  VKCHECK(vkCreatePipelineLayout(device, &info, nullptr, &layout));
  return layout;
}

static VkPipeline MakePipeline(const std::string& path, VkPipelineLayout layout) {
  std::vector<uint32_t> code = LoadSpirv(path);
  VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = code.size() * 4;
  smci.pCode = code.data();
  VkShaderModule module;
  VKCHECK(vkCreateShaderModule(device, &smci, nullptr, &module));
  VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
  cpci.layout = layout;
  VkPipeline pipeline;
  VKCHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));
  return pipeline;
}

static VkDescriptorSet AllocSet(VkDescriptorPool pool, VkDescriptorSetLayout layout) {
  VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &layout;
  VkDescriptorSet set;
  VKCHECK(vkAllocateDescriptorSets(device, &dsai, &set));
  return set;
}

static void WriteStorageBuffer(VkDescriptorSet set, uint32_t binding, const Buffer& buffer) {
  VkDescriptorBufferInfo bi = {buffer.buffer, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set;
  w.dstBinding = binding;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w.pBufferInfo = &bi;
  vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

static void WriteImage(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkImageView view) {
  VkDescriptorImageInfo ii = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set;
  w.dstBinding = binding;
  w.descriptorCount = 1;
  w.descriptorType = type;
  w.pImageInfo = &ii;
  vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0, samples = 1;
};

static Image MakeImage(VkFormat format, uint32_t width, uint32_t height, uint32_t samples, VkImageUsageFlags usage, VkImageCreateFlags flags) {
  Image im;
  im.width = width;
  im.height = height;
  im.samples = samples;
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.flags = flags;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {width, height, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VkSampleCountFlagBits(samples);
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VKCHECK(vkCreateImage(device, &ici, nullptr, &im.image));
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, im.image, &req);
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKCHECK(vkAllocateMemory(device, &alloc, nullptr, &im.memory));
  VKCHECK(vkBindImageMemory(device, im.image, im.memory, 0));
  return im;
}

static VkImageView MakeView(const Image& im, VkFormat format, VkImageAspectFlags aspect) {
  VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = im.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = {aspect, 0, 1, 0, 1};
  VkImageView view;
  VKCHECK(vkCreateImageView(device, &vi, nullptr, &view));
  return view;
}

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr, "usage: fused_resolve_bench <device> <scale 1..3> <iterations> <variant> <dir> [seed] [native2x 0|1] [bench|-] [req|all]\n"
                         "  req|all (full copy variants): the destination format of every resolve is the required one (req, default) or drawn from every format of the copy's class (all)\n"
                         "  environment HARNESS_TITLE_MASK=1: the dump of multisampled sources uses GetDumpSampleMask of the title (guest sample bits) instead of the exact host sample set\n");
    return 1;
  }
  const uint32_t dev_index = std::atoi(argv[1]);
  const uint32_t scale = std::atoi(argv[2]);
  const uint32_t iterations = std::atoi(argv[3]);
  const std::string variant_name = argv[4];
  const std::string dir = argv[5];
  Rng rng{argc > 6 ? std::strtoull(argv[6], nullptr, 0) : 0x9E3779B97F4A7C15ull};
  const bool native2x = argc > 7 ? std::atoi(argv[7]) != 0 : true;
  const bool bench = argc > 8 && std::string(argv[8]) == "bench";
  const bool dest_all = argc > 9 && std::string(argv[9]) == "all";
  const bool title_mask = std::getenv("HARNESS_TITLE_MASK") && std::atoi(std::getenv("HARNESS_TITLE_MASK")) != 0;
  if (scale < 1 || scale > 3) return 1;
  const Variant* variant = nullptr;
  for (const Variant& v : kVariants) {
    if (variant_name == v.name) variant = &v;
  }
  if (!variant) {
    std::fprintf(stderr, "unknown variant %s\n", variant_name.c_str());
    return 1;
  }
  const bool full = variant->full.bpp != 0;
  const std::string dump_name = variant->full.dump ? variant->full.dump : variant->name;
  const uint32_t host_samples = variant->msaa == 0 ? 1 : (variant->msaa == 1 ? (native2x ? 2 : 4) : 4);

  HMODULE lib = LoadLibraryA("vulkan-1.dll");
  if (!lib) return 2;
  gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
  vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VKCHECK(vkCreateInstance(&ici, nullptr, &instance));
#define LOAD_I(name) name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
  LOAD_I(vkEnumeratePhysicalDevices); LOAD_I(vkGetPhysicalDeviceProperties); LOAD_I(vkGetPhysicalDeviceMemoryProperties);
  LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties); LOAD_I(vkGetPhysicalDeviceFeatures); LOAD_I(vkCreateDevice); LOAD_I(vkGetDeviceProcAddr);
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  if (dev_index >= count) return 2;
  phys = devices[dev_index];
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(phys, &props);
  timestamp_period = props.limits.timestampPeriod;
  std::printf("device %u: %s, variant %s, scale %u, host samples %u\n", dev_index, props.deviceName, variant->name, scale, host_samples);
  vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
  VkPhysicalDeviceFeatures features;
  vkGetPhysicalDeviceFeatures(phys, &features);
  if (!features.shaderStorageImageMultisample && host_samples > 1 && !variant->depth) {
    std::fprintf(stderr, "shaderStorageImageMultisample is not supported: cannot fill a multisampled color image\n");
    return 3;
  }
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
  VkPhysicalDeviceFeatures enabled = {};
  enabled.shaderStorageImageMultisample = features.shaderStorageImageMultisample;
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.pEnabledFeatures = &enabled;
  VKCHECK(vkCreateDevice(phys, &dci, nullptr, &device));
#define LOAD_D(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))
  LOAD_D(vkGetDeviceQueue); LOAD_D(vkCreateBuffer); LOAD_D(vkGetBufferMemoryRequirements); LOAD_D(vkAllocateMemory); LOAD_D(vkBindBufferMemory);
  LOAD_D(vkMapMemory); LOAD_D(vkCreateImage); LOAD_D(vkGetImageMemoryRequirements); LOAD_D(vkBindImageMemory); LOAD_D(vkCreateImageView);
  LOAD_D(vkCreateShaderModule); LOAD_D(vkCreateDescriptorSetLayout); LOAD_D(vkCreatePipelineLayout); LOAD_D(vkCreateComputePipelines);
  LOAD_D(vkCreateDescriptorPool); LOAD_D(vkAllocateDescriptorSets); LOAD_D(vkUpdateDescriptorSets); LOAD_D(vkCreateCommandPool);
  LOAD_D(vkAllocateCommandBuffers); LOAD_D(vkBeginCommandBuffer); LOAD_D(vkEndCommandBuffer); LOAD_D(vkResetCommandBuffer);
  LOAD_D(vkCmdBindPipeline); LOAD_D(vkCmdBindDescriptorSets); LOAD_D(vkCmdPushConstants); LOAD_D(vkCmdDispatch); LOAD_D(vkCmdPipelineBarrier);
  LOAD_D(vkCmdFillBuffer); LOAD_D(vkCmdCopyBufferToImage); LOAD_D(vkCmdCopyBuffer); LOAD_D(vkQueueSubmit); LOAD_D(vkQueueWaitIdle);
  LOAD_D(vkCreateQueryPool); LOAD_D(vkCmdResetQueryPool); LOAD_D(vkCmdWriteTimestamp); LOAD_D(vkGetQueryPoolResults);
  vkGetDeviceQueue(device, family, 0, &queue);

  // ---- buffers: the EDRAM buffer (2,048 tiles of 80x16 samples at 4 bytes, scale^2 host samples each), two destinations, the comparison result
  const uint64_t edram_bytes = uint64_t(2048) * 80 * 16 * scale * scale * 4;
  const uint64_t dest_bytes = uint64_t(variant->full.bpp == 64 ? 192 : 96) << 20;  // a 64-bit destination of a 3D slice 4..7 at scale 3 reaches ~96 MB
  const VkBufferUsageFlags storage_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  Buffer edram = MakeBuffer(edram_bytes, storage_usage, false);
  Buffer dest_a = MakeBuffer(dest_bytes, storage_usage, false);
  Buffer dest_b = MakeBuffer(dest_bytes, storage_usage, false);
  Buffer result = MakeBuffer(16, storage_usage, false);
  Buffer result_host = MakeBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  Buffer result_init = MakeBuffer(16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
  {
    uint32_t init[4] = {0, UINT32_MAX, 0, 0};
    std::memcpy(result_init.mapped, init, sizeof(init));
  }

  // ---- the render target image
  const uint32_t sample_w = 80 * scale * kImgPitchTiles, sample_h = 16 * scale * kImgRowsTiles;
  const uint32_t pixel_w = variant->msaa == 2 ? sample_w / 2 : sample_w;
  const uint32_t pixel_h = variant->msaa >= 1 ? sample_h / 2 : sample_h;
  VkImageUsageFlags image_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkImageCreateFlags image_flags = 0;
  if (!variant->depth) {
    image_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (variant->fill == kFillR32) image_flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  }
  Image rt = MakeImage(variant->vk_format, pixel_w, pixel_h, host_samples, image_usage, image_flags);
  VkImageView view_sampled = VK_NULL_HANDLE, view_stencil = VK_NULL_HANDLE, view_fill = VK_NULL_HANDLE;
  if (variant->depth) {
    view_sampled = MakeView(rt, variant->vk_format, VK_IMAGE_ASPECT_DEPTH_BIT);
    view_stencil = MakeView(rt, variant->vk_format, VK_IMAGE_ASPECT_STENCIL_BIT);
  } else {
    VkFormat sampled_format = variant->source_is_uint ? VK_FORMAT_R32_UINT : variant->vk_format;
    view_sampled = MakeView(rt, sampled_format, VK_IMAGE_ASPECT_COLOR_BIT);
    view_fill = MakeView(rt, variant->fill == kFillR32 ? VK_FORMAT_R32_UINT : variant->vk_format, VK_IMAGE_ASPECT_COLOR_BIT);
  }
  // Depth contents come from staging buffers (no multisampled copy): only 1x depth is a test variant.
  Buffer depth_staging, stencil_staging;
  if (variant->depth) {
    if (host_samples != 1) {
      std::fprintf(stderr, "multisampled depth contents cannot be written by a copy: not a test variant\n");
      return 3;
    }
    depth_staging = MakeBuffer(uint64_t(pixel_w) * pixel_h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    stencil_staging = MakeBuffer(uint64_t(pixel_w) * pixel_h, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
  }

  // ---- layouts, pipelines, descriptor sets
  Context ctx;
  ctx.dsl_storage = MakeSetLayout({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER});
  ctx.dsl_image1 = MakeSetLayout({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE});
  ctx.dsl_image2 = MakeSetLayout({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE});
  ctx.dsl_storage_image = MakeSetLayout({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
  ctx.dsl_compare = MakeSetLayout({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER});
  VkDescriptorSetLayout dsl_source = variant->depth ? ctx.dsl_image2 : ctx.dsl_image1;
  VkPipelineLayout layout_dump = MakePipelineLayout({ctx.dsl_storage, dsl_source}, sizeof(PushDump));
  VkPipelineLayout layout_copy = MakePipelineLayout({ctx.dsl_storage, ctx.dsl_storage}, sizeof(PushCopy));
  VkPipelineLayout layout_fused = MakePipelineLayout({ctx.dsl_storage, dsl_source}, sizeof(PushFused));
  VkPipelineLayout layout_fill = MakePipelineLayout({ctx.dsl_storage_image}, 12);
  VkPipelineLayout layout_compare = MakePipelineLayout({ctx.dsl_compare}, 4);
  VkPipeline pipeline_dump = MakePipeline(dir + "/dump_" + dump_name + ".spv", layout_dump);
  VkPipeline pipeline_copy = MakePipeline(dir + (full ? (variant->full.bpp == 64 ? "/resolve_full_64bpp_scaled_cs.spv" : "/resolve_full_32bpp_scaled_cs.spv") : "/copy.spv"), layout_copy);
  VkPipeline pipeline_fused = MakePipeline(dir + "/fused_" + variant->name + ".spv", layout_fused);
  VkPipeline pipeline_fill = variant->depth ? VK_NULL_HANDLE : MakePipeline(dir + "/fill_" + variant->name + (std::getenv("HARNESS_FILL") ? std::string("_") + std::getenv("HARNESS_FILL") : std::string(variant->fill == kFillR32 && std::getenv("HARNESS_NO_NAN") && std::atoi(std::getenv("HARNESS_NO_NAN")) ? "_nonan" : "")) + ".spv", layout_fill);
  VkPipeline pipeline_compare = MakePipeline(dir + "/compare.spv", layout_compare);

  VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
  VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 12;
  dpci.poolSizeCount = 3;
  dpci.pPoolSizes = pool_sizes;
  VKCHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &ctx.pool));
  VkDescriptorSet set_edram = AllocSet(ctx.pool, ctx.dsl_storage), set_dest_a = AllocSet(ctx.pool, ctx.dsl_storage),
                  set_dest_b = AllocSet(ctx.pool, ctx.dsl_storage), set_source = AllocSet(ctx.pool, dsl_source),
                  set_compare = AllocSet(ctx.pool, ctx.dsl_compare), set_fill = AllocSet(ctx.pool, ctx.dsl_storage_image);
  WriteStorageBuffer(set_edram, 0, edram);
  WriteStorageBuffer(set_dest_a, 0, dest_a);
  WriteStorageBuffer(set_dest_b, 0, dest_b);
  WriteStorageBuffer(set_compare, 0, dest_a);
  WriteStorageBuffer(set_compare, 1, dest_b);
  WriteStorageBuffer(set_compare, 2, result);
  WriteImage(set_source, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view_sampled);
  if (variant->depth) WriteImage(set_source, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view_stencil);
  if (!variant->depth) WriteImage(set_fill, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, view_fill);

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

  VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qpi.queryCount = 8;
  VkQueryPool query_pool;
  VKCHECK(vkCreateQueryPool(device, &qpi, nullptr, &query_pool));

  auto memory_barrier = [&](VkPipelineStageFlags src_stage, VkAccessFlags src, VkPipelineStageFlags dst_stage, VkAccessFlags dst) {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &b, 0, nullptr, 0, nullptr);
  };
  auto image_barrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src_stage, VkAccessFlags src, VkPipelineStageFlags dst_stage,
                           VkAccessFlags dst) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = rt.image;
    b.subresourceRange = {variant->depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                          0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
  };
  bool image_initialized = false;
  uint32_t iteration_seed = 1;

  // One resolve: random legal parameters.
  struct Case {
    uint32_t msaa, is_depth, format, copy_pitch, base, dup, offx, offy, wdiv8, hdiv8, endian, is3d, slice, swap, doffx, doffy, pitch32, height32, select;
    uint32_t rt_pitch, rt_base, mask, span_base, span_row_length, span_rows;
    uint32_t dest_format;
    int32_t exp_bias;
    PushCopy copy;
  };
  auto make_case = [&](bool bench_case) -> Case {
    Case c = {};
    for (;;) {
      c.msaa = variant->msaa;
      c.is_depth = variant->depth;
      c.format = variant->guest_format;
      c.offx = rng.below(10);
      c.offy = rng.below(2);
      c.wdiv8 = bench_case ? (c.msaa >= 2 ? 80 : 160) : rng.range(1, 100);  // a 4x band is half as wide in pixels: the image is 16 tiles of 80 samples wide at 1x scale
      c.hdiv8 = bench_case ? 32 : rng.range(1, 60);
      const uint32_t x_scale_log2 = 3 + (c.msaa >= 2), y_scale_log2 = 3 + (c.msaa >= 1);
      const uint32_t x0 = (c.offx << x_scale_log2) / 80;
      const uint32_t x1 = (((c.offx + c.wdiv8) << x_scale_log2) + 79) / 80;
      const uint32_t y0 = (c.offy << y_scale_log2) / 16;
      const uint32_t y1 = (((c.offy + c.hdiv8) << y_scale_log2) + 15) / 16;
      c.span_row_length = x1 - x0;
      c.span_rows = y1 - y0;
      if (x1 > kImgPitchTiles) continue;
      c.copy_pitch = rng.range(std::max<uint32_t>(x1, 1), kImgPitchTiles);
      if (bench_case) c.copy_pitch = kImgPitchTiles;
      const uint32_t tiles_needed = y0 * c.copy_pitch + x0 + (c.span_rows - 1) * c.copy_pitch + c.span_row_length;
      if (tiles_needed >= 2048) continue;
      c.base = rng.below(2048 - tiles_needed);
      c.span_base = c.base + y0 * c.copy_pitch + x0;
      // The render target: the copy's pitch most of the time, a different one now and then; its base at or before the first tile of the span.
      c.rt_pitch = (bench_case || rng.below(5) != 0) ? c.copy_pitch : rng.range(1, kImgPitchTiles);
      c.rt_base = c.span_base - std::min<uint32_t>(c.span_base, rng.below(4));
      const uint32_t last_index = c.span_base + (c.span_rows - 1) * c.copy_pitch + c.span_row_length - 1 - c.rt_base;
      if (last_index / c.rt_pitch >= kImgRowsTiles) continue;
      if (c.rt_pitch < 1) continue;
      break;
    }
    c.dup = rng.below(3) == 0;
    c.endian = rng.below(4);
    c.is3d = rng.below(6) == 0;
    c.slice = rng.below(8);
    c.swap = rng.below(2);
    c.doffx = rng.below(16);
    c.doffy = rng.below(16);
    c.select = c.msaa >= 1 ? rng.below(2) : 0;
    c.mask = (c.msaa == 0 || rng.below(4) == 0) ? 0xF : (c.select == 0 ? 0x1 : 0x2);
    if (full) {
      const FullSpec& f = variant->full;
      c.format = f.formats[f.n_formats > 1 ? rng.below(f.n_formats) : 0];
      c.select = f.selects[f.n_selects > 1 ? rng.below(f.n_selects) : 0];
      c.endian = rng.below(8);  // 0..3 for both classes, 4 = 8in64 (the 64-bit copy), 5..7 do nothing in either copy
      if (dest_all) {
        c.dest_format = f.bpp == 64 ? kDest64[rng.below(sizeof(kDest64) / sizeof(kDest64[0]))] : kDest32[rng.below(sizeof(kDest32) / sizeof(kDest32[0]))];
      } else {
        c.dest_format = f.required_dest;
      }
      // Exponent bias of the destination (6-bit signed): mostly the 0 and 1 of the title, now and then others up to the extremes.
      const uint32_t bias_draw = rng.below(20);
      c.exp_bias = bias_draw < 7 ? 0 : (bias_draw < 14 ? 1 : (bias_draw < 17 ? int32_t(rng.below(17)) - 8 : int32_t(rng.below(64)) - 32));
    }
    if (bench_case) c.is3d = 0, c.endian = 2, c.dup = 0, c.doffx = c.doffy = 0;
    if (bench_case && full) c.swap = 0, c.exp_bias = 0;
    // Debug overrides of single parameters (HARNESS_FORCE_<name>=value), to bisect a failing case.
    auto force = [](const char* name, uint32_t& value) {
      const char* e = std::getenv(name);
      if (e) value = uint32_t(std::atoi(e));
    };
    force("HARNESS_FORCE_ENDIAN", c.endian);
    force("HARNESS_FORCE_SWAP", c.swap);
    force("HARNESS_FORCE_DUP", c.dup);
    force("HARNESS_FORCE_3D", c.is3d);
    force("HARNESS_FORCE_SELECT", c.select);
    force("HARNESS_FORCE_DEST", c.dest_format);
    force("HARNESS_FORCE_DOFFX", c.doffx);
    force("HARNESS_FORCE_DOFFY", c.doffy);
    if (const char* e = std::getenv("HARNESS_FORCE_BIAS")) c.exp_bias = std::atoi(e);
    if (full) {
      // The samples the dump has to write for this copy. Exact: the host samples the copy reads (2x: guest sample = parity of the sample row; 4x: the host sample id is
      // x | y << 1 of the position inside the pixel, the guest sample index of the copy is y | x << 1); title: GetDumpSampleMask (guest sample bits), which treats the
      // host id of a 4x sample as the guest index.
      if (title_mask) {
        static const uint32_t kTitleMask[8] = {0x1, 0x2, 0x4, 0x8, 0x3, 0xC, 0xF, 0xF};
        c.mask = kTitleMask[c.select & 7];
      } else if (c.msaa == 0) {
        c.mask = 0xF;
      } else if (c.msaa == 1) {
        c.mask = c.select == 0 ? 0x1u : (c.select == 1 ? 0x2u : 0x3u);
      } else {
        static const uint32_t kHostMask4x[8] = {0x1, 0x4, 0x2, 0x8, 0x5, 0xA, 0xF, 0xF};
        c.mask = kHostMask4x[c.select & 7];
      }
      if (!title_mask && c.msaa != 0 && rng.below(4) == 0) c.mask = 0xF;
    }
    const uint32_t width = c.wdiv8 * 8 + c.doffx * 8, height = c.hdiv8 * 8 + c.doffy * 8;
    c.pitch32 = (width + 31) / 32 + rng.below(3);
    c.height32 = (height + 31) / 32 + rng.below(3);
    c.copy.edram_info = c.copy_pitch | (c.msaa << 10) | (c.is_depth ? 4096u : 0u) | (c.base << 13) | (c.format << 24) | (c.dup ? 0x20000000u : 0u);
    c.copy.coordinate_info = c.offx | (c.offy << 4) | (c.wdiv8 << 5) | (scale << 16) | (scale << 19);
    c.copy.dest_info = c.endian | (c.is3d ? 8u : 0u) | (c.slice << 4) | (c.dest_format << 7) | ((uint32_t(c.exp_bias) & 63u) << 16) | (c.swap ? 0x1000000u : 0u);
    c.copy.dest_coordinate_info = c.pitch32 | (c.height32 << 10) | (c.doffx << 20) | (c.doffy << 24) | (c.select << 28);
    return c;
  };

  auto record_case = [&](const Case& c, bool two_paths, bool timed) {
    // 8 threads of 8 destination pixels in the fast copy, of 4 in the full copies.
    const uint32_t group_count_x = full ? (c.wdiv8 * 8 * scale + 31) / 32 : (c.wdiv8 * 8 * scale + 63) / 64;
    const uint32_t group_count_y = (c.hdiv8 * 8 * scale + 7) / 8;
    PushDump dump;
    dump.pitches = c.copy_pitch | (c.rt_pitch << 10);
    dump.offsets = c.span_base | (c.rt_base << 12);
    dump.sample_mask = c.mask;
    PushFused fused;
    fused.copy = c.copy;
    fused.dest_base = 0;
    fused.source_base_tiles = c.rt_base;
    fused.source_pitch_tiles = c.rt_pitch;
    fused.dispatch_first_tile = c.span_base;
    if (two_paths) {
      if (timed) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool, 0);
      // (a) the dump of the render target, then the original copy
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_dump);
      VkDescriptorSet dump_sets[2] = {set_edram, set_source};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_dump, 0, 2, dump_sets, 0, nullptr);
      vkCmdPushConstants(cmd, layout_dump, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dump), &dump);
      vkCmdDispatch(cmd, (scale * 80 * c.span_row_length + 7) / 8, (scale * 16 * c.span_rows + 15) / 16, 1);
      memory_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_copy);
      VkDescriptorSet copy_sets[2] = {set_edram, set_dest_a};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_copy, 0, 2, copy_sets, 0, nullptr);
      vkCmdPushConstants(cmd, layout_copy, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(c.copy), &c.copy);
      vkCmdDispatch(cmd, group_count_x, group_count_y, 1);
      if (timed) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool, 1);
    }
    // (b) the fused shader
    if (timed) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool, 2);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_fused);
    VkDescriptorSet fused_sets[2] = {set_dest_b, set_source};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_fused, 0, 2, fused_sets, 0, nullptr);
    vkCmdPushConstants(cmd, layout_fused, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fused), &fused);
    vkCmdDispatch(cmd, group_count_x, group_count_y, 1);
    if (timed) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool, 3);
  };

  auto submit_wait = [&]() {
    VKCHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VKCHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VKCHECK(vkQueueWaitIdle(queue));
  };
  auto begin = [&]() {
    VKCHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(cmd, &cbbi));
  };

  // Fills the render target with random contents (the image ends in the GENERAL layout).
  auto fill_image = [&]() {
    begin();
    if (variant->depth) {
      uint32_t* d = reinterpret_cast<uint32_t*>(depth_staging.mapped);
      uint8_t* s = stencil_staging.mapped;
      for (uint64_t i = 0; i < uint64_t(pixel_w) * pixel_h; ++i) {
        const uint32_t r = uint32_t(rng.next() >> 32);
        // Depth in [0, 1] as the title's targets hold it (saturated writes), with the exact 0 and 1 over-represented.
        float f;
        if ((r & 15) == 0) {
          f = 0.0f;
        } else if ((r & 15) == 1) {
          f = 1.0f;
        } else {
          f = float(double(r >> 4) / double(0x0FFFFFFFu));
        }
        std::memcpy(&d[i], &f, 4);
        s[i] = uint8_t(rng.next() >> 33);
      }
      image_barrier(image_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
      VkBufferImageCopy region = {};
      region.imageExtent = {pixel_w, pixel_h, 1};
      region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      vkCmdCopyBufferToImage(cmd, depth_staging.buffer, rt.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      region.imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
      vkCmdCopyBufferToImage(cmd, stencil_staging.buffer, rt.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      image_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    } else {
      image_barrier(image_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_fill);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_fill, 0, 1, &set_fill, 0, nullptr);
      uint32_t pc[3] = {iteration_seed++, pixel_w, pixel_h};
      vkCmdPushConstants(cmd, layout_fill, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, pc);
      vkCmdDispatch(cmd, (pixel_w + 7) / 8, (pixel_h + 7) / 8, 1);
      image_barrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    submit_wait();
    image_initialized = true;
  };

  if (!bench) {
    uint32_t failures = 0, empty = 0;
    uint64_t nonzero_total = 0;
    std::map<uint32_t, uint32_t> dest_formats_seen, formats_seen, selects_seen;
    uint32_t bias_zero = 0, bias_one = 0, bias_other = 0, case_3d = 0, case_swap = 0, case_dup = 0, nan_payload_only = 0;
    uint64_t nan_payload_words = 0, total_diff_words = 0;
    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
      if ((iteration % 8) == 0) fill_image();
      const Case c = make_case(false);
      ++dest_formats_seen[c.dest_format];
      ++formats_seen[c.format];
      ++selects_seen[c.select];
      (c.exp_bias == 0 ? bias_zero : (c.exp_bias == 1 ? bias_one : bias_other))++;
      case_3d += c.is3d;
      case_swap += c.swap;
      case_dup += c.dup;
      begin();
      vkCmdFillBuffer(cmd, edram.buffer, 0, VK_WHOLE_SIZE, 0xDEADBEEFu);
      vkCmdFillBuffer(cmd, dest_a.buffer, 0, VK_WHOLE_SIZE, 0);
      vkCmdFillBuffer(cmd, dest_b.buffer, 0, VK_WHOLE_SIZE, 0);
      memory_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
      record_case(c, true, false);
      memory_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
      // Compare on the GPU.
      VkBufferCopy init_copy = {0, 0, 16};
      // (the result buffer is reset before the dispatches through a copy recorded first in a second command buffer pass: do it now, then compare)
      vkCmdCopyBuffer(cmd, result_init.buffer, result.buffer, 1, &init_copy);
      memory_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_compare);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_compare, 0, 1, &set_compare, 0, nullptr);
      const uint32_t words = uint32_t(dest_bytes / 4);
      vkCmdPushConstants(cmd, layout_compare, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &words);
      vkCmdDispatch(cmd, (words + 255) / 256, 1, 1);
      memory_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      VkBufferCopy out_copy = {0, 0, 16};
      vkCmdCopyBuffer(cmd, result.buffer, result_host.buffer, 1, &out_copy);
      memory_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
      submit_wait();
      const uint32_t* r = reinterpret_cast<const uint32_t*>(result_host.mapped);
      nonzero_total += r[2];
      if ((r[0] || std::getenv("HARNESS_PRINT_NONZERO")) && std::getenv("HARNESS_PRINT_DIFFS") && failures < 2 && iteration < 3) {
        // Debug: read both destinations back and list the first differing words (a = dump + copy, b = fused).
        const uint32_t want = uint32_t(std::atoi(std::getenv("HARNESS_PRINT_DIFFS")));
        Buffer host_a = MakeBuffer(dest_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true), host_b = MakeBuffer(dest_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        begin();
        VkBufferCopy whole = {0, 0, dest_bytes};
        vkCmdCopyBuffer(cmd, dest_a.buffer, host_a.buffer, 1, &whole);
        vkCmdCopyBuffer(cmd, dest_b.buffer, host_b.buffer, 1, &whole);
        submit_wait();
        const uint32_t* wa = reinterpret_cast<const uint32_t*>(host_a.mapped);
        const uint32_t* wb = reinterpret_cast<const uint32_t*>(host_b.mapped);
        uint32_t shown = 0;
        for (uint64_t i = 0; i < dest_bytes / 4 && shown < want; ++i) {
          if (wa[i] != wb[i]) {
            std::printf("  diff word %llu (16-byte slot %llu lane %u): two-pass %08x fused %08x\n", (unsigned long long)i, (unsigned long long)(i / 4), unsigned(i % 4), wa[i], wb[i]);
            ++shown;
          }
        }
        if (std::getenv("HARNESS_PRINT_NONZERO")) {
          shown = 0;
          for (uint64_t i = 0; i < dest_bytes / 4 && shown < want; ++i) {
            if (wa[i] != 0 || wb[i] != 0) {
              std::printf("  word %llu: two-pass %08x fused %08x\n", (unsigned long long)i, wa[i], wb[i]);
              ++shown;
            }
          }
        }
      }
      if (r[0]) {
        ++failures;
        nan_payload_words += r[3];
        total_diff_words += r[0];
        if (r[0] == r[3]) ++nan_payload_only;
        if (failures <= 8) {
          std::printf("MISMATCH iteration %u: %u words differ (%u of them NaN in both buffers), the lowest at word %u\n", iteration, r[0], r[3], r[1]);
          std::printf("  msaa %u depth %u format %u copy_pitch %u base %u rt_pitch %u rt_base %u span %u+%ux%u dup %u off(%u,%u) size_div8(%u,%u) endian %u 3d %u slice %u swap %u "
                      "dest_off(%u,%u) pitch32 %u height32 %u select %u mask %x dest_format %u exp_bias %d\n",
                      c.msaa, c.is_depth, c.format, c.copy_pitch, c.base, c.rt_pitch, c.rt_base, c.span_base, c.span_row_length, c.span_rows, c.dup, c.offx, c.offy, c.wdiv8,
                      c.hdiv8, c.endian, c.is3d, c.slice, c.swap, c.doffx, c.doffy, c.pitch32, c.height32, c.select, c.mask, c.dest_format, c.exp_bias);
        }
      } else if (r[2] == 0) {
        ++empty;
        if (std::getenv("HARNESS_VERBOSE_EMPTY") && empty <= 6) {
          std::printf("EMPTY iteration %u: msaa %u format %u span %u+%ux%u size_div8(%u,%u) 3d %u slice %u select %u dest_format %u exp_bias %d dest_off(%u,%u) pitch32 %u height32 %u\n", iteration, c.msaa, c.format,
                      c.span_base, c.span_row_length, c.span_rows, c.wdiv8, c.hdiv8, c.is3d, c.slice, c.select, c.dest_format, c.exp_bias, c.doffx, c.doffy, c.pitch32, c.height32);
        }
      }
    }
    std::printf("%u iterations, %u with a difference, %u wrote nothing; %.1f million non-zero destination words compared in all\n", iterations, failures, empty,
                nonzero_total / 1e6);
    if (failures) {
      std::printf("  differing words: %llu, of which NaN in both buffers (only the NaN payload differs): %llu; iterations with nothing but such words: %u of %u\n",
                  (unsigned long long)total_diff_words, (unsigned long long)nan_payload_words, nan_payload_only, failures);
    }
    if (full) {
      auto print_map = [](const char* what, const std::map<uint32_t, uint32_t>& m) {
        std::printf("  %s:", what);
        for (const auto& kv : m) std::printf(" %u x%u", kv.first, kv.second);
        std::printf("\n");
      };
      print_map("copy EDRAM formats", formats_seen);
      print_map("sample selects", selects_seen);
      print_map("destination formats", dest_formats_seen);
      std::printf("  exponent bias 0: %u, 1: %u, other: %u; 3D destinations %u, red/blue swap %u, first-pixel duplication %u\n", bias_zero, bias_one, bias_other, case_3d, case_swap,
                  case_dup);
    }
    return failures ? 1 : 0;
  }

  // ---- bench: a full-width G-buffer-band-like resolve, the dump + copy path against the fused one, GPU timestamps
  fill_image();
  const Case c = make_case(true);
  std::printf("bench case: %ux%u px region, copy_pitch %u, span %u+%ux%u tiles\n", c.wdiv8 * 8, c.hdiv8 * 8, c.copy_pitch, c.span_base, c.span_row_length, c.span_rows);
  double sum_two = 0, sum_fused = 0;
  const uint32_t reps = iterations;
  for (uint32_t rep = 0; rep < reps; ++rep) {
    begin();
    vkCmdResetQueryPool(cmd, query_pool, 0, 8);
    record_case(c, true, true);
    submit_wait();
    uint64_t ts[4];
    VKCHECK(vkGetQueryPoolResults(device, query_pool, 0, 4, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    if (rep >= 4) {
      sum_two += double(ts[1] - ts[0]) * timestamp_period * 1e-3;
      sum_fused += double(ts[3] - ts[2]) * timestamp_period * 1e-3;
    }
  }
  const double n = double(reps > 4 ? reps - 4 : 1);
  std::printf("dump + copy: %.1f us, fused: %.1f us per resolve (average of %u)\n", sum_two / n, sum_fused / n, uint32_t(n));
  return 0;
}
