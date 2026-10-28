// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"
#include "common/string.h"

namespace drivers::nvidia {

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

} // namespace drivers::nvidia
