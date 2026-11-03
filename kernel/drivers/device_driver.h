#ifndef STELLUX_DRIVERS_DEVICE_DRIVER_H
#define STELLUX_DRIVERS_DEVICE_DRIVER_H

#include "common/types.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace sched { struct task; }

namespace drivers {

/**
 * Abstract base for all hardware device drivers.
 *
 * A device_driver owns a piece of hardware discovered by a bus (PCI, FDT, etc).
 * The driver framework spawns a dedicated kernel task for each driver instance.
 * Concrete drivers implement attach/run to initialize and operate the hardware.
 *
 * Whatever has work for the task, an interrupt handler or another task, calls
 * `notify_event()`, and the task waits for it with `wait_for_event()`.
 */
class device_driver {
public:
    device_driver(const char* name) : m_name(name), m_task(nullptr), m_event_pending(false) {
        m_event_wq.init();
        m_event_lock = sync::SPINLOCK_INIT;
    }

    virtual ~device_driver() = default;

    device_driver(const device_driver&) = delete;
    device_driver& operator=(const device_driver&) = delete;

    /**
     * Bind to hardware: map registers, allocate DMA, set up interrupts.
     * Called synchronously during bus enumeration.
     * @return 0 on success, negative error code on failure. A PCI driver may
     * also return `DECLINED`, a positive value that `pci_driver` describes.
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

    /**
     * Mark an event pending and wake the task if it waits for one. Safe to call
     * from any context, an interrupt handler among them.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void notify_event();

    const char* name() const { return m_name; }
    sched::task* task() const { return m_task; }

protected:
    /**
     * Block until `notify_event()` marks an event pending, then clear it. One
     * task may wait at a time, and it must drain all pending work on return,
     * since events coalesce.
     */
    void wait_for_event();

    /**
     * Block like `wait_for_event()`, but return once the clock reaches
     * `deadline_ns` too. The caller checks its work and the clock to tell an
     * event from the deadline.
     * @param deadline_ns The `clock::now_ns()` time at which to stop waiting.
     */
    void wait_for_event_until(uint64_t deadline_ns);

    const char* m_name;
    sched::task* m_task;

    sync::wait_queue m_event_wq;
    sync::spinlock   m_event_lock;
    bool             m_event_pending;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_DEVICE_DRIVER_H
