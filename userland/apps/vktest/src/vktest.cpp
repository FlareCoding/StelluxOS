// Tests the Vulkan library a Stellux program links, one stage per part of the driver. Every
// check prints a PASS or FAIL line, and the program exits with 1 when any check failed.

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <vulkan/vulkan.h>

static int g_checks = 0;
static int g_failed_checks = 0;

static void check(const char* name, bool passed) {
    printf("  %s: %s\n", passed ? "PASS" : "FAIL", name);
    g_checks++;

    if (!passed) {
        g_failed_checks++;
    }
}

static bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const VkExtensionProperties& extension : extensions) {
        if (strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }

    return false;
}

// The library exports only the global commands, so every other command comes from `vkGetInstanceProcAddr`
template <typename Command>
static Command find_command(VkInstance instance, const char* name) {
    return reinterpret_cast<Command>(vkGetInstanceProcAddr(instance, name));
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

    // Returning is the check, since a null device reaching NVK's dispatch ends the program before its summary
    destroy_device(VK_NULL_HANDLE, nullptr);
    check("vkDestroyDevice returns for a null device", true);

    destroy_instance(instance, nullptr);
}

int main() {
    printf("vktest: instance\n");
    check_loading_by_name();
    check_global_commands();
    check_instance();

    printf("vktest: %d of %d checks failed\n", g_failed_checks, g_checks);

    return g_failed_checks == 0 ? 0 : 1;
}
