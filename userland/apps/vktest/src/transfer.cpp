// The transfer stage: fills on a queue with transfer work alone, which NVK runs on the GPU's copy engine, each read
// back by the CPU through memory of the staging type

#include "vktest.h"

#include <stdint.h>

struct transfer_commands {
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    PFN_vkDestroyDevice                     destroy_device;

    PFN_vkCmdFillBuffer cmd_fill_buffer;
    PFN_vkCmdCopyBuffer cmd_copy_buffer;
};

// A device with a transfer queue alone, whose fills the CPU reads through memory of the staging type
struct transfer_stage {
    transfer_commands                commands;
    work_commands                    work;
    VkPhysicalDeviceMemoryProperties properties;
    uint32_t                         transfer_family;
    VkDevice                         device;
};

constexpr VkDeviceSize BUFFER_SIZE = 65536;
constexpr uint32_t BUFFER_WORD_COUNT = BUFFER_SIZE / sizeof(uint32_t);

// NVK fills in lines of 128 KiB, so 1 MiB plus 4 KiB takes one launch of 8 lines and one of the rest
constexpr VkDeviceSize LARGE_BUFFER_SIZE = 1052672;
constexpr uint32_t LARGE_BUFFER_WORD_COUNT = LARGE_BUFFER_SIZE / sizeof(uint32_t);

// A range across two page boundaries
constexpr VkDeviceSize RANGE_OFFSET = 4092;
constexpr VkDeviceSize RANGE_SIZE = 4104;

constexpr uint32_t WHOLE_FILL_WORD = 0x70000000;
constexpr uint32_t RANGE_FILL_WORD = 0x71000000;
constexpr uint32_t LARGE_FILL_WORD = 0x72000000;
constexpr uint32_t RANGE_CPU_WORDS_START = 0x73000000;

static void write_counting_words(uint32_t* words, uint32_t count, uint32_t first_word) {
    for (uint32_t i = 0; i < count; i++) {
        words[i] = first_word + i;
    }
}

static bool holds_fill(const uint32_t* words, uint32_t count, uint32_t word) {
    for (uint32_t i = 0; i < count; i++) {
        if (words[i] != word) {
            return false;
        }
    }

    return true;
}

// Runs a fill of `size` bytes of `buffer` from `offset` with `word` on the transfer queue until it ends, its writes
// visible to the CPU, returning the first failure
static VkResult fill_on_gpu(
    const transfer_stage& stage,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkDeviceSize size,
    uint32_t word
) {
    auto record = [&](VkCommandBuffer command_buffer) {
        stage.commands.cmd_fill_buffer(command_buffer, buffer, offset, size, word);
        wait_for_transfer_writes(stage.work, command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    };

    return run_once(stage.work, stage.device, stage.transfer_family, record);
}

static void record_fill_and_copy(
    const transfer_stage& stage,
    VkCommandBuffer command_buffer,
    VkBuffer filled,
    VkBuffer staging,
    uint32_t word
) {
    VkBufferCopy region = {};
    region.size = BUFFER_SIZE;

    stage.commands.cmd_fill_buffer(command_buffer, filled, 0, VK_WHOLE_SIZE, word);
    wait_for_transfer_writes(stage.work, command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    stage.commands.cmd_copy_buffer(command_buffer, filled, staging, 1, &region);
    wait_for_transfer_writes(stage.work, command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
}

// Whether a fill of a whole buffer on memory of `type`, copied into memory of the staging type, gives the CPU its word
static bool fills_whole_buffer(const transfer_stage& stage, uint32_t type) {
    bound_buffer filled = make_bound_buffer(stage.work, stage.device, type, BUFFER_SIZE, TRANSFER_USAGE);
    bound_buffer staging = make_staging_buffer(stage.work, stage.device, stage.properties, BUFFER_SIZE, TRANSFER_USAGE);

    bool holds_word = false;
    if (filled.buffer != VK_NULL_HANDLE && staging.words != nullptr) {
        uint32_t word = WHOLE_FILL_WORD + type;
        auto record = [&](VkCommandBuffer command_buffer) {
            record_fill_and_copy(stage, command_buffer, filled.buffer, staging.buffer, word);
        };

        bool is_run = run_once(stage.work, stage.device, stage.transfer_family, record) == VK_SUCCESS;
        holds_word = is_run && holds_fill(staging.words, BUFFER_WORD_COUNT, word);
    }

    destroy_bound_buffer(stage.work, stage.device, staging);
    destroy_bound_buffer(stage.work, stage.device, filled);

    return holds_word;
}

static bool fills_every_type(const transfer_stage& stage) {
    bool fills_all = stage.properties.memoryTypeCount > 0;
    for (uint32_t type = 0; type < stage.properties.memoryTypeCount; type++) {
        fills_all &= fills_whole_buffer(stage, type);
    }

    return fills_all;
}

static bool holds_range_fill(const uint32_t* words) {
    uint32_t first_filled = RANGE_OFFSET / sizeof(uint32_t);
    uint32_t end_filled = (RANGE_OFFSET + RANGE_SIZE) / sizeof(uint32_t);
    for (uint32_t i = 0; i < BUFFER_WORD_COUNT; i++) {
        bool is_filled = i >= first_filled && i < end_filled;
        uint32_t expected = is_filled ? RANGE_FILL_WORD : RANGE_CPU_WORDS_START + i;
        if (words[i] != expected) {
            return false;
        }
    }

    return true;
}

// Whether a fill of a range of a buffer the CPU wrote gives it the fill's word in the range and the CPU's words around
static bool fills_range_alone(const transfer_stage& stage) {
    bound_buffer staging = make_staging_buffer(stage.work, stage.device, stage.properties, BUFFER_SIZE, TRANSFER_USAGE);

    bool fills_range = false;
    if (staging.words != nullptr) {
        write_counting_words(staging.words, BUFFER_WORD_COUNT, RANGE_CPU_WORDS_START);
        VkResult result = fill_on_gpu(stage, staging.buffer, RANGE_OFFSET, RANGE_SIZE, RANGE_FILL_WORD);
        fills_range = result == VK_SUCCESS && holds_range_fill(staging.words);
    }

    destroy_bound_buffer(stage.work, stage.device, staging);

    return fills_range;
}

static bool fills_large_buffer(const transfer_stage& stage) {
    bound_buffer staging =
        make_staging_buffer(stage.work, stage.device, stage.properties, LARGE_BUFFER_SIZE, TRANSFER_USAGE);

    bool holds_word = false;
    if (staging.words != nullptr) {
        VkResult result = fill_on_gpu(stage, staging.buffer, 0, VK_WHOLE_SIZE, LARGE_FILL_WORD);
        holds_word = result == VK_SUCCESS && holds_fill(staging.words, LARGE_BUFFER_WORD_COUNT, LARGE_FILL_WORD);
    }

    destroy_bound_buffer(stage.work, stage.device, staging);

    return holds_word;
}

void check_transfer(VkInstance instance, VkPhysicalDevice physical_device) {
    transfer_stage stage = {};
    stage.commands = {
        .get_memory_properties = find_command<PFN_vkGetPhysicalDeviceMemoryProperties>(
            instance,
            "vkGetPhysicalDeviceMemoryProperties"
        ),
        .destroy_device = find_command<PFN_vkDestroyDevice>(instance, "vkDestroyDevice"),
        .cmd_fill_buffer = find_command<PFN_vkCmdFillBuffer>(instance, "vkCmdFillBuffer"),
        .cmd_copy_buffer = find_command<PFN_vkCmdCopyBuffer>(instance, "vkCmdCopyBuffer"),
    };

    const transfer_commands& commands = stage.commands;
    bool has_work_commands = find_work_commands(instance, stage.work);
    bool has_commands = commands.get_memory_properties && commands.destroy_device && commands.cmd_fill_buffer &&
        commands.cmd_copy_buffer && has_work_commands;
    check("vkGetInstanceProcAddr finds the transfer commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    commands.get_memory_properties(physical_device, &stage.properties);
    stage.transfer_family = find_transfer_family(instance, physical_device);
    if (stage.transfer_family != UINT32_MAX) {
        stage.device = create_device(instance, physical_device, {stage.transfer_family}, {}, nullptr);
    }

    bool has_device = stage.device != VK_NULL_HANDLE;
    check("a device with a transfer queue alone is created", has_device);
    if (!has_device) {
        return;
    }

    check(
        "fills on the transfer queue write their word over whole buffers on memory of every type",
        fills_every_type(stage)
    );
    check(
        "a fill of a range on the transfer queue writes its word there and leaves the words around it",
        fills_range_alone(stage)
    );
    check("a fill of 1 MiB plus 4 KiB on the transfer queue writes its word over all of it", fills_large_buffer(stage));

    commands.destroy_device(stage.device, nullptr);
}
