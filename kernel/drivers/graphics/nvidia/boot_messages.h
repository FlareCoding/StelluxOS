// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_BOOT_MESSAGES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_BOOT_MESSAGES_H

#include "drivers/graphics/nvidia/gsp.h"
#include "common/types.h"

namespace drivers::nvidia {

/**
 * Where the card sits on the PCI bus, and the physical addresses at which
 * the CPU reaches its BARs, numbered by their registers, so the BAR after
 * the 64-bit BAR1 is BAR3. An unassigned BAR's address is 0. NVIDIA's
 * driver numbers only the assigned BARs that hold memory: its framebuffer
 * BAR is BAR1, or BAR3 when BAR1 is unassigned, and its instance memory is
 * BAR3 only when BAR1 is assigned too.
 */
struct card_placement {
    uint64_t registers_address; // BAR0, the card's registers
    uint64_t bar1_address;      // where the CPU reaches video memory
    uint64_t bar3_address;
    uint8_t  bus;
    uint8_t  device;
};

// The framebuffer the boot firmware left on screen, at the physical address at which the CPU reaches it. Its address is
// 0 when the boot left none
struct boot_framebuffer {
    uint64_t address;
    uint64_t height;
    uint64_t pitch;   // bytes from the start of one row to the next
};

/**
 * @brief Work out how much of the card's video memory the GSP's firmware
 * must leave alone for a boot console (a boot framebuffer that may still be
 * drawn in), as NVIDIA's driver works it out before the firmware first runs.
 * A chip whose fuses disable its display needs none. A framebuffer at the
 * start of NVIDIA's framebuffer BAR, where a UEFI display driver sets the
 * console up, or 16 MiB into NVIDIA's instance memory, where the VBIOS sets
 * it up, needs its height times its pitch rounded up to 64 KiB. Otherwise,
 * or when that product is 0, NVIDIA's driver keeps 256 KiB, since it learns
 * whether the card booted through UEFI only from the firmware, after the
 * system information is sent. `card_placement` says which of the card's BARs
 * NVIDIA's framebuffer BAR and instance memory are.
 *
 * The rules that follow are this driver's own. An unassigned BAR holds no
 * console, so a framebuffer at the address NVIDIA's driver checks in place
 * of a BAR it lacks (physical 0 or 16 MiB) counts as lying elsewhere. A
 * framebuffer's address counts in full, where NVIDIA's driver keeps its low
 * 32 bits on a host whose kernel gives it no more. The height and the pitch
 * count in full, where NVIDIA's driver keeps 16 bits of each. The console
 * takes its height times its pitch on every architecture, where NVIDIA's
 * driver, on an aarch64 host whose kernel withholds the boot screen's
 * information from it, takes the size of the whole memory range the host's
 * framebuffer driver claims.
 * @param has_display Whether the chip's fuses leave its display enabled, as
 * `place_frts_region` reports it.
 * @return The bytes to keep, a multiple of 64 KiB, and 0 only for a chip
 * whose fuses disable its display.
 */
uint64_t boot_console_bytes(const card_placement& card, const boot_framebuffer& framebuffer, bool has_display);

/**
 * @brief Whether NVIDIA's driver keeps the boot console mapped at the start of
 * BAR1 once the GSP's firmware runs: the chip's fuses leave its display
 * enabled, so NVIDIA's driver keeps video memory for a console, and the boot
 * framebuffer lies at the start of NVIDIA's framebuffer BAR. NVIDIA's driver
 * also keeps none on a host of virtual GPUs or in a guest given one, neither
 * of which this driver supports. The rules that follow are this driver's own.
 * Only BAR1 counts, since the console is mapped through BAR1's page tables, so
 * a card without BAR1, whose BAR3 NVIDIA's driver takes as its framebuffer
 * BAR, never has its console kept there. The framebuffer's address counts in
 * full, as `boot_console_bytes` counts it, where NVIDIA's driver compares the
 * address its OS layer reports, cut to 32 bits on a host whose kernel gives it
 * no more. On an aarch64 host whose kernel withholds the boot screen's
 * information, that address is the start of the range the host's framebuffer
 * driver claims, the framebuffer's own start. A framebuffer whose height times
 * its pitch is 0 still counts, where NVIDIA's driver keeps a console for it
 * only when its OS layer still reports the base. The call never blocks.
 * @param card The card's PCI address and the addresses of its BARs.
 * @param framebuffer The framebuffer the boot firmware left on screen.
 * @param has_display Whether the chip's fuses leave its display enabled, as
 * `place_frts_region` reports it.
 * @return Whether the console is kept mapped at the start of BAR1.
 */
bool is_console_kept_at_bar1_start(const card_placement& card, const boot_framebuffer& framebuffer, bool has_display);

/**
 * @brief Queue the two messages NVIDIA's driver writes to the command queue
 * before the GSP's firmware starts, in NVIDIA's order, for the firmware's
 * resource manager (its task that serves the driver's requests). NVIDIA's
 * driver waits for no reply to either.
 *
 * The first message, the system information, gives the physical addresses at
 * which the CPU reaches BAR0, NVIDIA's framebuffer BAR and NVIDIA's instance
 * memory (`card_placement` says which BARs these are), the card's bus and
 * device, where BAR0 mirrors the card's PCI configuration space, the CPU's
 * architecture, `console_bytes` and that no hypervisor is known, every other
 * field 0. As in NVIDIA's driver, a framebuffer BAR it lacks goes out as 0,
 * and instance memory it lacks as BAR0's window into video memory, 7 MiB
 * into BAR0. The second, the registry, is NVIDIA's table of named settings
 * for the resource manager, sent with no entry, where NVIDIA's driver always
 * sends some and fails its boot rather than send none.
 *
 * `queue_boot_messages` never waits and touches none of the card's
 * registers, so the caller must then tell the firmware that the messages
 * wait, as NVIDIA's driver does after each one, before the GSP starts.
 * @param conversation The driver's `gsp`, which must already be started.
 * @param card Where the card sits on the PCI bus and the addresses at which
 * the CPU reaches its BARs.
 * @param console_bytes The bytes of video memory the firmware must leave
 * alone for a boot console, as `boot_console_bytes` works them out.
 * @return `OK`, or `ERR_QUEUE_FULL` when the command queue lacks room for a
 * message, as `gsp::send` decides, since neither message is too large for
 * it. After that error for the registry, the system information stays
 * queued.
 */
int32_t queue_boot_messages(gsp& conversation, const card_placement& card, uint64_t console_bytes);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_BOOT_MESSAGES_H
