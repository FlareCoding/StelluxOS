#include "eventfd/eventfd.h"
#include "resource/resource.h"
#include "fs/fstypes.h"
#include "common/string.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "signals/signal.h"
#include "sync/poll.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "dynpriv/dynpriv.h"

namespace eventfd {

// Every read and write moves exactly one count
constexpr size_t COUNT_SIZE = sizeof(uint64_t);

namespace {

// A single queue, since every change can unblock both readers and writers
struct eventfd_counter {
    sync::spinlock lock;
    sync::wait_queue waiters;
    uint64_t count;
    bool semaphore;
};

} // anonymous namespace

/**
 * Takes the whole count, or one unit in semaphore mode, waiting for a nonzero count unless `nonblock`.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t take_count(eventfd_counter* counter, void* kdst, bool nonblock) {
    sync::irq_state irq = sync::spin_lock_irqsave(counter->lock);

    while (counter->count == 0 && !nonblock && !signals::interrupt_pending(sched::current())) {
        irq = sync::wait(counter->waiters, counter->lock, irq);
    }

    if (counter->count == 0) {
        sync::spin_unlock_irqrestore(counter->lock, irq);
        return nonblock ? resource::ERR_AGAIN : resource::ERR_INTR;
    }

    uint64_t taken = counter->semaphore ? 1 : counter->count;
    counter->count -= taken;

    sync::spin_unlock_irqrestore(counter->lock, irq);
    sync::wake_all(counter->waiters);

    string::memcpy(kdst, &taken, COUNT_SIZE);

    return static_cast<ssize_t>(COUNT_SIZE);
}

/**
 * Adds the count in `ksrc`, waiting for room below COUNTER_MAX unless `nonblock`.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t add_count(eventfd_counter* counter, const void* ksrc, bool nonblock) {
    uint64_t added = 0;
    string::memcpy(&added, ksrc, COUNT_SIZE);

    // No amount of waiting could make room for the all-ones value
    if (added > COUNTER_MAX) {
        return resource::ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(counter->lock);

    while (COUNTER_MAX - counter->count < added && !nonblock && !signals::interrupt_pending(sched::current())) {
        irq = sync::wait(counter->waiters, counter->lock, irq);
    }

    if (COUNTER_MAX - counter->count < added) {
        sync::spin_unlock_irqrestore(counter->lock, irq);
        return nonblock ? resource::ERR_AGAIN : resource::ERR_INTR;
    }

    counter->count += added;

    sync::spin_unlock_irqrestore(counter->lock, irq);
    sync::wake_all(counter->waiters);

    return static_cast<ssize_t>(COUNT_SIZE);
}

/**
 * Reports the handle readable while the count is above zero and writable while one more unit fits.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint32_t poll_counter(eventfd_counter* counter, sync::poll_table* pt) {
    if (pt) {
        sync::poll_subscribe(*pt, counter->waiters);
    }

    sync::irq_lock_guard guard(counter->lock);

    uint32_t mask = counter->count > 0 ? sync::POLL_IN : 0;
    mask |= counter->count < COUNTER_MAX ? sync::POLL_OUT : 0;

    return mask;
}

static ssize_t eventfd_read(resource::resource_object* obj, void* kdst, size_t count, uint32_t flags) {
    if (!obj || !obj->impl || !kdst || count < COUNT_SIZE) {
        return resource::ERR_INVAL;
    }

    auto* counter = static_cast<eventfd_counter*>(obj->impl);
    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    ssize_t result = 0;

    RUN_ELEVATED({
        result = take_count(counter, kdst, nonblock);
    });

    return result;
}

static ssize_t eventfd_write(resource::resource_object* obj, const void* ksrc, size_t count, uint32_t flags) {
    if (!obj || !obj->impl || !ksrc || count < COUNT_SIZE) {
        return resource::ERR_INVAL;
    }

    auto* counter = static_cast<eventfd_counter*>(obj->impl);
    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    ssize_t result = 0;

    RUN_ELEVATED({
        result = add_count(counter, ksrc, nonblock);
    });

    return result;
}

static uint32_t eventfd_poll(resource::resource_object* obj, sync::poll_table* pt) {
    if (!obj || !obj->impl) {
        return sync::POLL_NVAL;
    }

    auto* counter = static_cast<eventfd_counter*>(obj->impl);
    uint32_t mask = 0;

    RUN_ELEVATED({
        mask = poll_counter(counter, pt);
    });

    return mask;
}

// Waiters and pollers hold references, so the queue is empty once the last reference closes the object
static void eventfd_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    heap::ufree_delete(static_cast<eventfd_counter*>(obj->impl));
    obj->impl = nullptr;
}

static const resource::resource_ops g_eventfd_ops = {
    .read = eventfd_read,
    .write = eventfd_write,
    .close = eventfd_close,
    .poll = eventfd_poll,
};

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create(uint64_t initial_value, bool semaphore, resource::resource_object** out) {
    if (!out || initial_value > COUNTER_MAX) {
        return resource::ERR_INVAL;
    }

    auto* counter = heap::ualloc_new<eventfd_counter>();
    if (!counter) {
        return resource::ERR_NOMEM;
    }

    counter->lock = sync::SPINLOCK_INIT;
    counter->waiters.init();
    counter->count = initial_value;
    counter->semaphore = semaphore;

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        heap::ufree_delete(counter);
        return resource::ERR_NOMEM;
    }

    obj->type = resource::resource_type::EVENTFD;
    obj->ops = &g_eventfd_ops;
    obj->impl = counter;
    *out = obj;

    return resource::OK;
}

} // namespace eventfd
