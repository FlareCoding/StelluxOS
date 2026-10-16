// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_BOOT_MESSAGES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_BOOT_MESSAGES_H

#include "drivers/graphics/nvidia/message_queues.h"
#include "common/types.h"

namespace drivers::nvidia {

// Where the card sits on the PCI bus, and the physical addresses at which the CPU reaches its BARs, numbered by their
// registers, so the BAR after the 64-bit BAR1 is BAR3
struct card_placement {
    uint64_t registers_address;       // BAR0, the card's registers
    uint64_t video_memory_address;    // BAR1, the window into video memory
    uint64_t instance_memory_address; // BAR3, which NVIDIA's driver counts as its BAR2 and calls instance memory
    uint8_t  bus;
    uint8_t  device;
};

/**
 * @brief Queue the two messages NVIDIA's driver writes to the command queue
 * before the GSP's firmware starts, in NVIDIA's order, for the firmware's
 * resource manager (its task that serves the driver's requests). NVIDIA's
 * driver waits for no reply to either.
 *
 * The first message, the system information, gives the physical addresses
 * at which the CPU reaches BAR0, BAR1 and BAR3, the card's bus and device,
 * where BAR0 mirrors the card's PCI configuration space, the CPU's
 * architecture and that no hypervisor is known, every other field 0. The
 * second, the registry, is NVIDIA's table of named settings for the resource
 * manager, sent with no entry, where NVIDIA's driver always sends some and
 * fails its boot rather than send none.
 *
 * `queue_boot_messages` never waits and touches none of the card's
 * registers, so the caller must then tell the firmware that the messages
 * wait, as NVIDIA's driver does after each one, before the GSP starts.
 * @return `OK`, or `ERR_QUEUE_FULL` when the command queue lacks room for a
 * message, as `message_queues::send` decides, since neither message is too
 * large for it. After that error for the registry, the system information
 * stays queued.
 */
int32_t queue_boot_messages(message_queues& queues, const card_placement& card);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_BOOT_MESSAGES_H
