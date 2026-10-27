// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_IDS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_IDS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

/**
 * The display ids (single bits that each name one way the firmware can drive a
 * monitor through one of the card's connectors) display common reports
 * supported, those of them with a monitor connected when it was asked, those
 * of them it reports digital and those of the digital ones it reports static.
 * A dynamic display id names a device behind a connector with a static one.
 */
struct display_ids {
    uint32_t supported;
    uint32_t connected;      // a subset of `supported`
    uint32_t digital;        // a subset of `supported`
    uint32_t static_digital; // a subset of `digital`
};

// The most display ids display common's masks hold, one per bit
constexpr uint32_t DISPLAY_IDS_MAX = 32;

/**
 * @brief Ask display common which display ids are supported and which of those
 * have a monitor connected, then each supported display id's kind in ascending
 * order of its bit and, for a digital one, whether the firmware allocates it
 * dynamically, each request carrying its parameters zeroed but for the display
 * ids it asks about, and give the supported, connected, digital and static
 * digital ones. A supported display id that is not digital, or whose kind the
 * firmware refuses with `ERR_FIRMWARE`, is logged and left out of the digital
 * ones, and the read goes on with the others. NVIDIA's modesetting module
 * instead takes the kind of a failed request as unknown, keeps a connector for
 * the display id whatever its kind and tells whether it drives DisplayPort
 * from the protocol display common reports for it, but sets a mode only on a
 * display id of digital or analog kind. A digital display id that is dynamic,
 * or whose dynamic flag the firmware refuses with `ERR_FIRMWARE`, is logged
 * and left out of the static ones, as NVIDIA's module keeps no connector for
 * either, and the read goes on likewise. Refused as malformed, with no later
 * request sent, where NVIDIA's module takes the report as it is, is a report
 * of a connected display id that is not supported.
 * @param fw The driver's firmware.
 * @param display_common The handle `allocate_display_objects` gave display
 * common.
 * @param out Receives on `OK` the display ids, and is left unchanged on an
 * error.
 * @return `OK`, what `firmware::control` returns, logged as its doc describes,
 * for any request but one for a display id's kind or dynamic flag the firmware
 * refuses with `ERR_FIRMWARE`, or `ERR_BAD_MESSAGE`, logged here, for a report
 * refused as malformed. So the read fails when the firmware refuses the
 * supported report, where NVIDIA's module goes on with no display id and then
 * fails its device for want of one, and when it refuses the connected report,
 * where NVIDIA's module asks again while the reply names a retry time or the
 * firmware is not ready, and takes none as connected when the firmware still
 * refuses it. A successful connected reply is taken whatever retry time it
 * names, where NVIDIA's module asks again while a reply names one. Every
 * supported display id is kept and asked its kind, where NVIDIA's module first
 * asks each whether it is dynamic, keeps only the static ones and asks about
 * only those whose connector it keeps. A report of none supported still goes
 * on to the connect state, where NVIDIA's module fails its device.
 */
int32_t read_display_ids(firmware& fw, rm::handle display_common, display_ids& out);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_IDS_H
