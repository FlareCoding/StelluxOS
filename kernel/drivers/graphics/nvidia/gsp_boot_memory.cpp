// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/gsp_boot_memory.h"
#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"

namespace drivers::nvidia {

// The firmware is copied in runs of at most 2 MiB, since its tens of MiB may not be free as one contiguous run
constexpr uint64_t FIRMWARE_RUN_BYTES = 0x200000;

// The table is sized for the firmware's pages but lists the runs' allocated pages, which agree only while every run
// but the last fills whole table pages and the DMA layer allocates pages of the table's size
static_assert(FIRMWARE_RUN_BYTES % RADIX3_PAGE_BYTES == 0);
static_assert(pmm::PAGE_SIZE == RADIX3_PAGE_BYTES);

// The most data pages a radix3 page table maps, the entries of a full level 2 under a level 0 of one page
constexpr uint64_t RADIX3_MAX_DATA_PAGES = RADIX3_PAGE_ENTRIES * RADIX3_PAGE_ENTRIES * RADIX3_PAGE_ENTRIES;

// What protected memory's metadata starts with, and the revision of its layout
constexpr uint64_t PROTECTED_METADATA_MAGIC    = 0xDC3AAE21371A60B3;
constexpr uint64_t PROTECTED_METADATA_REVISION = 1;

// NVIDIA's driver aligns the signature's memory to the 256 bytes booter load's DMA needs, and the metadata gives the
// signature's size rounded up to a whole number of them
constexpr uint64_t SIGNATURE_BLOCK_BYTES = 256;

// NVIDIA's size for each log buffer in its release build. A buffer starts with the firmware's write position, 8
// bytes, then lists the address of each of its pages
constexpr uint64_t LOG_BUFFER_BYTES = 0x10000;
constexpr uint64_t LOG_BUFFER_PAGES = LOG_BUFFER_BYTES / pmm::PAGE_SIZE;

// NVIDIA's names for the log buffers in LibOS's arguments, the init task's first, as the firmware's earliest log
// lines need, and for the resource manager's arguments
constexpr const char* LOG_BUFFER_NAMES[FIRMWARE_LOG_COUNT] = {"LOGINIT", "LOGINTR", "LOGRM"};
constexpr const char* RESOURCE_MANAGER_ARGUMENTS_NAME = "RMARGS";

// NVIDIA's driver gives LibOS's arguments and the resource manager's arguments a page each
constexpr uint64_t LIBOS_ARGUMENTS_BYTES            = 0x1000;
constexpr uint64_t RESOURCE_MANAGER_ARGUMENTS_BYTES = 0x1000;

// What LibOS's arguments say of each region they list: one contiguous range, in system memory
constexpr uint8_t LIBOS_REGION_CONTIGUOUS       = 1;
constexpr uint8_t LIBOS_REGION_IN_SYSTEM_MEMORY = 1;

namespace {

// Protected memory's metadata in the order NVIDIA's driver lays it out. Offsets count bytes from the start of video
// memory unless their comment says otherwise, and each address is a physical address the card reaches in system memory
struct protected_metadata {
    uint64_t magic;
    uint64_t revision;
    uint64_t radix3_address;
    uint64_t firmware_bytes;
    uint64_t bootloader_address;
    uint64_t bootloader_bytes;
    uint64_t bootloader_code_offset;               // the monitor's code, from the start of the bootloader's program
    uint64_t bootloader_data_offset;               // the monitor's data, from the start of the bootloader's program
    uint64_t bootloader_manifest_offset;           // from the start of the bootloader's program
    uint64_t signature_address;
    uint64_t signature_bytes;                      // of the signature's memory, a whole number of 256-byte blocks
    uint64_t reserved_start;                       // from here to the end of video memory is reserved for the firmware
    uint64_t unprotected_heap_offset;
    uint64_t unprotected_heap_bytes;
    uint64_t protected_start;
    uint64_t heap_offset;
    uint64_t heap_bytes;
    uint64_t firmware_offset;
    uint64_t bootloader_offset;
    uint64_t frts_offset;
    uint64_t frts_bytes;
    uint64_t protected_end;
    uint64_t video_memory_size;
    uint64_t vga_workspace_offset;
    uint64_t vga_workspace_bytes;
    uint64_t boot_count;                           // decides if the firmware's image is loaded, 0 on a first attempt
    uint8_t  partition_or_crash_report_fields[32]; // for a partition bootstrapper, a resume or a crash report queue
    uint8_t  partition_count;                      // virtual GPU partitions taking parts of the firmware's heap
    uint8_t  flags;                                // boot options, such as a margin past protected memory
    uint8_t  padding[6];
    uint64_t verified;                             // set by booter load to let the GSP boot, checked by the bootloader
};

// One region of LibOS's arguments, as NVIDIA's driver lays it out
struct libos_region {
    uint64_t id;         // NVIDIA's name for the region, as `libos_region_id` packs it
    uint64_t address;    // where the card reaches the region's first byte
    uint64_t bytes;
    uint8_t  kind;
    uint8_t  location;
    uint8_t  padding[6];
};

// The resource manager's arguments in the order NVIDIA's driver lays them out. Queue offsets count bytes from the
// start of the message queues' memory
struct resource_manager_arguments {
    uint64_t queue_memory_address;           // the memory's first page, which holds its page table
    uint32_t queue_page_table_entries;
    uint32_t padding;
    uint64_t command_queue_offset;
    uint64_t status_queue_offset;
    uint64_t interrupt_command_queue_offset; // NVIDIA's lockless queues for the firmware's interrupt task
    uint64_t interrupt_status_queue_offset;
    uint32_t power_transition_old_level;     // set only when the GSP boots during a power transition
    uint32_t power_transition_flags;
    uint8_t  in_power_transition;
    uint8_t  power_transition_padding[3];
    uint32_t gpu_instance;
    uint64_t profiler_address;               // a buffer for the firmware's profiler
    uint64_t profiler_bytes;
};

} // namespace

static_assert(sizeof(protected_metadata) == PROTECTED_METADATA_BYTES, "the metadata fills the room the layout leaves");
static_assert(sizeof(libos_region) == 32, "alignment pads each of LibOS's regions to 32 bytes, as in NVIDIA's driver");
static_assert(sizeof(resource_manager_arguments) == 80, "the resource manager's arguments take NVIDIA's 80 bytes");

// NVIDIA's id for a region of LibOS's arguments: up to eight characters of its name, the first in the highest byte used
static constexpr uint64_t libos_region_id(const char* name) {
    uint64_t id = 0;
    for (size_t i = 0; i < sizeof(id) && name[i] != '\0'; i++) {
        id = id << 8 | static_cast<uint8_t>(name[i]);
    }

    return id;
}

static_assert(
    libos_region_id("LONGER_NAME") == 0x4C4F4E4745525F4E,
    "an id keeps a name's first eight characters, as NVIDIA's driver packs it"
);

// The pages that hold `bytes` bytes, also when rounding `bytes` up would overflow
static uint64_t pages_for_bytes(uint64_t bytes) {
    return bytes / RADIX3_PAGE_BYTES + (bytes % RADIX3_PAGE_BYTES != 0);
}

// The pages of a radix3 level that hold `entries` addresses
static uint64_t pages_for_entries(uint64_t entries) {
    return entries / RADIX3_PAGE_ENTRIES + (entries % RADIX3_PAGE_ENTRIES != 0);
}

uint64_t radix3_table_pages(uint64_t data_pages) {
    if (data_pages == 0 || data_pages > RADIX3_MAX_DATA_PAGES) {
        return 0;
    }

    uint64_t level_2_pages = pages_for_entries(data_pages);
    uint64_t level_1_pages = pages_for_entries(level_2_pages);

    return 1 + level_1_pages + level_2_pages;
}

void write_radix3_table(uint64_t* table, uint64_t table_address, const dma::buffer* runs, size_t run_count) {
    uint64_t data_pages = 0;
    for (size_t i = 0; i < run_count; i++) {
        data_pages += runs[i].size / RADIX3_PAGE_BYTES;
    }

    uint64_t level_2_pages = pages_for_entries(data_pages);
    uint64_t level_1_pages = pages_for_entries(level_2_pages);
    uint64_t* level_1 = table + RADIX3_PAGE_ENTRIES;
    uint64_t* level_2 = level_1 + level_1_pages * RADIX3_PAGE_ENTRIES;
    uint64_t level_1_address = table_address + RADIX3_PAGE_BYTES;
    uint64_t level_2_address = level_1_address + level_1_pages * RADIX3_PAGE_BYTES;

    for (uint64_t i = 0; i < level_1_pages; i++) {
        table[i] = level_1_address + i * RADIX3_PAGE_BYTES;
    }

    for (uint64_t i = 0; i < level_2_pages; i++) {
        level_1[i] = level_2_address + i * RADIX3_PAGE_BYTES;
    }

    uint64_t entry = 0;
    for (size_t i = 0; i < run_count; i++) {
        uint64_t run_pages = runs[i].size / RADIX3_PAGE_BYTES;
        for (uint64_t page = 0; page < run_pages; page++) {
            level_2[entry] = runs[i].phys + page * RADIX3_PAGE_BYTES;
            entry++;
        }
    }
}

// Allocates whole pages holding `bytes` bytes for the card to reach, naming the memory as `what` in a failure's log
static int32_t allocate_for_card(
    const dma::device_profile& device,
    uint64_t bytes,
    dma::caching mode,
    const char* what,
    dma::buffer& out
) {
    uint64_t pages = pmm::page_align_up(bytes) / pmm::PAGE_SIZE;

    int32_t rc = dma::ERR_NO_MEM;
    RUN_ELEVATED(rc = dma::alloc_pages(device, pages, mode, paging::PAGE_USER, out));
    if (rc != dma::OK) {
        log::error("nvidia: allocating %lu pages for %s failed: %d", pages, what, rc);
        return ERR_NO_MEMORY;
    }

    return OK;
}

// Copies the `bytes` bytes at `offset` in `file` to `destination`, naming them as `what` in a failure's log
static int32_t copy_from_file(
    const fw::file& file,
    uint64_t offset,
    uint64_t bytes,
    uintptr_t destination,
    const char* what
) {
    int32_t fw_result = file.read(offset, reinterpret_cast<void*>(destination), bytes);
    int32_t rc = to_bring_up_result(fw_result);
    if (rc != OK) {
        log::error("nvidia: reading %s failed with fw error %d", what, fw_result);
    }

    return rc;
}

int32_t check_boot_copies(const boot_file_header& bootloader_header, const gsp_image_layout& gsp_image) {
    if (bootloader_header.program_size == 0) {
        log::error("nvidia: the bootloader's program holds no byte");
        return ERR_BAD_FIRMWARE;
    }

    if (radix3_table_pages(pages_for_bytes(gsp_image.firmware_size)) == 0) {
        log::error(
            "nvidia: the GSP's firmware of %lu bytes has more pages than a radix3 page table maps",
            gsp_image.firmware_size
        );
        return ERR_BAD_FIRMWARE;
    }

    return OK;
}

int32_t write_booter_load_image(
    const fw::file& booter_load,
    const boot_file_header& header,
    const booter_layout& layout,
    const booter_signature& signature,
    uint8_t* image
) {
    uint64_t slot_offset = static_cast<uint64_t>(layout.data_offset) + layout.signature_slot;
    uint64_t chosen_index = signature.index;
    uint64_t signature_offset = layout.signatures_offset + chosen_index * layout.signature_size;
    uintptr_t program = reinterpret_cast<uintptr_t>(image);

    int32_t rc = copy_from_file(
        booter_load,
        header.program_offset,
        header.program_size,
        program,
        "booter load's program"
    );
    if (rc != OK) {
        return rc;
    }

    return copy_from_file(
        booter_load,
        signature_offset,
        layout.signature_size,
        program + slot_offset,
        "booter load's signature"
    );
}

int32_t gsp_boot_memory::copy_boot_files(
    const dma::device_profile& device,
    const firmware_files& files,
    const boot_file_header& bootloader_header,
    const gsp_image_layout& gsp_image
) {
    if (holds_bootloader_and_gsp_image_copies()) {
        log::error("nvidia: the GSP's boot memory already holds copies of the bootloader and the GSP image");
        return ERR_BAD_STATE;
    }

    int32_t rc = check_boot_copies(bootloader_header, gsp_image);
    if (rc != OK) {
        return rc;
    }

    uint64_t table_pages = radix3_table_pages(pages_for_bytes(gsp_image.firmware_size));
    rc = copy_each_part(device, files, bootloader_header, gsp_image, table_pages);
    if (rc != OK) {
        // The card was never given these addresses, so nothing reads the memory
        free_bootloader_and_gsp_image_copies();
        return rc;
    }

    m_bootloader_bytes = bootloader_header.program_size;
    m_firmware_bytes = gsp_image.firmware_size;
    m_signature_bytes = gsp_image.signature_size;

    return OK;
}

bool gsp_boot_memory::holds_bootloader_and_gsp_image_copies() const {
    return m_bootloader.virt != 0 || m_signature.virt != 0 || m_radix3_table.virt != 0 || m_firmware_runs;
}

int32_t gsp_boot_memory::copy_each_part(
    const dma::device_profile& device,
    const firmware_files& files,
    const boot_file_header& bootloader_header,
    const gsp_image_layout& gsp_image,
    uint64_t table_pages
) {
    int32_t rc = allocate_for_card(
        device,
        bootloader_header.program_size,
        dma::caching::cached_when_coherent,
        "the bootloader's program",
        m_bootloader
    );
    if (rc != OK) {
        return rc;
    }

    rc = copy_from_file(
        files.bootloader,
        bootloader_header.program_offset,
        bootloader_header.program_size,
        m_bootloader.virt,
        "the bootloader's program"
    );
    if (rc != OK) {
        return rc;
    }

    rc = allocate_for_card(
        device,
        gsp_image.signature_size,
        dma::caching::cached_when_coherent,
        "the firmware's signature",
        m_signature
    );
    if (rc != OK) {
        return rc;
    }

    rc = copy_from_file(
        files.gsp_image,
        gsp_image.signature_file_offset,
        gsp_image.signature_size,
        m_signature.virt,
        "the firmware's signature"
    );
    if (rc != OK) {
        return rc;
    }

    rc = allocate_for_card(
        device,
        table_pages * RADIX3_PAGE_BYTES,
        dma::caching::cached_when_coherent,
        "the radix3 page table",
        m_radix3_table
    );
    if (rc != OK) {
        return rc;
    }

    size_t run_count = (gsp_image.firmware_size - 1) / FIRMWARE_RUN_BYTES + 1;
    m_firmware_runs = static_cast<dma::buffer*>(heap::uzalloc(run_count * sizeof(dma::buffer)));
    if (!m_firmware_runs) {
        log::error("nvidia: allocating the list of the GSP firmware's %lu runs failed", run_count);
        return ERR_NO_MEMORY;
    }

    m_firmware_run_count = run_count;

    for (size_t i = 0; i < run_count; i++) {
        uint64_t copied_bytes = i * FIRMWARE_RUN_BYTES;
        uint64_t left_bytes = gsp_image.firmware_size - copied_bytes;
        uint64_t run_bytes = left_bytes < FIRMWARE_RUN_BYTES ? left_bytes : FIRMWARE_RUN_BYTES;
        uint64_t file_offset = gsp_image.firmware_file_offset + copied_bytes;

        rc = allocate_for_card(
            device,
            run_bytes,
            dma::caching::cached_when_coherent,
            "the GSP's firmware",
            m_firmware_runs[i]
        );
        if (rc != OK) {
            return rc;
        }

        rc = copy_from_file(files.gsp_image, file_offset, run_bytes, m_firmware_runs[i].virt, "the GSP's firmware");
        if (rc != OK) {
            return rc;
        }
    }

    uint64_t* table = reinterpret_cast<uint64_t*>(m_radix3_table.virt);
    write_radix3_table(table, m_radix3_table.phys, m_firmware_runs, run_count);

    return OK;
}

int32_t gsp_boot_memory::copy_booter_load(
    const dma::device_profile& device,
    const fw::file& booter_load,
    const boot_file_header& header,
    const booter_layout& layout,
    const booter_signature& signature
) {
    if (m_booter_load.virt != 0) {
        log::error("nvidia: the GSP's boot memory already holds a copy of booter load's program");
        return ERR_BAD_STATE;
    }

    int32_t rc = allocate_for_card(
        device,
        header.program_size,
        dma::caching::uncached,
        "booter load's program",
        m_booter_load
    );
    if (rc != OK) {
        return rc;
    }

    uint8_t* image = reinterpret_cast<uint8_t*>(m_booter_load.virt);
    rc = write_booter_load_image(booter_load, header, layout, signature, image);
    if (rc != OK) {
        // The card was never given the copy's address, so nothing reads it
        RUN_ELEVATED(dma::free_pages(m_booter_load));
    }

    return rc;
}

int32_t gsp_boot_memory::allocate_message_queues(const dma::device_profile& device) {
    if (m_message_queues.virt != 0) {
        log::error("nvidia: the GSP's boot memory already holds the message queues' memory");
        return ERR_BAD_STATE;
    }

    return allocate_for_card(
        device,
        QUEUE_MEMORY_BYTES,
        dma::caching::cached_when_coherent,
        "the message queues",
        m_message_queues
    );
}

int32_t gsp_boot_memory::write_protected_metadata(
    const dma::device_profile& device,
    const frts_region& region,
    const protected_memory_layout& layout,
    const bootloader_layout& bootloader_parts
) {
    if (m_protected_metadata.virt != 0) {
        log::error("nvidia: the GSP's boot memory already holds protected memory's metadata");
        return ERR_BAD_STATE;
    }

    if (!holds_bootloader_and_gsp_image_copies()) {
        log::error("nvidia: protected memory's metadata needs the copies of the bootloader and the GSP image first");
        return ERR_BAD_STATE;
    }

    int32_t rc = allocate_for_card(
        device,
        PROTECTED_METADATA_BYTES,
        dma::caching::cached_when_coherent,
        "protected memory's metadata",
        m_protected_metadata
    );
    if (rc != OK) {
        return rc;
    }

    fill_protected_metadata(region, layout, bootloader_parts);

    return OK;
}

// Writes only what NVIDIA's driver sets by default on its first attempt to boot the GSP, since the page comes zeroed
void gsp_boot_memory::fill_protected_metadata(
    const frts_region& region,
    const protected_memory_layout& layout,
    const bootloader_layout& bootloader_parts
) {
    uint64_t signature_blocks = (m_signature_bytes + SIGNATURE_BLOCK_BYTES - 1) / SIGNATURE_BLOCK_BYTES;
    auto* metadata = reinterpret_cast<protected_metadata*>(m_protected_metadata.virt);

    metadata->magic = PROTECTED_METADATA_MAGIC;
    metadata->revision = PROTECTED_METADATA_REVISION;

    metadata->radix3_address = m_radix3_table.phys;
    metadata->firmware_bytes = m_firmware_bytes;
    metadata->bootloader_address = m_bootloader.phys;
    metadata->bootloader_bytes = m_bootloader_bytes;
    metadata->signature_address = m_signature.phys;
    metadata->signature_bytes = signature_blocks * SIGNATURE_BLOCK_BYTES;

    metadata->bootloader_code_offset = bootloader_parts.monitor_code_offset;
    metadata->bootloader_data_offset = bootloader_parts.monitor_data_offset;
    metadata->bootloader_manifest_offset = bootloader_parts.manifest_offset;

    metadata->reserved_start = layout.unprotected_heap_offset;
    metadata->unprotected_heap_offset = layout.unprotected_heap_offset;
    metadata->unprotected_heap_bytes = layout.unprotected_heap_bytes;

    metadata->protected_start = layout.protected_start;
    metadata->heap_offset = layout.heap_offset;
    metadata->heap_bytes = layout.heap_bytes;
    metadata->firmware_offset = layout.firmware_offset;
    metadata->bootloader_offset = layout.bootloader_offset;

    // Protected memory ends where its topmost part, the FRTS region, ends
    metadata->frts_offset = region.offset;
    metadata->frts_bytes = FRTS_REGION_BYTES;
    metadata->protected_end = region.offset + FRTS_REGION_BYTES;

    metadata->video_memory_size = region.video_memory_size;
    metadata->vga_workspace_offset = region.vga_workspace_offset;
    metadata->vga_workspace_bytes = region.video_memory_size - region.vga_workspace_offset;
}

int32_t gsp_boot_memory::write_firmware_arguments(const dma::device_profile& device) {
    if (m_libos_arguments.virt != 0) {
        log::error(
            "nvidia: the GSP's boot memory already holds the log buffers and arguments the GSP's firmware starts with"
        );
        return ERR_BAD_STATE;
    }

    if (m_message_queues.virt == 0) {
        log::error(
            "nvidia: the log buffers and arguments the GSP's firmware starts with need the message queues' memory first"
        );
        return ERR_BAD_STATE;
    }

    int32_t rc = allocate_firmware_arguments(device);
    if (rc != OK) {
        // The card was never given these addresses, so nothing reads the memory
        free_firmware_arguments();
        return rc;
    }

    // The firmware's write position, the first 8 bytes of each log buffer, stays 0 from the zeroed pages
    for (const dma::buffer& log_buffer : m_log_buffers) {
        uint64_t* words = reinterpret_cast<uint64_t*>(log_buffer.virt);
        for (uint64_t page = 0; page < LOG_BUFFER_PAGES; page++) {
            words[1 + page] = log_buffer.phys + page * pmm::PAGE_SIZE;
        }
    }

    fill_resource_manager_arguments();
    fill_libos_arguments();

    return OK;
}

int32_t gsp_boot_memory::allocate_firmware_arguments(const dma::device_profile& device) {
    for (dma::buffer& log_buffer : m_log_buffers) {
        int32_t rc = allocate_for_card(
            device,
            LOG_BUFFER_BYTES,
            dma::caching::cached_when_coherent,
            "a log buffer of the GSP's firmware",
            log_buffer
        );
        if (rc != OK) {
            return rc;
        }
    }

    int32_t rc = allocate_for_card(
        device,
        RESOURCE_MANAGER_ARGUMENTS_BYTES,
        dma::caching::cached_when_coherent,
        "the resource manager's arguments",
        m_resource_manager_arguments
    );
    if (rc != OK) {
        return rc;
    }

    return allocate_for_card(
        device,
        LIBOS_ARGUMENTS_BYTES,
        dma::caching::uncached,
        "LibOS's arguments",
        m_libos_arguments
    );
}

// Writes only what NVIDIA's driver sets by default on its first attempt to boot the first GPU it attaches, since the
// page comes zeroed: no queues for the interrupt task, no power transition, no profiler buffer and GPU instance 0
void gsp_boot_memory::fill_resource_manager_arguments() {
    auto* arguments = reinterpret_cast<resource_manager_arguments*>(m_resource_manager_arguments.virt);

    arguments->queue_memory_address = m_message_queues.phys;
    arguments->queue_page_table_entries = QUEUE_PAGE_TABLE_ENTRIES;
    arguments->command_queue_offset = COMMAND_QUEUE_OFFSET;
    arguments->status_queue_offset = STATUS_QUEUE_OFFSET;
}

// The page comes zeroed, as NVIDIA's driver zeroes it, so the regions past the last stay 0
void gsp_boot_memory::fill_libos_arguments() {
    auto* regions = reinterpret_cast<libos_region*>(m_libos_arguments.virt);

    for (size_t i = 0; i < FIRMWARE_LOG_COUNT; i++) {
        regions[i] = {
            .id       = libos_region_id(LOG_BUFFER_NAMES[i]),
            .address  = m_log_buffers[i].phys,
            .bytes    = m_log_buffers[i].size,
            .kind     = LIBOS_REGION_CONTIGUOUS,
            .location = LIBOS_REGION_IN_SYSTEM_MEMORY,
            .padding  = {},
        };
    }

    regions[FIRMWARE_LOG_COUNT] = {
        .id       = libos_region_id(RESOURCE_MANAGER_ARGUMENTS_NAME),
        .address  = m_resource_manager_arguments.phys,
        .bytes    = m_resource_manager_arguments.size,
        .kind     = LIBOS_REGION_CONTIGUOUS,
        .location = LIBOS_REGION_IN_SYSTEM_MEMORY,
        .padding  = {},
    };
}

void gsp_boot_memory::release() {
    free_bootloader_and_gsp_image_copies();
    free_firmware_arguments();
    RUN_ELEVATED(dma::free_pages(m_booter_load));
    RUN_ELEVATED(dma::free_pages(m_message_queues));
    RUN_ELEVATED(dma::free_pages(m_protected_metadata));
}

void gsp_boot_memory::free_firmware_arguments() {
    for (dma::buffer& log_buffer : m_log_buffers) {
        RUN_ELEVATED(dma::free_pages(log_buffer));
    }

    RUN_ELEVATED(dma::free_pages(m_resource_manager_arguments));
    RUN_ELEVATED(dma::free_pages(m_libos_arguments));
}

void gsp_boot_memory::free_bootloader_and_gsp_image_copies() {
    RUN_ELEVATED(dma::free_pages(m_bootloader));
    RUN_ELEVATED(dma::free_pages(m_signature));
    RUN_ELEVATED(dma::free_pages(m_radix3_table));

    for (size_t i = 0; i < m_firmware_run_count; i++) {
        RUN_ELEVATED(dma::free_pages(m_firmware_runs[i]));
    }

    if (m_firmware_runs) {
        heap::ufree(m_firmware_runs);
    }

    m_firmware_runs = nullptr;
    m_firmware_run_count = 0;
    m_bootloader_bytes = 0;
    m_firmware_bytes = 0;
    m_signature_bytes = 0;
}

} // namespace drivers::nvidia
