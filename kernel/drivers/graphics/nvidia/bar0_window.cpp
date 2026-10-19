// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/bar0_window.h"

namespace drivers::nvidia {

// The window's register, its base in 64 KiB steps in bits 23:0 and its target in bits 25:24, where 0 is video memory
constexpr uint32_t BAR0_WINDOW         = 0x1700;
constexpr uint32_t WINDOW_BASE_MASK    = 0xFFFFFF;
constexpr uint32_t WINDOW_TARGET_MASK  = 0x3000000;
constexpr uint32_t WINDOW_TARGET_VIDEO = 0x0;
constexpr uint32_t WINDOW_STEP_SHIFT   = 16;
constexpr uint64_t OFFSET_IN_STEP_MASK = 0xFFFF;

// Where the window starts in BAR0
constexpr uint32_t PRAMIN = 0x700000;

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

} // namespace drivers::nvidia
