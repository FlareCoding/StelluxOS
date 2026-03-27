#include "net/inet_socket.h"
#include "net/net.h"
#include "net/ipv4.h"
#include "net/icmp.h"
#include "net/byteorder.h"
#include "common/ring_buffer.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"
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

__PRIVILEGED_CODE static void inet_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    auto* sock = static_cast<inet_socket*>(obj->impl);

    // Unregister from the protocol layer
    if (sock->protocol == IPV4_PROTO_ICMP) {
        icmp_unregister_socket(sock);
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
    nullptr,        // ioctl
    nullptr,        // mmap
    inet_sendto,
    inet_recvfrom,
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

} // namespace net
