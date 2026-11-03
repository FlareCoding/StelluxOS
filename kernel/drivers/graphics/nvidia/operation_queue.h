#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_OPERATION_QUEUE_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_OPERATION_QUEUE_H

#include "common/types.h"
#include "drivers/device_driver.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace drivers::nvidia {

/**
 * Work another context hands the driver task, since only the task calls the
 * firmware and the display engine's instance memory once `attach()` has
 * returned. `work` runs on the task and sets `result`.
 *
 * The record must be unprivileged memory, since the lowered task reads it and
 * `work` writes it. A caller that waits owns it again once the wait returns.
 */
struct operation {
    void (*work)(operation& op);
    int32_t    result;
    bool       is_waited; // a caller waits, so the queue marks the record done
    bool       is_done;   // set under the queue's lock once `work` returns
    operation* next;
};

/**
 * The driver task's operations, which it runs one at a time in arrival order
 * each time it wakes. Posting one never sleeps, while running one sleeps
 * until the task has run it.
 *
 * A caller must not wait while holding a lock the task takes, and the task
 * must never wait for an operation.
 */
class operation_queue {
public:
    /**
     * @brief A queue with nothing queued.
     * @param driver The driver whose task serves the queue and is woken for
     * each operation queued. It must outlive the queue.
     */
    explicit operation_queue(device_driver& driver);

    operation_queue(const operation_queue&) = delete;
    operation_queue& operator=(const operation_queue&) = delete;

    /**
     * @brief Queue `op`, wake the task and wait without a deadline until its
     * `work` returns.
     * @param op The record, with `work` set, which the caller owns again once
     * the call returns.
     * @return The result `work` set.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t run_on_task(operation& op);

    /**
     * @brief Queue `op` and wake the task without waiting, which cannot fail.
     * The queue never reads `op` once its `work` starts, so `work` may free it.
     * @param op The record, with `work` set, kept until its `work` starts.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void post_to_task(operation& op);

    /**
     * @brief Run the queued operations on the task until none is left, those
     * queued meanwhile included.
     */
    void serve();

private:
    /**
     * @brief Mark `op` not done and link it last.
     * @param op The record `run_on_task` or `post_to_task` was given.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void append(operation& op);

    operation* take_next();

    device_driver&   m_driver;
    sync::spinlock   m_lock = sync::SPINLOCK_INIT;
    sync::wait_queue m_done; // callers of `run_on_task` wait here
    operation*       m_head = nullptr;
    operation*       m_tail = nullptr;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_OPERATION_QUEUE_H
