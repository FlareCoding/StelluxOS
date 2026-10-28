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

// The PHY repeaters' offsets of their revision, highest rate, count and lane byte, and revision 1.4 as their revision
// byte codes it, as NVIDIA's DPCD headers give them
static constexpr uint32_t REPEATER_REVISION     = 0;
static constexpr uint32_t REPEATER_MAX_RATE     = 1;
static constexpr uint32_t REPEATER_COUNT        = 2;
static constexpr uint32_t REPEATER_LANES        = 4;
static constexpr uint8_t  REPEATER_REVISION_1_4 = 0x14;

// What the log says of an AUX reply other than an acknowledgment
static const char* reply_meaning(uint32_t reply) {
    switch (reply) {
        case rm::AUX_REPLY_NACK:
            return "a refusal";
        case rm::AUX_REPLY_DEFER:
            return "a request to ask again later";
        case rm::AUX_REPLY_TIMEOUT:
            return "no answer in time";
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

// Reads `bytes` bytes of the DPCD from `address` into `out` with one request, its parameters zeroed but for the display
// id, the command, the address and the size, and logs why when the bytes cannot be read
static int32_t read_dpcd(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    uint32_t address,
    uint32_t bytes,
    uint8_t* out,
    bool& is_read
) {
    is_read = false;

    rm::aux_params params = {};
    params.display_id = display_id;
    params.command = rm::AUX_COMMAND_DPCD_READ;
    params.address = address;
    params.size = bytes - 1;

    int32_t rc = fw.control(display_common, rm::CONTROL_DP_AUX, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::error(
            "nvidia: display common refused display id 0x%X's %u-byte AUX read at 0x%05X, so no more of its DPCD is "
            "read",
            display_id,
            bytes,
            address
        );
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    if (params.reply != rm::AUX_REPLY_ACK) {
        log::error(
            "nvidia: display id 0x%X's %u-byte AUX read at 0x%05X ended with %s from its monitor (reply type %u), so "
            "no more of its DPCD is read",
            display_id,
            bytes,
            address,
            reply_meaning(params.reply),
            params.reply
        );
        return OK;
    }

    if (params.size != bytes) {
        log::error(
            "nvidia: display id 0x%X's monitor acknowledged the %u-byte AUX read at 0x%05X with a reply size of %u, so "
            "no more of its DPCD is read",
            display_id,
            bytes,
            address,
            params.size
        );
        return OK;
    }

    string::memcpy(out, params.data, bytes);
    is_read = true;

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
    uint8_t interval = 0;
    int32_t rc = read_dpcd(fw, display_common, display_id, DPCD_TRAINING_AUX_READ_INTERVAL, 1, &interval, is_read);
    if (rc != OK || !is_read) {
        return rc;
    }

    dpcd_capabilities capabilities = {};
    capabilities.is_extended = (interval & DPCD_EXTENDED_COPY_PRESENT) != 0;
    uint32_t receiver_address = capabilities.is_extended
        ? DPCD_EXTENDED_RECEIVER_CAPABILITIES
        : DPCD_RECEIVER_CAPABILITIES;
    rc = read_dpcd(
        fw,
        display_common,
        display_id,
        receiver_address,
        DPCD_RECEIVER_CAPABILITY_BYTES,
        capabilities.receiver,
        is_read
    );
    if (rc != OK || !is_read) {
        return rc;
    }

    if (trains_phy_repeaters) {
        rc = read_dpcd(
            fw,
            display_common,
            display_id,
            DPCD_PHY_REPEATER_CAPABILITIES,
            DPCD_PHY_REPEATER_CAPABILITY_BYTES,
            capabilities.phy_repeaters,
            is_read
        );
        if (rc != OK || !is_read) {
            return rc;
        }

        capabilities.has_phy_repeater_bytes = true;
    }

    out = capabilities;

    return OK;
}

displayport::link_limits monitor_link_limits(const dpcd_capabilities& dpcd) {
    displayport::link_rate rate = receiver_rate(dpcd.receiver[RECEIVER_MAX_RATE]);
    uint8_t lanes = dpcd.receiver[RECEIVER_LANES] & LANE_COUNT_MASK;
    bool is_lane_count = lanes == 0 || lanes == 1 || lanes == 2 || lanes == 4 || lanes == 8;
    if (!is_lane_count) {
        lanes = 1;
    }

    uint8_t count = dpcd.phy_repeaters[REPEATER_COUNT];
    bool names_one_count = count != 0 && (count & (count - 1)) == 0;
    bool is_at_least_revision_1_4 = dpcd.phy_repeaters[REPEATER_REVISION] >= REPEATER_REVISION_1_4;
    if (is_at_least_revision_1_4 && names_one_count) {
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

} // namespace drivers::nvidia
