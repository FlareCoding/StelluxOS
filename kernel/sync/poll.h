#ifndef STELLUX_SYNC_POLL_H
#define STELLUX_SYNC_POLL_H

#include "common/types.h"
#include "common/list.h"
#include "sync/spinlock.h"

namespace sched { struct task; }

namespace sync {

struct wait_queue;
struct poll_table;

struct poll_entry {
    list::node observer_link;
    list::node table_link;
    poll_table* table;
    wait_queue* source;
};

struct poll_table {
    sched::task* task;
    uint32_t triggered;
    spinlock lock;
    list::head<poll_entry, &poll_entry::table_link> entries;

    void init(sched::task* t) {
        task = t;
        triggered = 0;
        lock = SPINLOCK_INIT;
        entries.init();
    }
};

/**
 * Register an observer on a wait queue. After this call, any wake_one
 * or wake_all on wq will also wake pt.task.
 * Caller provides the poll_entry storage (stack or heap).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void poll_subscribe(poll_table& pt, wait_queue& wq, poll_entry& entry);

/**
 * Block until any subscribed source fires or timeout expires.
 * @param timeout_ns 0 = infinite wait (no timeout).
 * @return true if triggered by a source, false on timeout.
 * Caller must check sched::is_kill_pending() after return.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool poll_wait(poll_table& pt, uint64_t timeout_ns);

/**
 * Remove all entries from their source wait queues.
 * Must be called before poll_entry memory goes out of scope.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void poll_cleanup(poll_table& pt);

} // namespace sync

#endif // STELLUX_SYNC_POLL_H
