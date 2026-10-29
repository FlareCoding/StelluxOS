// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_active_heads.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"
#include "common/string.h"

namespace drivers::nvidia {

// Gives the display id a head shows, or 0 for a head the firmware refuses to report, and refuses a report of several
// display ids, logging both refusals
static int32_t read_shown_display(firmware& fw, rm::handle display_common, uint8_t head, uint32_t& display_id) {
    rm::active_display_params params = {};
    params.head = head;
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_ACTIVE_DISPLAY, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::error("nvidia: head %u counts as showing nothing, as display common refused to report it", head);
        display_id = 0;
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    bool is_several = (params.display_id & (params.display_id - 1)) != 0;
    if (is_several) {
        log::error(
            "nvidia: display common reports head %u showing display ids 0x%X, more than one",
            head,
            params.display_id
        );
        return ERR_BAD_MESSAGE;
    }

    display_id = params.display_id;

    return OK;
}

// Gives the serial link driving a head's display id, or counts the head as showing nothing for no link with a crossbar,
// failing, logged, on a refused request, on no link without a crossbar and on a link not among the chip's serial links
static int32_t read_head_link(
    firmware& fw,
    rm::handle display_common,
    uint8_t head,
    bool has_crossbar,
    uint8_t serial_link_count,
    active_head& state
) {
    rm::output_resource_params params = {};
    params.display_id = state.display_id;
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_OUTPUT_RESOURCE, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::error(
            "nvidia: display common refused to name the link driving head %u's display id 0x%X",
            head,
            state.display_id
        );
        return rc;
    }

    if (rc != OK) {
        return rc;
    }

    bool names_link = params.index != rm::NO_OUTPUT_RESOURCE;
    if (!names_link && has_crossbar) {
        log::error(
            "nvidia: head %u counts as showing nothing, as display common names no link for its display id 0x%X",
            head,
            state.display_id
        );
        state = {};
        return OK;
    }

    if (!names_link) {
        log::error(
            "nvidia: display common names no link for head %u's display id 0x%X and reports no crossbar",
            head,
            state.display_id
        );
        return ERR_BAD_MESSAGE;
    }

    bool is_serial_link = params.type == rm::OUTPUT_RESOURCE_TYPE_SERIAL && params.index < serial_link_count;
    if (!is_serial_link) {
        log::error(
            "nvidia: display common names link %u of NVIDIA's output resource type %u for head %u's display id 0x%X, "
            "not one of the chip's %u serial links",
            params.index,
            params.type,
            head,
            state.display_id,
            serial_link_count
        );
        return ERR_BAD_MESSAGE;
    }

    state.link_index = params.index;
    state.link_type = params.type;

    return OK;
}

int32_t read_active_heads(
    firmware& fw,
    rm::handle display_common,
    uint8_t head_count,
    bool has_crossbar,
    uint8_t serial_link_count,
    active_heads& out
) {
    active_heads heads = {};
    for (uint8_t head = 0; head < head_count; head++) {
        active_head& state = heads.heads[head];
        int32_t rc = read_shown_display(fw, display_common, head, state.display_id);
        if (rc != OK) {
            return rc;
        }

        if (state.display_id == 0) {
            continue;
        }

        rc = read_head_link(fw, display_common, head, has_crossbar, serial_link_count, state);
        if (rc != OK) {
            return rc;
        }
    }

    // An assignment of this size may call `memcpy`, which the kernel does not define
    string::memcpy(&out, &heads, sizeof(heads));

    return OK;
}

bool find_boot_head(const active_heads& heads, uint8_t& head) {
    for (uint8_t candidate = 0; candidate < DISPLAY_HEADS_MAX; candidate++) {
        if (heads.heads[candidate].display_id != 0) {
            head = candidate;
            return true;
        }
    }

    return false;
}

bool find_shown_output(const active_heads& heads, uint8_t head, const display_outputs& outputs, uint8_t& index) {
    uint32_t display_id = heads.heads[head].display_id;
    for (uint8_t candidate = 0; candidate < outputs.count; candidate++) {
        if (outputs.outputs[candidate].display_id == display_id) {
            index = candidate;
            return true;
        }
    }

    return false;
}

} // namespace drivers::nvidia
