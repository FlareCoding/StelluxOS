#include "sched/task_registry.h"
#include "common/logging.h"

__PRIVILEGED_BSS sched::task_registry sched::g_task_registry;

namespace sched {

__PRIVILEGED_CODE int32_t task_registry::init() {
    m_lock = sync::SPINLOCK_INIT;
    m_map.init(m_buckets, BUCKET_COUNT);
    log::info("sched: task registry initialized (%u buckets)", BUCKET_COUNT);
    return 0;
}

__PRIVILEGED_CODE void task_registry::insert(task* t) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
    m_map.insert(t);
    sync::spin_unlock_irqrestore(m_lock, irq);
}

__PRIVILEGED_CODE void task_registry::remove(task& t) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
    if (t.task_registry_link.pprev) {
        m_map.remove(t);
    }
    sync::spin_unlock_irqrestore(m_lock, irq);
}

__PRIVILEGED_CODE uint32_t task_registry::snapshot_tids(uint32_t* buf, uint32_t max) {
    uint32_t written = 0;
    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
    m_map.for_each([&](task& t) {
        if (written < max) {
            buf[written++] = t.tid;
        }
    });
    sync::spin_unlock_irqrestore(m_lock, irq);
    return written;
}

__PRIVILEGED_CODE sync::irq_state task_registry::lock() {
    return sync::spin_lock_irqsave(m_lock);
}

__PRIVILEGED_CODE void task_registry::unlock(sync::irq_state irq) {
    sync::spin_unlock_irqrestore(m_lock, irq);
}

__PRIVILEGED_CODE task* task_registry::find_locked(uint32_t tid) {
    return m_map.find(tid);
}

uint32_t task_registry::count() const {
    return m_map.size();
}

} // namespace sched
