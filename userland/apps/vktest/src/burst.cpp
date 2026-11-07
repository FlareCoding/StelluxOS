// Bursts held on a device's transfer queue, and the timed waits and destroys that each stage losing devices checks

#include "vktest.h"

#include <stdint.h>
#include <vector>

bool find_burst_commands(VkInstance instance, VkPhysicalDevice physical_device, burst_commands& bursts) {
    bursts.destroy_device = find_command<PFN_vkDestroyDevice>(instance, "vkDestroyDevice");
    bursts.transfer_family = find_transfer_family(instance, physical_device);

    return find_work_commands(instance, bursts.work) && bursts.destroy_device;
}

held_burst make_burst(const burst_commands& bursts, VkDevice device) {
    held_burst burst;
    burst.device = device;
    bursts.work.get_device_queue(device, bursts.transfer_family, 0, &burst.queue);

    burst.gate = create_timeline(bursts.work, device, 0, false);
    burst.done = create_timeline(bursts.work, device, 0, false);

    return burst;
}

void destroy_burst(const burst_commands& bursts, const held_burst& burst) {
    bursts.work.destroy_semaphore(burst.device, burst.done, nullptr);
    bursts.work.destroy_semaphore(burst.device, burst.gate, nullptr);
}

bool is_made(const held_burst& burst) {
    return burst.queue != VK_NULL_HANDLE && burst.gate != VK_NULL_HANDLE && burst.done != VK_NULL_HANDLE;
}

VkResult submit_batch(
    const burst_commands& bursts,
    VkQueue queue,
    VkSemaphore gate,
    const std::vector<VkSemaphore>& signals,
    const std::vector<uint64_t>& values
) {
    uint32_t wait_count = gate != VK_NULL_HANDLE ? 1 : 0;
    uint64_t gate_value = 1;
    VkPipelineStageFlags gate_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    VkTimelineSemaphoreSubmitInfo timeline_info = {};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.waitSemaphoreValueCount = wait_count;
    timeline_info.pWaitSemaphoreValues = &gate_value;
    timeline_info.signalSemaphoreValueCount = static_cast<uint32_t>(values.size());
    timeline_info.pSignalSemaphoreValues = values.data();

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = &timeline_info;
    submit_info.waitSemaphoreCount = wait_count;
    submit_info.pWaitSemaphores = &gate;
    submit_info.pWaitDstStageMask = &gate_stage;

    submit_info.signalSemaphoreCount = static_cast<uint32_t>(signals.size());
    submit_info.pSignalSemaphores = signals.data();

    start_hang_watch("vkQueueSubmit");
    VkResult result = bursts.work.queue_submit(queue, 1, &submit_info, VK_NULL_HANDLE);
    end_hang_watch();

    return result;
}

bool holds_burst(const burst_commands& bursts, const held_burst& burst) {
    bool submits_at_once = true;
    for (uint32_t i = 0; i < BURST_SIZE; i++) {
        VkSemaphore gate = i == 0 ? burst.gate : VK_NULL_HANDLE;

        uint64_t start_ns = monotonic_ns();
        VkResult result = submit_batch(bursts, burst.queue, gate, {burst.done}, {i + 1});
        uint64_t took_ns = monotonic_ns() - start_ns;

        submits_at_once &= result == VK_SUCCESS && took_ns < SUBMIT_LIMIT_NS;
    }

    return submits_at_once;
}

bool drains_burst(const burst_commands& bursts, const held_burst& burst) {
    VkResult result = signal_timeline(bursts.work, burst.device, burst.gate, 1);
    if (result == VK_SUCCESS) {
        result = wait_for_value(bursts.work, burst.device, burst.done, BURST_SIZE, WORK_WAIT_NS);
    }

    if (result == VK_SUCCESS) {
        start_hang_watch("vkQueueWaitIdle");
        result = bursts.work.queue_wait_idle(burst.queue);
        end_hang_watch();
    }

    return result == VK_SUCCESS;
}

bool wait_ends_lost(
    const burst_commands& bursts,
    VkDevice device,
    VkSemaphore semaphore,
    uint64_t value,
    uint64_t limit_ns
) {
    start_hang_watch("vkWaitSemaphores");
    uint64_t start_ns = monotonic_ns();
    VkResult result = wait_for_value(bursts.work, device, semaphore, value, UINT64_MAX);
    uint64_t took_ns = monotonic_ns() - start_ns;
    end_hang_watch();

    return result == VK_ERROR_DEVICE_LOST && took_ns < limit_ns;
}

bool idle_ends_lost(const burst_commands& bursts, VkQueue queue) {
    start_hang_watch("vkQueueWaitIdle");
    uint64_t start_ns = monotonic_ns();
    VkResult result = bursts.work.queue_wait_idle(queue);
    uint64_t took_ns = monotonic_ns() - start_ns;
    end_hang_watch();

    return result == VK_ERROR_DEVICE_LOST && took_ns < LOST_WAIT_LIMIT_NS;
}

bool destroys_in_time(const burst_commands& bursts, VkDevice device) {
    start_hang_watch("vkDestroyDevice");
    uint64_t start_ns = monotonic_ns();
    bursts.destroy_device(device, nullptr);
    uint64_t took_ns = monotonic_ns() - start_ns;
    end_hang_watch();

    return took_ns < DESTROY_LIMIT_NS;
}
