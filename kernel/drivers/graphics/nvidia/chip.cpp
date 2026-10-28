// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/chip.h"

namespace drivers::nvidia {

constexpr uint32_t PMC_BOOT_0_CHIP_ID_SHIFT = 20;
constexpr uint32_t PMC_BOOT_0_CHIP_ID_MASK  = 0x1FF;
constexpr uint32_t PMC_BOOT_0_REVISION_MASK = 0xFF;

// Only chips verified on real hardware are listed, so another chip of the same
// architecture, such as GA104, has no entry
constexpr chip_info CHIP_TABLE[] = {
    {
        .id = 0x172,
        .name = "GA102",
        .firmware = {
            .gsp_image = "gsp_ga10x.bin",
            .boot_directory = "ga102",
            .gsp_signature_section = ".fwsignature_ga10x",
        },
        .dma_address_bits = 47,
        .gsp_reservation = {
            .heap_os_bytes = 0x1400000,
            .heap_base_bytes = 0x800000,
            .heap_min_bytes = 0x5400000,
            .unprotected_heap_bytes = 0x100000,
            .prescrubbed_top_bytes = 0x10000000,
        },
        .interrupt_leaf_count = 8,
        .display = {
            .head_count = 4,
            .window_count = 8,
            .display_class = 0xC670,
            .core_channel_class = 0xC67D,
            .displayport = {
                .trains_phy_repeaters = true,
                .max_rate = displayport::link_rate::hbr3,
            },
        },
    },
};

static constexpr bool keeps_interrupt_leaves_within_max() {
    for (const chip_info& chip : CHIP_TABLE) {
        if (chip.interrupt_leaf_count > INTERRUPT_LEAVES_MAX) {
            return false;
        }
    }

    return true;
}

static constexpr bool keeps_display_windows_within_max() {
    for (const chip_info& chip : CHIP_TABLE) {
        if (chip.display.window_count > DISPLAY_WINDOWS_MAX) {
            return false;
        }
    }

    return true;
}

static constexpr bool keeps_display_heads_within_max() {
    for (const chip_info& chip : CHIP_TABLE) {
        if (chip.display.head_count > DISPLAY_HEADS_MAX) {
            return false;
        }
    }

    return true;
}

static_assert(keeps_interrupt_leaves_within_max(), "every chip's CPU interrupt tree has at most NVIDIA's 16 leaves");
static_assert(keeps_display_windows_within_max(), "every chip's windows fit the 32-bit mask the firmware reports");
static_assert(keeps_display_heads_within_max(), "every chip's display has at most the 4 heads NVIDIA's driver allows");

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
