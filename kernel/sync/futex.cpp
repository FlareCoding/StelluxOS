#include "sync/futex.h"
#include "sync/spinlock.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/vma.h"
#include "common/hash.h"
#include "clock/clock.h"
#include "timer/timer.h"

namespace sync {

constexpr uint32_t WAKE_BATCH_SIZE = 16;

__PRIVILEGED_BSS static futex_bucket g_futex_table[FUTEX_BUCKET_COUNT];

__PRIVILEGED_CODE void futex_init() {
    for (uint32_t i = 0; i < FUTEX_BUCKET_COUNT; i++) {
        g_futex_table[i].lock = SPINLOCK_INIT;
        g_futex_table[i].waiters.init();
    }
}

__PRIVILEGED_CODE static uint32_t futex_hash(mm::mm_context* mm, uintptr_t addr) {
    uint64_t h = hash::combine(hash::ptr(mm), hash::u64(addr));
    return static_cast<uint32_t>(h) & FUTEX_BUCKET_MASK;
}

__PRIVILEGED_CODE int32_t futex_wait(uintptr_t uaddr, uint32_t expected,
                                     uint64_t timeout_ns) {
    sched::task* self = sched::current();
    mm::mm_context* mm = self->exec.mm_ctx;
    if (uaddr & 0x3) return -22; // EINVAL

    uint32_t idx = futex_hash(mm, uaddr);
    futex_bucket* bucket = &g_futex_table[idx];

    futex_waiter waiter;
    waiter.task = self;
    waiter.mm = mm;
    waiter.addr = uaddr;
    waiter.link = {};

    irq_state irq = spin_lock_irqsave(bucket->lock);

    uint32_t current_val;
    int32_t rc = mm::uaccess::copy_from_user(
        &current_val, reinterpret_cast<const void*>(uaddr), sizeof(uint32_t));
    if (rc != mm::uaccess::OK) {
        spin_unlock_irqrestore(bucket->lock, irq);
        return -14; // EFAULT
    }

    if (current_val != expected) {
        spin_unlock_irqrestore(bucket->lock, irq);
        return -11; // EAGAIN
    }

    // Value matches: enqueue and sleep
    self->state = sched::TASK_STATE_BLOCKED;
    bucket->waiters.push_back(&waiter);
    spin_unlock_irqrestore(bucket->lock, irq);

    if (timeout_ns > 0) {
        uint64_t deadline = clock::now_ns() + timeout_ns;
        timer::schedule_sleep(self, deadline);
    }

    sched::yield();

    // Woken up. Remove self from bucket if still linked (timeout or kill).
    bool was_linked = false;
    irq = spin_lock_irqsave(bucket->lock);
    if (waiter.link.is_linked()) {
        bucket->waiters.remove(&waiter);
        was_linked = true;
    }
    spin_unlock_irqrestore(bucket->lock, irq);

    if (sched::is_kill_pending()) return -4; // EINTR
    if (was_linked) return -110; // ETIMEDOUT
    return 0;
}

__PRIVILEGED_CODE int32_t futex_wake(uintptr_t uaddr, uint32_t count) {
    sched::task* self = sched::current();
    mm::mm_context* mm = self->exec.mm_ctx;
    if (uaddr & 0x3) return -22; // EINVAL
    if (count == 0) return 0;

    uint32_t idx = futex_hash(mm, uaddr);
    futex_bucket* bucket = &g_futex_table[idx];

    int32_t total_woken = 0;

    for (;;) {
        sched::task* batch[WAKE_BATCH_SIZE];
        uint32_t n = 0;
        bool done = false;

        irq_state irq = spin_lock_irqsave(bucket->lock);

        auto it = bucket->waiters.begin();
        auto end = bucket->waiters.end();
        while (it != end && n < WAKE_BATCH_SIZE) {
            futex_waiter& w = *it;
            ++it; // advance before removal
            if (w.mm == mm && w.addr == uaddr) {
                bucket->waiters.remove(&w);
                batch[n++] = w.task;
                if (total_woken + static_cast<int32_t>(n) >=
                    static_cast<int32_t>(count)) {
                    done = true;
                    break;
                }
            }
        }

        if (n == 0) done = true;
        spin_unlock_irqrestore(bucket->lock, irq);

        for (uint32_t i = 0; i < n; i++) {
            sched::wake(batch[i]);
        }
        total_woken += static_cast<int32_t>(n);

        if (done) break;
    }

    return total_woken;
}

__PRIVILEGED_CODE int32_t futex_wake_all(uintptr_t uaddr) {
    sched::task* self = sched::current();
    mm::mm_context* mm = self->exec.mm_ctx;
    if (uaddr & 0x3) return -22; // EINVAL

    uint32_t idx = futex_hash(mm, uaddr);
    futex_bucket* bucket = &g_futex_table[idx];

    int32_t total_woken = 0;

    for (;;) {
        sched::task* batch[WAKE_BATCH_SIZE];
        uint32_t n = 0;

        irq_state irq = spin_lock_irqsave(bucket->lock);

        auto it = bucket->waiters.begin();
        auto end = bucket->waiters.end();
        while (it != end && n < WAKE_BATCH_SIZE) {
            futex_waiter& w = *it;
            ++it;
            if (w.mm == mm && w.addr == uaddr) {
                bucket->waiters.remove(&w);
                batch[n++] = w.task;
            }
        }

        bool drained = (n == 0);
        spin_unlock_irqrestore(bucket->lock, irq);

        for (uint32_t i = 0; i < n; i++) {
            sched::wake(batch[i]);
        }
        total_woken += static_cast<int32_t>(n);

        if (drained) break;
    }

    return total_woken;
}

} // namespace sync
