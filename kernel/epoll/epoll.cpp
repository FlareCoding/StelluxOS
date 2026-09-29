#include "epoll/epoll.h"
#include "resource/resource.h"
#include "common/list.h"
#include "mm/heap.h"
#include "rc/ref_counted.h"
#include "rc/strong_ref.h"
#include "sched/sched.h"
#include "signals/signal.h"
#include "clock/clock.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"

namespace epoll {

// Reported whether or not the interest asked for them
constexpr uint32_t ALWAYS_EVENTS = sync::POLL_ERR | sync::POLL_HUP;

namespace {

struct epoll_instance;
struct interest;

// Lets a wake on one of an interest's subscriptions find the interest
struct interest_table : sync::poll_table {
    interest* owner;
};

// A wake before an interest goes live is held until it does, so a refused add is never reported
enum class interest_state : uint8_t {
    ADDING,
    LIVE,
    ENDED,
};

struct interest : resource::resource_watch, rc::ref_counted<interest> {
    list::node                 epoll_link;
    list::node                 ready_link;
    epoll_instance*            owner;
    resource::resource_object* target;
    resource::handle_t         handle;
    uint32_t                   events;
    uint64_t                   data;
    interest_state             state;
    bool                       wake_pending;
    interest_table             table;
};

struct waiter {
    list::node       link;
    sync::poll_table table;
};

struct epoll_instance {
    list::head<interest, &interest::epoll_link> interests;
    sync::spinlock                              ready_lock;
    list::head<interest, &interest::ready_link> ready;
    list::head<waiter, &waiter::link>           waiters;
};

struct ready_candidate {
    interest*                                 entry;
    rc::strong_ref<resource::resource_object> target;
};

} // anonymous namespace

/**
 * The epoll behind `obj`, or null when `obj` is not an epoll.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static epoll_instance* instance_of(resource::resource_object* obj) {
    if (!obj || obj->type != resource::resource_type::EPOLL) {
        return nullptr;
    }

    return static_cast<epoll_instance*>(obj->impl);
}

// Looked up under the watch lock
static interest* find_interest(epoll_instance* instance, resource::handle_t handle,
                               resource::resource_object* target) {
    for (interest& entry : instance->interests) {
        if (entry.handle == handle && entry.target == target) {
            return &entry;
        }
    }

    return nullptr;
}

/**
 * Takes `entry` off its epoll and its target, under the watch lock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void unlink_interest(interest& entry) {
    entry.owner->interests.remove(&entry);
    entry.target->watches.remove(&entry);
}

static void release_interest(interest& entry) {
    if (entry.release()) {
        heap::ufree_delete(&entry);
    }
}

static bool queue_ready_locked(interest& entry) {
    if (entry.state == interest_state::ADDING) {
        entry.wake_pending = true;
        return false;
    }

    if (entry.state == interest_state::ENDED || entry.ready_link.is_linked()) {
        return false;
    }

    entry.owner->ready.push_back(&entry);

    return true;
}

/**
 * Runs under the ready lock, which a waiter takes before leaving, so its task is alive here.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE static rc::strong_ref<sched::task> take_waiter(epoll_instance* instance) {
    waiter* next = instance->waiters.pop_front();
    if (!next) {
        return {};
    }

    next->table.triggered.store_release(1);

    return sched::task_ref(next->table.task);
}

/**
 * Queues the interest behind a subscription whose queue woke. Runs under that queue's lock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static rc::strong_ref<sched::task> queue_woken_interest(sync::wait_observer& observer) {
    auto* table = static_cast<interest_table*>(static_cast<sync::poll_entry&>(observer).table);
    interest& entry = *table->owner;

    sync::irq_lock_guard guard(entry.owner->ready_lock);

    if (!queue_ready_locked(entry)) {
        return {};
    }

    return take_waiter(entry.owner);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void wake_waiter(epoll_instance* instance) {
    rc::strong_ref<sched::task> owed;
    {
        sync::irq_lock_guard guard(instance->ready_lock);
        if (!instance->ready.empty()) {
            owed = take_waiter(instance);
        }
    }

    if (owed) {
        sched::wake(owed.ptr());
    }
}

/**
 * Stops `entry` hearing its target and takes it off the ready list, so nothing reaches it once freed.
 * Unsubscribing first keeps a late wake from queuing it again.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void quiet_interest(interest& entry) {
    sync::poll_cleanup(entry.table);

    sync::irq_lock_guard guard(entry.owner->ready_lock);

    entry.state = interest_state::ENDED;
    if (entry.ready_link.is_linked()) {
        entry.owner->ready.remove(&entry);
    }
}

/**
 * Ends an interest whose target is being destroyed, under the watch lock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void detach_interest(resource::resource_watch& watch) {
    auto& entry = static_cast<interest&>(watch);

    unlink_interest(entry);
    quiet_interest(entry);
    release_interest(entry);
}

/**
 * Puts `entry` on its epoll and its target, unless the epoll already holds that interest or is full.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t link_interest(interest* entry, uint32_t mask) {
    sync::irq_lock_guard guard(resource::watch_lock());

    if (find_interest(entry->owner, entry->handle, entry->target)) {
        return ERR_EXIST;
    }

    if (entry->owner->interests.size() >= MAX_INTERESTS) {
        return ERR_NOSPC;
    }

    entry->owner->interests.push_back(entry);
    entry->target->watches.push_back(entry);

    sync::irq_lock_guard ready_guard(entry->owner->ready_lock);

    entry->state = interest_state::LIVE;
    if (entry->wake_pending || (mask & (entry->events | ALWAYS_EVENTS))) {
        queue_ready_locked(*entry);
    }

    return OK;
}

/**
 * Unlinks, quiets and returns the interest `instance` holds in `target` through `handle`, or null. The
 * watch lock is held throughout, so a target being destroyed cannot free its queues before the unsubscribe.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static interest* take_interest(epoll_instance* instance, resource::handle_t handle,
                                                 resource::resource_object* target) {
    sync::irq_lock_guard guard(resource::watch_lock());

    interest* entry = find_interest(instance, handle, target);
    if (entry) {
        unlink_interest(*entry);
        quiet_interest(*entry);
    }

    return entry;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t change_interest(epoll_instance* instance, resource::handle_t handle,
                                                 resource::resource_object* target, uint32_t events, uint64_t data) {
    sync::irq_lock_guard guard(resource::watch_lock());

    interest* entry = find_interest(instance, handle, target);
    if (!entry) {
        return ERR_NOENT;
    }

    // A wait reads both under the ready lock
    sync::irq_lock_guard ready_guard(instance->ready_lock);

    entry->events = events;
    entry->data = data;
    queue_ready_locked(*entry);

    return OK;
}

/**
 * Ends every interest of an epoll that is being destroyed.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void end_interests(epoll_instance* instance) {
    sync::irq_lock_guard guard(resource::watch_lock());

    while (interest* entry = instance->interests.front()) {
        unlink_interest(*entry);
        quiet_interest(*entry);
        release_interest(*entry);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint32_t queued_count(epoll_instance* instance) {
    sync::irq_lock_guard guard(instance->ready_lock);

    return static_cast<uint32_t>(instance->ready.size());
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint32_t take_ready(epoll_instance* instance, ready_candidate* batch, uint32_t max) {
    sync::irq_lock_guard guard(instance->ready_lock);

    uint32_t taken = 0;
    while (taken < max) {
        interest* entry = instance->ready.pop_front();
        if (!entry) {
            break;
        }

        // Null for a target being destroyed, whose detach ends the interest
        batch[taken].target = rc::strong_ref<resource::resource_object>::try_from_raw(entry->target);
        batch[taken].entry = entry;
        entry->add_ref();
        taken++;
    }

    return taken;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool report_interest(interest& entry, uint32_t mask, ready_event& event) {
    sync::irq_lock_guard guard(entry.owner->ready_lock);

    uint32_t events = mask & (entry.events | ALWAYS_EVENTS);
    if (entry.state != interest_state::LIVE || !events) {
        return false;
    }

    event = {.events = events, .data = entry.data};
    queue_ready_locked(entry);

    return true;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint32_t report_batch(ready_candidate* batch, uint32_t taken, ready_event* out) {
    uint32_t reported = 0;
    for (uint32_t i = 0; i < taken; i++) {
        resource::resource_object* target = batch[i].target.ptr();
        if (target && report_interest(*batch[i].entry, target->ops->poll(target, nullptr), out[reported])) {
            reported++;
        }

        batch[i].target.reset();
        release_interest(*batch[i].entry);
    }

    return reported;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t collect_events(epoll_instance* instance, ready_event* out, uint32_t max_events) {
    uint32_t limit = max_events < MAX_WAIT_EVENTS ? max_events : MAX_WAIT_EVENTS;
    uint32_t reported = 0;

    // Checks only what was queued at the start, so a target that keeps waking cannot hold the wait
    uint32_t unchecked = queued_count(instance);

    while (reported == 0 && unchecked > 0) {
        ready_candidate batch[MAX_WAIT_EVENTS];
        uint32_t taken = take_ready(instance, batch, limit < unchecked ? limit : unchecked);
        if (taken == 0) {
            break;
        }

        unchecked -= taken;
        reported = report_batch(batch, taken, out);
    }

    if (reported > 0) {
        wake_waiter(instance);
    }

    return static_cast<int32_t>(reported);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void block_until_ready(epoll_instance* instance, uint64_t deadline) {
    waiter self;
    self.table.init(sched::current());

    {
        sync::irq_lock_guard guard(instance->ready_lock);
        if (!instance->ready.empty()) {
            return;
        }

        instance->waiters.push_back(&self);
    }

    uint64_t now = clock::now_ns();
    if (!deadline || now < deadline) {
        (void)sync::poll_wait(self.table, deadline ? deadline - now : 0);
    }

    sync::irq_lock_guard guard(instance->ready_lock);

    if (self.link.is_linked()) {
        instance->waiters.remove(&self);
    }
}

static void epoll_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    auto* instance = static_cast<epoll_instance*>(obj->impl);

    RUN_ELEVATED({
        end_interests(instance);
    });

    heap::ufree_delete(instance);
    obj->impl = nullptr;
}

static const resource::resource_ops g_epoll_ops = {
    .close = epoll_close,
};

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create(resource::resource_object** out) {
    if (!out) {
        return ERR_INVAL;
    }

    auto* instance = heap::ualloc_new<epoll_instance>();
    if (!instance) {
        return ERR_NOMEM;
    }

    instance->interests.init();
    instance->ready_lock = sync::SPINLOCK_INIT;
    instance->ready.init();
    instance->waiters.init();

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        heap::ufree_delete(instance);
        return ERR_NOMEM;
    }

    obj->type = resource::resource_type::EPOLL;
    obj->ops = &g_epoll_ops;
    obj->impl = instance;
    *out = obj;

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t add_interest(resource::resource_object* ep, resource::handle_t handle,
                                       resource::resource_object* target, uint32_t events, uint64_t data) {
    epoll_instance* instance = instance_of(ep);
    if (!instance || !target || target->type == resource::resource_type::EPOLL || (events & ~INTEREST_EVENTS)) {
        return ERR_INVAL;
    }

    if (!target->ops || !target->ops->poll) {
        return ERR_PERM;
    }

    auto* entry = heap::ualloc_new<interest>();
    if (!entry) {
        return ERR_NOMEM;
    }

    entry->detach = detach_interest;
    entry->owner = instance;
    entry->target = target;
    entry->handle = handle;
    entry->events = events;
    entry->data = data;

    entry->state = interest_state::ADDING;
    entry->table.init(nullptr);
    entry->table.notify = queue_woken_interest;
    entry->table.owner = entry;

    // Subscribes before linking, since once linked another thread may remove the interest
    uint32_t mask = target->ops->poll(target, &entry->table);

    int32_t rc = entry->table.error.load_acquire() ? ERR_NOMEM : link_interest(entry, mask);
    if (rc != OK) {
        quiet_interest(*entry);
        release_interest(*entry);
        return rc;
    }

    wake_waiter(instance);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t modify_interest(resource::resource_object* ep, resource::handle_t handle,
                                          resource::resource_object* target, uint32_t events, uint64_t data) {
    epoll_instance* instance = instance_of(ep);
    if (!instance || (events & ~INTEREST_EVENTS)) {
        return ERR_INVAL;
    }

    int32_t rc = change_interest(instance, handle, target, events, data);
    if (rc == OK) {
        wake_waiter(instance);
    }

    return rc;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t remove_interest(resource::resource_object* ep, resource::handle_t handle,
                                          resource::resource_object* target) {
    epoll_instance* instance = instance_of(ep);
    if (!instance) {
        return ERR_INVAL;
    }

    interest* entry = take_interest(instance, handle, target);
    if (!entry) {
        return ERR_NOENT;
    }

    release_interest(*entry);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t interest_count(resource::resource_object* ep) {
    epoll_instance* instance = instance_of(ep);
    if (!instance) {
        return 0;
    }

    sync::irq_lock_guard guard(resource::watch_lock());

    return static_cast<uint32_t>(instance->interests.size());
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t ready_count(resource::resource_object* ep) {
    epoll_instance* instance = instance_of(ep);
    if (!instance) {
        return 0;
    }

    return queued_count(instance);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t wait(resource::resource_object* ep, ready_event* out, uint32_t max_events,
                               int64_t timeout_ns) {
    epoll_instance* instance = instance_of(ep);
    if (!instance || !out || max_events == 0) {
        return ERR_INVAL;
    }

    sched::task* self = sched::current();
    uint64_t deadline = timeout_ns > 0 ? clock::now_ns() + static_cast<uint64_t>(timeout_ns) : 0;

    while (true) {
        int32_t reported = collect_events(instance, out, max_events);
        if (reported > 0) {
            return reported;
        }

        // Checked before the timeout, since a signal may be what ended the wait
        if (timeout_ns != 0 && signals::interrupt_pending(self)) {
            return ERR_INTR;
        }

        if (timeout_ns == 0 || (deadline && clock::now_ns() >= deadline)) {
            return 0;
        }

        block_until_ready(instance, deadline);
    }
}

} // namespace epoll
