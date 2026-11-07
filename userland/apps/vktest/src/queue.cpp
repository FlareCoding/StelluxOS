// The queue stage: bursts of more submissions than one kernel queue has room for, held behind a point the CPU signals
// after its last submit. NVK keeps the rest on an overflow list, whose thread submits them in order as room appears.

#include "vktest.h"

#include <stdint.h>
#include <stlx/gpu_abi.h>
#include <string.h>
#include <time.h>
#include <vector>

struct queue_commands {
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    PFN_vkDestroyDevice                     destroy_device;

    PFN_vkCmdFillBuffer cmd_fill_buffer;
    PFN_vkCmdCopyBuffer cmd_copy_buffer;
};

// A device with a transfer queue alone, whose bursts write words the CPU reads through memory of the staging type
struct queue_stage {
    queue_commands                   commands;
    work_commands                    work;
    VkPhysicalDeviceMemoryProperties properties;
    uint32_t                         transfer_family;
    VkDevice                         device;
    VkQueue                          queue;
};

// What one burst showed, each false until shown
struct burst_result {
    bool submits_at_once;
    bool waits_live_for_gate;
    bool completes;
    bool runs_in_order;
};

// A submission past the queue's room with more waits than one `GPU_SUBMIT` takes, so the list holds it in two parts
constexpr uint32_t CHAINED_INDEX = GPU_QUEUE_PENDING_MAX + 16;
constexpr uint32_t CHAINED_WAIT_COUNT = GPU_POINTS_MAX + 8;

// The CPU submits this step right after it signals the gate, and the kernel must take it after the burst's last
constexpr uint32_t LATE_INDEX = BURST_SIZE;
constexpr uint32_t STEP_COUNT = BURST_SIZE + 1;
static_assert((STEP_COUNT + 1) * sizeof(uint32_t) <= PAGE_BUFFER_SIZE, "a burst's words must fit its buffer");

// How long the CPU holds a burst after its last submit, so the overflow thread's waits for room time out at least twice
constexpr uint64_t GATE_HOLD_NS = 3 * WAIT_SLICE_NS;

// Records submission `index`: it copies word 0 into word index + 1, then writes index + 1 to word 0
static VkResult record_step(
    const queue_stage& stage,
    VkCommandBuffer command_buffer,
    VkBuffer buffer,
    uint32_t index
) {
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkBufferCopy region = {};
    region.dstOffset = (index + 1) * sizeof(uint32_t);
    region.size = sizeof(uint32_t);

    VkResult result = stage.work.begin_command_buffer(command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        return result;
    }

    stage.commands.cmd_copy_buffer(command_buffer, buffer, buffer, 1, &region);

    wait_for_transfer_writes(stage.work, command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    stage.commands.cmd_fill_buffer(command_buffer, buffer, 0, sizeof(uint32_t), index + 1);

    // The next submission's copy reads word 0, and the CPU reads every word once the burst is done
    wait_for_transfer_writes(
        stage.work,
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT
    );

    return stage.work.end_command_buffer(command_buffer);
}

// Submits `command_buffer` as submission `index`, signalling `done` at index + 1 and waiting on each of `waits` at 1,
// returning whether `vkQueueSubmit` succeeded within `SUBMIT_LIMIT_NS`
static bool submits_at_once(
    const queue_stage& stage,
    VkCommandBuffer command_buffer,
    uint32_t index,
    const std::vector<VkSemaphore>& waits,
    VkSemaphore done
) {
    std::vector<uint64_t> wait_values(waits.size(), 1);
    std::vector<VkPipelineStageFlags> wait_stages(waits.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
    uint64_t done_value = index + 1;

    VkTimelineSemaphoreSubmitInfo timeline_info = {};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.waitSemaphoreValueCount = static_cast<uint32_t>(wait_values.size());
    timeline_info.pWaitSemaphoreValues = wait_values.data();
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = &done_value;

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = &timeline_info;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;

    submit_info.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    submit_info.pWaitSemaphores = waits.data();
    submit_info.pWaitDstStageMask = wait_stages.data();
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &done;

    start_hang_watch("vkQueueSubmit");
    uint64_t start_ns = monotonic_ns();
    VkResult result = stage.work.queue_submit(stage.queue, 1, &submit_info, VK_NULL_HANDLE);
    uint64_t took_ns = monotonic_ns() - start_ns;
    end_hang_watch();

    return result == VK_SUCCESS && took_ns < SUBMIT_LIMIT_NS;
}

// Submission i copied word 0 into word i + 1 before writing i + 1 to word 0, so in submission order word i + 1 holds i
static bool holds_burst_words(const uint32_t* words) {
    for (uint32_t i = 0; i < STEP_COUNT; i++) {
        if (words[i + 1] != i) {
            return false;
        }
    }

    return words[0] == STEP_COUNT;
}

// Submits a burst whose first submission waits on `gate` and whose chained one waits on `chained_waits`, holds it for
// `GATE_HOLD_NS`, then signals the gate, submits the late step and waits for every step
static burst_result run_held_burst(
    const queue_stage& stage,
    const std::vector<VkCommandBuffer>& command_buffers,
    VkSemaphore gate,
    const std::vector<VkSemaphore>& chained_waits,
    VkSemaphore done,
    const uint32_t* words
) {
    burst_result shown = {};

    shown.submits_at_once = true;
    for (uint32_t i = 0; i < BURST_SIZE; i++) {
        std::vector<VkSemaphore> waits;
        if (i == 0) {
            waits.push_back(gate);
        } else if (i == CHAINED_INDEX) {
            waits = chained_waits;
        }

        shown.submits_at_once &= submits_at_once(stage, command_buffers[i], i, waits, done);
    }

    timespec hold = {0, static_cast<long>(GATE_HOLD_NS)};
    nanosleep(&hold, nullptr);
    shown.waits_live_for_gate = reads_value(stage.work, stage.device, done, 0);

    // The held submissions must drain before their command buffers and semaphores go, whatever the checks found
    VkResult result = signal_timeline(stage.work, stage.device, gate, 1);
    bool submits_late_step =
        result == VK_SUCCESS && submits_at_once(stage, command_buffers[LATE_INDEX], LATE_INDEX, {}, done);
    uint64_t last_value = submits_late_step ? STEP_COUNT : BURST_SIZE;
    if (result == VK_SUCCESS) {
        result = wait_for_value(stage.work, stage.device, done, last_value, WORK_WAIT_NS);
    }

    start_hang_watch("vkQueueWaitIdle");
    VkResult idle_result = stage.work.queue_wait_idle(stage.queue);
    end_hang_watch();
    shown.completes = submits_late_step && result == VK_SUCCESS && idle_result == VK_SUCCESS;
    shown.runs_in_order = shown.completes && holds_burst_words(words);

    return shown;
}

// Makes a burst's buffer, semaphores and command buffers, runs it held, and destroys what it made
static burst_result run_burst(const queue_stage& stage) {
    bound_buffer burst_buffer =
        make_staging_buffer(stage.work, stage.device, stage.properties, PAGE_BUFFER_SIZE, TRANSFER_USAGE);
    VkSemaphore gate = create_timeline(stage.work, stage.device, 0, false);
    VkSemaphore done = create_timeline(stage.work, stage.device, 0, false);

    std::vector<VkSemaphore> chained_waits;
    for (uint32_t i = 0; i < CHAINED_WAIT_COUNT; i++) {
        chained_waits.push_back(create_timeline(stage.work, stage.device, 1, false));
    }

    VkCommandPool pool = make_command_pool(stage.work, stage.device, stage.transfer_family);

    VkCommandBufferAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = STEP_COUNT;

    std::vector<VkCommandBuffer> command_buffers(STEP_COUNT, VK_NULL_HANDLE);
    bool has_buffer = burst_buffer.words != nullptr;
    bool is_recorded = has_buffer && pool != VK_NULL_HANDLE &&
        stage.work.allocate_command_buffers(stage.device, &allocate_info, command_buffers.data()) == VK_SUCCESS;
    for (uint32_t i = 0; is_recorded && i < STEP_COUNT; i++) {
        is_recorded = record_step(stage, command_buffers[i], burst_buffer.buffer, i) == VK_SUCCESS;
    }

    bool has_semaphores = gate != VK_NULL_HANDLE && done != VK_NULL_HANDLE;
    for (VkSemaphore semaphore : chained_waits) {
        has_semaphores &= semaphore != VK_NULL_HANDLE;
    }

    burst_result shown = {};
    if (has_semaphores && is_recorded) {
        memset(burst_buffer.words, 0, PAGE_BUFFER_SIZE);
        shown = run_held_burst(stage, command_buffers, gate, chained_waits, done, burst_buffer.words);
    }

    stage.work.destroy_command_pool(stage.device, pool, nullptr);
    for (VkSemaphore semaphore : chained_waits) {
        stage.work.destroy_semaphore(stage.device, semaphore, nullptr);
    }

    stage.work.destroy_semaphore(stage.device, done, nullptr);
    stage.work.destroy_semaphore(stage.device, gate, nullptr);
    destroy_bound_buffer(stage.work, stage.device, burst_buffer);

    return shown;
}

void check_queue(VkInstance instance, VkPhysicalDevice physical_device) {
    queue_stage stage = {};
    stage.commands = {
        .get_memory_properties = find_command<PFN_vkGetPhysicalDeviceMemoryProperties>(
            instance,
            "vkGetPhysicalDeviceMemoryProperties"
        ),
        .destroy_device = find_command<PFN_vkDestroyDevice>(instance, "vkDestroyDevice"),
        .cmd_fill_buffer = find_command<PFN_vkCmdFillBuffer>(instance, "vkCmdFillBuffer"),
        .cmd_copy_buffer = find_command<PFN_vkCmdCopyBuffer>(instance, "vkCmdCopyBuffer"),
    };

    const queue_commands& commands = stage.commands;
    bool has_work_commands = find_work_commands(instance, stage.work);
    bool has_commands = commands.get_memory_properties && commands.destroy_device && commands.cmd_fill_buffer &&
        commands.cmd_copy_buffer && has_work_commands;
    check("vkGetInstanceProcAddr finds the queue commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    VkPhysicalDeviceVulkan12Features features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features.timelineSemaphore = VK_TRUE;

    commands.get_memory_properties(physical_device, &stage.properties);
    stage.transfer_family = find_transfer_family(instance, physical_device);
    if (stage.transfer_family != UINT32_MAX) {
        stage.device = create_device(instance, physical_device, {stage.transfer_family}, {}, &features);
    }

    bool has_device = stage.device != VK_NULL_HANDLE;
    check("a device with a transfer queue alone and timeline semaphores is created", has_device);
    if (!has_device) {
        return;
    }

    stage.work.get_device_queue(stage.device, stage.transfer_family, 0, &stage.queue);

    burst_result first = run_burst(stage);
    check("100 submits held behind a point the CPU signals later each succeed within 100 ms", first.submits_at_once);
    check(
        "the device stays live and no submission of the held burst runs while the CPU waits 300 ms to signal its point",
        first.waits_live_for_gate
    );
    check(
        "the held burst and a submit right after the CPU signals the burst's point complete, and the queue waits idle",
        first.completes
    );
    check(
        "every submission runs in submission order, the chained one of 40 waits and the one after the signal included",
        first.runs_in_order
    );

    burst_result second = run_burst(stage);
    bool repeats = second.submits_at_once && second.waits_live_for_gate && second.completes && second.runs_in_order;
    check(
        "a second held burst after the first drained is submitted at once, waits live, completes and runs in order",
        repeats
    );

    start_hang_watch("vkDestroyDevice");
    commands.destroy_device(stage.device, nullptr);
    end_hang_watch();
}
