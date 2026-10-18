// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/nvidia_gpu.h"
#include "drivers/graphics/nvidia/bar1.h"
#include "drivers/graphics/nvidia/boot_messages.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/cpu_sequencer.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/fwsec.h"
#include "drivers/graphics/nvidia/gsp_boot_memory.h"
#include "drivers/graphics/nvidia/gsp_unload.h"
#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/protected_memory.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/vbios.h"
#include "drivers/graphics/gfxfb.h"
#include "boot/boot_services.h"
#include "clock/clock.h"
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

// Checks the header of `booter`, which `name` names in the log lines, and reads its layout, logging where its
// descriptors place its parts, then chooses and logs the signature its slot must hold. The outputs are set on `OK`.
static int32_t check_booter(
    const nvidia::registers& regs,
    const fw::file& booter,
    const char* name,
    nvidia::boot_file_header& header_out,
    nvidia::booter_layout& layout_out,
    nvidia::booter_signature& signature_out
) {
    int32_t rc = nvidia::check_boot_file(booter, name, header_out);
    if (rc != nvidia::OK) {
        return rc;
    }

    rc = nvidia::read_booter_layout(booter, name, header_out, layout_out);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info(
        "nvidia: %s's %u bytes of code at 0x%X and %u bytes of data at 0x%X in its %u-byte program, ucode id %u, "
        "engine mask 0x%X",
        name,
        layout_out.code_size,
        layout_out.code_offset,
        layout_out.data_size,
        layout_out.data_offset,
        header_out.program_size,
        layout_out.ucode_id,
        layout_out.engine_mask
    );

    log::info(
        "nvidia: %s's signature slot at byte 0x%X of its data, %u signatures of %u bytes at 0x%X in the file",
        name,
        layout_out.signature_slot,
        layout_out.signature_count,
        layout_out.signature_size,
        layout_out.signatures_offset
    );

    rc = nvidia::select_booter_signature(regs, layout_out, name, signature_out);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info(
        "nvidia: %s's signature %u of %u chosen with fuse version %u",
        name,
        signature_out.index + 1,
        layout_out.signature_count,
        signature_out.fuse_version
    );

    return nvidia::OK;
}

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
    nvidia::booter_layout booter_load_parts = {};
    nvidia::booter_signature booter_load_signature = {};
    rc = check_booter(
        regs,
        files.booter_load,
        "booter load",
        booter_load_header,
        booter_load_parts,
        booter_load_signature
    );
    if (rc != nvidia::OK) {
        return rc;
    }

    // NVIDIA's driver prepares booter unload at bring-up too, so a bad file fails here, before FWSEC runs
    nvidia::boot_file_header booter_unload_header = {};
    nvidia::booter_layout booter_unload_parts = {};
    nvidia::booter_signature booter_unload_signature = {};
    rc = check_booter(
        regs,
        files.booter_unload,
        "booter unload",
        booter_unload_header,
        booter_unload_parts,
        booter_unload_signature
    );
    if (rc != nvidia::OK) {
        return rc;
    }

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

    rc = nvidia::reset_falcon(
        regs,
        nvidia::falcon_id::gsp,
        boot0,
        nvidia::FALCON_READY_TIMEOUT_NS,
        nvidia::FALCON_TIMEOUT_NS
    );
    if (rc != nvidia::OK) {
        RUN_ELEVATED(dma::free_pages(fwsec_image));
        return rc;
    }

    // The falcon reads the copy by DMA, which the card issues only while its bus mastering is on
    RUN_ELEVATED(m_dev->enable_bus_mastering());

    // After a timeout the falcon may still read the copy, so the GSP's boot memory keeps it until `detach()` confirms
    // the GSP stopped
    nvidia::falcon_program program = nvidia::describe_fwsec_program(fwsec, fwsec_image.phys, fwsec_image.cached);
    rc = nvidia::run_falcon_program(regs, nvidia::falcon_id::gsp, program, nvidia::FALCON_TIMEOUT_NS);
    if (rc != nvidia::OK) {
        m_boot_memory.keep_fwsec_copy(fwsec_image);
        return rc;
    }

    RUN_ELEVATED(dma::free_pages(fwsec_image));

    // Once the falcon has halted on FWSEC, `detach()` runs FWSEC's SB command after the stops
    m_must_run_fwsec_sb = true;
    m_fwsec = fwsec;
    m_fwsec_signature = signature;
    m_chip = chip;

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
        booter_load_parts,
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
    m_queues = nvidia::message_queues(m_boot_memory.message_queues_memory(), m_boot_memory.message_queues_address());

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
    uint64_t registers_bytes = 0;
    RUN_ELEVATED({
        card.registers_address = m_dev->get_bar(REGISTERS_BAR).phys;
        registers_bytes = m_dev->get_bar(REGISTERS_BAR).size;
        card.bar1_address = m_dev->get_bar(VIDEO_MEMORY_BAR).phys;
        card.bar3_address = m_dev->get_bar(INSTANCE_MEMORY_BAR).phys;
        card.bus = m_dev->bus();
        card.device = m_dev->slot();
        framebuffer.address = g_boot_info.framebuffer.fb_phys;
        framebuffer.height = g_boot_info.framebuffer.height;
        framebuffer.pitch = g_boot_info.framebuffer.pitch;
    });

    uint64_t console_bytes = nvidia::boot_console_bytes(card, framebuffer, frts.has_display);

    rc = nvidia::queue_boot_messages(m_queues, card, console_bytes);
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

    rc = nvidia::reset_falcon(
        regs,
        nvidia::falcon_id::sec2,
        boot0,
        nvidia::FALCON_READY_TIMEOUT_NS,
        nvidia::FALCON_TIMEOUT_NS
    );
    if (rc != nvidia::OK) {
        return rc;
    }

    uint64_t metadata_address = m_boot_memory.protected_metadata_address();
    nvidia::falcon_program booter_load = nvidia::describe_booter_load_program(
        booter_load_parts,
        m_boot_memory.booter_load_address(),
        card_profile.coherent()
    );

    // From booter load's load on, SEC2 may read the boot memory and booter load may start the GSP's RISC-V core, so
    // `detach()` stops SEC2 before the GSP
    m_must_stop_sec2 = true;

    // Read before the GSP's firmware can run, for `detach()` to write back once the GSP reads as stopped
    m_bar1_block = nvidia::read_bar1_block(regs);

    rc = nvidia::run_booter_load(regs, booter_load, metadata_address, nvidia::FALCON_TIMEOUT_NS);
    if (rc != nvidia::OK) {
        return rc;
    }

    nvidia::write_gsp_app_version(regs, bootloader_parts.app_version);

    if (!nvidia::is_gsp_riscv_running(regs)) {
        log::error("nvidia: the GSP's RISC-V core does not read as running after booter load");
        return nvidia::ERR_BAD_STATE;
    }

    log::info(
        "nvidia: booter load ran on SEC2 with protected memory's metadata at 0x%lX, and the GSP's RISC-V core reads "
        "as running",
        metadata_address
    );

    rc = nvidia::link_status_queue_within(m_queues, nvidia::STATUS_QUEUE_LINK_TIMEOUT_NS);
    if (rc != nvidia::OK) {
        return rc;
    }

    // A payload may fill nearly 64 KiB, more than the boot thread's stack can spare
    uint8_t* payload = static_cast<uint8_t*>(heap::ualloc(nvidia::MESSAGE_PAYLOAD_MAX_BYTES));
    if (!payload) {
        log::error("nvidia: allocating %u bytes for a message's payload failed", nvidia::MESSAGE_PAYLOAD_MAX_BYTES);
        return nvidia::ERR_NO_MEMORY;
    }

    // NVIDIA's driver waits for the ready message in one poll whose deadline the sequencer's run counts against, and
    // ends every sequencer wait but delays and ready waits once a default timeout begun with the poll runs out
    uint64_t wait_start = clock::now_ns();
    m_sequencer = {
        .register_bytes          = registers_bytes,
        .boot0                   = boot0,
        .libos_arguments_address = m_boot_memory.libos_arguments_address(),
        .app_version             = bootloader_parts.app_version,
        .ready_timeout_ns        = nvidia::FALCON_READY_TIMEOUT_NS,
        .deadline_ns             = wait_start + nvidia::DEFAULT_TIMEOUT_NS,
    };

    nvidia::received_message ready = {};
    rc = nvidia::receive_awaited(
        m_queues,
        regs,
        m_sequencer,
        nvidia::FUNCTION_GSP_INIT_DONE,
        ready,
        payload,
        nvidia::MESSAGE_PAYLOAD_MAX_BYTES,
        wait_start + nvidia::RECEIVE_TIMEOUT_NS
    );
    heap::ufree(payload);
    if (rc != nvidia::OK) {
        return rc;
    }

    // From the ready message on, whatever its result, `detach()` asks the firmware to unload before the resets
    m_must_unload = true;

    if (ready.result != 0) {
        log::error("nvidia: the GSP's firmware reports that its initialization failed, with status 0x%X", ready.result);
        return nvidia::ERR_FIRMWARE;
    }

    log::info("nvidia: the GSP's firmware sent its ready message with %u payload bytes", ready.payload_bytes);
    log::info(
        "nvidia: bring-up stops at the firmware's ready message, leaving detach to unload the firmware, stop SEC2 and "
        "the GSP and run FWSEC's SB command"
    );

    return DECLINED;
}

int32_t nvidia_gpu_driver::detach() {
    int32_t stop_rc = nvidia::OK;
    if (m_must_stop_gsp) {
        stop_rc = tear_down_gsp();
        m_must_stop_gsp = false;
        m_must_stop_sec2 = false;
        m_must_run_fwsec_sb = false;
    }

    m_queues = nvidia::message_queues();
    m_sequencer = {};

    int32_t rc = pci_driver::detach();
    if (stop_rc != nvidia::OK) {
        return stop_rc;
    }

    return rc;
}

// Unloads the firmware, stops SEC2 and the GSP, writes BAR1's block register back and runs FWSEC's SB command, each
// as far as bring-up got. NVIDIA's driver resets SEC2 only after the SB command, to run a booter unload never run here.
int32_t nvidia_gpu_driver::tear_down_gsp() {
    nvidia::registers regs(m_bar_mappings[REGISTERS_BAR].va);

    int32_t unload_rc = nvidia::OK;
    if (m_must_unload) {
        unload_rc = unload_firmware(regs);
        m_must_unload = false;
    }

    int32_t rc = nvidia::OK;
    if (m_must_stop_sec2) {
        rc = m_boot_memory.release_after_sec2_and_gsp_stops(
            regs,
            m_boot0,
            nvidia::FALCON_READY_TIMEOUT_NS,
            nvidia::FALCON_TIMEOUT_NS
        );
        nvidia::restore_bar1_block(regs, m_bar1_block);
    } else {
        rc = m_boot_memory.release_after_gsp_stop(
            regs,
            m_boot0,
            nvidia::FALCON_READY_TIMEOUT_NS,
            nvidia::FALCON_TIMEOUT_NS
        );
    }

    if (rc != nvidia::OK) {
        log::error(
            "nvidia: a falcon that detach resets was not confirmed stopped, so all of the GSP's boot memory stays "
            "allocated for good"
        );
        return rc;
    }

    log::info("nvidia: each falcon that detach resets reads as stopped, and all of the GSP's boot memory is freed");

    if (m_must_run_fwsec_sb) {
        run_fwsec_sb(regs);
    }

    return unload_rc;
}

// The GSP's stop has just reset the falcon FWSEC runs on, and the card's bus mastering stays on from bring-up.
// NVIDIA's driver logs and asserts on a failure of the SB command without failing the unload.
void nvidia_gpu_driver::run_fwsec_sb(nvidia::registers& regs) {
    nvidia::registers rom(m_bar_mappings[REGISTERS_BAR].va + nvidia::ROM_WINDOW);

    // The copy is uncached, which is correct for the card whether or not it is coherent
    dma::device_profile card_profile = dma::uncached_profile(m_chip->dma_address_bits);
    dma::buffer kept_copy = {};
    int32_t rc = nvidia::run_fwsec_sb(
        regs,
        rom,
        m_fwsec,
        m_fwsec_signature,
        card_profile,
        nvidia::FALCON_TIMEOUT_NS,
        kept_copy
    );
    if (rc == nvidia::ERR_TIMEOUT) {
        log::error(
            "nvidia: FWSEC's copy for the SB command stays allocated for good, since the GSP's falcon may still read it"
        );
    }

    if (rc != nvidia::OK) {
        return;
    }

    log::info(
        "nvidia: the GSP's falcon halted on FWSEC's SB command, with the boot firmware's progress reading complete and "
        "the SB error code reading 0"
    );
}

// The resets that follow run whatever this returns, as NVIDIA's driver resets the GSP whatever the unload's result
int32_t nvidia_gpu_driver::unload_firmware(nvidia::registers& regs) {
    uint8_t* payload = static_cast<uint8_t*>(heap::ualloc(nvidia::MESSAGE_PAYLOAD_MAX_BYTES));
    if (!payload) {
        log::error(
            "nvidia: allocating %u bytes for the firmware's reply failed, so the GSP's firmware is not asked to unload",
            nvidia::MESSAGE_PAYLOAD_MAX_BYTES
        );
        return nvidia::ERR_NO_MEMORY;
    }

    uint64_t wait_start = clock::now_ns();
    m_sequencer.deadline_ns = wait_start + nvidia::DEFAULT_TIMEOUT_NS;
    int32_t rc = nvidia::unload_gsp_firmware(
        m_queues,
        regs,
        m_sequencer,
        payload,
        nvidia::MESSAGE_PAYLOAD_MAX_BYTES,
        wait_start + nvidia::RECEIVE_TIMEOUT_NS,
        nvidia::DEFAULT_TIMEOUT_NS
    );
    heap::ufree(payload);
    if (rc != nvidia::OK) {
        return rc;
    }

    log::info("nvidia: the GSP's firmware accepted the request to unload, and the GSP's processor reads as suspended");

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
