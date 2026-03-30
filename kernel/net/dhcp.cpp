#include "net/dhcp.h"
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "net/udp.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

namespace net {

// ============================================================================
// DHCP Receive Hook
//
// The DHCP client runs at Ring 3 (user mode) in a kernel task, so it cannot
// use ring_buffer or other __PRIVILEGED_CODE APIs directly. Instead, we use
// a simple static receive context: udp_recv() (which runs at Ring 0 in
// interrupt/poll context) copies incoming port-68 packets into this buffer,
// and the DHCP client polls the ready flag.
// ============================================================================

namespace {

struct dhcp_rx_context {
    uint8_t  buffer[DHCP_PACKET_MAX];
    size_t   length;
    volatile bool ready;   // set by udp_recv hook, cleared by DHCP poll
    volatile bool active;  // true while DHCP is waiting for packets
};

// Single static context — only one DHCP exchange at a time.
// NOT __PRIVILEGED_DATA: both the DHCP client (Ring 3) and the UDP
// delivery path (Ring 0) access this buffer, so it must be in the
// unprivileged data section accessible from both privilege levels.
static dhcp_rx_context g_dhcp_rx = {};

} // anonymous namespace

// Called by udp_recv() when a UDP packet arrives on port 68 and the
// DHCP receive hook is active. Runs in the poll/RX delivery context
// which may be at Ring 0 (inside RUN_ELEVATED) or part of the
// interrupt-driven path. NOT __PRIVILEGED_CODE because the buffer
// is in regular .bss (accessible from both Ring 0 and Ring 3).
void dhcp_rx_hook(const uint8_t* data, size_t len) {
    if (!__atomic_load_n(&g_dhcp_rx.active, __ATOMIC_ACQUIRE)) return;
    if (__atomic_load_n(&g_dhcp_rx.ready, __ATOMIC_ACQUIRE)) return;

    size_t copy_len = len < DHCP_PACKET_MAX ? len : DHCP_PACKET_MAX;
    string::memcpy(g_dhcp_rx.buffer, data, copy_len);
    g_dhcp_rx.length = copy_len;

    // Write barrier: ensure buffer/length are visible before ready flag
    __atomic_store_n(&g_dhcp_rx.ready, true, __ATOMIC_RELEASE);
}

// ============================================================================
// Internal helpers
// ============================================================================

namespace {

/**
 * Initialize the common fields of a DHCP packet header.
 */
static void init_dhcp_header(dhcp_packet* pkt, const uint8_t* mac, uint32_t xid) {
    string::memset(pkt, 0, sizeof(dhcp_packet));
    pkt->op    = DHCP_OP_BOOTREQUEST;
    pkt->htype = DHCP_HTYPE_ETHERNET;
    pkt->hlen  = DHCP_HLEN_ETHERNET;
    pkt->hops  = 0;
    pkt->xid   = xid;  // already in network byte order
    pkt->secs  = 0;
    pkt->flags = htons(DHCP_FLAG_BROADCAST);
    // ciaddr, yiaddr, siaddr, giaddr all zero
    string::memcpy(pkt->chaddr, mac, MAC_ADDR_LEN);
    // sname and file are zero-filled by memset
    pkt->magic = htonl(DHCP_MAGIC_COOKIE);
}

/**
 * Append a DHCP option to the options buffer.
 * @return Number of bytes written, or 0 if buffer is too small.
 */
static size_t append_option(uint8_t* opts, size_t offset, size_t max_len,
                            uint8_t code, const uint8_t* data, uint8_t data_len) {
    if (code == DHCP_OPT_END) {
        if (offset >= max_len) return 0;
        opts[offset] = DHCP_OPT_END;
        return 1;
    }

    size_t needed = 2 + static_cast<size_t>(data_len);
    if (offset + needed > max_len) return 0;

    opts[offset]     = code;
    opts[offset + 1] = data_len;
    if (data_len > 0 && data) {
        string::memcpy(opts + offset + 2, data, data_len);
    }
    return needed;
}

/**
 * Append a single-byte DHCP option.
 */
static size_t append_option_u8(uint8_t* opts, size_t offset, size_t max_len,
                               uint8_t code, uint8_t value) {
    return append_option(opts, offset, max_len, code, &value, 1);
}

/**
 * Append a 4-byte (uint32_t, network byte order) DHCP option.
 */
static size_t append_option_u32(uint8_t* opts, size_t offset, size_t max_len,
                                uint8_t code, uint32_t value_net) {
    return append_option(opts, offset, max_len, code,
                         reinterpret_cast<const uint8_t*>(&value_net), 4);
}

/**
 * Generate a pseudo-random transaction ID from the clock and MAC address.
 * Returns the xid in network byte order.
 */
static uint32_t generate_xid(const uint8_t* mac) {
    uint64_t ns = clock::now_ns();
    uint32_t seed = static_cast<uint32_t>(ns ^ (ns >> 32));
    // Mix in MAC bytes for additional entropy
    seed ^= (static_cast<uint32_t>(mac[2]) << 24) |
            (static_cast<uint32_t>(mac[3]) << 16) |
            (static_cast<uint32_t>(mac[4]) << 8)  |
            static_cast<uint32_t>(mac[5]);
    return htonl(seed);
}

/**
 * Build a complete Ethernet frame containing an IPv4/UDP/DHCP broadcast
 * and transmit it directly via the interface's transmit callback.
 *
 * This bypasses eth_send() and ipv4_send() entirely because:
 * - ipv4_send() requires a configured interface (DHCP runs before config)
 * - eth_send() uses heap::kzalloc which is __PRIVILEGED_CODE
 *
 * Uses heap::uzalloc() (unprivileged, auto-elevating) for the frame buffer.
 *
 * @param iface   Interface to send on (must have MAC and transmit callback).
 * @param payload DHCP message (header + options).
 * @param payload_len Length of the DHCP message.
 * @return net::OK on success, negative error code on failure.
 */
static int32_t send_dhcp_broadcast(netif* iface, const uint8_t* payload,
                                   size_t payload_len) {
    if (!iface || !iface->transmit || !payload || payload_len == 0) {
        return ERR_INVAL;
    }

    // Build the complete Ethernet frame: ETH + IPv4 + UDP + DHCP payload.
    size_t udp_total = sizeof(udp_header) + payload_len;
    size_t ip_total  = sizeof(ipv4_header) + udp_total;
    size_t frame_len = sizeof(eth_header) + ip_total;

    if (frame_len > ETH_FRAME_MAX) {
        return ERR_INVAL;
    }

    // Use unprivileged heap — callable from Ring 3 (auto-elevates internally)
    auto* frame = static_cast<uint8_t*>(heap::uzalloc(frame_len));
    if (!frame) {
        return ERR_NOMEM;
    }

    // --- Ethernet header ---
    auto* eth = reinterpret_cast<eth_header*>(frame);
    string::memcpy(eth->dst, ETH_BROADCAST, MAC_ADDR_LEN);
    string::memcpy(eth->src, iface->mac, MAC_ADDR_LEN);
    eth->ethertype = htons(ETH_TYPE_IPV4);

    // --- IPv4 header ---
    auto* ip = reinterpret_cast<ipv4_header*>(frame + sizeof(eth_header));
    ip->ver_ihl   = (4 << 4) | 5;      // IPv4, IHL=5 (20 bytes)
    ip->tos       = 0;
    ip->total_len = htons(static_cast<uint16_t>(ip_total));
    ip->id        = 0;
    ip->flags_frag = 0;                 // no DF, no fragmentation
    ip->ttl       = 64;
    ip->protocol  = IPV4_PROTO_UDP;
    ip->checksum  = 0;
    ip->src_ip    = 0;                  // 0.0.0.0
    ip->dst_ip    = htonl(0xFFFFFFFF);  // 255.255.255.255
    ip->checksum  = inet_checksum(ip, sizeof(ipv4_header));

    // --- UDP header ---
    auto* udp = reinterpret_cast<udp_header*>(frame + sizeof(eth_header) + sizeof(ipv4_header));
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->length   = htons(static_cast<uint16_t>(udp_total));
    udp->checksum = 0;

    // --- DHCP payload ---
    string::memcpy(frame + sizeof(eth_header) + sizeof(ipv4_header) + sizeof(udp_header),
                   payload, payload_len);

    // Compute UDP checksum over pseudo-header + UDP segment.
    uint16_t csum = udp_checksum(ip->src_ip, ip->dst_ip,
                                 frame + sizeof(eth_header) + sizeof(ipv4_header),
                                 udp_total);
    udp->checksum = (csum == 0) ? static_cast<uint16_t>(0xFFFF) : csum;

    // Transmit the frame directly via the driver callback.
    // tx_callback uses RUN_ELEVATED internally for its lock/DMA access.
    int32_t rc = iface->transmit(iface, frame, frame_len);
    heap::ufree(frame);
    return rc;
}

/**
 * Check if a DHCP response is available in the receive hook buffer.
 * Non-blocking: returns false if no data is available.
 */
static bool try_read_dhcp_response(uint8_t* out_buf, size_t buf_size,
                                   size_t* out_len) {
    if (!out_buf || !out_len) return false;

    // Check ready flag with acquire semantics
    if (!__atomic_load_n(&g_dhcp_rx.ready, __ATOMIC_ACQUIRE)) {
        return false;
    }

    size_t copy_len = g_dhcp_rx.length < buf_size ? g_dhcp_rx.length : buf_size;
    string::memcpy(out_buf, g_dhcp_rx.buffer, copy_len);
    *out_len = copy_len;

    // Mark as consumed
    __atomic_store_n(&g_dhcp_rx.ready, false, __ATOMIC_RELEASE);

    return true;
}

/**
 * Activate the DHCP receive hook. udp_recv() will start copying
 * port-68 packets to the static buffer.
 */
static void activate_rx_hook() {
    g_dhcp_rx.length = 0;
    __atomic_store_n(&g_dhcp_rx.ready, false, __ATOMIC_RELEASE);
    __atomic_store_n(&g_dhcp_rx.active, true, __ATOMIC_RELEASE);
}

/**
 * Deactivate the DHCP receive hook.
 */
static void deactivate_rx_hook() {
    __atomic_store_n(&g_dhcp_rx.active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&g_dhcp_rx.ready, false, __ATOMIC_RELEASE);
}

} // anonymous namespace

// ============================================================================
// Packet Build Functions
// ============================================================================

size_t dhcp_build_discover(uint8_t* out, size_t out_size,
                           const uint8_t* mac, uint32_t xid) {
    if (!out || !mac || out_size < sizeof(dhcp_packet) + 16) {
        return 0;
    }

    auto* pkt = reinterpret_cast<dhcp_packet*>(out);
    init_dhcp_header(pkt, mac, xid);

    uint8_t* opts = out + sizeof(dhcp_packet);
    size_t opts_max = out_size - sizeof(dhcp_packet);
    size_t pos = 0;
    size_t n;

    // Option 53: DHCP Message Type = DISCOVER
    n = append_option_u8(opts, pos, opts_max, DHCP_OPT_MSG_TYPE, DHCP_MSG_DISCOVER);
    if (n == 0) return 0;
    pos += n;

    // Option 12: Hostname
    static const char hostname[] = "stellux";
    n = append_option(opts, pos, opts_max, DHCP_OPT_HOSTNAME,
                      reinterpret_cast<const uint8_t*>(hostname), sizeof(hostname) - 1);
    if (n == 0) return 0;
    pos += n;

    // Option 55: Parameter Request List
    uint8_t param_list[] = {
        DHCP_OPT_SUBNET_MASK,
        DHCP_OPT_ROUTER,
        DHCP_OPT_DNS,
        DHCP_OPT_LEASE_TIME,
    };
    n = append_option(opts, pos, opts_max, DHCP_OPT_PARAM_LIST,
                      param_list, sizeof(param_list));
    if (n == 0) return 0;
    pos += n;

    // Option 255: End
    n = append_option(opts, pos, opts_max, DHCP_OPT_END, nullptr, 0);
    if (n == 0) return 0;
    pos += n;

    return sizeof(dhcp_packet) + pos;
}

size_t dhcp_build_request(uint8_t* out, size_t out_size,
                          const uint8_t* mac, uint32_t xid,
                          uint32_t offered_ip, uint32_t server_id) {
    if (!out || !mac || out_size < sizeof(dhcp_packet) + 32) {
        return 0;
    }

    auto* pkt = reinterpret_cast<dhcp_packet*>(out);
    init_dhcp_header(pkt, mac, xid);

    uint8_t* opts = out + sizeof(dhcp_packet);
    size_t opts_max = out_size - sizeof(dhcp_packet);
    size_t pos = 0;
    size_t n;

    // Option 53: DHCP Message Type = REQUEST
    n = append_option_u8(opts, pos, opts_max, DHCP_OPT_MSG_TYPE, DHCP_MSG_REQUEST);
    if (n == 0) return 0;
    pos += n;

    // Option 12: Hostname
    static const char hostname[] = "stellux";
    n = append_option(opts, pos, opts_max, DHCP_OPT_HOSTNAME,
                      reinterpret_cast<const uint8_t*>(hostname), sizeof(hostname) - 1);
    if (n == 0) return 0;
    pos += n;

    // Option 50: Requested IP Address (network byte order)
    uint32_t req_ip_net = htonl(offered_ip);
    n = append_option_u32(opts, pos, opts_max, DHCP_OPT_REQUESTED_IP, req_ip_net);
    if (n == 0) return 0;
    pos += n;

    // Option 54: Server Identifier (network byte order)
    uint32_t srv_id_net = htonl(server_id);
    n = append_option_u32(opts, pos, opts_max, DHCP_OPT_SERVER_ID, srv_id_net);
    if (n == 0) return 0;
    pos += n;

    // Option 55: Parameter Request List
    uint8_t param_list[] = {
        DHCP_OPT_SUBNET_MASK,
        DHCP_OPT_ROUTER,
        DHCP_OPT_DNS,
        DHCP_OPT_LEASE_TIME,
    };
    n = append_option(opts, pos, opts_max, DHCP_OPT_PARAM_LIST,
                      param_list, sizeof(param_list));
    if (n == 0) return 0;
    pos += n;

    // Option 255: End
    n = append_option(opts, pos, opts_max, DHCP_OPT_END, nullptr, 0);
    if (n == 0) return 0;
    pos += n;

    return sizeof(dhcp_packet) + pos;
}

// ============================================================================
// Packet Parse Function
// ============================================================================

bool dhcp_parse_response(const dhcp_packet* pkt, size_t pkt_len,
                         dhcp_config* out) {
    if (!pkt || !out || pkt_len < sizeof(dhcp_packet)) {
        return false;
    }

    string::memset(out, 0, sizeof(dhcp_config));
    out->valid = false;

    if (pkt->op != DHCP_OP_BOOTREPLY) return false;
    if (ntohl(pkt->magic) != DHCP_MAGIC_COOKIE) return false;

    out->offered_ip = ntohl(pkt->yiaddr);

    const uint8_t* opts = reinterpret_cast<const uint8_t*>(pkt) + sizeof(dhcp_packet);
    size_t opts_len = pkt_len - sizeof(dhcp_packet);
    size_t pos = 0;

    while (pos < opts_len) {
        uint8_t code = opts[pos];

        if (code == DHCP_OPT_END) break;

        if (code == DHCP_OPT_PAD) {
            pos++;
            continue;
        }

        if (pos + 1 >= opts_len) break;
        uint8_t opt_len = opts[pos + 1];
        if (pos + 2 + opt_len > opts_len) break;

        const uint8_t* opt_data = opts + pos + 2;

        switch (code) {
        case DHCP_OPT_MSG_TYPE:
            if (opt_len >= 1) out->msg_type = opt_data[0];
            break;
        case DHCP_OPT_SUBNET_MASK:
            if (opt_len >= 4) {
                uint32_t val;
                string::memcpy(&val, opt_data, 4);
                out->subnet_mask = ntohl(val);
            }
            break;
        case DHCP_OPT_ROUTER:
            if (opt_len >= 4) {
                uint32_t val;
                string::memcpy(&val, opt_data, 4);
                out->gateway = ntohl(val);
            }
            break;
        case DHCP_OPT_DNS:
            if (opt_len >= 4) {
                uint32_t val;
                string::memcpy(&val, opt_data, 4);
                out->dns_server = ntohl(val);
            }
            break;
        case DHCP_OPT_LEASE_TIME:
            if (opt_len >= 4) {
                uint32_t val;
                string::memcpy(&val, opt_data, 4);
                out->lease_time = ntohl(val);
            }
            break;
        case DHCP_OPT_SERVER_ID:
            if (opt_len >= 4) {
                uint32_t val;
                string::memcpy(&val, opt_data, 4);
                out->server_id = ntohl(val);
            }
            break;
        default:
            break;
        }

        pos += 2 + opt_len;
    }

    if (out->msg_type == 0) return false;

    out->valid = true;
    return true;
}

// ============================================================================
// DHCP Client State Machine
// ============================================================================

int32_t dhcp_configure(netif* iface) {
    if (!iface || !iface->transmit) {
        return ERR_INVAL;
    }

    log::info("dhcp: %s starting DHCP configuration", iface->name);

    // Activate the receive hook so udp_recv() copies port-68 packets
    // into the static buffer for us to poll.
    activate_rx_hook();

    // Generate transaction ID
    uint32_t xid = generate_xid(iface->mac);

    // Allocate buffers using unprivileged heap (auto-elevates, Ring 3 safe)
    auto* tx_buf = static_cast<uint8_t*>(heap::uzalloc(DHCP_PACKET_MAX));
    auto* rx_buf = static_cast<uint8_t*>(heap::uzalloc(DHCP_PACKET_MAX));
    if (!tx_buf || !rx_buf) {
        if (tx_buf) heap::ufree(tx_buf);
        if (rx_buf) heap::ufree(rx_buf);
        deactivate_rx_hook();
        log::error("dhcp: failed to allocate buffers");
        return ERR_NOMEM;
    }

    int32_t result = ERR_TIMEOUT;

    for (uint32_t attempt = 0; attempt < DHCP_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            log::info("dhcp: %s attempt %u/%u", iface->name,
                      attempt + 1, DHCP_ATTEMPTS);
        }

        // ---- Phase 1: Send DISCOVER ----
        size_t discover_len = dhcp_build_discover(tx_buf, DHCP_PACKET_MAX,
                                                  iface->mac, xid);
        if (discover_len == 0) {
            log::error("dhcp: failed to build DISCOVER");
            result = ERR_INVAL;
            break;
        }

        log::info("dhcp: %s sending DISCOVER (xid=0x%08x)",
                  iface->name, ntohl(xid));

        int32_t tx_rc = send_dhcp_broadcast(iface, tx_buf, discover_len);
        if (tx_rc != OK) {
            log::error("dhcp: %s DISCOVER send failed: %d", iface->name, tx_rc);
            continue;
        }

        // ---- Phase 2: Wait for OFFER ----
        dhcp_config offer = {};
        bool got_offer = false;
        uint64_t deadline = clock::now_ns() +
            static_cast<uint64_t>(DHCP_TIMEOUT_MS) * 1000000ULL;

        while (clock::now_ns() < deadline) {
            // Poll the NIC to process incoming packets
            RUN_ELEVATED({
                if (iface->poll) {
                    iface->poll(iface);
                }
            });

            // Check if a DHCP response arrived via the hook
            size_t rx_len = 0;
            if (try_read_dhcp_response(rx_buf, DHCP_PACKET_MAX, &rx_len)) {
                if (rx_len >= sizeof(dhcp_packet)) {
                    auto* resp = reinterpret_cast<const dhcp_packet*>(rx_buf);

                    if (resp->xid == xid) {
                        if (dhcp_parse_response(resp, rx_len, &offer) &&
                            offer.msg_type == DHCP_MSG_OFFER) {
                            got_offer = true;
                            break;
                        }
                    }
                }
            }

            // Sleep briefly between polls
            RUN_ELEVATED(sched::sleep_ms(DHCP_POLL_INTERVAL_MS));
        }

        if (!got_offer) {
            log::warn("dhcp: %s no OFFER received (attempt %u)",
                      iface->name, attempt + 1);
            continue;
        }

        log::info("dhcp: %s received OFFER: %u.%u.%u.%u from server %u.%u.%u.%u",
                  iface->name,
                  (offer.offered_ip >> 24) & 0xFF,
                  (offer.offered_ip >> 16) & 0xFF,
                  (offer.offered_ip >> 8) & 0xFF,
                  offer.offered_ip & 0xFF,
                  (offer.server_id >> 24) & 0xFF,
                  (offer.server_id >> 16) & 0xFF,
                  (offer.server_id >> 8) & 0xFF,
                  offer.server_id & 0xFF);

        // ---- Phase 3: Send REQUEST ----
        size_t request_len = dhcp_build_request(tx_buf, DHCP_PACKET_MAX,
                                                iface->mac, xid,
                                                offer.offered_ip,
                                                offer.server_id);
        if (request_len == 0) {
            log::error("dhcp: failed to build REQUEST");
            result = ERR_INVAL;
            break;
        }

        log::info("dhcp: %s sending REQUEST for %u.%u.%u.%u",
                  iface->name,
                  (offer.offered_ip >> 24) & 0xFF,
                  (offer.offered_ip >> 16) & 0xFF,
                  (offer.offered_ip >> 8) & 0xFF,
                  offer.offered_ip & 0xFF);

        tx_rc = send_dhcp_broadcast(iface, tx_buf, request_len);
        if (tx_rc != OK) {
            log::error("dhcp: %s REQUEST send failed: %d", iface->name, tx_rc);
            continue;
        }

        // ---- Phase 4: Wait for ACK ----
        dhcp_config ack = {};
        bool got_ack = false;
        deadline = clock::now_ns() +
            static_cast<uint64_t>(DHCP_TIMEOUT_MS) * 1000000ULL;

        while (clock::now_ns() < deadline) {
            RUN_ELEVATED({
                if (iface->poll) {
                    iface->poll(iface);
                }
            });

            size_t rx_len = 0;
            if (try_read_dhcp_response(rx_buf, DHCP_PACKET_MAX, &rx_len)) {
                if (rx_len >= sizeof(dhcp_packet)) {
                    auto* resp = reinterpret_cast<const dhcp_packet*>(rx_buf);

                    if (resp->xid == xid) {
                        if (dhcp_parse_response(resp, rx_len, &ack)) {
                            if (ack.msg_type == DHCP_MSG_ACK) {
                                got_ack = true;
                                break;
                            } else if (ack.msg_type == DHCP_MSG_NAK) {
                                log::warn("dhcp: %s received NAK", iface->name);
                                break;
                            }
                        }
                    }
                }
            }

            RUN_ELEVATED(sched::sleep_ms(DHCP_POLL_INTERVAL_MS));
        }

        if (!got_ack) {
            log::warn("dhcp: %s no ACK received (attempt %u)",
                      iface->name, attempt + 1);
            continue;
        }

        // ---- Success: Configure the interface ----
        uint32_t ip   = ack.offered_ip   ? ack.offered_ip   : offer.offered_ip;
        uint32_t mask = ack.subnet_mask  ? ack.subnet_mask  : offer.subnet_mask;
        uint32_t gw   = ack.gateway      ? ack.gateway      : offer.gateway;
        uint32_t dns  = ack.dns_server   ? ack.dns_server   : offer.dns_server;

        log::info("dhcp: %s received ACK: %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u lease %us",
                  iface->name,
                  (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                  (ip >> 8) & 0xFF, ip & 0xFF,
                  (mask >> 24) & 0xFF, (mask >> 16) & 0xFF,
                  (mask >> 8) & 0xFF, mask & 0xFF,
                  (gw >> 24) & 0xFF, (gw >> 16) & 0xFF,
                  (gw >> 8) & 0xFF, gw & 0xFF,
                  (dns >> 24) & 0xFF, (dns >> 16) & 0xFF,
                  (dns >> 8) & 0xFF, dns & 0xFF,
                  ack.lease_time ? ack.lease_time : offer.lease_time);

        iface->ipv4_dns = dns;
        configure(iface, ip, mask, gw);

        result = OK;
        break;
    }

    // Cleanup
    deactivate_rx_hook();
    heap::ufree(tx_buf);
    heap::ufree(rx_buf);

    if (result == OK) {
        log::info("dhcp: %s configuration complete", iface->name);
    } else {
        log::warn("dhcp: %s configuration failed", iface->name);
    }

    return result;
}

} // namespace net
