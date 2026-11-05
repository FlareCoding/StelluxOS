#include "drivers/graphics/nvidia/operation_queue.h"
#include "dynpriv/dynpriv.h"

namespace drivers::nvidia {

operation_queue::operation_queue(device_driver& driver) : m_driver(driver) {
    m_done.init();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t operation_queue::run_on_task(operation& op) {
    op.is_waited = true;
    append(op);
    m_driver.notify_event();

    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
    while (!op.is_done) {
        irq = sync::wait(m_done, m_lock, irq);
    }

    sync::spin_unlock_irqrestore(m_lock, irq);

    return op.result;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void operation_queue::post_to_task(operation& op) {
    op.is_waited = false;
    append(op);
    m_driver.notify_event();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void operation_queue::post_unless_queued(operation& op) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
    if (!op.is_queued) {
        op.is_waited = false;
        link_last(op);
    }

    sync::spin_unlock_irqrestore(m_lock, irq);
    m_driver.notify_event();
}

void operation_queue::serve() {
    operation* op = take_next();
    while (op != nullptr) {
        // A record no caller waits for may be freed by its own work
        bool is_waited = op->is_waited;
        op->work(*op);

        if (is_waited) {
            RUN_ELEVATED({
                sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
                op->is_done = true;
                sync::wake_all(m_done);
                sync::spin_unlock_irqrestore(m_lock, irq);
            });
        }

        op = take_next();
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void operation_queue::append(operation& op) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
    link_last(op);
    sync::spin_unlock_irqrestore(m_lock, irq);
}

void operation_queue::link_last(operation& op) {
    op.is_done = false;
    op.is_queued = true;
    op.next = nullptr;
    if (m_tail != nullptr) {
        m_tail->next = &op;
    } else {
        m_head = &op;
    }

    m_tail = &op;
}

operation* operation_queue::take_next() {
    operation* op = nullptr;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
        op = m_head;
        if (op != nullptr) {
            op->is_queued = false;
            m_head = op->next;
            if (m_head == nullptr) {
                m_tail = nullptr;
            }
        }

        sync::spin_unlock_irqrestore(m_lock, irq);
    });

    return op;
}

} // namespace drivers::nvidia
