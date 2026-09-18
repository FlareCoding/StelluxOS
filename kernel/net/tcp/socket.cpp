#include "net/tcp/socket.h"
#include "net/tcp/conn.h"
#include "net/net.h"
#include "net/inet.h"
#include "net/interface.h"
#include "net/route.h"
#include "resource/socket_ops.h"
#include "sync/spinlock.h"
#include "sync/poll.h"
#include "sync/wait_queue.h"
#include "sched/sched.h"
#include "signals/signal.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"
#include "common/string.h"

namespace net {
namespace tcp {

static tcp_socket* g_sockets[MAX_SOCKETS];
static sync::spinlock g_sockets_lock = sync::SPINLOCK_INIT;

// Caller holds g_sockets_lock
static bool socket_conflicts_locked(const tcp_socket* self, const endpoint& local) {
    for (size_t i = 0; i < MAX_SOCKETS; i++) {
        tcp_socket* other = g_sockets[i];
        if (other && other != self && other->bound && endpoints_conflict(other->local, local)) {
            return true;
        }
    }

    return false;
}

__PRIVILEGED_CODE tcp_socket* socket_open() {
    tcp_socket* sock = heap::ualloc_new<tcp_socket>();
    if (!sock) {
        return nullptr;
    }

    sock->lock = sync::SPINLOCK_INIT;

    sync::irq_lock_guard guard(g_sockets_lock);
    for (size_t i = 0; i < MAX_SOCKETS; i++) {
        if (!g_sockets[i]) {
            g_sockets[i] = sock;
            return sock;
        }
    }

    heap::ufree_delete(sock);
    return nullptr;
}

__PRIVILEGED_CODE void socket_close(tcp_socket* sock) {
    {
        sync::irq_lock_guard guard(g_sockets_lock);
        for (size_t i = 0; i < MAX_SOCKETS; i++) {
            if (g_sockets[i] == sock) {
                g_sockets[i] = nullptr;
                break;
            }
        }
    }

    if (sock->listener) {
        listener_close(sock->listener.ptr());
    }

    if (sock->conn) {
        abort_connection(sock->conn.ptr());
    }

    heap::ufree_delete(sock);
}

__PRIVILEGED_CODE int32_t socket_listen(tcp_socket* sock, uint16_t backlog) {
    if (!sock->bound) {
        int32_t rc = socket_bind(sock, ipv4::UNSPECIFIED_ADDR, 0);
        if (rc != OK) {
            return rc;
        }
    }

    sync::irq_lock_guard guard(sock->lock);
    if (sock->conn) {
        return ERR_INVALID;
    }

    if (sock->listener) {
        sync::irq_lock_guard listener_guard(sock->listener->lock);
        sock->listener->backlog = backlog;
        return OK;
    }

    tcp_listener* listener = alloc_listener(sock->local);
    if (!listener) {
        return ERR_NO_MEMORY;
    }

    listener->backlog = backlog;
    int32_t rc = listener_insert(listener);
    if (rc != OK) {
        if (listener->release()) {
            tcp_listener::ref_destroy(listener);
        }

        return rc;
    }

    sock->listener = rc::strong_ref<tcp_listener>::adopt(listener);
    return OK;
}

__PRIVILEGED_CODE int32_t socket_bind(tcp_socket* sock, const ipv4::ipv4_addr& addr, uint16_t port) {
    if (!addr.is_unspecified() && !find_interface_by_address(addr)) {
        return ERR_NOT_LOCAL;
    }

    // The allocator consults the socket table itself, so it runs before the lock,
    // and the port it picked is checked under the lock like an explicit one
    for (size_t attempt = 0; attempt < EPHEMERAL_BIND_ATTEMPTS; attempt++) {
        endpoint local = {addr, port, nullptr, false};
        if (port == 0) {
            int32_t rc = take_ephemeral_port(&local.port);
            if (rc != OK) {
                return rc;
            }
        }

        sync::irq_lock_guard sockets_guard(g_sockets_lock);
        sync::irq_lock_guard guard(sock->lock);
        if (sock->bound) {
            return ERR_INVALID;
        }

        local.iface = sock->local.iface;
        local.reuseaddr = sock->local.reuseaddr;
        bool conflicts = socket_conflicts_locked(sock, local) || listener_conflicts(local);
        if (!conflicts) {
            sock->local = local;
            sock->bound = true;
            return OK;
        }

        if (port != 0) {
            return ERR_IN_USE;
        }
    }

    return ERR_FULL;
}

bool is_socket_port(uint16_t port) {
    bool taken = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_sockets_lock);
        for (size_t i = 0; i < MAX_SOCKETS; i++) {
            if (g_sockets[i] && g_sockets[i]->bound && g_sockets[i]->local.port == port) {
                taken = true;
                break;
            }
        }
    });

    return taken;
}

__PRIVILEGED_CODE static int32_t socket_bind(resource::resource_object* obj, const void* kaddr,
                                             size_t addrlen) {
    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);

    ipv4::ipv4_addr addr;
    uint16_t port = 0;
    if (inet::parse_sockaddr(kaddr, addrlen, &addr, &port) != OK) {
        return resource::ERR_INVAL;
    }

    int32_t rc = socket_bind(sock, addr, port);
    return inet::map_net_error(rc == ERR_FULL ? ERR_IN_USE : rc);
}

__PRIVILEGED_CODE static int32_t socket_listen(resource::resource_object* obj, int32_t backlog) {
    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);

    uint16_t depth = backlog < 1 ? 1 : (backlog > MAX_BACKLOG ? MAX_BACKLOG : static_cast<uint16_t>(backlog));
    int32_t rc = socket_listen(sock, depth);
    
    return inet::map_net_error(rc == ERR_FULL ? ERR_IN_USE : rc);
}

__PRIVILEGED_CODE static int32_t socket_accept(resource::resource_object* obj, resource::resource_object** new_obj,
                                               void* kaddr, size_t* addrlen, bool nonblock) {
    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    sched::task* task = sched::current();

    rc::strong_ref<tcp_listener> listener;
    {
        sync::irq_lock_guard guard(sock->lock);
        listener = sock->listener;
    }

    if (!listener) {
        return resource::ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(listener->lock);
    while (listener->accept_queue.empty() && !listener->closed && !nonblock &&
           !signals::interrupt_pending(task)) {
        irq = sync::wait(listener->accept_wq, listener->lock, irq);
    }

    tcp_conn* popped = listener->accept_queue.pop_front();
    if (popped) {
        listener->accept_count--;
    }

    bool closed = listener->closed;
    sync::spin_unlock_irqrestore(listener->lock, irq);

    if (!popped) {
        return closed ? resource::ERR_INVAL : (nonblock ? resource::ERR_AGAIN : resource::ERR_INTR);
    }

    rc::strong_ref<tcp_conn> conn = rc::strong_ref<tcp_conn>::adopt(popped);
    tcp_socket* child = socket_open();
    auto* child_obj = child ? heap::kalloc_new<resource::resource_object>() : nullptr;
    if (!child_obj) {
        abort_connection(conn.ptr());
        if (child) {
            socket_close(child);
        }

        return resource::ERR_NOMEM;
    }

    child->local = {conn->key.local_addr, conn->key.local_port, sock->local.iface, false};
    child->bound = true;
    child_obj->type = resource::resource_type::SOCKET;
    child_obj->ops = socket_ops();
    child_obj->impl = child;

    {
        sync::irq_lock_guard guard(conn->lock);
        conn->owner = child_obj;
    }

    if (kaddr && addrlen &&
        inet::fill_sockaddr(kaddr, addrlen, conn->key.remote_addr, conn->key.remote_port) != OK) {
        *addrlen = 0;
    }

    child->conn = conn;
    *new_obj = child_obj;

    return resource::OK;
}

__PRIVILEGED_CODE static int32_t socket_getname(resource::resource_object* obj, void* kaddr,
                                                size_t* addrlen, bool peer) {
    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    sync::irq_lock_guard guard(sock->lock);

    if (peer) {
        if (!sock->conn) {
            return resource::ERR_NOTCONN;
        }

        const tuple& key = sock->conn->key;
        return inet::fill_sockaddr(kaddr, addrlen, key.remote_addr, key.remote_port) == OK
                   ? resource::OK : resource::ERR_INVAL;
    }

    if (inet::fill_sockaddr(kaddr, addrlen, sock->local.addr, sock->local.port) != OK) {
        return resource::ERR_INVAL;
    }

    return resource::OK;
}

// Caller holds the socket lock. What a connect finds already there decides
// whether it may begin: EISCONN and EALREADY as POSIX names them, and a
// finished attempt reports its error once and makes room for the next.
static int32_t connect_precondition_locked(tcp_socket* sock) {
    if (sock->listener) {
        return resource::ERR_ISCONN;
    }

    if (!sock->conn) {
        return resource::OK;
    }

    tcp_state state;
    int32_t error;
    {
        sync::irq_lock_guard guard(sock->conn->lock);
        state = sock->conn->state;
        error = sock->conn->pending_error;
        sock->conn->pending_error = resource::OK;
    }

    if (state == tcp_state::syn_sent || state == tcp_state::syn_rcvd) {
        return resource::ERR_ALREADY;
    }

    if (state != tcp_state::closed) {
        return resource::ERR_ISCONN;
    }

    sock->conn.reset();
    return error != resource::OK ? error : resource::ERR_CONNREFUSED;
}

__PRIVILEGED_CODE static int32_t socket_connect(resource::resource_object* obj, const void* kaddr,
                                                size_t addrlen, bool nonblock) {
    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    sched::task* task = sched::current();

    ipv4::ipv4_addr dest;
    uint16_t port = 0;
    if (inet::parse_sockaddr(kaddr, addrlen, &dest, &port) != OK || port == 0 || dest.is_unspecified()) {
        return resource::ERR_INVAL;
    }

    interface* pinned = nullptr;
    {
        sync::irq_lock_guard guard(sock->lock);
        int32_t rc = connect_precondition_locked(sock);
        if (rc != resource::OK) {
            return rc;
        }

        pinned = sock->local.iface;
    }

    route::route_result route;
    int32_t rc = pinned ? route::lookup_on(pinned, dest, &route) : route::lookup(dest, &route);
    if (rc != OK) {
        return resource::ERR_NETUNREACH;
    }

    tuple key = {route.source, dest, 0, port};
    {
        sync::irq_lock_guard guard(sock->lock);
        if (sock->bound) {
            key.local_port = sock->local.port;
            if (!sock->local.addr.is_unspecified()) {
                key.local_addr = sock->local.addr;
            }
        }
    }

    if (key.local_port == 0 && take_ephemeral_port(&key.local_port) != OK) {
        return resource::ERR_ADDRNOTAVAIL;
    }

    rc::strong_ref<tcp_conn> conn;
    rc = open_active(key, route.iface, &conn);
    if (rc != OK) {
        return rc == ERR_FULL ? resource::ERR_NOBUFS : inet::map_net_error(rc);
    }

    {
        sync::irq_lock_guard guard(sock->lock);
        sock->local = {key.local_addr, key.local_port, pinned, sock->local.reuseaddr};
        sock->bound = true;
        sock->conn = conn;
    }

    {
        sync::irq_lock_guard guard(conn->lock);
        conn->owner = obj;
    }

    if (nonblock) {
        return resource::ERR_INPROGRESS;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(conn->lock);
    while ((conn->state == tcp_state::syn_sent || conn->state == tcp_state::syn_rcvd) &&
           !signals::interrupt_pending(task)) {
        irq = sync::wait(conn->conn_wq, conn->lock, irq);
    }

    tcp_state state = conn->state;
    int32_t error = conn->pending_error;
    conn->pending_error = resource::OK;
    sync::spin_unlock_irqrestore(conn->lock, irq);

    if (state == tcp_state::established) {
        return resource::OK;
    }

    if (state == tcp_state::closed) {
        return error != resource::OK ? error : resource::ERR_CONNREFUSED;
    }

    return resource::ERR_INTR;
}

__PRIVILEGED_CODE static int32_t socket_getsockopt(resource::resource_object* obj, int32_t level,
                                                   int32_t optname, void* optval, size_t* optlen) {
    if (level != inet::SOL_SOCKET || (optname != inet::SO_REUSEADDR && optname != inet::SO_ERROR)) {
        return resource::ERR_NOPROTOOPT;
    }

    if (*optlen < sizeof(int32_t)) {
        return resource::ERR_INVAL;
    }

    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    sync::irq_lock_guard guard(sock->lock);

    int32_t value = 0;
    if (optname == inet::SO_REUSEADDR) {
        value = sock->local.reuseaddr ? 1 : 0;
    } else if (sock->conn) {
        sync::irq_lock_guard conn_guard(sock->conn->lock);
        value = sock->conn->pending_error;
        sock->conn->pending_error = resource::OK;
    }

    string::memcpy(optval, &value, sizeof(value));
    *optlen = sizeof(value);

    return resource::OK;
}

__PRIVILEGED_CODE static int32_t socket_setsockopt(resource::resource_object* obj, int32_t level,
                                                   int32_t optname, const void* optval, size_t optlen) {
    if (level != inet::SOL_SOCKET || optname != inet::SO_REUSEADDR) {
        return resource::ERR_NOPROTOOPT;
    }

    if (optlen < sizeof(int32_t)) {
        return resource::ERR_INVAL;
    }

    int32_t enable = 0;
    string::memcpy(&enable, optval, sizeof(enable));

    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    sync::irq_lock_guard guard(sock->lock);
    sock->local.reuseaddr = enable != 0;

    return resource::OK;
}

__PRIVILEGED_CODE static ssize_t socket_read(resource::resource_object* obj, void*, size_t, uint32_t) {
    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    sync::irq_lock_guard guard(sock->lock);

    return sock->conn ? resource::ERR_UNSUP : resource::ERR_NOTCONN;
}

__PRIVILEGED_CODE static ssize_t socket_write(resource::resource_object* obj, const void*, size_t, uint32_t) {
    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    sync::irq_lock_guard guard(sock->lock);

    return sock->conn ? resource::ERR_UNSUP : resource::ERR_NOTCONN;
}

__PRIVILEGED_CODE static uint32_t socket_poll(resource::resource_object* obj, sync::poll_table* pt) {
    if (!obj || !obj->impl) {
        return sync::POLL_NVAL;
    }

    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    rc::strong_ref<tcp_listener> listener;
    rc::strong_ref<tcp_conn> conn;
    {
        sync::irq_lock_guard guard(sock->lock);
        listener = sock->listener;
        conn = sock->conn;
    }

    if (listener) {
        if (pt) {
            sync::poll_subscribe(*pt, listener->accept_wq);
        }

        sync::irq_lock_guard guard(listener->lock);
        return listener->accept_queue.empty() ? 0 : sync::POLL_IN;
    }

    if (conn) {
        if (pt) {
            sync::poll_subscribe(*pt, conn->conn_wq);
        }

        sync::irq_lock_guard guard(conn->lock);
        switch (conn->state) {
        case tcp_state::closed:
            return sync::POLL_HUP | (conn->pending_error != resource::OK ? sync::POLL_ERR : 0);
        case tcp_state::syn_sent:
        case tcp_state::syn_rcvd:
            return 0;
        default:
            return sync::POLL_OUT;
        }
    }

    return sync::POLL_HUP;
}

__PRIVILEGED_CODE static void socket_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    socket_close(static_cast<tcp_socket*>(obj->impl));
    obj->impl = nullptr;
}

static const resource::socket_ops g_tcp_socket_ops = {
    .bind = socket_bind,
    .listen = socket_listen,
    .accept = socket_accept,
    .connect = socket_connect,
    .getname = socket_getname,
    .setsockopt = socket_setsockopt,
    .getsockopt = socket_getsockopt,
};

static const resource::resource_ops g_socket_ops = {
    .read = socket_read,
    .write = socket_write,
    .close = socket_close,
    .ioctl = inet::socket_ioctl,
    .poll = socket_poll,
    .socket = &g_tcp_socket_ops,
};

const resource::resource_ops* socket_ops() {
    return &g_socket_ops;
}

} // namespace tcp
} // namespace net
