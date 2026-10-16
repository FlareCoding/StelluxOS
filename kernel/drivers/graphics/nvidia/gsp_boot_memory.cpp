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
    bool holds_copies = m_bootloader.virt != 0 || m_signature.virt != 0 || m_radix3_table.virt != 0 || m_firmware_runs;
    if (holds_copies) {
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
    }

    return rc;
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

void gsp_boot_memory::release() {
    free_bootloader_and_gsp_image_copies();
    RUN_ELEVATED(dma::free_pages(m_booter_load));
    RUN_ELEVATED(dma::free_pages(m_message_queues));
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
}

} // namespace drivers::nvidia
