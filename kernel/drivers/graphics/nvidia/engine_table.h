#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINE_TABLE_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINE_TABLE_H

#include "common/types.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

// An engine (a unit of the GPU that runs the work channels give it) as the firmware's device info table lists it
struct engine_info {
    uint32_t type;        // NVIDIA's engine type, by which a channel's allocation names its engine
    uint32_t runlist;     // the runlist (the list of channels the engine's scheduler runs) its channels go on
    uint32_t channel_ram; // the BAR0 offset of the runlist's channel RAM, the scheduler's word for each channel id
};

/**
 * The GPU's engines, as the firmware's device info table lists them. A
 * channel runs on one engine, scheduled on that engine's runlist. A copy
 * engine sharing the graphics engine's runlist runs only inside a graphics
 * context, so a channel that only copies goes on an asynchronous copy engine,
 * one on a runlist other than the graphics engine's.
 */
class engine_table {
public:
    /**
     * @brief Read the table from `subdevice` through
     * `rm::CONTROL_GET_DEVICE_INFO_TABLE`, a page at a time while the reply
     * says more follow, replacing what the object held.
     * @return `OK`, `ERR_NO_MEMORY` when the reply's buffer cannot be
     * allocated, an error of `firmware::control`, or `ERR_BAD_MESSAGE` when a
     * page counts more entries than it has room for or the table runs past
     * `rm::DEVICE_INFO_ENTRIES_MAX`. Every error is logged and leaves the
     * table empty.
     */
    int32_t read(firmware& fw, rm::handle subdevice);

    // The asynchronous copy engine of the lowest type, or null when the table lists none
    const engine_info* first_async_copy_engine() const;

    // The graphics engine (NVIDIA's GR0), whose runlist holds every graphics channel, or null when the table lists none
    const engine_info* graphics_engine() const;

    uint32_t count() const;

private:
    engine_info m_engines[rm::DEVICE_INFO_ENTRIES_MAX] = {};
    uint32_t    m_count = 0;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINE_TABLE_H
