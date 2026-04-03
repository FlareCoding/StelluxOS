#ifndef STELLUX_SYNC_FUTEX_H
#define STELLUX_SYNC_FUTEX_H

#include "common/types.h"
#include "common/list.h"
#include "sync/spinlock.h"

namespace sched { struct task; }
namespace mm { struct mm_context; }

namespace sync {

struct futex_waiter {
    sched::task*    task;
    mm::mm_context* mm;
    uintptr_t       addr;
    list::node      link;
};

struct futex_bucket {
    spinlock lock;
    list::head<futex_waiter, &futex_waiter::link> waiters;
};

constexpr uint32_t FUTEX_BUCKET_COUNT = 256;
constexpr uint32_t FUTEX_BUCKET_MASK  = FUTEX_BUCKET_COUNT - 1;

/**
 * Initialize the futex hash table. Call once during boot after sched::init().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void futex_init();

/**
 * Block if *uaddr == expected. timeout_ns=0 means wait indefinitely.
 * Returns 0 on wake, -EAGAIN on mismatch, -ETIMEDOUT, -EINTR, -EFAULT.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t futex_wait(uintptr_t uaddr, uint32_t expected,
                                     uint64_t timeout_ns);

/**
 * Wake up to count threads waiting on uaddr. Returns number woken.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t futex_wake(uintptr_t uaddr, uint32_t count);

/**
 * Wake all threads waiting on uaddr. Returns number woken.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t futex_wake_all(uintptr_t uaddr);

} // namespace sync

#endif // STELLUX_SYNC_FUTEX_H
