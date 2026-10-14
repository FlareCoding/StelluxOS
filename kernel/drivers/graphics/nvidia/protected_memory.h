// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_PROTECTED_MEMORY_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_PROTECTED_MEMORY_H

#include "common/types.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

/**
 * Where FWSEC's FRTS region goes in video memory. FRTS is NVIDIA's name for a
 * FWSEC command and for the 1 MiB of data it writes: run with that command,
 * FWSEC writes the data to the region and fences the region off as the first
 * and topmost part of protected memory, the video memory reserved for the
 * GSP's firmware.
 *
 * The region sits right below the VGA workspace, the memory at the top of
 * video memory that NVIDIA's driver leaves to the VBIOS:
 *
 *   +--------------------+ <- video_memory_size, the end of usable video memory
 *   | VGA workspace      |
 *   +--------------------+ <- vga_workspace_offset
 *   | FRTS region, 1 MiB |
 *   +--------------------+ <- offset
 */
struct frts_region {
    uint64_t video_memory_size;    // bytes of usable video memory, as the card's boot firmware records it
    uint64_t vga_workspace_offset; // bytes from the start of video memory, as the layout counts it
    uint64_t offset;               // bytes from the start of video memory
};

// Bytes of the FRTS region
constexpr uint64_t FRTS_REGION_BYTES = 0x100000;

// How long the card's boot firmware may take: 50 ms to start and 2 s to finish
constexpr uint64_t BOOT_FIRMWARE_TIMEOUT_NS = 50000000 + 2000000000;

/**
 * @brief Wait until the card's boot firmware has finished. After the card is
 * reset or powered on, the GSP runs NVIDIA's boot firmware from the card's
 * ROM, which sets up video memory, records its usable size and halts the GSP.
 * The firmware has finished once it has opened its progress register to
 * reading, the progress reads complete and the GSP is halted. The wait spins
 * on the calling CPU instead of sleeping.
 * @param regs A view of BAR0.
 * @param timeout_ns How long to wait, `BOOT_FIRMWARE_TIMEOUT_NS` at bring-up.
 * The registers are read at least once, even for a timeout of 0.
 * @return `OK`, or `ERR_TIMEOUT` when the firmware has not finished within
 * `timeout_ns`. The cause is logged.
 */
int32_t wait_for_boot_firmware(const registers& regs, uint64_t timeout_ns);

/**
 * @brief Place the FRTS region as NVIDIA's driver lays out video memory when
 * more than 4 GiB of it is usable. When the chip's fuses disable its display
 * or the display's workspace register is not valid, the VGA workspace takes
 * the last 1 MiB of usable video memory. Otherwise the layout counts it as the
 * last 128 KiB, wherever the register points. NVIDIA's driver gives the last
 * 128 KiB to a valid start below the last 1 MiB and keeps only the start's low
 * 32 bits, a value below the last 1 MiB whenever more than 4 GiB is usable.
 * @param regs A view of BAR0. The card's boot firmware must have finished, as
 * `wait_for_boot_firmware` waits for, since it records the usable size.
 * @param region_out Receives the region when the result is `OK`. Its
 * `vga_workspace_offset` is then the start of the last 128 KiB or the last
 * 1 MiB of usable video memory, and the region starts at a multiple of
 * 128 KiB above offset 0 and ends at `vga_workspace_offset`.
 * @return `OK`, or `ERR_BAD_STATE` when protected memory is already set up, as
 * after an earlier bring-up with no card reset since, when usable video memory
 * is below 1 MiB, or when the VGA workspace leaves no room for the region
 * above offset 0, since NVIDIA's driver never asks FWSEC to put it at offset 0.
 * The cause is logged.
 */
int32_t place_frts_region(const registers& regs, frts_region& region_out);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_PROTECTED_MEMORY_H
