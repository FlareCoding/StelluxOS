#include "net/inet_socket.h"
#include "net/net.h"
#include "net/netinfo.h"
#include "net/ipv4.h"
#include "net/icmp.h"
#include "net/udp.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "common/ring_buffer.h"
#include "common/string.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"

namespace net {

namespace {

constexpr size_t RX_BUF_CAPACITY = 16384;

// Ring buffer entry framing (must match icmp.cpp delivery format):
//   [4 bytes: src_ip in network byte order]
//   [2 bytes: payload length in host byte order]
//   [N bytes: payload data]
constexpr size_t RX_ENTRY_HEADER = 6;

__PRIVILEGED_CODE static ssize_t inet_sendto(
    resource::resource_object* obj, const void* ksrc, size_t count,
    uint32_t flags, const void* kaddr, size_t addrlen
) {
    (void)flags;
    if (!obj || !obj->impl || !ksrc) {
        return resource::ERR_INVAL;
    }

    auto* sock = static_cast<inet_socket*>(obj->impl);
    if (sock->protocol != IPV4_PROTO_ICMP) {
        return resource::ERR_INVAL;
    }

    if (!kaddr || addrlen < sizeof(kernel_sockaddr_in)) {
        return resource::ERR_INVAL;
    }
    const auto* addr = static_cast<const kernel_sockaddr_in*>(kaddr);
    if (addr->sin_family != AF_INET_VAL) {
        return resource::ERR_INVAL;
    }

    uint32_t dst_ip = ntohl(addr->sin_addr);

    netif* iface = get_default_netif();
    if (!iface || !iface->configured) {
        return resource::ERR_IO;
    }

    // Trigger RX processing so pending incoming packets are delivered
    if (iface->poll) {
        RUN_ELEVATED(iface->poll(iface));
    }

    int32_t rc = ipv4_send(iface, dst_ip, IPV4_PROTO_ICMP,
                           static_cast<const uint8_t*>(ksrc), count);
    if (rc != OK) {
        return resource::ERR_IO;
    }

    return static_cast<ssize_t>(count);
}

__PRIVILEGED_CODE static ssize_t inet_recvfrom(
    resource::resource_object* obj, void* kdst, size_t count,
    uint32_t flags, void* kaddr, size_t* addrlen
) {
    if (!obj || !obj->impl || !kdst) {
        return resource::ERR_INVAL;
    }

    auto* sock = static_cast<inet_socket*>(obj->impl);
    bool nonblock = (flags & 0x40) != 0; // MSG_DONTWAIT

    // Trigger RX processing to deliver pending packets
    netif* iface = get_default_netif();
    if (iface && iface->poll) {
        RUN_ELEVATED(iface->poll(iface));
    }

    // The header read may block if no data is available. Since entries
    // are written atomically via ring_buffer_write_all, once the header
    // is available the payload is guaranteed to be there too. No lock
    // is needed because the ring buffer has its own internal lock per
    // read/write, and the atomic write ensures the header and payload
    // are always a complete unit.
    uint8_t hdr[RX_ENTRY_HEADER];
    ssize_t hdr_rc = ring_buffer_read(sock->rx_buf, hdr, RX_ENTRY_HEADER, nonblock);
    if (hdr_rc == RB_ERR_AGAIN) {
        return resource::ERR_AGAIN;
    }
    if (hdr_rc < static_cast<ssize_t>(RX_ENTRY_HEADER)) {
        return resource::ERR_IO;
    }

    uint32_t src_ip_net;
    string::memcpy(&src_ip_net, hdr, 4);
    uint16_t payload_len;
    string::memcpy(&payload_len, hdr + 4, 2);

    size_t to_read = payload_len < count ? payload_len : count;
    ssize_t data_rc = ring_buffer_read(sock->rx_buf, static_cast<uint8_t*>(kdst),
                                       to_read, true);

    // Always drain remaining bytes from this entry to keep the stream
    // synchronized, even if the payload read failed or the user buffer
    // was smaller than the packet.
    size_t consumed = (data_rc > 0) ? static_cast<size_t>(data_rc) : 0;
    if (consumed < payload_len) {
        size_t discard = payload_len - consumed;
        uint8_t trash[64];
        while (discard > 0) {
            size_t chunk = discard < sizeof(trash) ? discard : sizeof(trash);
            (void)ring_buffer_read(sock->rx_buf, trash, chunk, true);
            discard -= chunk;
        }
    }

    if (data_rc < 0) {
        return resource::ERR_IO;
    }

    // Fill in source address
    if (kaddr && addrlen && *addrlen >= sizeof(kernel_sockaddr_in)) {
        auto* src = static_cast<kernel_sockaddr_in*>(kaddr);
        string::memset(src, 0, sizeof(*src));
        src->sin_family = AF_INET_VAL;
        src->sin_addr = src_ip_net;
        *addrlen = sizeof(kernel_sockaddr_in);
    }

    return data_rc;
}

__PRIVILEGED_CODE static int32_t inet_ioctl(
    resource::resource_object* obj, uint32_t cmd, uint64_t arg) {
    (void)obj;
    if (cmd == STLX_SIOCGNETSTATUS) {
        net_status status = {};
        query_status(&status);
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(arg), &status, sizeof(status));
        return (rc == mm::uaccess::OK) ? resource::OK : resource::ERR_INVAL;
    }
    if (cmd == STLX_SIOCGARPTABLE) {
        arp_table_status table = {};
        query_arp_table(&table);
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(arg), &table, sizeof(table));
        return (rc == mm::uaccess::OK) ? resource::OK : resource::ERR_INVAL;
    }
    return resource::ERR_UNSUP;
}

__PRIVILEGED_CODE static void inet_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    auto* sock = static_cast<inet_socket*>(obj->impl);

    // Unregister from the protocol layer
    if (sock->protocol == IPV4_PROTO_ICMP) {
        icmp_unregister_socket(sock);
    } else if (sock->protocol == IPV4_PROTO_UDP && sock->bound_port != 0) {
        udp_unregister_socket(sock);
    }

    if (sock->rx_buf) {
        ring_buffer_destroy(sock->rx_buf);
    }
    heap::kfree_delete(sock);
    obj->impl = nullptr;
}

static const resource::resource_ops g_inet_icmp_ops = {
    nullptr,        // read
    nullptr,        // write
    inet_close,
    inet_ioctl,
    nullptr,        // mmap
    inet_sendto,
    inet_recvfrom,
};

// UDP ring buffer entry framing (must match udp.cpp delivery format):
//   [4 bytes: src_ip in network byte order]
//   [2 bytes: src_port in network byte order]
//   [2 bytes: payload length in host byte order]
//   [N bytes: payload data]
constexpr size_t UDP_RX_ENTRY_HEADER = 8;

__PRIVILEGED_CODE static ssize_t inet_udp_sendto(
    resource::resource_object* obj, const void* ksrc, size_t count,
    uint32_t flags, const void* kaddr, size_t addrlen
) {
    (void)flags;
    if (!obj || !obj->impl || !ksrc) {
        return resource::ERR_INVAL;
    }

    auto* sock = static_cast<inet_socket*>(obj->impl);

    if (!kaddr || addrlen < sizeof(kernel_sockaddr_in)) {
        return resource::ERR_INVAL;
    }
    const auto* addr = static_cast<const kernel_sockaddr_in*>(kaddr);
    if (addr->sin_family != AF_INET_VAL) {
        return resource::ERR_INVAL;
    }

    uint32_t dst_ip = ntohl(addr->sin_addr);
    uint16_t dst_port = ntohs(addr->sin_port);

    netif* iface = get_default_netif();
    if (!iface || !iface->configured) {
        return resource::ERR_IO;
    }

    // Assign ephemeral port on first send and register with UDP layer.
    // Double-checked under sock->lock to prevent duplicate registration
    // if two sendto calls race on the same socket.
    if (sock->bound_port == 0) {
        sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);
        if (sock->bound_port == 0) {
            sock->bound_port = udp_alloc_ephemeral_port();
            udp_register_socket(sock);
        }
        sync::spin_unlock_irqrestore(sock->lock, irq);
    }

    if (iface->poll) {
        RUN_ELEVATED(iface->poll(iface));
    }

    // Build UDP packet: header + payload
    size_t udp_total = sizeof(udp_header) + count;
    auto* udp_pkt = static_cast<uint8_t*>(heap::kzalloc(udp_total));
    if (!udp_pkt) {
        return resource::ERR_NOMEM;
    }

    auto* uhdr = reinterpret_cast<udp_header*>(udp_pkt);
    uhdr->src_port = htons(sock->bound_port);
    uhdr->dst_port = htons(dst_port);
    uhdr->length = htons(static_cast<uint16_t>(udp_total));
    uhdr->checksum = 0;
    string::memcpy(udp_pkt + sizeof(udp_header), ksrc, count);

    uint16_t csum = udp_checksum(
        htonl(iface->ipv4_addr), htonl(dst_ip), udp_pkt, udp_total);
    uhdr->checksum = (csum == 0) ? static_cast<uint16_t>(0xFFFF) : csum;

    int32_t rc = ipv4_send(iface, dst_ip, IPV4_PROTO_UDP, udp_pkt, udp_total);
    heap::kfree(udp_pkt);

    if (rc != OK) {
        return resource::ERR_IO;
    }
    return static_cast<ssize_t>(count);
}

__PRIVILEGED_CODE static ssize_t inet_udp_recvfrom(
    resource::resource_object* obj, void* kdst, size_t count,
    uint32_t flags, void* kaddr, size_t* addrlen
) {
    if (!obj || !obj->impl || !kdst) {
        return resource::ERR_INVAL;
    }

    auto* sock = static_cast<inet_socket*>(obj->impl);
    bool nonblock = (flags & 0x40) != 0; // MSG_DONTWAIT

    netif* iface = get_default_netif();
    if (iface && iface->poll) {
        RUN_ELEVATED(iface->poll(iface));
    }

    uint8_t hdr[UDP_RX_ENTRY_HEADER];
    ssize_t hdr_rc = ring_buffer_read(sock->rx_buf, hdr, UDP_RX_ENTRY_HEADER, nonblock);
    if (hdr_rc == RB_ERR_AGAIN) {
        return resource::ERR_AGAIN;
    }
    if (hdr_rc < static_cast<ssize_t>(UDP_RX_ENTRY_HEADER)) {
        return resource::ERR_IO;
    }

    uint32_t src_ip_net;
    string::memcpy(&src_ip_net, hdr, 4);
    uint16_t src_port_net;
    string::memcpy(&src_port_net, hdr + 4, 2);
    uint16_t payload_len;
    string::memcpy(&payload_len, hdr + 6, 2);

    size_t to_read = payload_len < count ? payload_len : count;
    ssize_t data_rc = ring_buffer_read(sock->rx_buf, static_cast<uint8_t*>(kdst),
                                       to_read, true);

    size_t consumed = (data_rc > 0) ? static_cast<size_t>(data_rc) : 0;
    if (consumed < payload_len) {
        size_t discard = payload_len - consumed;
        uint8_t trash[64];
        while (discard > 0) {
            size_t chunk = discard < sizeof(trash) ? discard : sizeof(trash);
            (void)ring_buffer_read(sock->rx_buf, trash, chunk, true);
            discard -= chunk;
        }
    }

    if (data_rc < 0) {
        return resource::ERR_IO;
    }

    if (kaddr && addrlen && *addrlen >= sizeof(kernel_sockaddr_in)) {
        auto* src = static_cast<kernel_sockaddr_in*>(kaddr);
        string::memset(src, 0, sizeof(*src));
        src->sin_family = AF_INET_VAL;
        src->sin_port = src_port_net;
        src->sin_addr = src_ip_net;
        *addrlen = sizeof(kernel_sockaddr_in);
    }

    return data_rc;
}

static const resource::resource_ops g_inet_udp_ops = {
    nullptr,            // read
    nullptr,            // write
    inet_close,
    inet_ioctl,
    nullptr,            // mmap
    inet_udp_sendto,
    inet_udp_recvfrom,
};

} // anonymous namespace

int32_t create_inet_icmp_socket(resource::resource_object** out) {
    if (!out) {
        return resource::ERR_INVAL;
    }

    auto* sock = heap::kalloc_new<inet_socket>();
    if (!sock) {
        return resource::ERR_NOMEM;
    }
    sock->protocol = IPV4_PROTO_ICMP;
    sock->bound_addr = 0;
    sock->bound_port = 0;
    sock->lock = sync::SPINLOCK_INIT;
    sock->next = nullptr;

    sock->rx_buf = ring_buffer_create(RX_BUF_CAPACITY);
    if (!sock->rx_buf) {
        heap::kfree_delete(sock);
        return resource::ERR_NOMEM;
    }

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        ring_buffer_destroy(sock->rx_buf);
        heap::kfree_delete(sock);
        return resource::ERR_NOMEM;
    }
    obj->type = resource::resource_type::SOCKET;
    obj->ops = &g_inet_icmp_ops;
    obj->impl = sock;

    // Register with the ICMP protocol layer for packet delivery
    icmp_register_socket(sock);

    *out = obj;
    return resource::OK;
}

int32_t create_inet_udp_socket(resource::resource_object** out) {
    if (!out) {
        return resource::ERR_INVAL;
    }

    auto* sock = heap::kalloc_new<inet_socket>();
    if (!sock) {
        return resource::ERR_NOMEM;
    }
    sock->protocol = IPV4_PROTO_UDP;
    sock->bound_addr = 0;
    sock->bound_port = 0;
    sock->lock = sync::SPINLOCK_INIT;
    sock->next = nullptr;

    sock->rx_buf = ring_buffer_create(RX_BUF_CAPACITY);
    if (!sock->rx_buf) {
        heap::kfree_delete(sock);
        return resource::ERR_NOMEM;
    }

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        ring_buffer_destroy(sock->rx_buf);
        heap::kfree_delete(sock);
        return resource::ERR_NOMEM;
    }
    obj->type = resource::resource_type::SOCKET;
    obj->ops = &g_inet_udp_ops;
    obj->impl = sock;

    *out = obj;
    return resource::OK;
}

} // namespace net
