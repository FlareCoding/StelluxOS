// The compute stage: pipelines NVK's shader compiler, NAK, compiles from compute.spvasm and read.spvasm, dispatched on
// the general queue into memory of the staging type, whose words the CPU reads. The second reads read-only imports.

#include "compute_spirv.h"
#include "read_spirv.h"
#include "vktest.h"

#include <algorithm>
#include <stdint.h>
#include <unistd.h>
#include <vector>

struct compute_commands {
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    PFN_vkDestroyDevice                     destroy_device;
    PFN_vkCreateShaderModule                create_shader_module;
    PFN_vkDestroyShaderModule               destroy_shader_module;
    PFN_vkCreateDescriptorSetLayout         create_descriptor_set_layout;
    PFN_vkDestroyDescriptorSetLayout        destroy_descriptor_set_layout;
    PFN_vkCreatePipelineLayout              create_pipeline_layout;
    PFN_vkDestroyPipelineLayout             destroy_pipeline_layout;
    PFN_vkCreateComputePipelines            create_compute_pipelines;
    PFN_vkDestroyPipeline                   destroy_pipeline;
    PFN_vkCreateDescriptorPool              create_descriptor_pool;
    PFN_vkDestroyDescriptorPool             destroy_descriptor_pool;
    PFN_vkAllocateDescriptorSets            allocate_descriptor_sets;
    PFN_vkUpdateDescriptorSets              update_descriptor_sets;

    PFN_vkCmdBindPipeline       cmd_bind_pipeline;
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets;
    PFN_vkCmdPushConstants      cmd_push_constants;
    PFN_vkCmdDispatch           cmd_dispatch;
};

// A device with a queue of the general family alone and memory descriptors, whose dispatches the CPU reads through
// memory of the staging type
struct compute_stage {
    compute_commands                 commands;
    work_commands                    work;
    VkInstance                       instance;
    VkPhysicalDevice                 physical_device;
    VkPhysicalDeviceMemoryProperties properties;
    VkDevice                         device;
};

// A shader's SPIR-V with the count of storage buffers it uses at set 0 from binding 0 and the bytes of push constants
// it reads
struct compute_shader {
    const uint32_t* code;
    size_t          code_size;
    uint32_t        buffer_count;
    uint32_t        push_size;
};

// A pipeline with the layouts it is created with, each handle null when its creation failed or never ran
struct compute_pipeline {
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout      layout = VK_NULL_HANDLE;
    VkPipeline            pipeline = VK_NULL_HANDLE;
};

// A dispatch of read.spvasm copying one import into memory of the staging type, run when its submission completed
struct read_dispatch {
    bool is_run = false;
    bool reads_import = false;
};

constexpr compute_shader COMPUTE_SHADER = {COMPUTE_SPIRV, sizeof(COMPUTE_SPIRV), 1, sizeof(uint32_t)};
constexpr compute_shader READ_SHADER = {READ_SPIRV, sizeof(READ_SPIRV), 2, 0};

// Must match the `LocalSize` of compute.spvasm and read.spvasm
constexpr uint32_t LOCAL_SIZE = 64;

constexpr uint32_t GROUP_COUNT = 64;
constexpr uint32_t INVOCATION_COUNT = LOCAL_SIZE * GROUP_COUNT;

constexpr VkDeviceSize BUFFER_SIZE = 65536;
constexpr uint32_t BUFFER_WORD_COUNT = BUFFER_SIZE / sizeof(uint32_t);
constexpr uint32_t READ_GROUP_COUNT = BUFFER_WORD_COUNT / LOCAL_SIZE;

constexpr uint32_t DISPATCH_WORDS_START = 0x74000000;
constexpr uint32_t CPU_WORD = 0x75000000;
constexpr uint32_t READ_IMPORT_WORDS_START = 0x76000000;
constexpr uint32_t READ_SHARED_WORDS_START = 0x77000000;

// Creates the pipeline of `shader` with the storage buffers and push constants it uses
static compute_pipeline create_pipeline(const compute_stage& stage, const compute_shader& shader) {
    const compute_commands& commands = stage.commands;

    std::vector<VkDescriptorSetLayoutBinding> bindings(shader.buffer_count);
    for (uint32_t i = 0; i < shader.buffer_count; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo set_info = {};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = shader.buffer_count;
    set_info.pBindings = bindings.data();

    compute_pipeline created;
    if (commands.create_descriptor_set_layout(stage.device, &set_info, nullptr, &created.set_layout) != VK_SUCCESS) {
        created.set_layout = VK_NULL_HANDLE;
        return created;
    }

    VkPushConstantRange push_range = {};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.size = shader.push_size;

    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &created.set_layout;
    layout_info.pushConstantRangeCount = shader.push_size > 0 ? 1 : 0;
    layout_info.pPushConstantRanges = &push_range;

    if (commands.create_pipeline_layout(stage.device, &layout_info, nullptr, &created.layout) != VK_SUCCESS) {
        created.layout = VK_NULL_HANDLE;
        return created;
    }

    VkShaderModuleCreateInfo module_info = {};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = shader.code_size;
    module_info.pCode = shader.code;

    VkShaderModule module = VK_NULL_HANDLE;
    if (commands.create_shader_module(stage.device, &module_info, nullptr, &module) != VK_SUCCESS) {
        return created;
    }

    VkComputePipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.layout = created.layout;

    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";

    commands.create_compute_pipelines(stage.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &created.pipeline);
    commands.destroy_shader_module(stage.device, module, nullptr);

    return created;
}

static void destroy_pipeline(const compute_stage& stage, const compute_pipeline& created) {
    stage.commands.destroy_pipeline(stage.device, created.pipeline, nullptr);
    stage.commands.destroy_pipeline_layout(stage.device, created.layout, nullptr);
    stage.commands.destroy_descriptor_set_layout(stage.device, created.set_layout, nullptr);
}

static void write_storage_descriptor(
    const compute_stage& stage,
    VkDescriptorSet set,
    uint32_t binding,
    VkBuffer buffer
) {
    VkDescriptorBufferInfo buffer_info = {};
    buffer_info.buffer = buffer;
    buffer_info.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;

    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;

    stage.commands.update_descriptor_sets(stage.device, 1, &write, 0, nullptr);
}

static void record_dispatch(
    const compute_stage& stage,
    const compute_pipeline& pipeline,
    VkDescriptorSet set,
    const std::vector<uint32_t>& push_words,
    uint32_t group_count,
    VkCommandBuffer command_buffer
) {
    const compute_commands& commands = stage.commands;

    commands.cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
    commands.cmd_bind_descriptor_sets(
        command_buffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pipeline.layout,
        0,
        1,
        &set,
        0,
        nullptr
    );
    if (!push_words.empty()) {
        commands.cmd_push_constants(
            command_buffer,
            pipeline.layout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            static_cast<uint32_t>(push_words.size() * sizeof(uint32_t)),
            push_words.data()
        );
    }

    commands.cmd_dispatch(command_buffer, group_count, 1, 1);

    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    stage.work.cmd_pipeline_barrier(
        command_buffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        1,
        &barrier,
        0,
        nullptr,
        0,
        nullptr
    );
}

// Runs a dispatch of `group_count` groups on the general queue until it ends, `buffers` bound at set 0 from binding 0
// and `push_words` pushed, returning the first failure
static VkResult dispatch_on_gpu(
    const compute_stage& stage,
    const compute_pipeline& pipeline,
    const std::vector<VkBuffer>& buffers,
    const std::vector<uint32_t>& push_words,
    uint32_t group_count
) {
    const compute_commands& commands = stage.commands;

    VkDescriptorPoolSize pool_size = {};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = static_cast<uint32_t>(buffers.size());

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult result = commands.create_descriptor_pool(stage.device, &pool_info, nullptr, &pool);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkDescriptorSetAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = pool;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &pipeline.set_layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    result = commands.allocate_descriptor_sets(stage.device, &allocate_info, &set);
    if (result == VK_SUCCESS) {
        for (uint32_t binding = 0; binding < buffers.size(); binding++) {
            write_storage_descriptor(stage, set, binding, buffers[binding]);
        }

        auto record = [&](VkCommandBuffer command_buffer) {
            record_dispatch(stage, pipeline, set, push_words, group_count, command_buffer);
        };

        result = run_once(stage.work, stage.device, GENERAL_FAMILY, record);
    }

    commands.destroy_descriptor_pool(stage.device, pool, nullptr);

    return result;
}

static bool holds_dispatch_words(const uint32_t* words) {
    for (uint32_t i = 0; i < BUFFER_WORD_COUNT; i++) {
        uint32_t expected = i < INVOCATION_COUNT ? DISPATCH_WORDS_START + i : CPU_WORD;
        if (words[i] != expected) {
            return false;
        }
    }

    return true;
}

static void check_dispatch(const compute_stage& stage, const compute_pipeline& pipeline) {
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bound_buffer target = make_staging_buffer(stage.work, stage.device, stage.properties, BUFFER_SIZE, usage);

    bool is_run = false;
    if (target.words != nullptr) {
        std::fill_n(target.words, BUFFER_WORD_COUNT, CPU_WORD);
        is_run = dispatch_on_gpu(stage, pipeline, {target.buffer}, {DISPATCH_WORDS_START}, GROUP_COUNT) == VK_SUCCESS;
    }

    check("a submission of a dispatch of 64 groups of 64 invocations completes on the general queue", is_run);
    check(
        "each invocation of the dispatch writes its word, and the words past the dispatch keep the CPU's",
        is_run && holds_dispatch_words(target.words)
    );

    destroy_bound_buffer(stage.work, stage.device, target);
}

// Imports read-only `descriptor`, holding words from `first_word`, as memory of `type` and dispatches a copy of it into
// memory of the staging type. The import owns the descriptor once it succeeds, and this closes it otherwise.
static read_dispatch dispatch_read_of_import(
    const compute_stage& stage,
    const compute_pipeline& pipeline,
    uint32_t type,
    int descriptor,
    uint32_t first_word
) {
    bound_buffer imported;
    bool is_imported =
        import_memory(stage.work, stage.device, type, BUFFER_SIZE, descriptor, imported.memory) == VK_SUCCESS;
    if (!is_imported) {
        close(descriptor);
    }

    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (is_imported) {
        imported.buffer = bind_shared_buffer(stage.work, stage.device, imported.memory, type, BUFFER_SIZE, usage);
    }

    bound_buffer target = make_staging_buffer(stage.work, stage.device, stage.properties, BUFFER_SIZE, usage);

    read_dispatch dispatch;
    if (imported.buffer != VK_NULL_HANDLE && target.words != nullptr) {
        VkResult result = dispatch_on_gpu(stage, pipeline, {target.buffer, imported.buffer}, {}, READ_GROUP_COUNT);
        dispatch.is_run = result == VK_SUCCESS;
        dispatch.reads_import = dispatch.is_run && holds_words(target.words, BUFFER_SIZE, first_word);
    }

    destroy_bound_buffer(stage.work, stage.device, target);
    destroy_bound_buffer(stage.work, stage.device, imported);

    return dispatch;
}

// Dispatches a copy of memory of `type` the CPU wrote through its mapping, imported read-only on the device that
// exported it
static read_dispatch dispatch_read_of_read_only_import(
    const compute_stage& stage,
    const compute_pipeline& pipeline,
    uint32_t type
) {
    VkDeviceMemory exported = VK_NULL_HANDLE;
    int descriptor = export_memory(stage.work, stage.device, type, BUFFER_SIZE, exported);

    uint32_t first_word = READ_IMPORT_WORDS_START + type;
    void* mapping = nullptr;
    bool is_mapped = descriptor >= 0 &&
        stage.work.map_memory(stage.device, exported, 0, VK_WHOLE_SIZE, 0, &mapping) == VK_SUCCESS;
    if (is_mapped) {
        write_words(mapping, BUFFER_SIZE, first_word);
    }

    int read_only = is_mapped ? restrict_to_read(stage.instance, stage.physical_device, descriptor) : -1;
    if (descriptor >= 0) {
        close(descriptor);
    }

    read_dispatch dispatch;
    if (read_only >= 0) {
        dispatch = dispatch_read_of_import(stage, pipeline, type, read_only, first_word);
    }

    stage.work.free_memory(stage.device, exported, nullptr);

    return dispatch;
}

// Dispatches a copy of shared memory the CPU wrote, imported read-only as the system memory type
static read_dispatch dispatch_read_of_read_only_shared_memory(
    const compute_stage& stage,
    const compute_pipeline& pipeline
) {
    uint32_t type = find_system_type(stage.properties);
    if (type == UINT32_MAX) {
        return {};
    }

    int read_only =
        make_read_only_shared_memory(stage.instance, stage.physical_device, BUFFER_SIZE, READ_SHARED_WORDS_START);
    if (read_only < 0) {
        return {};
    }

    return dispatch_read_of_import(stage, pipeline, type, read_only, READ_SHARED_WORDS_START);
}

static void check_dispatch_reads(const compute_stage& stage, const compute_pipeline& pipeline) {
    bool reads_every_type = true;
    auto is_dispatch_run = [&](const compute_stage& reader, uint32_t type) {
        read_dispatch dispatch = dispatch_read_of_read_only_import(reader, pipeline, type);
        reads_every_type &= dispatch.reads_import;
        return dispatch.is_run;
    };

    read_dispatch shared_memory_dispatch = dispatch_read_of_read_only_shared_memory(stage, pipeline);
    bool runs_every_type = holds_for_every_type(stage, type_visibility::host_visible, is_dispatch_run);
    check(
        "submissions of dispatches reading read-only imports of shared memory and of every host-visible type complete",
        shared_memory_dispatch.is_run && runs_every_type
    );
    check(
        "shared memory and memory of every host-visible type imported read-only are each read by a dispatch",
        shared_memory_dispatch.reads_import && runs_every_type && reads_every_type
    );
}

void check_compute(VkInstance instance, VkPhysicalDevice physical_device) {
    compute_stage stage = {};
    stage.commands = {
        .get_memory_properties = find_command<PFN_vkGetPhysicalDeviceMemoryProperties>(
            instance,
            "vkGetPhysicalDeviceMemoryProperties"
        ),
        .destroy_device = find_command<PFN_vkDestroyDevice>(instance, "vkDestroyDevice"),
        .create_shader_module = find_command<PFN_vkCreateShaderModule>(instance, "vkCreateShaderModule"),
        .destroy_shader_module = find_command<PFN_vkDestroyShaderModule>(instance, "vkDestroyShaderModule"),
        .create_descriptor_set_layout = find_command<PFN_vkCreateDescriptorSetLayout>(
            instance,
            "vkCreateDescriptorSetLayout"
        ),
        .destroy_descriptor_set_layout = find_command<PFN_vkDestroyDescriptorSetLayout>(
            instance,
            "vkDestroyDescriptorSetLayout"
        ),
        .create_pipeline_layout = find_command<PFN_vkCreatePipelineLayout>(instance, "vkCreatePipelineLayout"),
        .destroy_pipeline_layout = find_command<PFN_vkDestroyPipelineLayout>(instance, "vkDestroyPipelineLayout"),
        .create_compute_pipelines = find_command<PFN_vkCreateComputePipelines>(instance, "vkCreateComputePipelines"),
        .destroy_pipeline = find_command<PFN_vkDestroyPipeline>(instance, "vkDestroyPipeline"),
        .create_descriptor_pool = find_command<PFN_vkCreateDescriptorPool>(instance, "vkCreateDescriptorPool"),
        .destroy_descriptor_pool = find_command<PFN_vkDestroyDescriptorPool>(instance, "vkDestroyDescriptorPool"),
        .allocate_descriptor_sets = find_command<PFN_vkAllocateDescriptorSets>(instance, "vkAllocateDescriptorSets"),
        .update_descriptor_sets = find_command<PFN_vkUpdateDescriptorSets>(instance, "vkUpdateDescriptorSets"),
        .cmd_bind_pipeline = find_command<PFN_vkCmdBindPipeline>(instance, "vkCmdBindPipeline"),
        .cmd_bind_descriptor_sets = find_command<PFN_vkCmdBindDescriptorSets>(instance, "vkCmdBindDescriptorSets"),
        .cmd_push_constants = find_command<PFN_vkCmdPushConstants>(instance, "vkCmdPushConstants"),
        .cmd_dispatch = find_command<PFN_vkCmdDispatch>(instance, "vkCmdDispatch"),
    };

    const compute_commands& commands = stage.commands;
    bool has_work_commands = find_work_commands(instance, stage.work);
    bool has_commands = commands.get_memory_properties && commands.destroy_device && commands.create_shader_module &&
        commands.destroy_shader_module && commands.create_descriptor_set_layout &&
        commands.destroy_descriptor_set_layout && commands.create_pipeline_layout && commands.destroy_pipeline_layout &&
        commands.create_compute_pipelines && commands.destroy_pipeline && commands.create_descriptor_pool &&
        commands.destroy_descriptor_pool && commands.allocate_descriptor_sets && commands.update_descriptor_sets &&
        commands.cmd_bind_pipeline && commands.cmd_bind_descriptor_sets && commands.cmd_push_constants &&
        commands.cmd_dispatch && has_work_commands;
    check("vkGetInstanceProcAddr finds the compute commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    stage.instance = instance;
    stage.physical_device = physical_device;
    commands.get_memory_properties(physical_device, &stage.properties);

    std::vector<const char*> extensions = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    stage.device = create_device(instance, physical_device, {GENERAL_FAMILY}, extensions, nullptr);

    bool has_device = stage.device != VK_NULL_HANDLE;
    check("a device with a queue of the general family alone and memory descriptors is created", has_device);
    if (!has_device) {
        return;
    }

    compute_pipeline pipeline = create_pipeline(stage, COMPUTE_SHADER);
    bool has_pipeline = pipeline.pipeline != VK_NULL_HANDLE;
    check("a compute pipeline is created from compute.spvasm, NAK compiling its shader", has_pipeline);
    if (has_pipeline) {
        check_dispatch(stage, pipeline);
    }

    compute_pipeline read_pipeline = create_pipeline(stage, READ_SHADER);
    bool has_read_pipeline = read_pipeline.pipeline != VK_NULL_HANDLE;
    check("a compute pipeline is created from read.spvasm, NAK compiling its shader", has_read_pipeline);
    if (has_read_pipeline) {
        check_dispatch_reads(stage, read_pipeline);
    }

    destroy_pipeline(stage, read_pipeline);
    destroy_pipeline(stage, pipeline);
    commands.destroy_device(stage.device, nullptr);
}
