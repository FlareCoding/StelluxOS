#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/inet_socket.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "net/net.h"
#include "common/string.h"
#include "common/ring_buffer.h"
#include "mm/heap.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"

namespace net {

namespace {

// Linked list of ICMP sockets registered for packet delivery.
// Protected by g_icmp_sock_lock.
__PRIVILEGED_DATA static inet_socket* g_icmp_sock_list = nullptr;
__PRIVILEGED_DATA static sync::spinlock g_icmp_sock_lock = sync::SPINLOCK_INIT;

// Ring buffer entry framing: [src_ip(4)] [payload_len(2)] [data(N)]
constexpr size_t RX_ENTRY_HEADER = 6;

static void deliver_to_sockets(uint32_t src_ip, const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    uint32_t src_ip_net = htonl(src_ip);
    uint16_t payload_len = static_cast<uint16_t>(len);

    size_t entry_len = RX_ENTRY_HEADER + len;
    auto* entry = static_cast<uint8_t*>(heap::kzalloc(entry_len));
    if (!entry) return;

    string::memcpy(entry, &src_ip_net, 4);
    string::memcpy(entry + 4, &payload_len, 2);
    string::memcpy(entry + RX_ENTRY_HEADER, data, len);

    uint32_t sock_count = 0;
    uint32_t write_ok = 0;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_icmp_sock_lock);
        for (inet_socket* s = g_icmp_sock_list; s; s = s->next) {
            sock_count++;
            if (s->rx_buf) {
                ssize_t wrc = ring_buffer_write_all(s->rx_buf, entry, entry_len, true);
                if (wrc > 0) write_ok++;
            }
        }
    });

    log::info("TRACE icmp_deliver: src=%u.%u.%u.%u len=%u socks=%u written=%u",
              (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
              (src_ip >> 8) & 0xFF, src_ip & 0xFF,
              static_cast<uint32_t>(len), sock_count, write_ok);

    heap::kfree(entry);
}

} // anonymous namespace

void icmp_recv(netif* iface, uint32_t src_ip, const uint8_t* data, size_t len) {
    if (!iface || !data || len < sizeof(icmp_header)) {
        return;
    }

    const auto* hdr = reinterpret_cast<const icmp_header*>(data);

    // Verify ICMP checksum
    uint16_t computed = inet_checksum(data, len);
    if (computed != 0) {
        log::info("TRACE icmp_recv: BAD CHECKSUM from %u.%u.%u.%u type=%u",
                  (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                  (src_ip >> 8) & 0xFF, src_ip & 0xFF, hdr->type);
        return;
    }

    log::info("TRACE icmp_recv: type=%u code=%u src=%u.%u.%u.%u len=%u",
              hdr->type, hdr->code,
              (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
              (src_ip >> 8) & 0xFF, src_ip & 0xFF,
              static_cast<uint32_t>(len));

    if (hdr->type == ICMP_TYPE_ECHO_REQUEST && hdr->code == 0) {
        // Build the echo reply and queue it for deferred transmission.
        // Sending inline from RX context would recurse through
        // ipv4_send → arp_resolve → poll → deliver_rx_batch.
        if (len <= ETH_MTU) {
            auto* reply = static_cast<uint8_t*>(heap::kzalloc(len));
            if (reply) {
                string::memcpy(reply, data, len);
                auto* reply_hdr = reinterpret_cast<icmp_header*>(reply);
                reply_hdr->type = ICMP_TYPE_ECHO_REPLY;
                reply_hdr->code = 0;
                reply_hdr->checksum = 0;
                reply_hdr->checksum = inet_checksum(reply, len);

                queue_deferred_tx(iface, src_ip, IPV4_PROTO_ICMP, reply, len);
                heap::kfree(reply);
            }
        }
    }

    // Deliver all ICMP packets to registered userland sockets
    deliver_to_sockets(src_ip, data, len);
}

void icmp_register_socket(inet_socket* sock) {
    if (!sock) return;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_icmp_sock_lock);
        sock->next = g_icmp_sock_list;
        g_icmp_sock_list = sock;
    });
}

void icmp_unregister_socket(inet_socket* sock) {
    if (!sock) return;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_icmp_sock_lock);
        inet_socket** pp = &g_icmp_sock_list;
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

} // namespace net
