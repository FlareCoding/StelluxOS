#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_CHIP_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_CHIP_H

#include "common/types.h"

namespace drivers::nvidia {

// Byte offset in BAR0 of the register that identifies the chip
constexpr uint32_t PMC_BOOT_0 = 0x0;

/**
 * What the driver knows about one chip it supports, as an entry of the chip
 * table. The table is the only record of chip-specific values, so the rest of
 * the driver looks them up with `find_chip` instead of keeping its own.
 */
struct chip_info {
    uint16_t    id;   // as `decode_chip_id` returns it
    const char* name; // NVIDIA's code name for the chip
};

/**
 * @brief Decode the chip id from a PMC_BOOT_0 value, bits 28 to 20, such as
 * 0x172 for GA102. The id's upper five bits name the architecture a family of
 * chips shares, and its lower four bits the chip within that family.
 */
uint16_t decode_chip_id(uint32_t boot0);

/**
 * @brief Decode the chip revision from a PMC_BOOT_0 value, bits 7 to 0, such
 * as 0xa1 for revision A1.
 */
uint8_t decode_chip_revision(uint32_t boot0);

/**
 * @brief Find the chip table entry for a chip id.
 * @return The entry, which stays valid for the kernel's life, or nullptr when
 * the table does not list the chip.
 */
const chip_info* find_chip(uint16_t chip_id);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_CHIP_H
