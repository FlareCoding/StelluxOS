// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/bar0_window.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"

namespace drivers::nvidia {

// The window's register, its base in 64 KiB steps in bits 23:0 and its target in bits 25:24, where 0 is video memory
constexpr uint32_t BAR0_WINDOW         = 0x1700;
constexpr uint32_t WINDOW_BASE_MASK    = 0xFFFFFF;
constexpr uint32_t WINDOW_TARGET_MASK  = 0x3000000;
constexpr uint32_t WINDOW_TARGET_VIDEO = 0x0;
constexpr uint32_t WINDOW_STEP_SHIFT   = 16;
constexpr uint64_t OFFSET_IN_STEP_MASK = 0xFFFF;

// Where the window starts in BAR0, and its size
constexpr uint32_t PRAMIN       = 0x700000;
constexpr uint64_t PRAMIN_BYTES = 0x100000;

uint32_t set_bar0_window(registers& regs, uint64_t offset) {
    uint32_t saved_window = regs.read(BAR0_WINDOW);
    uint32_t base = static_cast<uint32_t>(offset >> WINDOW_STEP_SHIFT);
    uint32_t window = (saved_window & ~(WINDOW_BASE_MASK | WINDOW_TARGET_MASK)) | base | WINDOW_TARGET_VIDEO;

    regs.write(BAR0_WINDOW, window);

    return saved_window;
}

void restore_bar0_window(registers& regs, uint32_t saved_window) {
    regs.write(BAR0_WINDOW, saved_window);
}

uint64_t read_video_memory_u64(registers& regs, uint64_t offset) {
    uint32_t low_word_bar0_offset = PRAMIN + static_cast<uint32_t>(offset & OFFSET_IN_STEP_MASK);

    uint32_t saved_window = set_bar0_window(regs, offset);
    uint32_t low = regs.read(low_word_bar0_offset);
    uint32_t high = regs.read(low_word_bar0_offset + sizeof(uint32_t));
    restore_bar0_window(regs, saved_window);

    return static_cast<uint64_t>(high) << 32 | low;
}

int32_t write_video_memory(registers& regs, uint64_t offset, const uint64_t* values, uint64_t count) {
    uint64_t offset_in_window = offset & OFFSET_IN_STEP_MASK;
    bool fits_window = count <= (PRAMIN_BYTES - offset_in_window) / sizeof(uint64_t);
    if (!fits_window) {
        log::error(
            "nvidia: %lu values from 0x%lX in video memory end past the 1 MiB that BAR0's window reaches from 0x%lX",
            count,
            offset,
            offset - offset_in_window
        );
        return ERR_BAD_STATE;
    }

    uint32_t saved_window = set_bar0_window(regs, offset);
    for (uint64_t i = 0; i < count; i++) {
        uint32_t low_word_bar0_offset = PRAMIN + static_cast<uint32_t>(offset_in_window + i * sizeof(uint64_t));
        regs.write(low_word_bar0_offset, static_cast<uint32_t>(values[i]));
        regs.write(low_word_bar0_offset + sizeof(uint32_t), static_cast<uint32_t>(values[i] >> 32));
    }

    restore_bar0_window(regs, saved_window);
    return OK;
}

} // namespace drivers::nvidia
