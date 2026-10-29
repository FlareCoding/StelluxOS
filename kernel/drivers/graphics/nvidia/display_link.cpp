// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_link.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "hw/cpu.h"

namespace drivers::nvidia {

// The steps a stop leaves undone, ending its log line: before the facts are read, before the training, at a failed
// check and at a refused stream
static constexpr const char* UNREAD        = ", so the link's DisplayPort facts are not read and the link is neither "
                                             "trained nor given a stream";
static constexpr const char* UNTRAINED     = ", so the link is neither trained nor given a stream";
static constexpr const char* NO_STREAM     = ", so the link is given no stream";
static constexpr const char* NO_ATTRIBUTES = ", so the stream's attributes are not overridden";

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
                UNREAD
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
                UNREAD
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
            UNREAD
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
                UNREAD
            );
            return OK;
        }

        if (facts.index >= rm::ASSIGNED_RESOURCES_MAX) {
            log::error(
                "nvidia: display id 0x%X is driven by serial output resource %u, past the %u display common assigns%s",
                display_id,
                facts.index,
                rm::ASSIGNED_RESOURCES_MAX,
                UNREAD
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
            UNREAD
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
        log::error(
            "nvidia: display common refused the DisplayPort facts of output resource %u%s",
            resource_index,
            UNTRAINED
        );
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

// Spins on the calling CPU for `ms` milliseconds, since `display_engine::set_up` may run on a thread that cannot sleep
static void spin_for_ms(uint64_t ms) {
    uint64_t end = clock::now_ns() + ms * NS_PER_MS;
    while (clock::now_ns() < end) {
        cpu::relax();
    }
}

// Sends one training request and, after an accepted reply asking for a wait, spins for that wait, as NVIDIA's library
// does after any reply asking for one
static int32_t request_training(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    uint32_t command,
    uint32_t data
) {
    rm::train_link_params params = {};
    params.display_id = display_id;
    params.command = command;
    params.data = data;
    int32_t rc = fw.control(display_common, rm::CONTROL_TRAIN_LINK, &params, sizeof(params));
    if (rc == OK && params.retry_time_ms > 0) {
        spin_for_ms(params.retry_time_ms);
    }

    return rc;
}

int32_t train_link(
    firmware& fw,
    rm::handle display_common,
    const display_output& output,
    const dpcd_capabilities& dpcd,
    bool trains_phy_repeaters,
    const displayport::link_configuration& config,
    bool enhanced_framing,
    bool& is_trained
) {
    is_trained = false;

    uint8_t repeaters = phy_repeater_count(dpcd);
    bool trains_repeaters_first = trains_phy_repeaters && output.phy_repeaters == repeaters;

    uint32_t command = rm::TRAIN_SET_LANES | rm::TRAIN_SET_RATE;
    if (enhanced_framing) {
        command |= rm::TRAIN_ENHANCED_FRAMING;
    }

    if (trains_repeaters_first) {
        command |= rm::TRAIN_PHY_REPEATER;
    }

    uint8_t first_target = trains_repeaters_first ? repeaters : 0;
    for (int32_t target = first_target; target >= 0; target--) {
        uint32_t data = (static_cast<uint32_t>(config.rate) << rm::TRAIN_DATA_RATE_SHIFT)
            | config.lanes
            | (static_cast<uint32_t>(target) << rm::TRAIN_DATA_TARGET_SHIFT);

        int32_t rc = request_training(fw, display_common, output.display_id, command, data);
        if (rc == ERR_FIRMWARE) {
            log::error(
                "nvidia: display common refused to train display id 0x%X's link at %u %s at DisplayPort rate 0x%02X "
                "(command 0x%X, data 0x%X)%s",
                output.display_id,
                config.lanes,
                config.lanes == 1 ? "lane" : "lanes",
                static_cast<uint32_t>(config.rate),
                command,
                data,
                LINK_UNCHECKED
            );
            return OK;
        }

        if (rc != OK) {
            return rc;
        }
    }

    is_trained = true;

    log::info(
        "nvidia: display common trained display id 0x%X's link at %u %s at DisplayPort rate 0x%02X (command 0x%X, "
        "%u PHY %s trained first)",
        output.display_id,
        config.lanes,
        config.lanes == 1 ? "lane" : "lanes",
        static_cast<uint32_t>(config.rate),
        command,
        first_target,
        first_target == 1 ? "repeater" : "repeaters"
    );

    return OK;
}

int32_t configure_stream(
    firmware& fw,
    rm::handle display_common,
    uint8_t head,
    const output_link& link,
    bool enhanced_framing,
    const displayport::stream_parameters& stream,
    bool& is_configured
) {
    is_configured = false;

    rm::configure_stream_params params = {};
    params.head = head;
    params.resource_index = link.index;
    params.sublink = link.sublink;
    params.overrides = 1;

    params.hblank_symbols = stream.hblank_symbols;
    params.vblank_symbols = stream.vblank_symbols;
    params.color_format = rm::STREAM_RGB;

    params.single.enhanced_framing = enhanced_framing ? 1 : 0;
    params.single.transfer_unit_symbols = stream.transfer_unit_symbols;
    params.single.watermark = stream.watermark;

    int32_t rc = fw.control(display_common, rm::CONTROL_CONFIGURE_STREAM, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::error(
            "nvidia: display common refused head %u's DisplayPort stream on output resource %u's link %c%s",
            head,
            link.index,
            link_letter(link.sublink),
            NO_ATTRIBUTES
        );
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    is_configured = true;

    log::info(
        "nvidia: display common programmed head %u's DisplayPort stream on output resource %u's link %c: transfer "
        "units of %u symbols, a watermark of %u symbols, %u symbols of horizontal blanking and %u of vertical",
        head,
        link.index,
        link_letter(link.sublink),
        stream.transfer_unit_symbols,
        stream.watermark,
        stream.hblank_symbols,
        stream.vblank_symbols
    );

    return OK;
}

int32_t set_stream_attributes(firmware& fw, rm::handle display_common, uint32_t display_id) {
    rm::stream_attributes_params params = {};
    params.display_id = display_id;
    params.overrides = 1;
    params.keeps_for_next_mode = 1;
    params.mask[rm::STREAM_ATTRIBUTE_MISC1] = rm::STREAM_ATTRIBUTE_STEREO;

    int32_t rc = fw.control(display_common, rm::CONTROL_SET_STREAM_ATTRIBUTES, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::error(
            "nvidia: display common refused to clear the stereo bits of display id 0x%X's main stream attributes, so "
            "the stream keeps those the firmware sets",
            display_id
        );
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    log::info("nvidia: display common clears the stereo bits of display id 0x%X's main stream attributes", display_id);

    return OK;
}

// Whether the status read after training shows the link trained at `config`, logging what it shows
static bool is_training_checked(
    uint32_t display_id,
    const displayport::link_status& status,
    const displayport::link_configuration& config
) {
    bool is_trained = displayport::is_trained(status, config.lanes);
    if (is_trained) {
        log::info(
            "nvidia: display id 0x%X's link status reads 0x%02X 0x%02X 0x%02X, so its %u %s trained",
            display_id,
            status.lanes_0_1,
            status.lanes_2_3,
            status.lane_align,
            config.lanes,
            config.lanes == 1 ? "lane is" : "lanes are"
        );
        return true;
    }

    log::error(
        "nvidia: display id 0x%X's link is not trained at %u %s, its link status reading 0x%02X 0x%02X 0x%02X%s",
        display_id,
        config.lanes,
        config.lanes == 1 ? "lane" : "lanes",
        status.lanes_0_1,
        status.lanes_2_3,
        status.lane_align,
        NO_STREAM
    );

    return false;
}

int32_t set_up_output_link(
    firmware& fw,
    rm::handle display_common,
    bool has_crossbar,
    const display_output& output,
    const dpcd_capabilities& dpcd,
    const first_light& light,
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
    bool is_read = false;
    rc = read_link_capabilities(fw, display_common, link.index, gpu, supports_1_2, is_read);
    if (rc != OK || !is_read) {
        return rc;
    }

    dpcd_capabilities link_dpcd = {};
    string::memcpy(&link_dpcd, &dpcd, sizeof(link_dpcd));

    // NVIDIA's library reads no PHY repeater when the facts say the firmware trains none
    if (!gpu.trains_phy_repeaters) {
        link_dpcd.has_phy_repeater_bytes = false;
        string::memset(link_dpcd.phy_repeaters, 0, sizeof(link_dpcd.phy_repeaters));
    }

    displayport::link_facts facts = output_link_facts(output, link_dpcd, gpu);
    displayport::link_configuration config = {};
    if (!displayport::find_lowest_configuration(facts, light.timing, config)) {
        log::error(
            "nvidia: no link configuration carries display id 0x%X's mode within output resource %u's DisplayPort "
            "facts%s",
            output.display_id,
            link.index,
            UNTRAINED
        );
        return OK;
    }

    bool is_trained = false;
    rc = train_link(
        fw,
        display_common,
        output,
        link_dpcd,
        gpu.trains_phy_repeaters,
        config,
        facts.enhanced_framing,
        is_trained
    );
    if (rc != OK || !is_trained) {
        return rc;
    }

    displayport::link_status status = {};
    bool is_status_read = false;
    rc = read_link_status(fw, display_common, output.display_id, link_dpcd, supports_1_2, status, is_status_read);
    if (rc != OK || !is_status_read) {
        return rc;
    }

    if (!is_training_checked(output.display_id, status, config)) {
        return OK;
    }

    // A configuration `find_lowest_configuration` gives always has a stream, as it tries only those
    displayport::stream_parameters stream = {};
    displayport::compute_stream(facts, config, light.timing, stream);

    bool is_configured = false;
    rc = configure_stream(fw, display_common, light.head, link, facts.enhanced_framing, stream, is_configured);
    if (rc != OK || !is_configured) {
        return rc;
    }

    rc = set_stream_attributes(fw, display_common, output.display_id);
    is_set_up = rc == OK;
    return rc;
}

} // namespace drivers::nvidia
