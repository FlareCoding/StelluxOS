// The window stage: a surface on a stlxwin window, offering the window's configured size as its only extent. Only the
// physical device on stlxdm's GPU, the GPU whose buffers stlxdm takes, can present to it.

#include "vktest.h"

#include <iterator>
#include <stdint.h>
#include <stdio.h>
#include <stlxwin/stlxwin.h>
#include <sys/socket.h>
#include <vector>
#include <vulkan/vulkan.h>

constexpr uint32_t WINDOW_WIDTH = 320;
constexpr uint32_t WINDOW_HEIGHT = 200;

struct window_commands {
    PFN_vkDestroyInstance                          destroy_instance;
    PFN_vkEnumeratePhysicalDevices                 enumerate_physical_devices;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties   get_queue_families;
    PFN_vkCreateStelluxSurfaceSTLX                 create_surface;
    PFN_vkDestroySurfaceKHR                        destroy_surface;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR       get_support;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR  get_capabilities;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR get_capabilities2;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR       get_formats;
    PFN_vkGetPhysicalDeviceSurfaceFormats2KHR      get_formats2;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR  get_present_modes;
    PFN_vkGetPhysicalDevicePresentRectanglesKHR    get_present_rectangles;
};

// A surface on a window of a connection of the stage's own, and the physical device on stlxdm's GPU
struct window_surface {
    stlxwin_window*  window;
    VkSurfaceKHR     surface;
    VkPhysicalDevice presenting_device;
};

// An instance with the surface extensions the stage uses, or `VK_NULL_HANDLE`
static VkInstance create_window_instance() {
    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_STLX_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
        VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_SURFACE_PROTECTED_CAPABILITIES_EXTENSION_NAME,
    };

    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "vktest";
    application.apiVersion = VK_API_VERSION_1_4;

    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &application;
    create_info.enabledExtensionCount = static_cast<uint32_t>(std::size(extensions));
    create_info.ppEnabledExtensionNames = extensions;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    return instance;
}

// The instance's physical devices, or none when enumerating them fails
static std::vector<VkPhysicalDevice> list_physical_devices(VkInstance instance, const window_commands& commands) {
    uint32_t count = 0;
    if (commands.enumerate_physical_devices(instance, &count, nullptr) != VK_SUCCESS) {
        return {};
    }

    std::vector<VkPhysicalDevice> physical_devices(count);
    if (commands.enumerate_physical_devices(instance, &count, physical_devices.data()) != VK_SUCCESS) {
        return {};
    }

    physical_devices.resize(count);
    return physical_devices;
}

// The physical device on the /dev/gpuN that `stlxwin_gpu_info_get` names, or `VK_NULL_HANDLE` when stlxdm takes no
// GPU buffers or no physical device is on that GPU
static VkPhysicalDevice find_presenting_device(
    VkInstance instance,
    const std::vector<VkPhysicalDevice>& physical_devices,
    stlxwin_conn* conn
) {
    stlxwin_gpu_info info = stlxwin_gpu_info_get(conn);
    if ((info.flags & STLXWIN_GPU_ACCEPTS) == 0) {
        printf("  stlxdm takes no GPU buffers, so no surface\n");
        return VK_NULL_HANDLE;
    }

    for (VkPhysicalDevice physical_device : physical_devices) {
        if (find_gpu_index(instance, physical_device) == info.device) {
            printf("  stlxdm takes the buffers of /dev/gpu%u\n", info.device);
            return physical_device;
        }
    }

    printf("  stlxdm takes the buffers of /dev/gpu%u, which no physical device is on, so no surface\n", info.device);
    return VK_NULL_HANDLE;
}

// Creates a surface on `target`'s window of `conn` into `target.surface`, returning the creation's result
static VkResult create_window_surface(
    VkInstance instance,
    const window_commands& commands,
    stlxwin_conn* conn,
    window_surface& target
) {
    VkStelluxSurfaceCreateInfoSTLX create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_STELLUX_SURFACE_CREATE_INFO_STLX;
    create_info.connection = conn;
    create_info.window = target.window;

    return commands.create_surface(instance, &create_info, nullptr, &target.surface);
}

// Whether every queue family of the presenting device can present to the surface and no family of another can
static bool presents_from_one_device(
    const window_commands& commands,
    const std::vector<VkPhysicalDevice>& physical_devices,
    const window_surface& target
) {
    bool holds = true;
    for (VkPhysicalDevice physical_device : physical_devices) {
        uint32_t family_count = 0;
        commands.get_queue_families(physical_device, &family_count, nullptr);
        holds &= family_count > 0;

        for (uint32_t family = 0; family < family_count; family++) {
            VkBool32 supported = VK_FALSE;
            VkResult result = commands.get_support(physical_device, family, target.surface, &supported);
            bool should_present = physical_device == target.presenting_device;
            holds &= result == VK_SUCCESS && (supported == VK_TRUE) == should_present;
        }
    }

    return holds;
}

// Reads into `capabilities` the surface's capabilities with `present_mode` chained, or with no
// `VkSurfacePresentModeKHR` when it is null, returning the query's result
static VkResult read_capabilities(
    const window_commands& commands,
    const window_surface& target,
    const VkSurfacePresentModeKHR* present_mode,
    VkSurfaceCapabilitiesKHR& capabilities
) {
    VkPhysicalDeviceSurfaceInfo2KHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surface_info.pNext = present_mode;
    surface_info.surface = target.surface;

    VkSurfaceCapabilities2KHR capabilities2 = {};
    capabilities2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    VkResult result = commands.get_capabilities2(target.presenting_device, &surface_info, &capabilities2);
    capabilities = capabilities2.surfaceCapabilities;

    return result;
}

// Whether `extent` is `width` by `height`
static bool is_size(VkExtent2D extent, uint32_t width, uint32_t height) {
    return extent.width == width && extent.height == height;
}

// Whether `capabilities` take images only of `width` by `height`, one layer and the identity transform, as color
// attachments and only with opaque alpha
static bool takes_window_images(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height) {
    bool has_configured_size = is_size(capabilities.currentExtent, width, height) &&
        is_size(capabilities.minImageExtent, width, height) && is_size(capabilities.maxImageExtent, width, height);
    bool has_one_layer = capabilities.maxImageArrayLayers == 1;
    bool has_identity_transform = capabilities.supportedTransforms == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR &&
        capabilities.currentTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    bool is_color_attachment = (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;

    return has_configured_size && has_one_layer && has_identity_transform && is_color_attachment &&
        capabilities.supportedCompositeAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

// Whether the surface's present rectangles for the presenting device are one rectangle at (0, 0) of the window's
// configured size
static bool has_window_rectangle(const window_commands& commands, const window_surface& target) {
    uint32_t width = 0;
    uint32_t height = 0;
    stlxwin_window_configured(target.window, &width, &height, nullptr);

    uint32_t rectangle_count = 0;
    VkResult result =
        commands.get_present_rectangles(target.presenting_device, target.surface, &rectangle_count, nullptr);
    std::vector<VkRect2D> rectangles(rectangle_count);
    if (result == VK_SUCCESS) {
        result = commands.get_present_rectangles(
            target.presenting_device,
            target.surface,
            &rectangle_count,
            rectangles.data()
        );
    }

    return result == VK_SUCCESS && rectangle_count == 1 && rectangles.size() == 1 && rectangles[0].offset.x == 0 &&
        rectangles[0].offset.y == 0 && is_size(rectangles[0].extent, width, height);
}

// Checks the structs a capabilities query naming MAILBOX fills, presetting each checked member to a wrong answer
static void check_chained_capabilities(const window_commands& commands, const window_surface& target) {
    uint32_t width = 0;
    uint32_t height = 0;
    stlxwin_window_configured(target.window, &width, &height, nullptr);

    VkSurfacePresentModeKHR mailbox = {};
    mailbox.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR;
    mailbox.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;

    VkPhysicalDeviceSurfaceInfo2KHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surface_info.pNext = &mailbox;
    surface_info.surface = target.surface;

    VkSurfaceCapabilitiesPresentWait2KHR present_wait2 = {};
    present_wait2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR;
    present_wait2.presentWait2Supported = VK_TRUE;

    VkSurfaceCapabilitiesPresentId2KHR present_id2 = {};
    present_id2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR;
    present_id2.pNext = &present_wait2;
    present_id2.presentId2Supported = VK_TRUE;

    VkPresentTimingSurfaceCapabilitiesEXT timing = {};
    timing.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;
    timing.pNext = &present_id2;

    timing.presentTimingSupported = VK_TRUE;
    timing.presentAtAbsoluteTimeSupported = VK_TRUE;
    timing.presentAtRelativeTimeSupported = VK_TRUE;
    timing.presentStageQueries = ~0u;

    VkPresentModeKHR compatible_modes[2] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR};
    VkSurfacePresentModeCompatibilityKHR compatibility = {};
    compatibility.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR;
    compatibility.pNext = &timing;
    compatibility.presentModeCount = 2;
    compatibility.pPresentModes = compatible_modes;

    VkSurfacePresentScalingCapabilitiesKHR scaling = {};
    scaling.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR;
    scaling.pNext = &compatibility;

    scaling.supportedPresentScaling = ~0u;
    scaling.supportedPresentGravityX = ~0u;
    scaling.supportedPresentGravityY = ~0u;
    scaling.minScaledImageExtent = {1, 1};
    scaling.maxScaledImageExtent = {1, 1};

    VkSurfaceProtectedCapabilitiesKHR protection = {};
    protection.sType = VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR;
    protection.pNext = &scaling;
    protection.supportsProtected = VK_TRUE;

    VkSurfaceCapabilities2KHR capabilities2 = {};
    capabilities2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    capabilities2.pNext = &protection;
    VkResult result = commands.get_capabilities2(target.presenting_device, &surface_info, &capabilities2);

    // A query with no array to fill counts the compatible modes
    VkSurfacePresentModeCompatibilityKHR counted = {};
    counted.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR;
    counted.presentModeCount = 2;

    VkSurfaceCapabilities2KHR counted_capabilities2 = {};
    counted_capabilities2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    counted_capabilities2.pNext = &counted;
    VkResult count_result = commands.get_capabilities2(target.presenting_device, &surface_info, &counted_capabilities2);

    bool has_no_present_id_or_wait =
        present_id2.presentId2Supported == VK_FALSE && present_wait2.presentWait2Supported == VK_FALSE;
    bool has_no_timing = timing.presentTimingSupported == VK_FALSE &&
        timing.presentAtAbsoluteTimeSupported == VK_FALSE && timing.presentAtRelativeTimeSupported == VK_FALSE &&
        timing.presentStageQueries == 0;
    bool has_no_scaling = scaling.supportedPresentScaling == 0 && scaling.supportedPresentGravityX == 0 &&
        scaling.supportedPresentGravityY == 0 && is_size(scaling.minScaledImageExtent, width, height) &&
        is_size(scaling.maxScaledImageExtent, width, height);
    bool is_mailbox_alone = compatibility.presentModeCount == 1 && compatible_modes[0] == VK_PRESENT_MODE_MAILBOX_KHR &&
        count_result == VK_SUCCESS && counted.presentModeCount == 1;
    check(
        "the surface offers MAILBOX compatible with MAILBOX alone and no present id 2, present wait 2, protected "
        "presentation, scaling or present timing",
        result == VK_SUCCESS && has_no_present_id_or_wait && has_no_timing && has_no_scaling && is_mailbox_alone &&
            protection.supportsProtected == VK_FALSE
    );
}

// Checks the image counts, extents, layers, transforms, usage, alpha, chained structs and present rectangles the
// surface offers the presenting device
static void check_capabilities(const window_commands& commands, const window_surface& target) {
    uint32_t width = 0;
    uint32_t height = 0;
    stlxwin_window_configured(target.window, &width, &height, nullptr);
    printf("  configured size: %ux%u\n", width, height);

    VkSurfaceCapabilitiesKHR capabilities = {};
    bool is_read = read_capabilities(commands, target, nullptr, capabilities) == VK_SUCCESS;

    VkSurfacePresentModeKHR fifo = {};
    fifo.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR;
    fifo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkSurfaceCapabilitiesKHR fifo_capabilities = {};
    bool is_fifo_read = read_capabilities(commands, target, &fifo, fifo_capabilities) == VK_SUCCESS;
    check(
        "the surface takes 2 to 4 images with FIFO or no present mode named, only of the window's configured size, one "
        "layer and the identity transform, as color attachments and only with opaque alpha",
        is_read && is_fifo_read && capabilities.minImageCount == 2 && capabilities.maxImageCount == 4 &&
            fifo_capabilities.minImageCount == 2 && fifo_capabilities.maxImageCount == 4 &&
            takes_window_images(capabilities, width, height) && takes_window_images(fifo_capabilities, width, height)
    );

    VkSurfacePresentModeKHR mailbox = {};
    mailbox.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR;
    mailbox.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    VkSurfaceCapabilitiesKHR mailbox_capabilities = {};
    is_read = read_capabilities(commands, target, &mailbox, mailbox_capabilities) == VK_SUCCESS;
    check(
        "the surface takes 3 to 4 images when a VkSurfacePresentModeKHR names MAILBOX",
        is_read && mailbox_capabilities.minImageCount == 3 && mailbox_capabilities.maxImageCount == 4
    );

    check_chained_capabilities(commands, target);
    check(
        "the surface's present rectangles for the device on stlxdm's GPU are one rectangle at (0, 0) of the window's "
        "configured size",
        has_window_rectangle(commands, target)
    );
}

// Whether `formats` are B8G8R8A8_UNORM and B8G8R8A8_SRGB in sRGB, and no other format
static bool is_bgra8_pair(const std::vector<VkSurfaceFormatKHR>& formats) {
    bool has_unorm = false;
    bool has_srgb = false;
    bool is_srgb_space = true;
    for (const VkSurfaceFormatKHR& format : formats) {
        has_unorm |= format.format == VK_FORMAT_B8G8R8A8_UNORM;
        has_srgb |= format.format == VK_FORMAT_B8G8R8A8_SRGB;
        is_srgb_space &= format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }

    return formats.size() == 2 && has_unorm && has_srgb && is_srgb_space;
}

// The surface's formats for the presenting device as `vkGetPhysicalDeviceSurfaceFormats2KHR` answers them, or none
// when the query fails
static std::vector<VkSurfaceFormatKHR> list_formats2(const window_commands& commands, const window_surface& target) {
    VkPhysicalDeviceSurfaceInfo2KHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surface_info.surface = target.surface;

    uint32_t count = 0;
    if (commands.get_formats2(target.presenting_device, &surface_info, &count, nullptr) != VK_SUCCESS) {
        return {};
    }

    std::vector<VkSurfaceFormat2KHR> formats2(count);
    for (VkSurfaceFormat2KHR& format2 : formats2) {
        format2.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
    }

    if (commands.get_formats2(target.presenting_device, &surface_info, &count, formats2.data()) != VK_SUCCESS) {
        return {};
    }

    std::vector<VkSurfaceFormatKHR> formats;
    for (uint32_t i = 0; i < count; i++) {
        formats.push_back(formats2[i].surfaceFormat);
    }

    return formats;
}

// Checks the formats and present modes the surface offers the presenting device
static void check_formats_and_modes(const window_commands& commands, const window_surface& target) {
    uint32_t format_count = 0;
    VkResult result = commands.get_formats(target.presenting_device, target.surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    if (result == VK_SUCCESS) {
        result = commands.get_formats(target.presenting_device, target.surface, &format_count, formats.data());
    }

    std::vector<VkSurfaceFormatKHR> formats2 = list_formats2(commands, target);
    check(
        "the surface offers B8G8R8A8_UNORM and B8G8R8A8_SRGB in sRGB, and no other format",
        result == VK_SUCCESS && format_count == 2 && is_bgra8_pair(formats) && is_bgra8_pair(formats2)
    );

    uint32_t mode_count = 0;
    result = commands.get_present_modes(target.presenting_device, target.surface, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    if (result == VK_SUCCESS) {
        result = commands.get_present_modes(target.presenting_device, target.surface, &mode_count, modes.data());
    }

    bool has_fifo = false;
    bool has_mailbox = false;
    for (VkPresentModeKHR mode : modes) {
        has_fifo |= mode == VK_PRESENT_MODE_FIFO_KHR;
        has_mailbox |= mode == VK_PRESENT_MODE_MAILBOX_KHR;
    }

    check(
        "the surface offers FIFO and MAILBOX, and no other present mode",
        result == VK_SUCCESS && mode_count == 2 && has_fifo && has_mailbox
    );
}

// Shuts the socket of a second connection with a window and a surface, checking that the surface is then lost to
// support, capabilities, formats and present modes while its present rectangles keep the window's configured size
static void check_lost_surface(
    VkInstance instance,
    const window_commands& commands,
    VkPhysicalDevice presenting_device
) {
    start_hang_watch("stlxwin_connect");
    stlxwin_conn* conn = stlxwin_connect("vktest");
    end_hang_watch();

    window_surface target = {};
    target.presenting_device = presenting_device;
    if (conn != nullptr) {
        start_hang_watch("stlxwin_window_create");
        target.window = stlxwin_window_create(conn, WINDOW_WIDTH, WINDOW_HEIGHT, "vktest", 0);
        end_hang_watch();
    }

    if (target.window != nullptr && create_window_surface(instance, commands, conn, target) != VK_SUCCESS) {
        target.surface = VK_NULL_HANDLE;
    }

    bool is_shut = target.surface != VK_NULL_HANDLE && shutdown(stlxwin_conn_fd(conn), SHUT_RDWR) == 0;
    bool is_lost = false;
    if (is_shut) {
        VkBool32 supported = VK_FALSE;
        VkResult support_result = commands.get_support(presenting_device, GENERAL_FAMILY, target.surface, &supported);

        VkSurfaceCapabilitiesKHR capabilities = {};
        VkResult capabilities_result = commands.get_capabilities(presenting_device, target.surface, &capabilities);
        VkResult capabilities2_result = read_capabilities(commands, target, nullptr, capabilities);

        uint32_t count = 0;
        VkResult formats_result = commands.get_formats(presenting_device, target.surface, &count, nullptr);

        VkPhysicalDeviceSurfaceInfo2KHR surface_info = {};
        surface_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
        surface_info.surface = target.surface;
        VkResult formats2_result = commands.get_formats2(presenting_device, &surface_info, &count, nullptr);

        VkResult modes_result = commands.get_present_modes(presenting_device, target.surface, &count, nullptr);

        is_lost = support_result == VK_ERROR_SURFACE_LOST_KHR && capabilities_result == VK_ERROR_SURFACE_LOST_KHR &&
            capabilities2_result == VK_ERROR_SURFACE_LOST_KHR && formats_result == VK_ERROR_SURFACE_LOST_KHR &&
            formats2_result == VK_ERROR_SURFACE_LOST_KHR && modes_result == VK_ERROR_SURFACE_LOST_KHR &&
            has_window_rectangle(commands, target);
    }

    check(
        "once a second connection's socket is shut, its surface is lost to support, capabilities, formats and present "
        "modes, and its present rectangles keep the window's configured size",
        is_lost
    );

    if (target.surface != VK_NULL_HANDLE) {
        commands.destroy_surface(instance, target.surface, nullptr);
    }

    if (conn != nullptr) {
        stlxwin_disconnect(conn);
    }
}

// Checks a surface on a fresh window of a connection of the stage's own
static void check_surface(VkInstance instance, const window_commands& commands) {
    std::vector<VkPhysicalDevice> physical_devices = list_physical_devices(instance, commands);
    if (physical_devices.empty()) {
        printf("  no physical device, so no window\n");
        return;
    }

    start_hang_watch("stlxwin_connect");
    stlxwin_conn* conn = stlxwin_connect("vktest");
    end_hang_watch();

    window_surface target = {};
    if (conn != nullptr) {
        start_hang_watch("stlxwin_window_create");
        target.window = stlxwin_window_create(conn, WINDOW_WIDTH, WINDOW_HEIGHT, "vktest", 0);
        end_hang_watch();
    }

    check("a stlxwin window is created on a connection of the stage's own", target.window != nullptr);
    if (target.window == nullptr) {
        if (conn != nullptr) {
            stlxwin_disconnect(conn);
        }

        return;
    }

    target.presenting_device = find_presenting_device(instance, physical_devices, conn);
    if (target.presenting_device == VK_NULL_HANDLE) {
        stlxwin_disconnect(conn);
        return;
    }

    VkResult result = create_window_surface(instance, commands, conn, target);
    check("vkCreateStelluxSurfaceSTLX creates a surface on the window", result == VK_SUCCESS);

    if (result == VK_SUCCESS) {
        check(
            "every queue family of the device on stlxdm's GPU can present to the surface, and no other device's",
            presents_from_one_device(commands, physical_devices, target)
        );
        check_capabilities(commands, target);
        check_formats_and_modes(commands, target);
        check_lost_surface(instance, commands, target.presenting_device);
        commands.destroy_surface(instance, target.surface, nullptr);
    }

    stlxwin_disconnect(conn);
}

void check_window() {
    VkInstance instance = create_window_instance();
    check(
        "vkCreateInstance succeeds with VK_KHR_surface, VK_STLX_surface, VK_KHR_get_surface_capabilities2, "
        "VK_KHR_surface_maintenance1 and VK_KHR_surface_protected_capabilities",
        instance != VK_NULL_HANDLE
    );
    if (instance == VK_NULL_HANDLE) {
        return;
    }

    window_commands commands = {
        .destroy_instance = find_command<PFN_vkDestroyInstance>(instance, "vkDestroyInstance"),
        .enumerate_physical_devices =
            find_command<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices"),
        .get_queue_families = find_command<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            instance,
            "vkGetPhysicalDeviceQueueFamilyProperties"
        ),
        .create_surface = find_command<PFN_vkCreateStelluxSurfaceSTLX>(instance, "vkCreateStelluxSurfaceSTLX"),
        .destroy_surface = find_command<PFN_vkDestroySurfaceKHR>(instance, "vkDestroySurfaceKHR"),
        .get_support = find_command<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
            instance,
            "vkGetPhysicalDeviceSurfaceSupportKHR"
        ),
        .get_capabilities = find_command<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            instance,
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"
        ),
        .get_capabilities2 = find_command<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
            instance,
            "vkGetPhysicalDeviceSurfaceCapabilities2KHR"
        ),
        .get_formats = find_command<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
            instance,
            "vkGetPhysicalDeviceSurfaceFormatsKHR"
        ),
        .get_formats2 = find_command<PFN_vkGetPhysicalDeviceSurfaceFormats2KHR>(
            instance,
            "vkGetPhysicalDeviceSurfaceFormats2KHR"
        ),
        .get_present_modes = find_command<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
            instance,
            "vkGetPhysicalDeviceSurfacePresentModesKHR"
        ),
        .get_present_rectangles = find_command<PFN_vkGetPhysicalDevicePresentRectanglesKHR>(
            instance,
            "vkGetPhysicalDevicePresentRectanglesKHR"
        ),
    };

    bool has_commands = commands.destroy_instance && commands.enumerate_physical_devices &&
        commands.get_queue_families && commands.create_surface && commands.destroy_surface && commands.get_support &&
        commands.get_capabilities && commands.get_capabilities2 && commands.get_formats && commands.get_formats2 &&
        commands.get_present_modes && commands.get_present_rectangles;
    check("vkGetInstanceProcAddr finds the window commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    check_surface(instance, commands);
    commands.destroy_instance(instance, nullptr);
}
