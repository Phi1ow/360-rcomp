/*
 * R-comp - Agent 4 (PS5_Vulkan integration).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * rcomp_vk_probe: report what the Vulkan implementation in front of us
 * exposes, as one JSON document.
 *
 *   rcomp_vk_probe [OUTPUT.json]      (stdout when no path is given)
 *
 * Rules this probe follows (gpu/vulkan/probe/README.md):
 *  - The instance version (vkEnumerateInstanceVersion; absent => 1.0) and each
 *    physical device's apiVersion are reported separately. They differ in
 *    general (a 1.3 loader in front of a 1.0 driver).
 *  - It enables only what it uses: VK_KHR_surface + VK_KHR_display, and only
 *    when both are listed, to enumerate displays and query a display-plane
 *    surface (the WSI path PS5_Vulkan provides: driver/ps5vk_wsi.c exposes
 *    VK_KHR_surface + VK_KHR_display on the instance, VK_KHR_swapchain on the
 *    device, VideoOut behind it). No layers. No device is created.
 *  - vkGetPhysicalDeviceFeatures2 is called only when both the instance
 *    version requested and the device apiVersion are >= 1.1, and a structure
 *    is chained only when the device version makes it valid
 *    (Vulkan11/12Features: >= 1.2, Vulkan13Features: >= 1.3). A 1.0 device is
 *    reported with 1.0 features only; VK_KHR_get_physical_device_properties2
 *    is not enabled for it, because there would be nothing valid to chain.
 *
 * Exit: 0 when the document was written (whatever it says), 1 when Vulkan
 * could not be reached at all (the document then carries "fatal").
 */
#include "rcvk_loader.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(RCOMP_TARGET_PS5)
#define RCOMP_TARGET_NAME "ps5"
#define RCOMP_EVIDENCE "PS5 (PS5_Vulkan linked statically)"
#else
#define RCOMP_TARGET_NAME "host"
#define RCOMP_EVIDENCE "HOST-ONLY: not evidence about PS5_Vulkan"
#endif

/* ---------------------------------------------------------------- JSON ---- */
static FILE *out;
static int depth;
static bool need_comma[64];

static void
indent(void)
{
   fputc('\n', out);
   for (int i = 0; i < depth; i++)
      fputs("  ", out);
}
static void
sep(void)
{
   if (need_comma[depth])
      fputc(',', out);
   need_comma[depth] = true;
   if (depth)
      indent();
}
static void
str(const char *s)
{
   fputc('"', out);
   for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
      if (*p == '"' || *p == '\\')
         fprintf(out, "\\%c", *p);
      else if (*p < 0x20)
         fprintf(out, "\\u%04x", *p);
      else
         fputc(*p, out);
   }
   fputc('"', out);
}
static void
key(const char *k)
{
   sep();
   if (k) {
      str(k);
      fputs(": ", out);
   }
}
static void
obj_begin(const char *k)
{
   key(k);
   fputc('{', out);
   need_comma[++depth] = false;
}
static void
obj_end(void)
{
   depth--;
   indent();
   fputc('}', out);
}
static void
arr_begin(const char *k)
{
   key(k);
   fputc('[', out);
   need_comma[++depth] = false;
}
static void
arr_end(void)
{
   depth--;
   indent();
   fputc(']', out);
}
static void
j_str(const char *k, const char *v)
{
   key(k);
   str(v);
}
static void
j_u64(const char *k, uint64_t v)
{
   key(k);
   fprintf(out, "%llu", (unsigned long long)v);
}
static void
j_i64(const char *k, int64_t v)
{
   key(k);
   fprintf(out, "%lld", (long long)v);
}
static void
j_hex(const char *k, uint64_t v)
{
   char buf[32];
   snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)v);
   j_str(k, buf);
}
static void
j_f(const char *k, double v)
{
   key(k);
   fprintf(out, "%.9g", v);
}
static void
j_bool(const char *k, bool v)
{
   key(k);
   fputs(v ? "true" : "false", out);
}
static void
j_version(const char *k, uint32_t v)
{
   char buf[48];
   snprintf(buf, sizeof buf, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
            VK_API_VERSION_PATCH(v));
   obj_begin(k);
   j_hex("raw", v);
   j_str("decoded", buf);
   obj_end();
}
static void
j_result(const char *k, VkResult r)
{
   char buf[96];
   snprintf(buf, sizeof buf, "%s (%d)", rcvk_result_name(r), (int)r);
   j_str(k, buf);
}

/* -------------------------------------------------------------- tables ---- */
#define FEATURES_10(X)                                                                             \
   X(robustBufferAccess)                                                                           \
   X(fullDrawIndexUint32)                                                                          \
   X(imageCubeArray)                                                                               \
   X(independentBlend)                                                                             \
   X(geometryShader)                                                                               \
   X(tessellationShader)                                                                           \
   X(sampleRateShading)                                                                            \
   X(dualSrcBlend)                                                                                 \
   X(logicOp)                                                                                      \
   X(multiDrawIndirect)                                                                            \
   X(drawIndirectFirstInstance)                                                                    \
   X(depthClamp)                                                                                   \
   X(depthBiasClamp)                                                                               \
   X(fillModeNonSolid)                                                                             \
   X(depthBounds)                                                                                  \
   X(wideLines)                                                                                    \
   X(largePoints)                                                                                  \
   X(alphaToOne)                                                                                   \
   X(multiViewport)                                                                                \
   X(samplerAnisotropy)                                                                            \
   X(textureCompressionETC2)                                                                       \
   X(textureCompressionASTC_LDR)                                                                   \
   X(textureCompressionBC)                                                                         \
   X(occlusionQueryPrecise)                                                                        \
   X(pipelineStatisticsQuery)                                                                      \
   X(vertexPipelineStoresAndAtomics)                                                               \
   X(fragmentStoresAndAtomics)                                                                     \
   X(shaderTessellationAndGeometryPointSize)                                                       \
   X(shaderImageGatherExtended)                                                                    \
   X(shaderStorageImageExtendedFormats)                                                            \
   X(shaderStorageImageMultisample)                                                                \
   X(shaderStorageImageReadWithoutFormat)                                                          \
   X(shaderStorageImageWriteWithoutFormat)                                                         \
   X(shaderUniformBufferArrayDynamicIndexing)                                                      \
   X(shaderSampledImageArrayDynamicIndexing)                                                       \
   X(shaderStorageBufferArrayDynamicIndexing)                                                      \
   X(shaderStorageImageArrayDynamicIndexing)                                                       \
   X(shaderClipDistance)                                                                           \
   X(shaderCullDistance)                                                                           \
   X(shaderFloat64)                                                                                \
   X(shaderInt64)                                                                                  \
   X(shaderInt16)                                                                                  \
   X(shaderResourceResidency)                                                                      \
   X(shaderResourceMinLod)                                                                         \
   X(sparseBinding)                                                                                \
   X(sparseResidencyBuffer)                                                                        \
   X(sparseResidencyImage2D)                                                                       \
   X(sparseResidencyImage3D)                                                                       \
   X(sparseResidency2Samples)                                                                      \
   X(sparseResidency4Samples)                                                                      \
   X(sparseResidency8Samples)                                                                      \
   X(sparseResidency16Samples)                                                                     \
   X(sparseResidencyAliased)                                                                       \
   X(variableMultisampleRate)                                                                      \
   X(inheritedQueries)

#define FEATURES_11(X)                                                                             \
   X(storageBuffer16BitAccess)                                                                     \
   X(uniformAndStorageBuffer16BitAccess)                                                           \
   X(storagePushConstant16)                                                                        \
   X(storageInputOutput16)                                                                         \
   X(multiview)                                                                                    \
   X(multiviewGeometryShader)                                                                      \
   X(multiviewTessellationShader)                                                                  \
   X(variablePointersStorageBuffer)                                                                \
   X(variablePointers)                                                                             \
   X(protectedMemory)                                                                              \
   X(samplerYcbcrConversion)                                                                       \
   X(shaderDrawParameters)

#define FEATURES_12(X)                                                                             \
   X(samplerMirrorClampToEdge)                                                                     \
   X(drawIndirectCount)                                                                            \
   X(storageBuffer8BitAccess)                                                                      \
   X(uniformAndStorageBuffer8BitAccess)                                                            \
   X(storagePushConstant8)                                                                         \
   X(shaderBufferInt64Atomics)                                                                     \
   X(shaderSharedInt64Atomics)                                                                     \
   X(shaderFloat16)                                                                                \
   X(shaderInt8)                                                                                   \
   X(descriptorIndexing)                                                                           \
   X(shaderInputAttachmentArrayDynamicIndexing)                                                    \
   X(shaderUniformTexelBufferArrayDynamicIndexing)                                                 \
   X(shaderStorageTexelBufferArrayDynamicIndexing)                                                 \
   X(shaderUniformBufferArrayNonUniformIndexing)                                                   \
   X(shaderSampledImageArrayNonUniformIndexing)                                                    \
   X(shaderStorageBufferArrayNonUniformIndexing)                                                   \
   X(shaderStorageImageArrayNonUniformIndexing)                                                    \
   X(shaderInputAttachmentArrayNonUniformIndexing)                                                 \
   X(shaderUniformTexelBufferArrayNonUniformIndexing)                                              \
   X(shaderStorageTexelBufferArrayNonUniformIndexing)                                              \
   X(descriptorBindingUniformBufferUpdateAfterBind)                                                \
   X(descriptorBindingSampledImageUpdateAfterBind)                                                 \
   X(descriptorBindingStorageImageUpdateAfterBind)                                                 \
   X(descriptorBindingStorageBufferUpdateAfterBind)                                                \
   X(descriptorBindingUniformTexelBufferUpdateAfterBind)                                           \
   X(descriptorBindingStorageTexelBufferUpdateAfterBind)                                           \
   X(descriptorBindingUpdateUnusedWhilePending)                                                    \
   X(descriptorBindingPartiallyBound)                                                              \
   X(descriptorBindingVariableDescriptorCount)                                                     \
   X(runtimeDescriptorArray)                                                                       \
   X(samplerFilterMinmax)                                                                          \
   X(scalarBlockLayout)                                                                            \
   X(imagelessFramebuffer)                                                                         \
   X(uniformBufferStandardLayout)                                                                  \
   X(shaderSubgroupExtendedTypes)                                                                  \
   X(separateDepthStencilLayouts)                                                                  \
   X(hostQueryReset)                                                                               \
   X(timelineSemaphore)                                                                            \
   X(bufferDeviceAddress)                                                                          \
   X(bufferDeviceAddressCaptureReplay)                                                             \
   X(bufferDeviceAddressMultiDevice)                                                               \
   X(vulkanMemoryModel)                                                                            \
   X(vulkanMemoryModelDeviceScope)                                                                 \
   X(vulkanMemoryModelAvailabilityVisibilityChains)                                                \
   X(shaderOutputViewportIndex)                                                                    \
   X(shaderOutputLayer)                                                                            \
   X(subgroupBroadcastDynamicId)

#define FEATURES_13(X)                                                                             \
   X(robustImageAccess)                                                                            \
   X(inlineUniformBlock)                                                                           \
   X(descriptorBindingInlineUniformBlockUpdateAfterBind)                                           \
   X(pipelineCreationCacheControl)                                                                 \
   X(privateData)                                                                                  \
   X(shaderDemoteToHelperInvocation)                                                               \
   X(shaderTerminateInvocation)                                                                    \
   X(subgroupSizeControl)                                                                          \
   X(computeFullSubgroups)                                                                         \
   X(synchronization2)                                                                             \
   X(textureCompressionASTC_HDR)                                                                   \
   X(shaderZeroInitializeWorkgroupMemory)                                                          \
   X(dynamicRendering)                                                                             \
   X(shaderIntegerDotProduct)                                                                      \
   X(maintenance4)

/* VkPhysicalDeviceLimits, every member, by type. */
#define LIMITS_U32(X)                                                                              \
   X(maxImageDimension1D)                                                                          \
   X(maxImageDimension2D)                                                                          \
   X(maxImageDimension3D)                                                                          \
   X(maxImageDimensionCube)                                                                        \
   X(maxImageArrayLayers)                                                                          \
   X(maxTexelBufferElements)                                                                       \
   X(maxUniformBufferRange)                                                                        \
   X(maxStorageBufferRange)                                                                        \
   X(maxPushConstantsSize)                                                                         \
   X(maxMemoryAllocationCount)                                                                     \
   X(maxSamplerAllocationCount)                                                                    \
   X(maxBoundDescriptorSets)                                                                       \
   X(maxPerStageDescriptorSamplers)                                                                \
   X(maxPerStageDescriptorUniformBuffers)                                                          \
   X(maxPerStageDescriptorStorageBuffers)                                                          \
   X(maxPerStageDescriptorSampledImages)                                                           \
   X(maxPerStageDescriptorStorageImages)                                                           \
   X(maxPerStageDescriptorInputAttachments)                                                        \
   X(maxPerStageResources)                                                                         \
   X(maxDescriptorSetSamplers)                                                                     \
   X(maxDescriptorSetUniformBuffers)                                                               \
   X(maxDescriptorSetUniformBuffersDynamic)                                                        \
   X(maxDescriptorSetStorageBuffers)                                                               \
   X(maxDescriptorSetStorageBuffersDynamic)                                                        \
   X(maxDescriptorSetSampledImages)                                                                \
   X(maxDescriptorSetStorageImages)                                                                \
   X(maxDescriptorSetInputAttachments)                                                             \
   X(maxVertexInputAttributes)                                                                     \
   X(maxVertexInputBindings)                                                                       \
   X(maxVertexInputAttributeOffset)                                                                \
   X(maxVertexInputBindingStride)                                                                  \
   X(maxVertexOutputComponents)                                                                    \
   X(maxTessellationGenerationLevel)                                                               \
   X(maxTessellationPatchSize)                                                                     \
   X(maxTessellationControlPerVertexInputComponents)                                               \
   X(maxTessellationControlPerVertexOutputComponents)                                              \
   X(maxTessellationControlPerPatchOutputComponents)                                               \
   X(maxTessellationControlTotalOutputComponents)                                                  \
   X(maxTessellationEvaluationInputComponents)                                                     \
   X(maxTessellationEvaluationOutputComponents)                                                    \
   X(maxGeometryShaderInvocations)                                                                 \
   X(maxGeometryInputComponents)                                                                   \
   X(maxGeometryOutputComponents)                                                                  \
   X(maxGeometryOutputVertices)                                                                    \
   X(maxGeometryTotalOutputComponents)                                                             \
   X(maxFragmentInputComponents)                                                                   \
   X(maxFragmentOutputAttachments)                                                                 \
   X(maxFragmentDualSrcAttachments)                                                                \
   X(maxFragmentCombinedOutputResources)                                                           \
   X(maxComputeSharedMemorySize)                                                                   \
   X(maxComputeWorkGroupInvocations)                                                               \
   X(subPixelPrecisionBits)                                                                        \
   X(subTexelPrecisionBits)                                                                        \
   X(mipmapPrecisionBits)                                                                          \
   X(maxDrawIndexedIndexValue)                                                                     \
   X(maxDrawIndirectCount)                                                                         \
   X(maxViewports)                                                                                 \
   X(viewportSubPixelBits)                                                                         \
   X(maxTexelGatherOffset)                                                                         \
   X(subPixelInterpolationOffsetBits)                                                              \
   X(maxFramebufferWidth)                                                                          \
   X(maxFramebufferHeight)                                                                         \
   X(maxFramebufferLayers)                                                                         \
   X(maxColorAttachments)                                                                          \
   X(maxSampleMaskWords)                                                                           \
   X(maxClipDistances)                                                                             \
   X(maxCullDistances)                                                                             \
   X(maxCombinedClipAndCullDistances)                                                              \
   X(discreteQueuePriorities)                                                                      \
   X(maxTexelOffset)
#define LIMITS_I32(X)                                                                              \
   X(minTexelOffset)                                                                               \
   X(minTexelGatherOffset)
#define LIMITS_F32(X)                                                                              \
   X(maxSamplerLodBias)                                                                            \
   X(maxSamplerAnisotropy)                                                                         \
   X(minInterpolationOffset)                                                                       \
   X(maxInterpolationOffset)                                                                       \
   X(timestampPeriod)                                                                              \
   X(pointSizeGranularity)                                                                         \
   X(lineWidthGranularity)
#define LIMITS_U64(X)                                                                              \
   X(bufferImageGranularity)                                                                       \
   X(sparseAddressSpaceSize)                                                                       \
   X(minMemoryMapAlignment)                                                                        \
   X(minTexelBufferOffsetAlignment)                                                                \
   X(minUniformBufferOffsetAlignment)                                                              \
   X(minStorageBufferOffsetAlignment)                                                              \
   X(optimalBufferCopyOffsetAlignment)                                                             \
   X(optimalBufferCopyRowPitchAlignment)                                                           \
   X(nonCoherentAtomSize)
#define LIMITS_FLAGS(X)                                                                            \
   X(framebufferColorSampleCounts)                                                                 \
   X(framebufferDepthSampleCounts)                                                                 \
   X(framebufferStencilSampleCounts)                                                               \
   X(framebufferNoAttachmentsSampleCounts)                                                         \
   X(sampledImageColorSampleCounts)                                                                \
   X(sampledImageIntegerSampleCounts)                                                              \
   X(sampledImageDepthSampleCounts)                                                                \
   X(sampledImageStencilSampleCounts)                                                              \
   X(storageImageSampleCounts)
#define LIMITS_BOOL(X)                                                                             \
   X(timestampComputeAndGraphics)                                                                  \
   X(strictLines)                                                                                  \
   X(standardSampleLocations)

/* The documented format list (probe/README.md): render targets and the
 * swapchain format, depth/stencil, the BC formats Xenos titles use, and the
 * vertex/texel formats a translated Xenos pipeline needs first. */
#define PROBE_FORMATS(X)                                                                           \
   X(VK_FORMAT_R8G8B8A8_UNORM)                                                                     \
   X(VK_FORMAT_R8G8B8A8_SRGB)                                                                      \
   X(VK_FORMAT_R8G8B8A8_SNORM)                                                                     \
   X(VK_FORMAT_R8G8B8A8_UINT)                                                                      \
   X(VK_FORMAT_B8G8R8A8_UNORM)                                                                     \
   X(VK_FORMAT_B8G8R8A8_SRGB)                                                                      \
   X(VK_FORMAT_A2B10G10R10_UNORM_PACK32)                                                           \
   X(VK_FORMAT_A2R10G10B10_UNORM_PACK32)                                                           \
   X(VK_FORMAT_B10G11R11_UFLOAT_PACK32)                                                            \
   X(VK_FORMAT_R5G6B5_UNORM_PACK16)                                                                \
   X(VK_FORMAT_B5G6R5_UNORM_PACK16)                                                                \
   X(VK_FORMAT_A1R5G5B5_UNORM_PACK16)                                                              \
   X(VK_FORMAT_R4G4B4A4_UNORM_PACK16)                                                              \
   X(VK_FORMAT_R8_UNORM)                                                                           \
   X(VK_FORMAT_R8G8_UNORM)                                                                         \
   X(VK_FORMAT_R16_UNORM)                                                                          \
   X(VK_FORMAT_R16G16_UNORM)                                                                       \
   X(VK_FORMAT_R16G16_SNORM)                                                                       \
   X(VK_FORMAT_R16G16B16A16_UNORM)                                                                 \
   X(VK_FORMAT_R16_SFLOAT)                                                                         \
   X(VK_FORMAT_R16G16_SFLOAT)                                                                      \
   X(VK_FORMAT_R16G16B16A16_SFLOAT)                                                                \
   X(VK_FORMAT_R32_UINT)                                                                           \
   X(VK_FORMAT_R32_SFLOAT)                                                                         \
   X(VK_FORMAT_R32G32_SFLOAT)                                                                      \
   X(VK_FORMAT_R32G32B32_SFLOAT)                                                                   \
   X(VK_FORMAT_R32G32B32A32_SFLOAT)                                                                \
   X(VK_FORMAT_D16_UNORM)                                                                          \
   X(VK_FORMAT_X8_D24_UNORM_PACK32)                                                                \
   X(VK_FORMAT_D24_UNORM_S8_UINT)                                                                  \
   X(VK_FORMAT_D32_SFLOAT)                                                                         \
   X(VK_FORMAT_D32_SFLOAT_S8_UINT)                                                                 \
   X(VK_FORMAT_S8_UINT)                                                                            \
   X(VK_FORMAT_BC1_RGB_UNORM_BLOCK)                                                                \
   X(VK_FORMAT_BC1_RGBA_UNORM_BLOCK)                                                               \
   X(VK_FORMAT_BC1_RGBA_SRGB_BLOCK)                                                                \
   X(VK_FORMAT_BC2_UNORM_BLOCK)                                                                    \
   X(VK_FORMAT_BC3_UNORM_BLOCK)                                                                    \
   X(VK_FORMAT_BC3_SRGB_BLOCK)                                                                     \
   X(VK_FORMAT_BC4_UNORM_BLOCK)                                                                    \
   X(VK_FORMAT_BC5_UNORM_BLOCK)                                                                    \
   X(VK_FORMAT_BC7_UNORM_BLOCK)

#define FORMAT_FEATURE_BITS(X)                                                                     \
   X(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "SAMPLED_IMAGE")                                         \
   X(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "STORAGE_IMAGE")                                         \
   X(VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT, "STORAGE_IMAGE_ATOMIC")                           \
   X(VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, "UNIFORM_TEXEL_BUFFER")                           \
   X(VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT, "STORAGE_TEXEL_BUFFER")                           \
   X(VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT, "STORAGE_TEXEL_BUFFER_ATOMIC")             \
   X(VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, "VERTEX_BUFFER")                                         \
   X(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "COLOR_ATTACHMENT")                                   \
   X(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, "COLOR_ATTACHMENT_BLEND")                       \
   X(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "DEPTH_STENCIL_ATTACHMENT")                   \
   X(VK_FORMAT_FEATURE_BLIT_SRC_BIT, "BLIT_SRC")                                                   \
   X(VK_FORMAT_FEATURE_BLIT_DST_BIT, "BLIT_DST")                                                   \
   X(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, "SAMPLED_IMAGE_FILTER_LINEAR")             \
   X(VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, "TRANSFER_SRC")                                           \
   X(VK_FORMAT_FEATURE_TRANSFER_DST_BIT, "TRANSFER_DST")

static void
j_format_features(const char *k, VkFormatFeatureFlags f)
{
   obj_begin(k);
   j_hex("raw", f);
   arr_begin("bits");
#define B(bit, name)                                                                               \
   if (f & (bit))                                                                                  \
      j_str(NULL, name);
   FORMAT_FEATURE_BITS(B)
#undef B
   arr_end();
   obj_end();
}

static bool
has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
   for (uint32_t i = 0; i < n; i++)
      if (strcmp(e[i].extensionName, name) == 0)
         return true;
   return false;
}

static void
j_extensions(const char *k, const VkExtensionProperties *e, uint32_t n)
{
   arr_begin(k);
   for (uint32_t i = 0; i < n; i++) {
      obj_begin(NULL);
      j_str("name", e[i].extensionName);
      j_u64("specVersion", e[i].specVersion);
      obj_end();
   }
   arr_end();
}

static const char *
device_type_name(VkPhysicalDeviceType t)
{
   switch (t) {
   case VK_PHYSICAL_DEVICE_TYPE_OTHER: return "OTHER";
   case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "INTEGRATED_GPU";
   case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "DISCRETE_GPU";
   case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "VIRTUAL_GPU";
   case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
   default: return "UNKNOWN";
   }
}

/* ------------------------------------------------------ presentation ---- */
static void
probe_presentation(VkInstance instance, VkPhysicalDevice pd, uint32_t queue_family_count,
                   bool display_enabled)
{
   obj_begin("presentation");
   j_str("wsi_path", "VK_KHR_surface + VK_KHR_display (PS5_Vulkan driver/ps5vk_wsi.c: VideoOut)");
   if (!display_enabled) {
      j_bool("available", false);
      j_str("reason", "instance does not list both VK_KHR_surface and VK_KHR_display");
      obj_end();
      return;
   }
   if (!rc_vkGetPhysicalDeviceDisplayPropertiesKHR || !rc_vkGetDisplayModePropertiesKHR ||
       !rc_vkGetPhysicalDeviceDisplayPlanePropertiesKHR ||
       !rc_vkGetDisplayPlaneSupportedDisplaysKHR || !rc_vkCreateDisplayPlaneSurfaceKHR ||
       !rc_vkDestroySurfaceKHR || !rc_vkGetPhysicalDeviceSurfaceSupportKHR) {
      j_bool("available", false);
      j_str("reason", "VK_KHR_display / VK_KHR_surface commands not resolved");
      obj_end();
      return;
   }
   uint32_t nd = 0;
   VkResult r = rc_vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &nd, NULL);
   j_result("vkGetPhysicalDeviceDisplayPropertiesKHR", r);
   VkDisplayPropertiesKHR *dp = calloc(nd ? nd : 1, sizeof *dp);
   if (r == VK_SUCCESS && nd)
      r = rc_vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &nd, dp);
   VkDisplayKHR first_display = VK_NULL_HANDLE;
   VkDisplayModeKHR first_mode = VK_NULL_HANDLE;
   VkExtent2D first_extent = {0, 0};
   arr_begin("displays");
   for (uint32_t i = 0; r == VK_SUCCESS && i < nd; i++) {
      obj_begin(NULL);
      j_str("displayName", dp[i].displayName ? dp[i].displayName : "");
      j_u64("physicalWidth", dp[i].physicalDimensions.width);
      j_u64("physicalHeight", dp[i].physicalDimensions.height);
      j_u64("resolutionWidth", dp[i].physicalResolution.width);
      j_u64("resolutionHeight", dp[i].physicalResolution.height);
      j_hex("supportedTransforms", dp[i].supportedTransforms);
      j_bool("planeReorderPossible", dp[i].planeReorderPossible);
      j_bool("persistentContent", dp[i].persistentContent);
      uint32_t nm = 0;
      VkResult rm = rc_vkGetDisplayModePropertiesKHR(pd, dp[i].display, &nm, NULL);
      VkDisplayModePropertiesKHR *mp = calloc(nm ? nm : 1, sizeof *mp);
      if (rm == VK_SUCCESS && nm)
         rm = rc_vkGetDisplayModePropertiesKHR(pd, dp[i].display, &nm, mp);
      j_result("vkGetDisplayModePropertiesKHR", rm);
      arr_begin("modes");
      for (uint32_t m = 0; rm == VK_SUCCESS && m < nm; m++) {
         obj_begin(NULL);
         j_u64("width", mp[m].parameters.visibleRegion.width);
         j_u64("height", mp[m].parameters.visibleRegion.height);
         j_u64("refreshRate_mHz", mp[m].parameters.refreshRate);
         obj_end();
         if (first_mode == VK_NULL_HANDLE) {
            first_display = dp[i].display;
            first_mode = mp[m].displayMode;
            first_extent = mp[m].parameters.visibleRegion;
         }
      }
      arr_end();
      free(mp);
      obj_end();
   }
   arr_end();
   free(dp);

   uint32_t np = 0;
   r = rc_vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &np, NULL);
   j_result("vkGetPhysicalDeviceDisplayPlanePropertiesKHR", r);
   j_u64("planeCount", np);
   uint32_t plane = UINT32_MAX;
   for (uint32_t p = 0; r == VK_SUCCESS && p < np && first_display && plane == UINT32_MAX; p++) {
      uint32_t ns = 0;
      if (rc_vkGetDisplayPlaneSupportedDisplaysKHR(pd, p, &ns, NULL) != VK_SUCCESS || !ns)
         continue;
      VkDisplayKHR *sd = calloc(ns, sizeof *sd);
      if (rc_vkGetDisplayPlaneSupportedDisplaysKHR(pd, p, &ns, sd) == VK_SUCCESS)
         for (uint32_t s = 0; s < ns; s++)
            if (sd[s] == first_display)
               plane = p;
      free(sd);
   }

   if (first_mode == VK_NULL_HANDLE || plane == UINT32_MAX) {
      j_bool("available", false);
      j_str("reason", first_mode == VK_NULL_HANDLE ? "no display with a mode"
                                                   : "no plane supports the first display");
      obj_end();
      return;
   }
   VkDisplaySurfaceCreateInfoKHR sci = {
      .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
      .displayMode = first_mode,
      .planeIndex = plane,
      .planeStackIndex = 0,
      .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .globalAlpha = 1.0f,
      .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
      .imageExtent = first_extent,
   };
   VkSurfaceKHR surface = VK_NULL_HANDLE;
   r = rc_vkCreateDisplayPlaneSurfaceKHR(instance, &sci, NULL, &surface);
   j_result("vkCreateDisplayPlaneSurfaceKHR", r);
   j_u64("surfacePlaneIndex", plane);
   if (r != VK_SUCCESS) {
      j_bool("available", false);
      j_str("reason", "display-plane surface creation failed");
      obj_end();
      return;
   }
   arr_begin("queueFamilySupportsPresent");
   bool any = false;
   for (uint32_t q = 0; q < queue_family_count; q++) {
      VkBool32 s = VK_FALSE;
      VkResult rs = rc_vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface, &s);
      obj_begin(NULL);
      j_u64("queueFamily", q);
      j_result("result", rs);
      j_bool("supported", rs == VK_SUCCESS && s);
      obj_end();
      any |= rs == VK_SUCCESS && s;
   }
   arr_end();
   if (rc_vkGetPhysicalDeviceSurfaceCapabilitiesKHR) {
      VkSurfaceCapabilitiesKHR c;
      memset(&c, 0, sizeof c);
      VkResult rc = rc_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &c);
      obj_begin("surfaceCapabilities");
      j_result("result", rc);
      if (rc == VK_SUCCESS) {
         j_u64("minImageCount", c.minImageCount);
         j_u64("maxImageCount", c.maxImageCount);
         j_u64("currentExtentWidth", c.currentExtent.width);
         j_u64("currentExtentHeight", c.currentExtent.height);
         j_u64("maxImageArrayLayers", c.maxImageArrayLayers);
         j_hex("supportedTransforms", c.supportedTransforms);
         j_hex("supportedCompositeAlpha", c.supportedCompositeAlpha);
         j_hex("supportedUsageFlags", c.supportedUsageFlags);
      }
      obj_end();
   }
   if (rc_vkGetPhysicalDeviceSurfaceFormatsKHR) {
      uint32_t nf = 0;
      VkResult rf = rc_vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nf, NULL);
      VkSurfaceFormatKHR *sf = calloc(nf ? nf : 1, sizeof *sf);
      if (rf == VK_SUCCESS && nf)
         rf = rc_vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nf, sf);
      arr_begin("surfaceFormats");
      for (uint32_t i = 0; rf == VK_SUCCESS && i < nf; i++) {
         obj_begin(NULL);
         j_u64("format", sf[i].format);
         j_u64("colorSpace", sf[i].colorSpace);
         obj_end();
      }
      arr_end();
      free(sf);
   }
   if (rc_vkGetPhysicalDeviceSurfacePresentModesKHR) {
      uint32_t nm = 0;
      VkResult rp = rc_vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &nm, NULL);
      VkPresentModeKHR *pm = calloc(nm ? nm : 1, sizeof *pm);
      if (rp == VK_SUCCESS && nm)
         rp = rc_vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &nm, pm);
      arr_begin("presentModes");
      for (uint32_t i = 0; rp == VK_SUCCESS && i < nm; i++)
         j_u64(NULL, pm[i]);
      arr_end();
      free(pm);
   }
   rc_vkDestroySurfaceKHR(instance, surface, NULL);
   j_bool("available", any);
   if (!any)
      j_str("reason", "no queue family supports presenting to the display surface");
   obj_end();
}

/* --------------------------------------------------------------- device ---- */
static void
probe_device(VkInstance instance, VkPhysicalDevice pd, uint32_t instance_api, bool display_enabled)
{
   VkPhysicalDeviceProperties p;
   rc_vkGetPhysicalDeviceProperties(pd, &p);
   obj_begin(NULL);
   j_str("deviceName", p.deviceName);
   j_version("apiVersion", p.apiVersion);
   j_hex("driverVersion", p.driverVersion);
   j_hex("vendorID", p.vendorID);
   j_hex("deviceID", p.deviceID);
   j_str("deviceType", device_type_name(p.deviceType));
   {
      char uuid[VK_UUID_SIZE * 2 + 1];
      for (unsigned i = 0; i < VK_UUID_SIZE; i++)
         snprintf(uuid + 2 * i, 3, "%02x", p.pipelineCacheUUID[i]);
      j_str("pipelineCacheUUID", uuid);
   }

   uint32_t ne = 0;
   VkResult r = rc_vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
   VkExtensionProperties *de = calloc(ne ? ne : 1, sizeof *de);
   if (r == VK_SUCCESS && ne)
      r = rc_vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, de);
   j_result("vkEnumerateDeviceExtensionProperties", r);
   j_extensions("deviceExtensions", de, r == VK_SUCCESS ? ne : 0);
   free(de);

   VkPhysicalDeviceFeatures f;
   rc_vkGetPhysicalDeviceFeatures(pd, &f);
   obj_begin("features10");
#define F(n) j_bool(#n, f.n);
   FEATURES_10(F)
#undef F
   obj_end();

   /* Features2 only when the versions allow it; see the header comment. */
   obj_begin("features2");
   const uint32_t dev_minor = VK_API_VERSION_MINOR(p.apiVersion);
   const uint32_t inst_minor = VK_API_VERSION_MINOR(instance_api);
   if (VK_API_VERSION_MAJOR(p.apiVersion) == 1 && dev_minor >= 1 && inst_minor >= 1 &&
       rc_vkGetPhysicalDeviceFeatures2) {
      VkPhysicalDeviceVulkan11Features f11 = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
      VkPhysicalDeviceVulkan12Features f12 = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
      VkPhysicalDeviceVulkan13Features f13 = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
      VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      /* Vulkan11/12Features are valid from 1.2, Vulkan13Features from 1.3;
       * and the instance must have been created at least at that version. */
      const uint32_t chain_minor = dev_minor < inst_minor ? dev_minor : inst_minor;
      void **tail = &f2.pNext;
      if (chain_minor >= 2) {
         *tail = &f11;
         tail = &f11.pNext;
         *tail = &f12;
         tail = &f12.pNext;
      }
      if (chain_minor >= 3) {
         *tail = &f13;
         tail = &f13.pNext;
      }
      rc_vkGetPhysicalDeviceFeatures2(pd, &f2);
      j_bool("used", true);
      arr_begin("chained");
      if (chain_minor >= 2) {
         j_str(NULL, "VkPhysicalDeviceVulkan11Features");
         j_str(NULL, "VkPhysicalDeviceVulkan12Features");
      }
      if (chain_minor >= 3)
         j_str(NULL, "VkPhysicalDeviceVulkan13Features");
      arr_end();
      if (chain_minor >= 2) {
         obj_begin("vulkan11");
#define F(n) j_bool(#n, f11.n);
         FEATURES_11(F)
#undef F
         obj_end();
         obj_begin("vulkan12");
#define F(n) j_bool(#n, f12.n);
         FEATURES_12(F)
#undef F
         obj_end();
      }
      if (chain_minor >= 3) {
         obj_begin("vulkan13");
#define F(n) j_bool(#n, f13.n);
         FEATURES_13(F)
#undef F
         obj_end();
      }
      if (chain_minor == 1)
         j_str("note", "1.1: base structure only; the per-feature 1.1 structures are not chained");
   } else {
      j_bool("used", false);
      j_str("reason", "device apiVersion or requested instance apiVersion is 1.0; "
                      "nothing valid to chain, VK_KHR_get_physical_device_properties2 "
                      "therefore not enabled");
   }
   obj_end();

   obj_begin("limits");
#define L(n) j_u64(#n, p.limits.n);
   LIMITS_U32(L)
   LIMITS_U64(L)
#undef L
#define L(n) j_i64(#n, p.limits.n);
   LIMITS_I32(L)
#undef L
#define L(n) j_f(#n, p.limits.n);
   LIMITS_F32(L)
#undef L
#define L(n) j_hex(#n, p.limits.n);
   LIMITS_FLAGS(L)
#undef L
#define L(n) j_bool(#n, p.limits.n);
   LIMITS_BOOL(L)
#undef L
   arr_begin("maxComputeWorkGroupCount");
   for (int i = 0; i < 3; i++)
      j_u64(NULL, p.limits.maxComputeWorkGroupCount[i]);
   arr_end();
   arr_begin("maxComputeWorkGroupSize");
   for (int i = 0; i < 3; i++)
      j_u64(NULL, p.limits.maxComputeWorkGroupSize[i]);
   arr_end();
   arr_begin("maxViewportDimensions");
   for (int i = 0; i < 2; i++)
      j_u64(NULL, p.limits.maxViewportDimensions[i]);
   arr_end();
   arr_begin("viewportBoundsRange");
   for (int i = 0; i < 2; i++)
      j_f(NULL, p.limits.viewportBoundsRange[i]);
   arr_end();
   arr_begin("pointSizeRange");
   for (int i = 0; i < 2; i++)
      j_f(NULL, p.limits.pointSizeRange[i]);
   arr_end();
   arr_begin("lineWidthRange");
   for (int i = 0; i < 2; i++)
      j_f(NULL, p.limits.lineWidthRange[i]);
   arr_end();
   obj_end();

   obj_begin("sparseProperties");
   j_bool("residencyStandard2DBlockShape", p.sparseProperties.residencyStandard2DBlockShape);
   j_bool("residencyStandard2DMultisampleBlockShape",
          p.sparseProperties.residencyStandard2DMultisampleBlockShape);
   j_bool("residencyStandard3DBlockShape", p.sparseProperties.residencyStandard3DBlockShape);
   j_bool("residencyAlignedMipSize", p.sparseProperties.residencyAlignedMipSize);
   j_bool("residencyNonResidentStrict", p.sparseProperties.residencyNonResidentStrict);
   obj_end();

   VkPhysicalDeviceMemoryProperties mp;
   rc_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   obj_begin("memory");
   arr_begin("heaps");
   for (uint32_t i = 0; i < mp.memoryHeapCount; i++) {
      obj_begin(NULL);
      j_u64("size", mp.memoryHeaps[i].size);
      j_hex("flags", mp.memoryHeaps[i].flags);
      obj_end();
   }
   arr_end();
   arr_begin("types");
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      obj_begin(NULL);
      j_u64("heapIndex", mp.memoryTypes[i].heapIndex);
      j_hex("propertyFlags", mp.memoryTypes[i].propertyFlags);
      obj_end();
   }
   arr_end();
   obj_end();

   uint32_t nq = 0;
   rc_vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties *qf = calloc(nq ? nq : 1, sizeof *qf);
   rc_vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   arr_begin("queueFamilies");
   for (uint32_t i = 0; i < nq; i++) {
      obj_begin(NULL);
      j_u64("index", i);
      j_hex("queueFlags", qf[i].queueFlags);
      j_bool("graphics", qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT);
      j_bool("compute", qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT);
      j_bool("transfer", qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT);
      j_u64("queueCount", qf[i].queueCount);
      j_u64("timestampValidBits", qf[i].timestampValidBits);
      j_u64("minImageTransferGranularityWidth", qf[i].minImageTransferGranularity.width);
      j_u64("minImageTransferGranularityHeight", qf[i].minImageTransferGranularity.height);
      j_u64("minImageTransferGranularityDepth", qf[i].minImageTransferGranularity.depth);
      obj_end();
   }
   arr_end();
   free(qf);

   obj_begin("formats");
#define FMT(fmt)                                                                                   \
   {                                                                                               \
      VkFormatProperties fp;                                                                       \
      memset(&fp, 0, sizeof fp);                                                                   \
      rc_vkGetPhysicalDeviceFormatProperties(pd, fmt, &fp);                                        \
      obj_begin(&(#fmt)[10]);                                                                     \
      j_u64("vkFormat", fmt);                                                                      \
      j_format_features("linearTilingFeatures", fp.linearTilingFeatures);                          \
      j_format_features("optimalTilingFeatures", fp.optimalTilingFeatures);                        \
      j_format_features("bufferFeatures", fp.bufferFeatures);                                      \
      obj_end();                                                                                   \
   }
   PROBE_FORMATS(FMT)
#undef FMT
   obj_end();

   probe_presentation(instance, pd, nq, display_enabled);
   obj_end();
}

/* ----------------------------------------------------------------- main ---- */
int
main(int argc, char **argv)
{
   out = stdout;
   if (argc > 1) {
      out = fopen(argv[1], "w");
      if (!out) {
         perror(argv[1]);
         return 1;
      }
   }
   int status = 0;
   fputc('{', out);
   need_comma[depth = 1] = false;
   j_str("schema", "rcomp-vk-probe-v1");
   j_str("target", RCOMP_TARGET_NAME);
   j_str("evidence", RCOMP_EVIDENCE);
   j_version("headerVersion", VK_HEADER_VERSION_COMPLETE);

   if (rcvk_load_global()) {
      j_str("fatal", "global commands not resolved through vkGetInstanceProcAddr");
      status = 1;
      goto done;
   }

   uint32_t instance_version = VK_API_VERSION_1_0;
   obj_begin("instanceVersion");
   if (rc_vkEnumerateInstanceVersion) {
      VkResult r = rc_vkEnumerateInstanceVersion(&instance_version);
      j_result("vkEnumerateInstanceVersion", r);
      j_str("source", "vkEnumerateInstanceVersion");
   } else {
      j_str("source", "vkEnumerateInstanceVersion absent: implementation is Vulkan 1.0");
   }
   j_version("version", instance_version);
   obj_end();

   uint32_t nl = 0;
   rc_vkEnumerateInstanceLayerProperties(&nl, NULL);
   j_u64("instanceLayerCount", nl);

   uint32_t ne = 0;
   VkResult r = rc_vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
   VkExtensionProperties *ie = calloc(ne ? ne : 1, sizeof *ie);
   if (r == VK_SUCCESS && ne)
      r = rc_vkEnumerateInstanceExtensionProperties(NULL, &ne, ie);
   j_result("vkEnumerateInstanceExtensionProperties", r);
   if (r != VK_SUCCESS)
      ne = 0;
   j_extensions("instanceExtensions", ie, ne);

   const bool display = has_ext(ie, ne, VK_KHR_SURFACE_EXTENSION_NAME) &&
                        has_ext(ie, ne, VK_KHR_DISPLAY_EXTENSION_NAME);
   const char *enabled[2];
   uint32_t n_enabled = 0;
   if (display) {
      enabled[n_enabled++] = VK_KHR_SURFACE_EXTENSION_NAME;
      enabled[n_enabled++] = VK_KHR_DISPLAY_EXTENSION_NAME;
   }
   free(ie);

   /* A 1.0 implementation may refuse any other apiVersion
    * (VK_ERROR_INCOMPATIBLE_DRIVER); a newer one gets what it reports, capped at
    * the newest version whose structures this probe knows. */
   uint32_t request = VK_API_VERSION_1_0;
   if (VK_API_VERSION_MINOR(instance_version) >= 1)
      request = instance_version > VK_API_VERSION_1_3 ? VK_API_VERSION_1_3
                                                      : VK_MAKE_API_VERSION(0, 1,
                                                           VK_API_VERSION_MINOR(instance_version), 0);
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "rcomp_vk_probe",
      .applicationVersion = 1,
      .pEngineName = "R-comp",
      .engineVersion = 1,
      .apiVersion = request,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = n_enabled,
      .ppEnabledExtensionNames = enabled,
   };
   obj_begin("instanceCreate");
   j_version("requestedApiVersion", request);
   arr_begin("enabledExtensions");
   for (uint32_t i = 0; i < n_enabled; i++)
      j_str(NULL, enabled[i]);
   arr_end();
   j_u64("enabledLayerCount", 0);
   VkInstance instance = VK_NULL_HANDLE;
   r = rc_vkCreateInstance(&ici, NULL, &instance);
   j_result("result", r);
   obj_end();
   if (r != VK_SUCCESS) {
      j_str("fatal", "vkCreateInstance failed");
      status = 1;
      goto done;
   }
   if (rcvk_load_instance(instance)) {
      j_str("fatal", "instance commands not resolved");
      status = 1;
      goto done;
   }
   rcvk_load_instance_optional(instance);
   if (VK_API_VERSION_MINOR(request) < 1)
      rc_vkGetPhysicalDeviceFeatures2 = NULL; /* core 1.1 only; the KHR alias is not enabled */

   uint32_t npd = 0;
   r = rc_vkEnumeratePhysicalDevices(instance, &npd, NULL);
   VkPhysicalDevice *pds = calloc(npd ? npd : 1, sizeof *pds);
   if (r == VK_SUCCESS && npd)
      r = rc_vkEnumeratePhysicalDevices(instance, &npd, pds);
   j_result("vkEnumeratePhysicalDevices", r);
   arr_begin("physicalDevices");
   for (uint32_t i = 0; r == VK_SUCCESS && i < npd; i++)
      probe_device(instance, pds[i], request, display);
   arr_end();
   free(pds);
   rc_vkDestroyInstance(instance, NULL);

done:
   depth = 0;
   fputs("\n}\n", out);
   if (out != stdout)
      fclose(out);
   return status;
}
