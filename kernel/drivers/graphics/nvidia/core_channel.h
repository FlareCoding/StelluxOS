// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_CORE_CHANNEL_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_CORE_CHANNEL_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

// NVIDIA's NV_UDISP_FE_CHN_ASSY_BASEADR_CORE, where the core channel's 64 KiB area in BAR0 begins: PUT and GET and
// the state its methods assemble, then from the area's middle the armed state, what the last update took
constexpr uint32_t CORE_CHANNEL_AREA = 0x680000;

/**
 * @brief Give the firmware the core channel's push buffer, then allocate the
 * core channel under the display, in the order NVIDIA's kernel-side driver
 * takes: the push buffer as system memory that the display engine reads with
 * snooping on, where NVIDIA's modesetting module first tries write-combined
 * memory with snooping off when the bus allows it, and the channel as instance
 * 0 with PUT and GET starting at offset 0, as NVIDIA's modesetting module
 * allocates it. The core channel is the display channel that sets up heads,
 * links and which head owns each window. The allocation names no context DMA
 * for the push buffer, since the firmware learns the push buffer from the
 * first request, where NVIDIA's modesetting module names the push buffer's
 * context DMA in its allocation and NVIDIA's kernel-side driver builds the
 * first request from it.
 * @param fw The driver's firmware, whose client must be allocated.
 * @param display The display object, the parent of the display engine's
 * channels.
 * @param chip The chip table's display entry, which gives the core channel's
 * class.
 * @param push_buffer The physical address of the channel's `PUSH_BUFFER_BYTES`
 * of push buffer, in DMA pages the card reaches that are cached only when the
 * card's DMA is coherent, which must stay allocated for good once this is
 * called, since the display engine's registers may name it from the moment its
 * address is sent, where NVIDIA's modesetting module frees the push buffer
 * when the allocation fails.
 * @param out Receives the channel's handle, from `firmware::new_handle()`, on
 * `OK` and on the allocation's `ERR_TIMEOUT` or `ERR_BAD_MESSAGE`, after which
 * the firmware may hold the channel, so that `release_core_channel` can
 * release it, where NVIDIA's modesetting module releases no channel whose
 * allocation failed. It is left unchanged on any other error.
 * @return `OK`, or the error `firmware::internal_control` returns for the push
 * buffer or `firmware::allocate` for the channel, logged as each describes. An
 * error for the push buffer ends the call before the allocation, where
 * NVIDIA's kernel-side driver drops the control's result and allocates the
 * channel.
 */
int32_t allocate_core_channel(
    firmware& fw,
    rm::handle display,
    const chip_display& chip,
    uint64_t push_buffer,
    rm::handle& out
);

/**
 * @brief Release the core channel after asking the display to keep the display
 * engine's hardware as it is through the release, so the heads the boot
 * firmware lit keep scanning out the boot framebuffer. NVIDIA's modesetting
 * module asks the same before releasing its core, base and window channels,
 * but only once it has restored the console itself. A channel released that
 * way stays allocated in the hardware, so its push buffer must stay allocated
 * for good, where NVIDIA's modesetting module frees the push buffer right
 * after the release.
 * @param fw The driver's firmware.
 * @param display The display object the channel was allocated under.
 * @param channel The handle `allocate_core_channel` gave.
 * @return `OK`, or the first error of `firmware::control` for the flag and
 * `firmware::release` for the channel, each logged as it describes. The
 * channel is released even when the flag fails, as NVIDIA's modesetting module
 * releases it, and the boot framebuffer may then go dark.
 */
int32_t release_core_channel(firmware& fw, rm::handle display, rm::handle channel);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_CORE_CHANNEL_H
