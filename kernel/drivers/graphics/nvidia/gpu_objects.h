// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_GPU_OBJECTS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_GPU_OBJECTS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

// The driver's device and subdevice, the objects under its client that stand for the GPU and under which its other
// objects lie
struct gpu_objects {
    rm::handle device;
    rm::handle subdevice;
};

/**
 * @brief Allocate the driver's device under its client, then the subdevice
 * under the device, each with a handle from `firmware::new_handle()` and
 * index 0, the first GPU. The device names the driver's client as the client
 * it shares a GPU virtual address space with, which gives it one of its own,
 * as NVIDIA's modesetting module allocates its device.
 * @param fw The driver's requests to the firmware, whose client must be
 * allocated.
 * @param out Receives both handles on `OK` and is left unchanged on an error.
 * @return `OK`, or the error `firmware::allocate` returns for the device or
 * the subdevice, logged as it describes. After any error of the subdevice's
 * allocation the device's release is asked for and its result dropped, so
 * after an error the firmware may still hold the device or the subdevice, as
 * `firmware::allocate` and `firmware::release` describe.
 */
int32_t allocate_gpu_objects(firmware& fw, gpu_objects& out);

/**
 * @brief Release the subdevice, then the device, the second even when the
 * first fails, as NVIDIA's modesetting module frees them.
 * @param fw The driver's requests to the firmware.
 * @param objects The handles `allocate_gpu_objects` gave.
 * @return `OK`, or the first error of `firmware::release`, each logged as it
 * describes.
 */
int32_t release_gpu_objects(firmware& fw, const gpu_objects& objects);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_GPU_OBJECTS_H
