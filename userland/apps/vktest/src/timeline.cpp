// The timeline stage: timeline semaphores signalled and waited on from the CPU and from submissions, shared and refused
// through opaque descriptors, and submissions and waits naming more points than one kernel call takes

#include "vktest.h"

#include <algorithm>
#include <stdint.h>
#include <stlx/gpu_abi.h>
#include <unistd.h>
#include <vector>

struct timeline_commands {
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties get_external_semaphore_properties;
    PFN_vkGetPhysicalDeviceExternalFenceProperties     get_external_fence_properties;

    PFN_vkCreateDevice             create_device;
    PFN_vkDestroyDevice            destroy_device;
    PFN_vkGetDeviceQueue           get_device_queue;
    PFN_vkQueueSubmit              queue_submit;
    PFN_vkCreateSemaphore          create_semaphore;
    PFN_vkDestroySemaphore         destroy_semaphore;
    PFN_vkGetSemaphoreCounterValue get_counter_value;
    PFN_vkSignalSemaphore          signal_semaphore;
    PFN_vkWaitSemaphores           wait_semaphores;
    PFN_vkGetSemaphoreFdKHR        get_semaphore_fd;
    PFN_vkImportSemaphoreFdKHR     import_semaphore_fd;
    PFN_vkAllocateMemory           allocate_memory;
    PFN_vkFreeMemory               free_memory;
    PFN_vkGetMemoryFdKHR           get_memory_fd;
};

struct timeline_stage {
    timeline_commands commands;
    VkInstance        instance;
    VkPhysicalDevice  physical_device;
    VkDevice          device;
    VkQueue           queue;
};

// Which of a wait's points must be reached
enum class wait_mode {
    all,
    any,
};

// More points than one GPU_TIMELINE_WAIT or one GPU_SUBMIT takes
constexpr uint32_t MANY_POINT_COUNT = GPU_POINTS_MAX + 8;

// Fewer points than one GPU_TIMELINE_WAIT takes, but two, since the runtime drops wait-any from a wait on one point
constexpr uint32_t FEW_POINT_COUNT = 2;

// How long a wait runs when its points must stay unreached, and when they must be reached
constexpr uint64_t UNREACHED_WAIT_NS = 10000000;
constexpr uint64_t REACHED_WAIT_NS = 1000000000;

// Bytes of the memory whose opaque descriptor a timeline import must refuse
constexpr VkDeviceSize REFUSED_MEMORY_SIZE = 65536;

static VkSemaphore create_timeline(const timeline_stage& stage, uint64_t initial_value, bool is_exportable) {
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
    if (stage.commands.create_semaphore(stage.device, &create_info, nullptr, &semaphore) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    return semaphore;
}

static std::vector<VkSemaphore> create_timelines(const timeline_stage& stage, uint32_t count) {
    std::vector<VkSemaphore> semaphores;
    for (uint32_t i = 0; i < count; i++) {
        semaphores.push_back(create_timeline(stage, 0, false));
    }

    return semaphores;
}

static bool are_created(const std::vector<VkSemaphore>& semaphores) {
    return std::find(semaphores.begin(), semaphores.end(), VK_NULL_HANDLE) == semaphores.end();
}

static void destroy_timelines(const timeline_stage& stage, const std::vector<VkSemaphore>& semaphores) {
    for (VkSemaphore semaphore : semaphores) {
        stage.commands.destroy_semaphore(stage.device, semaphore, nullptr);
    }
}

static bool reads_value(const timeline_stage& stage, VkSemaphore semaphore, uint64_t expected) {
    uint64_t value = 0;
    return stage.commands.get_counter_value(stage.device, semaphore, &value) == VK_SUCCESS && value == expected;
}

static VkResult signal_timeline(const timeline_stage& stage, VkSemaphore semaphore, uint64_t value) {
    VkSemaphoreSignalInfo signal_info = {};
    signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signal_info.semaphore = semaphore;
    signal_info.value = value;

    return stage.commands.signal_semaphore(stage.device, &signal_info);
}

// Waits for every semaphore of `semaphores`, or for any of them, to reach `value`
static VkResult wait_for_points(
    const timeline_stage& stage,
    const std::vector<VkSemaphore>& semaphores,
    uint64_t value,
    wait_mode mode,
    uint64_t timeout_ns
) {
    std::vector<uint64_t> values(semaphores.size(), value);

    VkSemaphoreWaitInfo wait_info = {};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait_info.flags = mode == wait_mode::any ? VK_SEMAPHORE_WAIT_ANY_BIT : 0;
    wait_info.semaphoreCount = static_cast<uint32_t>(semaphores.size());
    wait_info.pSemaphores = semaphores.data();
    wait_info.pValues = values.data();

    return stage.commands.wait_semaphores(stage.device, &wait_info, timeout_ns);
}

// Submits a batch with no command buffers that waits for every semaphore of `waits` to reach `wait_value` and then
// raises every semaphore of `signals` to `signal_value`
static bool submit_batch(
    const timeline_stage& stage,
    const std::vector<VkSemaphore>& waits,
    uint64_t wait_value,
    const std::vector<VkSemaphore>& signals,
    uint64_t signal_value
) {
    std::vector<uint64_t> wait_values(waits.size(), wait_value);
    std::vector<uint64_t> signal_values(signals.size(), signal_value);
    std::vector<VkPipelineStageFlags> wait_stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    VkTimelineSemaphoreSubmitInfo timeline_info = {};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.waitSemaphoreValueCount = static_cast<uint32_t>(wait_values.size());
    timeline_info.pWaitSemaphoreValues = wait_values.data();
    timeline_info.signalSemaphoreValueCount = static_cast<uint32_t>(signal_values.size());
    timeline_info.pSignalSemaphoreValues = signal_values.data();

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = &timeline_info;
    submit_info.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    submit_info.pWaitSemaphores = waits.data();
    submit_info.pWaitDstStageMask = wait_stages.data();

    submit_info.signalSemaphoreCount = static_cast<uint32_t>(signals.size());
    submit_info.pSignalSemaphores = signals.data();

    return stage.commands.queue_submit(stage.queue, 1, &submit_info, VK_NULL_HANDLE) == VK_SUCCESS;
}

static int export_timeline(const timeline_stage& stage, VkSemaphore semaphore) {
    VkSemaphoreGetFdInfoKHR fd_info = {};
    fd_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    fd_info.semaphore = semaphore;
    fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    int descriptor = -1;
    if (stage.commands.get_semaphore_fd(stage.device, &fd_info, &descriptor) != VK_SUCCESS) {
        return -1;
    }

    return descriptor;
}

// Imports `descriptor` into `semaphore`. The import owns the descriptor once it succeeds, and the caller still owns it
// after a failure.
static VkResult import_timeline(const timeline_stage& stage, VkSemaphore semaphore, int descriptor) {
    VkImportSemaphoreFdInfoKHR import_info = {};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    import_info.semaphore = semaphore;
    import_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    import_info.fd = descriptor;

    return stage.commands.import_semaphore_fd(stage.device, &import_info);
}

// Allocates exportable memory of type 0, which every device has, into `memory` and returns an opaque descriptor of it,
// or -1
static int export_memory(const timeline_stage& stage, VkDeviceMemory& memory) {
    VkExportMemoryAllocateInfo export_info = {};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &export_info;
    allocate_info.allocationSize = REFUSED_MEMORY_SIZE;
    allocate_info.memoryTypeIndex = 0;
    if (stage.commands.allocate_memory(stage.device, &allocate_info, nullptr, &memory) != VK_SUCCESS) {
        return -1;
    }

    VkMemoryGetFdInfoKHR fd_info = {};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory = memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    int descriptor = -1;
    if (stage.commands.get_memory_fd(stage.device, &fd_info, &descriptor) != VK_SUCCESS) {
        return -1;
    }

    return descriptor;
}

// Creates a device with no queues on the stage's physical device, or returns `VK_NULL_HANDLE`
static VkDevice create_queueless_device(const timeline_stage& stage) {
    VkPhysicalDeviceMaintenance9FeaturesKHR maintenance9 = {};
    maintenance9.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_FEATURES_KHR;
    maintenance9.maintenance9 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features.pNext = &maintenance9;
    features.timelineSemaphore = VK_TRUE;

    const char* extension = VK_KHR_MAINTENANCE_9_EXTENSION_NAME;
    VkDeviceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = &features;
    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = &extension;

    VkDevice device = VK_NULL_HANDLE;
    if (stage.commands.create_device(stage.physical_device, &create_info, nullptr, &device) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    return device;
}

// Creates a semaphore holding `exported`'s timeline through a descriptor with `GPU_RIGHT_READ` alone, or returns
// `VK_NULL_HANDLE`
static VkSemaphore import_read_only(const timeline_stage& stage, VkSemaphore exported) {
    int descriptor = export_timeline(stage, exported);
    int read_only = descriptor >= 0 ? restrict_to_read(stage.instance, stage.physical_device, descriptor) : -1;
    if (descriptor >= 0) {
        close(descriptor);
    }

    VkSemaphore imported = read_only >= 0 ? create_timeline(stage, 0, false) : VK_NULL_HANDLE;
    if (imported != VK_NULL_HANDLE && import_timeline(stage, imported, read_only) == VK_SUCCESS) {
        return imported;
    }

    if (read_only >= 0) {
        close(read_only);
    }

    stage.commands.destroy_semaphore(stage.device, imported, nullptr);

    return VK_NULL_HANDLE;
}

// Whether a timeline reads its initial value and then the value the CPU signals, ends a wait for that value and holds
// a wait for a higher one until its timeout
static bool counts_cpu_signals(const timeline_stage& stage) {
    const uint64_t initial_value = 5;
    const uint64_t signalled_value = 9;
    VkSemaphore semaphore = create_timeline(stage, initial_value, false);
    if (semaphore == VK_NULL_HANDLE) {
        return false;
    }

    bool reads_initial_value = reads_value(stage, semaphore, initial_value);
    bool reads_signalled_value = signal_timeline(stage, semaphore, signalled_value) == VK_SUCCESS &&
        reads_value(stage, semaphore, signalled_value);
    bool ends_reached_wait = wait_for_points(stage, {semaphore}, signalled_value, wait_mode::all, 0) == VK_SUCCESS;
    bool holds_unreached_wait =
        wait_for_points(stage, {semaphore}, signalled_value + 1, wait_mode::all, UNREACHED_WAIT_NS) == VK_TIMEOUT;
    stage.commands.destroy_semaphore(stage.device, semaphore, nullptr);

    return reads_initial_value && reads_signalled_value && ends_reached_wait && holds_unreached_wait;
}

// Whether a device created with no queues, which NVK makes without a GPU context, signals a timeline and ends a CPU
// wait for the signalled value
static bool signals_without_queues(const timeline_stage& stage) {
    timeline_stage queueless = stage;
    queueless.device = create_queueless_device(stage);
    queueless.queue = VK_NULL_HANDLE;
    if (queueless.device == VK_NULL_HANDLE) {
        return false;
    }

    VkSemaphore semaphore = create_timeline(queueless, 0, false);
    bool ends_wait = semaphore != VK_NULL_HANDLE && signal_timeline(queueless, semaphore, 1) == VK_SUCCESS &&
        wait_for_points(queueless, {semaphore}, 1, wait_mode::all, REACHED_WAIT_NS) == VK_SUCCESS;
    queueless.commands.destroy_semaphore(queueless.device, semaphore, nullptr);
    queueless.commands.destroy_device(queueless.device, nullptr);

    return ends_wait;
}

// Whether a submission waiting on a point nobody has signalled is held, and runs once the CPU signals the point
static bool holds_submission_until_signal(const timeline_stage& stage) {
    VkSemaphore awaited = create_timeline(stage, 0, false);
    VkSemaphore signalled = create_timeline(stage, 0, false);
    bool is_submitted = awaited != VK_NULL_HANDLE && signalled != VK_NULL_HANDLE &&
        submit_batch(stage, {awaited}, 1, {signalled}, 1);

    bool is_held = is_submitted &&
        wait_for_points(stage, {signalled}, 1, wait_mode::all, UNREACHED_WAIT_NS) == VK_TIMEOUT;
    bool runs = is_submitted && signal_timeline(stage, awaited, 1) == VK_SUCCESS &&
        wait_for_points(stage, {signalled}, 1, wait_mode::all, REACHED_WAIT_NS) == VK_SUCCESS;
    stage.commands.destroy_semaphore(stage.device, signalled, nullptr);
    stage.commands.destroy_semaphore(stage.device, awaited, nullptr);

    return is_held && runs;
}

static VkExternalSemaphoreFeatureFlags semaphore_features(
    const timeline_stage& stage,
    VkSemaphoreType type,
    VkExternalSemaphoreHandleTypeFlagBits handle_type
) {
    VkSemaphoreTypeCreateInfo type_info = {};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = type;

    VkPhysicalDeviceExternalSemaphoreInfo semaphore_info = {};
    semaphore_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
    semaphore_info.pNext = &type_info;
    semaphore_info.handleType = handle_type;

    VkExternalSemaphoreProperties properties = {};
    properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
    stage.commands.get_external_semaphore_properties(stage.physical_device, &semaphore_info, &properties);

    return properties.externalSemaphoreFeatures;
}

static VkExternalFenceFeatureFlags fence_features(
    const timeline_stage& stage,
    VkExternalFenceHandleTypeFlagBits handle_type
) {
    VkPhysicalDeviceExternalFenceInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO;
    fence_info.handleType = handle_type;

    VkExternalFenceProperties properties = {};
    properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES;
    stage.commands.get_external_fence_properties(stage.physical_device, &fence_info, &properties);

    return properties.externalFenceFeatures;
}

// Whether binary semaphores and fences report no external handle type, and timeline semaphores report opaque
// descriptors as exportable and importable
static bool answers_semaphore_handle_types(const timeline_stage& stage) {
    VkExternalSemaphoreFeatureFlags timeline_features =
        semaphore_features(stage, VK_SEMAPHORE_TYPE_TIMELINE, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
    bool shares_timelines = timeline_features ==
        (VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);

    bool shares_no_binary =
        semaphore_features(stage, VK_SEMAPHORE_TYPE_BINARY, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) == 0 &&
        semaphore_features(stage, VK_SEMAPHORE_TYPE_BINARY, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT) == 0;
    bool shares_no_fence = fence_features(stage, VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT) == 0 &&
        fence_features(stage, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT) == 0;

    return shares_timelines && shares_no_binary && shares_no_fence;
}

// Whether a timeline exported through an opaque descriptor and imported into a second semaphore shows each
// semaphore's signals through the other
static bool shares_value_through_descriptor(const timeline_stage& stage) {
    VkSemaphore exported = create_timeline(stage, 0, true);
    VkSemaphore imported = create_timeline(stage, 0, false);
    int descriptor = exported != VK_NULL_HANDLE ? export_timeline(stage, exported) : -1;
    bool is_imported = imported != VK_NULL_HANDLE && descriptor >= 0 &&
        import_timeline(stage, imported, descriptor) == VK_SUCCESS;
    if (descriptor >= 0 && !is_imported) {
        close(descriptor);
    }

    bool shows_exporter_signal = is_imported && signal_timeline(stage, exported, 3) == VK_SUCCESS &&
        reads_value(stage, imported, 3);
    bool shows_importer_signal = is_imported && signal_timeline(stage, imported, 4) == VK_SUCCESS &&
        reads_value(stage, exported, 4);
    stage.commands.destroy_semaphore(stage.device, imported, nullptr);
    stage.commands.destroy_semaphore(stage.device, exported, nullptr);

    return shows_exporter_signal && shows_importer_signal;
}

// Whether a read-only import of a timeline holds a CPU wait and a submission's wait until the exporter signals, and
// ends both after
static bool waits_on_read_only_import(const timeline_stage& stage) {
    VkSemaphore exported = create_timeline(stage, 0, true);
    VkSemaphore read_only = exported != VK_NULL_HANDLE ? import_read_only(stage, exported) : VK_NULL_HANDLE;
    VkSemaphore signalled = create_timeline(stage, 0, false);
    bool has_semaphores = read_only != VK_NULL_HANDLE && signalled != VK_NULL_HANDLE;
    bool is_submitted = has_semaphores && submit_batch(stage, {read_only}, 1, {signalled}, 1);

    bool holds_waits = is_submitted &&
        wait_for_points(stage, {read_only}, 1, wait_mode::all, UNREACHED_WAIT_NS) == VK_TIMEOUT &&
        reads_value(stage, signalled, 0);
    bool is_signalled = is_submitted && signal_timeline(stage, exported, 1) == VK_SUCCESS;
    bool ends_waits = is_signalled &&
        wait_for_points(stage, {read_only}, 1, wait_mode::all, REACHED_WAIT_NS) == VK_SUCCESS &&
        wait_for_points(stage, {signalled}, 1, wait_mode::all, REACHED_WAIT_NS) == VK_SUCCESS;
    stage.commands.destroy_semaphore(stage.device, signalled, nullptr);
    stage.commands.destroy_semaphore(stage.device, read_only, nullptr);
    stage.commands.destroy_semaphore(stage.device, exported, nullptr);

    return holds_waits && ends_waits;
}

// Whether a read-only import of a timeline refuses a CPU signal with `VK_ERROR_UNKNOWN`, leaving the timeline's value
static bool refuses_signal_of_read_only_import(const timeline_stage& stage) {
    VkSemaphore exported = create_timeline(stage, 0, true);
    VkSemaphore read_only = exported != VK_NULL_HANDLE ? import_read_only(stage, exported) : VK_NULL_HANDLE;

    bool refuses_signal = read_only != VK_NULL_HANDLE && signal_timeline(stage, read_only, 1) == VK_ERROR_UNKNOWN;
    bool keeps_value = refuses_signal && reads_value(stage, exported, 0);
    stage.commands.destroy_semaphore(stage.device, read_only, nullptr);
    stage.commands.destroy_semaphore(stage.device, exported, nullptr);

    return keeps_value;
}

// Whether an opaque descriptor of memory, which the kernel restricts as it does a timeline's, is refused as a timeline
// semaphore's payload with the result for an invalid handle, leaving the caller its descriptor
static bool refuses_memory_descriptor(const timeline_stage& stage) {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int descriptor = export_memory(stage, memory);
    VkSemaphore semaphore = descriptor >= 0 ? create_timeline(stage, 0, false) : VK_NULL_HANDLE;

    bool is_refused = semaphore != VK_NULL_HANDLE &&
        import_timeline(stage, semaphore, descriptor) == VK_ERROR_INVALID_EXTERNAL_HANDLE;
    bool stays_open = descriptor >= 0 && close(descriptor) == 0;
    stage.commands.destroy_semaphore(stage.device, semaphore, nullptr);
    stage.commands.free_memory(stage.device, memory, nullptr);

    return is_refused && stays_open;
}

// Whether a submission with more waits and signals than one GPU_SUBMIT takes is held while only its wait at
// `holding_wait` is unreached, and reaches every signal once that wait is reached
static bool runs_submission_held_by_wait(const timeline_stage& stage, uint32_t holding_wait) {
    std::vector<VkSemaphore> waits = create_timelines(stage, MANY_POINT_COUNT);
    std::vector<VkSemaphore> signals = create_timelines(stage, MANY_POINT_COUNT);
    bool is_submitted = are_created(waits) && are_created(signals) && submit_batch(stage, waits, 1, signals, 1);

    bool signals_other_waits = is_submitted;
    for (uint32_t i = 0; is_submitted && i < MANY_POINT_COUNT; i++) {
        if (i != holding_wait) {
            signals_other_waits &= signal_timeline(stage, waits[i], 1) == VK_SUCCESS;
        }
    }

    bool is_held = signals_other_waits &&
        wait_for_points(stage, signals, 1, wait_mode::any, UNREACHED_WAIT_NS) == VK_TIMEOUT;
    bool runs = is_held && signal_timeline(stage, waits[holding_wait], 1) == VK_SUCCESS &&
        wait_for_points(stage, signals, 1, wait_mode::all, REACHED_WAIT_NS) == VK_SUCCESS;
    destroy_timelines(stage, signals);
    destroy_timelines(stage, waits);

    return runs;
}

// Whether a submission with more waits and signals than one GPU_SUBMIT takes is held while only its first wait, or its
// first wait past `GPU_POINTS_MAX` in the next GPU_SUBMIT, is unreached, and reaches every signal once it is reached
static bool runs_submission_past_kernel_points(const timeline_stage& stage) {
    return runs_submission_held_by_wait(stage, 0) && runs_submission_held_by_wait(stage, GPU_POINTS_MAX);
}

// Whether a wait for all of more points than one GPU_TIMELINE_WAIT takes is held while only its last point is
// unreached, and ends once that point is reached
static bool waits_for_all_past_kernel_points(const timeline_stage& stage) {
    std::vector<VkSemaphore> semaphores = create_timelines(stage, MANY_POINT_COUNT);
    bool has_semaphores = are_created(semaphores);

    bool signals_all_but_last = has_semaphores;
    for (uint32_t i = 0; has_semaphores && i + 1 < MANY_POINT_COUNT; i++) {
        signals_all_but_last &= signal_timeline(stage, semaphores[i], 1) == VK_SUCCESS;
    }

    bool is_held = signals_all_but_last &&
        wait_for_points(stage, semaphores, 1, wait_mode::all, UNREACHED_WAIT_NS) == VK_TIMEOUT;
    bool ends = signals_all_but_last && signal_timeline(stage, semaphores.back(), 1) == VK_SUCCESS &&
        wait_for_points(stage, semaphores, 1, wait_mode::all, REACHED_WAIT_NS) == VK_SUCCESS;
    destroy_timelines(stage, semaphores);

    return is_held && ends;
}

// Whether a wait for any of `count` points is held while none is reached, and ends once the last point is reached
static bool waits_for_any_of(const timeline_stage& stage, uint32_t count) {
    std::vector<VkSemaphore> semaphores = create_timelines(stage, count);
    bool has_semaphores = are_created(semaphores);

    bool is_held = has_semaphores &&
        wait_for_points(stage, semaphores, 1, wait_mode::any, UNREACHED_WAIT_NS) == VK_TIMEOUT;
    bool ends = has_semaphores && signal_timeline(stage, semaphores.back(), 1) == VK_SUCCESS &&
        wait_for_points(stage, semaphores, 1, wait_mode::any, REACHED_WAIT_NS) == VK_SUCCESS;
    destroy_timelines(stage, semaphores);

    return is_held && ends;
}

static void check_timelines(const timeline_stage& stage) {
    check(
        "a timeline reads its initial value and a CPU signal's, ends a wait for it and holds a wait for a higher value",
        counts_cpu_signals(stage)
    );
    check(
        "a device created with no queues signals a timeline semaphore and ends a CPU wait on it",
        signals_without_queues(stage)
    );
    check(
        "a submission waiting on an unsignalled point runs once the CPU signals it",
        holds_submission_until_signal(stage)
    );

    check(
        "binary semaphores and fences report no external handle type, and timeline semaphores opaque descriptors",
        answers_semaphore_handle_types(stage)
    );
    check(
        "a timeline shared through an opaque descriptor shows each semaphore's signals through the other",
        shares_value_through_descriptor(stage)
    );
    check(
        "a read-only descriptor of a timeline is waited on from the CPU and from a submission",
        waits_on_read_only_import(stage)
    );
    check("a read-only descriptor of a timeline refuses a CPU signal", refuses_signal_of_read_only_import(stage));
    check(
        "a timeline semaphore refuses an opaque memory descriptor as an invalid external handle and leaves it open",
        refuses_memory_descriptor(stage)
    );

    check(
        "a submission with more waits and signals than one GPU_SUBMIT takes runs once every wait is reached",
        runs_submission_past_kernel_points(stage)
    );
    check(
        "a wait for all of more points than one GPU_TIMELINE_WAIT takes is held until its last point is reached",
        waits_for_all_past_kernel_points(stage)
    );
    check(
        "a wait for any of more points than one GPU_TIMELINE_WAIT takes ends once one is reached",
        waits_for_any_of(stage, MANY_POINT_COUNT)
    );
    check(
        "a wait for any of fewer points than one GPU_TIMELINE_WAIT takes ends once one is reached",
        waits_for_any_of(stage, FEW_POINT_COUNT)
    );
}

void check_timeline(VkInstance instance, VkPhysicalDevice physical_device) {
    timeline_stage stage = {};
    stage.instance = instance;
    stage.physical_device = physical_device;
    stage.commands = {
        .get_external_semaphore_properties = find_command<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
            instance,
            "vkGetPhysicalDeviceExternalSemaphoreProperties"
        ),
        .get_external_fence_properties = find_command<PFN_vkGetPhysicalDeviceExternalFenceProperties>(
            instance,
            "vkGetPhysicalDeviceExternalFenceProperties"
        ),
        .create_device = find_command<PFN_vkCreateDevice>(instance, "vkCreateDevice"),
        .destroy_device = find_command<PFN_vkDestroyDevice>(instance, "vkDestroyDevice"),
        .get_device_queue = find_command<PFN_vkGetDeviceQueue>(instance, "vkGetDeviceQueue"),
        .queue_submit = find_command<PFN_vkQueueSubmit>(instance, "vkQueueSubmit"),
        .create_semaphore = find_command<PFN_vkCreateSemaphore>(instance, "vkCreateSemaphore"),
        .destroy_semaphore = find_command<PFN_vkDestroySemaphore>(instance, "vkDestroySemaphore"),
        .get_counter_value = find_command<PFN_vkGetSemaphoreCounterValue>(instance, "vkGetSemaphoreCounterValue"),
        .signal_semaphore = find_command<PFN_vkSignalSemaphore>(instance, "vkSignalSemaphore"),
        .wait_semaphores = find_command<PFN_vkWaitSemaphores>(instance, "vkWaitSemaphores"),
        .get_semaphore_fd = find_command<PFN_vkGetSemaphoreFdKHR>(instance, "vkGetSemaphoreFdKHR"),
        .import_semaphore_fd = find_command<PFN_vkImportSemaphoreFdKHR>(instance, "vkImportSemaphoreFdKHR"),
        .allocate_memory = find_command<PFN_vkAllocateMemory>(instance, "vkAllocateMemory"),
        .free_memory = find_command<PFN_vkFreeMemory>(instance, "vkFreeMemory"),
        .get_memory_fd = find_command<PFN_vkGetMemoryFdKHR>(instance, "vkGetMemoryFdKHR"),
    };

    const timeline_commands& commands = stage.commands;
    bool has_commands = commands.get_external_semaphore_properties && commands.get_external_fence_properties &&
        commands.create_device && commands.destroy_device && commands.get_device_queue &&
        commands.queue_submit && commands.create_semaphore && commands.destroy_semaphore &&
        commands.get_counter_value && commands.signal_semaphore && commands.wait_semaphores &&
        commands.get_semaphore_fd && commands.import_semaphore_fd && commands.allocate_memory &&
        commands.free_memory && commands.get_memory_fd;
    check("vkGetInstanceProcAddr finds the timeline commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    VkPhysicalDeviceVulkan12Features features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features.timelineSemaphore = VK_TRUE;
    std::vector<const char*> extensions = {
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    };

    stage.device = create_device(instance, physical_device, extensions, &features);
    check("a device with timeline semaphores and their descriptors is created", stage.device != VK_NULL_HANDLE);
    if (stage.device == VK_NULL_HANDLE) {
        return;
    }

    commands.get_device_queue(stage.device, 0, 0, &stage.queue);
    check_timelines(stage);
    commands.destroy_device(stage.device, nullptr);
}
