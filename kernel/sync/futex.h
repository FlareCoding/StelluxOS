#ifndef STELLUX_SYNC_FUTEX_H
#define STELLUX_SYNC_FUTEX_H

#include "common/types.h"
#include "common/list.h"
#include "sync/spinlock.h"

namespace sched { struct task; }
namespace mm { struct mm_context; }

namespace sync {

struct futex_bucket;

struct futex_waiter {
    sched::task*    task;
    mm::mm_context* mm;
    uintptr_t       addr;
    uint32_t        bitset;     // Wakes reach the waiter only when their bitset overlaps this one
    futex_bucket*   bucket;     // Holds the waiter, changed by requeue under both buckets' locks
    sched::task*    claimed_by; // Task whose requeue wakes this waiter once its locks drop
    list::node      link;
};

struct futex_bucket {
    spinlock lock;
    list::head<futex_waiter, &futex_waiter::link> waiters;
};

constexpr uint32_t FUTEX_BUCKET_COUNT = 256;
constexpr uint32_t FUTEX_BUCKET_MASK  = FUTEX_BUCKET_COUNT - 1;

// Bitset of a plain wait or wake, overlapping every other bitset
constexpr uint32_t FUTEX_BITSET_ANY = 0xFFFFFFFF;

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
 * Block if *uaddr == expected until a wake overlapping `bitset` or the monotonic deadline_ns (0 for none).
 * Returns 0 on wake, -EAGAIN on mismatch, -ETIMEDOUT, -EINTR, -EFAULT, -EINVAL.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t futex_wait_until(uintptr_t uaddr, uint32_t expected,
                                           uint64_t deadline_ns, uint32_t bitset);

/**
 * Wake up to count threads waiting on uaddr. Returns number woken.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t futex_wake(uintptr_t uaddr, uint32_t count);

/**
 * Wake up to count threads waiting on uaddr whose bitset overlaps `bitset`.
 * Returns number woken, or -EINVAL.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t futex_wake_bitset(uintptr_t uaddr, uint32_t count, uint32_t bitset);

/**
 * Wake all threads waiting on uaddr. Returns number woken.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t futex_wake_all(uintptr_t uaddr);

/**
 * Wake up to nr_wake threads waiting on uaddr and move up to nr_requeue others to uaddr2.
 * Returns the number woken or moved, -EAGAIN when *uaddr differs from a non-null expected, -EINVAL, -EFAULT.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t futex_requeue(uintptr_t uaddr, uintptr_t uaddr2, uint32_t nr_wake,
                                        uint32_t nr_requeue, const uint32_t* expected);

} // namespace sync

#endif // STELLUX_SYNC_FUTEX_H
