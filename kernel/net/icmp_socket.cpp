#include "net/icmp_socket.h"
#include "net/icmp.h"
#include "net/inet.h"
#include "net/eth.h"
#include "net/interface.h"
#include "resource/resource.h"
#include "mm/heap.h"
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

// Open sockets by slot. Written under g_sockets_lock from syscall
// context, read under it from the driver task on delivery.
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

icmp_socket* socket_open() {
    icmp_socket* sock = heap::ualloc_new<icmp_socket>();
    if (!sock) {
        return nullptr;
    }

    sock->lock = sync::SPINLOCK_INIT;
    sock->rx_queue.init();

    sync::lock_guard guard(g_sockets_lock);
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

void socket_close(icmp_socket* sock) {
    if (!sock) {
        return;
    }

    {
        sync::lock_guard guard(g_sockets_lock);
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

    // The socket lock is taken inside the table lock so the socket
    // cannot be closed between the lookup and the enqueue.
    sync::lock_guard table_guard(g_sockets_lock);

    icmp_socket* sock = find_locked(id);
    if (!sock) {
        pkt->iface()->record_packet_dropped();
        packet::free(pkt);
        return;
    }

    sync::lock_guard sock_guard(sock->lock);
    if (sock->rx_queue.size() >= SOCKET_QUEUE_DEPTH) {
        packet* oldest = sock->rx_queue.pop_front();
        oldest->iface()->record_packet_dropped();
        packet::free(oldest);
    }

    sock->rx_queue.push_back(pkt);
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

// Hands the oldest waiting reply to the caller, truncated to
// `count` bytes, with its source address in `kaddr`.
static ssize_t socket_recvfrom(resource::resource_object* obj, void* kdst, size_t count,
                               uint32_t, void* kaddr, size_t* addrlen) {
    icmp_socket* sock = static_cast<icmp_socket*>(obj->impl);

    packet* pkt = nullptr;
    {
        sync::lock_guard guard(sock->lock);
        pkt = sock->rx_queue.pop_front();
    }

    if (!pkt) {
        return resource::ERR_AGAIN;
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

static void socket_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    socket_close(static_cast<icmp_socket*>(obj->impl));
    obj->impl = nullptr;
}

static ssize_t socket_read(resource::resource_object*, void*, size_t, uint32_t) {
    return resource::ERR_UNSUP;
}

static ssize_t socket_write(resource::resource_object*, const void*, size_t, uint32_t) {
    return resource::ERR_UNSUP;
}

static const resource::resource_ops g_socket_ops = {
    socket_read,
    socket_write,
    socket_close,
    nullptr, // ioctl
    nullptr, // mmap
    socket_sendto,
    socket_recvfrom,
    nullptr, // bind
    nullptr, // listen
    nullptr, // accept
    nullptr, // connect
    nullptr, // setsockopt
    nullptr, // getsockopt
    nullptr, // poll
    nullptr, // shutdown
};

const resource::resource_ops* socket_ops() {
    return &g_socket_ops;
}

} // namespace icmp
} // namespace net
