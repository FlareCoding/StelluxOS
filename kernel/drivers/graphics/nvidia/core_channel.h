// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_CORE_CHANNEL_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_CORE_CHANNEL_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

namespace drivers::nvidia {

// NVIDIA's NV_UDISP_FE_CHN_ASSY_BASEADR_CORE, where the core channel's 64 KiB area in BAR0 begins: PUT and GET and
// the state its methods assemble, then from the area's middle the armed state, what the last update took
constexpr uint32_t CORE_CHANNEL_AREA = 0x680000;

// NVIDIA's NVD_CORE_CHANNEL_DMA_ARMED_OFFSET, where the armed state begins in the core channel's area, its middle
constexpr uint32_t CORE_CHANNEL_ARMED_STATE = 0x8000;

// NVIDIA's NV_PDISP_CHN_NUM_CORE, the core channel's number among the display engine's channels, which
// `instance_memory::bind` takes to bind a context DMA to the core channel
constexpr uint32_t CORE_CHANNEL_NUMBER = 0;

// NVIDIA's NV_PDISP_CHN_NUM_WIN(window), `window`'s channel's number among the display engine's channels, which
// `instance_memory::bind` takes to bind a context DMA to that window's channel
constexpr uint32_t window_channel_number(uint8_t window) {
    return 1 + window;
}

// How long the firmware may report the core channel busy before `wait_core_channel_idle` gives up, NVIDIA's 100
// milliseconds
constexpr uint64_t CORE_CHANNEL_IDLE_WAIT_NS = 100 * NS_PER_MS;

// NVIDIA's NV_UDISP_FE_CHN_ASSY_BASEADR_WIN(0), where window 0's channel's 4 KiB area in BAR0 begins: PUT and GET and
// the state its methods assemble. Each next window's channel's area follows the one before
constexpr uint32_t WINDOW_CHANNEL_AREAS = 0x690000;
constexpr uint32_t WINDOW_CHANNEL_AREA_BYTES = 0x1000;

// Where `window`'s channel's area begins in BAR0, NVIDIA's NV_UDISP_FE_CHN_ASSY_BASEADR_WIN(window)
constexpr uint32_t window_channel_area(uint8_t window) {
    return WINDOW_CHANNEL_AREAS + window * WINDOW_CHANNEL_AREA_BYTES;
}

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
 * the firmware may hold the channel, so that `release_display_channel` can
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
 * @brief Give the firmware a window channel's push buffer, then allocate the
 * channel under the display, as `allocate_core_channel` does the core
 * channel's, with the chip's window channel class and the window as the
 * instance in both requests, as NVIDIA's modesetting module allocates each
 * window's channel. A window channel is the display channel that sets up what
 * its window scans out. The caller must check first that the window is among
 * those `read_present_windows` reports present, as NVIDIA's kernel-side driver
 * checks each window channel's instance against those windows and refuses any
 * other.
 * @param fw The driver's firmware, whose client must be allocated.
 * @param display The display object, the parent of the display engine's
 * channels.
 * @param chip The chip table's display entry, which gives the window channel's
 * class, where NVIDIA's modesetting module takes the first window channel
 * class the GPU's class list holds.
 * @param window The window whose channel is allocated, below the chip's window
 * count.
 * @param push_buffer The physical address of the channel's push buffer, which
 * must be as `allocate_core_channel` asks of the core channel's.
 * @param out Receives the channel's handle as `allocate_core_channel`'s does.
 * @return What `allocate_core_channel` returns, for this channel's requests.
 */
int32_t allocate_window_channel(
    firmware& fw,
    rm::handle display,
    const chip_display& chip,
    uint8_t window,
    uint64_t push_buffer,
    rm::handle& out
);

/**
 * @brief Release a display channel, the core channel or a window channel,
 * after asking the display to keep the display engine's hardware as it is
 * through the release, so the heads the boot firmware lit keep scanning out
 * the boot framebuffer. NVIDIA's modesetting module asks the same before
 * releasing its core, base and window channels, but only once it has restored
 * the console itself. A channel released that way stays allocated in the
 * hardware, so its push buffer must stay allocated for good, where NVIDIA's
 * modesetting module frees the push buffer right after the release.
 * @param fw The driver's firmware.
 * @param display The display object the channel was allocated under.
 * @param channel The handle `allocate_core_channel` or
 * `allocate_window_channel` gave.
 * @return `OK`, or the first error of `firmware::control` for the flag and
 * `firmware::release` for the channel, each logged as it describes. The
 * channel is released even when the flag fails, as NVIDIA's modesetting module
 * releases it, and the boot framebuffer may then go dark.
 */
int32_t release_display_channel(firmware& fw, rm::handle display, rm::handle channel);

/**
 * @brief Ask the display for the core channel's state until the firmware
 * reports the channel idle, with no method pending, or more than
 * `CORE_CHANNEL_IDLE_WAIT_NS` has passed since the first reply. NVIDIA's
 * modesetting module waits this way before it reads the core channel's armed
 * state ahead of its first update. Each request carries its parameters zeroed
 * but for the core channel's class, so it names the channel's instance 0 on
 * the device's first GPU. Only a reply naming the idle state alone reports the
 * channel idle. A request that the firmware refuses with `ERR_FIRMWARE` counts
 * as reporting the channel busy, as that module goes on past it, and any other
 * error ends the wait, where that module goes on past every failed request.
 * The requests follow one another with no pause, as that module sends them,
 * and each spins as `firmware` describes, so a channel the firmware never
 * reports idle keeps the calling CPU spinning for more than
 * `CORE_CHANNEL_IDLE_WAIT_NS`.
 * @param fw The driver's firmware.
 * @param display The display object the core channel was allocated under, by
 * `allocate_core_channel`.
 * @param chip The chip table's display entry, which gives the core channel's
 * class.
 * @param is_idle Receives on `OK` whether a reply reported the core channel
 * idle, false once the wait has given up. It is left unchanged on an error.
 * @return `OK`, or the error `firmware::control` returns for a request that
 * fails other than with `ERR_FIRMWARE`, logged as its doc describes.
 */
int32_t wait_core_channel_idle(firmware& fw, rm::handle display, const chip_display& chip, bool& is_idle);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_CORE_CHANNEL_H
