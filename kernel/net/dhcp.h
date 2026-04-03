#ifndef STELLUX_NET_DHCP_H
#define STELLUX_NET_DHCP_H

#include "common/types.h"
#include "net/net.h"

namespace net {

// ============================================================================
// DHCP Constants
// ============================================================================

constexpr uint16_t DHCP_SERVER_PORT = 67;
constexpr uint16_t DHCP_CLIENT_PORT = 68;
constexpr uint32_t DHCP_MAGIC_COOKIE = 0x63825363;

// BOOTP opcodes
constexpr uint8_t DHCP_OP_BOOTREQUEST = 1;
constexpr uint8_t DHCP_OP_BOOTREPLY   = 2;

// Hardware types
constexpr uint8_t DHCP_HTYPE_ETHERNET = 1;
constexpr uint8_t DHCP_HLEN_ETHERNET  = 6;

// DHCP message types (Option 53 values)
constexpr uint8_t DHCP_MSG_DISCOVER = 1;
constexpr uint8_t DHCP_MSG_OFFER    = 2;
constexpr uint8_t DHCP_MSG_REQUEST  = 3;
constexpr uint8_t DHCP_MSG_DECLINE  = 4;
constexpr uint8_t DHCP_MSG_ACK      = 5;
constexpr uint8_t DHCP_MSG_NAK      = 6;
constexpr uint8_t DHCP_MSG_RELEASE  = 7;

// DHCP option codes
constexpr uint8_t DHCP_OPT_PAD           = 0;
constexpr uint8_t DHCP_OPT_SUBNET_MASK   = 1;
constexpr uint8_t DHCP_OPT_ROUTER        = 3;
constexpr uint8_t DHCP_OPT_DNS           = 6;
constexpr uint8_t DHCP_OPT_HOSTNAME      = 12;
constexpr uint8_t DHCP_OPT_REQUESTED_IP  = 50;
constexpr uint8_t DHCP_OPT_LEASE_TIME    = 51;
constexpr uint8_t DHCP_OPT_MSG_TYPE      = 53;
constexpr uint8_t DHCP_OPT_SERVER_ID     = 54;
constexpr uint8_t DHCP_OPT_PARAM_LIST    = 55;
constexpr uint8_t DHCP_OPT_END           = 255;

// DHCP flags
constexpr uint16_t DHCP_FLAG_BROADCAST = 0x8000;

// Timing
constexpr uint32_t DHCP_ATTEMPTS       = 3;
constexpr uint32_t DHCP_POLL_INTERVAL_MS = 50;
constexpr uint32_t DHCP_TIMEOUT_MS     = 5000;

// ============================================================================
// DHCP Packet Structure
// ============================================================================

/**
 * Fixed-size BOOTP/DHCP header (236 bytes).
 * DHCP options follow immediately after this header in the packet.
 */
struct dhcp_packet {
    uint8_t  op;            // message op: 1=BOOTREQUEST, 2=BOOTREPLY
    uint8_t  htype;         // hardware type: 1=Ethernet
    uint8_t  hlen;          // hardware address length: 6 for Ethernet
    uint8_t  hops;          // relay hops, client sets to 0
    uint32_t xid;           // transaction ID (network byte order)
    uint16_t secs;          // seconds since client began (network byte order)
    uint16_t flags;         // flags (network byte order), 0x8000=broadcast
    uint32_t ciaddr;        // client IP (network byte order), 0 during discover
    uint32_t yiaddr;        // "your" (offered) IP (network byte order)
    uint32_t siaddr;        // next server IP (network byte order)
    uint32_t giaddr;        // relay agent IP (network byte order)
    uint8_t  chaddr[16];    // client hardware address (MAC in first 6 bytes)
    uint8_t  sname[64];     // server host name (optional, zero-filled)
    uint8_t  file[128];     // boot file name (optional, zero-filled)
    uint32_t magic;         // DHCP magic cookie: 0x63825363 (network byte order)
} __attribute__((packed));

static_assert(sizeof(dhcp_packet) == 240, "dhcp_packet must be 240 bytes");

// Maximum DHCP options region size (after the fixed header)
constexpr size_t DHCP_OPTIONS_MAX = 312;

// Maximum total DHCP message size (header + options)
constexpr size_t DHCP_PACKET_MAX = sizeof(dhcp_packet) + DHCP_OPTIONS_MAX;

// ============================================================================
// Parsed DHCP Configuration
// ============================================================================

/**
 * Holds parsed configuration from a DHCP OFFER or ACK response.
 * All IP addresses are in HOST byte order.
 */
struct dhcp_config {
    uint32_t offered_ip;     // "your" IP address
    uint32_t subnet_mask;    // subnet mask
    uint32_t gateway;        // default gateway (router)
    uint32_t dns_server;     // primary DNS server
    uint32_t server_id;      // DHCP server identifier
    uint32_t lease_time;     // lease duration in seconds
    uint8_t  msg_type;       // DHCP message type (OFFER, ACK, NAK)
    bool     valid;          // true if parsing succeeded
};

// ============================================================================
// Packet Build/Parse Functions (unit-testable)
// ============================================================================

/**
 * Build a DHCP DISCOVER packet.
 * @param out       Buffer to write the DHCP message (header + options).
 * @param out_size  Size of the output buffer.
 * @param mac       Client MAC address (6 bytes).
 * @param xid       Transaction ID in network byte order.
 * @return Number of bytes written, or 0 on failure.
 */
size_t dhcp_build_discover(uint8_t* out, size_t out_size,
                           const uint8_t* mac, uint32_t xid);

/**
 * Build a DHCP REQUEST packet.
 * @param out        Buffer to write the DHCP message (header + options).
 * @param out_size   Size of the output buffer.
 * @param mac        Client MAC address (6 bytes).
 * @param xid        Transaction ID in network byte order.
 * @param offered_ip Requested IP in HOST byte order (from OFFER).
 * @param server_id  Server identifier in HOST byte order (from OFFER).
 * @return Number of bytes written, or 0 on failure.
 */
size_t dhcp_build_request(uint8_t* out, size_t out_size,
                          const uint8_t* mac, uint32_t xid,
                          uint32_t offered_ip, uint32_t server_id);

/**
 * Parse DHCP options from a received DHCP packet.
 * @param pkt     Pointer to the DHCP packet (header + options).
 * @param pkt_len Total length of the DHCP packet.
 * @param out     Parsed configuration output.
 * @return true if parsing succeeded and a valid message type was found.
 */
bool dhcp_parse_response(const dhcp_packet* pkt, size_t pkt_len,
                         dhcp_config* out);

// ============================================================================
// DHCP Receive Hook (called by udp_recv, runs at Ring 0)
// ============================================================================

/**
 * Called by udp_recv() when a UDP packet arrives on port 68 (DHCP client).
 * Copies the UDP payload (DHCP message) into an internal static buffer
 * for the DHCP client to poll. Uses unprivileged data so it is callable
 * from any privilege level.
 *
 * @param data UDP payload (DHCP packet, after UDP header is stripped).
 * @param len  Length of the DHCP payload.
 */
void dhcp_rx_hook(const uint8_t* data, size_t len);

// ============================================================================
// DHCP Client API
// ============================================================================

/**
 * Run a full DHCP exchange on the given interface.
 *
 * Sends DISCOVER, waits for OFFER, sends REQUEST, waits for ACK.
 * On success, calls net::configure() to set the interface's IP configuration
 * and stores the DNS server address in iface->ipv4_dns.
 *
 * This function blocks and should be called from a kernel task context
 * (e.g. a driver's run() method) where sched::sleep_ms() is safe.
 *
 * The interface must be registered (via register_netif()) and the NIC
 * must be ready to transmit/receive, but need NOT be configured yet.
 *
 * @param iface The network interface to configure via DHCP.
 * @return net::OK on success, net::ERR_TIMEOUT if no response,
 *         or another negative error code on failure.
 */
int32_t dhcp_configure(netif* iface);

} // namespace net

#endif // STELLUX_NET_DHCP_H
