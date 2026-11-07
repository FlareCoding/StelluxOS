// Buffers on memory of their own, memory shared through descriptors, command pools, one-time work on a device's queue,
// timeline semaphores and the barrier after transfer writes, used by the stages that run work on the GPU

#include "vktest.h"

#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

bool find_work_commands(VkInstance instance, work_commands& commands) {
    commands = {
        .allocate_memory = find_command<PFN_vkAllocateMemory>(instance, "vkAllocateMemory"),
        .free_memory = find_command<PFN_vkFreeMemory>(instance, "vkFreeMemory"),
        .map_memory = find_command<PFN_vkMapMemory>(instance, "vkMapMemory"),
        .get_memory_fd = find_command<PFN_vkGetMemoryFdKHR>(instance, "vkGetMemoryFdKHR"),
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
        .create_semaphore = find_command<PFN_vkCreateSemaphore>(instance, "vkCreateSemaphore"),
        .destroy_semaphore = find_command<PFN_vkDestroySemaphore>(instance, "vkDestroySemaphore"),
        .signal_semaphore = find_command<PFN_vkSignalSemaphore>(instance, "vkSignalSemaphore"),
        .wait_semaphores = find_command<PFN_vkWaitSemaphores>(instance, "vkWaitSemaphores"),
        .get_counter_value = find_command<PFN_vkGetSemaphoreCounterValue>(instance, "vkGetSemaphoreCounterValue"),
    };

    return commands.allocate_memory && commands.free_memory && commands.map_memory && commands.get_memory_fd &&
        commands.create_buffer && commands.destroy_buffer && commands.get_buffer_memory_requirements &&
        commands.bind_buffer_memory && commands.get_device_queue && commands.create_command_pool &&
        commands.destroy_command_pool && commands.allocate_command_buffers && commands.begin_command_buffer &&
        commands.end_command_buffer && commands.cmd_pipeline_barrier && commands.queue_submit &&
        commands.queue_wait_idle && commands.create_semaphore && commands.destroy_semaphore &&
        commands.signal_semaphore && commands.wait_semaphores && commands.get_counter_value;
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

static bool is_host_visible(const VkMemoryType& type) {
    return (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}

bool has_visibility(const VkMemoryType& type, type_visibility visibility) {
    switch (visibility) {
    case type_visibility::any:
        return true;
    case type_visibility::host_visible:
        return is_host_visible(type);
    case type_visibility::device_only:
        return !is_host_visible(type);
    }

    return false;
}

uint32_t find_system_type(const VkPhysicalDeviceMemoryProperties& properties) {
    for (uint32_t type = 0; type < properties.memoryTypeCount; type++) {
        const VkMemoryType& memory_type = properties.memoryTypes[type];
        bool is_device_local = (memory_type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        if (is_host_visible(memory_type) && !is_device_local) {
            return type;
        }
    }

    return UINT32_MAX;
}

void write_words(void* mapping, VkDeviceSize size, uint32_t first_word) {
    uint32_t* words = static_cast<uint32_t*>(mapping);
    for (uint32_t i = 0; i < size / sizeof(uint32_t); i++) {
        words[i] = first_word + i;
    }
}

bool holds_words(const void* mapping, VkDeviceSize size, uint32_t first_word) {
    const uint32_t* words = static_cast<const uint32_t*>(mapping);
    for (uint32_t i = 0; i < size / sizeof(uint32_t); i++) {
        if (words[i] != first_word + i) {
            return false;
        }
    }

    return true;
}

VkResult allocate_memory(
    const work_commands& commands,
    VkDevice device,
    uint32_t type,
    VkDeviceSize size,
    const void* next,
    VkDeviceMemory& memory
) {
    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = next;
    allocate_info.allocationSize = size;
    allocate_info.memoryTypeIndex = type;

    VkResult result = commands.allocate_memory(device, &allocate_info, nullptr, &memory);
    if (result != VK_SUCCESS) {
        memory = VK_NULL_HANDLE;
    }

    return result;
}

int export_memory(
    const work_commands& commands,
    VkDevice device,
    uint32_t type,
    VkDeviceSize size,
    VkDeviceMemory& memory
) {
    VkExportMemoryAllocateInfo export_info = {};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (allocate_memory(commands, device, type, size, &export_info, memory) != VK_SUCCESS) {
        return -1;
    }

    VkMemoryGetFdInfoKHR fd_info = {};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory = memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    int descriptor = -1;
    if (commands.get_memory_fd(device, &fd_info, &descriptor) != VK_SUCCESS) {
        return -1;
    }

    return descriptor;
}

VkResult import_memory(
    const work_commands& commands,
    VkDevice device,
    uint32_t type,
    VkDeviceSize size,
    int descriptor,
    VkDeviceMemory& memory
) {
    VkImportMemoryFdInfoKHR import_info = {};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    import_info.fd = descriptor;

    return allocate_memory(commands, device, type, size, &import_info, memory);
}

int make_read_only_shared_memory(
    VkInstance instance,
    VkPhysicalDevice physical_device,
    VkDeviceSize size,
    uint32_t first_word
) {
    int shared = memfd_create("vktest", MFD_CLOEXEC);
    bool is_sized = shared >= 0 && ftruncate(shared, size) == 0;
    void* mapping = is_sized ? mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shared, 0) : MAP_FAILED;
    int buffer = -1;
    if (mapping != MAP_FAILED) {
        write_words(mapping, size, first_word);
        munmap(mapping, size);
        buffer = import_shared_memory(instance, physical_device, shared);
    }

    int read_only = buffer >= 0 ? restrict_to_read(instance, physical_device, buffer) : -1;
    if (buffer >= 0) {
        close(buffer);
    }

    if (shared >= 0) {
        close(shared);
    }

    return read_only;
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

VkBuffer bind_shared_buffer(
    const work_commands& commands,
    VkDevice device,
    VkDeviceMemory memory,
    uint32_t type,
    VkDeviceSize size,
    VkBufferUsageFlags usage
) {
    VkExternalMemoryBufferCreateInfo external_info = {};
    external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    return bind_new_buffer(commands, device, memory, type, size, usage, &external_info);
}

bound_buffer make_bound_buffer(
    const work_commands& commands,
    VkDevice device,
    uint32_t type,
    VkDeviceSize size,
    VkBufferUsageFlags usage
) {
    bound_buffer made;
    if (allocate_memory(commands, device, type, size, nullptr, made.memory) != VK_SUCCESS) {
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
        start_hang_watch("vkQueueWaitIdle");
        result = commands.queue_wait_idle(queue);
        end_hang_watch();
    }

    commands.destroy_command_pool(device, pool, nullptr);

    return result;
}

VkCommandPool make_command_pool(const work_commands& commands, VkDevice device, uint32_t family) {
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = family;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (commands.create_command_pool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    return pool;
}

VkSemaphore create_timeline(
    const work_commands& commands,
    VkDevice device,
    uint64_t initial_value,
    bool is_exportable
) {
    VkExportSemaphoreCreateInfo export_info = {};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkSemaphoreTypeCreateInfo type_info = {};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.pNext = is_exportable ? &export_info : nullptr;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue = initial_value;

    VkSemaphoreCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create_info.pNext = &type_info;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (commands.create_semaphore(device, &create_info, nullptr, &semaphore) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    return semaphore;
}

bool reads_value(const work_commands& commands, VkDevice device, VkSemaphore semaphore, uint64_t expected) {
    uint64_t value = 0;
    return commands.get_counter_value(device, semaphore, &value) == VK_SUCCESS && value == expected;
}

VkResult signal_timeline(const work_commands& commands, VkDevice device, VkSemaphore semaphore, uint64_t value) {
    VkSemaphoreSignalInfo signal_info = {};
    signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signal_info.semaphore = semaphore;
    signal_info.value = value;

    return commands.signal_semaphore(device, &signal_info);
}

VkResult wait_for_value(
    const work_commands& commands,
    VkDevice device,
    VkSemaphore semaphore,
    uint64_t value,
    uint64_t timeout_ns
) {
    VkSemaphoreWaitInfo wait_info = {};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &semaphore;
    wait_info.pValues = &value;

    return commands.wait_semaphores(device, &wait_info, timeout_ns);
}
