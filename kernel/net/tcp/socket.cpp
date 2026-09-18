#include "net/tcp/socket.h"
#include "net/tcp/conn.h"
#include "net/net.h"
#include "net/inet.h"
#include "net/interface.h"
#include "resource/socket_ops.h"
#include "sync/spinlock.h"
#include "sync/poll.h"
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

    heap::ufree_delete(sock);
}

__PRIVILEGED_CODE int32_t socket_bind(tcp_socket* sock, const ipv4::ipv4_addr& addr, uint16_t port) {
    if (!addr.is_unspecified() && !find_interface_by_address(addr)) {
        return ERR_NOT_LOCAL;
    }

    // The allocator consults the socket table itself, so it runs before the lock
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
    if (port != 0 && (socket_conflicts_locked(sock, local) || listener_conflicts(local))) {
        return ERR_IN_USE;
    }

    sock->local = local;
    sock->bound = true;

    return OK;
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

__PRIVILEGED_CODE static int32_t socket_getname(resource::resource_object* obj, void* kaddr,
                                                size_t* addrlen, bool peer) {
    if (peer) {
        return resource::ERR_NOTCONN;
    }

    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    sync::irq_lock_guard guard(sock->lock);

    if (inet::fill_sockaddr(kaddr, addrlen, sock->local.addr, sock->local.port) != OK) {
        return resource::ERR_INVAL;
    }

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

__PRIVILEGED_CODE static ssize_t socket_read(resource::resource_object*, void*, size_t, uint32_t) {
    return resource::ERR_NOTCONN;
}

__PRIVILEGED_CODE static ssize_t socket_write(resource::resource_object*, const void*, size_t, uint32_t) {
    return resource::ERR_NOTCONN;
}

__PRIVILEGED_CODE static uint32_t socket_poll(resource::resource_object* obj, sync::poll_table*) {
    if (!obj || !obj->impl) {
        return sync::POLL_NVAL;
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
    .getname = socket_getname,
    .setsockopt = socket_setsockopt,
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
