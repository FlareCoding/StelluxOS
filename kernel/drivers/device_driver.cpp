#include "drivers/device_driver.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

namespace drivers {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void device_driver::notify_event() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_event_lock);
    m_event_pending = true;
    sync::wake_one(m_event_wq);
    sync::spin_unlock_irqrestore(m_event_lock, irq);
}

void device_driver::wait_for_event() {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_event_lock);
        while (!m_event_pending) {
            irq = sync::wait(m_event_wq, m_event_lock, irq);
        }

        m_event_pending = false;
        sync::spin_unlock_irqrestore(m_event_lock, irq);
    });
}

void device_driver::wait_for_event_until(uint64_t deadline_ns) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_event_lock);
        while (!m_event_pending && clock::now_ns() < deadline_ns) {
            irq = sync::wait_until(m_event_wq, m_event_lock, irq, deadline_ns);
        }

        m_event_pending = false;
        sync::spin_unlock_irqrestore(m_event_lock, irq);
    });
}

} // namespace drivers
