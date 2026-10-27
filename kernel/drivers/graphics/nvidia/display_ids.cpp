// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_ids.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"

namespace drivers::nvidia {

static int32_t read_connected_ids(
    firmware& fw,
    rm::handle display_common,
    uint32_t supported_ids,
    uint32_t& connected_ids
) {
    rm::connected_displays_params params = {};
    params.display_mask = supported_ids;
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_CONNECTED_DISPLAYS, &params, sizeof(params));
    if (rc != OK) {
        return rc;
    }

    if ((params.display_mask & ~supported_ids) != 0) {
        log::error(
            "nvidia: display common reports display ids 0x%X connected, some of them outside the supported 0x%X",
            params.display_mask,
            supported_ids
        );
        return ERR_BAD_MESSAGE;
    }

    connected_ids = params.display_mask;

    return OK;
}

int32_t read_display_ids(firmware& fw, rm::handle display_common, display_ids& out) {
    rm::supported_displays_params supported = {};
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_SUPPORTED_DISPLAYS, &supported, sizeof(supported));
    if (rc != OK) {
        return rc;
    }

    display_ids ids = {};
    ids.supported = supported.display_mask;
    rc = read_connected_ids(fw, display_common, ids.supported, ids.connected);
    if (rc != OK) {
        return rc;
    }

    out = ids;

    return OK;
}

} // namespace drivers::nvidia
