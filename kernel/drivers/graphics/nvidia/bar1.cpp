// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/bar1.h"
#include "drivers/graphics/nvidia/bar0_window.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "hw/cpu.h"

namespace drivers::nvidia {

// The register that binds BAR1 to video memory. No NVIDIA source names it, so its offset and meaning are measured on
// the card, where the GSP's firmware rewrites it once resumed to send BAR1 through the firmware's own page tables
constexpr uint32_t BAR1_BLOCK = 0x1704;

// The invalidate registers, in the block of registers a virtual function (a PCI function the card gives a virtual
// machine) has, which NVIDIA's driver accesses from 0xB80000 on a card that is not a virtual function
constexpr uint32_t INVALIDATE_PDB       = 0xB830A0;
constexpr uint32_t INVALIDATE_UPPER_PDB = 0xB830A4;
constexpr uint32_t INVALIDATE           = 0xB830B0;

// The root's offset in 4 KiB units, whose low 28 bits fill bits 31:4 of the first register and whose other bits fill
// bits 19:0 of the second, and the first register's aperture field in bit 1, 0 for video memory
constexpr uint32_t PDB_UNIT_SHIFT           = 12;
constexpr uint32_t PDB_LOW_UNIT_BITS        = 28;
constexpr uint64_t PDB_LOW_UNIT_MASK        = 0xFFFFFFF;
constexpr uint32_t PDB_ADDRESS_FIELD_SHIFT  = 4;
constexpr uint32_t PDB_APERTURE_FIELD_SHIFT = 1;
constexpr uint32_t PDB_APERTURE_VIDEO       = 0x0;

// The invalidate's fields: every offset in the space, the hub's TLB alone, scope 2 in bits 16:15, which excludes the
// TLBs of NVLink (NVIDIA's link between GPUs), and the trigger, which the card clears once the invalidate is done
constexpr uint32_t INVALIDATE_ALL_VA       = 0x1;
constexpr uint32_t INVALIDATE_HUB_TLB_ONLY = 0x4;
constexpr uint32_t INVALIDATE_NON_LINK     = 0x10000;
constexpr uint32_t INVALIDATE_TRIGGER      = 0x80000000;

// Spins until the trigger reads clear or `deadline` passes, reading it at least once, and returns whether it cleared
static bool wait_for_invalidate(const registers& regs, uint64_t deadline) {
    while (true) {
        if ((regs.read(INVALIDATE) & INVALIDATE_TRIGGER) == 0) {
            return true;
        }

        if (clock::now_ns() > deadline) {
            return false;
        }

        cpu::relax();
    }
}

uint32_t read_bar1_block(const registers& regs) {
    return regs.read(BAR1_BLOCK);
}

void restore_bar1_block(registers& regs, uint32_t saved_block) {
    uint32_t block = regs.read(BAR1_BLOCK);
    if (block == saved_block) {
        return;
    }

    bool is_gsp_stopped = is_falcon_halted(regs, falcon_id::gsp) && !is_gsp_riscv_running(regs);
    if (!is_gsp_stopped) {
        log::error(
            "nvidia: BAR1's block register reads 0x%X, not the 0x%X it read before the GSP's firmware ran, and stays "
            "unwritten since the GSP does not read as stopped",
            block,
            saved_block
        );
        return;
    }

    regs.write(BAR1_BLOCK, saved_block);
    uint32_t written_block = regs.read(BAR1_BLOCK);
    if (written_block != saved_block) {
        log::error(
            "nvidia: BAR1's block register, written back to 0x%X once the GSP stopped, reads 0x%X",
            saved_block,
            written_block
        );
        return;
    }

    log::info(
        "nvidia: BAR1's block register, which read 0x%X before the GSP's firmware ran and 0x%X once the GSP stopped, "
        "written back",
        saved_block,
        block
    );
}

int32_t invalidate_bar1_tlb(registers& regs, uint64_t root_offset, uint64_t timeout_ns) {
    uint64_t deadline = clock::now_ns() + timeout_ns;
    uint64_t root_unit = root_offset >> PDB_UNIT_SHIFT;
    uint32_t pdb_address_field = static_cast<uint32_t>(root_unit & PDB_LOW_UNIT_MASK) << PDB_ADDRESS_FIELD_SHIFT;
    uint32_t pdb = pdb_address_field | PDB_APERTURE_VIDEO << PDB_APERTURE_FIELD_SHIFT;
    uint32_t upper_pdb = static_cast<uint32_t>(root_unit >> PDB_LOW_UNIT_BITS);
    uint32_t invalidate = INVALIDATE_TRIGGER | INVALIDATE_ALL_VA | INVALIDATE_HUB_TLB_ONLY | INVALIDATE_NON_LINK;

    if (!wait_for_invalidate(regs, deadline)) {
        log::error(
            "nvidia: the invalidate register's trigger bit still reads set after %lu ms of waiting for an earlier "
            "invalidate to finish, so BAR1's cached translations are not invalidated",
            timeout_ns / NS_PER_MS
        );
        return ERR_TIMEOUT;
    }

    regs.write(INVALIDATE_PDB, pdb);
    regs.write(INVALIDATE_UPPER_PDB, upper_pdb);
    regs.write(INVALIDATE, invalidate);

    if (!wait_for_invalidate(regs, deadline)) {
        log::error(
            "nvidia: the invalidate register's trigger bit still reads set %lu ms after the wait for an earlier "
            "invalidate began, with the invalidate of BAR1's cached translations through the root page directory at "
            "0x%lX in video memory asked for",
            timeout_ns / NS_PER_MS,
            root_offset
        );
        return ERR_TIMEOUT;
    }

    return OK;
}

int32_t link_console_page_tables(registers& regs, uint64_t root_offset, uint64_t root_entry, uint64_t timeout_ns) {
    int32_t rc = write_video_memory(regs, root_offset, &root_entry, 1);
    if (rc != OK) {
        return rc;
    }

    uint64_t written_entry = read_video_memory_u64(regs, root_offset);
    if (written_entry != root_entry) {
        log::error(
            "nvidia: the first entry of BAR1's root page directory at 0x%lX in video memory, written as 0x%lX to name "
            "the console's tables, reads back as 0x%lX",
            root_offset,
            root_entry,
            written_entry
        );
        return ERR_BAD_STATE;
    }

    log::info(
        "nvidia: the first entry of BAR1's root page directory at 0x%lX in video memory now reads 0x%lX, naming the "
        "console's tables",
        root_offset,
        written_entry
    );

    return invalidate_bar1_tlb(regs, root_offset, timeout_ns);
}

} // namespace drivers::nvidia
