// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_BOOT_MEMORY_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_BOOT_MEMORY_H

#include "drivers/graphics/nvidia/firmware_files.h"
#include "dma/dma.h"

namespace drivers::nvidia {

// Bytes of each page of a radix3 page table, the three levels of page addresses `gsp_boot_memory` describes, and
// of each page of the data it maps
constexpr uint64_t RADIX3_PAGE_BYTES = 0x1000;

// The 64-bit addresses one page of a radix3 page table holds
constexpr uint64_t RADIX3_PAGE_ENTRIES = RADIX3_PAGE_BYTES / sizeof(uint64_t);

/**
 * @brief How many pages a radix3 page table takes to map `data_pages` pages
 * of data, its three levels together, as `gsp_boot_memory` describes them.
 * @return The pages, or 0 when `data_pages` is 0 or more than the
 * 512 x 512 x 512 pages a level 0 of one page leads to.
 */
uint64_t radix3_table_pages(uint64_t data_pages);

/**
 * @brief Write a radix3 page table mapping the pages of `runs` in order, the
 * first page of `runs[0]` first.
 * @param table The CPU's mapping of the table, `radix3_table_pages` pages for
 * the runs' pages together, which must be zeroed, since entries past the
 * last one each level uses are left as they are.
 * @param table_address The address the card reaches the table's first byte
 * at, which must be a multiple of 4 KiB. The card must reach the whole table
 * as one contiguous range from there.
 * @param runs Ranges the card must reach each as one contiguous range from
 * its `phys`, whose `phys` and `size` must be multiples of 4 KiB and whose
 * pages together must number 1 to the 512 x 512 x 512 that
 * `radix3_table_pages` accepts.
 * @param run_count How many ranges `runs` holds.
 */
void write_radix3_table(uint64_t* table, uint64_t table_address, const dma::buffer* runs, size_t run_count);

/**
 * @brief Check that the bootloader's program holds at least one byte and
 * that a radix3 page table maps the GSP's firmware, the two rules on the
 * files that `gsp_boot_memory` relies on. Bring-up checks this with the
 * other firmware checks, before FWSEC runs, since a refusal after FWSEC
 * leaves protected memory set up until the card is reset.
 * @param bootloader_header Must be the header `check_boot_file` returned for
 * the bootloader.
 * @param gsp_image Must be the layout `check_gsp_image` returned for the GSP
 * image, whose firmware then holds at least one byte.
 * @return `OK`, or `ERR_BAD_FIRMWARE` when the bootloader's program holds no
 * byte or the firmware has more pages than a radix3 page table maps. The
 * cause is logged.
 */
int32_t check_boot_copies(const boot_file_header& bootloader_header, const gsp_image_layout& gsp_image);

/**
 * The system memory the card reads to boot the GSP, holding copies of what
 * the GSP boots from, which booter load's program, run on SEC2, reads by
 * their addresses to copy the bootloader's program and the firmware into
 * protected memory, verify the firmware with the signature and boot it. The
 * firmware is the GSP image's .fwimage section, the signature is the chip's
 * signature section of the GSP image, and the bootloader's program is the
 * program the GSP's RISC-V core runs to start the firmware. Each copy starts
 * on a page, and the rest of its last page holds zeros.
 *
 * The firmware lies in physically contiguous runs of up to 2 MiB, listed by
 * a radix3 page table, three levels of 4 KiB pages of 64-bit addresses.
 * Level 0 is one page that lists the pages of level 1, level 1 lists the
 * pages of level 2, and level 2 lists every page of the firmware, so the
 * card reaches the whole firmware from the table's first byte:
 *
 *   radix3_address()
 *   +---------+     +---------+     +---------+     +---------------+
 *   | level 0 | --> | level 1 | --> | level 2 | --> | firmware page |
 *   +---------+     +---------+     +---------+     +---------------+
 *
 * The table's three levels lie in that order in one contiguous range, each
 * level filling whole pages.
 *
 * One object owns all of this memory, and only `release` frees it. Once the
 * card has the addresses, booter load on SEC2 and a running GSP may read any
 * of it, so the caller calls `release` only once SEC2 and the GSP are both
 * confirmed halted, or before the card was given any of the addresses. An
 * object dropped without `release` leaves the memory allocated for good,
 * which is what a card whose halt was never confirmed needs.
 */
class gsp_boot_memory {
public:
    gsp_boot_memory() = default;

    gsp_boot_memory(const gsp_boot_memory&) = delete;
    gsp_boot_memory& operator=(const gsp_boot_memory&) = delete;

    /**
     * @brief Allocate the memory and copy the bootloader's program and the
     * GSP image's firmware and signature into it, then write the firmware's
     * radix3 page table. Each copy is read with `fw::file::read`, which
     * takes the file's mutex, so this must not run in an interrupt handler
     * or under a spinlock. On the boot thread, which cannot wait, the files
     * must be ones no other thread uses at the time, as `fw::file`
     * describes.
     * @param device The card's profile, from `pci_driver::dma_profile`. The
     * memory is cached when the card is coherent and uncached otherwise.
     * @param files The firmware files, each open.
     * @param bootloader_header Must be the header `check_boot_file` returned
     * for `files.bootloader`, whose program then lies inside the file.
     * @param gsp_image Must be the layout `check_gsp_image` returned for
     * `files.gsp_image`, whose firmware and signature then each hold at least
     * one byte inside the file.
     * @return `OK`, `ERR_BAD_STATE` when the object already holds memory,
     * which it keeps, `ERR_BAD_FIRMWARE` when `check_boot_copies` refuses the
     * bootloader's program or the firmware, or a read fails, or
     * `ERR_NO_MEMORY` when allocating fails, as it does for a part larger
     * than a single DMA allocation holds. The cause is logged. Any other
     * error frees everything the call allocated, which the card was never
     * given, and leaves the object holding no memory.
     */
    int32_t copy_boot_files(
        const dma::device_profile& device,
        const firmware_files& files,
        const boot_file_header& bootloader_header,
        const gsp_image_layout& gsp_image
    );

    /**
     * @brief Free all of the memory, leaving the object holding none. The
     * caller must have confirmed that SEC2 and the GSP are halted, or never
     * given the card any of the memory's addresses. Releasing an object
     * holding no memory does nothing.
     */
    void release();

    // Where the card reaches each part, or 0 while the object holds no memory
    uint64_t radix3_address() const { return m_radix3_table.phys; }
    uint64_t bootloader_address() const { return m_bootloader.phys; }
    uint64_t signature_address() const { return m_signature.phys; }

private:
    int32_t copy_each_part(
        const dma::device_profile& device,
        const firmware_files& files,
        const boot_file_header& bootloader_header,
        const gsp_image_layout& gsp_image,
        uint64_t table_pages
    );

    dma::buffer  m_bootloader = {};
    dma::buffer  m_signature = {};
    dma::buffer  m_radix3_table = {};
    dma::buffer* m_firmware_runs = nullptr;
    size_t       m_firmware_run_count = 0;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_BOOT_MEMORY_H
