#include "net/udp.h"
#include "net/inet_socket.h"
#include "net/dhcp.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "net/net.h"
#include "common/string.h"
#include "common/ring_buffer.h"
#include "mm/heap.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"
#include "common/logging.h"

namespace net {

namespace {

__PRIVILEGED_DATA static inet_socket* g_udp_sock_list = nullptr;
__PRIVILEGED_DATA static sync::spinlock g_udp_sock_lock = sync::SPINLOCK_INIT;
__PRIVILEGED_DATA static volatile uint32_t g_ephemeral_next = UDP_PORT_EPHEMERAL_MIN;

// Ring buffer entry framing for UDP:
//   [4 bytes: src_ip, network byte order]
//   [2 bytes: src_port, network byte order]
//   [2 bytes: payload_len, host byte order]
//   [N bytes: UDP payload]
constexpr size_t RX_ENTRY_HEADER = 8;

} // anonymous namespace

void udp_recv(netif* iface, uint32_t src_ip, uint32_t dst_ip,
              const uint8_t* data, size_t len) {
    if (!iface || !data || len < sizeof(udp_header)) {
        return;
    }

    const auto* hdr = reinterpret_cast<const udp_header*>(data);

    uint16_t udp_len = ntohs(hdr->length);
    if (udp_len < sizeof(udp_header) || udp_len > len) {
        return;
    }

    // Verify checksum if present (checksum == 0 means "not computed" per RFC 768)
    if (hdr->checksum != 0) {
        uint16_t computed = udp_checksum(htonl(src_ip), htonl(dst_ip), data, udp_len);
        if (computed != 0) {
            log::debug("udp: bad checksum, dropping");
            return;
        }
    }

    uint16_t dst_port_net = hdr->dst_port;
    const uint8_t* payload = data + sizeof(udp_header);
    size_t payload_len = udp_len - sizeof(udp_header);

    // DHCP receive hook: deliver port-68 packets to the DHCP client
    // via a static buffer, bypassing the socket/ring_buffer path.
    // This runs before socket delivery so the DHCP client gets the
    // packet even when no socket is registered on port 68.
    if (ntohs(dst_port_net) == DHCP_CLIENT_PORT && payload_len > 0) {
        dhcp_rx_hook(payload, payload_len);
    }

    // Build the framed ring buffer entry before taking the lock.
    // This keeps heap alloc/free outside the IRQ-disabled critical section.
    uint32_t src_ip_net = htonl(src_ip);
    uint16_t src_port_net = hdr->src_port;
    uint16_t plen = static_cast<uint16_t>(payload_len);

    size_t entry_len = RX_ENTRY_HEADER + payload_len;
    auto* entry = static_cast<uint8_t*>(heap::kzalloc(entry_len));
    if (!entry) return;

    string::memcpy(entry, &src_ip_net, 4);
    string::memcpy(entry + 4, &src_port_net, 2);
    string::memcpy(entry + 6, &plen, 2);
    if (payload && payload_len > 0) {
        string::memcpy(entry + RX_ENTRY_HEADER, payload, payload_len);
    }

    // Lock only for socket lookup + ring buffer write
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_udp_sock_lock);
        for (inet_socket* s = g_udp_sock_list; s; s = s->next) {
            if (htons(s->bound_port) == dst_port_net
                && (s->bound_addr == 0 || s->bound_addr == dst_ip)
                && s->rx_buf) {
                (void)ring_buffer_write_all(s->rx_buf, entry, entry_len, true);
                break;
            }
        }
    });

    heap::kfree(entry);
}

void udp_register_socket(inet_socket* sock) {
    if (!sock) return;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_udp_sock_lock);
        sock->next = g_udp_sock_list;
        g_udp_sock_list = sock;
    });
}

void udp_unregister_socket(inet_socket* sock) {
    if (!sock) return;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_udp_sock_lock);
        inet_socket** pp = &g_udp_sock_list;
        while (*pp) {
            if (*pp == sock) {
                *pp = sock->next;
                sock->next = nullptr;
                break;
            }
            pp = &(*pp)->next;
        }
    });
}

bool udp_try_register(inet_socket* sock) {
    if (!sock || sock->bound_port == 0) {
        return false;
    }
    bool reuse = (sock->so_options & static_cast<uint32_t>(SO_REUSEADDR)) != 0;
    bool registered = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_udp_sock_lock);

        bool conflict = false;
        for (inet_socket* s = g_udp_sock_list; s; s = s->next) {
            if (s == sock) continue;
            if (s->bound_port != sock->bound_port) continue;
            if (sock->bound_addr != 0 && s->bound_addr != 0
                && s->bound_addr != sock->bound_addr) continue;

            if (reuse && (s->so_options & static_cast<uint32_t>(SO_REUSEADDR))) {
                continue;
            }

            conflict = true;
            break;
        }

        if (!conflict) {
            sock->next = g_udp_sock_list;
            g_udp_sock_list = sock;
            registered = true;
        }
    });
    return registered;
}

uint16_t udp_alloc_ephemeral_port() {
    uint32_t port = __atomic_fetch_add(&g_ephemeral_next, 1, __ATOMIC_RELAXED);
    uint32_t range = UDP_PORT_EPHEMERAL_MAX - UDP_PORT_EPHEMERAL_MIN + 1;
    return static_cast<uint16_t>(
        UDP_PORT_EPHEMERAL_MIN + (port - UDP_PORT_EPHEMERAL_MIN) % range);
}

} // namespace net
