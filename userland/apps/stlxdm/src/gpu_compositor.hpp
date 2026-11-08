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
 * It owns a Vulkan device with one queue and stlxdm's timeline, which each
 * compose's submission raises and stlxdm waits on before presenting. It
 * also holds a probe context on the /dev/gpu0 file, with no queue and no
 * mapping, which reads lost with `GPU_LOST_DEVICE` once the GPU failed and
 * tells that apart from a loss of the compositor's own context.
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
        PFN_vkGetPhysicalDeviceQueueFamilyProperties    get_queue_families;
        PFN_vkGetPhysicalDeviceExternalBufferProperties get_external_buffer_properties;
        PFN_vkCreateDevice                              create_device;
        PFN_vkDestroyDevice                             destroy_device;
        PFN_vkGetDeviceQueue                            get_device_queue;
        PFN_vkCreateSemaphore                           create_semaphore;
        PFN_vkDestroySemaphore                          destroy_semaphore;
    };

    gpu_compositor() = default;

    const char* start(int gpu_fd);
    bool find_commands();
    const char* create_instance();
    bool offers_extension(VkPhysicalDevice physical_device, const char* name);
    const char* find_physical_device(const gpu_info& info);
    const char* check_physical_device();
    const char* create_device();
    const char* probe_gpu_file(int gpu_fd, const gpu_info& info);

    commands         m_vk = {};
    VkInstance       m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    char             m_device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
    uint32_t         m_queue_family = 0;

    VkDevice    m_device = VK_NULL_HANDLE;
    VkQueue     m_queue = VK_NULL_HANDLE;
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    int         m_probe_context = -1;
};

#endif
