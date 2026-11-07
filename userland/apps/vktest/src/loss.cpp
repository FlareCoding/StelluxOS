// The loss stage: devices lost by submits the kernel refuses while bursts of 100 overflow their kernel queues, each
// wait on a lost device's work and each lost device's destroy ending within a bound

#include "vktest.h"

#include <stdint.h>
#include <vector>

struct loss_stage {
    burst_commands   bursts;
    VkInstance       instance;
    VkPhysicalDevice physical_device;
};

// What a submit refused on another queue of a device holding a burst showed, each false until shown
struct refusal_result {
    bool holds_bursts;
    bool is_refused;
    bool waits_end_lost;
    bool destroys_in_time;
    bool other_burst_completes;
};

// What a submit refused from the overflow thread showed, each false until shown
struct thread_refusal_result {
    bool submits_at_once;
    bool waits_end_lost;
    bool destroys_in_time;
};

// A value no other timeline of vktest reads, by which vktest finds the semaphore whose descriptor it replaces
constexpr uint64_t REPLACED_VALUE = 1ull << 48;

// Holds bursts on the transfer queues of `other` and `lost`, then has the kernel refuse a submit on the general queue
// of `lost`, whose destroy takes that queue first and so ends only once the transfer queue's thread dropped its list
static refusal_result run_refusal_on_general_queue(const loss_stage& stage, VkDevice other, VkDevice lost) {
    refusal_result shown = {};
    const burst_commands& bursts = stage.bursts;

    held_burst other_burst = make_burst(bursts, other);
    held_burst lost_burst = make_burst(bursts, lost);
    VkSemaphore refused_done = create_timeline(bursts.work, lost, 0, false);
    VkSemaphore replaced = create_timeline(bursts.work, lost, REPLACED_VALUE, false);

    VkQueue general_queue = VK_NULL_HANDLE;
    bursts.work.get_device_queue(lost, GENERAL_FAMILY, 0, &general_queue);

    bool has_semaphores = refused_done != VK_NULL_HANDLE && replaced != VK_NULL_HANDLE;
    bool is_ready = is_made(other_burst) && is_made(lost_burst) && general_queue != VK_NULL_HANDLE && has_semaphores;
    if (is_ready) {
        bool holds_other_burst = holds_burst(bursts, other_burst);
        bool holds_lost_burst = holds_burst(bursts, lost_burst);
        shown.holds_bursts = holds_other_burst && holds_lost_burst;
    }

    bool is_replaced = is_ready && replace_timeline_descriptor(stage.instance, stage.physical_device, REPLACED_VALUE);
    if (is_replaced) {
        VkResult result = submit_batch(
            bursts,
            general_queue,
            VK_NULL_HANDLE,
            {refused_done, replaced},
            {1, REPLACED_VALUE + 1}
        );
        shown.is_refused = result == VK_ERROR_DEVICE_LOST;
    }

    if (shown.is_refused) {
        shown.waits_end_lost = wait_ends_lost(bursts, lost, refused_done, 1, LOST_WAIT_LIMIT_NS) &&
            wait_ends_lost(bursts, lost, lost_burst.done, BURST_SIZE, LOST_WAIT_LIMIT_NS) &&
            idle_ends_lost(bursts, lost_burst.queue);
    } else if (is_ready) {
        // A live device's destroy waits for its overflow lists to drain
        drains_burst(bursts, lost_burst);
    }

    bursts.work.destroy_semaphore(lost, replaced, nullptr);
    bursts.work.destroy_semaphore(lost, refused_done, nullptr);
    destroy_burst(bursts, lost_burst);
    shown.destroys_in_time = destroys_in_time(bursts, lost);

    shown.other_burst_completes = is_ready && drains_burst(bursts, other_burst);
    destroy_burst(bursts, other_burst);

    return shown;
}

// Puts on the overflow list, behind a held burst, a submit signalling a semaphore whose descriptor vktest replaced, so
// the kernel refuses that submit only when the overflow thread submits it, after the CPU signals the burst's gate
static thread_refusal_result run_refusal_on_thread(const loss_stage& stage, VkDevice device) {
    thread_refusal_result shown = {};
    const burst_commands& bursts = stage.bursts;

    held_burst burst = make_burst(bursts, device);
    VkSemaphore replaced = create_timeline(bursts.work, device, REPLACED_VALUE, false);

    bool is_ready = is_made(burst) && replaced != VK_NULL_HANDLE;
    bool is_replaced = is_ready && replace_timeline_descriptor(stage.instance, stage.physical_device, REPLACED_VALUE);
    if (is_replaced) {
        bool holds = holds_burst(bursts, burst);

        uint64_t start_ns = monotonic_ns();
        VkResult result = submit_batch(
            bursts,
            burst.queue,
            VK_NULL_HANDLE,
            {burst.done, replaced},
            {BURST_SIZE + 1, REPLACED_VALUE + 1}
        );
        uint64_t took_ns = monotonic_ns() - start_ns;

        shown.submits_at_once = holds && result == VK_SUCCESS && took_ns < SUBMIT_LIMIT_NS;
    }

    if (is_replaced && signal_timeline(bursts.work, device, burst.gate, 1) == VK_SUCCESS) {
        shown.waits_end_lost = wait_ends_lost(bursts, device, burst.done, BURST_SIZE + 1, WORK_WAIT_NS) &&
            idle_ends_lost(bursts, burst.queue);
    }

    bursts.work.destroy_semaphore(device, replaced, nullptr);
    destroy_burst(bursts, burst);
    shown.destroys_in_time = destroys_in_time(bursts, device);

    return shown;
}

void check_loss(VkInstance instance, VkPhysicalDevice physical_device) {
    loss_stage stage = {};
    stage.instance = instance;
    stage.physical_device = physical_device;

    bool has_commands = find_burst_commands(instance, physical_device, stage.bursts);
    check("vkGetInstanceProcAddr finds the loss commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    VkPhysicalDeviceVulkan12Features features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features.timelineSemaphore = VK_TRUE;

    const burst_commands& bursts = stage.bursts;
    uint32_t family = bursts.transfer_family;

    VkDevice other = VK_NULL_HANDLE;
    VkDevice lost = VK_NULL_HANDLE;
    VkDevice thread_lost = VK_NULL_HANDLE;
    if (family != UINT32_MAX) {
        other = create_device(instance, physical_device, {family}, {}, &features);
        lost = create_device(instance, physical_device, {GENERAL_FAMILY, family}, {}, &features);
        thread_lost = create_device(instance, physical_device, {family}, {}, &features);
    }

    bool has_devices = other != VK_NULL_HANDLE && lost != VK_NULL_HANDLE && thread_lost != VK_NULL_HANDLE;
    check("three devices with transfer queues and timeline semaphores are made, one with a general queue", has_devices);
    if (!has_devices) {
        bursts.destroy_device(thread_lost, nullptr);
        bursts.destroy_device(lost, nullptr);
        bursts.destroy_device(other, nullptr);
        return;
    }

    refusal_result on_general_queue = run_refusal_on_general_queue(stage, other, lost);
    check(
        "100 submits held on the transfer queues of two devices each succeed within 100 ms",
        on_general_queue.holds_bursts
    );
    check(
        "a submit on the general queue signalling a semaphore whose descriptor vktest replaced loses the second device",
        on_general_queue.is_refused
    );
    check(
        "waits with no timeout on its point and the burst's last, and the burst queue's idle wait, end lost in 100 ms",
        on_general_queue.waits_end_lost
    );
    check(
        "the lost device's destroy returns within 1 s, after its transfer queue's overflow thread drops its list",
        on_general_queue.is_refused && on_general_queue.destroys_in_time
    );
    check(
        "the first device's held burst completes and its queue waits idle once the CPU signals its point",
        on_general_queue.other_burst_completes
    );

    thread_refusal_result on_thread = run_refusal_on_thread(stage, thread_lost);
    check(
        "a submit signalling a semaphore whose descriptor vktest replaced succeeds at once behind a held burst of 100",
        on_thread.submits_at_once
    );
    check(
        "a wait with no timeout on that submit's point ends lost within 5 s and the queue's idle wait within 100 ms",
        on_thread.waits_end_lost
    );
    check(
        "the destroy of the device its overflow thread lost returns within 1 s",
        on_thread.waits_end_lost && on_thread.destroys_in_time
    );

    start_hang_watch("vkDestroyDevice");
    bursts.destroy_device(other, nullptr);
    end_hang_watch();
}
