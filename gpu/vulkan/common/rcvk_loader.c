/*
 * R-comp - Agent 4 (PS5_Vulkan integration).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rcvk_loader.h"

#include <stdio.h>

#define RCVK_DEFINE(name) PFN_##name rc_##name;
RCVK_GLOBAL_COMMANDS(RCVK_DEFINE)
RCVK_INSTANCE_COMMANDS(RCVK_DEFINE)
RCVK_OPTIONAL_INSTANCE_COMMANDS(RCVK_DEFINE)
RCVK_DEVICE_COMMANDS(RCVK_DEFINE)
RCVK_SWAPCHAIN_COMMANDS(RCVK_DEFINE)
#undef RCVK_DEFINE
PFN_vkEnumerateInstanceVersion rc_vkEnumerateInstanceVersion;

static int
missing(const char *name)
{
   fprintf(stderr, "rcvk: command not resolved: %s\n", name);
   return 1;
}

int
rcvk_load_global(void)
{
   int bad = 0;
#define LOAD(name)                                                                                 \
   rc_##name = (PFN_##name)vkGetInstanceProcAddr(VK_NULL_HANDLE, #name);                           \
   if (!rc_##name)                                                                                 \
      bad |= missing(#name);
   RCVK_GLOBAL_COMMANDS(LOAD)
#undef LOAD
   rc_vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
      VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
   return bad;
}

int
rcvk_load_instance(VkInstance instance)
{
   int bad = 0;
#define LOAD(name)                                                                                 \
   rc_##name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);                                 \
   if (!rc_##name)                                                                                 \
      bad |= missing(#name);
   RCVK_INSTANCE_COMMANDS(LOAD)
#undef LOAD
   return bad;
}

void
rcvk_load_instance_optional(VkInstance instance)
{
#define LOAD(name) rc_##name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);
   RCVK_OPTIONAL_INSTANCE_COMMANDS(LOAD)
#undef LOAD
}

int
rcvk_load_device(VkDevice device)
{
   int bad = 0;
#define LOAD(name)                                                                                 \
   rc_##name = (PFN_##name)rc_vkGetDeviceProcAddr(device, #name);                                  \
   if (!rc_##name)                                                                                 \
      bad |= missing(#name);
   RCVK_DEVICE_COMMANDS(LOAD)
#undef LOAD
   return bad;
}

int
rcvk_load_swapchain(VkDevice device)
{
   int bad = 0;
#define LOAD(name)                                                                                 \
   rc_##name = (PFN_##name)rc_vkGetDeviceProcAddr(device, #name);                                  \
   if (!rc_##name)                                                                                 \
      bad |= missing(#name);
   RCVK_SWAPCHAIN_COMMANDS(LOAD)
#undef LOAD
   return bad;
}

const char *
rcvk_result_name(VkResult r)
{
   switch (r) {
#define R(x)                                                                                       \
   case x:                                                                                         \
      return #x;
      R(VK_SUCCESS)
      R(VK_NOT_READY)
      R(VK_TIMEOUT)
      R(VK_EVENT_SET)
      R(VK_EVENT_RESET)
      R(VK_INCOMPLETE)
      R(VK_ERROR_OUT_OF_HOST_MEMORY)
      R(VK_ERROR_OUT_OF_DEVICE_MEMORY)
      R(VK_ERROR_INITIALIZATION_FAILED)
      R(VK_ERROR_DEVICE_LOST)
      R(VK_ERROR_MEMORY_MAP_FAILED)
      R(VK_ERROR_LAYER_NOT_PRESENT)
      R(VK_ERROR_EXTENSION_NOT_PRESENT)
      R(VK_ERROR_FEATURE_NOT_PRESENT)
      R(VK_ERROR_INCOMPATIBLE_DRIVER)
      R(VK_ERROR_TOO_MANY_OBJECTS)
      R(VK_ERROR_FORMAT_NOT_SUPPORTED)
      R(VK_ERROR_SURFACE_LOST_KHR)
      R(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
      R(VK_SUBOPTIMAL_KHR)
      R(VK_ERROR_OUT_OF_DATE_KHR)
#undef R
   default:
      return "VK_RESULT_OTHER";
   }
}
