#ifndef STELLUX_FS_FILE_LOCK_TABLE_H
#define STELLUX_FS_FILE_LOCK_TABLE_H

#include "common/types.h"
#include "common/list.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace fs {

class file;

enum class record_lock_type : uint8_t {
    shared    = 0, // Held by any number of owners over the same bytes
    exclusive = 1, // Held by one owner, conflicting with every other owner's lock
};

// Ordered from weakest to strongest, since the table compares values to tell when a holder's mode weakens
enum class whole_file_lock_mode : uint8_t {
    unlocked  = 0,
    shared    = 1, // Held by any number of open file descriptions at once
    exclusive = 2, // Held by one open file description, conflicting with every other description's lock
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
 * The advisory locks on one file, which reads and writes never consult: POSIX
 * record locks over byte ranges, and the whole-file locks `flock` takes. The two
 * kinds never conflict with each other, and they share one lock and wait queue.
 *
 * Record locks of different owners conflict when they overlap and either is
 * exclusive. An owner's own locks never conflict and never overlap, and they
 * merge when adjacent and of the same type.
 *
 * Whole-file locks belong to open file descriptions, so every handle to a
 * description shares its lock. Any number of descriptions may hold the lock
 * shared, or one may hold it exclusive. Each holder records its own mode, which
 * only this table changes, and the table counts holders instead of listing them.
 * A holder must therefore be a description of this table's file, and must unlock
 * before it is freed, or the table counts it as a holder forever.
 */
class file_lock_table {
public:
    file_lock_table();
    ~file_lock_table();

    file_lock_table(const file_lock_table&) = delete;
    file_lock_table& operator=(const file_lock_table&) = delete;

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

    /**
     * @brief Locks the file in `mode`, shared or exclusive, for the open file description
     * `holder` without waiting. The other mode, if `holder` holds it, is released first, so a
     * refused conversion leaves nothing held.
     * @return OK, or ERR_AGAIN when another description holds a conflicting lock.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t try_lock_whole_file(file& holder, whole_file_lock_mode mode);

    /**
     * @brief Like `try_lock_whole_file`, but waits for conflicting locks to be released instead
     * of failing. A conversion waits holding nothing, so two holders converting at once never
     * deadlock. Sleeps, so the caller must be able to.
     * @return OK, or ERR_INTR when a signal ends the wait.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t lock_whole_file(file& holder, whole_file_lock_mode mode);

    /**
     * @brief Releases the whole-file lock the open file description `holder` holds, if any.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void unlock_whole_file(file& holder);

private:
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t acquire(const record_lock& request, bool wait);

    /**
     * @brief True when a whole-file lock another description holds keeps `holder` from holding `mode`.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool whole_file_conflicts(const file& holder, whole_file_lock_mode mode) const;

    /**
     * @brief Records `holder` as holding `mode`.
     * @return True when the change weakens the mode `holder` held and so may let a waiter in.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool set_whole_file_mode(file& holder, whole_file_lock_mode mode);

    sync::spinlock   m_lock;
    record_lock_list m_locks;
    uint32_t         m_whole_file_shared_holders;
    bool             m_has_whole_file_exclusive_holder;
    sync::wait_queue m_waiters; // Tasks blocked by either kind of conflict, woken whenever held locks shrink or weaken
};

} // namespace fs

#endif // STELLUX_FS_FILE_LOCK_TABLE_H
