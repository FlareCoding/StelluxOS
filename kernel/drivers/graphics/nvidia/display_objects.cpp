// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_objects.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"

namespace drivers::nvidia {

int32_t allocate_display_objects(firmware& fw, rm::handle device, const chip_display& display, display_objects& out) {
    rm::handle display_common = fw.new_handle();
    int32_t rc = fw.allocate(device, display_common, rm::CLASS_DISPLAY_COMMON, nullptr, 0);
    if (rc != OK) {
        return rc;
    }

    rm::handle bandwidth_check = fw.new_handle();
    rc = fw.allocate(device, bandwidth_check, rm::CLASS_DISPLAY_BANDWIDTH_CHECK, nullptr, 0);
    if (rc != OK) {
        fw.release(display_common);
        return rc;
    }

    rm::handle display_object = fw.new_handle();
    rc = fw.allocate(device, display_object, display.display_class, nullptr, 0);
    if (rc != OK) {
        fw.release(bandwidth_check);
        fw.release(display_common);
        return rc;
    }

    out.display_common = display_common;
    out.bandwidth_check = bandwidth_check;
    out.display = display_object;

    return OK;
}

int32_t release_display_objects(firmware& fw, const display_objects& objects) {
    int32_t display_rc = fw.release(objects.display);
    int32_t bandwidth_check_rc = fw.release(objects.bandwidth_check);
    int32_t display_common_rc = fw.release(objects.display_common);

    if (display_rc != OK) {
        return display_rc;
    }

    return bandwidth_check_rc != OK ? bandwidth_check_rc : display_common_rc;
}

int32_t read_display_capabilities(firmware& fw, const display_objects& objects, bool& has_crossbar) {
    rm::display_capabilities_params display = {};
    int32_t rc = fw.control(objects.display, rm::CONTROL_GET_DISPLAY_CAPABILITIES, &display, sizeof(display));
    if (rc != OK) {
        return rc;
    }

    uint8_t video_memory_only = display.table[rm::DISPLAY_VIDEO_MEMORY_ONLY_BYTE] & rm::DISPLAY_VIDEO_MEMORY_ONLY_BIT;
    if (video_memory_only != 0) {
        log::error(
            "nvidia: the display engine reaches push buffers, semaphores and notifiers only in video memory, and this "
            "driver does not place them there"
        );
        return ERR_BAD_STATE;
    }

    rm::display_common_capabilities_params common = {};
    rc = fw.control(objects.display_common, rm::CONTROL_GET_DISPLAY_COMMON_CAPABILITIES, &common, sizeof(common));
    if (rc != OK) {
        return rc;
    }

    has_crossbar = (common.table[rm::DISPLAY_COMMON_CROSSBAR_BYTE] & rm::DISPLAY_COMMON_CROSSBAR_BIT) != 0;

    return OK;
}

} // namespace drivers::nvidia
