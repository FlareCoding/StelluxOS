#include "drivers/graphics/nvidia/chip.h"

namespace drivers::nvidia {

constexpr uint32_t PMC_BOOT_0_CHIP_ID_SHIFT = 20;
constexpr uint32_t PMC_BOOT_0_CHIP_ID_MASK  = 0x1FF;
constexpr uint32_t PMC_BOOT_0_REVISION_MASK = 0xFF;

// Only chips verified on real hardware are listed, so another chip of the same
// architecture, such as GA104, has no entry
constexpr chip_info CHIP_TABLE[] = {
    {.id = 0x172, .name = "GA102"},
};

uint16_t decode_chip_id(uint32_t boot0) {
    return static_cast<uint16_t>((boot0 >> PMC_BOOT_0_CHIP_ID_SHIFT) & PMC_BOOT_0_CHIP_ID_MASK);
}

uint8_t decode_chip_revision(uint32_t boot0) {
    return static_cast<uint8_t>(boot0 & PMC_BOOT_0_REVISION_MASK);
}

const chip_info* find_chip(uint16_t chip_id) {
    for (const chip_info& chip : CHIP_TABLE) {
        if (chip.id == chip_id) {
            return &chip;
        }
    }

    return nullptr;
}

} // namespace drivers::nvidia
