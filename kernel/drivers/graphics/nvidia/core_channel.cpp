// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/core_channel.h"
#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "clock/clock.h"

namespace drivers::nvidia {

// Asks the display for the core channel's state, setting `is_idle` only for a reply naming the idle state alone, so a
// refusal leaves it clear
static int32_t ask_core_channel_idle(firmware& fw, rm::handle display, uint32_t channel_class, bool& is_idle) {
    rm::display_channel_state_params params = {};
    params.channel_class = channel_class;

    int32_t rc = fw.control(display, rm::CONTROL_GET_DISPLAY_CHANNEL_STATE, &params, sizeof(params));
    if (rc != OK && rc != ERR_FIRMWARE) {
        return rc;
    }

    is_idle = rc == OK && params.state == rm::CHANNEL_STATE_IDLE;

    return OK;
}

// Gives the firmware the push buffer of the `channel_class` channel of instance `instance`, then allocates the channel
static int32_t allocate_channel(
    firmware& fw,
    rm::handle display,
    uint32_t channel_class,
    uint8_t instance,
    uint64_t push_buffer,
    rm::handle& out
) {
    rm::display_push_buffer_params location = {};
    location.address_space = rm::ADDRESS_SPACE_SYSTEM_MEMORY;
    location.address = push_buffer;
    location.limit = PUSH_BUFFER_BYTES - 1;
    location.snoops = 1;

    location.channel_class = channel_class;
    location.channel_instance = instance;
    location.is_valid = 1;

    int32_t rc = fw.internal_control(rm::CONTROL_SET_DISPLAY_PUSH_BUFFER, &location, sizeof(location));
    if (rc != OK) {
        return rc;
    }

    rm::handle channel = fw.new_handle();
    rm::display_channel_params params = {};
    params.channel_instance = instance;

    rc = fw.allocate(display, channel, channel_class, &params, sizeof(params));
    bool firmware_may_hold_channel = rc == OK || rc == ERR_TIMEOUT || rc == ERR_BAD_MESSAGE;
    if (firmware_may_hold_channel) {
        out = channel;
    }

    return rc;
}

int32_t allocate_core_channel(
    firmware& fw,
    rm::handle display,
    const chip_display& chip,
    uint64_t push_buffer,
    rm::handle& out
) {
    return allocate_channel(fw, display, chip.core_channel_class, 0, push_buffer, out);
}

int32_t allocate_window_channel(
    firmware& fw,
    rm::handle display,
    const chip_display& chip,
    uint8_t window,
    uint64_t push_buffer,
    rm::handle& out
) {
    return allocate_channel(fw, display, chip.window_channel_class, window, push_buffer, out);
}

int32_t release_display_channel(firmware& fw, rm::handle display, rm::handle channel) {
    rm::display_release_flags_params flags = {};
    flags.flags = rm::RELEASE_PRESERVES_HARDWARE;
    int32_t flags_rc = fw.control(display, rm::CONTROL_SET_DISPLAY_RELEASE_FLAGS, &flags, sizeof(flags));

    int32_t release_rc = fw.release(channel);

    return flags_rc != OK ? flags_rc : release_rc;
}

int32_t wait_core_channel_idle(firmware& fw, rm::handle display, const chip_display& chip, bool& is_idle) {
    bool is_reported_idle = false;
    int32_t rc = ask_core_channel_idle(fw, display, chip.core_channel_class, is_reported_idle);
    if (rc != OK) {
        return rc;
    }

    // NVIDIA's clock starts at the first reply not reporting the channel idle, which is the first reply whenever the
    // loop runs
    uint64_t first_reply_ns = clock::now_ns();
    bool has_waited_out = false;
    while (!is_reported_idle && !has_waited_out) {
        rc = ask_core_channel_idle(fw, display, chip.core_channel_class, is_reported_idle);
        if (rc != OK) {
            return rc;
        }

        has_waited_out = clock::now_ns() - first_reply_ns > CORE_CHANNEL_IDLE_WAIT_NS;
    }

    is_idle = is_reported_idle;

    return OK;
}

} // namespace drivers::nvidia
