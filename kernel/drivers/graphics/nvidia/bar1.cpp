#include "drivers/graphics/nvidia/bar1.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "common/logging.h"

namespace drivers::nvidia {

// The register that binds BAR1 to video memory. No NVIDIA source names it, so its offset and meaning are measured on
// the card, where the GSP's firmware rewrites it once resumed to send BAR1 through the firmware's own page tables
constexpr uint32_t BAR1_BLOCK = 0x1704;

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

} // namespace drivers::nvidia
