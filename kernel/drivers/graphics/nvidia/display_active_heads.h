// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ACTIVE_HEADS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ACTIVE_HEADS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/display_outputs.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

/**
 * A head (one of the display engine's units that each generate one monitor's
 * timing) as the boot firmware left it, showing one display id or nothing.
 */
struct active_head {
    uint32_t display_id; // a single bit, or 0 for a head that shows nothing
};

/**
 * Each head as the boot firmware left it, where a head that display common
 * refused to report counts as showing nothing. A head that shows a display id
 * is active, the lowest active head is the boot head, and the output the boot
 * head shows, if any, is the boot output.
 */
struct active_heads {
    active_head heads[DISPLAY_HEADS_MAX]; // entry i for head i, all 0 for a head that shows nothing or was not read
};

/**
 * @brief For each head below `head_count` in ascending order, ask display
 * common for the display id the head shows, each request carrying its
 * parameters zeroed but for the head, so its flags of 0 find a display the
 * boot firmware lit as well as one a client set up, and give the heads with
 * their display ids. NVIDIA's modesetting module asks in the same order after
 * allocating its core channel and binding the channel's notifier, since before
 * the allocation the firmware misreports the dynamic display ids it allocates
 * for a boot display on a DisplayPort multi-stream device. A head the firmware
 * refuses to report with `ERR_FIRMWARE` counts as showing nothing and is
 * logged, as that module goes on past it. A head reported showing several
 * display ids is refused as malformed, where that module asserts against it
 * and goes on with the first display id the report names.
 * @param fw The driver's firmware.
 * @param display_common The handle `allocate_display_objects` gave display
 * common.
 * @param head_count The heads `read_display_heads` left the display engine,
 * which must be at most `DISPLAY_HEADS_MAX`.
 * @param out Receives on `OK` the heads, each showing one display id or
 * nothing, those from `head_count` on showing nothing, and is left unchanged
 * on an error.
 * @return `OK`, what `firmware::control` returns for a request that fails
 * other than with `ERR_FIRMWARE`, logged as its doc describes, or
 * `ERR_BAD_MESSAGE`, logged here, for a head reported showing several display
 * ids.
 */
int32_t read_active_heads(firmware& fw, rm::handle display_common, uint8_t head_count, active_heads& out);

/**
 * @brief Give the boot head, the lowest active head.
 * @param heads The heads `read_active_heads` gave.
 * @param head Receives the boot head, below the `head_count` the heads were
 * read for, when a head is active, and is left unchanged otherwise.
 * @return Whether a head is active.
 */
bool find_boot_head(const active_heads& heads, uint8_t& head);

/**
 * @brief Give the output a head shows, the output whose display id is the
 * head's. A head that shows nothing shows no output, and neither does one
 * showing a display id that is no output, such as one whose signal is not
 * DisplayPort, though that head is still active, so when it is the boot head
 * there is no boot output. A dynamic display id is no output either, where
 * NVIDIA's modesetting module takes the connector of the static display id the
 * firmware names as its root port.
 * @param heads The heads `read_active_heads` gave.
 * @param head The head, which must be below `DISPLAY_HEADS_MAX`.
 * @param outputs The outputs `read_display_outputs` gave.
 * @param index Receives the index in `outputs` of the output `head` shows,
 * below its `count`, when there is one, and is left unchanged otherwise.
 * @return Whether `head` shows an output.
 */
bool find_shown_output(const active_heads& heads, uint8_t head, const display_outputs& outputs, uint8_t& index);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ACTIVE_HEADS_H
