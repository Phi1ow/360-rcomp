#include <cstdio>
#include "gpu/xenos/rexglue/rcomp/vulkan_fence.h"
namespace {
VkResult TESTDOUBLE_wait_result = VK_SUCCESS, TESTDOUBLE_reset_result = VK_SUCCESS;
unsigned TESTDOUBLE_waits = 0, TESTDOUBLE_resets = 0;
bool TESTDOUBLE_arguments_ok = true;
const VkDevice TESTDOUBLE_device = (VkDevice)(uintptr_t)0x1234;
const VkFence TESTDOUBLE_fence = (VkFence)(uintptr_t)0x5678;
VKAPI_ATTR VkResult VKAPI_CALL TESTDOUBLE_wait(VkDevice device, uint32_t count,
    const VkFence* fences, VkBool32 all, uint64_t timeout) {
    ++TESTDOUBLE_waits;
    TESTDOUBLE_arguments_ok = TESTDOUBLE_arguments_ok && device == TESTDOUBLE_device &&
        count == 1 && fences && fences[0] == TESTDOUBLE_fence && all == VK_TRUE && timeout == 99;
    return TESTDOUBLE_wait_result;
}
VKAPI_ATTR VkResult VKAPI_CALL TESTDOUBLE_reset(VkDevice device, uint32_t count, const VkFence* fences) {
    ++TESTDOUBLE_resets;
    TESTDOUBLE_arguments_ok = TESTDOUBLE_arguments_ok && device == TESTDOUBLE_device &&
        count == 1 && fences && fences[0] == TESTDOUBLE_fence;
    return TESTDOUBLE_reset_result;
}
}
int main() {
    struct Case { VkResult wait, reset, expected; unsigned resets; };
    const Case cases[] = {
        {VK_SUCCESS, VK_SUCCESS, VK_SUCCESS, 1}, {VK_TIMEOUT, VK_SUCCESS, VK_TIMEOUT, 0},
        {VK_ERROR_DEVICE_LOST, VK_SUCCESS, VK_ERROR_DEVICE_LOST, 0},
        {VK_SUCCESS, VK_ERROR_DEVICE_LOST, VK_ERROR_DEVICE_LOST, 1},
    };
    unsigned failures = 0;
    for (const auto& c : cases) {
        TESTDOUBLE_wait_result = c.wait; TESTDOUBLE_reset_result = c.reset;
        TESTDOUBLE_waits = TESTDOUBLE_resets = 0; TESTDOUBLE_arguments_ok = true;
        const VkResult result = rcomp::xenos::WaitAndResetFence(TESTDOUBLE_device, TESTDOUBLE_fence,
            TESTDOUBLE_wait, TESTDOUBLE_reset, 99);
        const bool ok = result == c.expected && TESTDOUBLE_waits == 1 &&
            TESTDOUBLE_resets == c.resets && TESTDOUBLE_arguments_ok;
        failures += !ok;
        std::printf("%s display_fence wait=%d reset=%d observed=%d reset_calls=%u scope=host_TESTDOUBLE\n",
            ok ? "PASS" : "FAIL", int(c.wait), int(c.reset), int(result), TESTDOUBLE_resets);
    }
    return failures ? 1 : 0;
}
