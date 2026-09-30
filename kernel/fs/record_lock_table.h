#ifndef STELLUX_FS_RECORD_LOCK_TABLE_H
#define STELLUX_FS_RECORD_LOCK_TABLE_H

#include "common/types.h"
#include "common/list.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace fs {

enum class record_lock_type : uint8_t {
    shared    = 0, // Held by any number of owners over the same bytes
    exclusive = 1, // Held by one owner, conflicting with every other owner's lock
};

// A byte range locked, or requested, by one owner
struct record_lock {
    const void*      owner; // Identifies the holder, compared by address only
    record_lock_type type;
    uint64_t         start;
    uint64_t         end;   // Last byte covered, never below start
    int32_t          pid;   // Process reported as the holder
};

struct record_lock_entry {
    list::node  link;
    record_lock lock;
};

using record_lock_list = list::head<record_lock_entry, &record_lock_entry::link>;

/**
 * The POSIX record locks on one file: advisory locks over byte ranges, which
 * reads and writes never consult. Locks of different owners conflict when they
 * overlap and either is exclusive. An owner's own locks never conflict and never
 * overlap, and they merge when adjacent and of the same type.
 */
class record_lock_table {
public:
    record_lock_table();
    ~record_lock_table();

    record_lock_table(const record_lock_table&) = delete;
    record_lock_table& operator=(const record_lock_table&) = delete;

    /**
     * @brief Finds another owner's lock that conflicts with `request`.
     * @return True with the lock copied into `out_conflict`, or false when none conflicts.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool find_conflict(const record_lock& request, record_lock* out_conflict);

    /**
     * @brief Locks the requested bytes for the request's owner, replacing whatever it held over them.
     * @return OK, ERR_AGAIN when another owner holds a conflicting lock, or ERR_NOMEM.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t try_lock(const record_lock& request);

    /**
     * @brief Like try_lock, but waits for conflicting locks to be released instead of failing.
     * @return OK, ERR_INTR when a signal ends the wait, or ERR_NOMEM.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t lock(const record_lock& request);

    /**
     * @brief Releases the owner's locks over [start, end], keeping their parts outside it.
     * @return OK, or ERR_NOMEM when a lock that must split in two cannot get memory.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t unlock(const void* owner, uint64_t start, uint64_t end);

    /**
     * @brief Releases every lock `owner` holds.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void unlock_all(const void* owner);

private:
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t acquire(const record_lock& request, bool wait);

    sync::spinlock   m_lock;
    record_lock_list m_locks;
    sync::wait_queue m_waiters; // Tasks blocked by a conflict, woken whenever held locks shrink or weaken
};

} // namespace fs

#endif // STELLUX_FS_RECORD_LOCK_TABLE_H
