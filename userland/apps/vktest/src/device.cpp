// The device stage: a device with queues makes one GPU context, holding a GPU queue for NVK's upload queue and one for
// each Vulkan queue, and each 3D or compute queue submits its state push when created, before its wait for idle

#include "vktest.h"

#include <stdint.h>
#include <vector>

struct device_commands {
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_families;
    PFN_vkCreateDevice                           create_device;
    PFN_vkDestroyDevice                          destroy_device;
    PFN_vkGetDeviceQueue                         get_device_queue;
    PFN_vkQueueWaitIdle                          queue_wait_idle;
    PFN_vkDeviceWaitIdle                         device_wait_idle;
};

// Whether a device with one queue of each family in `families` is created, its queues each wait idle, and the device
// waits idle, the device destroyed again either way
static bool runs_device(
    const device_commands& commands,
    VkPhysicalDevice physical_device,
    const std::vector<uint32_t>& families
) {
    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    for (uint32_t family : families) {
        VkDeviceQueueCreateInfo queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        queue_infos.push_back(queue_info);
    }

    VkDeviceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    create_info.pQueueCreateInfos = queue_infos.data();

    VkDevice device = VK_NULL_HANDLE;
    if (commands.create_device(physical_device, &create_info, nullptr, &device) != VK_SUCCESS) {
        return false;
    }

    bool queues_idle = true;
    for (uint32_t family : families) {
        VkQueue queue = VK_NULL_HANDLE;
        commands.get_device_queue(device, family, 0, &queue);
        queues_idle &= queue != VK_NULL_HANDLE && commands.queue_wait_idle(queue) == VK_SUCCESS;
    }

    bool device_idle = commands.device_wait_idle(device) == VK_SUCCESS;
    commands.destroy_device(device, nullptr);

    return queues_idle && device_idle;
}

VkDevice create_device(
    VkInstance instance,
    VkPhysicalDevice physical_device,
    const std::vector<const char*>& extensions,
    const void* features
) {
    auto create = find_command<PFN_vkCreateDevice>(instance, "vkCreateDevice");
    if (!create) {
        return VK_NULL_HANDLE;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = 0;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = features;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;

    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    VkDevice device = VK_NULL_HANDLE;
    if (create(physical_device, &create_info, nullptr, &device) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    return device;
}

void check_device(VkInstance instance, VkPhysicalDevice physical_device) {
    device_commands commands = {
        .get_queue_families = find_command<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            instance,
            "vkGetPhysicalDeviceQueueFamilyProperties"
        ),
        .create_device = find_command<PFN_vkCreateDevice>(instance, "vkCreateDevice"),
        .destroy_device = find_command<PFN_vkDestroyDevice>(instance, "vkDestroyDevice"),
        .get_device_queue = find_command<PFN_vkGetDeviceQueue>(instance, "vkGetDeviceQueue"),
        .queue_wait_idle = find_command<PFN_vkQueueWaitIdle>(instance, "vkQueueWaitIdle"),
        .device_wait_idle = find_command<PFN_vkDeviceWaitIdle>(instance, "vkDeviceWaitIdle"),
    };

    bool has_commands = commands.get_queue_families && commands.create_device && commands.destroy_device &&
        commands.get_device_queue && commands.queue_wait_idle && commands.device_wait_idle;
    check("vkGetInstanceProcAddr finds the device commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    uint32_t family_count = 0;
    commands.get_queue_families(physical_device, &family_count, nullptr);

    std::vector<uint32_t> every_family;
    bool runs_each_family = family_count > 0;
    for (uint32_t family = 0; family < family_count; family++) {
        every_family.push_back(family);
        runs_each_family &= runs_device(commands, physical_device, {family});
    }

    bool runs_every_family = family_count > 0 && runs_device(commands, physical_device, every_family);
    check("a device with one queue of a single family is created and waits idle, for every family", runs_each_family);
    check("a device with one queue of every family is created and waits idle", runs_every_family);
}
