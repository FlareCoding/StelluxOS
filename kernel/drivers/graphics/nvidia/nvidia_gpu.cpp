// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/nvidia_gpu.h"
#include "drivers/graphics/nvidia/boot_messages.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/fwsec.h"
#include "drivers/graphics/nvidia/gsp_boot_memory.h"
#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/protected_memory.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/vbios.h"
#include "drivers/graphics/gfxfb.h"
#include "boot/boot_services.h"
#include "common/logging.h"
#include "dma/dma.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"
#include "pci/pci_class_codes.h"

namespace drivers {

constexpr uint16_t NVIDIA_VENDOR_ID = 0x10DE;

// Named for each BAR's role on the card and numbered by register, so the BAR after the 64-bit BAR1 is BAR3.
// `nvidia::card_placement` says how NVIDIA's driver numbers them
constexpr uint8_t REGISTERS_BAR       = 0;
constexpr uint8_t VIDEO_MEMORY_BAR    = 1;
constexpr uint8_t INSTANCE_MEMORY_BAR = 3;

// Reads from a card that does not respond return all ones, a value PMC_BOOT_0
// never holds on a working card
constexpr uint32_t UNRESPONSIVE_CARD_READ = 0xFFFFFFFF;

int32_t nvidia_gpu_driver::attach() {
    bool is_display_registered = false;
    RUN_ELEVATED(is_display_registered = gfxfb::has_driver_display());
    if (is_display_registered) {
        log::info("nvidia: another driver's display already owns /dev/gfxfb");
        return DECLINED;
    }

    RUN_ELEVATED(m_dev->enable());

    uintptr_t registers_base = 0;
    int32_t rc = map_bar(REGISTERS_BAR, registers_base, paging::PAGE_USER);
    if (rc != OK) {
        log::error("nvidia: mapping BAR0 failed: %d", rc);
        return rc;
    }

    nvidia::registers regs(registers_base);
    uint32_t boot0 = regs.read(nvidia::PMC_BOOT_0);
    if (boot0 == UNRESPONSIVE_CARD_READ) {
        log::error("nvidia: the card does not respond, PMC_BOOT_0 reads all ones");
        return nvidia::ERR_NOT_RESPONDING;
    }

    uint16_t chip_id = nvidia::decode_chip_id(boot0);
    const nvidia::chip_info* chip = nvidia::find_chip(chip_id);
    if (!chip) {
        log::info("nvidia: chip 0x%03x has no chip table entry", chip_id);
        return DECLINED;
    }

    uint8_t revision = nvidia::decode_chip_revision(boot0);
    log::info("nvidia: %s revision %02X", chip->name, revision);

    nvidia::registers rom(registers_base + nvidia::ROM_WINDOW);
    nvidia::vbios_layout layout = {};
    rc = nvidia::read_vbios_layout(rom, layout);
    if (rc != nvidia::OK) {
        return rc;
    }

    for (size_t i = 0; i < layout.image_count; i++) {
        const nvidia::vbios_image& image = layout.images[i];
        log::info("nvidia: VBIOS image at 0x%X, %u bytes, code type 0x%02X", image.offset, image.size, image.code_type);
    }

    nvidia::firmware_files files;
    rc = nvidia::open_firmware_files(chip->firmware, files);
    if (rc != nvidia::OK) {
        return rc;
    }

    nvidia::gsp_image_layout gsp_image_parts = {};
    rc = nvidia::check_gsp_image(files.gsp_image, chip->firmware.gsp_signature_section, gsp_image_parts);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info(
        "nvidia: GSP image's firmware of %lu bytes at 0x%lX and signature section %s of %lu bytes at 0x%lX in the file",
        gsp_image_parts.firmware_size,
        gsp_image_parts.firmware_file_offset,
        chip->firmware.gsp_signature_section,
        gsp_image_parts.signature_size,
        gsp_image_parts.signature_file_offset
    );

    nvidia::boot_file_header bootloader_header = {};
    rc = nvidia::check_boot_file(files.bootloader, "bootloader", bootloader_header);
    if (rc != nvidia::OK) {
        return rc;
    }

    nvidia::bootloader_layout bootloader_parts = {};
    rc = nvidia::read_bootloader_layout(files.bootloader, bootloader_header, bootloader_parts);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info(
        "nvidia: bootloader's manifest at 0x%X, monitor data at 0x%X and monitor code at 0x%X in its %u-byte program, "
        "app version %u",
        bootloader_parts.manifest_offset,
        bootloader_parts.monitor_data_offset,
        bootloader_parts.monitor_code_offset,
        bootloader_header.program_size,
        bootloader_parts.app_version
    );

    nvidia::boot_file_header booter_load_header = {};
    rc = nvidia::check_boot_file(files.booter_load, "booter load", booter_load_header);
    if (rc != nvidia::OK) {
        return rc;
    }

    nvidia::booter_layout booter_parts = {};
    rc = nvidia::read_booter_layout(files.booter_load, booter_load_header, booter_parts);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info(
        "nvidia: booter load's %u bytes of code at 0x%X and %u bytes of data at 0x%X in its %u-byte program, "
        "ucode id %u, engine mask 0x%X",
        booter_parts.code_size,
        booter_parts.code_offset,
        booter_parts.data_size,
        booter_parts.data_offset,
        booter_load_header.program_size,
        booter_parts.ucode_id,
        booter_parts.engine_mask
    );

    log::info(
        "nvidia: booter load's signature slot at byte 0x%X of its data, %u signatures of %u bytes at 0x%X in the file",
        booter_parts.signature_slot,
        booter_parts.signature_count,
        booter_parts.signature_size,
        booter_parts.signatures_offset
    );

    nvidia::booter_signature booter_load_signature = {};
    rc = nvidia::select_booter_signature(regs, booter_parts, booter_load_signature);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info(
        "nvidia: booter load's signature %u of %u chosen with fuse version %u",
        booter_load_signature.index + 1,
        booter_parts.signature_count,
        booter_load_signature.fuse_version
    );

    rc = nvidia::check_boot_copies(bootloader_header, gsp_image_parts);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info("nvidia: firmware %s checked", nvidia::FIRMWARE_VERSION);

    nvidia::falcon_program_table table = {};
    rc = nvidia::find_falcon_program_table(rom, layout, table);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info(
        "nvidia: falcon program table at 0x%X in the VBIOS, %u entries of %u bytes",
        table.offset,
        table.entry_count,
        table.entry_size
    );

    nvidia::fwsec_layout fwsec = {};
    rc = nvidia::find_fwsec(rom, layout, table, fwsec);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info(
        "nvidia: FWSEC at 0x%X in the VBIOS, %u bytes holding %u bytes of code and %u bytes of data",
        fwsec.offset,
        fwsec.size,
        fwsec.code_size,
        fwsec.data_size
    );
    log::info(
        "nvidia: FWSEC's signature slot at byte 0x%X of its data, for one of its %u signatures at 0x%X in the VBIOS",
        fwsec.signature_slot,
        fwsec.signature_count,
        fwsec.signatures_offset
    );

    rc = nvidia::wait_for_boot_firmware(regs, nvidia::BOOT_FIRMWARE_TIMEOUT_NS);
    if (rc != nvidia::OK) {
        return rc;
    }

    nvidia::frts_region frts = {};
    rc = nvidia::place_frts_region(regs, frts);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info(
        "nvidia: FRTS region at 0x%lX, below the VGA workspace as laid out at 0x%lX, in 0x%lX bytes of usable video "
        "memory",
        frts.offset,
        frts.vga_workspace_offset,
        frts.video_memory_size
    );

    nvidia::protected_memory_layout protected_memory = {};
    rc = nvidia::lay_out_protected_memory(
        frts,
        chip->gsp_reservation,
        bootloader_header.program_size,
        gsp_image_parts.firmware_size,
        protected_memory
    );
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info(
        "nvidia: protected memory laid out in video memory from 0x%lX, with the bootloader's program at 0x%lX, the "
        "GSP's firmware at 0x%lX and its heap of 0x%lX bytes at 0x%lX, and the unprotected heap below it at 0x%lX",
        protected_memory.protected_start,
        protected_memory.bootloader_offset,
        protected_memory.firmware_offset,
        protected_memory.heap_bytes,
        protected_memory.heap_offset,
        protected_memory.unprotected_heap_offset
    );

    nvidia::fwsec_signature signature = {};
    rc = nvidia::select_fwsec_signature(regs, fwsec, signature);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info(
        "nvidia: FWSEC's signature %u of %u is the one for fuse version %u",
        signature.index + 1,
        fwsec.signature_count,
        signature.fuse_version
    );

    // NVIDIA's driver checks this before FWSEC, so FRTS is never set up for a GSP that cannot run its firmware
    if (!nvidia::has_gsp_riscv_core(regs)) {
        log::error("nvidia: the GSP has no RISC-V core to run its firmware on");
        return nvidia::ERR_BAD_STATE;
    }

    // Only the bus layer knows whether the card is coherent, and asking it takes privilege
    dma::device_profile card_profile = dma::uncached_profile(chip->dma_address_bits);
    RUN_ELEVATED(card_profile = dma_profile(chip->dma_address_bits));

    size_t image_pages = pmm::page_align_up(fwsec.size) / pmm::PAGE_SIZE;
    dma::buffer fwsec_image = {};
    RUN_ELEVATED(
        rc = dma::alloc_pages(
            card_profile,
            image_pages,
            dma::caching::uncached,
            paging::PAGE_USER,
            fwsec_image
        )
    );
    if (rc != dma::OK) {
        log::error("nvidia: allocating %lu pages for FWSEC's copy failed: %d", image_pages, rc);
        return nvidia::ERR_NO_MEMORY;
    }

    uint8_t* image = reinterpret_cast<uint8_t*>(fwsec_image.virt);
    rc = nvidia::prepare_fwsec_frts(rom, fwsec, signature, frts.offset, image);
    if (rc != nvidia::OK) {
        RUN_ELEVATED(dma::free_pages(fwsec_image));
        return rc;
    }

    log::info("nvidia: FWSEC copied into memory the card can reach and patched to fence off the FRTS region");

    // From the GSP's first reset on, `detach()` stops the GSP and frees its boot memory only once it reads as stopped
    m_must_stop_gsp = true;
    m_boot0 = boot0;

    rc = nvidia::reset_gsp_falcon(regs, boot0, nvidia::FALCON_READY_TIMEOUT_NS, nvidia::FALCON_TIMEOUT_NS);
    if (rc != nvidia::OK) {
        RUN_ELEVATED(dma::free_pages(fwsec_image));
        return rc;
    }

    // The falcon reads the copy by DMA, which the card issues only while its bus mastering is on
    RUN_ELEVATED(m_dev->enable_bus_mastering());

    // After a timeout the falcon may still read the copy, so the GSP's boot memory keeps it until `detach()` confirms
    // the GSP stopped
    nvidia::falcon_program program = nvidia::describe_fwsec_program(fwsec, fwsec_image.phys, fwsec_image.cached);
    rc = nvidia::run_gsp_falcon_program(regs, program, nvidia::FALCON_TIMEOUT_NS);
    if (rc != nvidia::OK) {
        m_boot_memory.keep_fwsec_copy(fwsec_image);
        return rc;
    }

    RUN_ELEVATED(dma::free_pages(fwsec_image));

    rc = nvidia::check_frts_result(regs, frts);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info("nvidia: FWSEC fenced off the FRTS region at 0x%lX", frts.offset);

    rc = m_boot_memory.copy_boot_files(card_profile, files, bootloader_header, gsp_image_parts);
    if (rc != nvidia::OK) {
        return rc;
    }

    rc = m_boot_memory.copy_booter_load(
        card_profile,
        files.booter_load,
        booter_load_header,
        booter_parts,
        booter_load_signature
    );
    if (rc != nvidia::OK) {
        return rc;
    }

    rc = m_boot_memory.allocate_message_queues(card_profile);
    if (rc != nvidia::OK) {
        return rc;
    }

    // Writes the page table and the command queue's header where the resource manager's arguments tell the firmware
    // to find them
    nvidia::message_queues queues(m_boot_memory.message_queues_memory(), m_boot_memory.message_queues_address());

    rc = m_boot_memory.write_protected_metadata(card_profile, frts, protected_memory, bootloader_parts);
    if (rc != nvidia::OK) {
        return rc;
    }

    rc = m_boot_memory.write_firmware_arguments(card_profile);
    if (rc != nvidia::OK) {
        return rc;
    }

    // The PCI layer's devices and `g_boot_info`'s record of the boot framebuffer sit in privileged memory
    nvidia::card_placement card = {};
    nvidia::boot_framebuffer framebuffer = {};
    RUN_ELEVATED({
        card.registers_address = m_dev->get_bar(REGISTERS_BAR).phys;
        card.bar1_address = m_dev->get_bar(VIDEO_MEMORY_BAR).phys;
        card.bar3_address = m_dev->get_bar(INSTANCE_MEMORY_BAR).phys;
        card.bus = m_dev->bus();
        card.device = m_dev->slot();
        framebuffer.address = g_boot_info.framebuffer.fb_phys;
        framebuffer.height = g_boot_info.framebuffer.height;
        framebuffer.pitch = g_boot_info.framebuffer.pitch;
    });

    uint64_t console_bytes = nvidia::boot_console_bytes(card, framebuffer, frts.has_display);

    rc = nvidia::queue_boot_messages(queues, card, console_bytes);
    if (rc != nvidia::OK) {
        log::error("nvidia: queueing the system information and the registry failed: %d", rc);
        return rc;
    }

    log::info(
        "nvidia: GSP's firmware copied into pages listed by a radix3 page table at physical address 0x%lX, the "
        "bootloader's program to 0x%lX and the firmware's signature to 0x%lX",
        m_boot_memory.radix3_address(),
        m_boot_memory.bootloader_address(),
        m_boot_memory.signature_address()
    );
    log::info(
        "nvidia: booter load's program copied with its chosen signature to physical address 0x%lX, and the message "
        "queues laid out at 0x%lX",
        m_boot_memory.booter_load_address(),
        m_boot_memory.message_queues_address()
    );
    log::info(
        "nvidia: protected memory's metadata written at physical address 0x%lX, for booter load to place at offset "
        "0x%lX in video memory",
        m_boot_memory.protected_metadata_address(),
        protected_memory.protected_start
    );
    log::info(
        "nvidia: the GSP firmware's log buffers for its init task, interrupt task and resource manager allocated at "
        "physical addresses 0x%lX, 0x%lX and 0x%lX, its resource manager's arguments written at 0x%lX and LibOS's "
        "arguments, which list them all, at 0x%lX",
        m_boot_memory.log_buffer_address(0),
        m_boot_memory.log_buffer_address(1),
        m_boot_memory.log_buffer_address(2),
        m_boot_memory.resource_manager_arguments_address(),
        m_boot_memory.libos_arguments_address()
    );
    log::info(
        "nvidia: the system information and an empty registry queued for the GSP's firmware, for the card whose BAR0, "
        "BAR1 and BAR3 lie at 0x%lX, 0x%lX and 0x%lX (0 for a BAR left unassigned), the system information asking the "
        "firmware to leave alone %lu bytes of video memory for a boot console",
        card.registers_address,
        card.bar1_address,
        card.bar3_address,
        console_bytes
    );

    rc = nvidia::reset_gsp_into_riscv(regs, nvidia::FALCON_READY_TIMEOUT_NS, nvidia::FALCON_TIMEOUT_NS);
    if (rc != nvidia::OK) {
        return rc;
    }

    // NVIDIA's driver queues both messages after the reset into the RISC-V core and writes this after each. Here both
    // were queued before that reset, so the two writes would come back to back after it, and this driver writes once.
    nvidia::notify_gsp_of_queued_messages(regs);

    nvidia::write_libos_arguments_address(regs, m_boot_memory.libos_arguments_address());

    log::info(
        "nvidia: the GSP reset into its RISC-V core, its firmware told that the two messages wait, and the address "
        "0x%lX of LibOS's arguments written into its falcon's mailboxes",
        m_boot_memory.libos_arguments_address()
    );

    log::info("nvidia: bring-up stops before booter load, leaving the GSP to be reset before its boot memory is freed");

    return DECLINED;
}

int32_t nvidia_gpu_driver::detach() {
    int32_t stop_rc = nvidia::OK;
    if (m_must_stop_gsp) {
        stop_rc = stop_gsp_and_free_boot_memory();
        m_must_stop_gsp = false;
    }

    int32_t rc = pci_driver::detach();
    if (stop_rc != nvidia::OK) {
        return stop_rc;
    }

    return rc;
}

int32_t nvidia_gpu_driver::stop_gsp_and_free_boot_memory() {
    nvidia::registers regs(m_bar_mappings[REGISTERS_BAR].va);

    int32_t rc = m_boot_memory.release_after_gsp_stop(
        regs,
        m_boot0,
        nvidia::FALCON_READY_TIMEOUT_NS,
        nvidia::FALCON_TIMEOUT_NS
    );
    if (rc != nvidia::OK) {
        log::error("nvidia: the GSP was not confirmed stopped, so all of its boot memory stays allocated for good");
        return rc;
    }

    log::info("nvidia: the GSP reads as stopped after its reset, and all of its boot memory is freed");

    return nvidia::OK;
}

void nvidia_gpu_driver::run() {
    // `attach()` never returns `OK`, so the framework never creates this task
}

__PRIVILEGED_CODE void nvidia_gpu_driver::on_interrupt(uint32_t vector) {
    // MSI is never set up, so the framework never calls this
    (void)vector;
}

// Any subclass, so VGA compatible and 3D controllers both bind
REGISTER_PCI_DRIVER(nvidia_gpu,
    PCI_MATCH(NVIDIA_VENDOR_ID, PCI_MATCH_ANY, pci::CLASS_DISPLAY, PCI_MATCH_ANY_8, PCI_MATCH_ANY_8),
    PCI_DRIVER_FACTORY(nvidia_gpu_driver));

} // namespace drivers
