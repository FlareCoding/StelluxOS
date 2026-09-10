#include "net/icmp_socket.h"
#include "net/icmp.h"
#include "net/inet.h"
#include "net/eth.h"
#include "net/interface.h"
#include "resource/socket_ops.h"
#include "mm/heap.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "signals/signal.h"
#include "sync/poll.h"
#include "dynpriv/dynpriv.h"
#include "common/string.h"

namespace net {
namespace icmp {

// The stack and the resource layer speak different result codes, this is the
// one place they are translated
static ssize_t map_net_error(int32_t rc) {
    switch (rc) {
    case OK:            return 0;
    case ERR_INVALID:   return resource::ERR_INVAL;
    case ERR_NO_MEMORY: return resource::ERR_NOMEM;
    case ERR_TOO_LARGE: return resource::ERR_MSGSIZE;
    case ERR_NO_ROUTE:  return resource::ERR_HOSTUNREACH;
    case ERR_DOWN:      return resource::ERR_HOSTUNREACH;
    default:            return resource::ERR_IO;
    }
}

static sync::spinlock g_sockets_lock = sync::SPINLOCK_INIT;
static icmp_socket* g_sockets[MAX_SOCKETS];
static uint16_t g_next_id = 1;

// Caller holds g_sockets_lock
static icmp_socket* find_locked(uint16_t id) {
    for (size_t i = 0; i < MAX_SOCKETS; i++) {
        if (g_sockets[i] && g_sockets[i]->id == id) {
            return g_sockets[i];
        }
    }

    return nullptr;
}

// Caller holds g_sockets_lock. Identifiers wrap at 16 bits and skip 0 and any in use.
static uint16_t take_free_id() {
    uint16_t id = g_next_id;
    while (id == 0 || find_locked(id)) {
        id++;
    }

    g_next_id = static_cast<uint16_t>(id + 1);
    return id;
}

__PRIVILEGED_CODE icmp_socket* socket_open() {
    icmp_socket* sock = heap::ualloc_new<icmp_socket>();
    if (!sock) {
        return nullptr;
    }

    sock->lock = sync::SPINLOCK_INIT;
    sock->rx_queue.init();
    sock->rx_wq.init();

    sync::irq_lock_guard guard(g_sockets_lock);
    for (size_t i = 0; i < MAX_SOCKETS; i++) {
        if (!g_sockets[i]) {
            sock->id = take_free_id();
            g_sockets[i] = sock;
            return sock;
        }
    }

    heap::ufree_delete(sock);
    return nullptr;
}

__PRIVILEGED_CODE void socket_close(icmp_socket* sock) {
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

    // Unregistered, so no more replies can arrive and the queue is ours alone
    while (packet* pkt = sock->rx_queue.pop_front()) {
        packet::free(pkt);
    }

    heap::ufree_delete(sock);
}

void socket_deliver(packet* pkt) {
    if (!pkt) {
        return;
    }

    const icmp_header* hdr = reinterpret_cast<const icmp_header*>(pkt->data());
    uint16_t id = ntohs(hdr->echo.id);

    // Waking the reader needs privilege, and the socket lock is taken inside
    // the table lock so the socket cannot be closed between lookup and enqueue
    RUN_ELEVATED({
        sync::irq_lock_guard table_guard(g_sockets_lock);

        icmp_socket* sock = find_locked(id);
        if (!sock) {
            pkt->iface()->record_packet_dropped();
            packet::free(pkt);
        } else {
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
}

// Sends the caller's echo request to `kaddr`. The identifier is replaced with
// the socket's own so the reply can be matched back to it.
static ssize_t socket_sendto(resource::resource_object* obj, const void* ksrc, size_t count,
                             uint32_t, const void* kaddr, size_t addrlen) {
    icmp_socket* sock = static_cast<icmp_socket*>(obj->impl);

    ipv4::ipv4_addr dest;
    uint16_t port = 0;

    if (inet::parse_sockaddr(kaddr, addrlen, &dest, &port) != OK) {
        return resource::ERR_INVAL;
    }

    // Only echo requests may be sent through a ping socket
    if (count < HEADER_LEN) {
        return resource::ERR_INVAL;
    }

    const icmp_header* user_hdr = static_cast<const icmp_header*>(ksrc);
    if (user_hdr->type != TYPE_ECHO_REQUEST || user_hdr->code != 0) {
        return resource::ERR_INVAL;
    }

    packet* pkt = packet::alloc();
    if (!pkt) {
        return resource::ERR_NOMEM;
    }

    uint8_t* body = nullptr;
    if (pkt->reserve(eth::HEADER_LEN + ipv4::HEADER_LEN)) {
        body = pkt->put(count);
    }

    if (!body) {
        packet::free(pkt);
        return resource::ERR_MSGSIZE;
    }

    string::memcpy(body, ksrc, count);
    reinterpret_cast<icmp_header*>(body)->echo.id = htons(sock->id);

    int32_t rc = output(pkt, dest);
    if (rc != OK) {
        return map_net_error(rc);
    }

    return static_cast<ssize_t>(count);
}

// Hands the oldest waiting reply to the caller with its source address. Without
// MSG_DONTWAIT the caller sleeps until a reply arrives or a signal interrupts.
__PRIVILEGED_CODE static ssize_t socket_recvfrom(resource::resource_object* obj, void* kdst, size_t count,
                                                 uint32_t flags, void* kaddr, size_t* addrlen) {
    icmp_socket* sock = static_cast<icmp_socket*>(obj->impl);
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

    size_t copied = pkt->length() < count ? pkt->length() : count;
    string::memcpy(kdst, pkt->data(), copied);

    if (kaddr && addrlen) {
        const ipv4::ipv4_header* ip = reinterpret_cast<const ipv4::ipv4_header*>(pkt->network_header());
        if (inet::fill_sockaddr(kaddr, addrlen, ip->src, 0) != OK) {
            *addrlen = 0;
        }
    }

    packet::free(pkt);
    return static_cast<ssize_t>(copied);
}

// Reports the socket as unbound with its identifier in the port field
static int32_t socket_getname(resource::resource_object* obj, void* kaddr, size_t* addrlen, bool peer) {
    if (peer) {
        return resource::ERR_NOTCONN;
    }

    icmp_socket* sock = static_cast<icmp_socket*>(obj->impl);
    if (inet::fill_sockaddr(kaddr, addrlen, ipv4::UNSPECIFIED_ADDR, sock->id) != OK) {
        return resource::ERR_INVAL;
    }

    return resource::OK;
}

__PRIVILEGED_CODE static void socket_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    socket_close(static_cast<icmp_socket*>(obj->impl));
    obj->impl = nullptr;
}

// A plain read is a receive that does not ask where the reply came from. The
// descriptor's nonblocking flag maps onto the receive flag with the same meaning.
__PRIVILEGED_CODE static ssize_t socket_read(resource::resource_object* obj, void* kdst, size_t count,
                                             uint32_t flags) {
    uint32_t msg_flags = (flags & fs::O_NONBLOCK) ? inet::MSG_DONTWAIT : 0;
    return socket_recvfrom(obj, kdst, count, msg_flags, nullptr, nullptr);
}

static ssize_t socket_write(resource::resource_object*, const void*, size_t, uint32_t) {
    return resource::ERR_UNSUP;
}

// Readable while a reply waits. Sending never blocks, a request is queued on
// the interface or refused at once, so the socket is always writable.
__PRIVILEGED_CODE static uint32_t socket_poll(resource::resource_object* obj, sync::poll_table* pt) {
    if (!obj || !obj->impl) {
        return sync::POLL_NVAL;
    }

    icmp_socket* sock = static_cast<icmp_socket*>(obj->impl);
    if (pt) {
        sync::poll_subscribe(*pt, sock->rx_wq);
    }

    sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);
    uint32_t mask = sock->rx_queue.empty() ? 0 : sync::POLL_IN;
    sync::spin_unlock_irqrestore(sock->lock, irq);

    return mask | sync::POLL_OUT;
}

static const resource::socket_ops g_icmp_socket_ops = {
    .sendto = socket_sendto,
    .recvfrom = socket_recvfrom,
    .getname = socket_getname,
};

static const resource::resource_ops g_socket_ops = {
    .read = socket_read,
    .write = socket_write,
    .close = socket_close,
    .poll = socket_poll,
    .socket = &g_icmp_socket_ops,
};

const resource::resource_ops* socket_ops() {
    return &g_socket_ops;
}

} // namespace icmp
} // namespace net
