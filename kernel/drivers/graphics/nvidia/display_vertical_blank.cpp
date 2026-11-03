// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_vertical_blank.h"

namespace drivers::nvidia {

// A head's timing event, its interrupt status and its interrupt enable, one word per head (NVIDIA's
// NV_PDISP_FE_EVT_STAT_HEAD_TIMING, NV_PDISP_FE_RM_INTR_STAT_HEAD_TIMING and NV_PDISP_FE_RM_INTR_EN_HEAD_TIMING)
constexpr uint32_t HEAD_TIMING_EVENT            = 0x611800;
constexpr uint32_t HEAD_TIMING_INTERRUPT_STATUS = 0x611C00;
constexpr uint32_t HEAD_TIMING_INTERRUPT_ENABLE = 0x611D80;
constexpr uint32_t HEAD_TIMING_WORD_BYTES       = 4;

// The display's interrupt dispatch (NVIDIA's NV_PDISP_FE_RM_INTR_DISPATCH)
constexpr uint32_t INTERRUPT_DISPATCH = 0x611EC0;

// LAST_DATA's bit in a head's timing event, status and enable. Written to the event, it clears the pending event
constexpr uint32_t LAST_DATA = 1u << 1;

// A head's raster position (NVIDIA's NV_PDISP_RG_DPCA), one register in each head's 2 KiB block
constexpr uint32_t RASTER_POSITION       = 0x616330;
constexpr uint32_t RASTER_POSITION_BYTES = 2048;

static uint32_t head_timing_register(uint32_t first, uint8_t head) {
    return first + head * HEAD_TIMING_WORD_BYTES;
}

static void clear_pending_vertical_blank(registers& regs, uint8_t head) {
    uint32_t event = head_timing_register(HEAD_TIMING_EVENT, head);

    // NVIDIA's driver reads the event word before it clears the event, so the clear follows the same read
    (void)regs.read(event);
    regs.write(event, LAST_DATA);
}

static void write_vertical_blank_enable(registers& regs, uint8_t head, bool is_enabled) {
    uint32_t enable = head_timing_register(HEAD_TIMING_INTERRUPT_ENABLE, head);
    uint32_t word = regs.read(enable);

    clear_pending_vertical_blank(regs, head);

    word = is_enabled ? word | LAST_DATA : word & ~LAST_DATA;
    regs.write(enable, word);
}

void enable_vertical_blank(registers& regs, uint8_t head) {
    write_vertical_blank_enable(regs, head, true);
}

void disable_vertical_blank(registers& regs, uint8_t head) {
    write_vertical_blank_enable(regs, head, false);
}

uint32_t pending_vertical_blanks(const registers& regs) {
    return regs.read(INTERRUPT_DISPATCH);
}

bool take_vertical_blank(registers& regs, uint8_t head) {
    uint32_t status = regs.read(head_timing_register(HEAD_TIMING_INTERRUPT_STATUS, head));
    if ((status & LAST_DATA) == 0) {
        return false;
    }

    clear_pending_vertical_blank(regs, head);
    return true;
}

uint32_t raster_position(const registers& regs, uint8_t head) {
    return regs.read(RASTER_POSITION + head * RASTER_POSITION_BYTES);
}

} // namespace drivers::nvidia
