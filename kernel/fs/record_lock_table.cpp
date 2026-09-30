#include "fs/record_lock_table.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "signals/signal.h"

namespace fs {

static bool overlaps(const record_lock& held, uint64_t start, uint64_t end) {
    return held.start <= end && start <= held.end;
}

static bool conflicts(const record_lock& held, const record_lock& request) {
    bool either_exclusive = held.type == record_lock_type::exclusive ||
                            request.type == record_lock_type::exclusive;

    return held.owner != request.owner && overlaps(held, request.start, request.end) && either_exclusive;
}

// True when `later` starts on the byte right after `earlier` ends
static bool adjoins(const record_lock& earlier, const record_lock& later) {
    return earlier.end < later.start && later.start - earlier.end == 1;
}

static const record_lock_entry* find_conflict_in(record_lock_list& locks, const record_lock& request) {
    for (const record_lock_entry& entry : locks) {
        if (conflicts(entry.lock, request)) {
            return &entry;
        }
    }

    return nullptr;
}

// True when one of the owner's locks reaches past both ends of [start, end], so releasing the range splits it
static bool splits_owner_lock(record_lock_list& locks, const void* owner, uint64_t start, uint64_t end) {
    for (const record_lock_entry& entry : locks) {
        if (entry.lock.owner == owner && entry.lock.start < start && entry.lock.end > end) {
            return true;
        }
    }

    return false;
}

/**
 * Releases the owner's locks over [start, end]. Locks inside the range move to `removed` and locks
 * reaching past it are trimmed, with `split_entry` taking the right part of a lock that reaches past
 * both ends, after which it is set to null. Returns whether any lock changed.
 */
static bool release_owner_range(
    record_lock_list& locks,
    const void* owner,
    uint64_t start,
    uint64_t end,
    record_lock_entry*& split_entry,
    record_lock_list& removed
) {
    bool changed = false;

    record_lock_list::iterator it = locks.begin();
    while (it != locks.end()) {
        record_lock_entry* entry = &*it;
        record_lock& held = entry->lock;
        ++it;

        if (held.owner != owner || !overlaps(held, start, end)) {
            continue;
        }

        changed = true;
        if (held.start < start && held.end > end) {
            split_entry->lock = held;
            split_entry->lock.start = end + 1;
            locks.push_back(split_entry);
            split_entry = nullptr;
            held.end = start - 1;
        } else if (held.start < start) {
            held.end = start - 1;
        } else if (held.end > end) {
            held.start = end + 1;
        } else {
            locks.remove(entry);
            removed.push_back(entry);
        }
    }

    return changed;
}

static void release_owner_locks(record_lock_list& locks, const void* owner, record_lock_list& removed) {
    record_lock_list::iterator it = locks.begin();
    while (it != locks.end()) {
        record_lock_entry* entry = &*it;
        ++it;

        if (entry->lock.owner == owner) {
            locks.remove(entry);
            removed.push_back(entry);
        }
    }
}

// Links `added` into the list after absorbing the owner's adjoining locks of the same type
static void insert_merged(record_lock_list& locks, record_lock_entry* added, record_lock_list& removed) {
    record_lock& merged = added->lock;

    record_lock_list::iterator it = locks.begin();
    while (it != locks.end()) {
        record_lock_entry* entry = &*it;
        const record_lock& held = entry->lock;
        ++it;

        bool mergeable = held.owner == merged.owner && held.type == merged.type;
        bool precedes = mergeable && adjoins(held, merged);
        bool follows = mergeable && adjoins(merged, held);
        if (!precedes && !follows) {
            continue;
        }

        if (precedes) {
            merged.start = held.start;
        } else {
            merged.end = held.end;
        }

        locks.remove(entry);
        removed.push_back(entry);
    }

    locks.push_back(added);
}

static void free_entries(record_lock_list& entries) {
    while (record_lock_entry* entry = entries.pop_front()) {
        heap::ufree_delete(entry);
    }
}

record_lock_table::record_lock_table() {
    m_lock = sync::SPINLOCK_INIT;
    m_locks.init();
    m_waiters.init();
}

record_lock_table::~record_lock_table() {
    free_entries(m_locks);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool record_lock_table::find_conflict(const record_lock& request, record_lock* out_conflict) {
    sync::irq_lock_guard guard(m_lock);

    const record_lock_entry* conflict = find_conflict_in(m_locks, request);
    if (!conflict) {
        return false;
    }

    *out_conflict = conflict->lock;

    return true;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t record_lock_table::try_lock(const record_lock& request) {
    return acquire(request, false);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t record_lock_table::lock(const record_lock& request) {
    return acquire(request, true);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t record_lock_table::unlock(const void* owner, uint64_t start, uint64_t end) {
    // Entries are allocated before the table lock, which holds interrupts off
    record_lock_entry* split_entry = heap::ualloc_new<record_lock_entry>();
    record_lock_list removed;
    removed.init();

    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);

    // Memory matters only when the range splits one of the owner's locks in two
    if (!split_entry && splits_owner_lock(m_locks, owner, start, end)) {
        sync::spin_unlock_irqrestore(m_lock, irq);
        return ERR_NOMEM;
    }

    bool changed = release_owner_range(m_locks, owner, start, end, split_entry, removed);

    sync::spin_unlock_irqrestore(m_lock, irq);

    if (changed) {
        sync::wake_all(m_waiters);
    }

    heap::ufree_delete(split_entry);
    free_entries(removed);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void record_lock_table::unlock_all(const void* owner) {
    record_lock_list removed;
    removed.init();

    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);

    release_owner_locks(m_locks, owner, removed);

    sync::spin_unlock_irqrestore(m_lock, irq);

    if (!removed.empty()) {
        sync::wake_all(m_waiters);
    }

    free_entries(removed);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t record_lock_table::acquire(const record_lock& request, bool wait) {
    // Entries are allocated before the table lock, which holds interrupts off
    record_lock_entry* added = heap::ualloc_new<record_lock_entry>();
    if (!added) {
        return ERR_NOMEM;
    }

    added->lock = request;
    record_lock_entry* split_entry = heap::ualloc_new<record_lock_entry>();
    record_lock_list removed;
    removed.init();

    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);

    while (wait && find_conflict_in(m_locks, request) && !signals::interrupt_pending(sched::current())) {
        irq = sync::wait(m_waiters, m_lock, irq);
    }

    int32_t result = OK;
    if (find_conflict_in(m_locks, request)) {
        result = wait ? ERR_INTR : ERR_AGAIN;
    } else if (!split_entry && splits_owner_lock(m_locks, request.owner, request.start, request.end)) {
        result = ERR_NOMEM;
    }

    bool changed = false;
    if (result == OK) {
        changed = release_owner_range(m_locks, request.owner, request.start, request.end, split_entry, removed);
        insert_merged(m_locks, added, removed);
        added = nullptr;
    }

    sync::spin_unlock_irqrestore(m_lock, irq);

    // Replacing the owner's locks over the range can weaken them, which may let waiters in
    if (changed) {
        sync::wake_all(m_waiters);
    }

    heap::ufree_delete(added);
    heap::ufree_delete(split_entry);
    free_entries(removed);

    return result;
}

} // namespace fs
