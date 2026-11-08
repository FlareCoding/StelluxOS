#include "gpu_compositor.hpp"
#include "decor.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stlx/gpu_abi.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
};

static_assert(sizeof(compose_args) == 56);

/* The version that makes timeline semaphores and buffer device addresses core */
constexpr uint32_t VULKAN_API_VERSION = VK_API_VERSION_1_2;

constexpr float QUEUE_PRIORITY = 1.0f;

/* Every picture and output buffer the compose reads or writes is a storage
 * buffer named by its device address */
constexpr VkBufferUsageFlags PICTURE_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

constexpr VkMemoryPropertyFlags COHERENT_MEMORY = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

template <typename Command>
static bool find_command(VkInstance instance, const char* name, Command& command) {
    command = reinterpret_cast<Command>(vkGetInstanceProcAddr(instance, name));
    return command != nullptr;
}

static bool is_at_address(const gpu_info& info, const VkPhysicalDevicePCIBusInfoPropertiesEXT& pci_bus) {
    return info.pci_segment == pci_bus.pciDomain && info.pci_bus == pci_bus.pciBus &&
        info.pci_slot == pci_bus.pciDevice && info.pci_function == pci_bus.pciFunction;
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

    destroy_buffer(m_corner_distances);

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

    refusal = upload_corner_distances();
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
        find_command(m_instance, "vkDestroyPipeline", m_vk.destroy_pipeline);
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

uint32_t gpu_compositor::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags properties) const {
    for (uint32_t type = 0; type < m_memory_properties.memoryTypeCount; type++) {
        bool is_allowed = (type_bits & (1u << type)) != 0;
        bool has_properties = (m_memory_properties.memoryTypes[type].propertyFlags & properties) == properties;
        if (is_allowed && has_properties) {
            return type;
        }
    }

    return UINT32_MAX;
}

bool gpu_compositor::create_buffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
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

    uint32_t type = find_memory_type(requirements.memoryTypeBits, properties);
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

    bool is_host_visible = (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
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

/* Freeing the memory also removes its mapping */
void gpu_compositor::destroy_buffer(device_buffer& buffer) {
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
