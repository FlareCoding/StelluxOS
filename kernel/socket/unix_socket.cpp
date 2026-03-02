#include "socket/unix_socket.h"
#include "mm/heap.h"
#include "sync/spinlock.h"
#include "fs/fstypes.h"

namespace socket {

__PRIVILEGED_CODE void unix_channel::ref_destroy(unix_channel* self) {
    if (!self) {
        return;
    }
    ring_buffer_destroy(self->buf_a_to_b);
    ring_buffer_destroy(self->buf_b_to_a);
    heap::kfree_delete(self);
}

__PRIVILEGED_CODE static ssize_t socket_read(
    resource::resource_object* obj, void* kdst, size_t count, uint32_t flags
) {
    if (!obj || !obj->impl || !kdst) {
        return resource::ERR_INVAL;
    }
    auto* sock = static_cast<unix_socket*>(obj->impl);
    if (sock->state != SOCK_STATE_CONNECTED) {
        return resource::ERR_NOTCONN;
    }
    ring_buffer* rb = sock->is_side_a
        ? sock->channel->buf_b_to_a
        : sock->channel->buf_a_to_b;
    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    return ring_buffer_read(rb, static_cast<uint8_t*>(kdst), count, nonblock);
}

__PRIVILEGED_CODE static ssize_t socket_write(
    resource::resource_object* obj, const void* ksrc, size_t count, uint32_t flags
) {
    if (!obj || !obj->impl || !ksrc) {
        return resource::ERR_INVAL;
    }
    auto* sock = static_cast<unix_socket*>(obj->impl);
    if (sock->state != SOCK_STATE_CONNECTED) {
        return resource::ERR_NOTCONN;
    }
    ring_buffer* rb = sock->is_side_a
        ? sock->channel->buf_a_to_b
        : sock->channel->buf_b_to_a;
    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    return ring_buffer_write(rb, static_cast<const uint8_t*>(ksrc), count, nonblock);
}

__PRIVILEGED_CODE static void socket_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }
    auto* sock = static_cast<unix_socket*>(obj->impl);

    switch (sock->state) {
    case SOCK_STATE_UNBOUND:
        break;

    case SOCK_STATE_BOUND:
        // bound_node strong_ref drops via ~unix_socket
        break;

    case SOCK_STATE_LISTENING: {
        if (sock->listener) {
            sync::irq_state irq = sync::spin_lock_irqsave(sock->listener->lock);
            sock->listener->closed = true;
            while (pending_conn* pc = sock->listener->accept_queue.pop_front()) {
                sock->listener->pending_count--;
                resource::resource_release(pc->server_obj);
                heap::kfree(pc);
            }
            sync::spin_unlock_irqrestore(sock->listener->lock, irq);
            sync::wake_all(sock->listener->accept_wq);
        }
        // bound_node and listener strong_refs drop via ~unix_socket
        break;
    }

    case SOCK_STATE_CONNECTED: {
        rc::strong_ref<unix_channel> chan = sock->channel;
        if (chan) {
            if (sock->is_side_a) {
                ring_buffer_close_write(chan->buf_a_to_b);
                ring_buffer_close_read(chan->buf_b_to_a);
            } else {
                ring_buffer_close_write(chan->buf_b_to_a);
                ring_buffer_close_read(chan->buf_a_to_b);
            }
        }
        break;
    }
    }

    heap::kfree_delete(sock);
    obj->impl = nullptr;
}

static const resource::resource_ops g_socket_ops = {
    socket_read,
    socket_write,
    socket_close,
};

const resource::resource_ops* get_socket_ops() {
    return &g_socket_ops;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_unbound_socket(
    resource::resource_object** out
) {
    if (!out) {
        return resource::ERR_INVAL;
    }

    auto* sock = heap::kalloc_new<unix_socket>();
    if (!sock) {
        return resource::ERR_NOMEM;
    }
    sock->state = SOCK_STATE_UNBOUND;
    sock->lock = sync::SPINLOCK_INIT;
    sock->is_side_a = false;

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        heap::kfree_delete(sock);
        return resource::ERR_NOMEM;
    }
    obj->type = resource::resource_type::SOCKET;
    obj->ops = &g_socket_ops;
    obj->impl = sock;

    *out = obj;
    return resource::OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_socket_pair(
    resource::resource_object** out_a,
    resource::resource_object** out_b
) {
    if (!out_a || !out_b) {
        return resource::ERR_INVAL;
    }

    auto chan = rc::make_kref<unix_channel>();
    if (!chan) {
        return resource::ERR_NOMEM;
    }
    chan->buf_a_to_b = nullptr;
    chan->buf_b_to_a = nullptr;

    chan->buf_a_to_b = ring_buffer_create(DEFAULT_CAPACITY);
    if (!chan->buf_a_to_b) {
        return resource::ERR_NOMEM;
    }

    chan->buf_b_to_a = ring_buffer_create(DEFAULT_CAPACITY);
    if (!chan->buf_b_to_a) {
        return resource::ERR_NOMEM;
    }

    auto* sock_a = heap::kalloc_new<unix_socket>();
    if (!sock_a) {
        return resource::ERR_NOMEM;
    }
    sock_a->state = SOCK_STATE_CONNECTED;
    sock_a->lock = sync::SPINLOCK_INIT;
    sock_a->is_side_a = true;
    sock_a->channel = chan;

    auto* sock_b = heap::kalloc_new<unix_socket>();
    if (!sock_b) {
        heap::kfree_delete(sock_a);
        return resource::ERR_NOMEM;
    }
    sock_b->state = SOCK_STATE_CONNECTED;
    sock_b->lock = sync::SPINLOCK_INIT;
    sock_b->is_side_a = false;
    sock_b->channel = static_cast<rc::strong_ref<unix_channel>&&>(chan);

    auto* obj_a = heap::kalloc_new<resource::resource_object>();
    if (!obj_a) {
        heap::kfree_delete(sock_b);
        heap::kfree_delete(sock_a);
        return resource::ERR_NOMEM;
    }
    obj_a->type = resource::resource_type::SOCKET;
    obj_a->ops = &g_socket_ops;
    obj_a->impl = sock_a;

    auto* obj_b = heap::kalloc_new<resource::resource_object>();
    if (!obj_b) {
        heap::kfree_delete(obj_a);
        heap::kfree_delete(sock_b);
        heap::kfree_delete(sock_a);
        return resource::ERR_NOMEM;
    }
    obj_b->type = resource::resource_type::SOCKET;
    obj_b->ops = &g_socket_ops;
    obj_b->impl = sock_b;

    *out_a = obj_a;
    *out_b = obj_b;
    return resource::OK;
}

} // namespace socket
