// Tests the Vulkan library a Stellux program links, one stage per part of the driver. Every check prints a PASS or FAIL
// line, and the program exits with 1 when any check failed, at once when the watchdog fails a hung call.

#include "vktest.h"

#include <algorithm>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stlx/gpu_abi.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <vulkan/vulkan.h>

// What one /dev/gpuN reports, which NVK's physical device for that GPU must match
struct gpu_node {
    gpu_info   info;
    gpu_memory memory;
    bool       has_memory;
};

static int g_checks = 0;
static int g_failed_checks = 0;

// Never destroyed, since the detached watchdog may still take it while `exit` runs the static destructors
static std::mutex& g_watch_lock = *new std::mutex;

// The watched call, or null, and its deadline. Once the watchdog finds a hang, it holds `g_watch_lock` until it exits,
// so the main thread, which must print nothing during a watch, can neither end that watch nor print past the summary.
static const char* g_watched_call = nullptr;
static uint64_t g_watch_deadline_ns = 0;

void check(const char* name, bool passed) {
    printf("  %s: %s\n", passed ? "PASS" : "FAIL", name);
    g_checks++;

    if (!passed) {
        g_failed_checks++;
    }
}

static void print_summary() {
    printf("vktest: %d of %d checks failed\n", g_failed_checks, g_checks);
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const VkExtensionProperties& extension : extensions) {
        if (strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }

    return false;
}

uint64_t monotonic_ns() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec);
}

void start_hang_watch(const char* call) {
    std::lock_guard<std::mutex> hold(g_watch_lock);
    g_watch_deadline_ns = monotonic_ns() + HANG_LIMIT_NS;
    g_watched_call = call;
}

void end_hang_watch() {
    std::lock_guard<std::mutex> hold(g_watch_lock);
    g_watched_call = nullptr;
}

// Fails a check naming the watched call at the first look that finds it past its deadline, then ends vktest with the
// summary, since the call may never return
static void watch_for_hangs() {
    timespec poll = {0, static_cast<long>(WAIT_SLICE_NS)};
    for (;;) {
        nanosleep(&poll, nullptr);

        g_watch_lock.lock();
        if (g_watched_call != nullptr && monotonic_ns() >= g_watch_deadline_ns) {
            break;
        }

        g_watch_lock.unlock();
    }

    char name[128];
    unsigned long long limit_s = HANG_LIMIT_NS / 1000000000;
    snprintf(name, sizeof(name), "%s returns within %llu s", g_watched_call, limit_s);
    check(name, false);
    print_summary();

    // `exit` would run exit handlers and destructors while the hung call still runs, and `_exit` does not flush stdout
    fflush(stdout);
    _exit(1);
}

template <typename Command>
static bool finds_linked_command(const char* name, Command command) {
    return vkGetInstanceProcAddr(VK_NULL_HANDLE, name) == reinterpret_cast<PFN_vkVoidFunction>(command);
}

// Programs that load Vulkan by name, as most toolkits do, find the library inside themselves
static void check_loading_by_name() {
    void* library = dlopen("libvulkan.so.1", RTLD_NOW);
    check("dlopen of libvulkan.so.1 returns a handle", library != nullptr);
    if (library == nullptr) {
        return;
    }

    void* get_instance_proc_addr = dlsym(library, "vkGetInstanceProcAddr");
    bool is_linked_command = get_instance_proc_addr == reinterpret_cast<void*>(vkGetInstanceProcAddr);
    check("dlsym finds the linked vkGetInstanceProcAddr", is_linked_command);

    void* unversioned_library = dlopen("libvulkan.so", RTLD_NOW);
    check("dlopen of libvulkan.so answers with the same library", unversioned_library == library);
    check("dlclose of libvulkan.so.1 succeeds", dlclose(library) == 0);
}

static void check_global_commands() {
    bool finds_global_commands = finds_linked_command("vkGetInstanceProcAddr", vkGetInstanceProcAddr) &&
        finds_linked_command("vkCreateInstance", vkCreateInstance) &&
        finds_linked_command("vkEnumerateInstanceVersion", vkEnumerateInstanceVersion) &&
        finds_linked_command("vkEnumerateInstanceExtensionProperties", vkEnumerateInstanceExtensionProperties) &&
        finds_linked_command("vkEnumerateInstanceLayerProperties", vkEnumerateInstanceLayerProperties);
    check("vkGetInstanceProcAddr without an instance finds the global commands", finds_global_commands);

    uint32_t api_version = 0;
    VkResult result = vkEnumerateInstanceVersion(&api_version);
    check("vkEnumerateInstanceVersion succeeds", result == VK_SUCCESS);
    check("vkEnumerateInstanceVersion reports Vulkan 1.4 or later", api_version >= VK_API_VERSION_1_4);

    uint32_t layer_count = 1;
    result = vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    check("vkEnumerateInstanceLayerProperties reports no layer", result == VK_SUCCESS && layer_count == 0);

    uint32_t extension_count = 0;
    result = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (result == VK_SUCCESS) {
        result = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data());
    }

    bool lists_properties2 = result == VK_SUCCESS &&
        has_extension(extensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    check("vkEnumerateInstanceExtensionProperties lists VK_KHR_get_physical_device_properties2", lists_properties2);
    check("no display surface is offered", !has_extension(extensions, VK_KHR_DISPLAY_EXTENSION_NAME));
    check("no headless surface is offered", !has_extension(extensions, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME));
}

// Reads what /dev/gpu0 and each file after it report, up to the first file that does not open, leaving out, as
// NVK does, any file whose GPU_GET_INFO fails
static std::vector<gpu_node> read_gpu_nodes() {
    std::vector<gpu_node> nodes;
    for (uint32_t index = 0;; index++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/gpu%u", index);

        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            return nodes;
        }

        gpu_node node = {};
        bool has_info = ioctl(fd, GPU_GET_INFO, &node.info) == 0;
        node.has_memory = ioctl(fd, GPU_GET_MEMORY, &node.memory) == 0;
        close(fd);

        if (has_info) {
            nodes.push_back(node);
        }
    }
}

static bool is_at_address(const gpu_info& info, const VkPhysicalDevicePCIBusInfoPropertiesEXT& pci_bus) {
    return info.pci_segment == pci_bus.pciDomain && info.pci_bus == pci_bus.pciBus &&
        info.pci_slot == pci_bus.pciDevice && info.pci_function == pci_bus.pciFunction;
}

static const gpu_node* find_gpu_node_at(
    const std::vector<gpu_node>& nodes,
    const VkPhysicalDevicePCIBusInfoPropertiesEXT& pci_bus
) {
    for (const gpu_node& node : nodes) {
        if (is_at_address(node.info, pci_bus)) {
            return &node;
        }
    }

    return nullptr;
}

static VkPhysicalDevicePCIBusInfoPropertiesEXT read_pci_bus(
    PFN_vkGetPhysicalDeviceProperties2 get_properties,
    VkPhysicalDevice physical_device
) {
    VkPhysicalDevicePCIBusInfoPropertiesEXT pci_bus = {};
    pci_bus.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 properties = {};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &pci_bus;
    get_properties(physical_device, &properties);

    return pci_bus;
}

// Opens the /dev/gpuN at `physical_device`'s PCI address, or returns -1
static int open_gpu_file_of(VkInstance instance, VkPhysicalDevice physical_device) {
    auto get_properties = find_command<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
    if (!get_properties) {
        return -1;
    }

    VkPhysicalDevicePCIBusInfoPropertiesEXT pci_bus = read_pci_bus(get_properties, physical_device);
    for (uint32_t index = 0;; index++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/gpu%u", index);

        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            return -1;
        }

        gpu_info info = {};
        if (ioctl(fd, GPU_GET_INFO, &info) == 0 && is_at_address(info, pci_bus)) {
            return fd;
        }

        close(fd);
    }
}

int restrict_to_read(VkInstance instance, VkPhysicalDevice physical_device, int descriptor) {
    int gpu_fd = open_gpu_file_of(instance, physical_device);
    if (gpu_fd < 0) {
        return -1;
    }

    gpu_restrict_args restriction = {};
    restriction.handle = descriptor;
    restriction.rights = GPU_RIGHT_READ;
    int result = ioctl(gpu_fd, GPU_HANDLE_RESTRICT, &restriction);
    close(gpu_fd);

    return result == 0 ? restriction.restricted : -1;
}

// The `GPU_TIMELINE_QUERY` reports of the timelines reading `value` among the descriptors below the soft
// `RLIMIT_NOFILE` limit, each query made through `gpu_fd`
static std::vector<gpu_timeline_args> query_timelines_at(int gpu_fd, uint64_t value) {
    std::vector<gpu_timeline_args> found;
    rlimit descriptor_limit = {};
    if (getrlimit(RLIMIT_NOFILE, &descriptor_limit) != 0) {
        return found;
    }

    for (rlim_t descriptor = 0; descriptor < descriptor_limit.rlim_cur; descriptor++) {
        gpu_timeline_args query = {};
        query.timeline = static_cast<int32_t>(descriptor);
        if (ioctl(gpu_fd, GPU_TIMELINE_QUERY, &query) == 0 && query.value == value) {
            found.push_back(query);
        }
    }

    return found;
}

bool replace_timeline_descriptor(VkInstance instance, VkPhysicalDevice physical_device, uint64_t value) {
    int gpu_fd = open_gpu_file_of(instance, physical_device);
    if (gpu_fd < 0) {
        return false;
    }

    std::vector<gpu_timeline_args> found = query_timelines_at(gpu_fd, value);

    // Closing the descriptor instead would free its number for a later one, which the semaphore's destroy would close
    bool is_replaced = !found.empty() && dup2(gpu_fd, found[0].timeline) == found[0].timeline;
    close(gpu_fd);

    return is_replaced;
}

uint32_t count_failed_timelines(VkInstance instance, VkPhysicalDevice physical_device, uint64_t value) {
    int gpu_fd = open_gpu_file_of(instance, physical_device);
    if (gpu_fd < 0) {
        return 0;
    }

    std::vector<gpu_timeline_args> found = query_timelines_at(gpu_fd, value);
    close(gpu_fd);

    auto is_failed = [](const gpu_timeline_args& timeline) {
        return (timeline.flags & GPU_TIMELINE_FAILED) != 0;
    };

    return static_cast<uint32_t>(std::count_if(found.begin(), found.end(), is_failed));
}

// NVK accepts every chip the kernel drives, so its physical devices are the NVIDIA GPUs with a 3D class
static bool lists_exactly_the_nvidia_3d_gpus(
    VkInstance instance,
    const std::vector<VkPhysicalDevice>& physical_devices,
    const std::vector<gpu_node>& nodes
) {
    auto get_properties = find_command<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
    if (!get_properties) {
        return false;
    }

    std::vector<VkPhysicalDevicePCIBusInfoPropertiesEXT> addresses;
    for (VkPhysicalDevice physical_device : physical_devices) {
        addresses.push_back(read_pci_bus(get_properties, physical_device));
    }

    size_t nvidia_3d_gpu_count = 0;
    for (const gpu_node& node : nodes) {
        bool is_nvidia_3d = node.info.backend == GPU_BACKEND_NVIDIA && node.info.nvidia.class_3d != 0;
        if (!is_nvidia_3d) {
            continue;
        }

        size_t listings = 0;
        for (const VkPhysicalDevicePCIBusInfoPropertiesEXT& address : addresses) {
            listings += is_at_address(node.info, address) ? 1 : 0;
        }

        if (listings != 1) {
            return false;
        }

        nvidia_3d_gpu_count++;
    }

    return nvidia_3d_gpu_count == addresses.size();
}

// Vulkan requires every device to support a sampled 2D R8G8B8A8_UNORM image with optimal tiling, so only the
// sparse flags can refuse it
static bool refuses_sparse_image(
    PFN_vkGetPhysicalDeviceImageFormatProperties2 get_image_format,
    VkPhysicalDevice physical_device,
    VkImageCreateFlags flags
) {
    VkPhysicalDeviceImageFormatInfo2 format_info = {};
    format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    format_info.flags = flags;

    format_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    format_info.type = VK_IMAGE_TYPE_2D;
    format_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    format_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

    VkImageFormatProperties2 properties = {};
    properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    VkResult result = get_image_format(physical_device, &format_info, &properties);

    return result == VK_ERROR_FORMAT_NOT_SUPPORTED;
}

static void check_physical_device(
    VkInstance instance,
    VkPhysicalDevice physical_device,
    const std::vector<gpu_node>& nodes
) {
    auto get_properties = find_command<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
    auto get_features = find_command<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2");
    auto get_memory_properties =
        find_command<PFN_vkGetPhysicalDeviceMemoryProperties2>(instance, "vkGetPhysicalDeviceMemoryProperties2");
    auto get_queue_families = find_command<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        instance,
        "vkGetPhysicalDeviceQueueFamilyProperties"
    );
    auto get_image_format = find_command<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
        instance,
        "vkGetPhysicalDeviceImageFormatProperties2"
    );

    bool has_commands = get_properties && get_features && get_memory_properties && get_queue_families &&
        get_image_format;
    check("vkGetInstanceProcAddr finds the physical device commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    VkPhysicalDeviceShaderSMBuiltinsPropertiesNV sm_builtins = {};
    sm_builtins.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_PROPERTIES_NV;
    VkPhysicalDevicePCIBusInfoPropertiesEXT pci_bus = {};
    pci_bus.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
    pci_bus.pNext = &sm_builtins;

    VkPhysicalDeviceProperties2 properties = {};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &pci_bus;
    get_properties(physical_device, &properties);

    const VkPhysicalDeviceProperties& device_properties = properties.properties;
    printf(
        "  %s at %04x:%02x:%02x.%x\n",
        device_properties.deviceName,
        pci_bus.pciDomain,
        pci_bus.pciBus,
        pci_bus.pciDevice,
        pci_bus.pciFunction
    );

    const gpu_node* node = find_gpu_node_at(nodes, pci_bus);
    bool has_gpu = node != nullptr && node->info.backend == GPU_BACKEND_NVIDIA;
    check("a /dev/gpuN reports an NVIDIA GPU at the physical device's PCI address", has_gpu);
    if (!has_gpu) {
        return;
    }

    const gpu_info& info = node->info;
    bool has_pci_ids = device_properties.vendorID == info.vendor_id && device_properties.deviceID == info.device_id;
    bool has_identity = has_pci_ids && device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    check("the physical device is a discrete GPU with GPU_GET_INFO's vendor and device ids", has_identity);

    size_t name_length = strnlen(info.name, sizeof(info.name));
    char expected_name_start[sizeof(info.name) + 8];
    snprintf(expected_name_start, sizeof(expected_name_start), "%.*s (NVK ", static_cast<int>(name_length), info.name);
    bool has_name = strncmp(device_properties.deviceName, expected_name_start, strlen(expected_name_start)) == 0;
    check("deviceName starts with GPU_GET_INFO's name followed by \" (NVK \"", has_name);

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 memory = {};
    memory.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    memory.pNext = &budget;
    get_memory_properties(physical_device, &memory);

    // NVK lists video memory first and system memory last
    const VkPhysicalDeviceMemoryProperties& memory_properties = memory.memoryProperties;
    uint32_t heap_count = memory_properties.memoryHeapCount;
    uint32_t system_heap_index = heap_count > 0 ? heap_count - 1 : 0;
    const VkMemoryHeap& video_heap = memory_properties.memoryHeaps[0];
    const VkMemoryHeap& system_heap = memory_properties.memoryHeaps[system_heap_index];

    bool is_video_heap = heap_count >= 2 && (video_heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
    bool has_video_heap = is_video_heap && video_heap.size == info.video_size;
    check("the first heap is device local and holds GPU_GET_INFO's video memory", has_video_heap);

    uint64_t video_free = node->memory.video_free;
    bool is_at_most_free = budget.heapBudget[0] <= video_free;
    bool is_over_half_free = budget.heapBudget[0] > video_free / 2;
    bool follows_free_memory = has_video_heap && node->has_memory && is_at_most_free && is_over_half_free;
    check(
        "the video memory heap's budget is over half of GPU_GET_MEMORY's free video memory and at most all of it",
        follows_free_memory
    );

    bool has_system_heap = heap_count >= 2 && system_heap.flags == 0;
    check(
        "the last heap is system memory with a nonzero budget",
        has_system_heap && budget.heapBudget[system_heap_index] > 0
    );

    printf(
        "  heap budgets: video %llu of %llu MiB, system %llu of %llu MiB\n",
        static_cast<unsigned long long>(budget.heapBudget[0] >> 20),
        static_cast<unsigned long long>(video_heap.size >> 20),
        static_cast<unsigned long long>(budget.heapBudget[system_heap_index] >> 20),
        static_cast<unsigned long long>(system_heap.size >> 20)
    );
    printf("  SMs: %u, %u warps each\n", sm_builtins.shaderSMCount, sm_builtins.shaderWarpsPerSM);

    uint64_t mappable_video_size = std::min(info.mappable_video_size, info.video_size);
    bool has_mappable_heap = false;
    for (uint32_t index = 0; index < heap_count; index++) {
        const VkMemoryHeap& heap = memory_properties.memoryHeaps[index];
        bool is_device_local = (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        has_mappable_heap |= is_device_local && heap.size == mappable_video_size;
    }

    check("a device local heap holds GPU_GET_INFO's mappable video memory", has_mappable_heap);

    uint32_t family_count = 0;
    get_queue_families(physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    get_queue_families(physical_device, &family_count, families.data());

    bool has_graphics_family = false;
    bool has_transfer_only_family = false;
    bool family_offers_sparse_binding = false;
    for (const VkQueueFamilyProperties& family : families) {
        bool is_transfer_only = (family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0;
        has_graphics_family |= (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        has_transfer_only_family |= is_transfer_only && (family.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
        family_offers_sparse_binding |= (family.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;
    }

    bool reports_copy_queue = (info.queue_kinds & (1u << GPU_QUEUE_COPY)) != 0;
    bool transfer_family_matches = has_transfer_only_family == reports_copy_queue;
    check("a queue family offers graphics", has_graphics_family);
    check("a transfer-only family exists exactly when GPU_GET_INFO reports a copy queue", transfer_family_matches);

    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT float_atomics = {};
    float_atomics.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT int64_atomics = {};
    int64_atomics.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT;
    int64_atomics.pNext = &float_atomics;

    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &int64_atomics;
    get_features(physical_device, &features2);

    const VkPhysicalDeviceFeatures& features = features2.features;
    bool offers_sparse_residency = features.shaderResourceResidency || features.sparseResidencyBuffer ||
        features.sparseResidencyImage2D || features.sparseResidencyImage3D || features.sparseResidency2Samples ||
        features.sparseResidency4Samples || features.sparseResidency8Samples || features.sparseResidency16Samples ||
        features.sparseResidencyAliased;
    bool offers_sparse_atomics = float_atomics.sparseImageFloat32Atomics ||
        float_atomics.sparseImageFloat32AtomicAdd || int64_atomics.sparseImageInt64Atomics;

    const VkPhysicalDeviceSparseProperties& sparse_properties = device_properties.sparseProperties;
    bool advertises_sparse_properties = device_properties.limits.sparseAddressSpaceSize != 0 ||
        sparse_properties.residencyStandard2DBlockShape || sparse_properties.residencyStandard3DBlockShape ||
        sparse_properties.residencyStandard2DMultisampleBlockShape || sparse_properties.residencyAlignedMipSize ||
        sparse_properties.residencyNonResidentStrict;
    bool advertises_sparse = family_offers_sparse_binding || features.sparseBinding || offers_sparse_residency ||
        offers_sparse_atomics || advertises_sparse_properties;
    check("no queue family, feature or property advertises sparse binding or residency", !advertises_sparse);

    // The residency query leaves out the binding flag, since the binding refusal would also refuse a query with both
    bool refuses_binding = refuses_sparse_image(get_image_format, physical_device, VK_IMAGE_CREATE_SPARSE_BINDING_BIT);
    bool refuses_residency =
        refuses_sparse_image(get_image_format, physical_device, VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT);
    check(
        "the image format query refuses a sampled 2D R8G8B8A8_UNORM image with sparse binding or residency",
        refuses_binding && refuses_residency
    );
}

static void check_instance() {
    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "vktest";
    application.apiVersion = VK_API_VERSION_1_4;

    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &application;

    const char* layer_name = "VK_LAYER_KHRONOS_validation";
    VkInstanceCreateInfo layer_create_info = create_info;
    layer_create_info.enabledLayerCount = 1;
    layer_create_info.ppEnabledLayerNames = &layer_name;

    VkInstance layer_instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&layer_create_info, nullptr, &layer_instance);
    check("vkCreateInstance refuses an absent layer", result == VK_ERROR_LAYER_NOT_PRESENT);

    VkInstance instance = VK_NULL_HANDLE;
    result = vkCreateInstance(&create_info, nullptr, &instance);
    check("vkCreateInstance succeeds", result == VK_SUCCESS);
    if (result != VK_SUCCESS) {
        return;
    }

    auto destroy_instance = find_command<PFN_vkDestroyInstance>(instance, "vkDestroyInstance");
    auto enumerate_physical_devices =
        find_command<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
    auto destroy_device = find_command<PFN_vkDestroyDevice>(instance, "vkDestroyDevice");
    bool has_commands = destroy_instance && enumerate_physical_devices && destroy_device;
    check("vkGetInstanceProcAddr finds the instance and device commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    uint32_t physical_device_count = 0;
    result = enumerate_physical_devices(instance, &physical_device_count, nullptr);
    check("vkEnumeratePhysicalDevices succeeds", result == VK_SUCCESS);
    printf("  physical devices: %u\n", physical_device_count);

    std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
    if (result == VK_SUCCESS && physical_device_count > 0) {
        result = enumerate_physical_devices(instance, &physical_device_count, physical_devices.data());
        check("vkEnumeratePhysicalDevices lists the devices it counted", result == VK_SUCCESS);
        physical_devices.resize(physical_device_count);
    }

    std::vector<gpu_node> nodes = read_gpu_nodes();
    check(
        "one physical device per NVIDIA GPU whose GPU_GET_INFO reports a 3D class, and no other",
        lists_exactly_the_nvidia_3d_gpus(instance, physical_devices, nodes)
    );

    // Returning is the check, since a null device reaching NVK's dispatch ends the program before its summary
    destroy_device(VK_NULL_HANDLE, nullptr);
    check("vkDestroyDevice returns for a null device", true);

    for (VkPhysicalDevice physical_device : physical_devices) {
        printf("vktest: physical device\n");
        check_physical_device(instance, physical_device, nodes);

        printf("vktest: device\n");
        check_device(instance, physical_device);

        printf("vktest: memory\n");
        check_memory(instance, physical_device);

        printf("vktest: timeline\n");
        check_timeline(instance, physical_device);

        printf("vktest: transfer\n");
        check_transfer(instance, physical_device);

        printf("vktest: compute\n");
        check_compute(instance, physical_device);

        printf("vktest: triangle\n");
        check_triangle(instance, physical_device);

        printf("vktest: queue\n");
        check_queue(instance, physical_device);

        printf("vktest: loss\n");
        check_loss(instance, physical_device);

        printf("vktest: fault\n");
        check_fault(instance, physical_device);
    }

    destroy_instance(instance, nullptr);
}

int main() {
    // The watchdog runs until vktest exits, so it is never joined
    std::thread(watch_for_hangs).detach();

    printf("vktest: instance\n");
    check_loading_by_name();
    check_global_commands();
    check_instance();

    print_summary();

    return g_failed_checks == 0 ? 0 : 1;
}
