// SPDX-License-Identifier: GPL-2.0-or-later
// 仅供已应用 0008 防护的 Android Turnip：执行非法 End 的受控验证，不提交到 GPU。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(call) do { \
    VkResult result = (call); \
    if (result != VK_SUCCESS) { \
        fprintf(stderr, "%s failed: %d\n", #call, result); \
        return 1; \
    } \
} while (0)

int main(int argc, char **argv) {
    // 防止误在未防护驱动上运行；该参数不能替代部署时的摘要验证。
    if (argc != 2 || strcmp(argv[1], "--patched-turnip") != 0) return 2;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "gow3-renderpass-guard-test",
                            .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                          .pApplicationInfo = &app};
    VkInstance instance;
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) return 3;
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) return 4;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDevice physical = devices[0];
    free(devices);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    fprintf(stderr, "guard-test device=%s\n", properties.deviceName);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *queues = calloc(count, sizeof(*queues));
    if (!queues) return 4;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
    uint32_t family = 0;
    while (family < count && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    free(queues);
    if (family == count) return 5;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceDynamicRenderingFeatures dynamic = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .dynamicRendering = VK_TRUE};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &dynamic, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    VkDevice device;
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .queueFamilyIndex = family};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device, &allocate, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    fprintf(stderr, "guard-test no-begin: expect one warning\n");
    vkCmdEndRendering(command);
    fprintf(stderr, "guard-test no-begin: returned\n");
    // 无附件的合法渲染范围；只录制，不执行，避免依赖游戏或图像内容。
    VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, {1, 1}}, .layerCount = 1};
    vkCmdBeginRendering(command, &rendering);
    vkCmdEndRendering(command);
    fprintf(stderr, "guard-test valid-end: returned; duplicate: expect one warning\n");
    vkCmdEndRendering(command);
    fprintf(stderr, "guard-test duplicate: returned\n");
    CHECK(vkEndCommandBuffer(command));
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    fprintf(stderr, "guard-test PASS (no GPU submission)\n");
    return 0;
}
