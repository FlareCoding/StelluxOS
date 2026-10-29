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

// How the line logged for a refused training or a failed status read ends, naming the steps it leaves undone
constexpr const char* LINK_UNCHECKED = ", so its link is neither checked nor given a stream";

// The bytes a monitor's DPCD reports of its capabilities and of any PHY repeater's, as read
struct dpcd_capabilities {
    bool    is_extended;                                       // `receiver` holds the extended copy
    bool    has_phy_repeater_bytes;                            // `phy_repeaters` was read, its bytes all 0 otherwise
    uint8_t receiver[DPCD_RECEIVER_CAPABILITY_BYTES];          // from the revision, highest rate and most lanes on
    uint8_t phy_repeaters[DPCD_PHY_REPEATER_CAPABILITY_BYTES]; // from the repeaters' revision, rate and count on
};

/**
 * @brief Tell the GSP's firmware to leave the DisplayPort hardware to the
 * driver, as NVIDIA's DisplayPort library does before its first AUX read.
 * Leaving it turns off the firmware's own watermark programming, its handling
 * of the DP IRQs that NVIDIA names CP IRQ (a monitor's content protection
 * requests for the card's attention), and its retries of a deferred AUX reply,
 * all three the driver's from then on. A success is logged, and a refusal with
 * `ERR_FIRMWARE` is logged as leaving all three to the firmware, as NVIDIA's
 * library goes on past it.
 * @param fw The driver's firmware.
 * @param display_common The handle `allocate_display_objects` gave display
 * common.
 * @return `OK`, after a refusal too, or what `firmware::control` returns,
 * logged as its doc describes, for an error other than `ERR_FIRMWARE`.
 */
int32_t take_displayport_from_firmware(firmware& fw, rm::handle display_common);

/**
 * @brief Read the DisplayPort capabilities of the monitor on a connected
 * output from its DPCD, as NVIDIA's DisplayPort library reads them: the byte
 * at `DPCD_TRAINING_AUX_READ_INTERVAL`, then the receiver capability field
 * from `DPCD_EXTENDED_RECEIVER_CAPABILITIES` when that byte has
 * `DPCD_EXTENDED_COPY_PRESENT` set or from `DPCD_RECEIVER_CAPABILITIES`
 * otherwise or when that byte cannot be read, then the PHY repeaters'
 * capabilities, when `trains_phy_repeaters` is set, a failed read of which
 * means no PHY repeater. Each read sends display common's AUX channel control
 * a request, its parameters zeroed but for the display id, the command, the
 * address and the size, and sends a fresh one while the monitor defers it or
 * acknowledges it with some but not all of the bytes or with more than those.
 * A read stops at its 7th deferral or acknowledgment of too few bytes for the
 * first byte and its 16th for each of the others, NVIDIA's counts, or at as
 * many acknowledgments of too many bytes, and the reads are made again up to 3
 * more times while the field cannot be read. A read gives no bytes, and is
 * logged, when the firmware refuses a request with `ERR_FIRMWARE`, when the
 * monitor answers with anything but an acknowledgment of exactly the bytes
 * asked for or an answer it is asked again on, or when it stops at a count.
 * NVIDIA's library counts no acknowledgment of too many bytes, asking again
 * for as long as the monitor sends them, and asks again when the firmware's
 * status for a request is its timeout, which `firmware::control` gives as
 * `ERR_FIRMWARE`. It also reads the PHY repeaters' capabilities unless display
 * common reports that the GPU does not train PHY repeaters, sends each request
 * up to 3 times, sleeping as long as the firmware says after each, while the
 * firmware fails it and names a time to retry after, and before its first read
 * asks display common for the GPU's DisplayPort capabilities. The counts here
 * are the only retries of a deferred reply once
 * `take_displayport_from_firmware` has turned off the firmware's own.
 * @param fw The driver's firmware.
 * @param display_common The handle `allocate_display_objects` gave display
 * common.
 * @param display_id The display id of an output `read_display_outputs` marks
 * connected.
 * @param trains_phy_repeaters Whether the GPU's firmware trains PHY repeaters.
 * @param out Set when the field is read, with `has_phy_repeater_bytes` set
 * when the PHY repeaters' capabilities are read too and `phy_repeaters` all 0
 * otherwise, and left unchanged otherwise.
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
 * revision below 1.0 unless a registry key replaces the revision.
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

/**
 * @brief Whether a monitor's DPCD gives a revision of 1.0 or later, as
 * NVIDIA's DisplayPort library tests it before driving a monitor, without its
 * registry keys.
 * @param dpcd The capabilities `read_output_dpcd` read.
 * @return Whether the receiver capability field's major revision, bits 7 to 4
 * of its first byte, is 1 or more, whatever its minor revision.
 */
bool is_at_least_revision_1_0(const dpcd_capabilities& dpcd);

/**
 * @brief The PHY repeaters on a monitor's link, as NVIDIA's DisplayPort
 * library counts them from the count byte of their capability field, which
 * sets one bit, 0x80 for one repeater down to 0x01 for eight.
 * @param dpcd The capabilities `read_output_dpcd` read.
 * @return The repeaters, 1 to 8, or 0 when the field was not read, gives a
 * revision below 1.4 or sets no single bit, as `monitor_link_limits` counts no
 * repeater then.
 */
uint8_t phy_repeater_count(const dpcd_capabilities& dpcd);

/**
 * @brief Read the status of a trained link, as NVIDIA's DisplayPort library
 * reads it: the three bytes from 0x0200C, the copy among the monitor's event
 * status indicators, when the monitor's DPCD gives revision 1.2 or later and
 * the GPU supports DisplayPort 1.2, else the three from 0x00202, and the same
 * three bytes of each PHY repeater `dpcd` counts, whether or not `train_link`
 * trained it, from the one closest to the GPU, each ANDed into the sink's, so
 * an untrained repeater's status can fail the training check and stop the
 * link. NVIDIA's library also reads 0x00202 for an eDP 1.4 or 1.4a panel and
 * for a monitor whose revision its workaround lowers to 1.1. Each read is sent
 * again as often as `read_output_dpcd` sends one, and a read that fails is
 * logged and leaves the status unread.
 * @param fw The bound firmware.
 * @param display_common The display common object.
 * @param display_id The display id the link drives, a single bit.
 * @param dpcd The capabilities `read_output_dpcd` read, from which the caller
 * leaves out the PHY repeaters when the output resource's DisplayPort facts
 * say the firmware trains none.
 * @param gpu_supports_1_2 Whether the GPU's link supports DisplayPort 1.2.
 * @param out Set to the status, its in-sync byte 0, when every read succeeds,
 * and left unchanged otherwise.
 * @param is_read Set to whether every read succeeded.
 * @return `OK`, including when a read fails, or what `firmware::control`
 * returns, logged as its doc describes, for an error other than
 * `ERR_FIRMWARE`.
 */
int32_t read_link_status(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    const dpcd_capabilities& dpcd,
    bool gpu_supports_1_2,
    displayport::link_status& out,
    bool& is_read
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_DPCD_H
