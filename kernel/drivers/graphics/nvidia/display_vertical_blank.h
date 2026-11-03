// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_VERTICAL_BLANK_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_VERTICAL_BLANK_H

#include "common/types.h"
#include "drivers/graphics/nvidia/registers.h"

// A head's vertical blank interrupt, which the head raises at the end of each frame's visible pixels (NVIDIA's
// LAST_DATA), and its raster position. Every call touches only BAR0, takes no lock and never waits

namespace drivers::nvidia {

// What `pending_vertical_blanks` returns for a card that has stopped responding, whose registers then read all ones
constexpr uint32_t UNRESPONSIVE_DISPATCH = 0xFFFFFFFF;

// Enables `head`'s vertical blank interrupt, clearing its pending event first
void enable_vertical_blank(registers& regs, uint8_t head);

// Disables `head`'s vertical blank interrupt, clearing its pending event first
void disable_vertical_blank(registers& regs, uint8_t head);

// The display's interrupt dispatch word, bit `i` set while head `i` has a timing interrupt pending
uint32_t pending_vertical_blanks(const registers& regs);

// Clears `head`'s vertical blank when its status shows one pending, and returns whether it did
bool take_vertical_blank(registers& regs, uint8_t head);

// `head`'s raster position, the frame count in bits 31:16 and the line in bits 15:0, both counted by the head
uint32_t raster_position(const registers& regs, uint8_t head);

// The line and the frame count of a raster position
inline uint16_t raster_line(uint32_t position) { return static_cast<uint16_t>(position & 0xFFFF); }
inline uint16_t raster_frame(uint32_t position) { return static_cast<uint16_t>(position >> 16); }

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_VERTICAL_BLANK_H
