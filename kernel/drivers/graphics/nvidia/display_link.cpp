// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_link.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"

namespace drivers::nvidia {

// The step a stop before the DisplayPort facts leaves undone, ending its log line
static constexpr const char* NO_FACTS = ", so the link's DisplayPort facts are not read";

// The letter NVIDIA's names give an output resource's link
static char link_letter(uint8_t sublink) {
    return sublink == 0 ? 'A' : 'B';
}

// Whether an assignment's reply names an output resource driving `display_id` on its own or as the primary of two fed
// by two heads, and which, the last entry naming one winning as NVIDIA's modesetting module reads the reply
static bool find_assigned_resource(
    const rm::assign_output_resource_params& reply,
    uint32_t display_id,
    uint8_t& out
) {
    bool is_found = false;
    for (uint8_t index = 0; index < rm::ASSIGNED_RESOURCES_MAX; index++) {
        const rm::assigned_resource& resource = reply.resources[index];
        bool drives_display_id = (resource.display_mask & display_id) == display_id;
        bool is_primary = resource.type == rm::ASSIGNED_SINGLE || resource.type == rm::ASSIGNED_PRIMARY;
        if (drives_display_id && is_primary) {
            out = index;
            is_found = true;
        }
    }

    return is_found;
}

int32_t assign_output_link(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    bool has_crossbar,
    output_link& out,
    bool& is_assigned
) {
    is_assigned = false;

    uint8_t index = 0;
    if (has_crossbar) {
        rm::assign_output_resource_params assignment = {};
        assignment.display_id = display_id;
        int32_t rc = fw.control(display_common, rm::CONTROL_ASSIGN_OUTPUT_RESOURCE, &assignment, sizeof(assignment));
        if (rc == ERR_FIRMWARE) {
            log::error(
                "nvidia: display common refused to assign display id 0x%X an output resource%s",
                display_id,
                NO_FACTS
            );
            return OK;
        }

        if (rc != OK) {
            return rc;
        }

        if (!find_assigned_resource(assignment, display_id, index)) {
            log::error(
                "nvidia: display common's assignment for display id 0x%X names no output resource driving it on its "
                "own or as primary%s",
                display_id,
                NO_FACTS
            );
            return OK;
        }
    }

    rm::output_resource_params facts = {};
    facts.display_id = display_id;
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_OUTPUT_RESOURCE, &facts, sizeof(facts));
    if (rc == ERR_FIRMWARE) {
        log::error(
            "nvidia: display common refused the output resource facts of display id 0x%X%s",
            display_id,
            NO_FACTS
        );
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    if (!has_crossbar) {
        if (facts.type != rm::OUTPUT_RESOURCE_TYPE_SERIAL) {
            log::error(
                "nvidia: display id 0x%X is driven by output resource %u of type %u, not a serial one%s",
                display_id,
                facts.index,
                facts.type,
                NO_FACTS
            );
            return OK;
        }

        if (facts.index >= rm::ASSIGNED_RESOURCES_MAX) {
            log::error(
                "nvidia: display id 0x%X is driven by serial output resource %u, past the %u display common assigns%s",
                display_id,
                facts.index,
                rm::ASSIGNED_RESOURCES_MAX,
                NO_FACTS
            );
            return OK;
        }

        index = static_cast<uint8_t>(facts.index);
    }

    bool is_link_a = facts.protocol == rm::PROTOCOL_DISPLAYPORT_A;
    bool is_link_b = facts.protocol == rm::PROTOCOL_DISPLAYPORT_B;
    if (!is_link_a && !is_link_b) {
        log::error(
            "nvidia: display id 0x%X's output resource %u drives protocol %u, not DisplayPort%s",
            display_id,
            index,
            facts.protocol,
            NO_FACTS
        );
        return OK;
    }

    out = {
        .index   = index,
        .sublink = static_cast<uint8_t>(is_link_a ? 0 : 1),
    };

    is_assigned = true;

    log::info(
        "nvidia: display id 0x%X is driven by output resource %u through its link %c%s",
        display_id,
        out.index,
        link_letter(out.sublink),
        has_crossbar ? ", as display common assigned it" : ""
    );

    return OK;
}

int32_t read_link_capabilities(
    firmware& fw,
    rm::handle display_common,
    uint8_t resource_index,
    chip_displayport& out,
    bool& supports_1_2,
    bool& is_read
) {
    is_read = false;

    rm::link_capabilities_params params = {};
    params.resource_index = resource_index;
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_LINK_CAPABILITIES, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::error("nvidia: display common refused the DisplayPort facts of output resource %u", resource_index);
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    out = {
        .trains_phy_repeaters       = params.trains_phy_repeaters == 1,
        .max_rate                   = link_rate_of_code(params.max_rate),
        .fec_supported              = params.fec_supported == 1,
        .increased_watermark_limits = params.increased_watermark_limits == 1,
    };

    supports_1_2 = (params.versions & rm::LINK_VERSION_1_2) != 0;
    is_read = true;

    log::info(
        "nvidia: output resource %u's DisplayPort links run up to DisplayPort rate 0x%02X, %s FEC, %s the increased "
        "watermark limits and %s DisplayPort 1.2, and the firmware %s PHY repeaters",
        resource_index,
        static_cast<uint32_t>(out.max_rate),
        out.fec_supported ? "support" : "do not support",
        out.increased_watermark_limits ? "take" : "do not take",
        supports_1_2 ? "support" : "do not support",
        out.trains_phy_repeaters ? "trains" : "does not train"
    );

    return OK;
}

int32_t set_up_output_link(
    firmware& fw,
    rm::handle display_common,
    bool has_crossbar,
    const display_output& output,
    bool& is_set_up
) {
    is_set_up = false;

    output_link link = {};
    bool is_assigned = false;
    int32_t rc = assign_output_link(fw, display_common, output.display_id, has_crossbar, link, is_assigned);
    if (rc != OK || !is_assigned) {
        return rc;
    }

    chip_displayport gpu = {};
    bool supports_1_2 = false;
    return read_link_capabilities(fw, display_common, link.index, gpu, supports_1_2, is_set_up);
}

} // namespace drivers::nvidia
