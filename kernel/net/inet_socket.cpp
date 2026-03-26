#include "net/inet_socket.h"
#include "net/net.h"
#include "net/ipv4.h"
#include "net/byteorder.h"
#include "common/ring_buffer.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"

namespace net {

namespace {

// ICMP socket registry
__PRIVILEGED_DATA static inet_socket* g_icmp_sockets[MAX_ICMP_SOCKETS] = {};
__PRIVILEGED_DATA static sync::spinlock g_icmp_reg_lock = sync::SPINLOCK_INIT;

// sockaddr_in layout (matches musl/Linux)
struct kernel_sockaddr_in {
    uint16_t sin_family; // AF_INET = 2
    uint16_t sin_port;   // network byte order
    uint32_t sin_addr;   // network byte order
    uint8_t  sin_zero[8];
};

constexpr uint16_t AF_INET_VAL = 2;
constexpr size_t   RX_BUF_CAPACITY = 16384;

// Each entry in the rx ring buffer is:
//   [4 bytes: src_ip in network byte order]
//   [2 bytes: payload length in host byte order]
//   [N bytes: ICMP payload data]
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

    // Parse destination address
    if (!kaddr || addrlen < sizeof(kernel_sockaddr_in)) {
        return resource::ERR_INVAL;
    }
    const auto* addr = static_cast<const kernel_sockaddr_in*>(kaddr);
    if (addr->sin_family != AF_INET_VAL) {
        return resource::ERR_INVAL;
    }

    uint32_t dst_ip = ntohl(addr->sin_addr);

    // Get the network interface
    netif* iface = get_default_netif();
    if (!iface || !iface->configured) {
        return resource::ERR_IO;
    }

    // Trigger RX processing so any pending incoming packets are delivered
    // before we send (ensures ARP table is populated from recent traffic)
    if (iface->poll) {
        RUN_ELEVATED(iface->poll(iface));
    }

    // Send the ICMP data as an IPv4 packet; the kernel does not
    // inspect the ICMP contents — it just wraps them in an IP header.
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

    // Before blocking, trigger RX processing to deliver pending packets
    netif* iface = get_default_netif();
    if (iface && iface->poll) {
        RUN_ELEVATED(iface->poll(iface));
    }

    // Read the entry header (src_ip + payload_len)
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

    // Read the payload
    size_t to_read = payload_len < count ? payload_len : count;
    ssize_t data_rc = ring_buffer_read(sock->rx_buf, static_cast<uint8_t*>(kdst),
                                       to_read, true);
    if (data_rc < 0) {
        return resource::ERR_IO;
    }

    // If user buffer was smaller than the packet, drain the rest
    if (to_read < payload_len) {
        size_t discard = payload_len - to_read;
        uint8_t trash[64];
        while (discard > 0) {
            size_t chunk = discard < sizeof(trash) ? discard : sizeof(trash);
            (void)ring_buffer_read(sock->rx_buf, trash, chunk, true);
            discard -= chunk;
        }
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
    unregister_icmp_socket(sock);

    if (sock->rx_buf) {
        ring_buffer_destroy(sock->rx_buf);
    }
    heap::kfree_delete(sock);
    obj->impl = nullptr;
}

static const resource::resource_ops g_inet_icmp_ops = {
    nullptr,        // read (use recvfrom instead)
    nullptr,        // write (use sendto instead)
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

    register_icmp_socket(sock);

    *out = obj;
    return resource::OK;
}

void register_icmp_socket(inet_socket* sock) {
    sync::irq_lock_guard guard(g_icmp_reg_lock);
    for (uint32_t i = 0; i < MAX_ICMP_SOCKETS; i++) {
        if (!g_icmp_sockets[i]) {
            g_icmp_sockets[i] = sock;
            return;
        }
    }
    log::warn("inet: ICMP socket registry full");
}

void unregister_icmp_socket(inet_socket* sock) {
    sync::irq_lock_guard guard(g_icmp_reg_lock);
    for (uint32_t i = 0; i < MAX_ICMP_SOCKETS; i++) {
        if (g_icmp_sockets[i] == sock) {
            g_icmp_sockets[i] = nullptr;
            return;
        }
    }
}

void deliver_to_icmp_sockets(uint32_t src_ip, const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    // Build the ring buffer entry: [src_ip_net(4)] [payload_len(2)] [data(N)]
    uint32_t src_ip_net = htonl(src_ip);
    uint16_t payload_len = static_cast<uint16_t>(len);

    // Build a single contiguous entry: [src_ip(4)] [payload_len(2)] [data(N)]
    size_t entry_len = RX_ENTRY_HEADER + len;
    uint8_t* entry = static_cast<uint8_t*>(heap::kzalloc(entry_len));
    if (!entry) return;

    string::memcpy(entry, &src_ip_net, 4);
    string::memcpy(entry + 4, &payload_len, 2);
    string::memcpy(entry + RX_ENTRY_HEADER, data, len);

    sync::irq_lock_guard guard(g_icmp_reg_lock);
    for (uint32_t i = 0; i < MAX_ICMP_SOCKETS; i++) {
        inet_socket* sock = g_icmp_sockets[i];
        if (!sock || !sock->rx_buf) continue;

        // All-or-nothing write; drop packet if buffer has insufficient space
        (void)ring_buffer_write_all(sock->rx_buf, entry, entry_len, true);
    }

    heap::kfree(entry);
}

} // namespace net
