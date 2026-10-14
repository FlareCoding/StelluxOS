// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/protected_memory.h"
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

// The GSP's falcon control register, whose bit 4 is set while the GSP is halted
constexpr uint32_t GSP_FALCON_CPUCTL = 0x110100;
constexpr uint32_t FALCON_HALTED     = 0x10;

// The bound that protected memory ends at, whose bits 31 to 4 stay 0 until protected memory is set up
constexpr uint32_t PROTECTED_MEMORY_END      = 0x1FA828;
constexpr uint32_t PROTECTED_MEMORY_END_MASK = 0xFFFFFFF0;

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
        bool is_halted = (regs.read(GSP_FALCON_CPUCTL) & FALCON_HALTED) != 0;
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

// Where the layout counts the VGA workspace as starting. `video_memory_size` must be at least `VGA_WORKSPACE_MAX_BYTES`
static uint64_t find_vga_workspace(const registers& regs, uint64_t video_memory_size) {
    bool has_display = (regs.read(DISPLAY_FUSE) & DISPLAY_DISABLED) == 0;
    bool has_valid_register = has_display && (regs.read(VGA_WORKSPACE_BASE) & VGA_WORKSPACE_VALID) != 0;
    if (!has_valid_register) {
        return video_memory_size - VGA_WORKSPACE_MAX_BYTES;
    }

    return video_memory_size - VALID_VGA_WORKSPACE_BYTES;
}

int32_t place_frts_region(const registers& regs, frts_region& region_out) {
    if ((regs.read(PROTECTED_MEMORY_END) & PROTECTED_MEMORY_END_MASK) != 0) {
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

    uint64_t vga_workspace_offset = find_vga_workspace(regs, video_memory_size);
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

    return OK;
}

} // namespace drivers::nvidia
