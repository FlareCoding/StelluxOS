// The fault stage: a copy from freed memory faults a held burst while another device holds one, and a second copy
// faults on a device with no overflow list while two others wait on the copy's point, each wait on a lost device timed

#include "vktest.h"

#include <functional>
#include <stdint.h>
#include <stlx/gpu_abi.h>
#include <unistd.h>
#include <vector>

struct fault_commands {
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    PFN_vkGetSemaphoreFdKHR                 get_semaphore_fd;
    PFN_vkImportSemaphoreFdKHR              import_semaphore_fd;

    PFN_vkCmdCopyBuffer     cmd_copy_buffer;
    PFN_vkCmdFillBuffer     cmd_fill_buffer;
    PFN_vkCmdWriteTimestamp cmd_write_timestamp;

    PFN_vkCreateQueryPool     create_query_pool;
    PFN_vkDestroyQueryPool    destroy_query_pool;
    PFN_vkResetQueryPool      reset_query_pool;
    PFN_vkGetQueryPoolResults get_query_pool_results;
};

struct fault_stage {
    burst_commands                   bursts;
    fault_commands                   commands;
    VkInstance                       instance;
    VkPhysicalDevice                 physical_device;
    VkPhysicalDeviceMemoryProperties properties;
};

// A copy of one word between staging buffers. vktest frees the source after submitting the copy, so the copy faults.
struct planted_fault {
    VkCommandPool   pool = VK_NULL_HANDLE;
    VkCommandBuffer copy = VK_NULL_HANDLE;
    bound_buffer    source;
    bound_buffer    destination;
};

// What a fault in one device's held burst showed while another device held a burst too, each false until shown
struct fault_result {
    bool holds_bursts;
    bool zeroes_while_held;
    bool point_ends_lost;
    bool destroys_in_time;
    bool other_burst_completes;
};

// What a faulting copy on a device holding no overflow list showed there and on two devices waiting on the copy's
// point, each false until shown
struct waiter_result {
    bool batch_ends_lost;
    bool query_ends_lost;
    bool idle_ends_lost;
    bool marks_every_signal;
};

// A wait sees a loss when its slice ends, so the slices after the first leave time for the kernel to see the fault
constexpr uint64_t FAULT_WAIT_LIMIT_NS = 10 * WAIT_SLICE_NS;

// NVK's wait for a query result declares a loss itself only after 2 s, so a `GPU_CONTEXT_QUERY` poll must end it first
constexpr uint64_t QUERY_WAIT_LIMIT_NS = 100000000;

// More signals than one submission names, so the batch goes to the kernel as a chain of parts
constexpr uint32_t MARKED_COUNT = GPU_POINTS_MAX + 8;

// A value no other timeline of vktest reads, by which vktest finds the batch's semaphores
constexpr uint64_t MARKED_VALUE = 1ull << 47;

constexpr uint32_t FILLED_WORD = 0x5a5a5a5a;

// Records work through `record` into a one-time command buffer from `pool`, or returns `VK_NULL_HANDLE`
static VkCommandBuffer record_work(
    const fault_stage& stage,
    VkDevice device,
    VkCommandPool pool,
    const std::function<void(VkCommandBuffer)>& record
) {
    VkCommandBufferAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    const work_commands& work = stage.bursts.work;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    bool is_begun = work.allocate_command_buffers(device, &allocate_info, &command_buffer) == VK_SUCCESS &&
        work.begin_command_buffer(command_buffer, &begin_info) == VK_SUCCESS;
    if (!is_begun) {
        return VK_NULL_HANDLE;
    }

    record(command_buffer);

    return work.end_command_buffer(command_buffer) == VK_SUCCESS ? command_buffer : VK_NULL_HANDLE;
}

// Makes the copy's command pool and staging buffers on `device` and records the copy, left null when a call fails
static planted_fault plant_fault(const fault_stage& stage, VkDevice device) {
    const work_commands& work = stage.bursts.work;

    planted_fault fault;
    fault.pool = make_command_pool(work, device, stage.bursts.transfer_family);
    fault.source = make_staging_buffer(work, device, stage.properties, PAGE_BUFFER_SIZE, TRANSFER_USAGE);
    fault.destination = make_staging_buffer(work, device, stage.properties, PAGE_BUFFER_SIZE, TRANSFER_USAGE);

    bool has_buffers = fault.source.buffer != VK_NULL_HANDLE && fault.destination.buffer != VK_NULL_HANDLE;
    if (fault.pool == VK_NULL_HANDLE || !has_buffers) {
        return fault;
    }

    VkBufferCopy region = {};
    region.size = sizeof(uint32_t);
    fault.copy = record_work(stage, device, fault.pool, [&](VkCommandBuffer command_buffer) {
        stage.commands.cmd_copy_buffer(command_buffer, fault.source.buffer, fault.destination.buffer, 1, &region);
    });

    return fault;
}

// Whether `plant_fault` recorded the copy
static bool is_planted(const planted_fault& fault) {
    return fault.copy != VK_NULL_HANDLE;
}

// Frees the memory the copy reads, whose addresses NVK unmaps at once, so the copy faults when it runs
static void free_fault_source(const fault_stage& stage, VkDevice device, planted_fault& fault) {
    destroy_bound_buffer(stage.bursts.work, device, fault.source);
    fault.source = {};
}

static void destroy_fault(const fault_stage& stage, VkDevice device, const planted_fault& fault) {
    const work_commands& work = stage.bursts.work;
    work.destroy_command_pool(device, fault.pool, nullptr);
    destroy_bound_buffer(work, device, fault.destination);
    destroy_bound_buffer(work, device, fault.source);
}

// The first type of `properties` in video memory the CPU cannot map, or `UINT32_MAX`
static uint32_t find_unmappable_type(const VkPhysicalDeviceMemoryProperties& properties) {
    for (uint32_t type = 0; type < properties.memoryTypeCount; type++) {
        VkMemoryPropertyFlags flags = properties.memoryTypes[type].propertyFlags;
        bool is_video = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        bool is_mappable = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        if (is_video && !is_mappable) {
            return type;
        }
    }

    return UINT32_MAX;
}

// Allocates a zeroed page the CPU cannot map, which NVK zeroes on its upload queue and waits for before returning, or
// returns `VK_NULL_HANDLE`
static VkDeviceMemory allocate_zeroed(const fault_stage& stage, VkDevice device) {
    uint32_t type = find_unmappable_type(stage.properties);
    if (type == UINT32_MAX) {
        return VK_NULL_HANDLE;
    }

    VkMemoryAllocateFlagsInfo flags_info = {};
    flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags_info.flags = VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT;

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &flags_info;
    allocate_info.allocationSize = PAGE_BUFFER_SIZE;
    allocate_info.memoryTypeIndex = type;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    start_hang_watch("vkAllocateMemory");
    VkResult result = stage.bursts.work.allocate_memory(device, &allocate_info, nullptr, &memory);
    end_hang_watch();
    if (result != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    return memory;
}

// Holds bursts on `faulting`, whose first submission runs a copy, and on `running`, makes zeroed memory and holds a
// fill behind `running`'s burst, then faults the copy by freeing its source before signalling `faulting`'s gate
static fault_result run_fault_in_burst(const fault_stage& stage, VkDevice faulting, VkDevice running) {
    fault_result shown = {};
    const burst_commands& bursts = stage.bursts;

    planted_fault fault = plant_fault(stage, faulting);
    held_burst faulting_burst = make_burst(bursts, faulting);
    faulting_burst.first_work = fault.copy;
    held_burst running_burst = make_burst(bursts, running);

    VkCommandPool fill_pool = make_command_pool(bursts.work, running, bursts.transfer_family);
    bound_buffer filled = make_staging_buffer(bursts.work, running, stage.properties, PAGE_BUFFER_SIZE, TRANSFER_USAGE);
    VkCommandBuffer fill = VK_NULL_HANDLE;
    if (fill_pool != VK_NULL_HANDLE && filled.words != nullptr) {
        fill = record_work(stage, running, fill_pool, [&](VkCommandBuffer command_buffer) {
            stage.commands.cmd_fill_buffer(command_buffer, filled.buffer, 0, sizeof(uint32_t), FILLED_WORD);
            wait_for_transfer_writes(bursts.work, command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        });
    }

    bool is_ready = is_planted(fault) && is_made(faulting_burst) && is_made(running_burst) && fill != VK_NULL_HANDLE;
    VkResult fill_result = VK_ERROR_INITIALIZATION_FAILED;
    if (is_ready) {
        bool holds_faulting_burst = holds_burst(bursts, faulting_burst);
        bool holds_running_burst = holds_burst(bursts, running_burst);
        shown.holds_bursts = holds_faulting_burst && holds_running_burst;

        // NVK's upload queue has a kernel queue of its own, which the held burst leaves empty
        VkDeviceMemory zeroed = allocate_zeroed(stage, running);
        shown.zeroes_while_held = zeroed != VK_NULL_HANDLE;
        bursts.work.free_memory(running, zeroed, nullptr);

        // NVK has every submission with work wait on the upload queue's last point, which is the zeroing's here
        fill_result = submit_batch(
            bursts,
            running_burst.queue,
            fill,
            VK_NULL_HANDLE,
            {running_burst.done},
            {BURST_SIZE + 1}
        );

        free_fault_source(stage, faulting, fault);
        bool is_signalled = signal_timeline(bursts.work, faulting, faulting_burst.gate, 1) == VK_SUCCESS;
        shown.point_ends_lost =
            is_signalled && wait_ends_lost(bursts, faulting, faulting_burst.done, BURST_SIZE, FAULT_WAIT_LIMIT_NS);
    }

    destroy_burst(bursts, faulting_burst);
    destroy_fault(stage, faulting, fault);
    shown.destroys_in_time = destroys_in_time(bursts, faulting);

    // The queue's idle wait in the drain covers the fill behind the burst
    bool drains = is_ready && drains_burst(bursts, running_burst);
    shown.other_burst_completes = drains && fill_result == VK_SUCCESS && filled.words[0] == FILLED_WORD;

    bursts.work.destroy_command_pool(running, fill_pool, nullptr);
    destroy_bound_buffer(bursts.work, running, filled);
    destroy_burst(bursts, running_burst);

    return shown;
}

// Creates a semaphore on `importer` holding the timeline of `exporter`'s `semaphore`, passed as an opaque descriptor,
// or returns `VK_NULL_HANDLE`
static VkSemaphore share_timeline(
    const fault_stage& stage,
    VkDevice exporter,
    VkSemaphore semaphore,
    VkDevice importer
) {
    VkSemaphoreGetFdInfoKHR get_info = {};
    get_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    get_info.semaphore = semaphore;
    get_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    int descriptor = -1;
    bool is_exported = semaphore != VK_NULL_HANDLE &&
        stage.commands.get_semaphore_fd(exporter, &get_info, &descriptor) == VK_SUCCESS;
    if (!is_exported) {
        return VK_NULL_HANDLE;
    }

    VkSemaphore imported = create_timeline(stage.bursts.work, importer, 0, false);

    VkImportSemaphoreFdInfoKHR import_info = {};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    import_info.semaphore = imported;
    import_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    import_info.fd = descriptor;

    // The import owns the descriptor once it succeeds
    bool is_imported = imported != VK_NULL_HANDLE &&
        stage.commands.import_semaphore_fd(importer, &import_info) == VK_SUCCESS;
    if (!is_imported) {
        close(descriptor);
        stage.bursts.work.destroy_semaphore(importer, imported, nullptr);
        return VK_NULL_HANDLE;
    }

    return imported;
}

// Creates a pool of one timestamp query, reset from the CPU, or returns `VK_NULL_HANDLE`
static VkQueryPool create_timestamp_pool(const fault_stage& stage, VkDevice device) {
    VkQueryPoolCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    create_info.queryCount = 1;

    VkQueryPool pool = VK_NULL_HANDLE;
    if (stage.commands.create_query_pool(device, &create_info, nullptr, &pool) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    stage.commands.reset_query_pool(device, pool, 0, 1);

    return pool;
}

// Whether a wait for the result of `pool`'s query ends with `VK_ERROR_DEVICE_LOST` within `QUERY_WAIT_LIMIT_NS`
static bool query_ends_lost(const fault_stage& stage, VkDevice device, VkQueryPool pool) {
    uint64_t timestamp = 0;

    uint64_t start_ns = monotonic_ns();
    VkResult result = stage.commands.get_query_pool_results(
        device,
        pool,
        0,
        1,
        sizeof(timestamp),
        &timestamp,
        sizeof(timestamp),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT
    );
    uint64_t took_ns = monotonic_ns() - start_ns;

    return result == VK_ERROR_DEVICE_LOST && took_ns < QUERY_WAIT_LIMIT_NS;
}

// Faults a copy on `queried`, which holds no overflow list, ahead of a query write on its queue, while a batch of
// `MARKED_COUNT` signals on `marked` and a lone submission on `idled` wait on the copy's point
static waiter_result run_fault_with_waiters(
    const fault_stage& stage,
    VkDevice queried,
    VkDevice marked,
    VkDevice idled
) {
    waiter_result shown = {};
    const burst_commands& bursts = stage.bursts;
    const work_commands& work = bursts.work;

    VkQueue queried_queue = VK_NULL_HANDLE;
    VkQueue marked_queue = VK_NULL_HANDLE;
    VkQueue idled_queue = VK_NULL_HANDLE;
    work.get_device_queue(queried, bursts.transfer_family, 0, &queried_queue);
    work.get_device_queue(marked, bursts.transfer_family, 0, &marked_queue);
    work.get_device_queue(idled, bursts.transfer_family, 0, &idled_queue);

    planted_fault fault = plant_fault(stage, queried);
    VkQueryPool query_pool = create_timestamp_pool(stage, queried);
    VkCommandBuffer timestamp = VK_NULL_HANDLE;
    if (is_planted(fault) && query_pool != VK_NULL_HANDLE) {
        timestamp = record_work(stage, queried, fault.pool, [&](VkCommandBuffer command_buffer) {
            stage.commands.cmd_write_timestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool, 0);
        });
    }

    VkSemaphore gate = create_timeline(work, queried, 0, false);
    VkSemaphore copied = create_timeline(work, queried, 0, true);
    VkSemaphore marked_wait = share_timeline(stage, queried, copied, marked);
    VkSemaphore idled_wait = share_timeline(stage, queried, copied, idled);

    std::vector<VkSemaphore> signals(MARKED_COUNT, VK_NULL_HANDLE);
    std::vector<uint64_t> values(MARKED_COUNT, MARKED_VALUE);
    bool has_signals = true;
    for (VkSemaphore& signal : signals) {
        signal = create_timeline(work, marked, 0, false);
        has_signals &= signal != VK_NULL_HANDLE;
    }

    bool has_waits = gate != VK_NULL_HANDLE && marked_wait != VK_NULL_HANDLE && idled_wait != VK_NULL_HANDLE;
    bool is_ready = timestamp != VK_NULL_HANDLE && has_waits && has_signals;
    bool is_submitted = false;
    if (is_ready) {
        VkResult copy_result = submit_batch(bursts, queried_queue, fault.copy, gate, {copied}, {1});
        VkResult timestamp_result = submit_batch(bursts, queried_queue, timestamp, VK_NULL_HANDLE, {}, {});
        VkResult batch_result = submit_batch(bursts, marked_queue, VK_NULL_HANDLE, marked_wait, signals, values);
        VkResult lone_result = submit_batch(bursts, idled_queue, VK_NULL_HANDLE, idled_wait, {}, {});
        is_submitted = copy_result == VK_SUCCESS && timestamp_result == VK_SUCCESS && batch_result == VK_SUCCESS &&
            lone_result == VK_SUCCESS;

        free_fault_source(stage, queried, fault);
        is_submitted &= signal_timeline(work, queried, gate, 1) == VK_SUCCESS;
    }

    if (is_submitted) {
        // The last signal is in the chain's last part, which ends after the others, so every signal has its value
        shown.batch_ends_lost = wait_ends_lost(bursts, marked, signals.back(), MARKED_VALUE, WORK_WAIT_NS);

        // The batch ends lost only once the copy's point is marked failed, so the query wait starts on a lost context
        shown.query_ends_lost = query_ends_lost(stage, queried, query_pool);
        shown.idle_ends_lost = idle_ends_lost(bursts, idled_queue);

        uint32_t marked_count = count_failed_timelines(stage.instance, stage.physical_device, MARKED_VALUE);
        shown.marks_every_signal = marked_count == MARKED_COUNT;
    }

    for (VkSemaphore signal : signals) {
        work.destroy_semaphore(marked, signal, nullptr);
    }

    work.destroy_semaphore(idled, idled_wait, nullptr);
    work.destroy_semaphore(marked, marked_wait, nullptr);
    work.destroy_semaphore(queried, copied, nullptr);
    work.destroy_semaphore(queried, gate, nullptr);
    stage.commands.destroy_query_pool(queried, query_pool, nullptr);
    destroy_fault(stage, queried, fault);

    return shown;
}

void check_fault(VkInstance instance, VkPhysicalDevice physical_device) {
    fault_stage stage = {};
    stage.commands = {
        .get_memory_properties = find_command<PFN_vkGetPhysicalDeviceMemoryProperties>(
            instance,
            "vkGetPhysicalDeviceMemoryProperties"
        ),
        .get_semaphore_fd = find_command<PFN_vkGetSemaphoreFdKHR>(instance, "vkGetSemaphoreFdKHR"),
        .import_semaphore_fd = find_command<PFN_vkImportSemaphoreFdKHR>(instance, "vkImportSemaphoreFdKHR"),
        .cmd_copy_buffer = find_command<PFN_vkCmdCopyBuffer>(instance, "vkCmdCopyBuffer"),
        .cmd_fill_buffer = find_command<PFN_vkCmdFillBuffer>(instance, "vkCmdFillBuffer"),
        .cmd_write_timestamp = find_command<PFN_vkCmdWriteTimestamp>(instance, "vkCmdWriteTimestamp"),
        .create_query_pool = find_command<PFN_vkCreateQueryPool>(instance, "vkCreateQueryPool"),
        .destroy_query_pool = find_command<PFN_vkDestroyQueryPool>(instance, "vkDestroyQueryPool"),
        .reset_query_pool = find_command<PFN_vkResetQueryPool>(instance, "vkResetQueryPool"),
        .get_query_pool_results = find_command<PFN_vkGetQueryPoolResults>(instance, "vkGetQueryPoolResults"),
    };

    stage.instance = instance;
    stage.physical_device = physical_device;

    const fault_commands& commands = stage.commands;
    bool has_burst_commands = find_burst_commands(instance, physical_device, stage.bursts);
    bool has_commands = commands.get_memory_properties && commands.get_semaphore_fd && commands.import_semaphore_fd &&
        commands.cmd_copy_buffer && commands.cmd_fill_buffer && commands.cmd_write_timestamp &&
        commands.create_query_pool && commands.destroy_query_pool && commands.reset_query_pool &&
        commands.get_query_pool_results && has_burst_commands;
    check("vkGetInstanceProcAddr finds the fault commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    commands.get_memory_properties(physical_device, &stage.properties);

    VkPhysicalDeviceZeroInitializeDeviceMemoryFeaturesEXT zeroing = {};
    zeroing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_DEVICE_MEMORY_FEATURES_EXT;
    zeroing.zeroInitializeDeviceMemory = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features.pNext = &zeroing;
    features.timelineSemaphore = VK_TRUE;
    features.hostQueryReset = VK_TRUE;

    std::vector<const char*> extensions = {
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_ZERO_INITIALIZE_DEVICE_MEMORY_EXTENSION_NAME,
    };

    const burst_commands& bursts = stage.bursts;
    uint32_t family = bursts.transfer_family;

    VkDevice faulting = VK_NULL_HANDLE;
    VkDevice running = VK_NULL_HANDLE;
    VkDevice queried = VK_NULL_HANDLE;
    VkDevice marked = VK_NULL_HANDLE;
    VkDevice idled = VK_NULL_HANDLE;
    if (family != UINT32_MAX) {
        // `faulting`'s general queue is destroyed first and waits for the transfer queue's thread to leave its list
        faulting = create_device(instance, physical_device, {GENERAL_FAMILY, family}, extensions, &features);
        running = create_device(instance, physical_device, {family}, extensions, &features);
        queried = create_device(instance, physical_device, {family}, extensions, &features);
        marked = create_device(instance, physical_device, {family}, extensions, &features);
        idled = create_device(instance, physical_device, {family}, extensions, &features);
    }

    bool has_devices = faulting != VK_NULL_HANDLE && running != VK_NULL_HANDLE && queried != VK_NULL_HANDLE &&
        marked != VK_NULL_HANDLE && idled != VK_NULL_HANDLE;
    check(
        "five devices with transfer queues, shared timeline semaphores, host query reset and zeroed memory are made",
        has_devices
    );
    if (!has_devices) {
        bursts.destroy_device(idled, nullptr);
        bursts.destroy_device(marked, nullptr);
        bursts.destroy_device(queried, nullptr);
        bursts.destroy_device(running, nullptr);
        bursts.destroy_device(faulting, nullptr);
        return;
    }

    fault_result in_burst = run_fault_in_burst(stage, faulting, running);
    check(
        "100 submits held on each of two devices succeed within 100 ms each, the first device's first running a copy",
        in_burst.holds_bursts
    );
    check(
        "memory NVK zeroes on its upload queue is made on the second device while that device's burst is held",
        in_burst.zeroes_while_held
    );
    check(
        "with the copy's source freed, a wait with no timeout on the burst's unsubmitted last point ends lost in 1 s",
        in_burst.point_ends_lost
    );
    check(
        "the faulted device's destroy returns within 1 s, after its transfer queue's overflow thread drops its list",
        in_burst.point_ends_lost && in_burst.destroys_in_time
    );
    check(
        "a fill waiting on the zeroing's upload point, held behind the second device's burst, completes with the burst",
        in_burst.other_burst_completes
    );

    waiter_result waiters = run_fault_with_waiters(stage, queried, marked, idled);
    check(
        "a wait with no timeout on a batch of 40 signals waiting on the copy's point ends lost on its device in 5 s",
        waiters.batch_ends_lost
    );
    check(
        "on the lost context with no overflow list, a wait for a query queued behind the copy ends lost in 100 ms",
        waiters.query_ends_lost
    );
    check(
        "the idle wait of a queue whose one submission waits on the copy's point ends lost within 100 ms",
        waiters.idle_ends_lost
    );
    check("the kernel marks all 40 timelines of the batch failed", waiters.marks_every_signal);

    for (VkDevice device : {idled, marked, queried, running}) {
        start_hang_watch("vkDestroyDevice");
        bursts.destroy_device(device, nullptr);
        end_hang_watch();
    }
}
