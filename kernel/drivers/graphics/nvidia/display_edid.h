// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_EDID_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_EDID_H

#include "common/types.h"
#include "drivers/graphics/edid.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

// NVIDIA's NV_DP_READ_EDID_RETRIES and NV_DP_REREAD_EDID_DELAY_USEC: the most times `read_output_edid` asks again for a
// monitor's description of itself while its base block fails its checksum, and the microseconds it waits before each
constexpr uint32_t EDID_REREADS_MAX     = 18;
constexpr uint32_t EDID_REREAD_DELAY_US = 500;

/**
 * @brief Ask display common for the EDID of the monitor on a connected output
 * (the monitor's description of itself, with the timings it accepts), each
 * request carrying its parameters zeroed but for the display id, and parse it
 * with `edid::parse`, asking again while the parse finds the base block's
 * checksum bad, up to `EDID_REREADS_MAX` more times, spinning on the calling
 * CPU for `EDID_REREAD_DELAY_US` before each. A CTA-861 or DisplayID extension
 * block failing its checksum is skipped by the parse, not read again. When the
 * firmware refuses a request with `ERR_FIRMWARE`, reports more EDID bytes than
 * its reply holds, or gives an EDID that fails to parse for another cause or
 * whose base block fails its checksum on every read, the read logs it and
 * leaves `is_parsed` cleared. NVIDIA's modesetting module instead first takes
 * a DisplayPort monitor's EDID from its DisplayPort code when that code holds
 * one, and hands it to the firmware before asking for it as here. That code
 * reads the EDID again for up to 100 milliseconds at each of three bus
 * addresses while a read fails or gives an EDID failing its checksum, and
 * keeps a failing EDID once a later read gives the same bytes as the first
 * failing one. For a DisplayPort monitor the module asks again up to
 * `EDID_REREADS_MAX` more times, sleeping `EDID_REREAD_DELAY_US` before each,
 * while a request fails for any cause or gives no bytes, never for a bad
 * checksum, and goes on without the EDID when every try or an allocation
 * fails, where here a failed allocation or a request failing with another
 * error than `ERR_FIRMWARE` ends the read. It also copies as many EDID bytes
 * as a reply reports, even more than the reply holds.
 * @param fw The driver's firmware.
 * @param display_common The handle `allocate_display_objects` gave display
 * common.
 * @param display_id The display id of an output `read_display_outputs` marks
 * connected.
 * @param out Rewritten whole by `edid::parse` each time a reply carries an
 * EDID of at most `rm::EDID_BUFFER_BYTES` bytes, every field zero after a
 * failed parse, and left unchanged when none does.
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
