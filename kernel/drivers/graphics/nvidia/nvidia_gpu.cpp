#include "drivers/graphics/nvidia/nvidia_gpu.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/fwsec.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/protected_memory.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/vbios.h"
#include "drivers/graphics/gfxfb.h"
#include "common/logging.h"
#include "dma/dma.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"
#include "pci/pci_class_codes.h"

namespace drivers {

constexpr uint16_t NVIDIA_VENDOR_ID = 0x10DE;

constexpr uint8_t REGISTERS_BAR = 0;

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

    rc = nvidia::check_gsp_image(files.gsp_image);
    if (rc != nvidia::OK) {
        return rc;
    }

    rc = nvidia::check_boot_file(files.bootloader, "bootloader");
    if (rc != nvidia::OK) {
        return rc;
    }

    rc = nvidia::check_boot_file(files.booter_load, "booter load");
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

    size_t image_pages = pmm::page_align_up(fwsec.size) / pmm::PAGE_SIZE;
    dma::buffer fwsec_image = {};
    RUN_ELEVATED(
        rc = dma::alloc_pages(
            dma_profile(chip->dma_address_bits),
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

    rc = nvidia::reset_gsp_falcon(regs, boot0, nvidia::FALCON_READY_TIMEOUT_NS, nvidia::FALCON_TIMEOUT_NS);
    if (rc != nvidia::OK) {
        RUN_ELEVATED(dma::free_pages(fwsec_image));
        return rc;
    }

    // The falcon reads the copy by DMA, which the card issues only while its bus mastering is on
    RUN_ELEVATED(m_dev->enable_bus_mastering());

    // After a timeout the falcon may still read the copy, so it is freed only once the falcon has halted
    nvidia::falcon_program program = nvidia::describe_fwsec_program(fwsec, fwsec_image.phys, fwsec_image.cached);
    rc = nvidia::run_gsp_falcon_program(regs, program, nvidia::FALCON_TIMEOUT_NS);
    if (rc != nvidia::OK) {
        return rc;
    }

    RUN_ELEVATED(dma::free_pages(fwsec_image));

    log::info(
        "nvidia: bring-up stops after the GSP's falcon was started on FWSEC and halted, FWSEC's result unchecked"
    );

    return DECLINED;
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
