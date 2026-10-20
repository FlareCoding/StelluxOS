// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR0_WINDOW_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR0_WINDOW_H

#include "common/types.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

/**
 * BAR0's window is the 1 MiB of BAR0 (the card's registers) from 0x700000 that
 * reaches memory by offset, without page tables. The window's register holds
 * its base (the offset it starts at, in 64 KiB steps) and its target (video
 * memory or system memory). As the base field has 24 bits, the window can
 * start only below this value, the reach of its base.
 */
constexpr uint64_t BAR0_WINDOW_REACH = 1ull << 40;

/**
 * @brief Set BAR0's window to the 64 KiB step holding `offset`, with video
 * memory as its target, keeping the register's bits outside both fields, in
 * one write. The call never blocks. No other access by this driver to BAR0's
 * window may run between this call and the `restore_bar0_window` call that
 * writes the returned value back, since every access sets the one register.
 * NVIDIA's driver writes the two fields in turn, keeping the other bits as
 * well, so between its two writes the window has the new base with the old
 * target. NVIDIA's function for reads and writes of up to 8 bytes sets the
 * window only when the window at its cached base (the base NVIDIA's driver
 * last set, or read when it had none) does not hold the bytes.
 * @param regs A view of BAR0.
 * @param offset Bytes from the start of video memory. The offset must lie
 * below `BAR0_WINDOW_REACH`, since the step of one at or above it does not fit
 * in the base field's 24 bits, so the window would start at another step and
 * the target or the bits above it could change.
 * @return The window's register as found.
 */
uint32_t set_bar0_window(registers& regs, uint64_t offset);

/**
 * @brief Write `saved_window` back into BAR0's window register whole, its
 * target included, since NVIDIA's driver notes that the VBIOS (the boot code
 * in the card's flash) also uses the window. After NVIDIA's function for reads
 * and writes of up to 8 bytes sets the window, NVIDIA's driver writes back
 * only the base it cached before the set, with video memory as the target,
 * unless it is setting up its BAR2 (BAR3 here). Its check that BAR2 works
 * writes back the target it found. The call never blocks.
 * @param regs A view of BAR0.
 * @param saved_window The value `set_bar0_window` returned.
 */
void restore_bar0_window(registers& regs, uint32_t saved_window);

/**
 * @brief Read the 64-bit value `offset` bytes into the card's video memory
 * through BAR0's window, as two 32-bit words, low word first, as NVIDIA's
 * function for reads and writes of up to 8 bytes does. The call sets the
 * window with `set_bar0_window` for every read, even one the window already
 * holds, and restores it with `restore_bar0_window` after the second word is
 * read. It never blocks.
 * @param regs A view of BAR0.
 * @param offset Bytes from the start of video memory. The offset must be a
 * multiple of 8 (the value's size) below `BAR0_WINDOW_REACH`.
 * @return The value, with the word at `offset` in its low 32 bits.
 */
uint64_t read_video_memory_u64(registers& regs, uint64_t offset);

/**
 * @brief Write `count` 64-bit values into the card's video memory at
 * consecutive offsets from `offset` on, through BAR0's window, each as two
 * 32-bit words, the low word first at the value's offset, as NVIDIA's function
 * for reads and writes of up to 8 bytes writes one value. The call sets the
 * window with `set_bar0_window` before the first value and restores it with
 * `restore_bar0_window` after the last, so every value must lie inside the 1
 * MiB the window reaches from the 64 KiB step holding `offset`. NVIDIA's
 * function sets the window again for each value its window does not hold. The
 * call never blocks.
 * @param regs A view of BAR0.
 * @param offset Bytes from the start of video memory. The offset must be a
 * multiple of 8 (the value's size), so each word is an aligned 32-bit access
 * to BAR0, and must lie below `BAR0_WINDOW_REACH`.
 * @param values The values, `count` of them, written in order.
 * @param count How many values to write.
 * @return `OK`, or `ERR_BAD_STATE`, logged, with nothing written, when the
 * values end past the window's 1 MiB.
 */
int32_t write_video_memory(registers& regs, uint64_t offset, const uint64_t* values, uint64_t count);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR0_WINDOW_H
