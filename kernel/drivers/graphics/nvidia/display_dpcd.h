// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_DPCD_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_DPCD_H

#include "common/types.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

// The addresses of the receiver capability field (a monitor's revision and link limits) in its DPCD (its registers on
// DisplayPort's AUX side channel) and of the field's extended copy, with NVIDIA's names
constexpr uint32_t DPCD_RECEIVER_CAPABILITIES          = 0x00000; // NV_DPCD_REV
constexpr uint32_t DPCD_EXTENDED_RECEIVER_CAPABILITIES = 0x02200; // NV_DPCD14_EXTENDED_REV

// NVIDIA's NV_DPCD_TRAINING_AUX_RD_INTERVAL, the address of the byte `read_output_dpcd` reads first, which flags the
// extended copy
constexpr uint32_t DPCD_TRAINING_AUX_READ_INTERVAL = 0x0000E;

// NVIDIA's NV_DPCD14_TRAINING_AUX_RD_INTERVAL_EXTENDED_RX_CAP, the bit of the byte at `DPCD_TRAINING_AUX_READ_INTERVAL`
// set when the receiver capability field has an extended copy
constexpr uint8_t DPCD_EXTENDED_COPY_PRESENT = 0x80;

// NVIDIA's NV_DPCD14_LT_TUNABLE_PHY_REPEATER_REV, the address of the first capability byte of any PHY repeater (a chip
// between the card and the monitor regenerating the signal)
constexpr uint32_t DPCD_PHY_REPEATER_CAPABILITIES = 0xF0000;

// How many bytes `read_output_dpcd` reads of the receiver capability field and of the PHY repeaters' capabilities
constexpr uint32_t DPCD_RECEIVER_CAPABILITY_BYTES     = 16;
constexpr uint32_t DPCD_PHY_REPEATER_CAPABILITY_BYTES = 8;

// The bytes a monitor's DPCD reports of its capabilities and of any PHY repeater's, as read
struct dpcd_capabilities {
    bool    is_extended;                                       // `receiver` holds the extended copy
    uint8_t receiver[DPCD_RECEIVER_CAPABILITY_BYTES];          // from the revision, highest rate and most lanes on
    uint8_t phy_repeaters[DPCD_PHY_REPEATER_CAPABILITY_BYTES]; // from the repeaters' revision, rate and count on
};

/**
 * @brief Read the DisplayPort capabilities of the monitor on a connected
 * output from its DPCD, as NVIDIA's DisplayPort library reads them: the byte
 * at `DPCD_TRAINING_AUX_READ_INTERVAL`, then the receiver capability field
 * from `DPCD_EXTENDED_RECEIVER_CAPABILITIES` when that byte has
 * `DPCD_EXTENDED_COPY_PRESENT` set or from `DPCD_RECEIVER_CAPABILITIES`
 * otherwise, then the PHY repeaters' capabilities. Each read is one request to
 * display common's AUX channel control, its parameters zeroed but for the
 * display id, the command, the address and the size. When the firmware refuses
 * a request with `ERR_FIRMWARE`, or a read ends with anything but the
 * monitor's acknowledgment carrying exactly the bytes asked for, the read logs
 * it, asks nothing more and leaves `is_read` cleared. NVIDIA's library instead
 * asks again for a read the monitor defers or acknowledges with some but not
 * all of the bytes, or the firmware fails with its timeout, making up to 7
 * requests for the first byte and 16 for each of the others, and beyond those
 * counts for as long as the monitor acknowledges a read with more bytes than
 * asked for, takes the field from `DPCD_RECEIVER_CAPABILITIES` when the first
 * byte cannot be read, reads the PHY repeaters' capabilities unless display
 * common reports that the GPU does not train PHY repeaters, going on without
 * them when that read fails, and makes the reads again up to 3 more times
 * while the field cannot be read, taking the DPCD as offline when it never
 * can. It also sends each request up to 3 times, sleeping as long as the
 * firmware says after each, while the firmware fails it and names a time to
 * retry after, and before its first read tells the firmware to leave the
 * DisplayPort hardware to it, which turns off the firmware's own retries of a
 * deferred reply, and asks display common for the GPU's DisplayPort
 * capabilities.
 * @param fw The driver's firmware.
 * @param display_common The handle `allocate_display_objects` gave display
 * common.
 * @param display_id The display id of an output `read_display_outputs` marks
 * connected.
 * @param out Set when every read gives its bytes, and left unchanged
 * otherwise.
 * @param is_read Set when `out` holds the monitor's capabilities, and cleared
 * otherwise.
 * @return `OK`, or what `firmware::control` returns, logged as its doc
 * describes, for a request the firmware does not refuse with `ERR_FIRMWARE`.
 */
int32_t read_output_dpcd(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    dpcd_capabilities& out,
    bool& is_read
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_DPCD_H
