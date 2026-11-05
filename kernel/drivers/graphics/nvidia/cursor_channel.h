// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_CURSOR_CHANNEL_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_CURSOR_CHANNEL_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

// NVIDIA's NV_UDISP_FE_CHN_ASSY_BASEADR_CURS(head), where a head's cursor channel's 4 KiB control page begins in BAR0
constexpr uint32_t CURSOR_CONTROL_PAGES      = 0x6D8000;
constexpr uint32_t CURSOR_CONTROL_PAGE_BYTES = 0x1000;

constexpr uint32_t cursor_control_page(uint8_t head) {
    return CURSOR_CONTROL_PAGES + head * CURSOR_CONTROL_PAGE_BYTES;
}

/**
 * A head's cursor channel: a channel of programmed I/O whose methods the
 * driver writes straight into the registers of its control page in BAR0, one
 * at a time, while the control page's free register shows room.
 *
 * It owns the channel's firmware object from `allocate` until `release`. It
 * takes no lock, so one context at a time must call it.
 */
class cursor_channel {
public:
    static constexpr uint32_t FREE           = 0x008; // nonzero while the channel has room for a method
    static constexpr uint32_t UPDATE         = 0x200;
    static constexpr uint32_t HOT_SPOT_POINT = 0x208; // x in bits 15:0, y in bits 31:16

    cursor_channel() = default;

    cursor_channel(const cursor_channel&) = delete;
    cursor_channel& operator=(const cursor_channel&) = delete;

    /**
     * @brief Allocate the head's channel under `display`, after the push
     * buffer control that names no push buffer. It must hold no channel.
     * @param present_heads Bit i for head i, each head the display reports.
     * @return `OK`, `ERR_INVALID_ARG` for a head outside `present_heads` or of
     * 8 or more, else the first failing request's error, logged as `firmware`
     * describes. After a late or garbled reply `release` still frees the
     * channel.
     */
    int32_t allocate(
        firmware& fw,
        registers& regs,
        rm::handle display,
        const chip_display& chip,
        uint8_t head,
        uint32_t present_heads
    );

    /**
     * @brief Release the channel's firmware object, with no flag keeping the
     * hardware's state, if the channel holds one.
     * @return `OK`, or the release's error, logged as `firmware` describes.
     */
    int32_t release(firmware& fw);

    // Whether the firmware may hold the channel, until `release` frees it
    bool holds_channel() const { return m_handle != 0; }

    /**
     * @brief Write one method into the control page when its free register
     * shows room. Never waits. The channel must be allocated.
     * @return `OK`, `ERR_QUEUE_FULL` when the register reads 0, or
     * `ERR_BAD_STATE` when it reads all ones, as a card gone reads. On an
     * error nothing is written, and neither error is logged.
     */
    int32_t write(uint32_t offset, uint32_t value);

private:
    rm::handle m_handle = 0; // nonzero while the firmware may hold the channel
    registers  m_page{0};
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_CURSOR_CHANNEL_H
