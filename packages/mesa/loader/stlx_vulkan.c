// The Vulkan loader of a Stellux program, with no layers and one driver, NVK, linked into the same
// program. It also answers `dlopen` of libvulkan with its own handle, and `dlsym` of that handle.

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

struct loader_command {
    const char* name;
    PFN_vkVoidFunction function;
};

static const struct loader_command loader_commands[] = {
    { "vkGetInstanceProcAddr", (PFN_vkVoidFunction)vkGetInstanceProcAddr },
    { "vkCreateInstance", (PFN_vkVoidFunction)vkCreateInstance },
    { "vkEnumerateInstanceVersion", (PFN_vkVoidFunction)vkEnumerateInstanceVersion },
    { "vkEnumerateInstanceExtensionProperties", (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties },
    { "vkEnumerateInstanceLayerProperties", (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties },
};

static const char* const libvulkan_names[] = { "libvulkan.so.1", "libvulkan.so" };

// Its address is the handle `dlopen` returns for libvulkan, which no handle of musl's can equal
static char libvulkan_handle;

static pthread_once_t icd_negotiation = PTHREAD_ONCE_INIT;

// NVK's `vkDestroyDevice` as `vkGetInstanceProcAddr` returns it, which dispatches through its device
static PFN_vkDestroyDevice nvk_destroy_device;

void* __real_dlopen(const char* file, int mode);
void* __real_dlsym(void* restrict handle, const char* restrict name);
int __real_dlclose(void* handle);

static void negotiate_icd_interface(void) {
    uint32_t version = CURRENT_LOADER_ICD_INTERFACE_VERSION;
    vk_icdNegotiateLoaderICDInterfaceVersion(&version);
}

static PFN_vkVoidFunction get_nvk_proc_addr(VkInstance instance, const char* name) {
    pthread_once(&icd_negotiation, negotiate_icd_interface);
    return vk_icdGetInstanceProcAddr(instance, name);
}

// The Vulkan specification lets a program destroy a null device, which NVK's dispatch cannot
static VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice device, const VkAllocationCallbacks* allocator) {
    if (device == VK_NULL_HANDLE) {
        return;
    }

    PFN_vkDestroyDevice destroy = __atomic_load_n(&nvk_destroy_device, __ATOMIC_ACQUIRE);
    destroy(device, allocator);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    for (size_t i = 0; i < sizeof(loader_commands) / sizeof(loader_commands[0]); i++) {
        if (strcmp(name, loader_commands[i].name) == 0) {
            return loader_commands[i].function;
        }
    }

    if (strcmp(name, "vkDestroyDevice") == 0) {
        PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)get_nvk_proc_addr(instance, name);
        if (destroy == NULL) {
            return NULL;
        }

        __atomic_store_n(&nvk_destroy_device, destroy, __ATOMIC_RELEASE);
        return (PFN_vkVoidFunction)destroy_device;
    }

    return get_nvk_proc_addr(instance, name);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkInstance* instance
) {
    // NVK leaves checking the requested layers to its loader, and this loader has none
    if (create_info->enabledLayerCount > 0) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)get_nvk_proc_addr(VK_NULL_HANDLE, "vkCreateInstance");
    return create_instance(create_info, allocator, instance);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* api_version) {
    PFN_vkEnumerateInstanceVersion enumerate_version =
        (PFN_vkEnumerateInstanceVersion)get_nvk_proc_addr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    return enumerate_version(api_version);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* layer_name,
    uint32_t* property_count,
    VkExtensionProperties* properties
) {
    PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)get_nvk_proc_addr(
            VK_NULL_HANDLE,
            "vkEnumerateInstanceExtensionProperties"
        );
    return enumerate_extensions(layer_name, property_count, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    uint32_t* property_count,
    VkLayerProperties* properties
) {
    (void)properties;
    *property_count = 0;
    return VK_SUCCESS;
}

static bool is_libvulkan_name(const char* file) {
    for (size_t i = 0; i < sizeof(libvulkan_names) / sizeof(libvulkan_names[0]); i++) {
        if (strcmp(file, libvulkan_names[i]) == 0) {
            return true;
        }
    }

    return false;
}

void* __wrap_dlopen(const char* file, int mode) {
    if (file != NULL && is_libvulkan_name(file)) {
        return &libvulkan_handle;
    }

    return __real_dlopen(file, mode);
}

void* __wrap_dlsym(void* restrict handle, const char* restrict name) {
    if (handle == &libvulkan_handle) {
        return (void*)vkGetInstanceProcAddr(VK_NULL_HANDLE, name);
    }

    return __real_dlsym(handle, name);
}

int __wrap_dlclose(void* handle) {
    if (handle == &libvulkan_handle) {
        return 0;
    }

    return __real_dlclose(handle);
}
