// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LINK_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LINK_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/display_outputs.h"
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
 * @brief Find the chosen output's link, logging each step: assign its display
 * id an output resource and read the resource's DisplayPort facts. A step
 * that fails is logged, and the steps after it are not taken.
 * @param fw The bound firmware.
 * @param display_common The display common object.
 * @param has_crossbar Whether display common reports the crossbar.
 * @param output The chosen output.
 * @param is_set_up Set to whether every step succeeded.
 * @return `OK`, including when a step fails, or what `firmware::control`
 * returns, logged as its doc describes, for an error other than
 * `ERR_FIRMWARE`.
 */
int32_t set_up_output_link(
    firmware& fw,
    rm::handle display_common,
    bool has_crossbar,
    const display_output& output,
    bool& is_set_up
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LINK_H
