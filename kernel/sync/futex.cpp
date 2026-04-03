#include "sync/futex.h"
#include "sync/spinlock.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/vma.h"
#include "common/hash.h"
#include "common/string.h"
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

    // Read the value before taking the bucket lock. copy_from_user
    // acquires mm_ctx->lock (a sleeping mutex) so it must not be called
    // under a spinlock. This also faults in the page so the re-read
    // under the spinlock below is safe.
    uint32_t pre_val;
    if (mm) {
        int32_t rc = mm::uaccess::copy_from_user(
            &pre_val, reinterpret_cast<const void*>(uaddr), sizeof(uint32_t));
        if (rc != 0) return -14; // EFAULT
    } else {
        string::memcpy(&pre_val, reinterpret_cast<const void*>(uaddr),
                       sizeof(uint32_t));
    }

    // Early exit: if the value already changed, no need to lock the bucket.
    if (pre_val != expected) return -11; // EAGAIN

    uint32_t idx = futex_hash(mm, uaddr);
    futex_bucket* bucket = &g_futex_table[idx];

    futex_waiter waiter;
    waiter.task = self;
    waiter.mm = mm;
    waiter.addr = uaddr;
    waiter.link = {};

    irq_state irq = spin_lock_irqsave(bucket->lock);

    // Re-read the futex word under the bucket lock. The page is already
    // validated/faulted by the copy_from_user above, so a direct read
    // is safe here. This atomic check-and-enqueue prevents lost wakeups.
    uint32_t current_val;
    string::memcpy(&current_val, reinterpret_cast<const void*>(uaddr),
                   sizeof(uint32_t));

    if (current_val != expected) {
        spin_unlock_irqrestore(bucket->lock, irq);
        return -11; // EAGAIN
    }

    self->state = sched::TASK_STATE_BLOCKED;
    bucket->waiters.push_back(&waiter);

    if (timeout_ns > 0) {
        uint64_t deadline = clock::now_ns() + timeout_ns;
        timer::schedule_sleep(self, deadline);
    }

    spin_unlock_irqrestore(bucket->lock, irq);
    sched::yield();

    // Cancel any outstanding timer to prevent spurious wakes of future
    // blocking operations if we were woken by futex_wake before timeout.
    timer::cancel_sleep(self);

    // Remove self from bucket if still linked (timeout or kill wakeup).
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

    uint32_t total_woken = 0;

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
                if (total_woken + n >= count) {
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
        total_woken += n;

        if (done) break;
    }

    return static_cast<int32_t>(total_woken);
}

__PRIVILEGED_CODE int32_t futex_wake_all(uintptr_t uaddr) {
    sched::task* self = sched::current();
    mm::mm_context* mm = self->exec.mm_ctx;
    if (uaddr & 0x3) return -22; // EINVAL

    uint32_t idx = futex_hash(mm, uaddr);
    futex_bucket* bucket = &g_futex_table[idx];

    uint32_t total_woken = 0;

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
        total_woken += n;

        if (drained) break;
    }

    return static_cast<int32_t>(total_woken);
}

} // namespace sync
