// The window stage: swapchains on a surface on a stlxwin window, its resize, and a second connection's GPU buffer
// budget and loss. Only the physical device on stlxdm's GPU, the GPU whose buffers stlxdm takes, presents to a surface.

#include "vktest.h"

#include <iterator>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stlxwin/stlxwin.h>
#include <sys/socket.h>
#include <thread>
#include <time.h>
#include <vector>
#include <vulkan/vulkan.h>

constexpr uint32_t WINDOW_WIDTH = 320;
constexpr uint32_t WINDOW_HEIGHT = 200;
constexpr uint64_t PRESENT_FRAMES = 1000;
constexpr uint32_t BUDGET_IMAGES = 4;

// Swapchains of `BUDGET_IMAGES` images that hold a connection's whole GPU buffer budget
constexpr uint32_t BUDGET_SWAPCHAINS = STLXWIN_GPU_BUFFERS_MAX / BUDGET_IMAGES;

// How long the resize check presents while it waits for a drag of the window's edge
constexpr uint64_t DRAG_WAIT_NS = 300 * WAIT_SLICE_NS;

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

struct present_commands {
    PFN_vkDestroyDevice         destroy_device;
    PFN_vkDeviceWaitIdle        device_wait_idle;
    PFN_vkCreateFence           create_fence;
    PFN_vkDestroyFence          destroy_fence;
    PFN_vkWaitForFences         wait_for_fences;
    PFN_vkResetFences           reset_fences;
    PFN_vkCmdClearColorImage    cmd_clear_color_image;
    PFN_vkCreateSwapchainKHR    create_swapchain;
    PFN_vkDestroySwapchainKHR   destroy_swapchain;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images;
    PFN_vkAcquireNextImageKHR   acquire_next_image;
    PFN_vkQueuePresentKHR       queue_present;
    PFN_vkWaitForPresentKHR     wait_for_present;
};

// A surface on a window of a connection of the stage's own, and the physical device on stlxdm's GPU
struct window_surface {
    stlxwin_window*  window;
    VkSurfaceKHR     surface;
    VkPhysicalDevice presenting_device;
};

// A device on the presenting physical device with swapchains, present ids and present waits, its general queue, and a
// fence each acquire signals, reset once waited
struct present_stage {
    work_commands    work;
    present_commands presents;
    VkDevice         device;
    VkQueue          queue;
    VkFence          acquire_fence;
};

// Fresh windows of a second connection, whose GPU buffers count toward its budget alone, with their surfaces and the
// swapchains made on them
struct budget_windows {
    stlxwin_conn*               conn;
    std::vector<window_surface> targets;
    std::vector<VkSwapchainKHR> swapchains;
};

// A swapchain's present mode, the image count it asks for and the count it takes. FIFO asks for 3, so an image stays
// free while stlxdm holds two pictures and only the present's own wait keeps a frame from replacing an unshown one.
struct present_case {
    const char*       name;
    VkPresentModeKHR  mode;
    uint32_t          requested_images;
    uint32_t          image_count;
    bool              checks_previous_shown;
    VkClearColorValue color;
    const char*       swapchain_check;
    const char*       frames_check;
};

static const present_case PRESENT_CASES[] = {
    {
        "FIFO",
        VK_PRESENT_MODE_FIFO_KHR,
        3,
        3,
        true,
        {{0.0f, 0.5f, 0.5f, 1.0f}},
        "a FIFO swapchain asked for 3 images of the window's configured size has 3",
        "1000 FIFO frames are acquired, cleared and presented, each present returning only after the frame before it "
        "shows, and a wait for the last present returns VK_SUCCESS",
    },
    {
        "MAILBOX",
        VK_PRESENT_MODE_MAILBOX_KHR,
        2,
        3,
        false,
        {{0.5f, 0.25f, 0.0f, 1.0f}},
        "a MAILBOX swapchain asked for 2 images has 3, since stlxdm holds two pictures of a window",
        "1000 MAILBOX frames are acquired, cleared and presented, and a wait for the last present returns VK_SUCCESS",
    },
};

static const VkClearColorValue FRAME_COLOR = {{0.25f, 0.0f, 0.5f, 1.0f}};

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

    VkSurfaceCapabilitiesPresentId2KHR present_id2 = {};
    present_id2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR;
    present_id2.pNext = &present_wait2;

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

    bool has_present_id_and_wait =
        present_id2.presentId2Supported == VK_TRUE && present_wait2.presentWait2Supported == VK_TRUE;
    bool has_no_timing = timing.presentTimingSupported == VK_FALSE &&
        timing.presentAtAbsoluteTimeSupported == VK_FALSE && timing.presentAtRelativeTimeSupported == VK_FALSE &&
        timing.presentStageQueries == 0;
    bool has_no_scaling = scaling.supportedPresentScaling == 0 && scaling.supportedPresentGravityX == 0 &&
        scaling.supportedPresentGravityY == 0 && is_size(scaling.minScaledImageExtent, width, height) &&
        is_size(scaling.maxScaledImageExtent, width, height);
    bool is_mailbox_alone = compatibility.presentModeCount == 1 && compatible_modes[0] == VK_PRESENT_MODE_MAILBOX_KHR &&
        count_result == VK_SUCCESS && counted.presentModeCount == 1;
    check(
        "the surface offers present id 2 and present wait 2 but no protected presentation, scaling or present timing, "
        "and MAILBOX compatible with MAILBOX alone",
        result == VK_SUCCESS && has_present_id_and_wait && has_no_timing && has_no_scaling && is_mailbox_alone &&
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

// Finds the commands the swapchains need into `presents`, returning whether it found every one
static bool find_present_commands(VkInstance instance, present_commands& presents) {
    presents = {
        .destroy_device = find_command<PFN_vkDestroyDevice>(instance, "vkDestroyDevice"),
        .device_wait_idle = find_command<PFN_vkDeviceWaitIdle>(instance, "vkDeviceWaitIdle"),
        .create_fence = find_command<PFN_vkCreateFence>(instance, "vkCreateFence"),
        .destroy_fence = find_command<PFN_vkDestroyFence>(instance, "vkDestroyFence"),
        .wait_for_fences = find_command<PFN_vkWaitForFences>(instance, "vkWaitForFences"),
        .reset_fences = find_command<PFN_vkResetFences>(instance, "vkResetFences"),
        .cmd_clear_color_image = find_command<PFN_vkCmdClearColorImage>(instance, "vkCmdClearColorImage"),
        .create_swapchain = find_command<PFN_vkCreateSwapchainKHR>(instance, "vkCreateSwapchainKHR"),
        .destroy_swapchain = find_command<PFN_vkDestroySwapchainKHR>(instance, "vkDestroySwapchainKHR"),
        .get_swapchain_images = find_command<PFN_vkGetSwapchainImagesKHR>(instance, "vkGetSwapchainImagesKHR"),
        .acquire_next_image = find_command<PFN_vkAcquireNextImageKHR>(instance, "vkAcquireNextImageKHR"),
        .queue_present = find_command<PFN_vkQueuePresentKHR>(instance, "vkQueuePresentKHR"),
        .wait_for_present = find_command<PFN_vkWaitForPresentKHR>(instance, "vkWaitForPresentKHR"),
    };

    return presents.destroy_device && presents.device_wait_idle && presents.create_fence && presents.destroy_fence &&
        presents.wait_for_fences && presents.reset_fences && presents.cmd_clear_color_image &&
        presents.create_swapchain && presents.destroy_swapchain && presents.get_swapchain_images &&
        presents.acquire_next_image && presents.queue_present && presents.wait_for_present;
}

// A device on `physical_device` with the swapchain, present id and present wait extensions, or `VK_NULL_HANDLE`
static VkDevice create_presenting_device(VkInstance instance, VkPhysicalDevice physical_device) {
    VkPhysicalDevicePresentWaitFeaturesKHR present_wait = {};
    present_wait.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    present_wait.presentWait = VK_TRUE;

    VkPhysicalDevicePresentIdFeaturesKHR present_id = {};
    present_id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    present_id.pNext = &present_wait;
    present_id.presentId = VK_TRUE;

    return create_device(
        instance,
        physical_device,
        {GENERAL_FAMILY},
        {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PRESENT_ID_EXTENSION_NAME, VK_KHR_PRESENT_WAIT_EXTENSION_NAME},
        &present_id
    );
}

// Creates a swapchain in `mode` asking for `image_count` images of the window's configured size, into `swapchain`,
// which a failure leaves null, and retires `old_swapchain` unless it is null
static VkResult create_swapchain(
    const present_stage& stage,
    const window_surface& target,
    VkPresentModeKHR mode,
    uint32_t image_count,
    VkSwapchainKHR old_swapchain,
    VkSwapchainKHR& swapchain
) {
    uint32_t width = 0;
    uint32_t height = 0;
    stlxwin_window_configured(target.window, &width, &height, nullptr);

    VkSwapchainCreateInfoKHR create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = target.surface;
    create_info.minImageCount = image_count;
    create_info.presentMode = mode;
    create_info.clipped = VK_TRUE;

    create_info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    create_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    create_info.imageExtent = {width, height};
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;

    create_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.oldSwapchain = old_swapchain;

    start_hang_watch("vkCreateSwapchainKHR");
    VkResult result = stage.presents.create_swapchain(stage.device, &create_info, nullptr, &swapchain);
    end_hang_watch();

    if (result != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
    }

    return result;
}

// The images of `swapchain`, or none when listing them fails
static std::vector<VkImage> list_swapchain_images(const present_stage& stage, VkSwapchainKHR swapchain) {
    uint32_t count = 0;
    if (stage.presents.get_swapchain_images(stage.device, swapchain, &count, nullptr) != VK_SUCCESS) {
        return {};
    }

    std::vector<VkImage> images(count);
    if (stage.presents.get_swapchain_images(stage.device, swapchain, &count, images.data()) != VK_SUCCESS) {
        return {};
    }

    images.resize(count);
    return images;
}

// Records a clear of `image` to `color` into `command_buffer`, leaving the image in the layout presents take
static void record_clear(
    const present_stage& stage,
    VkImage image,
    const VkClearColorValue& color,
    VkCommandBuffer command_buffer
) {
    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;

    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    stage.work.cmd_pipeline_barrier(
        command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );

    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    stage.presents.cmd_clear_color_image(command_buffer, image, layout, &color, 1, &range);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    stage.work.cmd_pipeline_barrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );
}

// Presents image `index` as `present_id`, waiting for the present when `is_waited`
static VkResult present_image(
    const present_stage& stage,
    VkSwapchainKHR swapchain,
    uint32_t index,
    uint64_t present_id,
    bool is_waited
) {
    VkPresentIdKHR id_info = {};
    id_info.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    id_info.swapchainCount = 1;
    id_info.pPresentIds = &present_id;

    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.pNext = &id_info;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain;
    present_info.pImageIndices = &index;

    start_hang_watch("vkQueuePresentKHR");
    VkResult result = stage.presents.queue_present(stage.queue, &present_info);
    end_hang_watch();

    if (result == VK_SUCCESS && is_waited) {
        start_hang_watch("vkWaitForPresentKHR");
        result = stage.presents.wait_for_present(stage.device, swapchain, present_id, UINT64_MAX);
        end_hang_watch();
    }

    return result;
}

// Acquires an image of `swapchain` into `index` and clears it to `color`, returning the first result other than
// `VK_SUCCESS`
static VkResult acquire_cleared_image(
    const present_stage& stage,
    VkSwapchainKHR swapchain,
    const std::vector<VkImage>& images,
    const VkClearColorValue& color,
    uint32_t& index
) {
    start_hang_watch("vkAcquireNextImageKHR");
    VkResult result = stage.presents.acquire_next_image(
        stage.device,
        swapchain,
        UINT64_MAX,
        VK_NULL_HANDLE,
        stage.acquire_fence,
        &index
    );
    end_hang_watch();

    if (result == VK_SUCCESS) {
        start_hang_watch("vkWaitForFences");
        result = stage.presents.wait_for_fences(stage.device, 1, &stage.acquire_fence, VK_TRUE, UINT64_MAX);
        end_hang_watch();
    }

    if (result == VK_SUCCESS) {
        result = stage.presents.reset_fences(stage.device, 1, &stage.acquire_fence);
    }

    if (result != VK_SUCCESS) {
        return result;
    }

    auto record = [&](VkCommandBuffer command_buffer) {
        record_clear(stage, images[index], color, command_buffer);
    };

    return run_once(stage.work, stage.device, GENERAL_FAMILY, record);
}

// Acquires, clears and presents `PRESENT_FRAMES` frames, waiting for the last and, when `checks_previous_shown`,
// finding the frame before each present shown once it returns, returning the first result other than `VK_SUCCESS`
static VkResult present_frames(
    const present_stage& stage,
    VkSwapchainKHR swapchain,
    const std::vector<VkImage>& images,
    const present_case& present
) {
    VkResult result = VK_SUCCESS;
    for (uint64_t present_id = 1; result == VK_SUCCESS && present_id <= PRESENT_FRAMES; present_id++) {
        uint32_t index = 0;
        result = acquire_cleared_image(stage, swapchain, images, present.color, index);
        if (result == VK_SUCCESS) {
            result = present_image(stage, swapchain, index, present_id, present_id == PRESENT_FRAMES);
        }

        if (result == VK_SUCCESS && present.checks_previous_shown && present_id > 1) {
            start_hang_watch("vkWaitForPresentKHR");
            result = stage.presents.wait_for_present(stage.device, swapchain, present_id - 1, 0);
            end_hang_watch();
        }
    }

    return result;
}

// Acquires, clears and presents one frame of `swapchain` as `present_id`, waiting for the present
static VkResult present_waited_frame(const present_stage& stage, VkSwapchainKHR swapchain, uint64_t present_id) {
    std::vector<VkImage> images = list_swapchain_images(stage, swapchain);
    if (images.empty()) {
        return VK_ERROR_UNKNOWN;
    }

    uint32_t index = 0;
    VkResult result = acquire_cleared_image(stage, swapchain, images, FRAME_COLOR, index);
    if (result != VK_SUCCESS) {
        return result;
    }

    return present_image(stage, swapchain, index, present_id, true);
}

// Destroys `swapchain` under the hang watch
static void destroy_swapchain(const present_stage& stage, VkSwapchainKHR swapchain) {
    start_hang_watch("vkDestroySwapchainKHR");
    stage.presents.destroy_swapchain(stage.device, swapchain, nullptr);
    end_hang_watch();
}

// Waits for the stage's device to idle under the hang watch
static void wait_for_device_idle(const present_stage& stage) {
    start_hang_watch("vkDeviceWaitIdle");
    stage.presents.device_wait_idle(stage.device);
    end_hang_watch();
}

// Presents `PRESENT_FRAMES` frames through a swapchain made for `present`, checking its image count and its frames
static void check_presents(const present_stage& stage, const window_surface& target, const present_case& present) {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    create_swapchain(stage, target, present.mode, present.requested_images, VK_NULL_HANDLE, swapchain);
    std::vector<VkImage> images;
    if (swapchain != VK_NULL_HANDLE) {
        images = list_swapchain_images(stage, swapchain);
    }

    check(present.swapchain_check, images.size() == present.image_count);
    if (!images.empty()) {
        uint64_t start_ns = monotonic_ns();
        VkResult result = present_frames(stage, swapchain, images, present);
        uint64_t elapsed_ms = (monotonic_ns() - start_ns) / 1000000;
        printf(
            "  %s's frames ended with VkResult %d after %llu ms\n",
            present.name,
            static_cast<int>(result),
            static_cast<unsigned long long>(elapsed_ms)
        );
        check(present.frames_check, result == VK_SUCCESS);

        wait_for_device_idle(stage);
    }

    if (swapchain != VK_NULL_HANDLE) {
        destroy_swapchain(stage, swapchain);
    }
}

// Drops the events queued on `conn`, which the stage never acts on, so a wait on it blocks until new ones arrive
static void drop_events(stlxwin_conn* conn) {
    stlxwin_event event;
    while (stlxwin_next_event(conn, &event)) {
        continue;
    }
}

// Whether stlxdm reports where `window` is within `WORK_WAIT_NS`, which it does once the window's first commit shows
static bool position_arrives(stlxwin_conn* conn, const stlxwin_window* window) {
    uint64_t deadline = monotonic_ns() + WORK_WAIT_NS;

    while (stlxwin_window_get_output(window, nullptr, nullptr, nullptr) != 0) {
        drop_events(conn);

        uint64_t now = monotonic_ns();
        if (now >= deadline || stlxwin_wait(conn, static_cast<int64_t>(deadline - now)) < 0) {
            return false;
        }
    }

    return true;
}

// Prints the point in desktop coordinates just right of the window's content, where a drag resizes its right edge
static void print_drag_point(stlxwin_window* window) {
    int32_t x = 0;
    int32_t y = 0;
    stlxwin_window_get_output(window, nullptr, &x, &y);

    uint32_t width = 0;
    uint32_t height = 0;
    stlxwin_window_configured(window, &width, &height, nullptr);

    printf(
        "  waiting %llu s for a drag of the window's right edge from (%d, %d)\n",
        static_cast<unsigned long long>(DRAG_WAIT_NS / 1000000000),
        x + static_cast<int32_t>(width),
        y + static_cast<int32_t>(height / 2)
    );

    // Whoever drags acts on the line while the stage waits, so the line cannot sit in stdout's buffer
    fflush(stdout);
}

// Presents frames of `swapchain` with the ids after `present_id`, left as the last one presented, returning the first
// result other than `VK_SUCCESS`, or `VK_TIMEOUT` once `DRAG_WAIT_NS` passes
static VkResult present_until_resized(
    const present_stage& stage,
    stlxwin_conn* conn,
    VkSwapchainKHR swapchain,
    const std::vector<VkImage>& images,
    uint64_t& present_id
) {
    uint64_t deadline = monotonic_ns() + DRAG_WAIT_NS;
    while (monotonic_ns() < deadline) {
        // A configure reaches the next present only once dispatched
        stlxwin_dispatch(conn);
        drop_events(conn);

        uint32_t index = 0;
        VkResult result = acquire_cleared_image(stage, swapchain, images, FRAME_COLOR, index);
        if (result == VK_SUCCESS) {
            present_id++;
            result = present_image(stage, swapchain, index, present_id, false);
        }

        if (result != VK_SUCCESS) {
            return result;
        }
    }

    return VK_TIMEOUT;
}

// Checks that a drag of the window's edge makes a present report the swapchain suboptimal, that retiring it makes a
// present on it and a wait for that present begun earlier find it out of date, and that its successor presents
static void check_resize(const present_stage& stage, stlxwin_conn* conn, const window_surface& target) {
    VkSwapchainKHR old_swapchain = VK_NULL_HANDLE;
    create_swapchain(stage, target, VK_PRESENT_MODE_FIFO_KHR, 2, VK_NULL_HANDLE, old_swapchain);

    std::vector<VkImage> images;
    if (old_swapchain != VK_NULL_HANDLE) {
        images = list_swapchain_images(stage, old_swapchain);
    }

    uint64_t present_id = 1;
    bool is_shown = !images.empty() && present_waited_frame(stage, old_swapchain, present_id) == VK_SUCCESS;
    VkResult resized = VK_ERROR_UNKNOWN;
    if (is_shown && position_arrives(conn, target.window)) {
        print_drag_point(target.window);
        resized = present_until_resized(stage, conn, old_swapchain, images, present_id);
    }

    check(
        "a drag of the window's right edge while it presents makes a present report the swapchain suboptimal",
        resized == VK_SUBOPTIMAL_KHR
    );

    uint32_t width = 0;
    uint32_t height = 0;
    stlxwin_window_configured(target.window, &width, &height, nullptr);
    printf("  configured size at the recreation: %ux%u\n", width, height);

    uint32_t held_index = 0;
    VkResult held = VK_ERROR_UNKNOWN;
    if (is_shown) {
        held = acquire_cleared_image(stage, old_swapchain, images, FRAME_COLOR, held_index);
    }

    uint64_t held_id = present_id + 1;
    VkResult waited = VK_ERROR_UNKNOWN;
    std::thread waiter;
    if (held == VK_SUCCESS) {
        waiter = std::thread([&] {
            waited = stage.presents.wait_for_present(stage.device, old_swapchain, held_id, UINT64_MAX);
        });

        // A slice lets the wait begin before the recreation retires the swapchain
        timespec slice = {0, static_cast<long>(WAIT_SLICE_NS)};
        nanosleep(&slice, nullptr);
    }

    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    create_swapchain(stage, target, VK_PRESENT_MODE_FIFO_KHR, 2, old_swapchain, new_swapchain);

    VkResult presented = held;
    if (held == VK_SUCCESS) {
        presented = present_image(stage, old_swapchain, held_index, held_id, false);
    }

    check(
        "a present on a swapchain retired by its recreation with oldSwapchain finds it out of date",
        presented == VK_ERROR_OUT_OF_DATE_KHR
    );

    if (waiter.joinable()) {
        start_hang_watch("vkWaitForPresentKHR");
        waiter.join();
        end_hang_watch();
    }

    check(
        "a wait with no timeout for that present, begun before the recreation, finds it out of date",
        waited == VK_ERROR_OUT_OF_DATE_KHR
    );

    VkResult result = VK_ERROR_UNKNOWN;
    if (new_swapchain != VK_NULL_HANDLE) {
        result = present_waited_frame(stage, new_swapchain, 1);
    }

    check("the recreated swapchain presents a frame, and a wait for it returns VK_SUCCESS", result == VK_SUCCESS);

    wait_for_device_idle(stage);
    for (VkSwapchainKHR swapchain : {old_swapchain, new_swapchain}) {
        if (swapchain != VK_NULL_HANDLE) {
            destroy_swapchain(stage, swapchain);
        }
    }
}

// Creates a surface on a fresh window of `conn` into `target`, leaving the window or the surface null on failure
static void create_budget_target(
    VkInstance instance,
    const window_commands& commands,
    stlxwin_conn* conn,
    window_surface& target
) {
    start_hang_watch("stlxwin_window_create");
    target.window = stlxwin_window_create(conn, WINDOW_WIDTH, WINDOW_HEIGHT, "vktest", 0);
    end_hang_watch();

    if (target.window != nullptr && create_window_surface(instance, commands, conn, target) != VK_SUCCESS) {
        target.surface = VK_NULL_HANDLE;
    }
}

// Fills a second connection's GPU buffer budget with swapchains on fresh windows, checking that one more is refused
// with `VK_ERROR_OUT_OF_HOST_MEMORY`, that one refused midway detaches its images, and that the first still presents
static void check_budget(
    VkInstance instance,
    const window_commands& commands,
    const present_stage& stage,
    VkPhysicalDevice presenting_device,
    budget_windows& budget
) {
    start_hang_watch("stlxwin_connect");
    budget.conn = stlxwin_connect("vktest");
    end_hang_watch();

    budget.targets.resize(BUDGET_SWAPCHAINS + 1);
    bool has_surfaces = budget.conn != nullptr;
    for (window_surface& target : budget.targets) {
        target.presenting_device = presenting_device;
        if (has_surfaces) {
            create_budget_target(instance, commands, budget.conn, target);
            has_surfaces = target.surface != VK_NULL_HANDLE;
        }
    }

    bool fills_budget = has_surfaces;
    for (uint32_t i = 0; fills_budget && i < BUDGET_SWAPCHAINS; i++) {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkResult result = create_swapchain(
            stage,
            budget.targets[i],
            VK_PRESENT_MODE_FIFO_KHR,
            BUDGET_IMAGES,
            VK_NULL_HANDLE,
            swapchain
        );
        if (swapchain != VK_NULL_HANDLE) {
            budget.swapchains.push_back(swapchain);
        }

        fills_budget = result == VK_SUCCESS && list_swapchain_images(stage, swapchain).size() == BUDGET_IMAGES;
    }

    check(
        "4 swapchains of 4 images on fresh windows of a second connection fill its budget of 16 GPU buffers",
        fills_budget
    );

    VkResult result = VK_ERROR_UNKNOWN;
    if (fills_budget) {
        VkSwapchainKHR refused = VK_NULL_HANDLE;
        result = create_swapchain(stage, budget.targets.back(), VK_PRESENT_MODE_FIFO_KHR, 2, VK_NULL_HANDLE, refused);
        if (refused != VK_NULL_HANDLE) {
            budget.swapchains.push_back(refused);
        }
    }

    check(
        "a fifth swapchain on that connection is refused as out of host memory",
        result == VK_ERROR_OUT_OF_HOST_MEMORY
    );

    // The last of the 4 swapchains remade at 2 images leaves 2 of the 16 buffers free, so a swapchain asking for 4
    // images attaches 2 before it is refused
    VkResult over_budget = VK_ERROR_UNKNOWN;
    VkResult after_detach = VK_ERROR_UNKNOWN;
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY && budget.swapchains.size() == BUDGET_SWAPCHAINS) {
        VkSwapchainKHR& last = budget.swapchains.back();
        destroy_swapchain(stage, last);
        VkResult remade = create_swapchain(
            stage,
            budget.targets[BUDGET_SWAPCHAINS - 1],
            VK_PRESENT_MODE_FIFO_KHR,
            2,
            VK_NULL_HANDLE,
            last
        );

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        if (remade == VK_SUCCESS) {
            over_budget = create_swapchain(
                stage,
                budget.targets.back(),
                VK_PRESENT_MODE_FIFO_KHR,
                BUDGET_IMAGES,
                VK_NULL_HANDLE,
                swapchain
            );
        }

        if (over_budget == VK_ERROR_OUT_OF_HOST_MEMORY) {
            after_detach =
                create_swapchain(stage, budget.targets.back(), VK_PRESENT_MODE_FIFO_KHR, 2, VK_NULL_HANDLE, swapchain);
        }

        // Destroyed at once, since check_connection_loss creates another on that window
        if (swapchain != VK_NULL_HANDLE) {
            destroy_swapchain(stage, swapchain);
        }
    }

    check(
        "a swapchain refused after 2 of its images attached detaches them, so a 2-image swapchain then fits in the "
        "budget",
        over_budget == VK_ERROR_OUT_OF_HOST_MEMORY && after_detach == VK_SUCCESS
    );

    result = VK_ERROR_UNKNOWN;
    if (!budget.swapchains.empty()) {
        result = present_waited_frame(stage, budget.swapchains[0], 1);
    }

    check(
        "the first of the 4 swapchains then presents a frame, and a wait for it returns VK_SUCCESS",
        result == VK_SUCCESS
    );
}

// Shuts the second connection's socket, which ends it while stlxdm and the first connection run on, checking that its
// surfaces are lost to presents, present waits, acquires, queries and new swapchains, which still retire old ones
static void check_connection_loss(
    const window_commands& commands,
    const present_stage& stage,
    const budget_windows& budget
) {
    bool has_swapchain = !budget.swapchains.empty();
    uint32_t held_index = 0;
    VkResult held = VK_ERROR_UNKNOWN;
    if (has_swapchain) {
        std::vector<VkImage> images = list_swapchain_images(stage, budget.swapchains[0]);
        held = acquire_cleared_image(stage, budget.swapchains[0], images, FRAME_COLOR, held_index);
    }

    bool is_shut = budget.conn != nullptr && shutdown(stlxwin_conn_fd(budget.conn), SHUT_RDWR) == 0;

    VkResult presented = VK_ERROR_UNKNOWN;
    if (is_shut && held == VK_SUCCESS) {
        presented = present_image(stage, budget.swapchains[0], held_index, 2, false);
    }

    check(
        "once the second connection's socket is shut, a present on its first swapchain finds the surface lost",
        presented == VK_ERROR_SURFACE_LOST_KHR
    );

    VkResult waited = VK_ERROR_UNKNOWN;
    if (presented == VK_ERROR_SURFACE_LOST_KHR) {
        start_hang_watch("vkWaitForPresentKHR");
        waited = stage.presents.wait_for_present(stage.device, budget.swapchains[0], 2, UINT64_MAX);
        end_hang_watch();
    }

    check("a wait with no timeout for that present finds the surface lost", waited == VK_ERROR_SURFACE_LOST_KHR);

    bool is_lost = is_shut && has_swapchain;
    if (is_lost) {
        uint32_t index = 0;
        start_hang_watch("vkAcquireNextImageKHR");
        VkResult acquired = stage.presents.acquire_next_image(
            stage.device,
            budget.swapchains[0],
            UINT64_MAX,
            VK_NULL_HANDLE,
            stage.acquire_fence,
            &index
        );
        end_hang_watch();

        VkSurfaceCapabilitiesKHR capabilities = {};
        VkResult read = read_capabilities(commands, budget.targets[0], nullptr, capabilities);

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkResult created =
            create_swapchain(stage, budget.targets.back(), VK_PRESENT_MODE_FIFO_KHR, 2, VK_NULL_HANDLE, swapchain);
        if (swapchain != VK_NULL_HANDLE) {
            destroy_swapchain(stage, swapchain);
        }

        is_lost = acquired == VK_ERROR_SURFACE_LOST_KHR && read == VK_ERROR_SURFACE_LOST_KHR &&
            created == VK_ERROR_SURFACE_LOST_KHR;
    }

    check("an acquire, a capabilities query and a new swapchain on that connection's surfaces find them lost", is_lost);

    bool is_retired = false;
    if (is_shut && budget.swapchains.size() > 1) {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkResult created =
            create_swapchain(stage, budget.targets[1], VK_PRESENT_MODE_FIFO_KHR, 2, budget.swapchains[1], swapchain);
        if (swapchain != VK_NULL_HANDLE) {
            destroy_swapchain(stage, swapchain);
        }

        uint32_t index = 0;
        start_hang_watch("vkAcquireNextImageKHR");
        VkResult acquired = stage.presents.acquire_next_image(
            stage.device,
            budget.swapchains[1],
            UINT64_MAX,
            VK_NULL_HANDLE,
            stage.acquire_fence,
            &index
        );
        end_hang_watch();

        is_retired = created == VK_ERROR_SURFACE_LOST_KHR && acquired == VK_ERROR_OUT_OF_DATE_KHR;
    }

    check(
        "a creation on a lost surface still retires its oldSwapchain, so an acquire on it finds it out of date",
        is_retired
    );
}

// Destroys the budget's swapchains, surfaces and connection once the stage's device idles
static void destroy_budget_windows(
    VkInstance instance,
    const window_commands& commands,
    const present_stage& stage,
    const budget_windows& budget
) {
    wait_for_device_idle(stage);
    for (VkSwapchainKHR swapchain : budget.swapchains) {
        destroy_swapchain(stage, swapchain);
    }

    for (const window_surface& target : budget.targets) {
        if (target.surface != VK_NULL_HANDLE) {
            commands.destroy_surface(instance, target.surface, nullptr);
        }
    }

    if (budget.conn != nullptr) {
        stlxwin_disconnect(budget.conn);
    }
}

// Checks presents, a resize, a connection's budget and its loss with swapchains of a device of the stage's own
static void check_swapchains(
    VkInstance instance,
    const window_commands& commands,
    stlxwin_conn* conn,
    const window_surface& target
) {
    present_stage stage = {};
    bool has_commands = find_work_commands(instance, stage.work) && find_present_commands(instance, stage.presents);
    if (has_commands) {
        stage.device = create_presenting_device(instance, target.presenting_device);
    }

    check(
        "a device on stlxdm's GPU is created with VK_KHR_swapchain, VK_KHR_present_id and VK_KHR_present_wait",
        stage.device != VK_NULL_HANDLE
    );
    if (stage.device == VK_NULL_HANDLE) {
        return;
    }

    stage.work.get_device_queue(stage.device, GENERAL_FAMILY, 0, &stage.queue);

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkResult result = stage.presents.create_fence(stage.device, &fence_info, nullptr, &stage.acquire_fence);
    check("a fence for acquires to signal is created on that device", result == VK_SUCCESS);

    if (result == VK_SUCCESS) {
        for (const present_case& present : PRESENT_CASES) {
            check_presents(stage, target, present);
        }

        check_resize(stage, conn, target);

        budget_windows budget = {};
        check_budget(instance, commands, stage, target.presenting_device, budget);
        check_connection_loss(commands, stage, budget);
        destroy_budget_windows(instance, commands, stage, budget);

        stage.presents.destroy_fence(stage.device, stage.acquire_fence, nullptr);
    }

    stage.presents.destroy_device(stage.device, nullptr);
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
        target.window = stlxwin_window_create(conn, WINDOW_WIDTH, WINDOW_HEIGHT, "vktest", STLXWIN_WF_RESIZABLE);
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
        check_swapchains(instance, commands, conn, target);
        commands.destroy_surface(instance, target.surface, nullptr);
    }

    stlxwin_disconnect(conn);
}

// Finds the window commands of `instance` into `commands`, returning whether it found every one
static bool find_window_commands(VkInstance instance, window_commands& commands) {
    commands = {
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

    return commands.destroy_instance && commands.enumerate_physical_devices && commands.get_queue_families &&
        commands.create_surface && commands.destroy_surface && commands.get_support && commands.get_capabilities &&
        commands.get_capabilities2 && commands.get_formats && commands.get_formats2 && commands.get_present_modes &&
        commands.get_present_rectangles;
}

// Checks that an instance whose devices were enumerated with MESA_VK_WSI_DEBUG=linear, which turns off the buffer blit
// stlxdm's buffers need, is refused a swapchain on a fresh window, though the variable is unset by then
static void check_linear_refused() {
    setenv("MESA_VK_WSI_DEBUG", "linear", 1);
    VkInstance instance = create_window_instance();
    window_commands commands = {};
    bool has_commands = instance != VK_NULL_HANDLE && find_window_commands(instance, commands);
    std::vector<VkPhysicalDevice> physical_devices;
    if (has_commands) {
        physical_devices = list_physical_devices(instance, commands);
    }

    unsetenv("MESA_VK_WSI_DEBUG");

    stlxwin_conn* conn = nullptr;
    if (!physical_devices.empty()) {
        start_hang_watch("stlxwin_connect");
        conn = stlxwin_connect("vktest");
        end_hang_watch();
    }

    window_surface target = {};
    if (conn != nullptr) {
        start_hang_watch("stlxwin_window_create");
        target.window = stlxwin_window_create(conn, WINDOW_WIDTH, WINDOW_HEIGHT, "vktest", 0);
        end_hang_watch();
    }

    if (target.window != nullptr) {
        target.presenting_device = find_presenting_device(instance, physical_devices, conn);
    }

    if (target.presenting_device != VK_NULL_HANDLE &&
        create_window_surface(instance, commands, conn, target) != VK_SUCCESS) {
        target.surface = VK_NULL_HANDLE;
    }

    present_stage stage = {};
    if (target.surface != VK_NULL_HANDLE && find_work_commands(instance, stage.work) &&
        find_present_commands(instance, stage.presents)) {
        stage.device = create_presenting_device(instance, target.presenting_device);
    }

    VkResult result = VK_ERROR_UNKNOWN;
    if (stage.device != VK_NULL_HANDLE) {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        result = create_swapchain(stage, target, VK_PRESENT_MODE_FIFO_KHR, 2, VK_NULL_HANDLE, swapchain);
        if (swapchain != VK_NULL_HANDLE) {
            destroy_swapchain(stage, swapchain);
        }

        stage.presents.destroy_device(stage.device, nullptr);
    }

    // Skips where check_surface skips, when no physical device can present
    bool is_skipped = (has_commands && physical_devices.empty()) ||
        (target.window != nullptr && target.presenting_device == VK_NULL_HANDLE);
    if (!is_skipped) {
        check(
            "an instance whose devices were enumerated with MESA_VK_WSI_DEBUG=linear is refused a swapchain as "
            "failing to initialize",
            result == VK_ERROR_INITIALIZATION_FAILED
        );
    }

    if (target.surface != VK_NULL_HANDLE) {
        commands.destroy_surface(instance, target.surface, nullptr);
    }

    if (conn != nullptr) {
        stlxwin_disconnect(conn);
    }

    if (instance != VK_NULL_HANDLE && commands.destroy_instance != nullptr) {
        commands.destroy_instance(instance, nullptr);
    }
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

    window_commands commands = {};
    bool has_commands = find_window_commands(instance, commands);
    check("vkGetInstanceProcAddr finds the window commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    check_surface(instance, commands);
    commands.destroy_instance(instance, nullptr);
    check_linear_refused();
}
