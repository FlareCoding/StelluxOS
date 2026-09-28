#include "epoll/epoll.h"
#include "resource/resource.h"
#include "common/list.h"
#include "mm/heap.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"

namespace epoll {

namespace {

struct epoll_instance;

struct interest : resource::resource_watch {
    list::node                 epoll_link;
    epoll_instance*            owner;
    resource::resource_object* target;
    resource::handle_t         handle;
    uint32_t                   events;
    uint64_t                   data;
};

struct epoll_instance {
    list::head<interest, &interest::epoll_link> interests;
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

/**
 * Ends an interest whose target is being destroyed, under the watch lock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void detach_interest(resource::resource_watch& watch) {
    auto& entry = static_cast<interest&>(watch);

    unlink_interest(entry);
    heap::ufree_delete(&entry);
}

/**
 * Puts `entry` on its epoll and its target, unless the epoll already holds that interest or is full.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t link_interest(interest* entry) {
    sync::irq_lock_guard guard(resource::watch_lock());

    if (find_interest(entry->owner, entry->handle, entry->target)) {
        return ERR_EXIST;
    }

    if (entry->owner->interests.size() >= MAX_INTERESTS) {
        return ERR_NOSPC;
    }

    entry->owner->interests.push_back(entry);
    entry->target->watches.push_back(entry);

    return OK;
}

/**
 * Unlinks and returns the interest `instance` holds in `target` through `handle`, or null.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static interest* take_interest(epoll_instance* instance, resource::handle_t handle,
                                                 resource::resource_object* target) {
    sync::irq_lock_guard guard(resource::watch_lock());

    interest* entry = find_interest(instance, handle, target);
    if (entry) {
        unlink_interest(*entry);
    }

    return entry;
}

/**
 * Ends every interest of an epoll that is being destroyed.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void end_interests(epoll_instance* instance) {
    sync::irq_lock_guard guard(resource::watch_lock());

    while (interest* entry = instance->interests.front()) {
        unlink_interest(*entry);
        heap::ufree_delete(entry);
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

    int32_t rc = link_interest(entry);
    if (rc != OK) {
        heap::ufree_delete(entry);
    }

    return rc;
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

    sync::irq_lock_guard guard(resource::watch_lock());

    interest* entry = find_interest(instance, handle, target);
    if (!entry) {
        return ERR_NOENT;
    }

    entry->events = events;
    entry->data = data;

    return OK;
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

    heap::ufree_delete(entry);

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

} // namespace epoll
