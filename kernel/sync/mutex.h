#ifndef STELLUX_SYNC_MUTEX_H
#define STELLUX_SYNC_MUTEX_H

#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace sync {

struct mutex {
    spinlock lock;
    sched::task* owner;
    wait_queue wq;

    void init() {
        lock = SPINLOCK_INIT;
        owner = nullptr;
        wq.init();
    }
};

/**
 * Acquire the mutex. Blocks if held by another task.
 * Must not be called from IRQ context or by the idle task.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mutex_lock(mutex& m);

/**
 * Release the mutex. Wakes one waiting task if any.
 * Must be called by the current owner.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mutex_unlock(mutex& m);

/**
 * Try to acquire the mutex without blocking.
 * @return true if the lock was acquired, false if already held.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE bool mutex_trylock(mutex& m);

/**
 * Check if the mutex is currently held. Advisory only; the result
 * may be stale if checked without external synchronization.
 */
inline bool mutex_is_locked(const mutex& m) {
    return __atomic_load_n(&m.owner, __ATOMIC_RELAXED) != nullptr;
}

} // namespace sync

#endif // STELLUX_SYNC_MUTEX_H
