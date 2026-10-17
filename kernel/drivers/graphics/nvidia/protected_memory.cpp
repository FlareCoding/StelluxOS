// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/protected_memory.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "hw/cpu.h"

namespace drivers::nvidia {

// The boot firmware's progress register, readable once bit 0 of its read protection register is set
constexpr uint32_t BOOT_PROGRESS_READ_PROTECTION = 0x118128;
constexpr uint32_t BOOT_PROGRESS                 = 0x118234;
constexpr uint32_t READABLE_AT_LEVEL_0           = 0x1;
constexpr uint32_t BOOT_PROGRESS_MASK            = 0xFF;
constexpr uint32_t BOOT_PROGRESS_COMPLETE        = 0xFF;

// The lower and upper bounds of protected memory, each holding an address in 4 KiB units in bits 31 to 4. The upper
// bound's address stays 0 until protected memory is set up.
constexpr uint32_t PROTECTED_MEMORY_LOWER_BOUND = 0x1FA824;
constexpr uint32_t PROTECTED_MEMORY_UPPER_BOUND = 0x1FA828;
constexpr uint32_t BOUND_ADDRESS_SHIFT          = 4;
constexpr uint32_t BOUND_UNIT_SHIFT             = 12;

// A VBIOS scratch register, one of those the VBIOS's programs leave results in, whose bits 31 to 16 hold the error
// code FWSEC records for the FRTS command, 0 for none
constexpr uint32_t FRTS_ERROR_SCRATCH    = 0x1438;
constexpr uint32_t FRTS_ERROR_CODE_SHIFT = 16;

// Where the boot firmware records the usable size of video memory, in MiB
constexpr uint32_t USABLE_VIDEO_MEMORY_MIB = 0x1183A4;
constexpr uint32_t MIB_SHIFT               = 20;

// The fuse whose bit 0 is set when the chip's display is disabled
constexpr uint32_t DISPLAY_FUSE     = 0x820C04;
constexpr uint32_t DISPLAY_DISABLED = 0x1;

// The display's workspace register, whose bit 3 marks it valid
constexpr uint32_t VGA_WORKSPACE_BASE  = 0x625F04;
constexpr uint32_t VGA_WORKSPACE_VALID = 0x8;

// The most the VGA workspace takes, which it takes without a valid register, and what it takes with one
constexpr uint64_t VGA_WORKSPACE_MAX_BYTES   = 0x100000;
constexpr uint64_t VALID_VGA_WORKSPACE_BYTES = 0x20000;

// The firmware heap's share for each GiB of video memory, with the total rounded up to a whole MiB, and its share for
// the firmware's records of up to 2048 channels
constexpr uint32_t HEAP_BYTES_PER_GIB  = 0x18000;
constexpr uint32_t HEAP_SHARE_ROUNDING = 0x100000;
constexpr uint32_t HEAP_CHANNEL_BYTES  = 0x6000000;

// The boundaries the parts below the FRTS region start on, the last one also for protected memory's start and the
// unprotected heap
constexpr uint64_t BOOTLOADER_ALIGNMENT = 0x1000;
constexpr uint64_t FIRMWARE_ALIGNMENT   = 0x10000;
constexpr uint64_t HEAP_ALIGNMENT       = 0x100000;

constexpr uint32_t GIB_SHIFT = 30;
constexpr uint64_t NS_PER_MS = 1000000;

static void log_unfinished_boot_firmware(uint64_t timeout_ns, bool is_readable, uint32_t progress, bool is_halted) {
    if (!is_readable) {
        log::error(
            "nvidia: the card's boot firmware did not finish within %lu ms: its progress is still read-protected",
            timeout_ns / NS_PER_MS
        );
        return;
    }

    log::error(
        "nvidia: the card's boot firmware did not finish within %lu ms: its progress reads 0x%02X of 0xFF, and the "
        "GSP %s",
        timeout_ns / NS_PER_MS,
        progress,
        is_halted ? "is halted" : "has not halted"
    );
}

int32_t wait_for_boot_firmware(const registers& regs, uint64_t timeout_ns) {
    uint64_t deadline = clock::now_ns() + timeout_ns;

    while (true) {
        bool is_readable = (regs.read(BOOT_PROGRESS_READ_PROTECTION) & READABLE_AT_LEVEL_0) != 0;
        uint32_t progress = is_readable ? regs.read(BOOT_PROGRESS) & BOOT_PROGRESS_MASK : 0;
        bool is_halted = is_gsp_falcon_halted(regs);
        bool is_complete = progress == BOOT_PROGRESS_COMPLETE;
        if (is_complete && is_halted) {
            return OK;
        }

        if (clock::now_ns() > deadline) {
            log_unfinished_boot_firmware(timeout_ns, is_readable, progress, is_halted);
            return ERR_TIMEOUT;
        }

        cpu::relax();
    }
}

static bool is_protected_memory_set_up(const registers& regs) {
    return (regs.read(PROTECTED_MEMORY_UPPER_BOUND) >> BOUND_ADDRESS_SHIFT) != 0;
}

// Where the layout counts the VGA workspace as starting. `video_memory_size` must be at least `VGA_WORKSPACE_MAX_BYTES`
static uint64_t find_vga_workspace(const registers& regs, uint64_t video_memory_size, bool has_display) {
    bool has_valid_register = has_display && (regs.read(VGA_WORKSPACE_BASE) & VGA_WORKSPACE_VALID) != 0;
    if (!has_valid_register) {
        return video_memory_size - VGA_WORKSPACE_MAX_BYTES;
    }

    return video_memory_size - VALID_VGA_WORKSPACE_BYTES;
}

int32_t place_frts_region(const registers& regs, frts_region& region_out) {
    if (is_protected_memory_set_up(regs)) {
        log::error("nvidia: protected memory is already set up, as after an earlier bring-up with no card reset since");
        return ERR_BAD_STATE;
    }

    uint64_t video_memory_size = static_cast<uint64_t>(regs.read(USABLE_VIDEO_MEMORY_MIB)) << MIB_SHIFT;
    if (video_memory_size < VGA_WORKSPACE_MAX_BYTES) {
        log::error(
            "nvidia: %lu MiB of usable video memory cannot hold 1 MiB of VGA workspace",
            video_memory_size >> MIB_SHIFT
        );
        return ERR_BAD_STATE;
    }

    bool has_display = (regs.read(DISPLAY_FUSE) & DISPLAY_DISABLED) == 0;
    uint64_t vga_workspace_offset = find_vga_workspace(regs, video_memory_size, has_display);
    if (vga_workspace_offset <= FRTS_REGION_BYTES) {
        log::error(
            "nvidia: the VGA workspace at 0x%lX leaves no room for the 1 MiB FRTS region above offset 0",
            vga_workspace_offset
        );
        return ERR_BAD_STATE;
    }

    region_out.video_memory_size = video_memory_size;
    region_out.vga_workspace_offset = vga_workspace_offset;
    region_out.offset = vga_workspace_offset - FRTS_REGION_BYTES;
    region_out.has_display = has_display;

    return OK;
}

static uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

// The highest offset on an `alignment` boundary at which `bytes` bytes end at or below `top`, false when that offset
// would lie below 0
static bool place_below(uint64_t top, uint64_t bytes, uint64_t alignment, uint64_t& offset_out) {
    if (bytes > top) {
        return false;
    }

    offset_out = align_down(top - bytes, alignment);
    return true;
}

// Summed in 32 bits as NVIDIA's driver sums it, so a sum past 4 GiB wraps as NVIDIA's does, which takes about 41 TiB
// of video memory, before it is raised to the chip's minimum
static uint64_t firmware_heap_bytes(const chip_gsp_reservation& reservation, uint64_t video_memory_size) {
    uint64_t gib = static_cast<uint64_t>(1) << GIB_SHIFT;
    uint32_t video_memory_gib = static_cast<uint32_t>((video_memory_size + gib - 1) >> GIB_SHIFT);
    uint32_t video_memory_share = HEAP_BYTES_PER_GIB * video_memory_gib;
    uint32_t rounded_share = (video_memory_share + HEAP_SHARE_ROUNDING - 1) & ~(HEAP_SHARE_ROUNDING - 1);
    uint32_t sum = reservation.heap_os_bytes + reservation.heap_base_bytes + rounded_share + HEAP_CHANNEL_BYTES;

    return sum > reservation.heap_min_bytes ? sum : reservation.heap_min_bytes;
}

int32_t lay_out_protected_memory(
    const frts_region& region,
    const chip_gsp_reservation& reservation,
    uint64_t bootloader_bytes,
    uint64_t firmware_bytes,
    protected_memory_layout& layout_out
) {
    uint64_t video_memory_size = region.video_memory_size;
    uint64_t prescrubbed_bytes = reservation.prescrubbed_top_bytes;
    if (prescrubbed_bytes > video_memory_size) {
        prescrubbed_bytes = video_memory_size;
    }

    uint64_t computed_heap_bytes = firmware_heap_bytes(reservation, video_memory_size);
    uint64_t bootloader_offset = 0;
    uint64_t firmware_offset = 0;
    uint64_t heap_offset = 0;
    uint64_t protected_start = 0;
    uint64_t unprotected_heap_offset = 0;

    bool is_placed =
        place_below(region.offset, bootloader_bytes, BOOTLOADER_ALIGNMENT, bootloader_offset) &&
        place_below(bootloader_offset, firmware_bytes, FIRMWARE_ALIGNMENT, firmware_offset) &&
        place_below(firmware_offset, computed_heap_bytes, HEAP_ALIGNMENT, heap_offset) &&
        place_below(heap_offset, PROTECTED_METADATA_BYTES, HEAP_ALIGNMENT, protected_start) &&
        place_below(protected_start, reservation.unprotected_heap_bytes, HEAP_ALIGNMENT, unprotected_heap_offset);
    bool is_in_prescrubbed_top = unprotected_heap_offset >= video_memory_size - prescrubbed_bytes;
    if (!is_placed || !is_in_prescrubbed_top) {
        log::error(
            "nvidia: protected memory and the heap below it, for a %lu-byte bootloader program, %lu bytes of GSP "
            "firmware and a %lu-byte firmware heap, do not fit in the last 0x%lX of the 0x%lX bytes of usable video "
            "memory, the part cleared before the firmware runs",
            bootloader_bytes,
            firmware_bytes,
            computed_heap_bytes,
            prescrubbed_bytes,
            video_memory_size
        );
        return ERR_BAD_STATE;
    }

    layout_out.bootloader_offset = bootloader_offset;
    layout_out.firmware_offset = firmware_offset;
    layout_out.heap_offset = heap_offset;
    layout_out.heap_bytes = align_down(firmware_offset - heap_offset, HEAP_ALIGNMENT);

    layout_out.protected_start = protected_start;
    layout_out.unprotected_heap_offset = unprotected_heap_offset;
    layout_out.unprotected_heap_bytes = reservation.unprotected_heap_bytes;

    return OK;
}

int32_t check_frts_result(const registers& regs, const frts_region& region) {
    uint32_t error_code = regs.read(FRTS_ERROR_SCRATCH) >> FRTS_ERROR_CODE_SHIFT;
    if (error_code != 0) {
        log::error("nvidia: FWSEC's FRTS error code reads 0x%04X", error_code);
        return ERR_BAD_STATE;
    }

    if (!is_protected_memory_set_up(regs)) {
        log::error("nvidia: FWSEC did not set up protected memory for the FRTS region at 0x%lX", region.offset);
        return ERR_BAD_STATE;
    }

    // NVIDIA's driver compares the start with the region's offset in 4 KiB units kept to 32 bits
    uint32_t start_4k = regs.read(PROTECTED_MEMORY_LOWER_BOUND) >> BOUND_ADDRESS_SHIFT;
    uint32_t region_4k = static_cast<uint32_t>(region.offset >> BOUND_UNIT_SHIFT);
    if (start_4k != region_4k) {
        log::error(
            "nvidia: protected memory's lower bound reads address 0x%lX instead of the FRTS region's 0x%lX",
            static_cast<uint64_t>(start_4k) << BOUND_UNIT_SHIFT,
            region.offset
        );
        return ERR_BAD_STATE;
    }

    return OK;
}

} // namespace drivers::nvidia
