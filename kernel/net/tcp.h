#ifndef STELLUX_NET_TCP_H
#define STELLUX_NET_TCP_H

#include "net/net.h"

namespace net {

// TCP header flags (RFC 9293 Section 3.1)
constexpr uint8_t TCP_FIN = 0x01; // no more data from sender
constexpr uint8_t TCP_SYN = 0x02; // synchronize sequence numbers
constexpr uint8_t TCP_RST = 0x04; // reset the connection
constexpr uint8_t TCP_PSH = 0x08; // push buffered data to receiver
constexpr uint8_t TCP_ACK = 0x10; // acknowledgment field is valid
constexpr uint8_t TCP_URG = 0x20; // urgent pointer field is valid

// TCP header (RFC 9293 Section 3.1) — minimum 20 bytes.
// All multi-byte fields are in network byte order on the wire.
struct tcp_header {
    uint16_t src_port;   // source port
    uint16_t dst_port;   // destination port
    uint32_t seq;        // sequence number
    uint32_t ack;        // acknowledgment number
    uint8_t  data_off;   // upper 4 bits: data offset (header length in 32-bit words)
    uint8_t  flags;      // lower 6 bits: URG|ACK|PSH|RST|SYN|FIN
    uint16_t window;     // receive window size
    uint16_t checksum;   // checksum (pseudo-header + header + data)
    uint16_t urgent_ptr; // urgent pointer (only valid if URG flag set)
} __attribute__((packed));

static_assert(sizeof(tcp_header) == 20, "tcp_header must be 20 bytes");

// Extract the header length in bytes from the data_off field.
// The upper 4 bits encode the number of 32-bit words in the header.
inline constexpr size_t tcp_header_len(const tcp_header* hdr) {
    return static_cast<size_t>((hdr->data_off >> 4) & 0xF) * 4;
}

/**
 * Process a received TCP segment (after IPv4 header is stripped).
 * Validates header, verifies checksum, and dispatches to connection state.
 * @param src_ip Source IP in host byte order.
 * @param dst_ip Destination IP in host byte order.
 */
void tcp_recv(netif* iface, uint32_t src_ip, uint32_t dst_ip,
              const uint8_t* data, size_t len);

} // namespace net

#endif // STELLUX_NET_TCP_H
