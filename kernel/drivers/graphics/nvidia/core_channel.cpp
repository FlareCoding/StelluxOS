// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/core_channel.h"
#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

int32_t allocate_core_channel(
    firmware& fw,
    rm::handle display,
    const chip_display& chip,
    uint64_t push_buffer,
    rm::handle& out
) {
    rm::display_push_buffer_params location = {};
    location.address_space = rm::ADDRESS_SPACE_SYSTEM_MEMORY;
    location.address = push_buffer;
    location.limit = PUSH_BUFFER_BYTES - 1;
    location.snoops = 1;

    location.channel_class = chip.core_channel_class;
    location.channel_instance = 0;
    location.is_valid = 1;

    int32_t rc = fw.internal_control(rm::CONTROL_SET_DISPLAY_PUSH_BUFFER, &location, sizeof(location));
    if (rc != OK) {
        return rc;
    }

    rm::handle channel = fw.new_handle();
    rm::display_channel_params params = {};

    rc = fw.allocate(display, channel, chip.core_channel_class, &params, sizeof(params));
    bool firmware_may_hold_channel = rc == OK || rc == ERR_TIMEOUT || rc == ERR_BAD_MESSAGE;
    if (firmware_may_hold_channel) {
        out = channel;
    }

    return rc;
}

int32_t release_core_channel(firmware& fw, rm::handle display, rm::handle channel) {
    rm::display_release_flags_params flags = {};
    flags.flags = rm::RELEASE_PRESERVES_HARDWARE;
    int32_t flags_rc = fw.control(display, rm::CONTROL_SET_DISPLAY_RELEASE_FLAGS, &flags, sizeof(flags));

    int32_t release_rc = fw.release(channel);

    return flags_rc != OK ? flags_rc : release_rc;
}

} // namespace drivers::nvidia
