#ifndef STELLUX_SCHED_TASK_REGISTRY_H
#define STELLUX_SCHED_TASK_REGISTRY_H

#include "sched/task.h"
#include "common/hash.h"
#include "sync/spinlock.h"

namespace sched {

struct tid_key_ops {
    using key_type = uint32_t;
    static key_type key_of(const task& t) { return t.tid; }
    static uint64_t hash(const key_type& k) {
        return hash::u64(static_cast<uint64_t>(k));
    }
    static bool equal(const key_type& a, const key_type& b) { return a == b; }
};

class task_registry {
public:
    static constexpr uint32_t BUCKET_COUNT = 1024;

    /**
     * Initialize the registry. Must be called before any insert/remove.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t init();

    /**
     * Insert a task into the registry. Task must have a valid TID.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void insert(task* t);

    /**
     * Remove a task from the registry. Safe to call on tasks that were
     * never inserted (pprev guard).
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void remove(task& t);

    /**
     * Copy up to max TIDs of registered tasks into buf.
     * @return Number of TIDs written.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint32_t snapshot_tids(uint32_t* buf, uint32_t max);

    /**
     * Acquire the registry lock. Must be paired with unlock().
     * @note Privilege: **required**
     */
    [[nodiscard]] __PRIVILEGED_CODE sync::irq_state lock();

    /**
     * Release the registry lock.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void unlock(sync::irq_state irq);

    /**
     * Find a task by TID. Caller must hold the registry lock.
     * The returned pointer is valid only while the lock is held.
     * @return task pointer, or nullptr if not found.
     * @note Privilege: **required**
     */
    [[nodiscard]] __PRIVILEGED_CODE task* find_locked(uint32_t tid);

    /**
     * Advisory task count. May be stale under concurrent mutation.
     */
    uint32_t count() const;

private:
    using map_type = hashmap::map<task, &task::task_registry_link, tid_key_ops>;

    sync::spinlock  m_lock;
    hashmap::bucket m_buckets[BUCKET_COUNT];
    map_type        m_map;
};

extern task_registry g_task_registry;

} // namespace sched

#endif // STELLUX_SCHED_TASK_REGISTRY_H
