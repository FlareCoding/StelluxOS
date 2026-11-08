#include "gpu_compositor.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stlx/gpu_abi.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

/* The version that makes timeline semaphores and buffer device addresses core */
constexpr uint32_t VULKAN_API_VERSION = VK_API_VERSION_1_2;

constexpr float QUEUE_PRIORITY = 1.0f;

/* Every picture and output buffer the compose reads or writes is a storage
 * buffer named by its device address */
constexpr VkBufferUsageFlags PICTURE_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

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

    return probe_gpu_file(gpu_fd, info);
}

bool gpu_compositor::find_commands() {
    return find_command(m_instance, "vkDestroyInstance", m_vk.destroy_instance) &&
        find_command(m_instance, "vkEnumeratePhysicalDevices", m_vk.enumerate_physical_devices) &&
        find_command(m_instance, "vkEnumerateDeviceExtensionProperties", m_vk.enumerate_device_extensions) &&
        find_command(m_instance, "vkGetPhysicalDeviceProperties2", m_vk.get_physical_device_properties) &&
        find_command(m_instance, "vkGetPhysicalDeviceFeatures2", m_vk.get_physical_device_features) &&
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
        find_command(m_instance, "vkDestroySemaphore", m_vk.destroy_semaphore);
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
        return "the Vulkan device cannot import memory descriptors";
    }

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
