// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_active_heads.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"

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

int32_t read_active_heads(firmware& fw, rm::handle display_common, uint8_t head_count, active_heads& out) {
    active_heads heads = {};
    for (uint8_t head = 0; head < head_count; head++) {
        int32_t rc = read_shown_display(fw, display_common, head, heads.heads[head].display_id);
        if (rc != OK) {
            return rc;
        }
    }

    out = heads;

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
