// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_PROTECTED_MEMORY_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_PROTECTED_MEMORY_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
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
    bool     has_display;          // whether the chip's fuses leave its display enabled, as the layout read them
};

/**
 * Where the rest of protected memory and the heap below it go in video
 * memory, as NVIDIA's driver lays them out below the FRTS region. Protected
 * memory runs from `protected_start` up to the VGA workspace, and the
 * unprotected heap lies below it. Booter load, the program SEC2 (the GPU's
 * security processor) runs to boot the GSP, reads the bootloader's program
 * and the GSP's firmware from the driver's copies in system memory, writes
 * them to their offsets and places protected memory's metadata, which
 * records this layout and where those copies lie, at `protected_start`. The
 * firmware keeps its heaps where the layout puts them. Every offset counts
 * bytes from the start of video memory:
 *
 *   +----------------------+ <- the VGA workspace's start
 *   | FRTS region          |
 *   +----------------------+ <- the FRTS region's offset
 *   | bootloader's program |
 *   +----------------------+ <- bootloader_offset
 *   | GSP's firmware       |
 *   +----------------------+ <- firmware_offset
 *   | firmware's heap      |
 *   +----------------------+ <- heap_offset
 *   | metadata             |
 *   +----------------------+ <- protected_start
 *   | unprotected heap     |
 *   +----------------------+ <- unprotected_heap_offset
 *
 * Each part starts at its offset and ends at or below the start of the part
 * above it, so rounding can leave a gap between them. NVIDIA's driver counts
 * all of video memory from `unprotected_heap_offset` up, the VGA workspace
 * included, as reserved for the firmware.
 */
struct protected_memory_layout {
    uint64_t bootloader_offset;
    uint64_t firmware_offset;
    uint64_t heap_offset;
    uint64_t heap_bytes;              // a whole number of MiB
    uint64_t protected_start;
    uint64_t unprotected_heap_offset;
    uint64_t unprotected_heap_bytes;
};

// Bytes of the FRTS region
constexpr uint64_t FRTS_REGION_BYTES = 0x100000;

// Bytes of protected memory's metadata, which protected memory's start leaves room for below the heap
constexpr uint64_t PROTECTED_METADATA_BYTES = 256;

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
 * `vga_workspace_offset` is then the start of the last 128 KiB or the last 1
 * MiB of usable video memory, and the region starts at a multiple of 128 KiB
 * above offset 0 and ends at `vga_workspace_offset`. The region thus lies below
 * 16 TiB whenever usable video memory does, as `prepare_fwsec_frts` and
 * `check_frts_result` need. Its `has_display` is false when the chip's fuses
 * disable its display.
 * @return `OK`, or `ERR_BAD_STATE` when protected memory is already set up, as
 * after an earlier bring-up with no card reset since, when usable video memory
 * is below 1 MiB, or when the VGA workspace leaves no room for the region
 * above offset 0, since NVIDIA's driver never asks FWSEC to put it at offset 0.
 * The cause is logged.
 */
int32_t place_frts_region(const registers& regs, frts_region& region_out);

/**
 * @brief Lay out the rest of protected memory below the FRTS region, and
 * the unprotected heap below it, as NVIDIA's driver does on its first
 * attempt to boot the GSP. The bootloader's program goes at the highest
 * 4 KiB boundary where it ends at or below the region, and the firmware at
 * the highest 64 KiB boundary where it ends at or below the program. The
 * heap, protected memory's start and the unprotected heap each go at the
 * highest 1 MiB boundary where they end at or below the part above them, the
 * start leaving room below the heap for `PROTECTED_METADATA_BYTES` bytes of
 * metadata. The heap is placed for the sum, in 32 bits as NVIDIA's driver
 * computes it, of `reservation.heap_os_bytes`, `reservation.heap_base_bytes`,
 * 96 KiB for each whole or part GiB of video memory with that share rounded
 * up to 1 MiB, and 96 MiB for the firmware's records of up to 2048 channels
 * (queues of work for the GPU's engines), raised to
 * `reservation.heap_min_bytes` when below it. `heap_bytes` is then the room
 * from `heap_offset` to the firmware rounded down to 1 MiB, and
 * `unprotected_heap_bytes` is `reservation.unprotected_heap_bytes`. No
 * register is read.
 * @param region Must be the FRTS region as `place_frts_region` placed it.
 * @param reservation The chip's values from the chip table.
 * @param bootloader_bytes Bytes of the bootloader's program.
 * @param firmware_bytes Bytes of the GSP's firmware.
 * @param layout_out Receives the layout when the result is `OK`, and is left
 * unchanged otherwise. Every part then lies in the top
 * `reservation.prescrubbed_top_bytes` of video memory.
 * @return `OK`, or `ERR_BAD_STATE` when the memory from the unprotected
 * heap's start to the end of video memory would be larger than
 * `reservation.prescrubbed_top_bytes` or than video memory itself. NVIDIA's
 * driver fails the first on a chip without a program to scrub more, and the
 * check against video memory itself is this driver's own. The cause is
 * logged.
 */
int32_t lay_out_protected_memory(
    const frts_region& region,
    const chip_gsp_reservation& reservation,
    uint64_t bootloader_bytes,
    uint64_t firmware_bytes,
    protected_memory_layout& layout_out
);

/**
 * @brief Check that FWSEC fenced off the FRTS region, as NVIDIA's driver
 * checks once the GSP's falcon has halted on FWSEC, reading each register
 * once. The error code FWSEC records for the FRTS command must be 0,
 * protected memory must be set up, and it must start at the region.
 * @param regs A view of BAR0. `run_falcon_program` must have returned `OK`
 * for FWSEC's copy as `prepare_fwsec_frts` patched it for `region`.
 * @param region The region FWSEC's copy was patched to fence off, as
 * `place_frts_region` placed it. Its offset must lie below 16 TiB, as for
 * `prepare_fwsec_frts`, since the check keeps it in 4 KiB units to 32 bits,
 * as NVIDIA's driver does.
 * @return `OK`, or `ERR_BAD_STATE` when FWSEC's FRTS error code is not 0,
 * protected memory is not set up, or it starts elsewhere than at the region.
 * The cause is logged.
 */
int32_t check_frts_result(const registers& regs, const frts_region& region);

/**
 * @brief Check that the registers show no failure of FWSEC's SB command, as
 * NVIDIA's driver checks once the GSP's falcon has halted on FWSEC, reading
 * each register once. The boot firmware's progress register must be open to
 * reading and read complete, as `wait_for_boot_firmware` also requires, and
 * the error code FWSEC records for the SB command must be 0.
 * @param regs A view of BAR0. `run_falcon_program` must have returned `OK` for
 * FWSEC's copy as `prepare_fwsec_sb` patched it.
 * @return `OK`, or `ERR_BAD_STATE` when the progress register is
 * read-protected or does not read complete, or FWSEC's SB error code is not 0.
 * The cause is logged.
 */
int32_t check_fwsec_sb_result(const registers& regs);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_PROTECTED_MEMORY_H
