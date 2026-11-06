#ifndef VKTEST_H
#define VKTEST_H

#include <vector>
#include <vulkan/vulkan.h>

// Prints a PASS or FAIL line for one check and counts it toward the summary
void check(const char* name, bool passed);

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name);

// The library exports only the global commands, so every other command comes from `vkGetInstanceProcAddr`
template <typename Command>
Command find_command(VkInstance instance, const char* name) {
    return reinterpret_cast<Command>(vkGetInstanceProcAddr(instance, name));
}

// Makes a descriptor with `GPU_RIGHT_READ` alone for the object `descriptor` names, through the /dev/gpuN of
// `physical_device` and outside Vulkan, or returns -1. The caller owns the new descriptor.
int restrict_to_read(VkInstance instance, VkPhysicalDevice physical_device, int descriptor);

// Creates a device with one queue of family 0, the family with every kind of work on NVK, enabling `extensions` and
// the feature structures chained at `features`, or returns `VK_NULL_HANDLE`
VkDevice create_device(
    VkInstance instance,
    VkPhysicalDevice physical_device,
    const std::vector<const char*>& extensions,
    const void* features
);

// Creates devices on `physical_device` with each queue family alone and with all of them, each queue and the device
// waiting idle
void check_device(VkInstance instance, VkPhysicalDevice physical_device);

// Allocates memory of every type on `physical_device`, maps and shares the host-visible types and imports the others
// read-only, and binds a buffer at a destroyed buffer's address
void check_memory(VkInstance instance, VkPhysicalDevice physical_device);

// Signals, waits on and shares timeline semaphores on `physical_device`, from the CPU and from submissions
void check_timeline(VkInstance instance, VkPhysicalDevice physical_device);

#endif // VKTEST_H
