// Host boundary double for the actual production radv_icd_bridge.c.
// Does not implement Vulkan or prove RADV/PS5 execution.
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static VkInstance TESTDOUBLE_seen_instance;
static unsigned TESTDOUBLE_calls;
static void VKAPI_CALL TESTDOUBLE_command(void) {}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name) {
    TESTDOUBLE_seen_instance = instance;
    ++TESTDOUBLE_calls;
    return strcmp(name, "vkTESTDOUBLE_supported") == 0 ? TESTDOUBLE_command : NULL;
}

int main(void) {
    VkInstance instance = (VkInstance)(uintptr_t)0x1234;
    int errors = 0;
    errors += vkGetInstanceProcAddr(instance, NULL) != NULL || TESTDOUBLE_calls != 0;
    errors += vkGetInstanceProcAddr(instance, "vkTESTDOUBLE_supported") != TESTDOUBLE_command;
    errors += TESTDOUBLE_seen_instance != instance || TESTDOUBLE_calls != 1;
    errors += vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkTESTDOUBLE_supported") != TESTDOUBLE_command;
    errors += TESTDOUBLE_seen_instance != VK_NULL_HANDLE;
    errors += vkGetInstanceProcAddr(instance, "vkUnsupported") != NULL;
    errors += vkGetInstanceProcAddr(instance, "") != NULL;
    printf("radv_icd_bridge/host_TESTDOUBLE %s errors=%d; RADV_PS5=NOT_TESTED\n", errors ? "FAIL" : "PASS", errors);
    return errors != 0;
}
