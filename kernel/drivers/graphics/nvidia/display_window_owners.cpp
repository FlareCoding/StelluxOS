// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_window_owners.h"
#include "drivers/graphics/nvidia/core_channel.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

// Where window 0's control lies in BAR0, in the core channel's armed state
static constexpr uint32_t ARMED_WINDOW_CONTROL = CORE_CHANNEL_AREA + CORE_CHANNEL_ARMED_STATE + rm::CORE_WINDOW_CONTROL;

window_owners read_window_owners(const registers& bar0, uint32_t present_windows) {
    window_owners owners = {};
    for (uint8_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        bool is_present = (present_windows & (1u << window)) != 0;
        if (!is_present) {
            continue;
        }

        uint32_t control = bar0.read(ARMED_WINDOW_CONTROL + window * rm::CORE_WINDOW_METHODS_STRIDE);
        owners.owners[window] = static_cast<uint8_t>(control & rm::WINDOW_CONTROL_OWNER_MASK);
    }

    return owners;
}

} // namespace drivers::nvidia
