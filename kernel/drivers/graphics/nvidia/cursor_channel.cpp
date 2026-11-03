// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/cursor_channel.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

// NVIDIA's NV_PDISP_CHN_NUM_CURS__SIZE_1, the count of cursor channel numbers, as many as feHwSysCap's head bits
constexpr uint8_t CURSOR_CHANNELS_MAX = 8;

// What the free register reads once the card stops responding
constexpr uint32_t FREE_CARD_GONE = 0xFFFFFFFF;

int32_t cursor_channel::allocate(
    firmware& fw,
    registers& regs,
    rm::handle display,
    const chip_display& chip,
    uint8_t head,
    uint32_t present_heads
) {
    bool is_present = head < CURSOR_CHANNELS_MAX && (present_heads & (1u << head)) != 0;
    if (!is_present) {
        return ERR_INVALID_ARG;
    }

    rm::display_push_buffer_params location = {};
    location.channel_class = chip.cursor_channel_class;
    location.channel_instance = head;

    // A refused control ends the allocation, as it ends the core channel's, so the head lights without a cursor rather
    // than with a channel whose control the firmware refused, where NVIDIA's driver ignores the refusal
    int32_t rc = fw.internal_control(rm::CONTROL_SET_DISPLAY_PUSH_BUFFER, &location, sizeof(location));
    if (rc != OK) {
        return rc;
    }

    rm::handle channel = fw.new_handle();
    rm::cursor_channel_params params = {};
    params.channel_instance = head;

    rc = fw.allocate(display, channel, chip.cursor_channel_class, &params, sizeof(params));
    bool firmware_may_hold_channel = rc == OK || rc == ERR_TIMEOUT || rc == ERR_BAD_MESSAGE;
    if (firmware_may_hold_channel) {
        m_handle = channel;
    }

    if (rc != OK) {
        return rc;
    }

    m_page = regs.at(cursor_control_page(head));

    return OK;
}

int32_t cursor_channel::release(firmware& fw) {
    if (m_handle == 0) {
        return OK;
    }

    int32_t rc = fw.release(m_handle);
    m_handle = 0;

    return rc;
}

int32_t cursor_channel::write(uint32_t offset, uint32_t value) {
    uint32_t room = m_page.read(FREE);
    if (room == FREE_CARD_GONE) {
        return ERR_BAD_STATE;
    }

    if (room == 0) {
        return ERR_QUEUE_FULL;
    }

    m_page.write(offset, value);

    return OK;
}

} // namespace drivers::nvidia
