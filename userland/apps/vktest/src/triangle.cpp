// The triangle stage: a graphics pipeline NAK compiles from triangle.spvasm draws one triangle on the general queue
// into an image, whose copy in memory of the staging type the CPU compares pixel by pixel and shows in a stlxwin window

#include "triangle_spirv.h"
#include "vktest.h"

#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include <stlxwin/stlxwin.h>
#include <string.h>
#include <time.h>

struct triangle_commands {
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    PFN_vkDestroyDevice                     destroy_device;
    PFN_vkCreateImage                       create_image;
    PFN_vkDestroyImage                      destroy_image;
    PFN_vkGetImageMemoryRequirements        get_image_memory_requirements;
    PFN_vkBindImageMemory                   bind_image_memory;
    PFN_vkCreateImageView                   create_image_view;
    PFN_vkDestroyImageView                  destroy_image_view;
    PFN_vkCreateShaderModule                create_shader_module;
    PFN_vkDestroyShaderModule               destroy_shader_module;
    PFN_vkCreatePipelineLayout              create_pipeline_layout;
    PFN_vkDestroyPipelineLayout             destroy_pipeline_layout;
    PFN_vkCreateGraphicsPipelines           create_graphics_pipelines;
    PFN_vkDestroyPipeline                   destroy_pipeline;

    PFN_vkCmdBeginRendering    cmd_begin_rendering;
    PFN_vkCmdEndRendering      cmd_end_rendering;
    PFN_vkCmdBindPipeline      cmd_bind_pipeline;
    PFN_vkCmdDraw              cmd_draw;
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer;
};

// A device with a queue of the general family alone and dynamic rendering, whose drawing the CPU reads through memory
// of the staging type
struct triangle_stage {
    triangle_commands                commands;
    work_commands                    work;
    VkPhysicalDeviceMemoryProperties properties;
    VkDevice                         device;
};

// The pipeline with the layout it is created with, each handle null when its creation failed or never ran
struct triangle_pipeline {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline       pipeline = VK_NULL_HANDLE;
};

// The image the triangle is drawn into, on device-local memory, with the view the drawing writes through
struct target_image {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
};

// A barrier changing the image from one layout to another
struct layout_change {
    VkImageLayout        from;
    VkImageLayout        to;
    VkPipelineStageFlags src_stage;
    VkAccessFlags        src_access;
    VkPipelineStageFlags dst_stage;
    VkAccessFlags        dst_access;
};

// A point in quarter pixels from the image's top-left
struct quarter_point {
    int64_t x;
    int64_t y;
};

// Must match the image size triangle.spvasm places its corners for
constexpr uint32_t IMAGE_SIZE = 256;
constexpr uint32_t PIXEL_COUNT = IMAGE_SIZE * IMAGE_SIZE;
constexpr VkDeviceSize IMAGE_BYTES = PIXEL_COUNT * sizeof(uint32_t);
constexpr VkFormat IMAGE_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;

constexpr VkImageSubresourceRange COLOR_RANGE = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceLayers COLOR_LAYERS = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};

// A B8G8R8A8_UNORM pixel is a stlxwin buffer's XRGB word, its alpha in the top byte XRGB leaves unused
constexpr VkClearColorValue CLEAR_COLOR = {{0.0f, 0.0f, 0.0f, 1.0f}};
constexpr uint32_t CLEAR_PIXEL = 0xff000000;
constexpr uint32_t TRIANGLE_PIXEL = 0xffff0000;
constexpr uint32_t CPU_PIXEL = 0x00808080;

// triangle.spvasm's corners, placed where no pixel center lies on an edge, so the expected image needs no rule for one
constexpr quarter_point CORNERS[] = {{513, 133}, {897, 897}, {133, 897}};

constexpr layout_change INTO_ATTACHMENT = {
    .from = VK_IMAGE_LAYOUT_UNDEFINED,
    .to = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    .src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
    .src_access = 0,
    .dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    .dst_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
};

constexpr layout_change INTO_COPY_SOURCE = {
    .from = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    .to = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    .src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    .src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    .dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT,
    .dst_access = VK_ACCESS_TRANSFER_READ_BIT,
};

constexpr uint64_t FRAME_WAIT_NS = 2000000000;

static triangle_pipeline create_pipeline(const triangle_stage& stage) {
    const triangle_commands& commands = stage.commands;
    triangle_pipeline created;

    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (commands.create_pipeline_layout(stage.device, &layout_info, nullptr, &created.layout) != VK_SUCCESS) {
        created.layout = VK_NULL_HANDLE;
        return created;
    }

    VkShaderModuleCreateInfo module_info = {};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = sizeof(TRIANGLE_SPIRV);
    module_info.pCode = TRIANGLE_SPIRV;

    VkShaderModule module = VK_NULL_HANDLE;
    if (commands.create_shader_module(stage.device, &module_info, nullptr, &module) != VK_SUCCESS) {
        return created;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = module;
    stages[0].pName = "vertex";

    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].pName = "fragment";

    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {0.0f, 0.0f, static_cast<float>(IMAGE_SIZE), static_cast<float>(IMAGE_SIZE), 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {IMAGE_SIZE, IMAGE_SIZE}};

    VkPipelineViewportStateCreateInfo viewport_state = {};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterization = {};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment = {};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    VkPipelineRenderingCreateInfo rendering = {};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &IMAGE_FORMAT;

    VkGraphicsPipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering;
    pipeline_info.layout = created.layout;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;

    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;

    commands.create_graphics_pipelines(stage.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &created.pipeline);
    commands.destroy_shader_module(stage.device, module, nullptr);

    return created;
}

static void destroy_pipeline(const triangle_stage& stage, const triangle_pipeline& created) {
    stage.commands.destroy_pipeline(stage.device, created.pipeline, nullptr);
    stage.commands.destroy_pipeline_layout(stage.device, created.layout, nullptr);
}

// The first device-local type of those that `allowed` sets a bit for, or `UINT32_MAX` for none
static uint32_t find_device_local_type(const VkPhysicalDeviceMemoryProperties& properties, uint32_t allowed) {
    for (uint32_t type = 0; type < properties.memoryTypeCount; type++) {
        bool is_allowed = (allowed & (1u << type)) != 0;
        bool is_device_local = (properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        if (is_allowed && is_device_local) {
            return type;
        }
    }

    return UINT32_MAX;
}

// Makes the image the triangle is drawn into, the view left null on a failure
static target_image make_target_image(const triangle_stage& stage) {
    const triangle_commands& commands = stage.commands;
    target_image made;

    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = IMAGE_FORMAT;
    image_info.extent = {IMAGE_SIZE, IMAGE_SIZE, 1};

    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (commands.create_image(stage.device, &image_info, nullptr, &made.image) != VK_SUCCESS) {
        made.image = VK_NULL_HANDLE;
        return made;
    }

    VkMemoryRequirements requirements = {};
    commands.get_image_memory_requirements(stage.device, made.image, &requirements);

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = find_device_local_type(stage.properties, requirements.memoryTypeBits);

    bool has_type = allocate_info.memoryTypeIndex != UINT32_MAX;
    if (!has_type || stage.work.allocate_memory(stage.device, &allocate_info, nullptr, &made.memory) != VK_SUCCESS) {
        made.memory = VK_NULL_HANDLE;
        return made;
    }

    if (commands.bind_image_memory(stage.device, made.image, made.memory, 0) != VK_SUCCESS) {
        return made;
    }

    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = made.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = IMAGE_FORMAT;
    view_info.subresourceRange = COLOR_RANGE;

    if (commands.create_image_view(stage.device, &view_info, nullptr, &made.view) != VK_SUCCESS) {
        made.view = VK_NULL_HANDLE;
    }

    return made;
}

static void destroy_target_image(const triangle_stage& stage, const target_image& made) {
    stage.commands.destroy_image_view(stage.device, made.view, nullptr);
    stage.commands.destroy_image(stage.device, made.image, nullptr);
    stage.work.free_memory(stage.device, made.memory, nullptr);
}

static void record_layout_change(
    const triangle_stage& stage,
    VkCommandBuffer command_buffer,
    VkImage image,
    const layout_change& change
) {
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.subresourceRange = COLOR_RANGE;
    barrier.oldLayout = change.from;
    barrier.newLayout = change.to;

    barrier.srcAccessMask = change.src_access;
    barrier.dstAccessMask = change.dst_access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    stage.work.cmd_pipeline_barrier(
        command_buffer,
        change.src_stage,
        change.dst_stage,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );
}

static void record_drawing(
    const triangle_stage& stage,
    const triangle_pipeline& pipeline,
    const target_image& target,
    VkBuffer readback,
    VkCommandBuffer command_buffer
) {
    const triangle_commands& commands = stage.commands;

    VkRenderingAttachmentInfo attachment = {};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = target.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = CLEAR_COLOR;

    VkRenderingInfo rendering = {};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = {IMAGE_SIZE, IMAGE_SIZE};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;

    record_layout_change(stage, command_buffer, target.image, INTO_ATTACHMENT);
    commands.cmd_begin_rendering(command_buffer, &rendering);
    commands.cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
    commands.cmd_draw(command_buffer, 3, 1, 0, 0);
    commands.cmd_end_rendering(command_buffer);

    VkBufferImageCopy region = {};
    region.imageSubresource = COLOR_LAYERS;
    region.imageExtent = {IMAGE_SIZE, IMAGE_SIZE, 1};

    record_layout_change(stage, command_buffer, target.image, INTO_COPY_SOURCE);
    commands.cmd_copy_image_to_buffer(
        command_buffer,
        target.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback,
        1,
        &region
    );
    wait_for_transfer_writes(stage.work, command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
}

// Twice the signed area of `from`, `to` and (`x`, `y`). Its sign tells the point's side of the edge `from` to `to`.
static int64_t edge_side(const quarter_point& from, const quarter_point& to, int64_t x, int64_t y) {
    return (to.x - from.x) * (y - from.y) - (to.y - from.y) * (x - from.x);
}

// The triangle's color for a pixel whose center lies on the same side of all three edges, the clear color for others
static uint32_t expected_pixel(uint32_t index) {
    int64_t x = 4 * static_cast<int64_t>(index % IMAGE_SIZE) + 2;
    int64_t y = 4 * static_cast<int64_t>(index / IMAGE_SIZE) + 2;

    bool first_side = edge_side(CORNERS[0], CORNERS[1], x, y) > 0;
    bool second_side = edge_side(CORNERS[1], CORNERS[2], x, y) > 0;
    bool third_side = edge_side(CORNERS[2], CORNERS[0], x, y) > 0;
    bool is_covered = first_side == second_side && second_side == third_side;

    return is_covered ? TRIANGLE_PIXEL : CLEAR_PIXEL;
}

// Whether every pixel holds its expected color, printing how many do not and the first of them
static bool holds_triangle(const uint32_t* pixels) {
    uint32_t wrong_pixels = 0;
    uint32_t first_wrong = 0;
    for (uint32_t i = 0; i < PIXEL_COUNT; i++) {
        if (pixels[i] == expected_pixel(i)) {
            continue;
        }

        if (wrong_pixels == 0) {
            first_wrong = i;
        }

        wrong_pixels++;
    }

    if (wrong_pixels > 0) {
        printf(
            "  triangle pixels: %u of %u differ, the first at (%u, %u) holding 0x%08x, not 0x%08x\n",
            wrong_pixels,
            PIXEL_COUNT,
            first_wrong % IMAGE_SIZE,
            first_wrong / IMAGE_SIZE,
            pixels[first_wrong],
            expected_pixel(first_wrong)
        );
    }

    return wrong_pixels == 0;
}

static uint64_t monotonic_ns() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec);
}

// Whether `window` gets the FRAME event of its commit within `FRAME_WAIT_NS` and before the connection is dead
static bool frame_arrives(stlxwin_conn* conn, stlxwin_window* window) {
    uint64_t deadline = monotonic_ns() + FRAME_WAIT_NS;

    while (true) {
        stlxwin_event event;
        while (stlxwin_next_event(conn, &event)) {
            if (event.type == STLXWIN_EVT_FRAME && event.window == window) {
                return true;
            }
        }

        uint64_t now = monotonic_ns();
        if (now >= deadline || stlxwin_wait(conn, static_cast<int64_t>(deadline - now)) < 0) {
            return false;
        }
    }
}

// Whether a frame of `pixels`, committed to a window of the image's size through a shared memory buffer, is presented
static bool shows_in_window(const uint32_t* pixels) {
    stlxwin_conn* conn = stlxwin_connect("vktest");
    if (conn == nullptr) {
        return false;
    }

    stlxwin_window* window = stlxwin_window_create(conn, IMAGE_SIZE, IMAGE_SIZE, "vktest", 0);
    stlxwin_buffer* buffer = window != nullptr ? stlxwin_begin_frame(window) : nullptr;

    bool is_committed = false;
    if (buffer != nullptr) {
        uint32_t width = std::min(buffer->width, IMAGE_SIZE);
        uint32_t height = std::min(buffer->height, IMAGE_SIZE);
        uint8_t* buffer_rows = reinterpret_cast<uint8_t*>(buffer->pixels);
        for (uint32_t row = 0; row < height; row++) {
            uint8_t* buffer_row = buffer_rows + static_cast<size_t>(row) * buffer->stride;
            memcpy(buffer_row, pixels + row * IMAGE_SIZE, width * sizeof(uint32_t));
        }

        is_committed = stlxwin_commit(window, buffer, nullptr, 0, STLXWIN_COMMIT_WANT_FRAME) == 0;
    }

    bool is_presented = is_committed && frame_arrives(conn, window);
    stlxwin_disconnect(conn);

    return is_presented;
}

static void check_drawing(const triangle_stage& stage, const triangle_pipeline& pipeline) {
    target_image target = make_target_image(stage);
    bound_buffer readback =
        make_staging_buffer(stage.work, stage.device, stage.properties, IMAGE_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    bool is_run = false;
    if (target.view != VK_NULL_HANDLE && readback.words != nullptr) {
        std::fill_n(readback.words, PIXEL_COUNT, CPU_PIXEL);

        auto record = [&](VkCommandBuffer command_buffer) {
            record_drawing(stage, pipeline, target, readback.buffer, command_buffer);
        };

        is_run = run_once(stage.work, stage.device, GENERAL_FAMILY, record) == VK_SUCCESS;
    }

    check("a submission drawing into a 256x256 image and copying it to a host-visible buffer completes", is_run);
    check(
        "the copy holds the triangle's color at every pixel whose center it covers and the clear color elsewhere",
        is_run && holds_triangle(readback.words)
    );
    check(
        "a stlxwin window is created and its frame, committed through a shared memory buffer, is presented",
        is_run && shows_in_window(readback.words)
    );

    destroy_bound_buffer(stage.work, stage.device, readback);
    destroy_target_image(stage, target);
}

void check_triangle(VkInstance instance, VkPhysicalDevice physical_device) {
    triangle_stage stage = {};
    stage.commands = {
        .get_memory_properties = find_command<PFN_vkGetPhysicalDeviceMemoryProperties>(
            instance,
            "vkGetPhysicalDeviceMemoryProperties"
        ),
        .destroy_device = find_command<PFN_vkDestroyDevice>(instance, "vkDestroyDevice"),
        .create_image = find_command<PFN_vkCreateImage>(instance, "vkCreateImage"),
        .destroy_image = find_command<PFN_vkDestroyImage>(instance, "vkDestroyImage"),
        .get_image_memory_requirements = find_command<PFN_vkGetImageMemoryRequirements>(
            instance,
            "vkGetImageMemoryRequirements"
        ),
        .bind_image_memory = find_command<PFN_vkBindImageMemory>(instance, "vkBindImageMemory"),
        .create_image_view = find_command<PFN_vkCreateImageView>(instance, "vkCreateImageView"),
        .destroy_image_view = find_command<PFN_vkDestroyImageView>(instance, "vkDestroyImageView"),
        .create_shader_module = find_command<PFN_vkCreateShaderModule>(instance, "vkCreateShaderModule"),
        .destroy_shader_module = find_command<PFN_vkDestroyShaderModule>(instance, "vkDestroyShaderModule"),
        .create_pipeline_layout = find_command<PFN_vkCreatePipelineLayout>(instance, "vkCreatePipelineLayout"),
        .destroy_pipeline_layout = find_command<PFN_vkDestroyPipelineLayout>(instance, "vkDestroyPipelineLayout"),
        .create_graphics_pipelines = find_command<PFN_vkCreateGraphicsPipelines>(instance, "vkCreateGraphicsPipelines"),
        .destroy_pipeline = find_command<PFN_vkDestroyPipeline>(instance, "vkDestroyPipeline"),
        .cmd_begin_rendering = find_command<PFN_vkCmdBeginRendering>(instance, "vkCmdBeginRendering"),
        .cmd_end_rendering = find_command<PFN_vkCmdEndRendering>(instance, "vkCmdEndRendering"),
        .cmd_bind_pipeline = find_command<PFN_vkCmdBindPipeline>(instance, "vkCmdBindPipeline"),
        .cmd_draw = find_command<PFN_vkCmdDraw>(instance, "vkCmdDraw"),
        .cmd_copy_image_to_buffer = find_command<PFN_vkCmdCopyImageToBuffer>(instance, "vkCmdCopyImageToBuffer"),
    };

    const triangle_commands& commands = stage.commands;
    bool has_work_commands = find_work_commands(instance, stage.work);
    bool has_commands = commands.get_memory_properties && commands.destroy_device && commands.create_image &&
        commands.destroy_image && commands.get_image_memory_requirements && commands.bind_image_memory &&
        commands.create_image_view && commands.destroy_image_view && commands.create_shader_module &&
        commands.destroy_shader_module && commands.create_pipeline_layout && commands.destroy_pipeline_layout &&
        commands.create_graphics_pipelines && commands.destroy_pipeline && commands.cmd_begin_rendering &&
        commands.cmd_end_rendering && commands.cmd_bind_pipeline && commands.cmd_draw &&
        commands.cmd_copy_image_to_buffer && has_work_commands;
    check("vkGetInstanceProcAddr finds the triangle commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    commands.get_memory_properties(physical_device, &stage.properties);

    VkPhysicalDeviceVulkan13Features vulkan_1_3 = {};
    vulkan_1_3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan_1_3.dynamicRendering = VK_TRUE;
    stage.device = create_device(instance, physical_device, {GENERAL_FAMILY}, {}, &vulkan_1_3);

    bool has_device = stage.device != VK_NULL_HANDLE;
    check("a device with a queue of the general family alone and dynamic rendering is created", has_device);
    if (!has_device) {
        return;
    }

    triangle_pipeline pipeline = create_pipeline(stage);
    bool has_pipeline = pipeline.pipeline != VK_NULL_HANDLE;
    check("a graphics pipeline is created from triangle.spvasm, NAK compiling its two shaders", has_pipeline);
    if (has_pipeline) {
        check_drawing(stage, pipeline);
    }

    destroy_pipeline(stage, pipeline);
    commands.destroy_device(stage.device, nullptr);
}
