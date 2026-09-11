#include "net/udp_socket.h"
#include "net/udp.h"
#include "net/net.h"
#include "net/inet.h"
#include "net/eth.h"
#include "net/interface.h"
#include "resource/socket_ops.h"
#include "mm/heap.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "signals/signal.h"
#include "sync/spinlock.h"
#include "sync/poll.h"
#include "dynpriv/dynpriv.h"
#include "common/string.h"

namespace net {
namespace udp {

static sync::spinlock g_sockets_lock = sync::SPINLOCK_INIT;
static udp_socket* g_sockets[MAX_SOCKETS];
static uint16_t g_next_ephemeral_port = EPHEMERAL_PORT_MIN;

__PRIVILEGED_CODE udp_socket* socket_open() {
    udp_socket* sock = heap::ualloc_new<udp_socket>();
    if (!sock) {
        return nullptr;
    }

    sock->local_addr = ipv4::UNSPECIFIED_ADDR;
    sock->local_port = 0;
    sock->iface = nullptr;
    sock->broadcast_allowed = false;
    sock->lock = sync::SPINLOCK_INIT;
    sock->rx_queue.init();
    sock->rx_wq.init();

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

__PRIVILEGED_CODE void socket_close(udp_socket* sock) {
    if (!sock) {
        return;
    }

    {
        sync::irq_lock_guard guard(g_sockets_lock);

        for (size_t i = 0; i < MAX_SOCKETS; i++) {
            if (g_sockets[i] == sock) {
                g_sockets[i] = nullptr;
                break;
            }
        }
    }

    // At this point no more datagrams can arrive and the queue is ours alone
    while (packet* pkt = sock->rx_queue.pop_front()) {
        packet::free(pkt);
    }

    heap::ufree_delete(sock);
}

// Caller holds g_sockets_lock. Only one socket may match a datagram, so the
// unspecified address conflicts with every address on the same port, while
// sockets pinned to different interfaces can never both match one.
static bool is_port_taken_locked(const interface* iface, const ipv4::ipv4_addr& addr, uint16_t port) {
    for (size_t i = 0; i < MAX_SOCKETS; i++) {
        udp_socket* other = g_sockets[i];
        if (!other || other->local_port != port) {
            continue;
        }

        if (iface && other->iface && other->iface != iface) {
            continue;
        }

        if (addr.is_unspecified() || other->local_addr.is_unspecified() || other->local_addr == addr) {
            return true;
        }
    }

    return false;
}

// Caller holds g_sockets_lock. One lap of the range from the last port handed
// out, so recently released ports are not reused straight away. Zero when full.
static uint16_t take_ephemeral_port_locked(const ipv4::ipv4_addr& addr) {
    constexpr uint32_t RANGE = EPHEMERAL_PORT_MAX - EPHEMERAL_PORT_MIN + 1;

    for (uint32_t tried = 0; tried < RANGE; tried++) {
        uint16_t port = g_next_ephemeral_port;
        g_next_ephemeral_port = port == EPHEMERAL_PORT_MAX ? EPHEMERAL_PORT_MIN : port + 1;

        if (!is_port_taken_locked(nullptr, addr, port)) {
            return port;
        }
    }

    return 0;
}

__PRIVILEGED_CODE int32_t socket_bind(udp_socket* sock, const ipv4::ipv4_addr& addr, uint16_t port) {
    if (!sock) {
        return ERR_INVALID;
    }

    sync::irq_lock_guard guard(g_sockets_lock);
    if (sock->local_port != 0) {
        return ERR_INVALID;
    }

    if (port == 0) {
        port = take_ephemeral_port_locked(addr);

        if (port == 0) {
            return ERR_FULL;
        }
    } else if (is_port_taken_locked(sock->iface, addr, port)) {
        return ERR_IN_USE;
    }

    sock->local_addr = addr;
    sock->local_port = port;

    return OK;
}

// Caller holds g_sockets_lock. A socket bound to the datagram's own address wins
// over one bound to every address, and one bound to an interface sees only its traffic.
static udp_socket* find_bound_socket_locked(interface* iface, const ipv4::ipv4_addr& addr, uint16_t port) {
    udp_socket* any_addr = nullptr;

    for (size_t i = 0; i < MAX_SOCKETS; i++) {
        udp_socket* sock = g_sockets[i];

        // An unbound socket has no port and matches nothing, port zero included
        if (!sock || sock->local_port == 0 || sock->local_port != port) {
            continue;
        }

        if (sock->iface && sock->iface != iface) {
            continue;
        }

        if (sock->local_addr == addr) {
            return sock;
        }

        if (sock->local_addr.is_unspecified()) {
            any_addr = sock;
        }
    }

    return any_addr;
}

int32_t socket_deliver(packet* pkt) {
    if (!pkt) {
        return ERR_INVALID;
    }

    const ipv4::ipv4_header* ip = reinterpret_cast<const ipv4::ipv4_header*>(pkt->network_header());
    const udp_header* hdr = reinterpret_cast<const udp_header*>(pkt->data());
    uint16_t port = ntohs(hdr->dst_port);

    int32_t rc = OK;

    RUN_ELEVATED({
        sync::irq_lock_guard table_guard(g_sockets_lock);

        udp_socket* sock = find_bound_socket_locked(pkt->iface(), ip->dst, port);
        if (!sock) {
            rc = ERR_NOT_FOUND;
        } else {
            (void)pkt->pull(HEADER_LEN);

            packet* evicted = nullptr;
            {
                sync::irq_lock_guard sock_guard(sock->lock);

                if (sock->rx_queue.size() >= SOCKET_QUEUE_DEPTH) {
                    evicted = sock->rx_queue.pop_front();
                }

                sock->rx_queue.push_back(pkt);
            }

            if (evicted) {
                evicted->iface()->record_packet_dropped();
                packet::free(evicted);
            }

            sync::wake_one(sock->rx_wq);
        }
    });

    return rc;
}

__PRIVILEGED_CODE static int32_t socket_bind(resource::resource_object* obj, const void* kaddr,
                                             size_t addrlen) {
    udp_socket* sock = static_cast<udp_socket*>(obj->impl);

    ipv4::ipv4_addr addr;
    uint16_t port = 0;
    if (inet::parse_sockaddr(kaddr, addrlen, &addr, &port) != OK) {
        return resource::ERR_INVAL;
    }

    // An exhausted ephemeral range is reported like a taken port
    int32_t rc = socket_bind(sock, addr, port);
    return inet::map_net_error(rc == ERR_FULL ? ERR_IN_USE : rc);
}

// Sends the caller's payload to `kaddr`. A socket without a port takes an ephemeral
// one first so replies can find it. Losing a race to bind is fine, the port is set either way.
__PRIVILEGED_CODE static ssize_t socket_sendto(resource::resource_object* obj, const void* ksrc,
                                               size_t count, uint32_t, const void* kaddr,
                                               size_t addrlen) {
    udp_socket* sock = static_cast<udp_socket*>(obj->impl);

    ipv4::ipv4_addr dest;
    uint16_t dest_port = 0;
    if (inet::parse_sockaddr(kaddr, addrlen, &dest, &dest_port) != OK || dest_port == 0) {
        return resource::ERR_INVAL;
    }

    if (sock->local_port == 0 && socket_bind(sock, ipv4::UNSPECIFIED_ADDR, 0) == ERR_FULL) {
        return resource::ERR_AGAIN;
    }

    packet* pkt = packet::alloc();
    if (!pkt) {
        return resource::ERR_NOMEM;
    }

    uint8_t* body = nullptr;
    if (pkt->reserve(eth::HEADER_LEN + ipv4::HEADER_LEN + HEADER_LEN)) {
        body = pkt->put(count);
    }

    if (!body) {
        packet::free(pkt);
        return resource::ERR_MSGSIZE;
    }

    string::memcpy(body, ksrc, count);

    int32_t rc = output(pkt, sock->iface, dest, sock->local_port, dest_port, sock->broadcast_allowed);
    if (rc != OK) {
        return inet::map_net_error(rc);
    }

    return static_cast<ssize_t>(count);
}

// Hands the oldest waiting datagram to the caller with its source endpoint. Without
// MSG_DONTWAIT the caller sleeps until one arrives or a signal interrupts.
__PRIVILEGED_CODE static ssize_t socket_recvfrom(resource::resource_object* obj, void* kdst, size_t count,
                                                 uint32_t flags, void* kaddr, size_t* addrlen) {
    udp_socket* sock = static_cast<udp_socket*>(obj->impl);
    bool nonblock = (flags & inet::MSG_DONTWAIT) != 0;

    sched::task* task = sched::current();
    if (!task) {
        return resource::ERR_IO;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);
    while (sock->rx_queue.empty() && !nonblock && !signals::interrupt_pending(task)) {
        irq = sync::wait(sock->rx_wq, sock->lock, irq);
    }

    packet* pkt = sock->rx_queue.pop_front();
    sync::spin_unlock_irqrestore(sock->lock, irq);

    if (!pkt) {
        return nonblock ? resource::ERR_AGAIN : resource::ERR_INTR;
    }

    // A datagram longer than the buffer loses its tail, it is never split across reads
    size_t copied = pkt->length() < count ? pkt->length() : count;
    string::memcpy(kdst, pkt->data(), copied);

    if (kaddr && addrlen) {
        const ipv4::ipv4_header* ip = reinterpret_cast<const ipv4::ipv4_header*>(pkt->network_header());
        const udp_header* hdr = reinterpret_cast<const udp_header*>(pkt->transport_header());
        if (inet::fill_sockaddr(kaddr, addrlen, ip->src, ntohs(hdr->src_port)) != OK) {
            *addrlen = 0;
        }
    }

    packet::free(pkt);
    return static_cast<ssize_t>(copied);
}

// SO_BROADCAST opts into broadcast destinations, SO_BINDTODEVICE names the
// interface the socket lives on and an empty name frees it
__PRIVILEGED_CODE static int32_t socket_setsockopt(resource::resource_object* obj, int32_t level,
                                                   int32_t optname, const void* optval, size_t optlen) {
    udp_socket* sock = static_cast<udp_socket*>(obj->impl);

    if (level == inet::SOL_SOCKET && optname == inet::SO_BROADCAST) {
        if (optlen < sizeof(int32_t)) {
            return resource::ERR_INVAL;
        }

        int32_t enable = 0;
        string::memcpy(&enable, optval, sizeof(enable));
        sock->broadcast_allowed = enable != 0;
        return resource::OK;
    }

    if (level != inet::SOL_SOCKET || optname != inet::SO_BINDTODEVICE) {
        return resource::ERR_NOPROTOOPT;
    }

    char name[IFACE_NAME_MAX];
    size_t len = optlen < IFACE_NAME_MAX - 1 ? optlen : IFACE_NAME_MAX - 1;
    string::memcpy(name, optval, len);
    name[len] = '\0';

    interface* iface = nullptr;
    if (name[0] != '\0') {
        iface = find_interface_by_name(name);
        if (!iface) {
            return resource::ERR_NOENT;
        }
    }

    sync::irq_lock_guard guard(g_sockets_lock);
    sock->iface = iface;

    return resource::OK;
}

// Reports the bound endpoint, all zero before bind. Nothing connects a socket yet, so no peer.
static int32_t socket_getname(resource::resource_object* obj, void* kaddr, size_t* addrlen, bool peer) {
    if (peer) {
        return resource::ERR_NOTCONN;
    }

    udp_socket* sock = static_cast<udp_socket*>(obj->impl);
    if (inet::fill_sockaddr(kaddr, addrlen, sock->local_addr, sock->local_port) != OK) {
        return resource::ERR_INVAL;
    }

    return resource::OK;
}

__PRIVILEGED_CODE static void socket_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    socket_close(static_cast<udp_socket*>(obj->impl));
    obj->impl = nullptr;
}

// A plain read is a receive that does not ask where the datagram came from. The
// descriptor's nonblocking flag maps onto the receive flag with the same meaning.
__PRIVILEGED_CODE static ssize_t socket_read(resource::resource_object* obj, void* kdst, size_t count,
                                             uint32_t flags) {
    uint32_t msg_flags = (flags & fs::O_NONBLOCK) ? inet::MSG_DONTWAIT : 0;
    return socket_recvfrom(obj, kdst, count, msg_flags, nullptr, nullptr);
}

// Readable while a datagram waits. Sending never blocks, a datagram is queued on
// the interface or refused at once, so the socket is always writable.
__PRIVILEGED_CODE static uint32_t socket_poll(resource::resource_object* obj, sync::poll_table* pt) {
    if (!obj || !obj->impl) {
        return sync::POLL_NVAL;
    }

    udp_socket* sock = static_cast<udp_socket*>(obj->impl);
    if (pt) {
        sync::poll_subscribe(*pt, sock->rx_wq);
    }

    sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);
    uint32_t mask = sock->rx_queue.empty() ? 0 : sync::POLL_IN;
    sync::spin_unlock_irqrestore(sock->lock, irq);

    return mask | sync::POLL_OUT;
}

static const resource::socket_ops g_udp_socket_ops = {
    .bind = socket_bind,
    .sendto = socket_sendto,
    .recvfrom = socket_recvfrom,
    .getname = socket_getname,
    .setsockopt = socket_setsockopt,
};

static const resource::resource_ops g_socket_ops = {
    .read = socket_read,
    .close = socket_close,
    .ioctl = inet::socket_ioctl,
    .poll = socket_poll,
    .socket = &g_udp_socket_ops,
};

const resource::resource_ops* socket_ops() {
    return &g_socket_ops;
}

} // namespace udp
} // namespace net
