#ifndef VKTEST_H
#define VKTEST_H

#include <functional>
#include <stlx/gpu_abi.h>
#include <vector>
#include <vulkan/vulkan.h>

// The commands that make buffers on memory of their own, record one-time work and submit it to a device's queue, and
// that make, signal, wait on and read timeline semaphores
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

    PFN_vkCreateSemaphore          create_semaphore;
    PFN_vkDestroySemaphore         destroy_semaphore;
    PFN_vkSignalSemaphore          signal_semaphore;
    PFN_vkWaitSemaphores           wait_semaphores;
    PFN_vkGetSemaphoreCounterValue get_counter_value;
};

// A buffer bound at the start of memory of its own, with the CPU's mapping of that memory when it has one
struct bound_buffer {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t*      words = nullptr;
};

// The commands and queue family that each stage losing devices holds
struct burst_commands {
    work_commands       work;
    PFN_vkDestroyDevice destroy_device;
    uint32_t            transfer_family;
};

// A burst of `BURST_SIZE` submissions on `queue`: submission i signals `done` at i + 1, and the first also waits for
// `gate` to reach 1
struct held_burst {
    VkDevice    device = VK_NULL_HANDLE;
    VkQueue     queue = VK_NULL_HANDLE;
    VkSemaphore gate = VK_NULL_HANDLE;
    VkSemaphore done = VK_NULL_HANDLE;
};

// NVK's first queue family, which takes every kind of work
constexpr uint32_t GENERAL_FAMILY = 0;

// A buffer usage that lets transfer commands both read and write the buffer
constexpr VkBufferUsageFlags TRANSFER_USAGE = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

// A whole page, since `bind_new_buffer` refuses memory smaller than the buffer's requirements, which NVK rounds up
constexpr VkDeviceSize PAGE_BUFFER_SIZE = 4096;

// The longest NVK's waits sleep in the kernel before they check whether the device was lost
constexpr uint64_t WAIT_SLICE_NS = 100000000;

// More submissions than one kernel queue has room for, so the rest go on NVK's overflow list
constexpr uint32_t BURST_SIZE = 100;
static_assert(BURST_SIZE > GPU_QUEUE_PENDING_MAX, "a burst must overflow the kernel queue");

// The longest one submit may take, below the slice a submit waiting for room in a kernel queue would sleep
constexpr uint64_t SUBMIT_LIMIT_NS = WAIT_SLICE_NS;

// The longest held work may take to end, done or lost, once vktest releases it: far past what it needs
constexpr uint64_t WORK_WAIT_NS = 50 * WAIT_SLICE_NS;

// One wait slice, so a wait ending lost within it found the loss without sleeping out a slice in the kernel
constexpr uint64_t LOST_WAIT_LIMIT_NS = WAIT_SLICE_NS;

// An overflow thread sees a loss when its wait for room ends, so the slices after the first are for the teardown
constexpr uint64_t DESTROY_LIMIT_NS = 10 * WAIT_SLICE_NS;

// How long a watch may last before the watchdog's next look fails its call, past every bound a check gives its calls
constexpr uint64_t HANG_LIMIT_NS = 2 * WORK_WAIT_NS;

// Prints a PASS or FAIL line for one check and counts it toward the summary
void check(const char* name, bool passed);

// Watches `call`, which a regression can keep waiting forever: if the watchdog's first look past `HANG_LIMIT_NS` comes
// before `end_hang_watch`, the watchdog fails a check naming the call and ends vktest with its summary
void start_hang_watch(const char* call);

// Ends the watch, or never returns once the watchdog has failed the call, since vktest is then exiting. The caller
// must print nothing during the watch, so none of its lines or counts can come after the watchdog's summary.
void end_hang_watch();

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name);

// The monotonic clock's reading in nanoseconds
uint64_t monotonic_ns();

// The library exports only the global commands, so every other command comes from `vkGetInstanceProcAddr`
template <typename Command>
Command find_command(VkInstance instance, const char* name) {
    return reinterpret_cast<Command>(vkGetInstanceProcAddr(instance, name));
}

// Makes a descriptor with `GPU_RIGHT_READ` alone for the object `descriptor` names, through the /dev/gpuN of
// `physical_device` and outside Vulkan, or returns -1. The caller owns the new descriptor.
int restrict_to_read(VkInstance instance, VkPhysicalDevice physical_device, int descriptor);

// Puts a descriptor of the /dev/gpuN of `physical_device` in place of the descriptor of the timeline reading `value`,
// outside Vulkan, so the kernel refuses submissions signalling it. Returns whether it replaced one.
bool replace_timeline_descriptor(VkInstance instance, VkPhysicalDevice physical_device, uint64_t value);

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

// Creates a command pool for `family` on `device`, or returns `VK_NULL_HANDLE`
VkCommandPool make_command_pool(const work_commands& commands, VkDevice device, uint32_t family);

// Creates a timeline semaphore at `initial_value` on `device`, exportable as an opaque descriptor when `is_exportable`,
// or returns `VK_NULL_HANDLE`
VkSemaphore create_timeline(const work_commands& commands, VkDevice device, uint64_t initial_value, bool is_exportable);

// Whether a read of `semaphore` succeeds and finds `expected`, which a lost device's reads never do
bool reads_value(const work_commands& commands, VkDevice device, VkSemaphore semaphore, uint64_t expected);

// Signals `semaphore` to `value` from the CPU
VkResult signal_timeline(const work_commands& commands, VkDevice device, VkSemaphore semaphore, uint64_t value);

// Waits up to `timeout_ns` for `semaphore` to reach `value`
VkResult wait_for_value(
    const work_commands& commands,
    VkDevice device,
    VkSemaphore semaphore,
    uint64_t value,
    uint64_t timeout_ns
);

// Finds the work commands, the device destroy and the transfer family of `physical_device`, returning whether every
// command was found
bool find_burst_commands(VkInstance instance, VkPhysicalDevice physical_device, burst_commands& bursts);

// Makes a burst's semaphores and finds its queue, the transfer queue of `device`. The caller destroys the semaphores
// with `destroy_burst`.
held_burst make_burst(const burst_commands& bursts, VkDevice device);

void destroy_burst(const burst_commands& bursts, const held_burst& burst);

// Whether `make_burst` found the burst's queue and made both its semaphores
bool is_made(const held_burst& burst);

// Submits to `queue` a batch that waits for `gate` to reach 1 unless it is null, then raises each semaphore of
// `signals` to its value in `values`
VkResult submit_batch(
    const burst_commands& bursts,
    VkQueue queue,
    VkSemaphore gate,
    const std::vector<VkSemaphore>& signals,
    const std::vector<uint64_t>& values
);

// Submits the burst, returning whether each submit succeeded within `SUBMIT_LIMIT_NS`
bool holds_burst(const burst_commands& bursts, const held_burst& burst);

// Signals the burst's gate, returning whether the burst then completed and its queue waited idle
bool drains_burst(const burst_commands& bursts, const held_burst& burst);

// Whether a wait with no timeout for `semaphore` to reach `value` ends with `VK_ERROR_DEVICE_LOST` within `limit_ns`
bool wait_ends_lost(
    const burst_commands& bursts,
    VkDevice device,
    VkSemaphore semaphore,
    uint64_t value,
    uint64_t limit_ns
);

// Whether the idle wait of `queue` ends with `VK_ERROR_DEVICE_LOST` within `LOST_WAIT_LIMIT_NS`
bool idle_ends_lost(const burst_commands& bursts, VkQueue queue);

// Destroys `device`, returning whether that took less than `DESTROY_LIMIT_NS`
bool destroys_in_time(const burst_commands& bursts, VkDevice device);

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

// Submits bursts of 100 on `physical_device`'s transfer queue, held behind a point the CPU signals after the last
// submit, each submit returning at once and each burst running in submission order
void check_queue(VkInstance instance, VkPhysicalDevice physical_device);

// Loses devices on `physical_device` while bursts of 100 overflow their kernel queues, by a submit the kernel refuses
// on another queue and by one it refuses from the overflow thread, timing the waits and destroy of each lost device
void check_loss(VkInstance instance, VkPhysicalDevice physical_device);

#endif // VKTEST_H
