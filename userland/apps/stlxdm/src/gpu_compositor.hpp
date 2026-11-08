#ifndef STLXDM_GPU_COMPOSITOR_HPP
#define STLXDM_GPU_COMPOSITOR_HPP

#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>

struct gpu_info;

/* The GPU compositor composes an output's frame on the GPU, in one compute
 * pass that reads each window's picture where it lies, through the Vulkan
 * library on the GPU of stlxdm's /dev/gpu0 file.
 *
 * It owns a Vulkan device with one queue, the compose pipeline and
 * stlxdm's timeline, which each compose's submission raises and stlxdm
 * waits on before presenting. It also holds a probe context on the
 * /dev/gpu0 file, with no queue and no mapping, which reads lost with
 * `GPU_LOST_DEVICE` once the GPU failed and tells that apart from a loss
 * of the compositor's own context.
 *
 * stlxdm's one thread makes every call. */
class gpu_compositor {
public:
    /* Creates the compositor on the GPU of `gpu_fd`, which must outlive it,
     * or returns null after printing why stlxdm composes on the CPU */
    static std::unique_ptr<gpu_compositor> create(int gpu_fd);

    ~gpu_compositor();

    gpu_compositor(const gpu_compositor&) = delete;
    gpu_compositor& operator=(const gpu_compositor&) = delete;

private:
    /* The library exports only the global commands, so the others come
     * from `vkGetInstanceProcAddr` */
    struct commands {
        PFN_vkDestroyInstance                           destroy_instance;
        PFN_vkEnumeratePhysicalDevices                  enumerate_physical_devices;
        PFN_vkEnumerateDeviceExtensionProperties        enumerate_device_extensions;
        PFN_vkGetPhysicalDeviceProperties2              get_physical_device_properties;
        PFN_vkGetPhysicalDeviceFeatures2                get_physical_device_features;
        PFN_vkGetPhysicalDeviceMemoryProperties         get_memory_properties;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties    get_queue_families;
        PFN_vkGetPhysicalDeviceExternalBufferProperties get_external_buffer_properties;
        PFN_vkCreateDevice                              create_device;
        PFN_vkDestroyDevice                             destroy_device;
        PFN_vkGetDeviceQueue                            get_device_queue;
        PFN_vkCreateSemaphore                           create_semaphore;
        PFN_vkDestroySemaphore                          destroy_semaphore;
        PFN_vkCreateBuffer                              create_buffer;
        PFN_vkDestroyBuffer                             destroy_buffer;
        PFN_vkGetBufferMemoryRequirements               get_buffer_memory_requirements;
        PFN_vkAllocateMemory                            allocate_memory;
        PFN_vkFreeMemory                                free_memory;
        PFN_vkBindBufferMemory                          bind_buffer_memory;
        PFN_vkMapMemory                                 map_memory;
        PFN_vkGetBufferDeviceAddress                    get_buffer_device_address;
        PFN_vkCreateShaderModule                        create_shader_module;
        PFN_vkDestroyShaderModule                       destroy_shader_module;
        PFN_vkCreatePipelineLayout                      create_pipeline_layout;
        PFN_vkDestroyPipelineLayout                     destroy_pipeline_layout;
        PFN_vkCreateComputePipelines                    create_compute_pipelines;
        PFN_vkDestroyPipeline                           destroy_pipeline;
    };

    /* A buffer bound to memory of its own, with its GPU address and, in
     * host-visible memory, its CPU mapping */
    struct device_buffer {
        VkBuffer        buffer = VK_NULL_HANDLE;
        VkDeviceMemory  memory = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        void*           mapping = nullptr;
    };

    gpu_compositor() = default;

    const char* start(int gpu_fd);
    bool find_commands();
    const char* create_instance();
    bool offers_extension(VkPhysicalDevice physical_device, const char* name);
    const char* find_physical_device(const gpu_info& info);
    const char* check_physical_device();
    const char* create_device();
    const char* create_pipeline();
    const char* upload_corner_distances();
    const char* probe_gpu_file(int gpu_fd, const gpu_info& info);

    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags properties) const;

    /* Leaves what it made in `created` when it fails,
     * for `destroy_buffer` */
    bool create_buffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties,
        device_buffer& created
    );

    void destroy_buffer(device_buffer& buffer);

    commands                         m_vk = {};
    VkInstance                       m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice                 m_physical_device = VK_NULL_HANDLE;
    char                             m_device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
    VkPhysicalDeviceMemoryProperties m_memory_properties = {};
    uint32_t                         m_queue_family = 0;

    VkDevice         m_device = VK_NULL_HANDLE;
    VkQueue          m_queue = VK_NULL_HANDLE;
    VkSemaphore      m_timeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline = VK_NULL_HANDLE;
    int              m_probe_context = -1;

    /* decor's corner distance table in GPU memory, its entry count, and the
     * distance past its last entry */
    device_buffer m_corner_distances;
    uint32_t      m_corner_count = 0;
    int32_t       m_corner_far_distance = 0;
};

#endif
