// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_info.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"

namespace drivers::nvidia {

int32_t read_present_windows(firmware& fw, const chip_display& display, uint32_t& present_windows) {
    rm::display_static_info_params info = {};
    int32_t rc = fw.internal_control(rm::CONTROL_GET_DISPLAY_STATIC_INFO, &info, sizeof(info));
    if (rc != OK) {
        return rc;
    }

    // A shift by the mask's 32 bits is undefined, so a chip of the most windows takes the whole mask
    bool takes_whole_mask = display.window_count >= DISPLAY_WINDOWS_MAX;
    uint32_t chip_windows = takes_whole_mask ? 0xFFFFFFFF : (1u << display.window_count) - 1;
    if ((info.present_windows & ~chip_windows) != 0) {
        log::error(
            "nvidia: the GSP's firmware reports windows 0x%X present, some past the chip's %u windows",
            info.present_windows,
            display.window_count
        );
        return ERR_BAD_MESSAGE;
    }

    present_windows = info.present_windows;

    return OK;
}

} // namespace drivers::nvidia
