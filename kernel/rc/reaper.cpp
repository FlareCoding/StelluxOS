#include "rc/reaper.h"
#include "sched/sched.h"
#include "sched/task_exec_core.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "common/logging.h"

namespace rc {
namespace reaper {

__PRIVILEGED_DATA static sync::atomic<dead_node*> g_queue_head{nullptr};
__PRIVILEGED_DATA static sync::wait_queue g_wait_queue;
__PRIVILEGED_DATA static sync::spinlock g_wait_lock = sync::SPINLOCK_INIT;
__PRIVILEGED_DATA static sync::atomic<uint32_t> g_initialized{0};

constexpr uint32_t QUEUED_EMPTY = 0;
constexpr uint32_t QUEUED_FIRST = 1;
constexpr uint32_t RETRY_COUNT_MAX = 0xFFFFFFFEu;

static uint32_t encode_retry_count(uint32_t retry_count) {
    if (retry_count > RETRY_COUNT_MAX) {
        retry_count = RETRY_COUNT_MAX;
    }
    return retry_count + 1;
}

static uint32_t decode_retry_count(uint32_t encoded) {
    if (encoded == QUEUED_EMPTY) {
        return 0;
    }
    return encoded - 1;
}

static dead_node* reverse_list(dead_node* head) {
    dead_node* reversed = nullptr;
    while (head) {
        dead_node* next = head->next;
        head->next = reversed;
        reversed = head;
        head = next;
    }
    return reversed;
}

__PRIVILEGED_CODE static dead_node* pop_all() {
    dead_node* head = g_queue_head.exchange_acq_rel(nullptr);
    return reverse_list(head);
}

__PRIVILEGED_CODE static void enqueue_chain(dead_node* first, bool wake_if_empty) {
    if (!first) {
        return;
    }

    dead_node* tail = first;
    while (tail->next) {
        tail = tail->next;
    }

    dead_node* old_head = g_queue_head.load_acquire();
    do {
        tail->next = old_head;
    } while (!g_queue_head.cmpxchg_strong_release(old_head, first));

    if (wake_if_empty && old_head == nullptr) {
        // Serialized with g_wait_lock to avoid lost wakeups.
        sync::irq_state irq = sync::spin_lock_irqsave(g_wait_lock);
        sync::wake_one(g_wait_queue);
        sync::spin_unlock_irqrestore(g_wait_lock, irq);
    }
}

__PRIVILEGED_CODE static void requeue_retry(dead_node* node, uint32_t retry_count) {
    uint32_t encoded_retry = encode_retry_count(retry_count);
    uint32_t expected = QUEUED_EMPTY;
    if (!node->queued.cmpxchg_strong_acq_rel(expected, encoded_retry)) {
        return;
    }

    node->next = nullptr;
    enqueue_chain(node, false);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void reaper_main(void*) {
    while (true) {
        dead_node* list = pop_all();
        if (!list) {
            sync::irq_state irq = sync::spin_lock_irqsave(g_wait_lock);
            while (g_queue_head.load_acquire() == nullptr) {
                irq = sync::wait(g_wait_queue, g_wait_lock, irq);
            }
            sync::spin_unlock_irqrestore(g_wait_lock, irq);
            continue;
        }

        uint32_t processed = 0;
        dead_node* cursor = list;
        while (cursor && processed < REAPER_BATCH_SIZE) {
            dead_node* node = cursor;
            cursor = cursor->next;
            node->next = nullptr;

            uint32_t encoded = node->queued.exchange_acq_rel(QUEUED_EMPTY);
            uint32_t retry_count = decode_retry_count(encoded);

            cleanup_result result = DONE;
            if (node->cleanup) {
                result = node->cleanup(node);
            }

            if (result == RETRY_LATER) {
                requeue_retry(node, retry_count + 1);
            }

            processed++;
        }

        if (cursor) {
            enqueue_chain(cursor, false);
        }

        // Fairness: give other runnable tasks a chance after each batch.
        sched::yield();
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    uint32_t expected = 0;
    if (!g_initialized.cmpxchg_strong_acq_rel(expected, 1)) {
        return OK;
    }

    g_queue_head.store_release(nullptr);
    g_wait_lock = sync::SPINLOCK_INIT;
    g_wait_queue.init();

    sched::task* task = sched::create_kernel_task(
        reaper_main, nullptr, "reaper", sched::TASK_FLAG_ELEVATED);
    if (!task) {
        g_initialized.store_release(0);
        return ERR_NO_MEM;
    }

    sched::enqueue(task);

    log::info("reaper: initialized");
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void defer(dead_node* node) {
    if (!node || !node->cleanup) {
        return;
    }

    uint32_t expected = QUEUED_EMPTY;
    if (!node->queued.cmpxchg_strong_acq_rel(expected, QUEUED_FIRST)) {
        return;
    }

    node->next = nullptr;
    enqueue_chain(node, true);
}

} // namespace reaper
} // namespace rc
