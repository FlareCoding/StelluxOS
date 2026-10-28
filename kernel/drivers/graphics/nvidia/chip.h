// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_CHIP_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_CHIP_H

#include "common/types.h"
#include "drivers/graphics/nvidia/displayport.h"

namespace drivers::nvidia {

// Byte offset in BAR0 of the register that identifies the chip
constexpr uint32_t PMC_BOOT_0 = 0x0;

// The most leaf registers (the 32-bit registers reporting a chip's interrupts to the CPU) NVIDIA's driver expects a
// chip to have, NV_MAX_INTR_LEAVES. No chip table entry exceeds it, so one top-level register serves every leaf
constexpr uint8_t INTERRUPT_LEAVES_MAX = 16;

// The most heads (the units of the GPU that each generate one monitor's timing) NVIDIA's driver allows on one GPU,
// NV_MAX_HEADS
constexpr uint8_t DISPLAY_HEADS_MAX = 4;

// The most windows (the units of the GPU that each scan one image out onto a head) NVIDIA's GPUs number,
// NV_PDISP_CHN_NUM_WIN__SIZE_1, one bit each in the 32-bit mask the firmware reports them in
constexpr uint8_t DISPLAY_WINDOWS_MAX = 32;

/**
 * The names of a chip's firmware files, which `open_firmware_files` looks up
 * in the version's directory, nvidia/<FIRMWARE_VERSION>/ in `fw::DIRECTORY`,
 * and of the GSP image's section holding the chip's signature, which booter
 * load's program checks the GSP's firmware against and `check_gsp_image`
 * finds. The GSP is the processor inside the GPU that runs NVIDIA's
 * firmware, which manages the GPU's resources, the GSP image is the file
 * holding that firmware, and the bootloader and booter load are files
 * holding small programs that start the firmware.
 */
struct chip_firmware {
    const char* gsp_image;             // file name of the GSP image in the version's directory, shared by several chips
    const char* boot_directory;        // directory in the version's directory holding the chip's bootloader and booters
    const char* gsp_signature_section; // name of the GSP image's section holding this chip's signature for the firmware
};

/**
 * The chip's values from which `lay_out_protected_memory` sizes the video
 * memory NVIDIA's driver reserves for the GSP's firmware: the firmware's heap
 * inside protected memory (the video memory fenced off for that firmware),
 * the heap the firmware keeps below protected memory, and the top of video
 * memory all of it must fit in. Only the top `prescrubbed_top_bytes` of video
 * memory is scrubbed, cleared to known values, before the firmware runs, and
 * the firmware scrubs the rest itself once it runs. The driver runs no
 * program that scrubs more beforehand, so `lay_out_protected_memory` refuses
 * a layout that does not fit in that top part.
 */
struct chip_gsp_reservation {
    uint32_t heap_os_bytes;          // the firmware heap's share for the operating system the firmware runs on
    uint32_t heap_base_bytes;        // the firmware heap's share for what the firmware allocates while it boots
    uint32_t heap_min_bytes;         // the firmware heap's minimum size, whatever its shares add up to
    uint32_t unprotected_heap_bytes; // the heap the firmware keeps below protected memory
    uint64_t prescrubbed_top_bytes;  // scrubbed before the firmware runs
};

/**
 * What every DisplayPort link of the chip can do, as the firmware reports it
 * for the one link a request names. Bring-up reads it for outputs it has
 * assigned no link, so the driver takes it from the table.
 */
struct chip_displayport {
    bool trains_phy_repeaters; // the firmware trains PHY repeaters, chips regenerating the signal before the monitor

    displayport::link_rate max_rate; // the highest rate a link runs at
};

/**
 * The chip's display engine, the part of the GPU that reads images from memory
 * and sends them to monitors, as far as the driver depends on it. Its channels
 * are the queues of commands it reads from memory. The firmware reports which
 * of the chip's windows this GPU has, as `read_present_windows` reads them,
 * and which of its heads the engine can use and the heads each window may
 * serve, as `read_display_heads` reads them.
 */
struct chip_display {
    uint8_t  head_count;         // the chip's heads, numbered from 0, at most `DISPLAY_HEADS_MAX`
    uint8_t  window_count;       // the chip's windows, numbered from 0, at most `DISPLAY_WINDOWS_MAX`
    uint32_t display_class;      // NVIDIA's class of the display object, the parent of the display engine's channels
    uint32_t core_channel_class; // NVIDIA's class of the core channel, which sets up heads, links and windows' heads

    chip_displayport displayport;
};

/**
 * What the driver knows about one chip it supports, as an entry of the chip
 * table. The table is the only record of chip-specific values, so the rest of
 * the driver looks them up with `find_chip` instead of keeping its own.
 */
struct chip_info {
    uint16_t             id;                   // as `decode_chip_id` returns it
    const char*          name;                 // NVIDIA's code name for the chip
    chip_firmware        firmware;
    uint8_t              dma_address_bits;     // physical address bits the chip reaches system memory with, 32 to 64
    chip_gsp_reservation gsp_reservation;
    uint8_t              interrupt_leaf_count; // 32-bit leaf registers reporting the chip's interrupts to the CPU
    chip_display         display;
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
