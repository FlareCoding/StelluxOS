#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_H

#include "common/types.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

/**
 * The driver's requests to the GSP's firmware: to allocate and release objects
 * it keeps under the driver's own client (the owner of a tree of objects), to
 * send those objects controls (commands to an object), and to send controls to
 * the subdevice of the firmware's internal client, which the firmware keeps
 * for the controls NVIDIA's driver sends on its own behalf. `gsp` implements
 * it, and tests replace it with a double. `allocate`, `control`, `release` and
 * `internal_control` each spin on the calling CPU, once their request is sent,
 * until the firmware's reply arrives or `RECEIVE_TIMEOUT_NS` passes, and only
 * one thread may make the calls at a time, as only one may use a `gsp`.
 */
class firmware {
public:
    virtual ~firmware() = default;

    /**
     * @brief Allocate an object of one of NVIDIA's classes under the driver's
     * client.
     * @param parent The object to allocate it under, the client itself for a
     * device.
     * @param object The handle the object takes, from `new_handle()`.
     * @param class_id NVIDIA's number for the object's class.
     * @param params The class's allocation parameters in NVIDIA's flat
     * layout, which receive the reply's copy on `OK` and are left unchanged
     * on an error. It may be null when `size` is 0.
     * @param size How many bytes `params` holds, refused above what one
     * message carries.
     * @return `OK`, `ERR_TOO_LARGE` when the parameters do not fit one
     * message, `ERR_NO_MEMORY` when the request's buffer cannot be allocated,
     * `ERR_QUEUE_FULL` when the request cannot be queued, `ERR_TIMEOUT` when
     * the reply does not arrive in time, `ERR_FIRMWARE` when the firmware
     * refuses the allocation, or `ERR_BAD_MESSAGE` when the reply ends before
     * the parameters or a message in the status queue fails its checks. After
     * `ERR_TIMEOUT` or `ERR_BAD_MESSAGE` the firmware may hold the object.
     * Every error is logged.
     */
    virtual int32_t allocate(rm::handle parent, rm::handle object, uint32_t class_id, void* params, size_t size) = 0;

    /**
     * @brief Send a control to an object under the driver's client.
     * @param object An object `allocate` allocated, given by its handle.
     * @param command NVIDIA's number for the control, one that the object's
     * class accepts.
     * @param params The control's parameters in NVIDIA's flat layout, which
     * receive the reply's copy on `OK` and are left unchanged on an error. It
     * may be null when `size` is 0.
     * @param size How many bytes `params` holds, refused above what one
     * message carries.
     * @return `OK`, `ERR_TOO_LARGE` when the parameters do not fit one
     * message, `ERR_NO_MEMORY` when the request's buffer cannot be allocated,
     * `ERR_QUEUE_FULL` when the request cannot be queued, `ERR_TIMEOUT` when
     * the reply does not arrive in time, `ERR_FIRMWARE` when the firmware
     * refuses the request or the control, or `ERR_BAD_MESSAGE` when the reply
     * ends before the parameters or a message in the status queue fails its
     * checks. Every error is logged.
     */
    virtual int32_t control(rm::handle object, uint32_t command, void* params, size_t size) = 0;

    /**
     * @brief Release an object under the driver's client.
     * @param object An object `allocate` allocated, given by its handle.
     * @return `OK`, `ERR_QUEUE_FULL` when the request cannot be queued,
     * `ERR_TIMEOUT` when the reply does not arrive in time, `ERR_BAD_MESSAGE`
     * when a message in the status queue fails its checks, or `ERR_FIRMWARE`
     * when the firmware refuses the release. After `ERR_TIMEOUT` or
     * `ERR_BAD_MESSAGE` the firmware may have released the object. Every
     * error is logged.
     */
    virtual int32_t release(rm::handle object) = 0;

    /**
     * @brief A handle for an object to allocate under the driver's client, one
     * no object under it has had before.
     */
    virtual rm::handle new_handle() = 0;

    /**
     * @brief The driver's client, 0 while the driver has none.
     */
    virtual rm::handle client() const = 0;

    /**
     * @brief Send a control to the subdevice (the object standing for this
     * GPU) of the firmware's internal client.
     * @param command NVIDIA's number for the control, one of the internal
     * subdevice's.
     * @param params The control's parameters in NVIDIA's flat layout, which
     * receive the reply's copy on `OK` and are left unchanged on an error. It
     * may be null when `size` is 0.
     * @param size How many bytes `params` holds, refused above what one
     * message carries.
     * @return `OK`, `ERR_TOO_LARGE` when the parameters do not fit one
     * message, `ERR_NO_MEMORY` when the request's buffer cannot be allocated,
     * `ERR_QUEUE_FULL` when the request cannot be queued, `ERR_TIMEOUT` when
     * the reply does not arrive in time, `ERR_FIRMWARE` when the firmware
     * refuses the request or the control, or `ERR_BAD_MESSAGE` when the reply
     * ends before the parameters or a message in the status queue fails its
     * checks. Every error is logged.
     */
    virtual int32_t internal_control(uint32_t command, void* params, size_t size) = 0;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_H
