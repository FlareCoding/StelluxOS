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

// Sets `is_digital` only for a digital display id, and logs and returns `OK` for one whose kind the firmware
// refuses or is not digital
static int32_t read_is_digital(firmware& fw, rm::handle display_common, uint32_t display_id, bool& is_digital) {
    rm::display_kind_params params = {};
    params.display_id = display_id;
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_DISPLAY_KIND, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::error(
            "nvidia: display id 0x%X is left out of the digital ones, as display common refused its kind",
            display_id
        );
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    if (params.display_kind != rm::DISPLAY_KIND_DIGITAL) {
        log::info(
            "nvidia: display id 0x%X is left out of the digital ones, as display common reports its kind %u",
            display_id,
            params.display_kind
        );
        return OK;
    }

    is_digital = true;

    return OK;
}

// Sets `is_static` only for a display id the firmware reports static, and logs and returns `OK` for one it reports
// dynamic or whose dynamic flag it refuses
static int32_t read_is_static(firmware& fw, rm::handle display_common, uint32_t display_id, bool& is_static) {
    rm::output_resource_params params = {};
    params.display_id = display_id;
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_OUTPUT_RESOURCE, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::error(
            "nvidia: display id 0x%X is left out of the static ones, as display common refused to say whether it is "
            "dynamic",
            display_id
        );
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    if (params.is_dynamic != 0) {
        log::info(
            "nvidia: display id 0x%X is left out of the static ones, as display common reports it dynamic",
            display_id
        );
        return OK;
    }

    is_static = true;

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

    for (uint32_t bit = 0; bit < DISPLAY_IDS_MAX; bit++) {
        uint32_t display_id = 1u << bit;
        if ((ids.supported & display_id) == 0) {
            continue;
        }

        bool is_digital = false;
        rc = read_is_digital(fw, display_common, display_id, is_digital);
        if (rc != OK) {
            return rc;
        }

        if (!is_digital) {
            continue;
        }

        ids.digital |= display_id;
        bool is_static = false;
        rc = read_is_static(fw, display_common, display_id, is_static);
        if (rc != OK) {
            return rc;
        }

        if (is_static) {
            ids.static_digital |= display_id;
        }
    }

    out = ids;

    return OK;
}

} // namespace drivers::nvidia
