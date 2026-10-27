// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_EDID_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_EDID_H

#include "common/types.h"
#include "drivers/graphics/edid.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

/**
 * @brief Ask display common for the EDID of the monitor on a connected output
 * (the monitor's description of itself, with the timings it accepts), the
 * request carrying its parameters zeroed but for the display id, and parse it
 * with `edid::parse`. When the firmware refuses the request with
 * `ERR_FIRMWARE`, reports more EDID bytes than its reply holds or gives an
 * EDID that fails to parse, the read logs it and leaves `is_parsed` cleared.
 * NVIDIA's modesetting module instead first takes a DisplayPort monitor's EDID
 * from its DisplayPort code when that code holds one, and hands it to the
 * firmware before asking for it as here. For a DisplayPort monitor it asks
 * again up to 18 more times, sleeping 500 microseconds before each, while a
 * request fails for any cause or gives no bytes, and goes on without the EDID
 * when every try or an allocation fails, where here a failed allocation or a
 * request failing with another error than `ERR_FIRMWARE` ends the read. It
 * also copies as many EDID bytes as a reply reports, even more than the reply
 * holds.
 * @param fw The driver's firmware.
 * @param display_common The handle `allocate_display_objects` gave display
 * common.
 * @param display_id The display id of an output `read_display_outputs` marks
 * connected.
 * @param out Rewritten whole by `edid::parse` once a reply carries an EDID of
 * at most `rm::EDID_BUFFER_BYTES` bytes, every field zero after a failed
 * parse, and left unchanged otherwise.
 * @param is_parsed Set when `out` holds the monitor's EDID, and cleared
 * otherwise.
 * @return `OK`, `ERR_NO_MEMORY` when the request's parameters cannot be
 * allocated, logged, or what `firmware::control` returns, logged as its doc
 * describes, for a request the firmware does not refuse with `ERR_FIRMWARE`.
 */
int32_t read_output_edid(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    edid::monitor& out,
    bool& is_parsed
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_EDID_H
