// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_IDS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_IDS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

/**
 * The display ids (single bits that each name one way the firmware can drive a
 * monitor through one of the card's connectors) display common reports
 * supported, and those of them with a monitor connected when it was asked.
 */
struct display_ids {
    uint32_t supported;
    uint32_t connected; // a subset of `supported`
};

/**
 * @brief Ask display common which display ids are supported, then which of
 * those have a monitor connected, each request carrying its parameters zeroed
 * but for the display ids it asks about, and give both. Refused as malformed,
 * where NVIDIA's modesetting module takes the report as it is, is a report of
 * a connected display id that is not supported.
 * @param fw The driver's firmware.
 * @param display_common The handle `allocate_display_objects` gave display
 * common.
 * @param out Receives on `OK` the display ids, and is left unchanged on an
 * error.
 * @return `OK`, what `firmware::control` returns, logged as its doc describes,
 * or `ERR_BAD_MESSAGE`, logged here, for a report refused as malformed. So the
 * read fails when the firmware refuses the supported report, where NVIDIA's
 * module goes on with no display id and then fails its device for want of one,
 * and when it refuses the connected report, where NVIDIA's module asks again
 * while the reply names a retry time or the firmware is not ready, and takes
 * none as connected when the firmware still refuses it. A successful connected
 * reply is taken whatever retry time it names, where NVIDIA's module asks
 * again while a reply names one. Every supported display id is kept and asked
 * about, where NVIDIA's module keeps and asks about only those the firmware
 * reports static. A report of none supported still goes on to the connect
 * state, where NVIDIA's module fails its device.
 */
int32_t read_display_ids(firmware& fw, rm::handle display_common, display_ids& out);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_IDS_H
