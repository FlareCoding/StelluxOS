#include "net/tcp/listen.h"
#include "net/net.h"
#include "sync/spinlock.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

namespace net {
namespace tcp {

static tcp_listener* g_listeners[MAX_LISTENERS];
static sync::spinlock g_listeners_lock = sync::SPINLOCK_INIT;

// Caller holds g_listeners_lock. Two listeners conflict when a SYN could match
// both, unless both allow sharing between a wildcard and a specific address
static bool conflicts_locked(const tcp_listener* a, const tcp_listener* b) {
    if (a->local_port != b->local_port) {
        return false;
    }

    if (a->iface && b->iface && a->iface != b->iface) {
        return false;
    }

    if (a->local_addr == b->local_addr) {
        return true;
    }

    bool one_is_wildcard = a->local_addr.is_unspecified() || b->local_addr.is_unspecified();
    return one_is_wildcard && !(a->reuseaddr && b->reuseaddr);
}

static void release_listener(tcp_listener* listener) {
    if (listener->release()) {
        tcp_listener::ref_destroy(listener);
    }
}

void tcp_listener::ref_destroy(tcp_listener* self) {
    heap::ufree_delete(self);
}

tcp_listener* alloc_listener(const ipv4::ipv4_addr& local_addr, uint16_t local_port) {
    tcp_listener* listener = heap::ualloc_new<tcp_listener>();
    if (!listener) {
        return nullptr;
    }

    listener->local_addr = local_addr;
    listener->local_port = local_port;
    listener->lock = sync::SPINLOCK_INIT;
    listener->accept_queue.init();
    listener->accept_wq.init();

    return listener;
}

int32_t listener_insert(tcp_listener* listener) {
    int32_t rc = ERR_FULL;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_listeners_lock);
        size_t free_slot = MAX_LISTENERS;

        for (size_t i = 0; i < MAX_LISTENERS; i++) {
            if (!g_listeners[i]) {
                free_slot = free_slot == MAX_LISTENERS ? i : free_slot;
            } else if (conflicts_locked(g_listeners[i], listener)) {
                rc = ERR_IN_USE;
                break;
            }
        }

        if (rc != ERR_IN_USE && free_slot != MAX_LISTENERS) {
            listener->add_ref();
            g_listeners[free_slot] = listener;
            rc = OK;
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
            if (!candidate || candidate->local_port != port) {
                continue;
            }

            if (candidate->iface && candidate->iface != iface) {
                continue;
            }

            if (candidate->local_addr == local_addr) {
                found = candidate;
                break;
            }

            if (candidate->local_addr.is_unspecified()) {
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
            if (g_listeners[i] && g_listeners[i]->local_port == port) {
                taken = true;
                break;
            }
        }
    });

    return taken;
}

} // namespace tcp
} // namespace net
