// The memory stage: memory of every type, the host-visible types mapped and shared with a second device through opaque
// descriptors and the others imported read-only, a buffer made again at a destroyed buffer's address, and GPU copies

#include "vktest.h"

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

struct memory_commands {
    PFN_vkGetPhysicalDeviceMemoryProperties         get_memory_properties;
    PFN_vkEnumerateDeviceExtensionProperties        enumerate_device_extensions;
    PFN_vkGetPhysicalDeviceExternalBufferProperties get_external_buffer_properties;
    PFN_vkGetPhysicalDeviceImageFormatProperties2   get_image_format_properties;

    PFN_vkDestroyDevice                 destroy_device;
    PFN_vkMapMemory2                    map_memory2;
    PFN_vkUnmapMemory                   unmap_memory;
    PFN_vkUnmapMemory2                  unmap_memory2;
    PFN_vkGetMemoryFdKHR                get_memory_fd;
    PFN_vkGetBufferOpaqueCaptureAddress get_buffer_opaque_capture_address;

    PFN_vkCmdCopyBuffer cmd_copy_buffer;
};

// Two devices on one GPU, the exporter allocating the memory the importer imports and copying on its transfer queue
struct memory_stage {
    memory_commands                  commands;
    work_commands                    work;
    VkInstance                       instance;
    VkPhysicalDevice                 physical_device;
    VkPhysicalDeviceMemoryProperties properties;
    uint32_t                         transfer_family;
    VkDevice                         exporter;
    VkDevice                         importer;
};

// Which memory types a check covers
enum class type_visibility {
    any,
    host_visible,
    device_only,
};

constexpr VkDeviceSize MEMORY_SIZE = 65536;
constexpr uint32_t MEMORY_WORD_COUNT = MEMORY_SIZE / sizeof(uint32_t);
constexpr uint32_t CPU_WORDS_START = 0x10000000;
constexpr uint32_t EXPORTER_WORDS_START = 0x20000000;
constexpr uint32_t IMPORTER_WORDS_START = 0x30000000;
constexpr uint32_t PLACED_WORDS_START = 0x40000000;
constexpr uint32_t COPIED_WORDS_START = 0x50000000;
constexpr uint32_t WRITER_WORDS_START = 0x60000000;

static bool is_host_visible(const VkMemoryType& type) {
    return (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}

static bool has_visibility(const VkMemoryType& type, type_visibility visibility) {
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

// Whether `holds` is true of every memory type of `visibility`, of which there must be at least one
template <typename Predicate>
static bool holds_for_every_type(const memory_stage& stage, type_visibility visibility, Predicate holds) {
    uint32_t covered_count = 0;
    bool holds_for_all = true;
    for (uint32_t type = 0; type < stage.properties.memoryTypeCount; type++) {
        if (!has_visibility(stage.properties.memoryTypes[type], visibility)) {
            continue;
        }

        covered_count++;
        holds_for_all &= holds(stage, type);
    }

    return covered_count > 0 && holds_for_all;
}

static VkResult allocate(
    const memory_stage& stage,
    VkDevice device,
    uint32_t type,
    const void* next,
    VkDeviceMemory& memory
) {
    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = next;
    allocate_info.allocationSize = MEMORY_SIZE;
    allocate_info.memoryTypeIndex = type;

    VkResult result = stage.work.allocate_memory(device, &allocate_info, nullptr, &memory);
    if (result != VK_SUCCESS) {
        memory = VK_NULL_HANDLE;
    }

    return result;
}

static void* map(const memory_stage& stage, VkDevice device, VkDeviceMemory memory) {
    void* mapping = nullptr;
    if (stage.work.map_memory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
        return nullptr;
    }

    return mapping;
}

static void write_words(void* mapping, uint32_t first_word) {
    uint32_t* words = static_cast<uint32_t*>(mapping);
    for (uint32_t i = 0; i < MEMORY_WORD_COUNT; i++) {
        words[i] = first_word + i;
    }
}

static bool holds_words(const void* mapping, uint32_t first_word) {
    const uint32_t* words = static_cast<const uint32_t*>(mapping);
    for (uint32_t i = 0; i < MEMORY_WORD_COUNT; i++) {
        if (words[i] != first_word + i) {
            return false;
        }
    }

    return true;
}

// Allocates exportable memory of `type` on the exporter into `memory` and returns an opaque descriptor of it, or -1
static int export_memory(const memory_stage& stage, uint32_t type, VkDeviceMemory& memory) {
    VkExportMemoryAllocateInfo export_info = {};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (allocate(stage, stage.exporter, type, &export_info, memory) != VK_SUCCESS) {
        return -1;
    }

    VkMemoryGetFdInfoKHR fd_info = {};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory = memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    int descriptor = -1;
    if (stage.commands.get_memory_fd(stage.exporter, &fd_info, &descriptor) != VK_SUCCESS) {
        return -1;
    }

    return descriptor;
}

// Imports `descriptor` into `memory` on `device` as memory of `type`. The import owns the descriptor once it succeeds,
// and the caller still owns it after a failure.
static VkResult import_memory(
    const memory_stage& stage,
    VkDevice device,
    uint32_t type,
    int descriptor,
    VkDeviceMemory& memory
) {
    VkImportMemoryFdInfoKHR import_info = {};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    import_info.fd = descriptor;

    return allocate(stage, device, type, &import_info, memory);
}

static bool allocates(const memory_stage& stage, uint32_t type) {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool is_allocated = allocate(stage, stage.exporter, type, nullptr, memory) == VK_SUCCESS;
    stage.work.free_memory(stage.exporter, memory, nullptr);

    return is_allocated;
}

// Whether memory of `type` still holds what the CPU wrote through one mapping when mapped again
static bool keeps_cpu_writes(const memory_stage& stage, uint32_t type) {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (allocate(stage, stage.exporter, type, nullptr, memory) != VK_SUCCESS) {
        return false;
    }

    bool keeps_writes = false;
    void* written = map(stage, stage.exporter, memory);
    if (written != nullptr) {
        write_words(written, CPU_WORDS_START + type);
        stage.commands.unmap_memory(stage.exporter, memory);

        void* reread = map(stage, stage.exporter, memory);
        keeps_writes = reread != nullptr && holds_words(reread, CPU_WORDS_START + type);
    }

    stage.work.free_memory(stage.exporter, memory, nullptr);

    return keeps_writes;
}

// Whether memory of `type` mapped at a range the program reserved takes the CPU's writes there, and an unmap with
// `VK_MEMORY_UNMAP_RESERVE_BIT_EXT` leaves the range reserved without the memory
static bool maps_at_reserved_range(const memory_stage& stage, uint32_t type) {
    void* range = mmap(nullptr, MEMORY_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (range == MAP_FAILED) {
        return false;
    }

    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool is_allocated = allocate(stage, stage.exporter, type, nullptr, memory) == VK_SUCCESS;

    VkMemoryMapPlacedInfoEXT placed_info = {};
    placed_info.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT;
    placed_info.pPlacedAddress = range;

    VkMemoryMapInfo map_info = {};
    map_info.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO;
    map_info.pNext = &placed_info;
    map_info.flags = VK_MEMORY_MAP_PLACED_BIT_EXT;
    map_info.memory = memory;
    map_info.size = VK_WHOLE_SIZE;

    void* placed = nullptr;
    bool is_placed = is_allocated && stage.commands.map_memory2(stage.exporter, &map_info, &placed) == VK_SUCCESS &&
        placed == range;
    if (is_placed) {
        write_words(placed, PLACED_WORDS_START + type);
    }

    VkMemoryUnmapInfo unmap_info = {};
    unmap_info.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO;
    unmap_info.flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT;
    unmap_info.memory = memory;
    bool is_unmapped = is_placed && stage.commands.unmap_memory2(stage.exporter, &unmap_info) == VK_SUCCESS;

    // A mapping that may not replace another fails over a range still reserved
    bool keeps_range = false;
    if (is_unmapped) {
        int no_replace = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE;
        keeps_range = mmap(range, MEMORY_SIZE, PROT_READ, no_replace, -1, 0) == MAP_FAILED && errno == EEXIST;
    }

    bool drops_memory = keeps_range && mprotect(range, MEMORY_SIZE, PROT_READ) == 0 &&
        !holds_words(range, PLACED_WORDS_START + type);

    void* reread = keeps_range ? map(stage, stage.exporter, memory) : nullptr;
    bool keeps_writes = reread != nullptr && holds_words(reread, PLACED_WORDS_START + type);

    stage.work.free_memory(stage.exporter, memory, nullptr);
    munmap(range, MEMORY_SIZE);

    return keeps_range && drops_memory && keeps_writes;
}

// Whether memory of `type` exported by one device and imported by the other shows each device's CPU writes to the
// other device's mapping
static bool shares_through_descriptor(const memory_stage& stage, uint32_t type) {
    VkDeviceMemory exported = VK_NULL_HANDLE;
    int descriptor = export_memory(stage, type, exported);

    VkDeviceMemory imported = VK_NULL_HANDLE;
    bool is_imported =
        descriptor >= 0 && import_memory(stage, stage.importer, type, descriptor, imported) == VK_SUCCESS;
    if (descriptor >= 0 && !is_imported) {
        close(descriptor);
    }

    bool shares = false;
    void* exported_mapping = is_imported ? map(stage, stage.exporter, exported) : nullptr;
    void* imported_mapping = is_imported ? map(stage, stage.importer, imported) : nullptr;
    if (exported_mapping != nullptr && imported_mapping != nullptr) {
        write_words(exported_mapping, EXPORTER_WORDS_START);
        bool shows_exporter_writes = holds_words(imported_mapping, EXPORTER_WORDS_START);

        write_words(imported_mapping, IMPORTER_WORDS_START);
        shares = shows_exporter_writes && holds_words(exported_mapping, IMPORTER_WORDS_START);
    }

    stage.work.free_memory(stage.importer, imported, nullptr);
    stage.work.free_memory(stage.exporter, exported, nullptr);

    return shares;
}

// Whether importing memory of `type` through a descriptor with `GPU_RIGHT_READ` alone returns `expected`
static bool imports_read_only_as(const memory_stage& stage, uint32_t type, VkResult expected) {
    VkDeviceMemory exported = VK_NULL_HANDLE;
    int descriptor = export_memory(stage, type, exported);
    int read_only = descriptor >= 0 ? restrict_to_read(stage.instance, stage.physical_device, descriptor) : -1;
    if (descriptor >= 0) {
        close(descriptor);
    }

    VkDeviceMemory imported = VK_NULL_HANDLE;
    bool returns_expected = false;
    if (read_only >= 0) {
        VkResult result = import_memory(stage, stage.importer, type, read_only, imported);
        returns_expected = result == expected;
        if (result != VK_SUCCESS) {
            close(read_only);
        }
    }

    stage.work.free_memory(stage.importer, imported, nullptr);
    stage.work.free_memory(stage.exporter, exported, nullptr);

    return returns_expected;
}

// The CPU could write through a mapping of a host-visible import, so only types it cannot map take one
static bool refuses_read_only_import(const memory_stage& stage, uint32_t type) {
    return imports_read_only_as(stage, type, VK_ERROR_INVALID_EXTERNAL_HANDLE);
}

static bool takes_read_only_import(const memory_stage& stage, uint32_t type) {
    return imports_read_only_as(stage, type, VK_SUCCESS);
}

// A zero `address` lets the driver place the buffer
static VkBuffer create_replayable_buffer(const memory_stage& stage, uint64_t address) {
    VkBufferOpaqueCaptureAddressCreateInfo address_info = {};
    address_info.sType = VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO;
    address_info.opaqueCaptureAddress = address;

    VkBufferCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create_info.pNext = &address_info;
    create_info.flags = VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
    create_info.size = MEMORY_SIZE;
    create_info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (stage.work.create_buffer(stage.exporter, &create_info, nullptr, &buffer) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    return buffer;
}

static uint64_t read_capture_address(const memory_stage& stage, VkBuffer buffer) {
    VkBufferDeviceAddressInfo address_info = {};
    address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address_info.buffer = buffer;

    return stage.commands.get_buffer_opaque_capture_address(stage.exporter, &address_info);
}

// Whether a buffer created at the address of a destroyed buffer binds memory there, which needs the destroyed
// buffer's GPU mapping and address range released
static bool binds_at_released_address(const memory_stage& stage) {
    VkBuffer first = create_replayable_buffer(stage, 0);
    if (first == VK_NULL_HANDLE) {
        return false;
    }

    VkMemoryRequirements requirements = {};
    stage.work.get_buffer_memory_requirements(stage.exporter, first, &requirements);

    VkMemoryAllocateFlagsInfo flags_info = {};
    flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT | VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
    bool has_type = requirements.memoryTypeBits != 0;
    uint32_t type = has_type ? __builtin_ctz(requirements.memoryTypeBits) : 0;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool has_memory = has_type && allocate(stage, stage.exporter, type, &flags_info, memory) == VK_SUCCESS;
    bool first_binds = has_memory && stage.work.bind_buffer_memory(stage.exporter, first, memory, 0) == VK_SUCCESS;
    uint64_t address = first_binds ? read_capture_address(stage, first) : 0;
    stage.work.destroy_buffer(stage.exporter, first, nullptr);

    VkBuffer second = address != 0 ? create_replayable_buffer(stage, address) : VK_NULL_HANDLE;
    bool second_binds = second != VK_NULL_HANDLE &&
        stage.work.bind_buffer_memory(stage.exporter, second, memory, 0) == VK_SUCCESS;
    bool keeps_address = second_binds && read_capture_address(stage, second) == address;
    stage.work.destroy_buffer(stage.exporter, second, nullptr);
    stage.work.free_memory(stage.exporter, memory, nullptr);

    return keeps_address;
}

// Binds a new buffer for GPU copies at the start of `memory` of `type` on `device`, memory shared through an opaque
// descriptor, or returns `VK_NULL_HANDLE`. A buffer on such memory must name that handle type.
static VkBuffer bind_shared_buffer(const memory_stage& stage, VkDevice device, uint32_t type, VkDeviceMemory memory) {
    VkExternalMemoryBufferCreateInfo external_info = {};
    external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    return bind_new_buffer(stage.work, device, memory, type, MEMORY_SIZE, TRANSFER_USAGE, &external_info);
}

// Records a copy of each buffer of `chain` into the next, each reading what the one before it wrote, the last one's
// writes visible to the CPU
static void record_copies(
    const memory_stage& stage,
    VkCommandBuffer command_buffer,
    const std::vector<VkBuffer>& chain
) {
    VkBufferCopy region = {};
    region.size = MEMORY_SIZE;
    for (size_t i = 0; i + 1 < chain.size(); i++) {
        if (i > 0) {
            VkAccessFlags access = VK_ACCESS_TRANSFER_READ_BIT;
            wait_for_transfer_writes(stage.work, command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, access);
        }

        stage.commands.cmd_copy_buffer(command_buffer, chain[i], chain[i + 1], 1, &region);
    }

    wait_for_transfer_writes(stage.work, command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
}

// Runs the copies along `chain` on `device`, which must have a transfer queue, until done, returning the first failure
static VkResult copy_on_gpu(const memory_stage& stage, VkDevice device, const std::vector<VkBuffer>& chain) {
    auto record = [&](VkCommandBuffer command_buffer) {
        record_copies(stage, command_buffer, chain);
    };

    return run_once(stage.work, device, stage.transfer_family, record);
}

// Whether GPU copies carry a host-visible buffer's words through memory of `type` into a second host-visible buffer
static bool carries_gpu_copies(const memory_stage& stage, uint32_t type) {
    bound_buffer source =
        make_staging_buffer(stage.work, stage.exporter, stage.properties, MEMORY_SIZE, TRANSFER_USAGE);
    bound_buffer middle = make_bound_buffer(stage.work, stage.exporter, type, MEMORY_SIZE, TRANSFER_USAGE);
    bound_buffer target =
        make_staging_buffer(stage.work, stage.exporter, stage.properties, MEMORY_SIZE, TRANSFER_USAGE);

    bool carries = false;
    bool is_ready = source.words != nullptr && target.words != nullptr && middle.buffer != VK_NULL_HANDLE;
    if (is_ready) {
        write_words(source.words, COPIED_WORDS_START + type);
        VkResult result = copy_on_gpu(stage, stage.exporter, {source.buffer, middle.buffer, target.buffer});
        carries = result == VK_SUCCESS && holds_words(target.words, COPIED_WORDS_START + type);
    }

    destroy_bound_buffer(stage.work, stage.exporter, target);
    destroy_bound_buffer(stage.work, stage.exporter, middle);
    destroy_bound_buffer(stage.work, stage.exporter, source);

    return carries;
}

// Whether a new device's GPU copy into `descriptor`, imported as memory of `type`, loses that device. The import owns
// the descriptor once it succeeds, and this closes it otherwise.
static bool loses_writer(const memory_stage& stage, uint32_t type, int descriptor) {
    std::vector<uint32_t> families = {stage.transfer_family};
    std::vector<const char*> extensions = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    VkDevice writer = create_device(stage.instance, stage.physical_device, families, extensions, nullptr);
    if (writer == VK_NULL_HANDLE) {
        close(descriptor);
        return false;
    }

    bound_buffer imported;
    bool is_imported = import_memory(stage, writer, type, descriptor, imported.memory) == VK_SUCCESS;
    if (!is_imported) {
        close(descriptor);
    }

    imported.buffer = is_imported ? bind_shared_buffer(stage, writer, type, imported.memory) : VK_NULL_HANDLE;
    bound_buffer source = make_staging_buffer(stage.work, writer, stage.properties, MEMORY_SIZE, TRANSFER_USAGE);

    bool is_lost = false;
    if (source.words != nullptr && imported.buffer != VK_NULL_HANDLE) {
        write_words(source.words, WRITER_WORDS_START + type);
        is_lost = copy_on_gpu(stage, writer, {source.buffer, imported.buffer}) == VK_ERROR_DEVICE_LOST;
    }

    destroy_bound_buffer(stage.work, writer, source);
    destroy_bound_buffer(stage.work, writer, imported);
    stage.commands.destroy_device(writer, nullptr);

    return is_lost;
}

// Whether a GPU copy into a read-only import of memory of `type` loses the copying device, while the exporter copies
// the memory's words out unchanged afterwards
static bool refuses_gpu_writes_to_read_only_import(const memory_stage& stage, uint32_t type) {
    bound_buffer source =
        make_staging_buffer(stage.work, stage.exporter, stage.properties, MEMORY_SIZE, TRANSFER_USAGE);
    bound_buffer target =
        make_staging_buffer(stage.work, stage.exporter, stage.properties, MEMORY_SIZE, TRANSFER_USAGE);

    bound_buffer exported;
    int descriptor = export_memory(stage, type, exported.memory);
    if (descriptor >= 0) {
        exported.buffer = bind_shared_buffer(stage, stage.exporter, type, exported.memory);
    }

    bool is_written = false;
    bool is_ready = source.words != nullptr && target.words != nullptr && exported.buffer != VK_NULL_HANDLE;
    if (is_ready) {
        write_words(source.words, EXPORTER_WORDS_START + type);
        is_written = copy_on_gpu(stage, stage.exporter, {source.buffer, exported.buffer}) == VK_SUCCESS;
    }

    int read_only = is_written ? restrict_to_read(stage.instance, stage.physical_device, descriptor) : -1;
    if (descriptor >= 0) {
        close(descriptor);
    }

    bool keeps_words = false;
    if (read_only >= 0 && loses_writer(stage, type, read_only)) {
        VkResult result = copy_on_gpu(stage, stage.exporter, {exported.buffer, target.buffer});
        keeps_words = result == VK_SUCCESS && holds_words(target.words, EXPORTER_WORDS_START + type);
    }

    destroy_bound_buffer(stage.work, stage.exporter, exported);
    destroy_bound_buffer(stage.work, stage.exporter, target);
    destroy_bound_buffer(stage.work, stage.exporter, source);

    return keeps_words;
}

// Stellux GPUs share memory through opaque descriptors alone, so NVK may offer no dma-buf extension
static bool lists_extensions_without_dma_buf(const memory_stage& stage) {
    uint32_t extension_count = 0;
    VkResult result =
        stage.commands.enumerate_device_extensions(stage.physical_device, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (result == VK_SUCCESS) {
        result = stage.commands.enumerate_device_extensions(
            stage.physical_device,
            nullptr,
            &extension_count,
            extensions.data()
        );
    }

    return result == VK_SUCCESS && extension_count > 0 &&
        !has_extension(extensions, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
}

static bool is_opaque_alone(const VkExternalMemoryProperties& properties) {
    VkExternalMemoryFeatureFlags features = properties.externalMemoryFeatures;
    bool is_exportable = (features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
    bool is_importable = (features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
    bool is_compatible_alone = properties.compatibleHandleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    return is_exportable && is_importable && is_compatible_alone;
}

// Whether the external memory answers for a buffer and a linear image make opaque descriptors exportable and
// importable, and compatible with no other handle type
static bool answers_opaque_descriptors_alone(const memory_stage& stage) {
    VkPhysicalDeviceExternalBufferInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    buffer_info.usage = TRANSFER_USAGE;
    buffer_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkExternalBufferProperties buffer_properties = {};
    buffer_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    stage.commands.get_external_buffer_properties(stage.physical_device, &buffer_info, &buffer_properties);

    VkPhysicalDeviceExternalImageFormatInfo external_info = {};
    external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkPhysicalDeviceImageFormatInfo2 image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    image_info.pNext = &external_info;

    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.type = VK_IMAGE_TYPE_2D;
    image_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkExternalImageFormatProperties external_properties = {};
    external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;

    VkImageFormatProperties2 image_properties = {};
    image_properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    image_properties.pNext = &external_properties;
    VkResult image_result =
        stage.commands.get_image_format_properties(stage.physical_device, &image_info, &image_properties);

    return is_opaque_alone(buffer_properties.externalMemoryProperties) && image_result == VK_SUCCESS &&
        is_opaque_alone(external_properties.externalMemoryProperties);
}

static void check_memory_types(const memory_stage& stage) {
    check("every memory type allocates", holds_for_every_type(stage, type_visibility::any, allocates));
    check(
        "memory of every host-visible type holds what the CPU wrote through an earlier mapping",
        holds_for_every_type(stage, type_visibility::host_visible, keeps_cpu_writes)
    );
    check(
        "memory of every host-visible type mapped at a reserved range takes the CPU's writes until a reserving unmap",
        holds_for_every_type(stage, type_visibility::host_visible, maps_at_reserved_range)
    );

    check(
        "memory of every host-visible type shared through an opaque descriptor shows each device's CPU writes",
        holds_for_every_type(stage, type_visibility::host_visible, shares_through_descriptor)
    );
    check(
        "a read-only descriptor is refused as memory of every host-visible type",
        holds_for_every_type(stage, type_visibility::host_visible, refuses_read_only_import)
    );
    check(
        "a read-only descriptor imports as memory of every type the CPU cannot map",
        holds_for_every_type(stage, type_visibility::device_only, takes_read_only_import)
    );

    check("a buffer created at a destroyed buffer's address binds memory there", binds_at_released_address(stage));

    check(
        "GPU copies carry words through memory of every type between host-visible buffers",
        holds_for_every_type(stage, type_visibility::any, carries_gpu_copies)
    );
    check(
        "a GPU copy into a read-only import of every type the CPU cannot map loses its device alone, writing nothing",
        holds_for_every_type(stage, type_visibility::device_only, refuses_gpu_writes_to_read_only_import)
    );
}

void check_memory(VkInstance instance, VkPhysicalDevice physical_device) {
    memory_stage stage = {};
    stage.instance = instance;
    stage.physical_device = physical_device;
    stage.commands = {
        .get_memory_properties = find_command<PFN_vkGetPhysicalDeviceMemoryProperties>(
            instance,
            "vkGetPhysicalDeviceMemoryProperties"
        ),
        .enumerate_device_extensions = find_command<PFN_vkEnumerateDeviceExtensionProperties>(
            instance,
            "vkEnumerateDeviceExtensionProperties"
        ),
        .get_external_buffer_properties = find_command<PFN_vkGetPhysicalDeviceExternalBufferProperties>(
            instance,
            "vkGetPhysicalDeviceExternalBufferProperties"
        ),
        .get_image_format_properties = find_command<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            instance,
            "vkGetPhysicalDeviceImageFormatProperties2"
        ),
        .destroy_device = find_command<PFN_vkDestroyDevice>(instance, "vkDestroyDevice"),
        .map_memory2 = find_command<PFN_vkMapMemory2>(instance, "vkMapMemory2"),
        .unmap_memory = find_command<PFN_vkUnmapMemory>(instance, "vkUnmapMemory"),
        .unmap_memory2 = find_command<PFN_vkUnmapMemory2>(instance, "vkUnmapMemory2"),
        .get_memory_fd = find_command<PFN_vkGetMemoryFdKHR>(instance, "vkGetMemoryFdKHR"),
        .get_buffer_opaque_capture_address = find_command<PFN_vkGetBufferOpaqueCaptureAddress>(
            instance,
            "vkGetBufferOpaqueCaptureAddress"
        ),
        .cmd_copy_buffer = find_command<PFN_vkCmdCopyBuffer>(instance, "vkCmdCopyBuffer"),
    };

    const memory_commands& commands = stage.commands;
    bool has_work_commands = find_work_commands(instance, stage.work);
    bool has_commands = commands.get_memory_properties && commands.enumerate_device_extensions &&
        commands.get_external_buffer_properties && commands.get_image_format_properties && commands.destroy_device &&
        commands.map_memory2 && commands.unmap_memory && commands.unmap_memory2 && commands.get_memory_fd &&
        commands.get_buffer_opaque_capture_address && commands.cmd_copy_buffer && has_work_commands;
    check("vkGetInstanceProcAddr finds the memory commands vktest calls", has_commands);
    if (!has_commands) {
        return;
    }

    commands.get_memory_properties(physical_device, &stage.properties);
    stage.transfer_family = find_transfer_family(instance, physical_device);
    check(
        "vkEnumerateDeviceExtensionProperties lists no VK_EXT_external_memory_dma_buf",
        lists_extensions_without_dma_buf(stage)
    );
    check(
        "buffer and linear image queries answer opaque descriptors as exportable, importable and compatible alone",
        answers_opaque_descriptors_alone(stage)
    );

    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed_features = {};
    placed_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT;
    placed_features.memoryMapPlaced = VK_TRUE;
    placed_features.memoryUnmapReserve = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features.pNext = &placed_features;
    features.bufferDeviceAddress = VK_TRUE;
    features.bufferDeviceAddressCaptureReplay = VK_TRUE;
    std::vector<const char*> extensions = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME,
    };

    std::vector<uint32_t> exporter_families = {GENERAL_FAMILY, stage.transfer_family};
    std::vector<uint32_t> importer_families = {GENERAL_FAMILY};
    if (stage.transfer_family != UINT32_MAX) {
        stage.exporter = create_device(instance, physical_device, exporter_families, extensions, &features);
        stage.importer = create_device(instance, physical_device, importer_families, extensions, &features);
    }

    bool has_devices = stage.exporter != VK_NULL_HANDLE && stage.importer != VK_NULL_HANDLE;
    check(
        "two devices with memory descriptors, replayable addresses, placed maps and one transfer queue are created",
        has_devices
    );
    if (has_devices) {
        check_memory_types(stage);
    }

    commands.destroy_device(stage.importer, nullptr);
    commands.destroy_device(stage.exporter, nullptr);
}
