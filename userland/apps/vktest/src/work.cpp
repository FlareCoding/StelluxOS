// Buffers on memory of their own, one-time work on a device's queue and the barrier after transfer writes, shared by
// the stages that run work on the GPU

#include "vktest.h"

#include <stdint.h>

bool find_work_commands(VkInstance instance, work_commands& commands) {
    commands = {
        .allocate_memory = find_command<PFN_vkAllocateMemory>(instance, "vkAllocateMemory"),
        .free_memory = find_command<PFN_vkFreeMemory>(instance, "vkFreeMemory"),
        .map_memory = find_command<PFN_vkMapMemory>(instance, "vkMapMemory"),
        .create_buffer = find_command<PFN_vkCreateBuffer>(instance, "vkCreateBuffer"),
        .destroy_buffer = find_command<PFN_vkDestroyBuffer>(instance, "vkDestroyBuffer"),
        .get_buffer_memory_requirements = find_command<PFN_vkGetBufferMemoryRequirements>(
            instance,
            "vkGetBufferMemoryRequirements"
        ),
        .bind_buffer_memory = find_command<PFN_vkBindBufferMemory>(instance, "vkBindBufferMemory"),
        .get_device_queue = find_command<PFN_vkGetDeviceQueue>(instance, "vkGetDeviceQueue"),
        .create_command_pool = find_command<PFN_vkCreateCommandPool>(instance, "vkCreateCommandPool"),
        .destroy_command_pool = find_command<PFN_vkDestroyCommandPool>(instance, "vkDestroyCommandPool"),
        .allocate_command_buffers = find_command<PFN_vkAllocateCommandBuffers>(instance, "vkAllocateCommandBuffers"),
        .begin_command_buffer = find_command<PFN_vkBeginCommandBuffer>(instance, "vkBeginCommandBuffer"),
        .end_command_buffer = find_command<PFN_vkEndCommandBuffer>(instance, "vkEndCommandBuffer"),
        .cmd_pipeline_barrier = find_command<PFN_vkCmdPipelineBarrier>(instance, "vkCmdPipelineBarrier"),
        .queue_submit = find_command<PFN_vkQueueSubmit>(instance, "vkQueueSubmit"),
        .queue_wait_idle = find_command<PFN_vkQueueWaitIdle>(instance, "vkQueueWaitIdle"),
    };

    return commands.allocate_memory && commands.free_memory && commands.map_memory && commands.create_buffer &&
        commands.destroy_buffer && commands.get_buffer_memory_requirements && commands.bind_buffer_memory &&
        commands.get_device_queue && commands.create_command_pool && commands.destroy_command_pool &&
        commands.allocate_command_buffers && commands.begin_command_buffer && commands.end_command_buffer &&
        commands.cmd_pipeline_barrier && commands.queue_submit && commands.queue_wait_idle;
}

static uint32_t find_staging_type(const VkPhysicalDeviceMemoryProperties& properties) {
    VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t type = 0; type < properties.memoryTypeCount; type++) {
        if ((properties.memoryTypes[type].propertyFlags & coherent) == coherent) {
            return type;
        }
    }

    return UINT32_MAX;
}

VkBuffer bind_new_buffer(
    const work_commands& commands,
    VkDevice device,
    VkDeviceMemory memory,
    uint32_t type,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    const void* next
) {
    VkBufferCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create_info.pNext = next;
    create_info.size = size;
    create_info.usage = usage;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (commands.create_buffer(device, &create_info, nullptr, &buffer) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    VkMemoryRequirements requirements = {};
    commands.get_buffer_memory_requirements(device, buffer, &requirements);
    bool fits = requirements.size <= size && (requirements.memoryTypeBits & (1u << type)) != 0;
    if (!fits || commands.bind_buffer_memory(device, buffer, memory, 0) != VK_SUCCESS) {
        commands.destroy_buffer(device, buffer, nullptr);
        return VK_NULL_HANDLE;
    }

    return buffer;
}

bound_buffer make_bound_buffer(
    const work_commands& commands,
    VkDevice device,
    uint32_t type,
    VkDeviceSize size,
    VkBufferUsageFlags usage
) {
    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = size;
    allocate_info.memoryTypeIndex = type;

    bound_buffer made;
    if (commands.allocate_memory(device, &allocate_info, nullptr, &made.memory) != VK_SUCCESS) {
        made.memory = VK_NULL_HANDLE;
        return made;
    }

    made.buffer = bind_new_buffer(commands, device, made.memory, type, size, usage, nullptr);

    return made;
}

bound_buffer make_staging_buffer(
    const work_commands& commands,
    VkDevice device,
    const VkPhysicalDeviceMemoryProperties& properties,
    VkDeviceSize size,
    VkBufferUsageFlags usage
) {
    uint32_t type = find_staging_type(properties);
    if (type == UINT32_MAX) {
        return {};
    }

    bound_buffer made = make_bound_buffer(commands, device, type, size, usage);

    void* mapping = nullptr;
    bool is_mapped = made.buffer != VK_NULL_HANDLE &&
        commands.map_memory(device, made.memory, 0, VK_WHOLE_SIZE, 0, &mapping) == VK_SUCCESS;
    made.words = is_mapped ? static_cast<uint32_t*>(mapping) : nullptr;

    return made;
}

void destroy_bound_buffer(const work_commands& commands, VkDevice device, const bound_buffer& made) {
    commands.destroy_buffer(device, made.buffer, nullptr);
    commands.free_memory(device, made.memory, nullptr);
}

void wait_for_transfer_writes(
    const work_commands& commands,
    VkCommandBuffer command_buffer,
    VkPipelineStageFlags stage_mask,
    VkAccessFlags access
) {
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = access;
    commands.cmd_pipeline_barrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        stage_mask,
        0,
        1,
        &barrier,
        0,
        nullptr,
        0,
        nullptr
    );
}

VkResult run_once(
    const work_commands& commands,
    VkDevice device,
    uint32_t family,
    const std::function<void(VkCommandBuffer)>& record
) {
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = family;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkResult result = commands.create_command_pool(device, &pool_info, nullptr, &pool);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkCommandBufferAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    result = commands.allocate_command_buffers(device, &allocate_info, &command_buffer);
    if (result == VK_SUCCESS) {
        result = commands.begin_command_buffer(command_buffer, &begin_info);
    }

    if (result == VK_SUCCESS) {
        record(command_buffer);
        result = commands.end_command_buffer(command_buffer);
    }

    VkQueue queue = VK_NULL_HANDLE;
    commands.get_device_queue(device, family, 0, &queue);

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    if (result == VK_SUCCESS) {
        result = commands.queue_submit(queue, 1, &submit_info, VK_NULL_HANDLE);
    }

    if (result == VK_SUCCESS) {
        result = commands.queue_wait_idle(queue);
    }

    commands.destroy_command_pool(device, pool, nullptr);

    return result;
}
