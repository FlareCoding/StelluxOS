#ifndef STELLUX_NET_ICMP_H
#define STELLUX_NET_ICMP_H

#include "net/ipv4.h"

namespace net {
namespace icmp {

constexpr size_t HEADER_LEN = 8;

constexpr uint8_t TYPE_ECHO_REPLY        = 0;
constexpr uint8_t TYPE_DEST_UNREACHABLE  = 3;
constexpr uint8_t TYPE_ECHO_REQUEST      = 8;
constexpr uint8_t TYPE_TIME_EXCEEDED     = 11;
constexpr uint8_t TYPE_PARAMETER_PROBLEM = 12;

// Codes for TYPE_DEST_UNREACHABLE
constexpr uint8_t CODE_NET_UNREACHABLE      = 0;
constexpr uint8_t CODE_HOST_UNREACHABLE     = 1;
constexpr uint8_t CODE_PROTOCOL_UNREACHABLE = 2;
constexpr uint8_t CODE_PORT_UNREACHABLE     = 3;
constexpr uint8_t CODE_FRAGMENTATION_NEEDED = 4;

// Codes for TYPE_TIME_EXCEEDED
constexpr uint8_t CODE_TTL_EXCEEDED        = 0;
constexpr uint8_t CODE_REASSEMBLY_EXCEEDED = 1;

// Error messages quote the offending IP header plus this much of its payload
constexpr size_t ERROR_QUOTE_LEN = 8;

/**
 * ICMP message header (RFC 792). The last four bytes depend on the type:
 * identifier and sequence for echo, zero for most errors, the next hop MTU
 * for fragmentation needed. `checksum` covers the header and the body.
 * https://www.rfc-editor.org/info/rfc792/
 */
struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;

    union {
        struct {
            uint16_t id;
            uint16_t seq;
        } __attribute__((packed)) echo;

        struct {
            uint16_t unused;
            uint16_t next_hop_mtu;
        } __attribute__((packed)) frag;

        uint32_t unused;
    } __attribute__((packed));
} __attribute__((packed));
static_assert(sizeof(icmp_header) == HEADER_LEN);

/*
 * Consumes an ICMP message whose window starts at the header. Echo requests
 * are answered in place, other messages are logged and freed.
 */
int32_t input(packet* pkt);

/*
 * Consumes a finished ICMP message, fills in its checksum, and hands it to
 * IPv4 for `dest`.
 */
int32_t output(packet* pkt, const ipv4::ipv4_addr& dest);

/*
 * Builds and sends an echo request to `dest` carrying `len` bytes of `payload`.
 */
int32_t send_echo_request(const ipv4::ipv4_addr& dest, uint16_t id, uint16_t seq,
                          const void* payload, size_t len);

/*
 * Reports an error about `offending` packet whose window must start at its IPv4
 * header, quoting that header and the first bytes of its payload. Nothing is
 * sent about broadcasts, later fragments, or other ICMP errors.
 */
int32_t send_error(const packet* offending, uint8_t type, uint8_t code);

} // namespace icmp
} // namespace net

#endif // STELLUX_NET_ICMP_H
