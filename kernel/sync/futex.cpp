#include "sync/futex.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"
#include "mm/uaccess.h"
#include "mm/vma.h"
#include "mm/mm.h"
#include "common/hash.h"
#include "common/string.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "syscall/syscall_table.h"

namespace sync {

constexpr uint32_t WAKE_BATCH_SIZE = 16;

// A FUTEX_WAKE_OP update, from the high bits: operation:4 comparison:4 operand:12 comparand:12
constexpr uint32_t UPDATE_OPERATION_SHIFT  = 28;
constexpr uint32_t UPDATE_COMPARISON_SHIFT = 24;
constexpr uint32_t UPDATE_OPERAND_SHIFT    = 12;
constexpr uint32_t UPDATE_FIELD_MASK       = 0xF;
constexpr uint32_t UPDATE_ARGUMENT_MASK    = 0xFFF;
constexpr uint32_t UPDATE_ARGUMENT_SIGN    = 0x800;
constexpr uint32_t UPDATE_OPERAND_IS_SHIFT = 0x8; // Operation bit that makes the operand a shift count
constexpr uint32_t MAX_UPDATE_SHIFT        = 31;

// Bounds how long racing writers can keep an update retrying with interrupts masked
constexpr uint32_t MAX_LOCKED_UPDATE_ATTEMPTS = 128;

namespace {

// Operations and comparisons, numbered as FUTEX_WAKE_OP encodes them
enum class update_operation : uint32_t {
    SET     = 0,
    ADD     = 1,
    OR      = 2,
    AND_NOT = 3,
    XOR     = 4,
};

enum class update_comparison : uint32_t {
    EQUAL         = 0,
    NOT_EQUAL     = 1,
    LESS          = 2,
    LESS_EQUAL    = 3,
    GREATER       = 4,
    GREATER_EQUAL = 5,
};

struct word_update {
    update_operation  operation;
    uint32_t          operand;
    update_comparison comparison;
    int32_t           comparand;
};

} // namespace

__PRIVILEGED_BSS static futex_bucket g_futex_table[FUTEX_BUCKET_COUNT];

__PRIVILEGED_CODE void futex_init() {
    for (uint32_t i = 0; i < FUTEX_BUCKET_COUNT; i++) {
        g_futex_table[i].lock = SPINLOCK_INIT;
        g_futex_table[i].waiters.init();
    }
}

static uint32_t futex_hash(mm::mm_context* mm, uintptr_t addr) {
    uint64_t h = hash::combine(hash::ptr(mm), hash::u64(addr));
    return static_cast<uint32_t>(h) & FUTEX_BUCKET_MASK;
}

static bool word_aligned(uintptr_t addr) {
    return addr % sizeof(uint32_t) == 0;
}

/**
 * Locks the bucket holding `waiter`, which a requeue can change until that lock is held.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static futex_bucket* lock_waiter_bucket(futex_waiter& waiter, irq_state* out_irq) {
    while (true) {
        futex_bucket* bucket = atomic_ref<futex_bucket*>(waiter.bucket).load_acquire();
        irq_state irq = spin_lock_irqsave(bucket->lock);
        if (atomic_ref<futex_bucket*>(waiter.bucket).load_relaxed() == bucket) {
            *out_irq = irq;
            return bucket;
        }

        spin_unlock_irqrestore(bucket->lock, irq);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool waiter_queued(futex_waiter& waiter) {
    irq_state irq;
    futex_bucket* bucket = lock_waiter_bucket(waiter, &irq);
    bool queued = waiter.link.is_linked();
    spin_unlock_irqrestore(bucket->lock, irq);

    return queued;
}

/**
 * Reads the futex word, faulting its page in if needed, so the caller must not hold a bucket lock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool read_word(mm::mm_context* mm, uintptr_t uaddr, uint32_t* out) {
    if (!mm) {
        string::memcpy(out, reinterpret_cast<const void*>(uaddr), sizeof(uint32_t));
        return true;
    }

    int32_t rc = mm::uaccess::copy_from_user(out, reinterpret_cast<const void*>(uaddr), sizeof(uint32_t));
    return rc == mm::uaccess::OK;
}

/**
 * Reads the futex word under a bucket lock, failing on a page that is not present.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool load_word_locked(mm::mm_context* mm, uintptr_t uaddr, uint32_t* out) {
    if (!mm) {
        *out = atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(uaddr)).load_relaxed();
        return true;
    }

    int32_t rc = mm::uaccess::load_u32_from_user(reinterpret_cast<const uint32_t*>(uaddr), out);
    return rc == mm::uaccess::OK;
}

/**
 * Locks both buckets in address order, so requeues in opposite directions cannot deadlock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static irq_state lock_bucket_pair(futex_bucket* a, futex_bucket* b) {
    if (a == b) {
        return spin_lock_irqsave(a->lock);
    }

    futex_bucket* first = a < b ? a : b;
    futex_bucket* second = a < b ? b : a;
    irq_state irq = spin_lock_irqsave(first->lock);
    spin_lock(second->lock);
    return irq;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void unlock_bucket_pair(futex_bucket* a, futex_bucket* b, irq_state irq) {
    if (a == b) {
        spin_unlock_irqrestore(a->lock, irq);
        return;
    }

    futex_bucket* first = a < b ? a : b;
    futex_bucket* second = a < b ? b : a;
    spin_unlock(second->lock);
    spin_unlock_irqrestore(first->lock, irq);
}

/**
 * Moves a waiter onto `uaddr2` without waking it. The caller must hold both bucket locks.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void move_waiter(futex_waiter& waiter, futex_bucket* from, futex_bucket* to,
                                          uintptr_t uaddr2) {
    waiter.addr = uaddr2;
    if (from != to) {
        from->waiters.remove(&waiter);
        to->waiters.push_back(&waiter);
        atomic_ref<futex_bucket*>(waiter.bucket).store_release(to);
    }
}

__PRIVILEGED_CODE int32_t futex_wait_until(uintptr_t uaddr, uint32_t expected,
                                           uint64_t deadline_ns, uint32_t bitset) {
    if (!word_aligned(uaddr) || bitset == 0) {
        return syscall::EINVAL;
    }

    sched::task* self = sched::current();
    mm::mm_context* mm = self->exec.mm_ctx;

    uint32_t pre_val;
    if (!read_word(mm, uaddr, &pre_val)) {
        return syscall::EFAULT;
    }

    if (pre_val != expected) {
        return syscall::EAGAIN;
    }

    uint32_t idx = futex_hash(mm, uaddr);
    futex_bucket* bucket = &g_futex_table[idx];

    futex_waiter waiter = {};
    waiter.task = self;
    waiter.mm = mm;
    waiter.addr = uaddr;
    waiter.bitset = bitset;
    waiter.bucket = bucket;

    irq_state irq = spin_lock_irqsave(bucket->lock);

    // Re-read under the bucket lock so a waker cannot slip between the check
    // and the enqueue, a page unmapped since the first read reports EFAULT.
    uint32_t current_val;
    if (!load_word_locked(mm, uaddr, &current_val)) {
        spin_unlock_irqrestore(bucket->lock, irq);
        return syscall::EFAULT;
    }

    if (current_val != expected) {
        spin_unlock_irqrestore(bucket->lock, irq);
        return syscall::EAGAIN;
    }

    sched::prepare_to_block_task();
    bucket->waiters.push_back(&waiter);

    bool timed = deadline_ns > 0;
    if (timed) {
        timer::schedule_sleep(self, deadline_ns);
    }

    spin_unlock_irqrestore(bucket->lock, irq);

    while (!sched::block_task_interrupted() && waiter_queued(waiter) &&
           (!timed || clock::now_ns() < deadline_ns)) {
        sched::yield();
        sched::prepare_to_block_task();
    }

    sched::cancel_block_task();
    if (timed) {
        timer::cancel_sleep(self);
    }

    // Still queued means no wake arrived, unless a requeue claimed the waiter to wake it
    bool woken = true;
    futex_bucket* holder = lock_waiter_bucket(waiter, &irq);
    if (waiter.link.is_linked()) {
        holder->waiters.remove(&waiter);
        woken = waiter.claimed_by != nullptr;
    }
    spin_unlock_irqrestore(holder->lock, irq);

    if (signals::interrupt_pending(self)) {
        return syscall::EINTR;
    }

    return woken ? 0 : syscall::ETIMEDOUT;
}

__PRIVILEGED_CODE int32_t futex_wait(uintptr_t uaddr, uint32_t expected,
                                     uint64_t timeout_ns) {
    uint64_t deadline = timeout_ns > 0 ? clock::now_ns() + timeout_ns : 0;
    return futex_wait_until(uaddr, expected, deadline, FUTEX_BITSET_ANY);
}

__PRIVILEGED_CODE int32_t futex_wake(uintptr_t uaddr, uint32_t count) {
    return futex_wake_bitset(uaddr, count, FUTEX_BITSET_ANY);
}

__PRIVILEGED_CODE int32_t futex_wake_bitset(uintptr_t uaddr, uint32_t count, uint32_t bitset) {
    if (!word_aligned(uaddr) || bitset == 0) {
        return syscall::EINVAL;
    }

    if (count == 0) {
        return 0;
    }

    sched::task* self = sched::current();
    mm::mm_context* mm = self->exec.mm_ctx;
    uint32_t idx = futex_hash(mm, uaddr);
    futex_bucket* bucket = &g_futex_table[idx];

    uint32_t total_woken = 0;

    while (true) {
        rc::strong_ref<sched::task> batch[WAKE_BATCH_SIZE];
        uint32_t n = 0;
        bool done = false;

        irq_state irq = spin_lock_irqsave(bucket->lock);

        auto it = bucket->waiters.begin();
        auto end = bucket->waiters.end();
        while (it != end && n < WAKE_BATCH_SIZE) {
            futex_waiter& w = *it;
            ++it; // advance before removal
            if (w.mm == mm && w.addr == uaddr && !w.claimed_by && (w.bitset & bitset)) {
                bucket->waiters.remove(&w);
                batch[n++] = sched::task_ref(w.task);
                if (total_woken + n >= count) {
                    done = true;
                    break;
                }
            }
        }

        if (n == 0) {
            done = true;
        }
        spin_unlock_irqrestore(bucket->lock, irq);

        for (uint32_t i = 0; i < n; i++) {
            if (batch[i]) {
                sched::wake(batch[i].ptr());
            }
        }
        total_woken += n;

        if (done) {
            break;
        }
    }

    return static_cast<int32_t>(total_woken);
}

__PRIVILEGED_CODE int32_t futex_wake_all(uintptr_t uaddr) {
    if (!word_aligned(uaddr)) {
        return syscall::EINVAL;
    }

    sched::task* self = sched::current();
    mm::mm_context* mm = self->exec.mm_ctx;
    uint32_t idx = futex_hash(mm, uaddr);
    futex_bucket* bucket = &g_futex_table[idx];

    uint32_t total_woken = 0;

    while (true) {
        rc::strong_ref<sched::task> batch[WAKE_BATCH_SIZE];
        uint32_t n = 0;

        irq_state irq = spin_lock_irqsave(bucket->lock);

        auto it = bucket->waiters.begin();
        auto end = bucket->waiters.end();
        while (it != end && n < WAKE_BATCH_SIZE) {
            futex_waiter& w = *it;
            ++it;
            if (w.mm == mm && w.addr == uaddr && !w.claimed_by) {
                bucket->waiters.remove(&w);
                batch[n++] = sched::task_ref(w.task);
            }
        }

        bool drained = (n == 0);
        spin_unlock_irqrestore(bucket->lock, irq);

        for (uint32_t i = 0; i < n; i++) {
            if (batch[i]) {
                sched::wake(batch[i].ptr());
            }
        }
        total_woken += n;

        if (drained) {
            break;
        }
    }

    return static_cast<int32_t>(total_woken);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void wake_claimed(futex_bucket* bucket, sched::task* claimer) {
    while (true) {
        rc::strong_ref<sched::task> batch[WAKE_BATCH_SIZE];
        uint32_t n = 0;

        irq_state irq = spin_lock_irqsave(bucket->lock);

        auto it = bucket->waiters.begin();
        auto end = bucket->waiters.end();
        while (it != end && n < WAKE_BATCH_SIZE) {
            futex_waiter& w = *it;
            ++it;
            if (w.claimed_by == claimer) {
                bucket->waiters.remove(&w);
                batch[n++] = sched::task_ref(w.task);
            }
        }
        spin_unlock_irqrestore(bucket->lock, irq);

        for (uint32_t i = 0; i < n; i++) {
            if (batch[i]) {
                sched::wake(batch[i].ptr());
            }
        }

        if (n < WAKE_BATCH_SIZE) {
            return;
        }
    }
}

__PRIVILEGED_CODE int32_t futex_requeue(uintptr_t uaddr, uintptr_t uaddr2, uint32_t nr_wake,
                                        uint32_t nr_requeue, const uint32_t* expected) {
    if (!word_aligned(uaddr) || !word_aligned(uaddr2)) {
        return syscall::EINVAL;
    }

    sched::task* self = sched::current();
    mm::mm_context* mm = self->exec.mm_ctx;

    // Faults the word in first, since the comparison under the bucket locks cannot page it in
    uint32_t value = 0;
    if (expected && !read_word(mm, uaddr, &value)) {
        return syscall::EFAULT;
    }

    futex_bucket* from = &g_futex_table[futex_hash(mm, uaddr)];
    futex_bucket* to = &g_futex_table[futex_hash(mm, uaddr2)];
    irq_state irq = lock_bucket_pair(from, to);

    if (expected) {
        bool readable = load_word_locked(mm, uaddr, &value);
        if (!readable || value != *expected) {
            unlock_bucket_pair(from, to, irq);
            return readable ? syscall::EAGAIN : syscall::EFAULT;
        }
    }

    // Waking under a spinlock can deadlock, so waiters to wake are only claimed until the locks drop
    uint32_t claimed = 0;
    uint32_t moved = 0;
    auto it = from->waiters.begin();
    auto end = from->waiters.end();
    while (it != end && (claimed < nr_wake || moved < nr_requeue)) {
        futex_waiter& w = *it;
        ++it;
        if (w.mm != mm || w.addr != uaddr || w.claimed_by) {
            continue;
        }

        if (claimed < nr_wake) {
            w.claimed_by = self;
            claimed++;
        } else {
            move_waiter(w, from, to, uaddr2);
            moved++;
        }
    }
    unlock_bucket_pair(from, to, irq);

    if (claimed > 0) {
        wake_claimed(from, self);
    }

    return static_cast<int32_t>(claimed + moved);
}

static uint32_t sign_extend_argument(uint32_t bits) {
    uint32_t argument = bits & UPDATE_ARGUMENT_MASK;
    return (argument & UPDATE_ARGUMENT_SIGN) ? (argument | ~UPDATE_ARGUMENT_MASK) : argument;
}

static bool decode_word_update(uint32_t encoded, word_update* out) {
    uint32_t operation = (encoded >> UPDATE_OPERATION_SHIFT) & UPDATE_FIELD_MASK;
    uint32_t comparison = (encoded >> UPDATE_COMPARISON_SHIFT) & UPDATE_FIELD_MASK;
    uint32_t operand = sign_extend_argument(encoded >> UPDATE_OPERAND_SHIFT);

    if (operation & UPDATE_OPERAND_IS_SHIFT) {
        if (operand > MAX_UPDATE_SHIFT) {
            return false;
        }

        operation &= ~UPDATE_OPERAND_IS_SHIFT;
        operand = 1u << operand;
    }

    if (operation > static_cast<uint32_t>(update_operation::XOR) ||
        comparison > static_cast<uint32_t>(update_comparison::GREATER_EQUAL)) {
        return false;
    }

    out->operation = static_cast<update_operation>(operation);
    out->operand = operand;
    out->comparison = static_cast<update_comparison>(comparison);
    out->comparand = static_cast<int32_t>(sign_extend_argument(encoded));

    return true;
}

static uint32_t apply_word_update(const word_update& update, uint32_t value) {
    switch (update.operation) {
        case update_operation::SET:
            return update.operand;
        case update_operation::ADD:
            return value + update.operand;
        case update_operation::OR:
            return value | update.operand;
        case update_operation::AND_NOT:
            return value & ~update.operand;
        case update_operation::XOR:
            return value ^ update.operand;
    }
}

static bool comparison_holds(const word_update& update, uint32_t old_value) {
    int32_t value = static_cast<int32_t>(old_value);
    switch (update.comparison) {
        case update_comparison::EQUAL:
            return value == update.comparand;
        case update_comparison::NOT_EQUAL:
            return value != update.comparand;
        case update_comparison::LESS:
            return value < update.comparand;
        case update_comparison::LESS_EQUAL:
            return value <= update.comparand;
        case update_comparison::GREATER:
            return value > update.comparand;
        case update_comparison::GREATER_EQUAL:
            return value >= update.comparand;
    }
}

/**
 * One weak compare-exchange on the futex word under a bucket lock. Returns 0 when it stored,
 * -EAGAIN when it did not, with *expected then holding the word as found, or -EFAULT.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t cmpxchg_word_locked(mm::mm_context* mm, uintptr_t uaddr, uint32_t* expected,
                                                     uint32_t desired) {
    uint32_t* word = reinterpret_cast<uint32_t*>(uaddr);
    if (!mm) {
        return atomic_ref<uint32_t>(*word).cmpxchg_weak_acq_rel(*expected, desired) ? 0 : syscall::EAGAIN;
    }

    int32_t rc = mm::uaccess::cmpxchg_u32_in_user(word, expected, desired);
    if (rc == mm::uaccess::OK) {
        return 0;
    }

    return rc == mm::uaccess::ERR_RETRY ? syscall::EAGAIN : syscall::EFAULT;
}

/**
 * Applies `update` to the futex word under the bucket locks, where a missing page cannot be faulted in.
 * Returns 0 with the old value in *out_old, -EFAULT, or -EAGAIN once racing writers kept winning.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t update_word_locked(mm::mm_context* mm, uintptr_t uaddr, const word_update& update,
                                                    uint32_t* out_old) {
    uint32_t old_value = 0;
    if (!load_word_locked(mm, uaddr, &old_value)) {
        return syscall::EFAULT;
    }

    for (uint32_t attempt = 0; attempt < MAX_LOCKED_UPDATE_ATTEMPTS; attempt++) {
        int32_t rc = cmpxchg_word_locked(mm, uaddr, &old_value, apply_word_update(update, old_value));
        if (rc != syscall::EAGAIN) {
            *out_old = old_value;
            return rc;
        }
    }

    return syscall::EAGAIN;
}

/**
 * Claims up to `count` waiters on `uaddr` for `claimer` to wake once the locks drop. The caller must
 * hold the bucket's lock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint32_t claim_waiters(futex_bucket* bucket, mm::mm_context* mm, uintptr_t uaddr,
                                                uint32_t count, sched::task* claimer) {
    uint32_t claimed = 0;
    auto it = bucket->waiters.begin();
    auto end = bucket->waiters.end();
    while (it != end && claimed < count) {
        futex_waiter& w = *it;
        ++it;
        if (w.mm == mm && w.addr == uaddr && !w.claimed_by) {
            w.claimed_by = claimer;
            claimed++;
        }
    }

    return claimed;
}

__PRIVILEGED_CODE int32_t futex_wake_op(uintptr_t uaddr, uintptr_t uaddr2, uint32_t nr_wake,
                                        uint32_t nr_wake2, uint32_t update) {
    word_update decoded = {};
    if (!word_aligned(uaddr) || !word_aligned(uaddr2) || !decode_word_update(update, &decoded)) {
        return syscall::EINVAL;
    }

    sched::task* self = sched::current();
    mm::mm_context* mm = self->exec.mm_ctx;
    futex_bucket* first = &g_futex_table[futex_hash(mm, uaddr)];
    futex_bucket* second = &g_futex_table[futex_hash(mm, uaddr2)];

    while (true) {
        uint32_t old_value = 0;
        uint32_t claimed = 0;

        irq_state irq = lock_bucket_pair(first, second);

        int32_t rc = update_word_locked(mm, uaddr2, decoded, &old_value);
        if (rc == 0) {
            claimed = claim_waiters(first, mm, uaddr, nr_wake, self);
            if (comparison_holds(decoded, old_value)) {
                claimed += claim_waiters(second, mm, uaddr2, nr_wake2, self);
            }
        }

        unlock_bucket_pair(first, second, irq);

        if (rc == 0) {
            if (claimed > 0) {
                wake_claimed(first, self);
                if (second != first) {
                    wake_claimed(second, self);
                }
            }

            return static_cast<int32_t>(claimed);
        }

        // With the locks dropped, the page can be faulted in for writing before the update retries
        if (rc == syscall::EFAULT && mm::handle_user_pf(mm, uaddr2, mm::PF_FLAG_WRITE) != mm::MM_CTX_OK) {
            return syscall::EFAULT;
        }

        if (rc == syscall::EAGAIN) {
            sched::yield();
        }
    }
}

} // namespace sync
