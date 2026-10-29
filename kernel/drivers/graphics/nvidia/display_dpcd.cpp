// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"
#include "common/string.h"

namespace drivers::nvidia {

// The receiver capability field's offsets of its highest rate and lane byte, the rate code's low bits, and the lane
// count's bits and enhanced framing bit of a lane byte, as NVIDIA's DPCD headers give them
static constexpr uint32_t RECEIVER_MAX_RATE = 1;
static constexpr uint32_t RECEIVER_LANES    = 2;
static constexpr uint8_t  RATE_CODE_MASK    = 0x1F;
static constexpr uint8_t  LANE_COUNT_MASK   = 0x1F;
static constexpr uint8_t  ENHANCED_FRAMING  = 0x80;

// The receiver capability field's offset of its revision, and revision 1.0 as that byte codes it, the major revision in
// bits 7 to 4 and the minor in bits 3 to 0, as NVIDIA's DPCD headers give them
static constexpr uint32_t RECEIVER_REVISION     = 0;
static constexpr uint8_t  RECEIVER_REVISION_1_0 = 0x10;
static constexpr uint8_t  RECEIVER_REVISION_1_2 = 0x12;

// The PHY repeaters' offsets of their revision, highest rate, count and lane byte, revision 1.4 as their revision
// byte codes it, and the count byte's bit for one repeater, as NVIDIA's DPCD headers give them
static constexpr uint32_t REPEATER_REVISION     = 0;
static constexpr uint32_t REPEATER_MAX_RATE     = 1;
static constexpr uint32_t REPEATER_COUNT        = 2;
static constexpr uint32_t REPEATER_LANES        = 4;
static constexpr uint8_t  REPEATER_REVISION_1_4 = 0x14;
static constexpr uint8_t  REPEATER_COUNT_FIRST  = 0x80;

// The link status bytes NVIDIA's library reads, lanes 0 and 1, lanes 2 and 3 and the interlane alignment, from the
// sink's field, its copy among the event status indicators, and PHY repeater 1's, each higher-numbered one 0x50 on
static constexpr uint32_t LINK_STATUS_BYTES           = 3;
static constexpr uint32_t SINK_LINK_STATUS            = 0x00202; // NV_DPCD_LANE0_1_STATUS
static constexpr uint32_t SINK_LINK_STATUS_INDICATORS = 0x0200C; // NV_DPCD_LANE0_1_STATUS_ESI
static constexpr uint32_t REPEATER_LINK_STATUS        = 0xF0030; // NV_DPCD14_PHY_REPEATER_START(0) + 0x20
static constexpr uint32_t REPEATER_LINK_STATUS_STRIDE = 0x50;

// NVIDIA's library's requests for the byte at 0x0000E (minimumRetriesOnDefer) and for each later read, and its passes,
// the first and the 3 more (faultyRetries) it makes while the field cannot be read
static constexpr uint32_t INTERVAL_REQUESTS   = 7;
static constexpr uint32_t CAPABILITY_REQUESTS = 16;
static constexpr uint32_t PASSES              = 4;

// How the line logged for a read that gives no bytes ends, for each read and pass
static constexpr const char* FIELD_FROM_BASE = ", so its receiver capability field is read from 0x00000";
static constexpr const char* READ_AGAIN      = ", so its DPCD is read again";
static constexpr const char* NO_MORE_READ    = ", so no more of its DPCD is read";
static constexpr const char* NO_PHY_REPEATER = ", so its link is taken to have no PHY repeater";

namespace {

// How one AUX request of a DPCD read ended
struct aux_answer {
    bool     is_refused; // the firmware refused the request with `ERR_FIRMWARE`
    uint32_t reply;      // NVIDIA's replyType, when not refused
    uint32_t size;       // the bytes the transaction moved, when not refused
};

} // namespace

// What the log says of an AUX reply other than an acknowledgment
static const char* reply_meaning(uint32_t reply) {
    switch (reply) {
        case rm::AUX_REPLY_NACK:
            return "a refusal";
        case rm::AUX_REPLY_DEFER:
        case rm::AUX_REPLY_I2C_DEFER:
            return "a request to ask again later";
        case rm::AUX_REPLY_TIMEOUT:
            return "no answer in time";
        case rm::AUX_REPLY_I2C_NACK:
            return "a refusal of an I2C transaction";
    }

    return "an unknown reply";
}

// The rate the PHY repeaters' rate code stands for, or HBR for a code DisplayPort does not define, as NVIDIA's library
// takes it
static displayport::link_rate repeater_rate(uint8_t code) {
    switch (static_cast<displayport::link_rate>(code)) {
        case displayport::link_rate::rbr:
        case displayport::link_rate::hbr:
        case displayport::link_rate::hbr2:
        case displayport::link_rate::hbr3:
            return static_cast<displayport::link_rate>(code);
    }

    return displayport::link_rate::hbr;
}

// The rate the receiver capability field's rate code stands for, as NVIDIA's library reads it: RBR, HBR and HBR2 from
// the code's bits 4 to 0 alone, HBR3 from the whole byte, and HBR for any other code
static displayport::link_rate receiver_rate(uint8_t code) {
    if (code == static_cast<uint8_t>(displayport::link_rate::hbr3)) {
        return displayport::link_rate::hbr3;
    }

    auto low_rate = static_cast<displayport::link_rate>(code & RATE_CODE_MASK);
    bool is_rbr = low_rate == displayport::link_rate::rbr;
    bool is_hbr2 = low_rate == displayport::link_rate::hbr2;
    return is_rbr || is_hbr2 ? low_rate : displayport::link_rate::hbr;
}

// The lanes a link trains at most for a lane count, as NVIDIA's library has it: 4 for 0, else the most of 1, 2 and 4
// within the count, the only lane counts its search tries
static uint8_t trained_lanes(uint8_t count) {
    if (count == 0 || count >= 4) {
        return 4;
    }

    return count >= 2 ? 2 : 1;
}

// Whether `answer` is the monitor's acknowledgment moving exactly the `bytes` bytes asked for
static bool is_acknowledged_exactly(const aux_answer& answer, uint32_t bytes) {
    bool is_acknowledged = !answer.is_refused && answer.reply == rm::AUX_REPLY_ACK;
    return is_acknowledged && answer.size == bytes;
}

// Whether `answer` is the monitor's acknowledgment moving more than the `bytes` bytes asked for
static bool is_acknowledged_with_more(const aux_answer& answer, uint32_t bytes) {
    bool is_acknowledged = !answer.is_refused && answer.reply == rm::AUX_REPLY_ACK;
    return is_acknowledged && answer.size > bytes;
}

// Sends one request reading `bytes` bytes of the DPCD from `address`, gives how it ended in `answer` and copies the
// bytes into `out` when the monitor acknowledges it with exactly those
static int32_t request_dpcd_read(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    uint32_t address,
    uint32_t bytes,
    uint8_t* out,
    aux_answer& answer
) {
    rm::aux_params params = {};
    params.display_id = display_id;
    params.command = rm::AUX_COMMAND_DPCD_READ;
    params.address = address;
    params.size = bytes - 1;

    int32_t rc = fw.control(display_common, rm::CONTROL_DP_AUX, &params, sizeof(params));
    if (rc != OK && rc != ERR_FIRMWARE) {
        return rc;
    }

    answer = {
        .is_refused = rc == ERR_FIRMWARE,
        .reply      = params.reply,
        .size       = params.size,
    };

    if (is_acknowledged_exactly(answer, bytes)) {
        string::memcpy(out, params.data, bytes);
    }

    return OK;
}

// Whether a read is asked again after `answer`: a deferred reply, or an acknowledgment moving some but not all of the
// bytes asked for or more than those, which NVIDIA's library asks again on
static bool is_asked_again(const aux_answer& answer, uint32_t bytes) {
    if (answer.is_refused) {
        return false;
    }

    bool is_deferred = answer.reply == rm::AUX_REPLY_DEFER || answer.reply == rm::AUX_REPLY_I2C_DEFER;
    bool is_acknowledged = answer.reply == rm::AUX_REPLY_ACK;
    bool moved_other_bytes = answer.size != 0 && answer.size != bytes;
    return is_deferred || (is_acknowledged && moved_other_bytes);
}

// Logs why a read of `bytes` bytes at `address` gave none after `requests` requests, the line ending with `outcome`
static void log_unread(
    uint32_t display_id,
    uint32_t address,
    uint32_t bytes,
    const aux_answer& answer,
    uint32_t requests,
    const char* outcome
) {
    if (answer.is_refused) {
        log::error(
            "nvidia: display common refused display id 0x%X's %u-byte AUX read at 0x%05X%s",
            display_id,
            bytes,
            address,
            outcome
        );
        return;
    }

    const char* request_word = requests == 1 ? "request" : "requests";
    if (answer.reply != rm::AUX_REPLY_ACK) {
        log::error(
            "nvidia: display id 0x%X's %u-byte AUX read at 0x%05X ended with %s from its monitor (reply type %u) after "
            "%u %s%s",
            display_id,
            bytes,
            address,
            reply_meaning(answer.reply),
            answer.reply,
            requests,
            request_word,
            outcome
        );
        return;
    }

    const char* size_word = answer.size == 1 ? "byte" : "bytes";
    log::error(
        "nvidia: display id 0x%X's monitor acknowledged the %u-byte AUX read at 0x%05X with a reply size of %u %s "
        "after %u %s%s",
        display_id,
        bytes,
        address,
        answer.size,
        size_word,
        requests,
        request_word,
        outcome
    );
}

// Reads `bytes` bytes of the DPCD from `address` into `out`, asking again while `is_asked_again` holds until `requests`
// deferred or short replies or as many longer ones have come, and logs a failed read in a line ending with `outcome`
static int32_t read_dpcd(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    uint32_t address,
    uint32_t bytes,
    uint32_t requests,
    const char* outcome,
    uint8_t* out,
    bool& is_read
) {
    is_read = false;

    aux_answer answer = {};
    uint32_t made = 0;
    uint32_t deferred_or_short = 0;
    uint32_t longer = 0;
    while (deferred_or_short < requests && longer < requests) {
        int32_t rc = request_dpcd_read(fw, display_common, display_id, address, bytes, out, answer);
        if (rc != OK) {
            return rc;
        }

        made++;
        if (!is_asked_again(answer, bytes)) {
            break;
        }

        if (is_acknowledged_with_more(answer, bytes)) {
            longer++;
        } else {
            deferred_or_short++;
        }
    }

    is_read = is_acknowledged_exactly(answer, bytes);
    if (!is_read) {
        log_unread(display_id, address, bytes, answer, made, outcome);
    }

    return OK;
}

// Makes one pass of NVIDIA's reads into `capabilities`, reading the PHY repeaters' bytes only once the field is read
// and leaving them all 0 when they cannot be read
static int32_t read_capabilities(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    bool trains_phy_repeaters,
    bool is_last_pass,
    dpcd_capabilities& capabilities,
    bool& is_field_read
) {
    is_field_read = false;
    capabilities = {};

    uint8_t interval = 0;
    bool is_interval_read = false;
    int32_t rc = read_dpcd(
        fw,
        display_common,
        display_id,
        DPCD_TRAINING_AUX_READ_INTERVAL,
        1,
        INTERVAL_REQUESTS,
        FIELD_FROM_BASE,
        &interval,
        is_interval_read
    );
    if (rc != OK) {
        return rc;
    }

    capabilities.is_extended = is_interval_read && (interval & DPCD_EXTENDED_COPY_PRESENT) != 0;
    uint32_t receiver_address = capabilities.is_extended
        ? DPCD_EXTENDED_RECEIVER_CAPABILITIES
        : DPCD_RECEIVER_CAPABILITIES;
    const char* field_outcome = is_last_pass ? NO_MORE_READ : READ_AGAIN;
    rc = read_dpcd(
        fw,
        display_common,
        display_id,
        receiver_address,
        DPCD_RECEIVER_CAPABILITY_BYTES,
        CAPABILITY_REQUESTS,
        field_outcome,
        capabilities.receiver,
        is_field_read
    );
    if (rc != OK || !is_field_read) {
        return rc;
    }

    if (!trains_phy_repeaters) {
        return OK;
    }

    return read_dpcd(
        fw,
        display_common,
        display_id,
        DPCD_PHY_REPEATER_CAPABILITIES,
        DPCD_PHY_REPEATER_CAPABILITY_BYTES,
        CAPABILITY_REQUESTS,
        NO_PHY_REPEATER,
        capabilities.phy_repeaters,
        capabilities.has_phy_repeater_bytes
    );
}

int32_t take_displayport_from_firmware(firmware& fw, rm::handle display_common) {
    rm::manual_displayport_params params = {};
    int32_t rc = fw.control(display_common, rm::CONTROL_SET_MANUAL_DISPLAYPORT, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::error(
            "nvidia: display common refused to leave the DisplayPort hardware to the driver, so the firmware keeps its "
            "own watermark programming, DP IRQ (CP IRQ) handling and retries of a deferred AUX reply"
        );
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: display common leaves the DisplayPort hardware to the driver, turning off the firmware's own "
        "watermark programming, DP IRQ (CP IRQ) handling and retries of a deferred AUX reply"
    );

    return OK;
}

int32_t read_output_dpcd(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    bool trains_phy_repeaters,
    dpcd_capabilities& out,
    bool& is_read
) {
    is_read = false;

    dpcd_capabilities capabilities = {};
    for (uint32_t pass = 0; pass < PASSES; pass++) {
        bool is_field_read = false;
        bool is_last_pass = pass + 1 == PASSES;
        int32_t rc = read_capabilities(
            fw,
            display_common,
            display_id,
            trains_phy_repeaters,
            is_last_pass,
            capabilities,
            is_field_read
        );
        if (rc != OK) {
            return rc;
        }

        if (is_field_read) {
            string::memcpy(&out, &capabilities, sizeof(out));
            is_read = true;
            return OK;
        }
    }

    return OK;
}

displayport::link_limits monitor_link_limits(const dpcd_capabilities& dpcd) {
    displayport::link_rate rate = receiver_rate(dpcd.receiver[RECEIVER_MAX_RATE]);
    uint8_t lanes = dpcd.receiver[RECEIVER_LANES] & LANE_COUNT_MASK;
    bool is_lane_count = lanes == 0 || lanes == 1 || lanes == 2 || lanes == 4 || lanes == 8;
    if (!is_lane_count) {
        lanes = 1;
    }

    if (phy_repeater_count(dpcd) != 0) {
        displayport::link_rate repeaters_highest_rate = repeater_rate(dpcd.phy_repeaters[REPEATER_MAX_RATE]);
        uint8_t repeaters_most_lanes = dpcd.phy_repeaters[REPEATER_LANES] & LANE_COUNT_MASK;
        if (static_cast<uint8_t>(repeaters_highest_rate) < static_cast<uint8_t>(rate)) {
            rate = repeaters_highest_rate;
        }

        if (repeaters_most_lanes < lanes) {
            lanes = repeaters_most_lanes;
        }
    }

    return {
        .max_lanes = trained_lanes(lanes),
        .max_rate = rate,
    };
}

bool takes_enhanced_framing(const dpcd_capabilities& dpcd) {
    return (dpcd.receiver[RECEIVER_LANES] & ENHANCED_FRAMING) != 0;
}

bool is_at_least_revision_1_0(const dpcd_capabilities& dpcd) {
    return dpcd.receiver[RECEIVER_REVISION] >= RECEIVER_REVISION_1_0;
}

uint8_t phy_repeater_count(const dpcd_capabilities& dpcd) {
    uint8_t count = dpcd.phy_repeaters[REPEATER_COUNT];
    bool names_one_count = count != 0 && (count & (count - 1)) == 0;
    bool is_at_least_revision_1_4 = dpcd.phy_repeaters[REPEATER_REVISION] >= REPEATER_REVISION_1_4;
    if (!is_at_least_revision_1_4 || !names_one_count) {
        return 0;
    }

    uint8_t repeaters = 1;
    for (uint8_t bit = REPEATER_COUNT_FIRST; (count & bit) == 0; bit >>= 1) {
        repeaters++;
    }

    return repeaters;
}

int32_t read_link_status(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    const dpcd_capabilities& dpcd,
    bool gpu_supports_1_2,
    displayport::link_status& out,
    bool& is_read
) {
    is_read = false;

    uint8_t combined[LINK_STATUS_BYTES] = {0xFF, 0xFF, 0xFF};
    // Reads every repeater the DPCD counts, as NVIDIA's library does, even one `train_link` left out
    uint8_t repeaters = phy_repeater_count(dpcd);
    for (uint8_t target = repeaters; target > 0; target--) {
        uint32_t address = REPEATER_LINK_STATUS + REPEATER_LINK_STATUS_STRIDE * (target - 1);
        uint8_t status[LINK_STATUS_BYTES] = {};
        bool is_status_read = false;
        int32_t rc = read_dpcd(
            fw,
            display_common,
            display_id,
            address,
            LINK_STATUS_BYTES,
            CAPABILITY_REQUESTS,
            LINK_UNCHECKED,
            status,
            is_status_read
        );
        if (rc != OK || !is_status_read) {
            return rc;
        }

        for (uint32_t i = 0; i < LINK_STATUS_BYTES; i++) {
            combined[i] &= status[i];
        }
    }

    bool reads_indicators = gpu_supports_1_2 && dpcd.receiver[RECEIVER_REVISION] >= RECEIVER_REVISION_1_2;
    uint32_t sink_address = reads_indicators ? SINK_LINK_STATUS_INDICATORS : SINK_LINK_STATUS;
    uint8_t status[LINK_STATUS_BYTES] = {};
    bool is_status_read = false;
    int32_t rc = read_dpcd(
        fw,
        display_common,
        display_id,
        sink_address,
        LINK_STATUS_BYTES,
        CAPABILITY_REQUESTS,
        LINK_UNCHECKED,
        status,
        is_status_read
    );
    if (rc != OK || !is_status_read) {
        return rc;
    }

    out = {
        .lanes_0_1   = static_cast<uint8_t>(combined[0] & status[0]),
        .lanes_2_3   = static_cast<uint8_t>(combined[1] & status[1]),
        .lane_align  = static_cast<uint8_t>(combined[2] & status[2]),
        .sink_status = 0,
    };

    is_read = true;

    return OK;
}

} // namespace drivers::nvidia
