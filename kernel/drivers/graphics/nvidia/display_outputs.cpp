// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_outputs.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"
#include "common/string.h"

namespace drivers::nvidia {

// Sets `is_output` and fills `output` only for a display id whose signal is DisplayPort, and logs and returns `OK`
// for one whose digital facts the firmware refuses or whose signal is another
static int32_t read_output_facts(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    display_output& output,
    bool& is_output
) {
    rm::digital_display_facts_params params = {};
    params.display_id = display_id;
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_DIGITAL_DISPLAY_FACTS, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::error("nvidia: display id 0x%X is not an output, as display common refused its digital facts", display_id);
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    uint32_t signal = (params.flags >> rm::DIGITAL_SIGNAL_SHIFT) & rm::DIGITAL_SIGNAL_MASK;
    if (signal != rm::DIGITAL_SIGNAL_DISPLAYPORT) {
        log::info(
            "nvidia: display id 0x%X is not an output, as display common reports its signal %u, not DisplayPort's %u",
            display_id,
            signal,
            rm::DIGITAL_SIGNAL_DISPLAYPORT
        );
        return OK;
    }

    output.display_id = display_id;
    output.lanes_code = static_cast<uint8_t>((params.flags >> rm::DIGITAL_LANES_SHIFT) & rm::DIGITAL_LANES_MASK);
    output.link_rate_code =
        static_cast<uint8_t>((params.flags >> rm::DIGITAL_LINK_RATE_SHIFT) & rm::DIGITAL_LINK_RATE_MASK);
    output.is_limited_to_60_hz = (params.flags & rm::DIGITAL_LIMITED_TO_60_HZ) != 0;
    output.prefers_rbr = (params.flags & rm::DIGITAL_PREFERS_RBR) != 0;
    is_output = true;

    return OK;
}

int32_t read_display_outputs(firmware& fw, rm::handle display_common, const display_ids& ids, display_outputs& out) {
    display_outputs outputs = {};
    for (uint32_t bit = 0; bit < DISPLAY_IDS_MAX; bit++) {
        uint32_t display_id = 1u << bit;
        if ((ids.static_digital & display_id) == 0) {
            continue;
        }

        display_output output = {};
        bool is_output = false;
        int32_t rc = read_output_facts(fw, display_common, display_id, output, is_output);
        if (rc != OK) {
            return rc;
        }

        if (is_output) {
            output.is_connected = (ids.connected & display_id) != 0;
            outputs.outputs[outputs.count++] = output;
        }
    }

    // An assignment of this size may call `memcpy`, which the kernel does not define
    string::memcpy(&out, &outputs, sizeof(outputs));

    return OK;
}

} // namespace drivers::nvidia
