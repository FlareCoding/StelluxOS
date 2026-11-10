#include "gpu_compositor.hpp"
#include "decor.hpp"
#include "material.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stlx/gfxfb_abi.h>
#include <stlx/gpu_abi.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "compose_spirv.h"

/* One dispatch's push constants, which compose.comp's `compose_args` must match */
struct compose_args {
    int32_t  x0, y0;
    int32_t  x1, y1;
    uint64_t target;
    uint32_t target_pitch;
    uint32_t layer_count;
    uint64_t layers;
    uint64_t corner_distances;
    uint32_t corner_count;
    int32_t  corner_far_distance;
    uint64_t material;
};

static_assert(sizeof(compose_args) == 64);

/* The version that makes timeline semaphores and buffer device addresses core */
constexpr uint32_t VULKAN_API_VERSION = VK_API_VERSION_1_2;

constexpr float QUEUE_PRIORITY = 1.0f;

/* The width and height of compose.comp's workgroups */
constexpr uint32_t WORKGROUP_PIXELS = 16;

constexpr uint32_t BYTES_PER_PIXEL = 4;

/* NVIDIA's display scans out rows that start a multiple of 256 bytes apart */
constexpr uint64_t SCANOUT_PITCH_ALIGNMENT = 256;

/* The queries of the timestamp pool, written around each compose's dispatches */
constexpr uint32_t BEFORE_DISPATCHES = 0;
constexpr uint32_t AFTER_DISPATCHES = 1;
constexpr uint32_t TIMESTAMP_COUNT = 2;

/* Every picture and output buffer the compose reads or writes is a storage
 * buffer named by its device address */
constexpr VkBufferUsageFlags PICTURE_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

constexpr VkMemoryPropertyFlags HOST_COHERENT = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

/* Memory the CPU writes and the GPU reads coherently, which on NVK lies in the part of video memory the CPU can map,
 * for the layer lists and the corner distances */
const gpu_compositor::memory_kind gpu_compositor::COHERENT_MEMORY = { HOST_COHERENT, 0 };

/* System memory the CPU caches and the GPU reads coherently */
const gpu_compositor::memory_kind gpu_compositor::PICTURE_MEMORY = {
    HOST_COHERENT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
    0
};

/* Video memory the CPU cannot map, for twins, output buffers and window buffers that are not mappable */
const gpu_compositor::memory_kind gpu_compositor::VIDEO_MEMORY = {
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
};

/* An opaque import must name the type its exporter allocated from, and a window buffer lies in system memory, in video
 * memory its client maps, or in video memory that is not mappable, which is VIDEO_MEMORY's */
const gpu_compositor::memory_kind gpu_compositor::SYSTEM_WINDOW_MEMORY = {
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
};

const gpu_compositor::memory_kind gpu_compositor::VIDEO_WINDOW_MEMORY = {
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    0
};

/* A picture in a buffer of the compositor's own, freed with the picture */
class gpu_compositor::owned_picture : public picture {
public:
    owned_picture(gpu_compositor& owner, const device_buffer& buffer, size_t bytes)
        : picture(static_cast<uint8_t*>(buffer.mapping), buffer.address, bytes), m_owner(owner), m_buffer(buffer) {}

    ~owned_picture() override {
        m_owner.destroy_buffer(m_buffer);
    }

    VkBuffer buffer() const { return m_buffer.buffer; }

private:
    gpu_compositor& m_owner;
    device_buffer   m_buffer;
};

/* An output buffer's memory, imported into gfxfb and bound to a buffer the compose writes */
class gpu_compositor::scanout_buffer : public output_buffer {
public:
    scanout_buffer(gpu_compositor& owner, int gfxfb_fd, uint32_t pitch)
        : m_owner(owner), m_gfxfb_fd(gfxfb_fd), m_pitch(pitch) {}

    /* gfxfb keeps the memory while an output shows it */
    ~scanout_buffer() override {
        if (m_gfxfb_handle != 0) {
            ioctl(m_gfxfb_fd, gfxfb::GFXFB_DESTROY_BUFFER, static_cast<unsigned long>(m_gfxfb_handle));
        }

        m_owner.destroy_buffer(m_import);
    }

    uint32_t gfxfb_handle() const override { return m_gfxfb_handle; }
    uint64_t gpu_address() const override { return m_import.address; }
    uint32_t pitch() const override { return m_pitch; }

private:
    friend class gpu_compositor;

    gpu_compositor& m_owner;
    int             m_gfxfb_fd;
    uint32_t        m_pitch;
    uint32_t        m_gfxfb_handle = 0;
    device_buffer   m_import;
};

/* A window buffer's memory, imported through a copy of its handle narrowed to reading and freed with the import */
class gpu_compositor::imported_window_buffer : public window_import {
public:
    imported_window_buffer(gpu_compositor& owner, const device_buffer& buffer)
        : window_import(buffer.address), m_owner(owner), m_buffer(buffer) {}

    ~imported_window_buffer() override {
        m_owner.destroy_buffer(m_buffer);
    }

    VkBuffer buffer() const { return m_buffer.buffer; }

private:
    gpu_compositor& m_owner;
    device_buffer   m_buffer;
};

template <typename Command>
static bool find_command(VkInstance instance, const char* name, Command& command) {
    command = reinterpret_cast<Command>(vkGetInstanceProcAddr(instance, name));
    return command != nullptr;
}

static bool is_at_address(const gpu_info& info, const VkPhysicalDevicePCIBusInfoPropertiesEXT& pci_bus) {
    return info.pci_segment == pci_bus.pciDomain && info.pci_bus == pci_bus.pciBus &&
        info.pci_slot == pci_bus.pciDevice && info.pci_function == pci_bus.pciFunction;
}

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

static uint32_t workgroups(int32_t pixels) {
    return (static_cast<uint32_t>(pixels) + WORKGROUP_PIXELS - 1) / WORKGROUP_PIXELS;
}

/* The CPU time stlxdm's thread has run, which excludes the time it was preempted */
static uint64_t thread_cpu_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

/* Only the low `valid_bits` of a timestamp are meaningful, so a later one may read below an earlier one past a wrap */
static uint64_t timestamps_apart_ns(uint64_t first, uint64_t last, uint32_t valid_bits, float period_ns) {
    uint64_t mask = valid_bits >= 64 ? ~0ull : (1ull << valid_bits) - 1;
    uint64_t ticks = (last - first) & mask;
    return static_cast<uint64_t>(static_cast<double>(ticks) * period_ns);
}

std::unique_ptr<gpu_compositor> gpu_compositor::create(int gpu_fd) {
    std::unique_ptr<gpu_compositor> compositor(new gpu_compositor());

    const char* refusal = compositor->start(gpu_fd);
    if (refusal) {
        printf("stlxdm: composing on the CPU, %s\r\n", refusal);
        return nullptr;
    }

    printf("stlxdm: the GPU compositor runs on %s\r\n", compositor->m_device_name);

    return compositor;
}

gpu_compositor::~gpu_compositor() {
    if (m_probe_context >= 0) {
        close(m_probe_context);
    }

    end_retirement();
    free_buffer(m_corner_distances);
    free_buffer(m_material);

    if (m_timestamps != VK_NULL_HANDLE) {
        m_vk.destroy_query_pool(m_device, m_timestamps, nullptr);
    }

    if (m_command_pool != VK_NULL_HANDLE) {
        m_vk.destroy_command_pool(m_device, m_command_pool, nullptr);
    }

    if (m_pipeline != VK_NULL_HANDLE) {
        m_vk.destroy_pipeline(m_device, m_pipeline, nullptr);
    }

    if (m_pipeline_layout != VK_NULL_HANDLE) {
        m_vk.destroy_pipeline_layout(m_device, m_pipeline_layout, nullptr);
    }

    if (m_timeline != VK_NULL_HANDLE) {
        m_vk.destroy_semaphore(m_device, m_timeline, nullptr);
    }

    if (m_device != VK_NULL_HANDLE) {
        m_vk.destroy_device(m_device, nullptr);
    }

    if (m_instance != VK_NULL_HANDLE && m_vk.destroy_instance) {
        m_vk.destroy_instance(m_instance, nullptr);
    }
}

/* Returns null once every object is made, else why stlxdm composes on the CPU */
const char* gpu_compositor::start(int gpu_fd) {
    gpu_info info = {};
    if (ioctl(gpu_fd, GPU_GET_INFO, &info) != 0) {
        return "GPU_GET_INFO failed on /dev/gpu0";
    }

    m_gpu_fd = gpu_fd;
    m_page_size = info.page_size;
    m_contexts_may_map_zero = info.va_start == 0;

    const char* refusal = create_instance();
    if (refusal) {
        return refusal;
    }

    refusal = find_physical_device(info);
    if (refusal) {
        return refusal;
    }

    refusal = check_physical_device();
    if (refusal) {
        return refusal;
    }

    refusal = create_device();
    if (refusal) {
        return refusal;
    }

    refusal = create_pipeline();
    if (refusal) {
        return refusal;
    }

    refusal = create_command_pool();
    if (refusal) {
        return refusal;
    }

    refusal = create_timestamp_queries();
    if (refusal) {
        return refusal;
    }

    refusal = upload_corner_distances();
    if (refusal) {
        return refusal;
    }

    refusal = make_material_buffer();
    if (refusal) {
        return refusal;
    }

    return probe_gpu_file(gpu_fd, info);
}

bool gpu_compositor::find_commands() {
    return find_command(m_instance, "vkDestroyInstance", m_vk.destroy_instance) &&
        find_command(m_instance, "vkEnumeratePhysicalDevices", m_vk.enumerate_physical_devices) &&
        find_command(m_instance, "vkEnumerateDeviceExtensionProperties", m_vk.enumerate_device_extensions) &&
        find_command(m_instance, "vkGetPhysicalDeviceProperties2", m_vk.get_physical_device_properties) &&
        find_command(m_instance, "vkGetPhysicalDeviceFeatures2", m_vk.get_physical_device_features) &&
        find_command(m_instance, "vkGetPhysicalDeviceMemoryProperties", m_vk.get_memory_properties) &&
        find_command(m_instance, "vkGetPhysicalDeviceQueueFamilyProperties", m_vk.get_queue_families) &&
        find_command(
            m_instance,
            "vkGetPhysicalDeviceExternalBufferProperties",
            m_vk.get_external_buffer_properties
        ) &&
        find_command(m_instance, "vkCreateDevice", m_vk.create_device) &&
        find_command(m_instance, "vkDestroyDevice", m_vk.destroy_device) &&
        find_command(m_instance, "vkGetDeviceQueue", m_vk.get_device_queue) &&
        find_command(m_instance, "vkCreateSemaphore", m_vk.create_semaphore) &&
        find_command(m_instance, "vkDestroySemaphore", m_vk.destroy_semaphore) &&
        find_command(m_instance, "vkCreateBuffer", m_vk.create_buffer) &&
        find_command(m_instance, "vkDestroyBuffer", m_vk.destroy_buffer) &&
        find_command(m_instance, "vkGetBufferMemoryRequirements", m_vk.get_buffer_memory_requirements) &&
        find_command(m_instance, "vkAllocateMemory", m_vk.allocate_memory) &&
        find_command(m_instance, "vkFreeMemory", m_vk.free_memory) &&
        find_command(m_instance, "vkBindBufferMemory", m_vk.bind_buffer_memory) &&
        find_command(m_instance, "vkMapMemory", m_vk.map_memory) &&
        find_command(m_instance, "vkGetBufferDeviceAddress", m_vk.get_buffer_device_address) &&
        find_command(m_instance, "vkCreateShaderModule", m_vk.create_shader_module) &&
        find_command(m_instance, "vkDestroyShaderModule", m_vk.destroy_shader_module) &&
        find_command(m_instance, "vkCreatePipelineLayout", m_vk.create_pipeline_layout) &&
        find_command(m_instance, "vkDestroyPipelineLayout", m_vk.destroy_pipeline_layout) &&
        find_command(m_instance, "vkCreateComputePipelines", m_vk.create_compute_pipelines) &&
        find_command(m_instance, "vkDestroyPipeline", m_vk.destroy_pipeline) &&
        find_command(m_instance, "vkCreateCommandPool", m_vk.create_command_pool) &&
        find_command(m_instance, "vkDestroyCommandPool", m_vk.destroy_command_pool) &&
        find_command(m_instance, "vkAllocateCommandBuffers", m_vk.allocate_command_buffers) &&
        find_command(m_instance, "vkFreeCommandBuffers", m_vk.free_command_buffers) &&
        find_command(m_instance, "vkBeginCommandBuffer", m_vk.begin_command_buffer) &&
        find_command(m_instance, "vkEndCommandBuffer", m_vk.end_command_buffer) &&
        find_command(m_instance, "vkCmdBindPipeline", m_vk.cmd_bind_pipeline) &&
        find_command(m_instance, "vkCmdPushConstants", m_vk.cmd_push_constants) &&
        find_command(m_instance, "vkCmdDispatch", m_vk.cmd_dispatch) &&
        find_command(m_instance, "vkCmdPipelineBarrier", m_vk.cmd_pipeline_barrier) &&
        find_command(m_instance, "vkCmdCopyBuffer", m_vk.cmd_copy_buffer) &&
        find_command(m_instance, "vkCreateQueryPool", m_vk.create_query_pool) &&
        find_command(m_instance, "vkDestroyQueryPool", m_vk.destroy_query_pool) &&
        find_command(m_instance, "vkGetQueryPoolResults", m_vk.get_query_pool_results) &&
        find_command(m_instance, "vkCmdResetQueryPool", m_vk.cmd_reset_query_pool) &&
        find_command(m_instance, "vkCmdWriteTimestamp", m_vk.cmd_write_timestamp) &&
        find_command(m_instance, "vkQueueSubmit", m_vk.queue_submit) &&
        find_command(m_instance, "vkWaitSemaphores", m_vk.wait_semaphores) &&
        find_command(m_instance, "vkGetSemaphoreCounterValue", m_vk.get_semaphore_counter_value);
}

const char* gpu_compositor::create_instance() {
    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "stlxdm";
    application.apiVersion = VULKAN_API_VERSION;

    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &application;

    if (vkCreateInstance(&create_info, nullptr, &m_instance) != VK_SUCCESS) {
        m_instance = VK_NULL_HANDLE;
        return "Vulkan refused an instance";
    }

    if (!find_commands()) {
        return "the Vulkan library lacks a command stlxdm calls";
    }

    return nullptr;
}

bool gpu_compositor::offers_extension(VkPhysicalDevice physical_device, const char* name) {
    uint32_t count = 0;
    m_vk.enumerate_device_extensions(physical_device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    m_vk.enumerate_device_extensions(physical_device, nullptr, &count, extensions.data());

    for (const VkExtensionProperties& extension : extensions) {
        if (strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }

    return false;
}

/* The physical device for /dev/gpu0 is the one at its PCI address */
const char* gpu_compositor::find_physical_device(const gpu_info& info) {
    uint32_t count = 0;
    m_vk.enumerate_physical_devices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physical_devices(count);
    m_vk.enumerate_physical_devices(m_instance, &count, physical_devices.data());

    for (VkPhysicalDevice physical_device : physical_devices) {
        if (!offers_extension(physical_device, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME)) {
            continue;
        }

        VkPhysicalDevicePCIBusInfoPropertiesEXT pci_bus = {};
        pci_bus.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;

        VkPhysicalDeviceProperties2 properties = {};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &pci_bus;
        m_vk.get_physical_device_properties(physical_device, &properties);

        if (is_at_address(info, pci_bus)) {
            m_physical_device = physical_device;
            memcpy(m_device_name, properties.properties.deviceName, sizeof(m_device_name));
            m_timestamp_period_ns = properties.properties.limits.timestampPeriod;
            return nullptr;
        }
    }

    return "no Vulkan device for /dev/gpu0";
}

const char* gpu_compositor::check_physical_device() {
    uint32_t family_count = 0;
    m_vk.get_queue_families(m_physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    m_vk.get_queue_families(m_physical_device, &family_count, families.data());

    m_queue_family = family_count;
    for (uint32_t family = 0; family < family_count; family++) {
        if ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            m_queue_family = family;
            break;
        }
    }

    if (m_queue_family == family_count) {
        return "the Vulkan device has no compute queue";
    }

    m_timestamp_bits = families[m_queue_family].timestampValidBits;

    VkPhysicalDeviceVulkan12Features features_12 = {};
    features_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceFeatures2 features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features_12;
    m_vk.get_physical_device_features(m_physical_device, &features);

    if (!features_12.timelineSemaphore) {
        return "the Vulkan device has no timeline semaphores";
    }

    if (!features_12.bufferDeviceAddress) {
        return "the Vulkan device has no buffer device addresses";
    }

    if (!offers_extension(m_physical_device, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        return "the Vulkan device cannot import memory descriptors";
    }

    VkPhysicalDeviceExternalBufferInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    buffer_info.usage = PICTURE_USAGE;
    buffer_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkExternalBufferProperties buffer_properties = {};
    buffer_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    m_vk.get_external_buffer_properties(m_physical_device, &buffer_info, &buffer_properties);

    VkExternalMemoryFeatureFlags memory_features = buffer_properties.externalMemoryProperties.externalMemoryFeatures;
    if ((memory_features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
        return "the Vulkan device cannot import memory descriptors as storage buffers";
    }

    m_vk.get_memory_properties(m_physical_device, &m_memory_properties);

    return nullptr;
}

const char* gpu_compositor::create_device() {
    VkDeviceQueueCreateInfo queue_info = {};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = m_queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &QUEUE_PRIORITY;

    VkPhysicalDeviceVulkan12Features features_12 = {};
    features_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features_12.timelineSemaphore = VK_TRUE;
    features_12.bufferDeviceAddress = VK_TRUE;

    const char* extensions[] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME };

    VkDeviceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = &features_12;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;

    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = extensions;

    if (m_vk.create_device(m_physical_device, &create_info, nullptr, &m_device) != VK_SUCCESS) {
        m_device = VK_NULL_HANDLE;
        return "Vulkan refused the device";
    }

    m_vk.get_device_queue(m_device, m_queue_family, 0, &m_queue);

    VkSemaphoreTypeCreateInfo type_info = {};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo semaphore_info = {};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore_info.pNext = &type_info;

    if (m_vk.create_semaphore(m_device, &semaphore_info, nullptr, &m_timeline) != VK_SUCCESS) {
        m_timeline = VK_NULL_HANDLE;
        return "Vulkan refused stlxdm's timeline";
    }

    return nullptr;
}

const char* gpu_compositor::create_pipeline() {
    VkPushConstantRange push_range = {};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.size = sizeof(compose_args);

    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (m_vk.create_pipeline_layout(m_device, &layout_info, nullptr, &m_pipeline_layout) != VK_SUCCESS) {
        m_pipeline_layout = VK_NULL_HANDLE;
        return "Vulkan refused the compose pipeline's layout";
    }

    VkShaderModuleCreateInfo module_info = {};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = sizeof(COMPOSE_SPIRV);
    module_info.pCode = COMPOSE_SPIRV;

    VkShaderModule module = VK_NULL_HANDLE;
    if (m_vk.create_shader_module(m_device, &module_info, nullptr, &module) != VK_SUCCESS) {
        return "Vulkan refused the compose shader";
    }

    VkComputePipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.layout = m_pipeline_layout;

    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";

    VkResult created = m_vk.create_compute_pipelines(m_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_pipeline);
    m_vk.destroy_shader_module(m_device, module, nullptr);

    if (created != VK_SUCCESS) {
        m_pipeline = VK_NULL_HANDLE;
        return "Vulkan refused the compose pipeline";
    }

    return nullptr;
}

/* Each output's command buffer is recorded afresh at every compose */
const char* gpu_compositor::create_command_pool() {
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = m_queue_family;

    if (m_vk.create_command_pool(m_device, &pool_info, nullptr, &m_command_pool) != VK_SUCCESS) {
        m_command_pool = VK_NULL_HANDLE;
        return "Vulkan refused a command pool";
    }

    return nullptr;
}

/* Composes on a queue that writes no timestamps go without a GPU time */
const char* gpu_compositor::create_timestamp_queries() {
    if (m_timestamp_bits == 0) {
        return nullptr;
    }

    VkQueryPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    pool_info.queryCount = TIMESTAMP_COUNT;

    if (m_vk.create_query_pool(m_device, &pool_info, nullptr, &m_timestamps) != VK_SUCCESS) {
        m_timestamps = VK_NULL_HANDLE;
        return "Vulkan refused the timestamp queries";
    }

    return nullptr;
}

const char* gpu_compositor::upload_corner_distances() {
    decor::corner_table corners = decor::corner_distances();
    VkDeviceSize table_bytes = corners.count * sizeof(uint16_t);

    /* The shader reads the table two entries a word */
    VkDeviceSize buffer_bytes = (table_bytes + sizeof(uint32_t) - 1) / sizeof(uint32_t) * sizeof(uint32_t);

    if (!create_buffer(buffer_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, COHERENT_MEMORY, m_corner_distances)) {
        return "Vulkan refused memory for the corner distances";
    }

    memcpy(m_corner_distances.mapping, corners.distances, table_bytes);

    m_corner_count = corners.count;
    m_corner_far_distance = corners.far_distance;

    return nullptr;
}

const char* gpu_compositor::make_material_buffer() {
    if (!create_buffer(sizeof(material::tables), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, COHERENT_MEMORY, m_material)) {
        return "Vulkan refused memory for the glass tables";
    }

    return nullptr;
}

/* No compose reads the tables meanwhile, since each compose ends before the next one starts */
void gpu_compositor::upload_material() {
    if (m_material_generation == material::tables_generation()) {
        return;
    }

    memcpy(m_material.mapping, &material::current_tables(), sizeof(material::tables));
    m_material_generation = material::tables_generation();
}

uint32_t gpu_compositor::find_memory_type(uint32_t type_bits, const memory_kind& kind) const {
    for (uint32_t type = 0; type < m_memory_properties.memoryTypeCount; type++) {
        VkMemoryPropertyFlags properties = m_memory_properties.memoryTypes[type].propertyFlags;
        bool is_allowed = (type_bits & (1u << type)) != 0;
        bool has_required = (properties & kind.required) == kind.required;
        bool has_denied = (properties & kind.denied) != 0;
        if (is_allowed && has_required && !has_denied) {
            return type;
        }
    }

    return UINT32_MAX;
}

bool gpu_compositor::create_buffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    const memory_kind& kind,
    device_buffer& created
) {
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (m_vk.create_buffer(m_device, &buffer_info, nullptr, &created.buffer) != VK_SUCCESS) {
        created.buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements = {};
    m_vk.get_buffer_memory_requirements(m_device, created.buffer, &requirements);

    uint32_t type = find_memory_type(requirements.memoryTypeBits, kind);
    if (type == UINT32_MAX) {
        return false;
    }

    VkMemoryAllocateFlagsInfo flags_info = {};
    flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &flags_info;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = type;

    if (m_vk.allocate_memory(m_device, &allocate_info, nullptr, &created.memory) != VK_SUCCESS) {
        created.memory = VK_NULL_HANDLE;
        return false;
    }

    if (m_vk.bind_buffer_memory(m_device, created.buffer, created.memory, 0) != VK_SUCCESS) {
        return false;
    }

    bool is_host_visible = (kind.required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    if (is_host_visible) {
        VkResult mapped = m_vk.map_memory(m_device, created.memory, 0, VK_WHOLE_SIZE, 0, &created.mapping);
        if (mapped != VK_SUCCESS) {
            created.mapping = nullptr;
            return false;
        }
    }

    VkBufferDeviceAddressInfo address_info = {};
    address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address_info.buffer = created.buffer;
    created.address = m_vk.get_buffer_device_address(m_device, &address_info);

    return true;
}

std::unique_ptr<picture> gpu_compositor::make_picture(size_t bytes) {
    device_buffer buffer;
    if (!create_buffer(bytes, PICTURE_USAGE, PICTURE_MEMORY, buffer)) {
        destroy_buffer(buffer);
        return nullptr;
    }

    return std::make_unique<owned_picture>(*this, buffer, bytes);
}

std::unique_ptr<picture> gpu_compositor::make_twin(size_t bytes) {
    device_buffer buffer;
    if (!create_buffer(bytes, PICTURE_USAGE, VIDEO_MEMORY, buffer)) {
        destroy_buffer(buffer);
        return nullptr;
    }

    return std::make_unique<owned_picture>(*this, buffer, bytes);
}

uint32_t gpu_compositor::output_pitch(uint32_t width) {
    return static_cast<uint32_t>(align_up(width * BYTES_PER_PIXEL, SCANOUT_PITCH_ALIGNMENT));
}

std::unique_ptr<output_buffer> gpu_compositor::make_output_buffer(
    int gfxfb_fd,
    uint32_t output,
    uint32_t width,
    uint32_t height
) {
    uint32_t pitch = output_pitch(width);
    std::unique_ptr<scanout_buffer> buffer(new scanout_buffer(*this, gfxfb_fd, pitch));
    device_buffer& import = buffer->m_import;

    if (create_import_buffer(static_cast<VkDeviceSize>(pitch) * height, import.buffer) != VK_SUCCESS) {
        printf("stlxdm: no GPU output buffer for output %u, Vulkan refused its buffer\r\n", output);
        return nullptr;
    }

    VkMemoryRequirements requirements = {};
    m_vk.get_buffer_memory_requirements(m_device, import.buffer, &requirements);

    uint32_t type = find_memory_type(requirements.memoryTypeBits, VIDEO_MEMORY);
    if (type == UINT32_MAX) {
        printf("stlxdm: no GPU output buffer for output %u, Vulkan offers no video memory for it\r\n", output);
        return nullptr;
    }

    gpu_buffer_args create = {};
    create.size = align_up(requirements.size, m_page_size);
    create.placement = GPU_PLACEMENT_VIDEO;
    create.flags = GPU_BUFFER_SCANOUT;
    if (ioctl(m_gpu_fd, GPU_BUFFER_CREATE, &create) != 0) {
        printf("stlxdm: no GPU output buffer for output %u, GPU_BUFFER_CREATE failed: %s\r\n", output, strerror(errno));
        return nullptr;
    }

    if (!import_into_gfxfb(*buffer, create.buffer, output, width, height)) {
        close(create.buffer);
        return nullptr;
    }

    /* A failed import takes no handle, and a successful one closes it */
    if (import_memory(create.buffer, create.size, type, import.memory) != VK_SUCCESS) {
        close(create.buffer);
        printf("stlxdm: no GPU output buffer for output %u, Vulkan refused to import it\r\n", output);
        return nullptr;
    }

    if (m_vk.bind_buffer_memory(m_device, import.buffer, import.memory, 0) != VK_SUCCESS) {
        printf("stlxdm: no GPU output buffer for output %u, Vulkan refused to bind it\r\n", output);
        return nullptr;
    }

    VkBufferDeviceAddressInfo address_info = {};
    address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address_info.buffer = import.buffer;
    import.address = m_vk.get_buffer_device_address(m_device, &address_info);

    return buffer;
}

VkResult gpu_compositor::create_import_buffer(VkDeviceSize size, VkBuffer& created) {
    VkExternalMemoryBufferCreateInfo external_info = {};
    external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.pNext = &external_info;
    buffer_info.size = size;
    buffer_info.usage = PICTURE_USAGE;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = m_vk.create_buffer(m_device, &buffer_info, nullptr, &created);
    if (result != VK_SUCCESS) {
        created = VK_NULL_HANDLE;
    }

    return result;
}

/* gfxfb's import holds a reference of its own, so the copy narrowed to reading closes once the call returns */
bool gpu_compositor::import_into_gfxfb(
    scanout_buffer& buffer,
    int gpu_buffer,
    uint32_t output,
    uint32_t width,
    uint32_t height
) {
    gpu_restrict_args narrowed = {};
    narrowed.handle = gpu_buffer;
    narrowed.rights = GPU_RIGHT_READ;
    if (ioctl(m_gpu_fd, GPU_HANDLE_RESTRICT, &narrowed) != 0) {
        printf("stlxdm: no GPU output buffer for output %u, GPU_HANDLE_RESTRICT failed: %s\r\n", output,
               strerror(errno));
        return false;
    }

    gfxfb::gfxfb_import_args import = {};
    import.gpu_buffer = narrowed.restricted;
    import.width = width;
    import.height = height;
    import.pitch = buffer.m_pitch;
    import.format = gfxfb::GFXFB_FORMAT_XRGB8888;

    int import_error = ioctl(buffer.m_gfxfb_fd, gfxfb::GFXFB_IMPORT_BUFFER, &import) == 0 ? 0 : errno;
    close(narrowed.restricted);

    if (import_error != 0) {
        printf("stlxdm: no GPU output buffer for output %u, GFXFB_IMPORT_BUFFER failed: %s\r\n", output,
               strerror(import_error));
        return false;
    }

    buffer.m_gfxfb_handle = import.handle;

    return true;
}

VkResult gpu_compositor::import_memory(int gpu_buffer, uint64_t size, uint32_t type, VkDeviceMemory& imported) {
    VkMemoryAllocateFlagsInfo flags_info = {};
    flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkImportMemoryFdInfoKHR import_info = {};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.pNext = &flags_info;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    import_info.fd = gpu_buffer;

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &import_info;
    allocate_info.allocationSize = size;
    allocate_info.memoryTypeIndex = type;

    VkResult result = m_vk.allocate_memory(m_device, &allocate_info, nullptr, &imported);
    if (result != VK_SUCCESS) {
        imported = VK_NULL_HANDLE;
    }

    return result;
}

window_import_result gpu_compositor::import_window_buffer(
    int gpu_buffer,
    uint64_t bytes,
    uint32_t placement,
    bool is_mappable,
    std::unique_ptr<window_import>& imported
) {
    device_buffer import;
    VkResult result = import_window_memory(gpu_buffer, bytes, placement, is_mappable, import);
    if (result != VK_SUCCESS) {
        destroy_buffer(import);
        bool is_out_of_memory = result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return is_out_of_memory ? window_import_result::no_memory : window_import_result::refused;
    }

    imported = std::make_unique<imported_window_buffer>(*this, import);

    return window_import_result::imported;
}

/* Leaves what it made in `import` when it fails, for `destroy_buffer`. A full handle table refusing the copy of the
 * handle counts as Vulkan out of host memory */
VkResult gpu_compositor::import_window_memory(
    int gpu_buffer,
    uint64_t bytes,
    uint32_t placement,
    bool is_mappable,
    device_buffer& import
) {
    const memory_kind* kind = nullptr;
    if (placement == GPU_PLACEMENT_SYSTEM) {
        kind = &SYSTEM_WINDOW_MEMORY;
    } else if (placement == GPU_PLACEMENT_VIDEO) {
        kind = is_mappable ? &VIDEO_WINDOW_MEMORY : &VIDEO_MEMORY;
    } else {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    VkResult result = create_import_buffer(bytes, import.buffer);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkMemoryRequirements requirements = {};
    m_vk.get_buffer_memory_requirements(m_device, import.buffer, &requirements);

    uint32_t type = find_memory_type(requirements.memoryTypeBits, *kind);
    if (type == UINT32_MAX || requirements.size > bytes) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    gpu_restrict_args narrowed = {};
    narrowed.handle = gpu_buffer;
    narrowed.rights = GPU_RIGHT_READ;
    if (ioctl(m_gpu_fd, GPU_HANDLE_RESTRICT, &narrowed) != 0) {
        return errno == ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    /* A failed import takes no handle, and a successful one closes it */
    result = import_memory(narrowed.restricted, bytes, type, import.memory);
    if (result != VK_SUCCESS) {
        close(narrowed.restricted);
        return result;
    }

    result = m_vk.bind_buffer_memory(m_device, import.buffer, import.memory, 0);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkBufferDeviceAddressInfo address_info = {};
    address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address_info.buffer = import.buffer;
    import.address = m_vk.get_buffer_device_address(m_device, &address_info);

    return VK_SUCCESS;
}

bool gpu_compositor::has_gpu_failed() const {
    gpu_context_args query = {};
    query.context = m_probe_context;
    return ioctl(m_gpu_fd, GPU_CONTEXT_QUERY, &query) == 0 && query.lost == GPU_LOST_DEVICE;
}

std::unique_ptr<gpu_compositor::compose_buffers> gpu_compositor::make_compose_buffers() {
    std::unique_ptr<compose_buffers> buffers(new compose_buffers(*this));

    VkCommandBufferAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = m_command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;

    if (m_vk.allocate_command_buffers(m_device, &allocate_info, &buffers->m_commands) != VK_SUCCESS) {
        buffers->m_commands = VK_NULL_HANDLE;
        return nullptr;
    }

    return buffers;
}

/* A retired compositor's last compose may still run the command buffer, which the pool frees */
gpu_compositor::compose_buffers::~compose_buffers() {
    if (m_commands != VK_NULL_HANDLE && !m_owner.m_is_retired) {
        m_owner.m_vk.free_command_buffers(m_owner.m_device, m_owner.m_command_pool, 1, &m_commands);
    }

    m_owner.destroy_buffer(m_layer_buffer);
}

compose_result gpu_compositor::compose(
    compose_buffers& buffers,
    const compose_target& target,
    const layer_scene& scene,
    const std::vector<damage_list::rect>& rects,
    backdrop_picture& backdrop,
    frost_picture* frost
) {
    uint64_t start_ns = thread_cpu_ns();

    if (!list_layers(buffers, scene, rects)) {
        return compose_result::not_submitted;
    }

    buffers.m_copies.clear();
    backdrop.take_copies(buffers.m_copies);
    buffers.m_frost_copies.clear();
    if (frost) {
        frost->take_copies(buffers.m_frost_copies);
    }

    upload_material();
    if (!record_compose(buffers, target, scene, backdrop, frost)) {
        return compose_result::not_submitted;
    }

    uint64_t submitted_ns = 0;
    compose_result result = submit_and_wait(buffers, submitted_ns);
    if (result == compose_result::composed) {
        m_last_times = {};
        m_last_times.cpu_ns = submitted_ns - start_ns;
        m_last_times.has_gpu_time = read_gpu_time(m_last_times.gpu_ns);
    }

    return result;
}

/* Lists the layers of each of `rects` inside the scene and copies the lists into the layer buffer, grown first to
 * hold `damage_list::MAX_RECTS` lists of the scene's windows when they no longer fit. False when it cannot grow */
bool gpu_compositor::list_layers(
    compose_buffers& buffers,
    const layer_scene& scene,
    const std::vector<damage_list::rect>& rects
) {
    buffers.m_layers.clear();
    buffers.m_list_rects.clear();
    buffers.m_list_starts.clear();

    for (damage_list::rect r : rects) {
        if (!damage_list::clip(r, scene.width, scene.height)) {
            continue;
        }

        buffers.m_list_rects.push_back(r);
        buffers.m_list_starts.push_back(static_cast<uint32_t>(buffers.m_layers.size()));
        append_layers(scene, r, buffers.m_layers);
    }

    if (m_fails_next_compose && !buffers.m_list_rects.empty()) {
        buffers.m_layers.push_back(failure_layer(buffers.m_list_rects.back()));
        m_fails_next_compose = false;
    }

    VkDeviceSize bytes = buffers.m_layers.size() * sizeof(layer);
    if (bytes > buffers.m_layer_buffer_bytes) {
        destroy_buffer(buffers.m_layer_buffer);
        buffers.m_layer_buffer_bytes = 0;

        VkDeviceSize lists_bytes = damage_list::MAX_RECTS * max_layers(scene.windows.size()) * sizeof(layer);
        VkDeviceSize grown = std::max(bytes, lists_bytes);
        if (!create_buffer(grown, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, COHERENT_MEMORY, buffers.m_layer_buffer)) {
            destroy_buffer(buffers.m_layer_buffer);
            return false;
        }

        buffers.m_layer_buffer_bytes = grown;
    }

    if (bytes > 0) {
        memcpy(buffers.m_layer_buffer.mapping, buffers.m_layers.data(), bytes);
    }

    return true;
}

/* A layer over the first pixel of `r` that reads GPU address 0, below every address a context maps, or spins there */
layer gpu_compositor::failure_layer(const damage_list::rect& r) const {
    layer failing = {};
    failing.x0 = r.x;
    failing.y0 = r.y;
    failing.x1 = r.x + 1;
    failing.y1 = r.y + 1;

    failing.address = 0;
    failing.origin_x = r.x;
    failing.origin_y = r.y;
    failing.pitch = BYTES_PER_PIXEL;

    switch (m_next_failure) {
    case compose_failure::unmapped_read:
        failing.op = layer_op::copy;
        break;
    case compose_failure::spin:
        failing.op = layer_op::spin;
        break;
    }

    return failing;
}

/* Records the copies that bring `picture`'s twin up to date, building their regions in `regions` */
void gpu_compositor::record_copies(
    VkCommandBuffer commands,
    const backdrop_picture& picture,
    const std::vector<picture_copy>& copies,
    std::vector<VkBufferCopy>& regions
) {
    if (copies.empty()) {
        return;
    }

    regions.clear();
    for (const picture_copy& copy : copies) {
        regions.push_back({ copy.offset, copy.offset, copy.bytes });
    }

    VkBuffer drawn = static_cast<const owned_picture&>(picture.memory()).buffer();
    VkBuffer twin = static_cast<const owned_picture&>(picture.twin()).buffer();
    m_vk.cmd_copy_buffer(commands, drawn, twin, static_cast<uint32_t>(regions.size()), regions.data());
}

bool gpu_compositor::record_compose(
    compose_buffers& buffers,
    const compose_target& target,
    const layer_scene& scene,
    const backdrop_picture& backdrop,
    const frost_picture* frost
) {
    VkCommandBuffer commands = buffers.m_commands;

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (m_vk.begin_command_buffer(commands, &begin_info) != VK_SUCCESS) {
        return false;
    }

    if (m_timestamps != VK_NULL_HANDLE) {
        m_vk.cmd_reset_query_pool(commands, m_timestamps, 0, TIMESTAMP_COUNT);
    }

    transfer_buffers(buffers, target, scene, true);

    record_copies(commands, backdrop, buffers.m_copies, buffers.m_copy_regions);
    if (frost) {
        record_copies(commands, *frost, buffers.m_frost_copies, buffers.m_copy_regions);
    }

    if (!buffers.m_copies.empty() || !buffers.m_frost_copies.empty()) {
        VkMemoryBarrier copied = {};
        copied.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        copied.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copied.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        m_vk.cmd_pipeline_barrier(
            commands,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            1,
            &copied,
            0,
            nullptr,
            0,
            nullptr
        );
    }

    write_timestamp(commands, BEFORE_DISPATCHES);
    m_vk.cmd_bind_pipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);

    VkMemoryBarrier between = {};
    between.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    between.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    between.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    size_t list_count = buffers.m_list_rects.size();
    for (size_t i = 0; i < list_count; i++) {
        const damage_list::rect& r = buffers.m_list_rects[i];
        uint32_t first = buffers.m_list_starts[i];
        size_t end = i + 1 < list_count ? buffers.m_list_starts[i + 1] : buffers.m_layers.size();

        compose_args args = {};
        args.x0 = r.x;
        args.y0 = r.y;
        args.x1 = r.x + r.w;
        args.y1 = r.y + r.h;

        args.target = target.address;
        args.target_pitch = target.pitch;
        args.layer_count = static_cast<uint32_t>(end - first);
        args.layers = buffers.m_layer_buffer.address + first * sizeof(layer);

        args.corner_distances = m_corner_distances.address;
        args.corner_count = m_corner_count;
        args.corner_far_distance = m_corner_far_distance;
        args.material = m_material.address;

        /* Rectangles may overlap, so each dispatch waits for the writes of the one before */
        if (i > 0) {
            m_vk.cmd_pipeline_barrier(
                commands,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                1,
                &between,
                0,
                nullptr,
                0,
                nullptr
            );
        }

        m_vk.cmd_push_constants(commands, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
        m_vk.cmd_dispatch(commands, workgroups(r.w), workgroups(r.h), 1);
    }

    write_timestamp(commands, AFTER_DISPATCHES);

    if (target.readback) {
        record_readback(buffers, target);
    }

    transfer_buffers(buffers, target, scene, false);

    VkMemoryBarrier to_cpu = {};
    to_cpu.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    to_cpu.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    to_cpu.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    m_vk.cmd_pipeline_barrier(
        commands,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        1,
        &to_cpu,
        0,
        nullptr,
        0,
        nullptr
    );

    return m_vk.end_command_buffer(commands) == VK_SUCCESS;
}

void gpu_compositor::record_readback(compose_buffers& buffers, const compose_target& target) {
    VkCommandBuffer commands = buffers.m_commands;

    VkMemoryBarrier written = {};
    written.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    written.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    m_vk.cmd_pipeline_barrier(
        commands,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        1,
        &written,
        0,
        nullptr,
        0,
        nullptr
    );

    covered_rows(buffers.m_list_rects, buffers.m_readback_rows);
    buffers.m_copy_regions.clear();
    for (const span& rows : buffers.m_readback_rows) {
        VkDeviceSize offset = static_cast<VkDeviceSize>(rows.first) * target.pitch;
        VkDeviceSize bytes = static_cast<VkDeviceSize>(rows.end - rows.first) * target.pitch;
        buffers.m_copy_regions.push_back({ offset, offset, bytes });
    }

    if (buffers.m_copy_regions.empty()) {
        return;
    }

    VkBuffer scanout = static_cast<const scanout_buffer&>(*target.scanout).m_import.buffer;
    VkBuffer readback = static_cast<const owned_picture&>(*target.readback).buffer();
    uint32_t region_count = static_cast<uint32_t>(buffers.m_copy_regions.size());
    m_vk.cmd_copy_buffer(commands, scanout, readback, region_count, buffers.m_copy_regions.data());
}

/* Written once every command recorded before it has ended, so the two around the dispatches time them alone */
void gpu_compositor::write_timestamp(VkCommandBuffer commands, uint32_t query) {
    if (m_timestamps != VK_NULL_HANDLE) {
        m_vk.cmd_write_timestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestamps, query);
    }
}

/* Submits the recorded compose to raise stlxdm's timeline to the next compose point and, once Vulkan accepts it,
 * notes in `submitted_ns` the thread's CPU time at the submit's return and waits for that point */
compose_result gpu_compositor::submit_and_wait(compose_buffers& buffers, uint64_t& submitted_ns) {
    uint64_t point = m_compose_point + 1;

    VkTimelineSemaphoreSubmitInfo timeline_info = {};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = &point;

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = &timeline_info;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &buffers.m_commands;

    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &m_timeline;

    VkResult submitted = m_vk.queue_submit(m_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (submitted == VK_ERROR_DEVICE_LOST) {
        return compose_result::lost;
    }

    if (submitted != VK_SUCCESS) {
        return compose_result::not_submitted;
    }

    submitted_ns = thread_cpu_ns();
    m_compose_point = point;

    VkSemaphoreWaitInfo wait_info = {};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &m_timeline;
    wait_info.pValues = &point;

    VkResult waited = m_vk.wait_semaphores(m_device, &wait_info, COMPOSE_DEADLINE_NS);
    if (waited == VK_SUCCESS) {
        return compose_result::composed;
    }

    return waited == VK_ERROR_DEVICE_LOST ? compose_result::lost : compose_result::unfinished;
}

/* The GPU time between the timestamps of a compose that ended. False without timestamps or when their read fails */
bool gpu_compositor::read_gpu_time(uint64_t& gpu_ns) const {
    if (m_timestamps == VK_NULL_HANDLE) {
        return false;
    }

    uint64_t ticks[TIMESTAMP_COUNT] = {};
    VkResult read = m_vk.get_query_pool_results(
        m_device,
        m_timestamps,
        0,
        TIMESTAMP_COUNT,
        sizeof(ticks),
        ticks,
        sizeof(ticks[0]),
        VK_QUERY_RESULT_64_BIT
    );
    if (read != VK_SUCCESS) {
        return false;
    }

    uint64_t first = ticks[BEFORE_DISPATCHES];
    uint64_t last = ticks[AFTER_DISPATCHES];
    gpu_ns = timestamps_apart_ns(first, last, m_timestamp_bits, m_timestamp_period_ns);

    return true;
}

bool gpu_compositor::fail_next_compose(compose_failure failure) {
    if (failure == compose_failure::unmapped_read && m_contexts_may_map_zero) {
        return false;
    }

    m_fails_next_compose = true;
    m_next_failure = failure;

    return true;
}

void gpu_compositor::retire() {
    m_is_retired = true;
}

void gpu_compositor::end_retirement() {
    for (device_buffer& buffer : m_retired_buffers) {
        free_buffer(buffer);
    }

    m_retired_buffers.clear();
    m_is_retired = false;
}

bool gpu_compositor::is_retired() const {
    return m_is_retired;
}

compose_state gpu_compositor::last_compose_state() const {
    uint64_t value = 0;
    VkResult read = m_vk.get_semaphore_counter_value(m_device, m_timeline, &value);
    if (read == VK_ERROR_DEVICE_LOST) {
        return compose_state::lost;
    }

    return read == VK_SUCCESS && value >= m_compose_point ? compose_state::ended : compose_state::running;
}

/* Hands the output buffer, which the display scans out outside Vulkan, and each window buffer the scene reads, which
 * its client uses outside this device, to the compose's accesses, or back */
void gpu_compositor::transfer_buffers(
    compose_buffers& buffers,
    const compose_target& target,
    const layer_scene& scene,
    bool to_compose
) {
    buffers.m_transfers.clear();
    if (target.scanout) {
        VkBuffer scanout = static_cast<const scanout_buffer&>(*target.scanout).m_import.buffer;
        VkAccessFlags accesses = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        buffers.m_transfers.push_back(transfer_barrier(scanout, accesses, to_compose));
    }

    for (const layer_window& window : scene.windows) {
        if (window.import) {
            VkBuffer read = static_cast<const imported_window_buffer&>(*window.import).buffer();
            buffers.m_transfers.push_back(transfer_barrier(read, VK_ACCESS_SHADER_READ_BIT, to_compose));
        }
    }

    if (buffers.m_transfers.empty()) {
        return;
    }

    /* A readback's copy reads the output buffer after the dispatches, so its release waits for that copy too */
    m_vk.cmd_pipeline_barrier(
        buffers.m_commands,
        to_compose ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                   : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        to_compose ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0,
        nullptr,
        static_cast<uint32_t>(buffers.m_transfers.size()),
        buffers.m_transfers.data(),
        0,
        nullptr
    );
}

/* Moves `buffer` between the queue families outside Vulkan and the compose's queue family, whose shaders make
 * `accesses` to it */
VkBufferMemoryBarrier gpu_compositor::transfer_barrier(VkBuffer buffer, VkAccessFlags accesses, bool to_compose) const {
    VkBufferMemoryBarrier transfer = {};
    transfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    transfer.srcAccessMask = to_compose ? 0 : accesses & VK_ACCESS_SHADER_WRITE_BIT;
    transfer.dstAccessMask = to_compose ? accesses : 0;

    transfer.srcQueueFamilyIndex = to_compose ? VK_QUEUE_FAMILY_EXTERNAL : m_queue_family;
    transfer.dstQueueFamilyIndex = to_compose ? m_queue_family : VK_QUEUE_FAMILY_EXTERNAL;

    transfer.buffer = buffer;
    transfer.offset = 0;
    transfer.size = VK_WHOLE_SIZE;

    return transfer;
}

void gpu_compositor::destroy_buffer(device_buffer& buffer) {
    bool is_made = buffer.buffer != VK_NULL_HANDLE || buffer.memory != VK_NULL_HANDLE;
    if (m_is_retired && is_made) {
        m_retired_buffers.push_back(buffer);
        buffer = {};
        return;
    }

    free_buffer(buffer);
}

/* Freeing the memory also removes its mapping */
void gpu_compositor::free_buffer(device_buffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        m_vk.destroy_buffer(m_device, buffer.buffer, nullptr);
    }

    if (buffer.memory != VK_NULL_HANDLE) {
        m_vk.free_memory(m_device, buffer.memory, nullptr);
    }

    buffer = {};
}

/* Makes the probe context, and imports a page of shared memory of stlxdm's
 * own, dropped at once, which a GPU whose DMA is not coherent refuses */
const char* gpu_compositor::probe_gpu_file(int gpu_fd, const gpu_info& info) {
    gpu_context_args context = {};
    if (ioctl(gpu_fd, GPU_CONTEXT_CREATE, &context) != 0) {
        return "creating the probe context on /dev/gpu0 failed";
    }

    m_probe_context = context.context;

    int shared = memfd_create("stlxdm-probe", 0);
    if (shared < 0) {
        return "creating a shared memory object failed";
    }

    if (ftruncate(shared, info.page_size) != 0) {
        close(shared);
        return "sizing a shared memory object to a page failed";
    }

    gpu_buffer_args import = {};
    import.source = shared;
    int import_error = ioctl(gpu_fd, GPU_BUFFER_IMPORT, &import) == 0 ? 0 : errno;
    close(shared);

    if (import_error == ENOMEM) {
        return "the GPU cannot reach shared memory coherently, or memory ran out";
    }

    if (import_error != 0) {
        return "importing shared memory into /dev/gpu0 failed";
    }

    close(import.buffer);

    return nullptr;
}
