#ifndef STELLUX_NET_INET_H
#define STELLUX_NET_INET_H

#include "common/types.h"
#include "net/net.h"
#include "net/ipv4.h"

namespace resource { struct resource_object; }

namespace net {
namespace inet {

// Values shared with userland through the socket system calls
constexpr uint16_t AF_INET      = 2;
constexpr uint32_t SOCK_STREAM  = 1;
constexpr uint32_t SOCK_DGRAM   = 2;
constexpr uint32_t IPPROTO_IP   = 0;
constexpr uint32_t IPPROTO_ICMP = ipv4::PROTO_ICMP;
constexpr uint32_t IPPROTO_TCP  = ipv4::PROTO_TCP;
constexpr uint32_t IPPROTO_UDP  = ipv4::PROTO_UDP;
constexpr uint32_t MSG_DONTWAIT = 0x40;

constexpr size_t SOCKADDR_IN_LEN = 16;

/**
 * An IPv4 endpoint as userland passes it to the socket calls, padded to the
 * size of the generic `sockaddr`. `port` is in network byte order.
 */
struct sockaddr_in {
    uint16_t        family; // AF_INET
    uint16_t        port;
    ipv4::ipv4_addr addr;
    uint8_t         zero[8];
} __attribute__((packed));
static_assert(sizeof(sockaddr_in) == SOCKADDR_IN_LEN);

/*
 * Validates `len` bytes of a user-supplied address and extracts the endpoint.
 * `port` is returned in host byte order.
 */
int32_t parse_sockaddr(const void* addr, size_t len, ipv4::ipv4_addr* out_addr, uint16_t* out_port);

/*
 * Writes the endpoint into a buffer of `*len` bytes and sets `*len` to the
 * size written. `port` is taken in host byte order.
 */
int32_t fill_sockaddr(void* addr, size_t* len, const ipv4::ipv4_addr& ip, uint16_t port);

/**
 * Creates the resource object for an AF_INET socket of `type` and `protocol`,
 * with one reference held by the caller. Speaks resource result codes, and
 * ERR_UNSUP for a combination no protocol provides.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_socket(uint32_t type, uint32_t protocol,
                                        resource::resource_object** out);

} // namespace inet
} // namespace net

#endif // STELLUX_NET_INET_H
