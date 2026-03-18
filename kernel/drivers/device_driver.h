#ifndef STELLUX_DRIVERS_DEVICE_DRIVER_H
#define STELLUX_DRIVERS_DEVICE_DRIVER_H

#include "common/types.h"

namespace sched { struct task; }

namespace drivers {

/**
 * Abstract base for all hardware device drivers.
 *
 * A device_driver owns a piece of hardware discovered by a bus (PCI, FDT, etc).
 * The driver framework spawns a dedicated kernel task for each driver instance.
 * Concrete drivers implement attach/run to initialize and operate the hardware.
 */
class device_driver {
public:
    device_driver(const char* name) : m_name(name), m_task(nullptr) {}
    virtual ~device_driver() = default;

    device_driver(const device_driver&) = delete;
    device_driver& operator=(const device_driver&) = delete;

    /**
     * Bind to hardware: map registers, allocate DMA, set up interrupts.
     * Called synchronously during bus enumeration.
     * @return 0 on success, negative error code on failure.
     */
    virtual int32_t attach() = 0;

    /**
     * Release hardware resources. Default is a no-op since most drivers
     * are permanent. Override for hot-removable or runtime-loaded drivers.
     * @return 0 on success, negative error code on failure.
     */
    virtual int32_t detach() { return 0; }

    /**
     * Driver main loop, executed in the driver's own kernel task.
     * The driver task is created and started as unprivileged/lowered.
     */
    virtual void run() = 0;

    const char* name() const { return m_name; }
    sched::task* task() const { return m_task; }

protected:
    const char* m_name;
    sched::task* m_task;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_DEVICE_DRIVER_H
