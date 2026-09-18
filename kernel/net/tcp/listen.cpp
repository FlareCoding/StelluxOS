#include "net/tcp/listen.h"
#include "net/net.h"
#include "sync/spinlock.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

namespace net {
namespace tcp {

static tcp_listener* g_listeners[MAX_LISTENERS];
static sync::spinlock g_listeners_lock = sync::SPINLOCK_INIT;

// Caller holds g_listeners_lock
static bool listener_conflicts_locked(const endpoint& local) {
    for (size_t i = 0; i < MAX_LISTENERS; i++) {
        if (g_listeners[i] && endpoints_conflict(g_listeners[i]->local, local)) {
            return true;
        }
    }

    return false;
}

static void release_listener(tcp_listener* listener) {
    if (listener->release()) {
        tcp_listener::ref_destroy(listener);
    }
}

void tcp_listener::ref_destroy(tcp_listener* self) {
    heap::ufree_delete(self);
}

bool endpoints_conflict(const endpoint& a, const endpoint& b) {
    if (a.port != b.port) {
        return false;
    }

    if (a.iface && b.iface && a.iface != b.iface) {
        return false;
    }

    if (a.addr == b.addr) {
        return true;
    }

    bool one_is_wildcard = a.addr.is_unspecified() || b.addr.is_unspecified();
    return one_is_wildcard && !(a.reuseaddr && b.reuseaddr);
}

bool listener_conflicts(const endpoint& local) {
    bool conflicts = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_listeners_lock);
        conflicts = listener_conflicts_locked(local);
    });

    return conflicts;
}

tcp_listener* alloc_listener(const endpoint& local) {
    tcp_listener* listener = heap::ualloc_new<tcp_listener>();
    if (!listener) {
        return nullptr;
    }

    listener->local = local;
    listener->lock = sync::SPINLOCK_INIT;
    listener->accept_queue.init();
    listener->accept_wq.init();

    return listener;
}

int32_t listener_insert(tcp_listener* listener) {
    int32_t rc = ERR_FULL;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_listeners_lock);
        if (listener_conflicts_locked(listener->local)) {
            rc = ERR_IN_USE;
        } else {
            for (size_t i = 0; i < MAX_LISTENERS; i++) {
                if (!g_listeners[i]) {
                    listener->add_ref();
                    g_listeners[i] = listener;
                    rc = OK;
                    break;
                }
            }
        }
    });

    return rc;
}

int32_t listener_remove(tcp_listener* listener) {
    bool removed = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_listeners_lock);
        for (size_t i = 0; i < MAX_LISTENERS; i++) {
            if (g_listeners[i] == listener) {
                g_listeners[i] = nullptr;
                removed = true;
                break;
            }
        }
    });

    if (!removed) {
        return ERR_NOT_FOUND;
    }

    release_listener(listener);
    return OK;
}

rc::strong_ref<tcp_listener> listener_lookup(const ipv4::ipv4_addr& local_addr, uint16_t port,
                                             interface* iface) {
    tcp_listener* found = nullptr;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_listeners_lock);
        for (size_t i = 0; i < MAX_LISTENERS; i++) {
            tcp_listener* candidate = g_listeners[i];
            if (!candidate || candidate->local.port != port) {
                continue;
            }

            if (candidate->local.iface && candidate->local.iface != iface) {
                continue;
            }

            if (candidate->local.addr == local_addr) {
                found = candidate;
                break;
            }

            if (candidate->local.addr.is_unspecified()) {
                found = candidate;
            }
        }

        if (found) {
            found->add_ref();
        }
    });

    return rc::strong_ref<tcp_listener>::adopt(found);
}

bool is_listener_port(uint16_t port) {
    bool taken = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_listeners_lock);
        for (size_t i = 0; i < MAX_LISTENERS; i++) {
            if (g_listeners[i] && g_listeners[i]->local.port == port) {
                taken = true;
                break;
            }
        }
    });

    return taken;
}

} // namespace tcp
} // namespace net
