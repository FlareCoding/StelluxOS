// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LINK_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LINK_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/display_light.h"
#include "drivers/graphics/nvidia/display_outputs.h"
#include "drivers/graphics/nvidia/displayport.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

/**
 * The output resource driving a display id's DisplayPort link.
 */
struct output_link {
    uint8_t index;   // the output resource's number, NVIDIA's SOR index
    uint8_t sublink; // the resource's link the display id uses, 0 for NVIDIA's link A and 1 for link B
};

/**
 * @brief Give the output resource driving a display id's link, as NVIDIA's
 * modesetting module finds it. When display common reports the crossbar, ask
 * it to assign the display id an output resource, excluding none and for one
 * head, and take the last resource whose entry in the reply drives the
 * display id on its own or as the primary of two output resources fed by two
 * heads. Then ask for the display id's output resource facts, taking the
 * index from them without the crossbar and the link from the protocol either
 * way, as NVIDIA keeps no index from them on a crossbar GPU. A refused
 * request or an assignment naming no resource is logged and assigns none, and
 * so, where NVIDIA's module takes the facts' resource whatever its type and
 * index and its library asserts on another protocol, is a resource that is
 * not serial, a serial one past the 4 display common assigns or a protocol
 * other than DisplayPort's.
 * @param fw The bound firmware.
 * @param display_common The display common object.
 * @param display_id The display id, a single bit.
 * @param has_crossbar Whether display common reports the crossbar.
 * @param out The output resource and link, when one is assigned.
 * @param is_assigned Set to whether one is.
 * @return `OK`, including when none is assigned, or what `firmware::control`
 * returns, logged as its doc describes, for an error other than
 * `ERR_FIRMWARE`.
 */
int32_t assign_output_link(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    bool has_crossbar,
    output_link& out,
    bool& is_assigned
);

/**
 * @brief Ask display common for the DisplayPort facts of an output resource's
 * links, as NVIDIA's DisplayPort library does before training them. A highest
 * rate outside 1 to 4 counts as RBR, and FEC support, the increased watermark
 * limits and PHY repeater training count as set only for a byte of 1. A
 * refused request is logged and gives no facts, where NVIDIA's library
 * asserts and goes on with the facts it held before the request.
 * @param fw The bound firmware.
 * @param display_common The display common object.
 * @param resource_index The output resource's number.
 * @param out The facts, when display common gives them.
 * @param supports_1_2 Set, when display common gives the facts, to whether
 * the links support DisplayPort 1.2.
 * @param is_read Set to whether display common gave the facts.
 * @return `OK`, including when the request is refused, or what
 * `firmware::control` returns, logged as its doc describes, for an error
 * other than `ERR_FIRMWARE`.
 */
int32_t read_link_capabilities(
    firmware& fw,
    rm::handle display_common,
    uint8_t resource_index,
    chip_displayport& out,
    bool& supports_1_2,
    bool& is_read
);

/**
 * @brief Train a display id's link at a configuration, as NVIDIA's
 * DisplayPort library asks display common to: setting the lane count and the
 * rate, turning on enhanced framing when the link runs it and training the
 * PHY repeaters when the GPU trains them and the firmware counts as many as
 * the DPCD does, one request for each repeater from the one closest to the
 * GPU, then one for the sink. After an accepted reply naming a wait, the
 * calling CPU spins for that wait, as NVIDIA's library does after any reply
 * naming one. A refused request is logged and leaves the link untrained,
 * where NVIDIA's library sends a refused request naming a wait again, up to 3
 * times in all, since `firmware::control` gives no refused request's reply.
 * @param fw The bound firmware.
 * @param display_common The display common object.
 * @param output The output the link drives.
 * @param dpcd The capabilities `read_output_dpcd` read for its monitor, from
 * which the caller leaves out the PHY repeaters when the output resource's
 * DisplayPort facts say the firmware trains none.
 * @param trains_phy_repeaters Whether the GPU trains PHY repeaters.
 * @param config The configuration.
 * @param enhanced_framing Whether the link runs enhanced framing.
 * @param is_trained Set to whether every request succeeded.
 * @return `OK`, including when the link is untrained, or what
 * `firmware::control` returns, logged as its doc describes, for an error
 * other than `ERR_FIRMWARE`.
 */
int32_t train_link(
    firmware& fw,
    rm::handle display_common,
    const display_output& output,
    const dpcd_capabilities& dpcd,
    bool trains_phy_repeaters,
    const displayport::link_configuration& config,
    bool enhanced_framing,
    bool& is_trained
);

/**
 * @brief Ask display common to program a head's single stream on its link
 * with the given parameters, as NVIDIA's DisplayPort library does after
 * training, overriding what the firmware would compute. A refused request is
 * logged, where NVIDIA's library ignores its status.
 * @param fw The bound firmware.
 * @param display_common The display common object.
 * @param head The head sending the stream.
 * @param link The output resource and link carrying it.
 * @param enhanced_framing Whether the link runs enhanced framing.
 * @param stream The parameters `compute_stream` gave for the trained link.
 * @param is_configured Set to whether display common took the request.
 * @return `OK`, including when it is refused, or what `firmware::control`
 * returns, logged as its doc describes, for an error other than
 * `ERR_FIRMWARE`.
 */
int32_t configure_stream(
    firmware& fw,
    rm::handle display_common,
    uint8_t head,
    const output_link& link,
    bool enhanced_framing,
    const displayport::stream_parameters& stream,
    bool& is_configured
);

/**
 * @brief Ask display common to override a display id's main stream attributes
 * as NVIDIA's DisplayPort library does unless its caller asks for in-band
 * stereo signaling, which the driver never does, clearing the MISC1 byte's
 * stereo bits and keeping the override for the next mode. A refusal is logged
 * and the stream keeps the attributes the firmware sets, as NVIDIA's library
 * goes on after one.
 * @param fw The bound firmware.
 * @param display_common The display common object.
 * @param display_id The display id, a single bit.
 * @return `OK`, including when the request is refused, or what
 * `firmware::control` returns, logged as its doc describes, for an error
 * other than `ERR_FIRMWARE`.
 */
int32_t set_stream_attributes(firmware& fw, rm::handle display_common, uint32_t display_id);

/**
 * @brief Make the chosen output's link ready to carry its mode, logging each
 * step: assign its display id an output resource, read the resource's
 * DisplayPort facts, find the lowest configuration carrying the mode under
 * them, train the link there and check the training against the status bytes
 * NVIDIA's library reads, then program the head's stream and its attributes.
 * When the facts say the firmware trains no PHY repeaters, the configuration,
 * the training and the check leave out any the DPCD counts, as NVIDIA's
 * library then reads none. A step that fails is logged and leaves the link
 * not ready, except a refused override of the attributes, which leaves it
 * ready with those the firmware sets.
 * @param fw The bound firmware.
 * @param display_common The display common object.
 * @param has_crossbar Whether display common reports the crossbar.
 * @param output The chosen output.
 * @param dpcd The capabilities `read_output_dpcd` read for its monitor.
 * @param light The output's head and mode.
 * @param is_set_up Set to whether the link is ready.
 * @return `OK`, including when the link is not ready, or what
 * `firmware::control` returns, logged as its doc describes, for an error
 * other than `ERR_FIRMWARE`.
 */
int32_t set_up_output_link(
    firmware& fw,
    rm::handle display_common,
    bool has_crossbar,
    const display_output& output,
    const dpcd_capabilities& dpcd,
    const first_light& light,
    bool& is_set_up
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LINK_H
