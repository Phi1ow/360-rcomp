/* R-comp: opt-in entry-point adapter for Mihawk's statically linked RADV.
 * Integration reference: PS5_RetroArch@72847d6, src/radv_icd_ps5.c.
 * No driver code or advertised features are changed here.
 * Compile with RCOMP_VK_RADV and link RADV ONLY, never together with ps5vk.
 * This file is not part of the default (console-tested ps5vk) link recipe.
 */
#if defined(RCOMP_VK_RADV)
#include <stddef.h>
#include <vulkan/vulkan.h>

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    /* Preserve RADV's version/extension filtering, including NULL results. */
    return name ? vk_icdGetInstanceProcAddr(instance, name) : NULL;
}
#endif
