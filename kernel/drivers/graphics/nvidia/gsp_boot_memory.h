// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_BOOT_MEMORY_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_BOOT_MEMORY_H

#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/protected_memory.h"
#include "drivers/graphics/nvidia/registers.h"
#include "dma/dma.h"

namespace drivers::nvidia {

// Bytes of each page of a radix3 page table, the three levels of page addresses `gsp_boot_memory` describes, and
// of each page of the data it maps
constexpr uint64_t RADIX3_PAGE_BYTES = 0x1000;

// The 64-bit addresses one page of a radix3 page table holds
constexpr uint64_t RADIX3_PAGE_ENTRIES = RADIX3_PAGE_BYTES / sizeof(uint64_t);

// The GSP firmware's log buffers, one for each of its tasks that log: the init task, the interrupt task and the
// resource manager (its task that serves the driver's requests), in that order
constexpr size_t FIRMWARE_LOG_COUNT = 3;

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
 * @brief Copy booter load's program to `image`, then write the signature
 * `signature` names over the copy's signature slot, the
 * `layout.signature_size` bytes at `layout.signature_slot` from the start of
 * the program's data, as NVIDIA's driver patches booter load. Both parts are
 * read with `fw::file::read`, which takes the file's mutex, so this must not
 * run in an interrupt handler or under a spinlock. On the boot thread, which
 * cannot wait, `booter_load` must be one no other thread uses at the time,
 * as `fw::file` describes.
 * @param booter_load Must hold booter load open.
 * @param header Must be the header `check_boot_file` returned for
 * `booter_load`, whose program then lies inside the file.
 * @param layout Must be the layout `read_booter_layout` returned for
 * `booter_load`, whose signature slot then lies inside the program's data
 * and whose signatures lie inside the file.
 * @param signature The signature to write, whose `index` must be below
 * `layout.signature_count`, as it is in any choice `select_booter_signature`
 * returns for `layout`.
 * @param image Must hold `header.program_size` bytes the CPU may write at any
 * alignment, as `dma::alloc_pages` promises of its buffers, since the reads
 * copy into it at any alignment.
 * @return `OK`, or `ERR_BAD_FIRMWARE` when a read fails, which leaves
 * `image` partly written. The cause is logged.
 */
int32_t write_booter_load_image(
    const fw::file& booter_load,
    const boot_file_header& header,
    const booter_layout& layout,
    const booter_signature& signature,
    uint8_t* image
);

/**
 * The system memory the card reaches to boot the GSP, and through which the
 * GSP then exchanges messages with the driver. It holds copies of what the
 * GSP boots from, which booter load's program, run on SEC2, the GPU's
 * security processor, reads by their addresses to copy the bootloader's
 * program and the firmware into protected memory, verify the firmware with
 * the signature and boot it. The firmware is the GSP image's .fwimage
 * section, the signature is the chip's signature section of the GSP image,
 * and the bootloader's program is the program the GSP's RISC-V core runs to
 * start the firmware. It also holds a copy of booter load's program, which
 * SEC2 loads, with the signature given to `copy_booter_load` in its signature
 * slot, the memory `message_queues` lays the message queues out in (the two
 * rings through which the driver and the running firmware exchange messages)
 * and a page holding protected memory's metadata. The metadata tells booter
 * load where the copies of the bootloader's program, the firmware and the
 * signature lie and where each part of protected memory goes. Each copy
 * starts on a page, and the rest of its last page holds zeros. The card
 * reaches the copies of the bootloader's program and of the signature each as
 * one contiguous range, from `bootloader_address()` and
 * `signature_address()`.
 *
 * The object also holds what the firmware reads when it starts: a log buffer
 * for each of the firmware's tasks that log, a page of arguments telling the
 * resource manager (the firmware's task that serves the driver's requests)
 * where the message queues lie, and a page of LibOS's arguments, which lists
 * those buffers and that page as the regions LibOS (the small operating system
 * the firmware runs on) maps for the firmware.
 *
 * After a load or run of FWSEC (the signed program from the VBIOS that sets up
 * protected memory) that timed out, the object also holds FWSEC's copy, which
 * `keep_fwsec_copy` hands it, since the GSP may still read the copy.
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
 * One object owns all of this memory, and only `release` frees it all. The
 * copies `copy_boot_files` makes, booter load's copy, the message queues'
 * memory, the metadata and what the firmware reads when it starts are each
 * filled by a call of their own, which on an error frees only what it
 * allocated. Once the card has the addresses, booter load on SEC2 and a
 * running GSP may read any of it, and the GSP writes the message queues and
 * the log buffers, so the caller calls `release` only once every processor
 * given any of the addresses, SEC2 or the GSP, is confirmed stopped.
 * `release_after_gsp_stop` stops the GSP and calls `release` only once the GSP
 * reads as stopped. An object dropped without `release` leaves the memory
 * allocated for good, which is what a card whose stop was never confirmed
 * needs.
 */
class gsp_boot_memory {
public:
    gsp_boot_memory() = default;

    gsp_boot_memory(const gsp_boot_memory&) = delete;
    gsp_boot_memory& operator=(const gsp_boot_memory&) = delete;

    /**
     * @brief Allocate memory for the bootloader's program and the GSP image's
     * firmware and signature, copy each into its memory, then write the
     * firmware's radix3 page table. Each copy is read with `fw::file::read`,
     * which takes the file's mutex, so this must not run in an interrupt
     * handler or under a spinlock. On the boot thread, which cannot wait, the
     * files must be ones no other thread uses at the time, as `fw::file`
     * describes.
     * @param device The card's profile, from `pci_driver::dma_profile`. The
     * memory is cached when the card is coherent and uncached otherwise.
     * @param files The firmware files, each open.
     * @param bootloader_header Must be the header `check_boot_file` returned
     * for `files.bootloader`, whose program then lies inside the file.
     * @param gsp_image Must be the layout `check_gsp_image` returned for
     * `files.gsp_image`, whose firmware and signature then each hold at least
     * one byte inside the file.
     * @return `OK`, `ERR_BAD_STATE` when the object already holds these
     * copies, which it keeps, `ERR_BAD_FIRMWARE` when `check_boot_copies`
     * refuses the bootloader's program or the firmware, or a read fails, or
     * `ERR_NO_MEMORY` when allocating fails, as it does for a part larger
     * than a single DMA allocation holds. The cause is logged. Any other
     * error frees everything the call allocated, which the card was never
     * given, and leaves the object's other memory as it was.
     */
    int32_t copy_boot_files(
        const dma::device_profile& device,
        const firmware_files& files,
        const boot_file_header& bootloader_header,
        const gsp_image_layout& gsp_image
    );

    /**
     * @brief Allocate memory for SEC2 to load booter load from, and write
     * booter load's program into it with its signature, as
     * `write_booter_load_image` describes. The card reaches the copy as one
     * contiguous range from `booter_load_address()`, and the copy is uncached,
     * both as NVIDIA's driver allocates it. The reads take the file's mutex,
     * so this must not run in an interrupt handler or under a spinlock, and on
     * the boot thread `booter_load` must be one no other thread uses at the
     * time.
     * @param device The card's profile, from `pci_driver::dma_profile`.
     * @param booter_load Must hold booter load open.
     * @param header Must be the header `check_boot_file` returned for
     * `booter_load`.
     * @param layout Must be the layout `read_booter_layout` returned for
     * `booter_load`.
     * @param signature The signature to write, whose `index` must be below
     * `layout.signature_count`, as it is in any choice
     * `select_booter_signature` returns for `layout`.
     * @return `OK`, `ERR_BAD_STATE` when the object already holds booter
     * load's copy, which it keeps, `ERR_BAD_FIRMWARE` when a read fails, or
     * `ERR_NO_MEMORY` when allocating fails, as it does for a program larger
     * than a single DMA allocation holds. The cause is logged. Any other
     * error frees the copy, which the card was never given, and leaves the
     * object's other memory as it was.
     */
    int32_t copy_booter_load(
        const dma::device_profile& device,
        const fw::file& booter_load,
        const boot_file_header& header,
        const booter_layout& layout,
        const booter_signature& signature
    );

    /**
     * @brief Allocate the `QUEUE_MEMORY_BYTES` bytes `message_queues` lays
     * the message queues out in. The memory starts on a 4 KiB page, the card
     * reaches it as one contiguous range from `message_queues_address()`, the
     * CPU may access it at any alignment, as `dma::alloc_pages` promises, and
     * it is cached when the card is coherent and uncached otherwise, so it
     * meets what `message_queues` requires of its memory.
     * @param device The card's profile, from `pci_driver::dma_profile`.
     * @return `OK`, `ERR_BAD_STATE` when the object already holds the message
     * queues' memory, which it keeps, or `ERR_NO_MEMORY` when allocating
     * fails, which leaves the object's other memory as it was. The cause is
     * logged.
     */
    int32_t allocate_message_queues(const dma::device_profile& device);

    /**
     * @brief Allocate a page for protected memory's metadata and write the
     * metadata into it as NVIDIA's driver writes it by default on its first
     * attempt to boot the GSP, rather than a retry or a resume: where the
     * copies `copy_boot_files` made lie and their sizes, the signature's
     * rounded up to whole 256-byte blocks, where the bootloader's descriptor
     * places the parts of its program, video memory's size, and where `region`
     * and `layout` put the VGA workspace, each part of protected memory and
     * the unprotected heap in video memory. Booter load reads the metadata
     * from `protected_metadata_address()`, the page's start, and places it at
     * `layout.protected_start`. The page is cached when the card is coherent
     * and uncached otherwise.
     * @param device The card's profile, from `pci_driver::dma_profile`.
     * @param region Must be the FRTS region `layout` was laid out below.
     * @param layout Must be the layout `lay_out_protected_memory` returned for
     * `region` and the sizes of the bootloader's program and the firmware
     * that `copy_boot_files` copied.
     * @param bootloader_parts Must be the layout `read_bootloader_layout`
     * returned for the bootloader whose program `copy_boot_files` copied.
     * @return `OK`, `ERR_BAD_STATE` when the object already holds the
     * metadata, which it keeps, or does not hold the copies `copy_boot_files`
     * makes, or `ERR_NO_MEMORY` when allocating fails. Either error leaves the
     * object's other memory as it was. The cause is logged.
     */
    int32_t write_protected_metadata(
        const dma::device_profile& device,
        const frts_region& region,
        const protected_memory_layout& layout,
        const bootloader_layout& bootloader_parts
    );

    /**
     * @brief Allocate and write what the GSP's firmware reads when it starts,
     * as NVIDIA's driver writes it by default on its first attempt to boot the
     * first GPU it attaches. The resource manager's arguments give where the
     * card reaches the message queues' memory, how many entries its page table
     * holds and where the command queue and the status queue start in it, as
     * `message_queues` lays them out, with no queues for the firmware's
     * interrupt task and GPU instance 0, the number NVIDIA's driver gives the
     * first GPU it attaches. Each of the `FIRMWARE_LOG_COUNT` log buffers
     * holds 64 KiB, NVIDIA's size in its release build, starting with the
     * firmware's write position (0), then the address of each of its pages.
     * LibOS's arguments list the memory regions LibOS maps for the firmware:
     * the log buffers (the init task's first), then the resource manager's
     * arguments. Each region gives NVIDIA's name for it, its size and the
     * address the card reaches it at as one contiguous range in system memory.
     * The card reaches each log buffer and each page of arguments as one
     * contiguous range from `log_buffer_address(index)`,
     * `resource_manager_arguments_address()` or `libos_arguments_address()`,
     * starting on a 4 KiB boundary, as NVIDIA's driver aligns them. LibOS's
     * arguments are uncached, as NVIDIA's driver allocates them, and the rest
     * is cached when the card is coherent and uncached otherwise.
     * @param device The card's profile, from `pci_driver::dma_profile`.
     * @return `OK`, `ERR_BAD_STATE` when the object already holds the log
     * buffers and arguments, which it keeps, or does not hold the message
     * queues' memory, or `ERR_NO_MEMORY` when allocating fails, which frees
     * what the call allocated. Either error leaves the object's other memory
     * as it was. The cause is logged.
     */
    int32_t write_firmware_arguments(const dma::device_profile& device);

    /**
     * @brief Take the memory holding FWSEC's copy, which the GSP may still
     * read after a load or run of FWSEC that timed out, so that `release`
     * frees it with the rest of the memory.
     * @param copy The memory, from `dma::alloc_pages`, which the caller must
     * not free afterwards. The object must not hold FWSEC's copy already, or
     * the copy it holds stays allocated for good.
     */
    void keep_fwsec_copy(const dma::buffer& copy);

    /**
     * @brief Free all of the memory, leaving the object holding none. The
     * caller must have confirmed that every processor given any of the
     * memory's addresses, SEC2 or the GSP, is stopped. Releasing an
     * object holding no memory does nothing.
     */
    void release();

    /**
     * @brief Stop the GSP with `stop_gsp`, then free all of the memory as
     * `release` does, but only once the GSP reads as stopped, since a GSP not
     * confirmed stopped may still read or write any of it. The stop spins on
     * the calling CPU, as `stop_gsp` describes. The caller must have confirmed
     * that SEC2 is stopped if it was given any of the memory's addresses,
     * since only the GSP is stopped here.
     * @param regs A view of BAR0, as for `stop_gsp`.
     * @param boot0 The chip's `PMC_BOOT_0` value, for the reset.
     * @param ready_timeout_ns How long each wait for the ready report may
     * take, as for `stop_gsp`.
     * @param timeout_ns How long each wait for the memories or the core
     * selection may take, as for `stop_gsp`.
     * @return `OK` with all of the memory freed, or the `ERR_TIMEOUT` or
     * `ERR_BAD_STATE` that `stop_gsp` returns, with all of the memory kept
     * allocated. The cause is logged.
     */
    int32_t release_after_gsp_stop(registers& regs, uint32_t boot0, uint64_t ready_timeout_ns, uint64_t timeout_ns);

    // Where the card reaches each part, or 0 while the object does not hold that part
    uint64_t radix3_address() const { return m_radix3_table.phys; }
    uint64_t bootloader_address() const { return m_bootloader.phys; }
    uint64_t signature_address() const { return m_signature.phys; }
    uint64_t booter_load_address() const { return m_booter_load.phys; }
    uint64_t message_queues_address() const { return m_message_queues.phys; }
    uint64_t protected_metadata_address() const { return m_protected_metadata.phys; }
    uint64_t resource_manager_arguments_address() const { return m_resource_manager_arguments.phys; }
    uint64_t libos_arguments_address() const { return m_libos_arguments.phys; }

    // Where the card reaches the log buffer of the init task, the interrupt task or the resource manager at `index` 0,
    // 1 or 2, or 0 while the object does not hold the log buffers or for an index past the last
    uint64_t log_buffer_address(size_t index) const {
        return index < FIRMWARE_LOG_COUNT ? m_log_buffers[index].phys : 0;
    }

    // The CPU's mapping of the message queues' memory, for `message_queues` to lay the queues out in, or null while
    // the object does not hold it
    uint8_t* message_queues_memory() const { return reinterpret_cast<uint8_t*>(m_message_queues.virt); }

private:
    friend struct gsp_boot_memory_test_access;

    bool holds_bootloader_and_gsp_image_copies() const;

    int32_t copy_each_part(
        const dma::device_profile& device,
        const firmware_files& files,
        const boot_file_header& bootloader_header,
        const gsp_image_layout& gsp_image,
        uint64_t table_pages
    );

    // Frees what `copy_boot_files` allocates, leaving the object's other memory as it was
    void free_bootloader_and_gsp_image_copies();

    void fill_protected_metadata(
        const frts_region& region,
        const protected_memory_layout& layout,
        const bootloader_layout& bootloader_parts
    );

    int32_t allocate_firmware_arguments(const dma::device_profile& device);

    // Frees what `write_firmware_arguments` allocates, leaving the object's other memory as it was
    void free_firmware_arguments();

    void fill_resource_manager_arguments();
    void fill_libos_arguments();

    dma::buffer  m_bootloader = {};
    dma::buffer  m_signature = {};
    dma::buffer  m_radix3_table = {};
    dma::buffer* m_firmware_runs = nullptr;
    size_t       m_firmware_run_count = 0;
    dma::buffer  m_booter_load = {};
    dma::buffer  m_message_queues = {};
    dma::buffer  m_protected_metadata = {};
    dma::buffer  m_log_buffers[FIRMWARE_LOG_COUNT] = {};
    dma::buffer  m_resource_manager_arguments = {};
    dma::buffer  m_libos_arguments = {};
    dma::buffer  m_fwsec_copy = {};

    // Bytes of each part `copy_boot_files` copied, while the object holds the copies
    uint64_t m_bootloader_bytes = 0;
    uint64_t m_firmware_bytes = 0;
    uint64_t m_signature_bytes = 0;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_BOOT_MEMORY_H
