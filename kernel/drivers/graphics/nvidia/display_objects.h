// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_OBJECTS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_OBJECTS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

/**
 * The display's objects under the driver's device. Display common takes the
 * controls about connectors, heads (the units that each generate one monitor's
 * timing) and links (the units that each drive one connector). The bandwidth
 * check's object answers whether modes fit the display engine's limits. The
 * display is the parent of the engine's channels (its queues of commands).
 */
struct display_objects {
    rm::handle display_common;
    rm::handle bandwidth_check;
    rm::handle display;
};

/**
 * @brief Allocate display common, the bandwidth check's object and the display
 * under the driver's device, in that order and each with a handle from
 * `firmware::new_handle()` and no parameters, as NVIDIA's modesetting module
 * allocates them.
 * @param fw The driver's firmware, whose client must be allocated.
 * @param device The driver's device.
 * @param display The chip table's display entry, which gives the display's
 * class.
 * @param out Receives the three handles on `OK` and is left unchanged on an
 * error.
 * @return `OK`, or the error `firmware::allocate` returns for an object,
 * logged as it describes. After an error the objects allocated before it are
 * released in the reverse order and their results dropped, so the firmware may
 * still hold the object that failed or any of those before it, as
 * `firmware::allocate` and `firmware::release` describe.
 */
int32_t allocate_display_objects(firmware& fw, rm::handle device, const chip_display& display, display_objects& out);

/**
 * @brief Release the display, the bandwidth check's object and display common,
 * in the reverse order of their allocation and each even when an earlier one
 * fails, as NVIDIA's modesetting module frees them.
 * @param fw The driver's firmware.
 * @param objects The handles `allocate_display_objects` gave.
 * @return `OK`, or the first error of `firmware::release`, each logged as it
 * describes.
 */
int32_t release_display_objects(firmware& fw, const display_objects& objects);

/**
 * @brief Ask the display and then display common for their capabilities,
 * tables of bits naming what the display engine supports and the workarounds
 * it needs, each request carrying its table zeroed. A display whose
 * capabilities say its engine reaches push buffers (the memory a channel of
 * the engine takes its commands from), semaphores (memory the engine reads and
 * writes to keep in step with other work) and notifiers (memory the engine
 * writes its progress into) only in video memory is refused before display
 * common is asked, as this driver does not place them there, while NVIDIA's
 * modesetting module then places all three in video memory.
 * @param fw The driver's firmware.
 * @param objects The handles `allocate_display_objects` gave.
 * @param has_crossbar Receives on `OK` whether display common reports a
 * crossbar (the switch that lets any link drive any connector), and is left
 * unchanged on an error.
 * @return `OK`, what `firmware::control` returns for either request, logged as
 * its doc describes, or `ERR_BAD_STATE`, logged here, when the display's
 * engine reaches push buffers, semaphores and notifiers only in video memory.
 */
int32_t read_display_capabilities(firmware& fw, const display_objects& objects, bool& has_crossbar);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_OBJECTS_H
