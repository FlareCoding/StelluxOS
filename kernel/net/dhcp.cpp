#include "net/dhcp.h"
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "net/udp.h"
#include "net/inet_socket.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "common/logging.h"
#include "common/string.h"
#include "common/ring_buffer.h"
#include "mm/heap.h"
#include "sync/spinlock.h"
#include "sched/sched.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

namespace net {

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
 * Build a raw UDP broadcast frame (ETH + IPv4 + UDP + payload) and
 * transmit it via eth_send(). This bypasses ipv4_send() entirely,
 * allowing transmission on an unconfigured interface.
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

    // Build the IPv4 + UDP + DHCP payload to pass to eth_send().
    // eth_send() prepends the Ethernet header, so we build everything
    // from IPv4 onward.
    size_t udp_total = sizeof(udp_header) + payload_len;
    size_t ip_total  = sizeof(ipv4_header) + udp_total;

    if (ip_total > ETH_MTU) {
        return ERR_INVAL;
    }

    auto* pkt = static_cast<uint8_t*>(heap::kzalloc(ip_total));
    if (!pkt) {
        return ERR_NOMEM;
    }

    // --- IPv4 header ---
    auto* ip = reinterpret_cast<ipv4_header*>(pkt);
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
    auto* udp = reinterpret_cast<udp_header*>(pkt + sizeof(ipv4_header));
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->length   = htons(static_cast<uint16_t>(udp_total));
    udp->checksum = 0;

    // --- DHCP payload ---
    string::memcpy(pkt + sizeof(ipv4_header) + sizeof(udp_header),
                   payload, payload_len);

    // Compute UDP checksum over pseudo-header + UDP segment.
    // src_ip and dst_ip must be in network byte order for udp_checksum().
    uint16_t csum = udp_checksum(ip->src_ip, ip->dst_ip,
                                 pkt + sizeof(ipv4_header), udp_total);
    udp->checksum = (csum == 0) ? static_cast<uint16_t>(0xFFFF) : csum;

    // Send as a broadcast Ethernet frame
    int32_t rc = eth_send(iface, ETH_BROADCAST, ETH_TYPE_IPV4, pkt, ip_total);
    heap::kfree(pkt);
    return rc;
}

// UDP ring buffer entry framing (must match udp.cpp delivery format):
//   [4 bytes: src_ip in network byte order]
//   [2 bytes: src_port in network byte order]
//   [2 bytes: payload_len in host byte order]
//   [N bytes: payload data (raw UDP payload = DHCP packet)]
constexpr size_t UDP_RX_ENTRY_HEADER = 8;

/**
 * Try to read one DHCP response from the socket's ring buffer.
 * Non-blocking: returns false if no data is available.
 *
 * @param sock     The UDP socket bound to DHCP client port 68.
 * @param out_buf  Buffer to receive the DHCP payload.
 * @param buf_size Size of the output buffer.
 * @param out_len  Receives the actual DHCP payload length.
 * @return true if a DHCP packet was read, false otherwise.
 */
static bool try_read_dhcp_response(inet_socket* sock, uint8_t* out_buf,
                                   size_t buf_size, size_t* out_len) {
    if (!sock || !sock->rx_buf || !out_buf || !out_len) return false;

    // Try to read the entry header (non-blocking)
    uint8_t hdr[UDP_RX_ENTRY_HEADER];
    ssize_t hdr_rc = ring_buffer_read(sock->rx_buf, hdr,
                                      UDP_RX_ENTRY_HEADER, true);
    if (hdr_rc < static_cast<ssize_t>(UDP_RX_ENTRY_HEADER)) {
        return false;
    }

    // Extract payload length (host byte order, at offset 6)
    uint16_t payload_len = 0;
    string::memcpy(&payload_len, hdr + 6, 2);

    if (payload_len == 0) {
        return false;
    }

    // Read the payload
    size_t to_read = payload_len < buf_size ? payload_len : buf_size;
    ssize_t data_rc = ring_buffer_read(sock->rx_buf, out_buf, to_read, true);

    // Drain any remaining bytes if buffer was too small
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

    if (data_rc <= 0) {
        return false;
    }

    *out_len = static_cast<size_t>(data_rc);
    return true;
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

    // Fill the fixed header
    auto* pkt = reinterpret_cast<dhcp_packet*>(out);
    init_dhcp_header(pkt, mac, xid);

    // Build options after the fixed header
    uint8_t* opts = out + sizeof(dhcp_packet);
    size_t opts_max = out_size - sizeof(dhcp_packet);
    size_t pos = 0;
    size_t n;

    // Option 53: DHCP Message Type = DISCOVER
    n = append_option_u8(opts, pos, opts_max, DHCP_OPT_MSG_TYPE, DHCP_MSG_DISCOVER);
    if (n == 0) return 0;
    pos += n;

    // Option 55: Parameter Request List
    uint8_t param_list[] = {
        DHCP_OPT_SUBNET_MASK,   // 1
        DHCP_OPT_ROUTER,        // 3
        DHCP_OPT_DNS,           // 6
        DHCP_OPT_LEASE_TIME,    // 51
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

    // Fill the fixed header
    auto* pkt = reinterpret_cast<dhcp_packet*>(out);
    init_dhcp_header(pkt, mac, xid);

    // Build options after the fixed header
    uint8_t* opts = out + sizeof(dhcp_packet);
    size_t opts_max = out_size - sizeof(dhcp_packet);
    size_t pos = 0;
    size_t n;

    // Option 53: DHCP Message Type = REQUEST
    n = append_option_u8(opts, pos, opts_max, DHCP_OPT_MSG_TYPE, DHCP_MSG_REQUEST);
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

    // Validate BOOTP reply and magic cookie
    if (pkt->op != DHCP_OP_BOOTREPLY) return false;
    if (ntohl(pkt->magic) != DHCP_MAGIC_COOKIE) return false;

    // Extract "your" IP address
    out->offered_ip = ntohl(pkt->yiaddr);

    // Parse TLV options
    const uint8_t* opts = reinterpret_cast<const uint8_t*>(pkt) + sizeof(dhcp_packet);
    size_t opts_len = pkt_len - sizeof(dhcp_packet);
    size_t pos = 0;

    while (pos < opts_len) {
        uint8_t code = opts[pos];

        if (code == DHCP_OPT_END) {
            break;
        }

        if (code == DHCP_OPT_PAD) {
            pos++;
            continue;
        }

        // Need at least a length byte after the code
        if (pos + 1 >= opts_len) break;
        uint8_t opt_len = opts[pos + 1];

        // Ensure the option data fits within the packet
        if (pos + 2 + opt_len > opts_len) break;

        const uint8_t* opt_data = opts + pos + 2;

        switch (code) {
        case DHCP_OPT_MSG_TYPE:
            if (opt_len >= 1) {
                out->msg_type = opt_data[0];
            }
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
                // Take the first router only
                uint32_t val;
                string::memcpy(&val, opt_data, 4);
                out->gateway = ntohl(val);
            }
            break;

        case DHCP_OPT_DNS:
            if (opt_len >= 4) {
                // Take the first DNS server only
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
            // Skip unknown options
            break;
        }

        pos += 2 + opt_len;
    }

    // A valid response must have a message type
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

    // Create a temporary kernel-level UDP socket bound to port 68.
    // This reuses the existing UDP delivery infrastructure: udp_recv()
    // dispatches incoming packets to registered sockets by port number,
    // writing them into the socket's ring buffer.
    auto* sock = heap::kalloc_new<inet_socket>();
    if (!sock) {
        log::error("dhcp: failed to allocate socket");
        return ERR_NOMEM;
    }
    sock->protocol = IPV4_PROTO_UDP;
    sock->bound_addr = 0;
    sock->bound_port = DHCP_CLIENT_PORT;
    sock->lock = sync::SPINLOCK_INIT;
    sock->next = nullptr;

    sock->rx_buf = ring_buffer_create(4096);
    if (!sock->rx_buf) {
        heap::kfree_delete(sock);
        log::error("dhcp: failed to allocate rx buffer");
        return ERR_NOMEM;
    }

    // Register with the UDP layer for delivery on port 68
    udp_register_socket(sock);

    // Generate transaction ID
    uint32_t xid = generate_xid(iface->mac);

    // Allocate buffers for TX and RX
    auto* tx_buf = static_cast<uint8_t*>(heap::kzalloc(DHCP_PACKET_MAX));
    auto* rx_buf = static_cast<uint8_t*>(heap::kzalloc(DHCP_PACKET_MAX));
    if (!tx_buf || !rx_buf) {
        if (tx_buf) heap::kfree(tx_buf);
        if (rx_buf) heap::kfree(rx_buf);
        udp_unregister_socket(sock);
        ring_buffer_destroy(sock->rx_buf);
        heap::kfree_delete(sock);
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

            // Check if a DHCP response arrived
            size_t rx_len = 0;
            if (try_read_dhcp_response(sock, rx_buf, DHCP_PACKET_MAX, &rx_len)) {
                if (rx_len >= sizeof(dhcp_packet)) {
                    auto* resp = reinterpret_cast<const dhcp_packet*>(rx_buf);

                    // Verify transaction ID
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
            if (try_read_dhcp_response(sock, rx_buf, DHCP_PACKET_MAX, &rx_len)) {
                if (rx_len >= sizeof(dhcp_packet)) {
                    auto* resp = reinterpret_cast<const dhcp_packet*>(rx_buf);

                    if (resp->xid == xid) {
                        if (dhcp_parse_response(resp, rx_len, &ack)) {
                            if (ack.msg_type == DHCP_MSG_ACK) {
                                got_ack = true;
                                break;
                            } else if (ack.msg_type == DHCP_MSG_NAK) {
                                log::warn("dhcp: %s received NAK", iface->name);
                                break; // retry with new DISCOVER
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
        // Use the ACK values; fall back to OFFER values for any
        // fields the ACK didn't include.
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

        // Apply the configuration
        configure(iface, ip, mask, gw);
        iface->ipv4_dns = dns;

        result = OK;
        break;
    }

    // Cleanup: unregister socket, free buffers
    udp_unregister_socket(sock);
    ring_buffer_destroy(sock->rx_buf);
    heap::kfree_delete(sock);
    heap::kfree(tx_buf);
    heap::kfree(rx_buf);

    if (result == OK) {
        log::info("dhcp: %s configuration complete", iface->name);
    } else {
        log::warn("dhcp: %s configuration failed", iface->name);
    }

    return result;
}

} // namespace net
