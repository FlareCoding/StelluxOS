// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_DPCD_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_DPCD_H

#include "common/types.h"
#include "drivers/graphics/nvidia/displayport.h"
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
    bool    has_phy_repeater_bytes;                            // `phy_repeaters` was read, its bytes all 0 otherwise
    uint8_t receiver[DPCD_RECEIVER_CAPABILITY_BYTES];          // from the revision, highest rate and most lanes on
    uint8_t phy_repeaters[DPCD_PHY_REPEATER_CAPABILITY_BYTES]; // from the repeaters' revision, rate and count on
};

/**
 * @brief Read the DisplayPort capabilities of the monitor on a connected
 * output from its DPCD, as NVIDIA's DisplayPort library reads them: the byte
 * at `DPCD_TRAINING_AUX_READ_INTERVAL`, then the receiver capability field
 * from `DPCD_EXTENDED_RECEIVER_CAPABILITIES` when that byte has
 * `DPCD_EXTENDED_COPY_PRESENT` set or from `DPCD_RECEIVER_CAPABILITIES`
 * otherwise, then the PHY repeaters' capabilities when `trains_phy_repeaters`
 * is set. Each read is one request to display common's AUX channel control,
 * its parameters zeroed but for the display id, the command, the address and
 * the size. When the firmware refuses a request with `ERR_FIRMWARE`, or a read
 * ends with anything but the monitor's acknowledgment carrying exactly the
 * bytes asked for, the read logs it, asks nothing more and leaves `is_read`
 * cleared. NVIDIA's library instead asks again for a read the monitor defers
 * or acknowledges with some but not all of the bytes, or the firmware fails
 * with its timeout, making up to 7 requests for the first byte and 16 for each
 * of the others, and beyond those counts for as long as the monitor
 * acknowledges a read with more bytes than asked for, takes the field from
 * `DPCD_RECEIVER_CAPABILITIES` when the first byte cannot be read, reads the
 * PHY repeaters' capabilities unless display common reports that the GPU does
 * not train PHY repeaters, going on without them when that read fails, and
 * makes the reads again up to 3 more times while the field cannot be read,
 * taking the DPCD as offline when it never can. It also sends each request up
 * to 3 times, sleeping as long as the firmware says after each, while the
 * firmware fails it and names a time to retry after, and before its first read
 * tells the firmware to leave the DisplayPort hardware to it, which turns off
 * the firmware's own retries of a deferred reply, and asks display common for
 * the GPU's DisplayPort capabilities.
 * @param fw The driver's firmware.
 * @param display_common The handle `allocate_display_objects` gave display
 * common.
 * @param display_id The display id of an output `read_display_outputs` marks
 * connected.
 * @param trains_phy_repeaters Whether the GPU's firmware trains PHY repeaters.
 * @param out Set when every read gives its bytes, with
 * `has_phy_repeater_bytes` equal to `trains_phy_repeaters` and `phy_repeaters`
 * all 0 when that is cleared, and left unchanged otherwise.
 * @param is_read Set when `out` holds the monitor's capabilities, and cleared
 * otherwise.
 * @return `OK`, or what `firmware::control` returns, logged as its doc
 * describes, for a request the firmware does not refuse with `ERR_FIRMWARE`.
 */
int32_t read_output_dpcd(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    bool trains_phy_repeaters,
    dpcd_capabilities& out,
    bool& is_read
);

/**
 * @brief Give the most lanes and the highest rate a link to a monitor takes,
 * as NVIDIA's DisplayPort library parses the monitor's capabilities. The
 * receiver capability field's rate is RBR, HBR or HBR2 when its code's bits 4
 * to 0 name one and HBR3 when the whole byte does, else HBR, and a lane count
 * other than 0, 1, 2, 4 and 8 counts as 1. PHY repeaters of revision 1.4 or
 * later whose count byte names 1 to 8 of them cap both, an undefined rate code
 * of theirs as HBR. Their bytes are all 0, naming none, when
 * `read_output_dpcd` did not read them, as NVIDIA's library reads none when
 * display common reports that the GPU does not train them. The lanes left
 * train at the most of 1, 2 and 4 within them, and at 4 for 0, so a repeater's
 * 3 trains 2, as NVIDIA's search tries only those. NVIDIA's revision test also
 * passes a major revision of 0 with a minor one of 4 or more, and without its
 * registry keys NVIDIA takes the rate of a monitor whose field gives revision
 * 1.2 or later and declares eDP display control at eDP 1.4 or later from its
 * link rate table when the table's first entry is not 0, a table read nowhere
 * here. NVIDIA also skips its search for four monitors it lists by maker and
 * product, training each at the highest configuration within its limits and
 * the GPU's, with no rounding of the lanes, and replaces the limits of two of
 * them with 4 lanes at HBR2. NVIDIA drives no monitor whose field gives a
 * revision below 1.0.
 * @param dpcd The capabilities `read_output_dpcd` read.
 * @return The most lanes, 1, 2 or 4, and the highest rate, RBR, HBR, HBR2 or
 * HBR3.
 */
displayport::link_limits monitor_link_limits(const dpcd_capabilities& dpcd);

/**
 * @brief Whether a monitor takes enhanced framing.
 * @param dpcd The capabilities `read_output_dpcd` read.
 * @return Bit 7 of the receiver capability field's lane byte.
 */
bool takes_enhanced_framing(const dpcd_capabilities& dpcd);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_DPCD_H
