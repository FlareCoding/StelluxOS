#ifndef STELLUX_SYNC_SPINLOCK_H
#define STELLUX_SYNC_SPINLOCK_H

#include "common/types.h"
#include "hw/cpu.h"
#include "sync/atomic.h"

namespace sync {

struct alignas(64) spinlock {
    uint16_t next_ticket;
    uint16_t now_serving;
};

constexpr spinlock SPINLOCK_INIT = {0, 0};

struct irq_state {
    uint64_t flags;
};

inline void spin_lock(spinlock& lock) {
    uint16_t ticket = atomic_ref<uint16_t>{lock.next_ticket}.fetch_add_relaxed(1);

    atomic_ref<uint16_t> serving{lock.now_serving};
    while (serving.load_acquire() != ticket) {
        cpu::relax();
    }
}

inline void spin_unlock(spinlock& lock) {
    atomic_ref<uint16_t>{lock.now_serving}.fetch_add_release(1);
    cpu::send_event();
}

/**
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE inline irq_state spin_lock_irqsave(spinlock& lock) {
    irq_state state{cpu::irq_save()};
    spin_lock(lock);
    return state;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void spin_unlock_irqrestore(spinlock& lock, irq_state state) {
    spin_unlock(lock);
    cpu::irq_restore(state.flags);
}

class irq_lock_guard {
public:
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE explicit irq_lock_guard(spinlock& lk)
        : m_lock(lk), m_state(spin_lock_irqsave(lk)) {}

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE ~irq_lock_guard() {
        spin_unlock_irqrestore(m_lock, m_state);
    }

    irq_lock_guard(const irq_lock_guard&) = delete;
    irq_lock_guard& operator=(const irq_lock_guard&) = delete;
    irq_lock_guard(irq_lock_guard&&) = delete;
    irq_lock_guard& operator=(irq_lock_guard&&) = delete;

private:
    spinlock& m_lock;
    irq_state m_state;
};

} // namespace sync

#endif // STELLUX_SYNC_SPINLOCK_H
