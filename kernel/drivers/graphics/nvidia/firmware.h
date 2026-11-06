#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_H

#include "common/types.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

// An event the GSP's firmware posts when one of the driver's event objects fires, as `gsp` decodes it
struct firmware_event {
    rm::handle client;             // the client the event object belongs to
    rm::handle event;              // the event object posted for
    uint32_t   notifier;           // the notifier that fired
    bool       is_for_notify_list; // `event` stands for its parent's list of event objects on `notifier`
};

/**
 * The receiver of each event the GSP's firmware posts, called on the context
 * reading the firmware's messages and inside the wait or drain that reads the
 * event.
 */
class firmware_event_sink {
public:
    virtual ~firmware_event_sink() = default;

    /**
     * @brief Take `event`. A log line here can outlast a CPU sequencer's poll
     * window, so the call must not log, sleep or send a request.
     * @return Whether the event is the sink's, else the wait or drain that
     * read it logs it as any other message it reads.
     */
    virtual bool on_firmware_event(const firmware_event& event) = 0;
};

// A kind of channel error the GSP's firmware reports
enum class channel_error : uint8_t {
    fault,  // an MMU fault the firmware queued, NVIDIA's MMU_FAULT_QUEUED
    engine, // an engine's error in a channel's work, NVIDIA's RC_TRIGGERED
};

/**
 * The receiver of each channel error the GSP's firmware reports, called as a
 * `firmware_event_sink` is and under its limits: the call must not log, sleep
 * or send a request.
 */
class firmware_error_sink {
public:
    virtual ~firmware_error_sink() = default;

    // Takes `error`, which the wait or drain that read it then does not log
    virtual void on_channel_error(channel_error error) = 0;
};

/**
 * The driver's requests to the GSP's firmware: to allocate and release objects
 * it keeps under the driver's own client (the owner of a tree of objects), to
 * send those objects controls (commands to an object), and to send controls to
 * the subdevice of the firmware's internal client, which the firmware keeps
 * for the controls NVIDIA's driver sends on its own behalf. `gsp` implements
 * it, and tests replace it with a double. It also hands each event the
 * firmware posts to the sink `set_event_sink` names, and each channel error to
 * the sink `set_error_sink` names. `allocate`, `control`, `release` and
 * `internal_control` each spin on the calling CPU, once their request is sent,
 * until the firmware's reply arrives or `RECEIVE_TIMEOUT_NS` passes, and only
 * one thread may make the calls at a time, as only one may use a `gsp`.
 *
 * From the driver task's start, a reply missing at its deadline marks the GPU
 * failed, and once the GPU is marked failed each of those four calls returns
 * `ERR_GPU_FAILED` in place of sending its request.
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
     * Every error is logged. Once the GPU is marked failed, the call returns
     * `ERR_GPU_FAILED` in place of sending its request.
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
     * checks. Every error is logged. Once the GPU is marked failed, the call
     * returns `ERR_GPU_FAILED` in place of sending its request.
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
     * error is logged. Once the GPU is marked failed, the call returns
     * `ERR_GPU_FAILED` in place of sending its request.
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
     * checks. Every error is logged. Once the GPU is marked failed, the call
     * returns `ERR_GPU_FAILED` in place of sending its request.
     */
    virtual int32_t internal_control(uint32_t command, void* params, size_t size) = 0;

    /**
     * @brief Hand each event the firmware posts to `sink` from this call on.
     * A null sink takes none, so each is logged as before any sink is set.
     * @param sink The sink, which must stay alive while it is set.
     */
    virtual void set_event_sink(firmware_event_sink* sink) = 0;

    /**
     * @brief Hand each channel error the firmware reports to `sink` from
     * this call on. A null sink takes none, so each is logged as before any
     * sink is set.
     * @param sink The sink, which must stay alive while it is set.
     */
    virtual void set_error_sink(firmware_error_sink* sink) = 0;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_H
