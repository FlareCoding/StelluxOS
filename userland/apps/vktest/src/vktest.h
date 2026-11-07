#ifndef VKTEST_H
#define VKTEST_H

#include <functional>
#include <vector>
#include <vulkan/vulkan.h>

// The commands that make buffers on memory of their own, record one-time work and submit it to a device's queue
struct work_commands {
    PFN_vkAllocateMemory              allocate_memory;
    PFN_vkFreeMemory                  free_memory;
    PFN_vkMapMemory                   map_memory;
    PFN_vkCreateBuffer                create_buffer;
    PFN_vkDestroyBuffer               destroy_buffer;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements;
    PFN_vkBindBufferMemory            bind_buffer_memory;

    PFN_vkGetDeviceQueue         get_device_queue;
    PFN_vkCreateCommandPool      create_command_pool;
    PFN_vkDestroyCommandPool     destroy_command_pool;
    PFN_vkAllocateCommandBuffers allocate_command_buffers;
    PFN_vkBeginCommandBuffer     begin_command_buffer;
    PFN_vkEndCommandBuffer       end_command_buffer;
    PFN_vkCmdPipelineBarrier     cmd_pipeline_barrier;
    PFN_vkQueueSubmit            queue_submit;
    PFN_vkQueueWaitIdle          queue_wait_idle;
};

// A buffer bound at the start of memory of its own, with the CPU's mapping of that memory when it has one
struct bound_buffer {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t*      words = nullptr;
};

// NVK's first queue family, which takes every kind of work
constexpr uint32_t GENERAL_FAMILY = 0;

// A buffer usage that lets transfer commands both read and write the buffer
constexpr VkBufferUsageFlags TRANSFER_USAGE = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

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

// The queue family with transfer work alone, which NVK runs on the GPU's copy engine, or `UINT32_MAX` if it finds none
uint32_t find_transfer_family(VkInstance instance, VkPhysicalDevice physical_device);

// Creates a device with one queue of each family in `families`, enabling `extensions` and the feature structures
// chained at `features`, or returns `VK_NULL_HANDLE`
VkDevice create_device(
    VkInstance instance,
    VkPhysicalDevice physical_device,
    const std::vector<uint32_t>& families,
    const std::vector<const char*>& extensions,
    const void* features
);

// Finds every command of `commands` through `vkGetInstanceProcAddr`, returning whether all were found
bool find_work_commands(VkInstance instance, work_commands& commands);

// Creates a buffer of `size` bytes for `usage`, `next` chained to its creation, and binds it at the start of `memory`,
// which must hold `size` bytes of `type`, or returns `VK_NULL_HANDLE` when a call fails or the buffer does not fit
VkBuffer bind_new_buffer(
    const work_commands& commands,
    VkDevice device,
    VkDeviceMemory memory,
    uint32_t type,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    const void* next
);

// Allocates `size` bytes of memory of `type` on `device` with a buffer for `usage` bound at its start, the buffer left
// null on a failure. The caller destroys the result with `destroy_bound_buffer`, a failed one included.
bound_buffer make_bound_buffer(
    const work_commands& commands,
    VkDevice device,
    uint32_t type,
    VkDeviceSize size,
    VkBufferUsageFlags usage
);

// Makes a bound buffer on memory of the staging type (the first type of `properties` the CPU maps coherently), mapped
// for the CPU unless a call fails or no type is coherent. The caller destroys the result with `destroy_bound_buffer`.
bound_buffer make_staging_buffer(
    const work_commands& commands,
    VkDevice device,
    const VkPhysicalDeviceMemoryProperties& properties,
    VkDeviceSize size,
    VkBufferUsageFlags usage
);

void destroy_bound_buffer(const work_commands& commands, VkDevice device, const bound_buffer& made);

// Records a barrier making the transfer writes recorded so far visible to `access` in `stage_mask`
void wait_for_transfer_writes(
    const work_commands& commands,
    VkCommandBuffer command_buffer,
    VkPipelineStageFlags stage_mask,
    VkAccessFlags access
);

// Records work through `record` into a one-time command buffer of `family` on `device`, submits it to the family's
// first queue and waits for that queue to idle, returning the first failure
VkResult run_once(
    const work_commands& commands,
    VkDevice device,
    uint32_t family,
    const std::function<void(VkCommandBuffer)>& record
);

// Creates devices on `physical_device` with each queue family alone and with all of them, each queue and the device
// waiting idle
void check_device(VkInstance instance, VkPhysicalDevice physical_device);

// Allocates memory of every type on `physical_device`, maps and shares the host-visible types and imports the others
// read-only, binds a buffer at a destroyed buffer's address, and copies through every type and into read-only imports
void check_memory(VkInstance instance, VkPhysicalDevice physical_device);

// Signals, waits on and shares timeline semaphores on `physical_device`, from the CPU and from submissions
void check_timeline(VkInstance instance, VkPhysicalDevice physical_device);

// Fills buffers on `physical_device`'s transfer queue, whole ones on memory of every type, a range of one and a
// buffer of 1 MiB plus 4 KiB, the CPU reading each fill back
void check_transfer(VkInstance instance, VkPhysicalDevice physical_device);

// Dispatches a compute pipeline on `physical_device`'s general queue, the CPU reading the words it writes
void check_compute(VkInstance instance, VkPhysicalDevice physical_device);

// Draws a triangle on `physical_device`'s general queue, the CPU comparing its image pixel by pixel and showing it in a
// stlxwin window
void check_triangle(VkInstance instance, VkPhysicalDevice physical_device);

#endif // VKTEST_H
