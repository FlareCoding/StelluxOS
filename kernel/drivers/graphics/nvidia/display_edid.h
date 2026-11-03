// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_EDID_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_EDID_H

#include "common/types.h"
#include "drivers/graphics/edid.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

// NVIDIA's NV_DP_READ_EDID_RETRIES and NV_DP_REREAD_EDID_DELAY_USEC: the most EDID rereads and the wait before each
constexpr uint32_t EDID_REREADS_MAX     = 18;
constexpr uint32_t EDID_REREAD_DELAY_US = 500;

/**
 * @brief Read and parse the EDID of the monitor on a connected output, setting
 * `is_parsed` only when the last read's EDID parsed into `out`.
 *
 * Reads again while `edid::parse`'s result is worth one more read, up to
 * `EDID_REREADS_MAX` more times, spinning `EDID_REREAD_DELAY_US` before each.
 * @return `OK`, `ERR_NO_MEMORY` when the request cannot be allocated, or the
 * error of a request failing other than by `ERR_FIRMWARE`. Every error is
 * logged, as is a read leaving `is_parsed` cleared.
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
